#include <Base/InitializerList.h>
#include <Core/Containers/Array.h>
#include <Core/FileRead.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <Core/Text/Json.h>
#include <Core/Text/StringBuilder.h>
#include <Localization/Text.h>
#include <Platform/Clock.h>
#include <Platform/File.h>
#include <Platform/Process.h>
#include <Progress/ProgressHub.h>
#include <SplashScreen/StartupProgress.h>
#include <TestSupport/Test.h>

using namespace rocks;
using namespace rocks::splash;
namespace startup_profile_tests_file {

// 시작 단계 보정 시험(Docs/Design/SplashScreen 14절 시험 7, 8.2절). 실제 ProgressHub(가짜 시계)와
// 작업 디렉터리 아래 시험 전용 폴더의 프로필로 읽기·가중치·지수 평균·바닥·쓰기를 본다. 가중치는
// 스플래시 원천이 보고하는 비율로 읽는다(Engine 단계에 들어간 순간 = HostServices 몫).
struct FakeClock {
    uint64 now = 0;
};

uint64 ReadFakeClock(void* context) noexcept {
    return static_cast<FakeClock*>(context)->now;
}

constexpr uint64 NanosecondsPerSecond = 1'000'000'000;
constexpr double Tolerance = 1e-9;
constexpr StartupStep SandboxSteps[] = {StartupStep::HostServices, StartupStep::Engine,
                                        StartupStep::FirstFrame};
constexpr StringView Target = "Sandbox";
constexpr usize PathBytes = 512;

bool Near(double left, double right, double tolerance = Tolerance) noexcept {
    const double difference = left - right;
    return difference < tolerance && difference > -tolerance;
}

struct Fixture {
    SystemAllocator allocator;
    FakeClock clock;
    Text title;
    bool ready = false;
    char root[PathBytes]{};
    usize rootBytes = 0;
    char path[PathBytes]{};
    usize pathBytes = 0;

    Fixture() noexcept {
        ProgressHubSettings settings;
        settings.clock = ReadFakeClock;
        settings.clockContext = &clock;
        ready = static_cast<bool>(ProgressHub::Get().Initialize(allocator, settings));
        if (auto created = Text::Invariant(allocator, "Sandbox")) {
            title = Move(created.Value());
        }

        FixedStringBuilder rootText{Span<char>(root)};
        ready = ready &&
                rootText.TryAppendFormat("rocks-startup-profile-{}-{}",
                                         platform::CurrentProcessId(), platform::MonotonicTicks());
        rootBytes = rootText.ByteCount();
        FixedStringBuilder pathText{Span<char>(path)};
        ready = ready && pathText.TryAppendFormat("{}/Saved/Startup/StartupProfile.json",
                                                  StringView(root, rootBytes));
        pathBytes = pathText.ByteCount();
    }

    ~Fixture() noexcept {
        (void)platform::DeleteDirectoryTree(platform::PhysicalFileSystem(), root, rootBytes);
        ProgressHub::Get().Shutdown();
    }

    StringView Path() const noexcept {
        return StringView(path, pathBytes);
    }

    Result<StartupProgress> Begin() noexcept {
        StartupProgressDesc desc;
        desc.appTitle = &title;
        desc.steps = Span<const StartupStep>(SandboxSteps);
        desc.nowNanoseconds = ReadFakeClock;
        desc.clockContext = &clock;
        desc.profilePath = Path();
        desc.profileTarget = Target;
        return StartupProgress::Begin(allocator, desc);
    }

    // 프로필 파일에 text를 쓴다(부모 폴더를 만든다).
    bool WriteProfile(StringView text) noexcept {
        char directory[PathBytes];
        FixedStringBuilder directoryText{Span<char>(directory)};
        if (!directoryText.TryAppendFormat("{}/Saved/Startup", StringView(root, rootBytes)) ||
            !platform::MakeDirectoryTree(platform::PhysicalFileSystem(), directory,
                                         directoryText.ByteCount())) {
            return false;
        }

        platform::File file;
        if (!file.Open(platform::PhysicalFileSystem(), path, pathBytes,
                       platform::FileOpenMode::CreateAlways)) {
            return false;
        }

        const bool written = static_cast<bool>(file.WriteAll(text.Data(), text.ByteCount()).status);
        return static_cast<bool>(file.Close()) && written;
    }

    // 루트 기준 relative에 빈 파일(directory=false) 또는 폴더를 만든다(부모 폴더 포함).
    bool Make(StringView relative, bool directory) noexcept {
        char full[PathBytes];
        FixedStringBuilder fullText{Span<char>(full)};
        if (!fullText.TryAppendFormat("{}/{}", StringView(root, rootBytes), relative)) {
            return false;
        }

        if (directory) {
            return static_cast<bool>(platform::MakeDirectoryTree(platform::PhysicalFileSystem(),
                                                                 full, fullText.ByteCount()));
        }

        usize parent = fullText.ByteCount();
        while (parent != 0 && full[parent - 1] != '/') {
            --parent;
        }

        platform::File file;
        return platform::MakeDirectoryTree(platform::PhysicalFileSystem(), full, parent - 1) &&
               file.Open(platform::PhysicalFileSystem(), full, fullText.ByteCount(),
                         platform::FileOpenMode::CreateAlways) &&
               file.Close();
    }

    // 프로필 파일 전체다(없으면 빈 배열, existed false).
    bool ReadProfile(Array<uint8>& bytes, bool& existed) noexcept {
        return static_cast<bool>(ReadWholeFile(platform::PhysicalFileSystem(), Path(),
                                               MaxStartupProfileBytes, bytes, existed));
    }

    // Engine 단계에 들어간 순간의 비율(= HostServices 몫)이다.
    double HostServicesShare(StartupProgress& progress) noexcept {
        auto made = progress.MakeSplashSource(allocator, nullptr);
        if (!made) {
            return -1.0;
        }

        const SplashStatusSource source = made.Value();
        progress.Enter(StartupStep::HostServices);
        progress.Enter(StartupStep::Engine);
        SplashStatus status;
        const bool changed = source.poll(source.context, status);
        source.release(source.context);
        // 끝까지 간 시작이 아니다(프로필을 쓰지 않는다).
        progress.Finish(ProgressOutcome::Abandoned);
        return changed ? status.fraction : -1.0;
    }

    // HostServices·Engine·FirstFrame을 초 단위로 돌고(FirstFrame 뒤 MarkReady, 대기는 빼야 한다)
    // outcome으로 끝낸다.
    void Run(StartupProgress& progress, uint64 hostSeconds, uint64 engineSeconds,
             uint64 firstFrameSeconds, ProgressOutcome outcome) noexcept {
        progress.Enter(StartupStep::HostServices);
        clock.now += hostSeconds * NanosecondsPerSecond;
        progress.Enter(StartupStep::Engine);
        clock.now += engineSeconds * NanosecondsPerSecond;
        progress.Enter(StartupStep::FirstFrame);
        clock.now += firstFrameSeconds * NanosecondsPerSecond;
        progress.MarkReady();
        // 최소 표시 대기 — 단계 시간에 들어가지 않는다.
        clock.now += 5 * NanosecondsPerSecond;
        progress.Finish(outcome);
    }
};

// 대상 항목의 단계 값·samples를 읽는다.
struct SavedEntry {
    bool found = false;
    double host = -1.0;
    double engine = -1.0;
    double firstFrame = -1.0;
    uint64 samples = 0;
    bool hasStudio = false;
};

SavedEntry ReadSaved(Fixture& fixture) noexcept {
    SavedEntry saved;
    Array<uint8> bytes(fixture.allocator);
    bool existed = false;
    if (!fixture.ReadProfile(bytes, existed) || !existed) {
        return saved;
    }

    JsonDocument document(fixture.allocator);
    if (!document.Parse(StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size()))
             .Succeeded()) {
        return saved;
    }

    const JsonValue entry = document.Root().Find(Target);
    if (!entry.IsValid()) {
        return saved;
    }

    const auto read = [&entry](StringView key, double& out) noexcept {
        const JsonValue value = entry.Find(key);
        if (value.IsValid() && value.IsNumber()) {
            if (auto number = value.TryGetDouble()) {
                out = number.Value();
            }
        }
    };
    saved.found = true;
    read("HostServices", saved.host);
    read("Engine", saved.engine);
    read("FirstFrame", saved.firstFrame);
    const JsonValue samples = entry.Find("samples");
    if (samples.IsValid() && samples.IsNumber()) {
        if (auto count = samples.TryGetUInt64()) {
            saved.samples = count.Value();
        }
    }

    saved.hasStudio = document.Root().Find("Studio").IsValid();
    return saved;
}

// 프로필이 없으면 코드 기본 가중치이고, 끝까지 간 시작은 단계 시간을 쓴다(MarkReady 뒤 대기 제외).
// 다음 실행은 저장값을 가중치로 쓰고 이번 값과 반반 섞는다.
TEST(StartupProfile_WritesThenCalibratesAndBlends) {
    Fixture fixture;
    REQUIRE(fixture.ready);
    {
        auto startup = fixture.Begin();
        REQUIRE(startup);
        CHECK(Near(fixture.HostServicesShare(startup.Value()), 10.0 / 55.0));
    }

    CHECK(!ReadSaved(fixture).found);
    {
        auto startup = fixture.Begin();
        REQUIRE(startup);
        fixture.Run(startup.Value(), 1, 3, 1, ProgressOutcome::Succeeded);
        const StartupStepTimes times = startup.Value().StepTimes();
        CHECK(Near(times.seconds[static_cast<uint32>(StartupStep::FirstFrame)], 1.0));
    }

    SavedEntry first = ReadSaved(fixture);
    REQUIRE(first.found);
    CHECK(Near(first.host, 1.0));
    CHECK(Near(first.engine, 3.0));
    CHECK(Near(first.firstFrame, 1.0));
    CHECK(first.samples == 1);
    {
        auto startup = fixture.Begin();
        REQUIRE(startup);
        // 저장값 1·3·1 → HostServices 몫 1/5.
        CHECK(Near(fixture.HostServicesShare(startup.Value()), 0.2));
    }

    {
        auto startup = fixture.Begin();
        REQUIRE(startup);
        fixture.Run(startup.Value(), 3, 1, 1, ProgressOutcome::Succeeded);
    }

    SavedEntry second = ReadSaved(fixture);
    CHECK(Near(second.host, 2.0));
    CHECK(Near(second.engine, 2.0));
    CHECK(Near(second.firstFrame, 1.0));
    CHECK(second.samples == 2);

    // 임시 파일은 남지 않는다(이름 바꾸기로 게시).
    char temporary[PathBytes];
    FixedStringBuilder temporaryText{Span<char>(temporary)};
    REQUIRE(temporaryText.TryAppendFormat("{}.tmp", fixture.Path()));
    bool exists = true;
    CHECK(platform::PhysicalFileSystem().FileExists(temporary, temporaryText.ByteCount(), exists));
    CHECK(!exists);
}

// 저장값이 없는 단계는 코드 기본값 비율로 채우고, 각 단계는 합의 2% 아래로 내려가지 않는다.
// 밝히지 않은 단계의 저장값은 가중치에 들지 않는다.
TEST(StartupProfile_FillsMissingStepsAndKeepsTheFloor) {
    Fixture fixture;
    REQUIRE(fixture.ready);
    // Engine만 8초 → 비율 8/40 = 0.2 → HostServices 2, FirstFrame 1, 합 11. Shaders는 밝히지
    // 않았다.
    REQUIRE(fixture.WriteProfile("{\"v\":1,\"Sandbox\":{\"Engine\":8,\"Shaders\":100}}"));
    {
        auto startup = fixture.Begin();
        REQUIRE(startup);
        CHECK(Near(fixture.HostServicesShare(startup.Value()), 2.0 / 11.0));
    }

    REQUIRE(fixture.WriteProfile(
        "{\"v\":1,\"Sandbox\":{\"HostServices\":0.0001,\"Engine\":10,\"FirstFrame\":0.0001}}"));
    {
        auto startup = fixture.Begin();
        REQUIRE(startup);
        const double share = fixture.HostServicesShare(startup.Value());
        CHECK(share >= StartupStepWeightFloor - 0.001);
        CHECK(share < StartupStepWeightFloor + 0.001);
    }
}

// 깨진 파일·버전 불일치는 코드 기본값으로 시작하고 성공한 시작이 덮어쓴다. 실패한 시작은 쓰지 않고,
// 다른 대상 항목은 쓸 때 그대로 남는다.
TEST(StartupProfile_InvalidFilesAndFailedRuns) {
    Fixture fixture;
    REQUIRE(fixture.ready);
    for (const StringView broken :
         {StringView("{not json"), StringView("{\"v\":2,\"Sandbox\":{\"Engine\":8}}"),
          StringView("{\"v\":1,\"Sandbox\":{\"Engine\":-1}}")}) {
        REQUIRE(fixture.WriteProfile(broken));
        auto startup = fixture.Begin();
        REQUIRE(startup);
        CHECK(Near(fixture.HostServicesShare(startup.Value()), 10.0 / 55.0));
    }

    {
        auto startup = fixture.Begin();
        REQUIRE(startup);
        fixture.Run(startup.Value(), 1, 1, 1, ProgressOutcome::Succeeded);
    }

    CHECK(ReadSaved(fixture).samples == 1);
    const StringView withStudio = "{\"v\":1,\"Studio\":{\"Engine\":4,\"samples\":7},\"Sandbox\":{"
                                  "\"Engine\":2,\"samples\":3}}";
    REQUIRE(fixture.WriteProfile(withStudio));
    {
        auto startup = fixture.Begin();
        REQUIRE(startup);
        fixture.Run(startup.Value(), 1, 1, 1, ProgressOutcome::Failed);
    }

    Array<uint8> bytes(fixture.allocator);
    bool existed = false;
    REQUIRE(fixture.ReadProfile(bytes, existed));
    CHECK(StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size()) == withStudio);
    {
        auto startup = fixture.Begin();
        REQUIRE(startup);
        fixture.Run(startup.Value(), 1, 4, 1, ProgressOutcome::Succeeded);
    }

    const SavedEntry merged = ReadSaved(fixture);
    CHECK(merged.hasStudio);
    CHECK(Near(merged.engine, 3.0));
    CHECK(merged.samples == 4);

    StartupProgressDesc noTarget;
    noTarget.appTitle = &fixture.title;
    noTarget.profilePath = fixture.Path();
    CHECK(StartupProgress::Begin(fixture.allocator, noTarget).Error() ==
          ErrorCode::InvalidArgument);
}

// 대상 항목만 깨졌으면 그 항목만 버리고 다른 대상 항목은 쓸 때 남는다. 형식 오류 세부(객체가 아님·
// 문자열 값·소수 samples·범위 밖 수·상한 초과)는 모두 코드 기본값이다.
TEST(StartupProfile_BrokenTargetEntryKeepsOtherTargets) {
    Fixture fixture;
    REQUIRE(fixture.ready);
    for (const StringView broken :
         {StringView("{\"v\":1,\"Sandbox\":3}"),
          StringView("{\"v\":1,\"Sandbox\":{\"Engine\":\"8\"}}"),
          StringView("{\"v\":1,\"Sandbox\":{\"Engine\":8,\"samples\":1.5}}"),
          StringView("{\"v\":1,\"Sandbox\":{\"Engine\":1e400}}"),
          StringView("{\"v\":1,\"Sandbox\":{\"Engine\":601}}")}) {
        REQUIRE(fixture.WriteProfile(broken));
        auto startup = fixture.Begin();
        REQUIRE(startup);
        CHECK(Near(fixture.HostServicesShare(startup.Value()), 10.0 / 55.0));
    }

    REQUIRE(fixture.WriteProfile(
        "{\"v\":1,\"Studio\":{\"Engine\":4,\"samples\":7},\"Sandbox\":{\"Engine\":-1}}"));
    {
        auto startup = fixture.Begin();
        REQUIRE(startup);
        fixture.Run(startup.Value(), 1, 2, 1, ProgressOutcome::Succeeded);
    }

    const SavedEntry saved = ReadSaved(fixture);
    CHECK(saved.hasStudio);
    CHECK(Near(saved.engine, 2.0));
    CHECK(saved.samples == 1);
}

// 들어가지 않은 단계와 MarkReady 뒤의 단계는 쓰지 않는다(이전 저장값도 버린다). MarkReady는 두
// 번째부터 아무 일도 하지 않고, 이번 측정값은 MaxStartupStepSeconds로 잘린다.
TEST(StartupProfile_SkipsStepsNotEnteredAndClampsTimes) {
    Fixture fixture;
    REQUIRE(fixture.ready);
    REQUIRE(fixture.WriteProfile(
        "{\"v\":1,\"Sandbox\":{\"HostServices\":1,\"Engine\":1,\"FirstFrame\":1}}"));
    {
        auto startup = fixture.Begin();
        REQUIRE(startup);
        StartupProgress& progress = startup.Value();
        progress.Enter(StartupStep::HostServices);
        fixture.clock.now += NanosecondsPerSecond;
        progress.Enter(StartupStep::Engine);
        fixture.clock.now += 1000 * NanosecondsPerSecond;
        progress.MarkReady();
        progress.MarkReady();
        progress.Enter(StartupStep::FirstFrame);
        fixture.clock.now += NanosecondsPerSecond;
        progress.Finish();
        CHECK(
            Near(progress.StepTimes().seconds[static_cast<uint32>(StartupStep::FirstFrame)], 0.0));
    }

    const SavedEntry saved = ReadSaved(fixture);
    CHECK(Near(saved.host, 1.0));
    CHECK(Near(saved.engine, 0.5 * 1.0 + 0.5 * MaxStartupStepSeconds));
    CHECK(Near(saved.firstFrame, -1.0));
    CHECK(saved.samples == 1);
}

// 파일을 읽지 못하면(프로필 자리가 폴더) 이번 실행은 쓰지 않고, 부모 자리가 파일이라 쓰지 못해도
// 시작은 끝난다. 대상 이름 "v"는 거부한다.
TEST(StartupProfile_ReadAndWriteFailuresDoNotStopStartup) {
    Fixture fixture;
    REQUIRE(fixture.ready);
    REQUIRE(fixture.Make("Saved/Startup/StartupProfile.json", true));
    {
        auto startup = fixture.Begin();
        REQUIRE(startup);
        fixture.Run(startup.Value(), 1, 1, 1, ProgressOutcome::Succeeded);
    }

    bool directory = false;
    CHECK(
        platform::PhysicalFileSystem().DirectoryExists(fixture.path, fixture.pathBytes, directory));
    CHECK(directory);
    StartupProgressDesc version;
    version.appTitle = &fixture.title;
    version.profilePath = fixture.Path();
    version.profileTarget = "v";
    CHECK(StartupProgress::Begin(fixture.allocator, version).Error() == ErrorCode::InvalidArgument);
}

// 부모 자리가 파일이라 프로필을 쓰지 못해도(Warning) 시작은 끝까지 간다.
TEST(StartupProfile_WriteFailureDoesNotStopStartup) {
    Fixture fixture;
    REQUIRE(fixture.ready);
    REQUIRE(fixture.Make("Saved", false));
    {
        auto startup = fixture.Begin();
        REQUIRE(startup);
        fixture.Run(startup.Value(), 1, 1, 1, ProgressOutcome::Succeeded);
        CHECK(Near(startup.Value().StepTimes().seconds[static_cast<uint32>(StartupStep::Engine)],
                   1.0));
    }

    Array<uint8> bytes(fixture.allocator);
    bool existed = true;
    CHECK(!fixture.ReadProfile(bytes, existed) || !existed);
}

} // namespace startup_profile_tests_file
