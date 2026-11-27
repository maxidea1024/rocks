#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <Core/Text/String.h>
#include <Localization/Catalog.h>
#include <Localization/Text.h>
#include <Platform/Memory.h>
#include <Progress/ProgressHub.h>
#include <Progress/ProgressScope.h>
#include <SplashScreen/StartupProgress.h>
#include <TestSupport/OsThread.h>
#include <TestSupport/Test.h>

using namespace rocks;
using namespace rocks::splash;
namespace startup_progress_tests_file {

// 시작 진행 시험(Docs/Design/SplashScreen 14절 시험 5). 실제 ProgressHub(가짜 시계)에 시작 진행을
// 열고 단계·중첩 구간·다른 스레드의 단계 계수로 보고한 뒤 상태 원천 poll 결과를 단언한다. 다른
// 스레드 일은 RunOnOsThread(만들고 끝까지 기다림)로 순서가 정해진다.
struct FakeClock {
    uint64 now = 0;
};

uint64 ReadFakeClock(void* context) noexcept {
    return static_cast<FakeClock*>(context)->now;
}

constexpr uint64 NanosecondsPerSecond = 1'000'000'000;
constexpr double Tolerance = 1e-12;
// Sandbox와 같은 단계(HostServices 10, Engine 40, FirstFrame 5 — 합 55)다.
constexpr StartupStep SandboxSteps[] = {StartupStep::HostServices, StartupStep::Engine,
                                        StartupStep::FirstFrame};
constexpr double SandboxTotal = 55.0;

bool Near(double left, double right) noexcept {
    const double difference = left - right;
    return difference < Tolerance && difference > -Tolerance;
}

struct Fixture {
    SystemAllocator allocator;
    FakeClock clock;
    Text title;
    bool ready = false;

    Fixture() noexcept {
        ProgressHubSettings settings;
        settings.clock = ReadFakeClock;
        settings.clockContext = &clock;
        ready = static_cast<bool>(ProgressHub::Get().Initialize(allocator, settings));
        if (auto created = Text::Invariant(allocator, "Sandbox")) {
            title = Move(created.Value());
        }
    }

    ~Fixture() noexcept {
        ProgressHub::Get().Shutdown();
    }

    Result<StartupProgress> Begin() noexcept {
        StartupProgressDesc desc;
        desc.appTitle = &title;
        desc.steps = Span<const StartupStep>(SandboxSteps);
        desc.nowNanoseconds = ReadFakeClock;
        desc.clockContext = &clock;
        return StartupProgress::Begin(allocator, desc);
    }
};

// 원천을 당긴 결과다. 원천은 바뀐 것이 없으면 false만 돌려주고 out을 채우지 않으므로(스플래시
// 모델처럼) 마지막 상태를 쌓아 둔다.
struct Polled {
    bool changed = false;
    SplashStatus status;

    StringView Message() const noexcept {
        return StringView(status.message, status.messageBytes);
    }
};

struct Poller {
    const SplashStatusSource& source;
    SplashStatus last;

    Polled Next() noexcept {
        SplashStatus fresh;
        Polled polled;
        polled.changed = source.poll(source.context, fresh);
        if (polled.changed) {
            last.determinate = fresh.determinate;
            last.fraction = fresh.fraction;
            last.finished = fresh.finished;
            last.messageChanged = fresh.messageChanged;
            if (fresh.messageChanged) {
                platform::MemoryCopy(last.message, fresh.message, fresh.messageBytes);
                last.messageBytes = fresh.messageBytes;
            }
        }

        polled.status = last;
        polled.status.messageChanged = polled.changed && fresh.messageChanged;
        return polled;
    }
};

void AddTwoOnThread(void* context) noexcept {
    static_cast<ProgressCounter*>(context)->Add(2);
}

// 시험 5: 단계 가중치대로의 비율, 문구 우선순위(구간 문구 → 단계 문구), 기본 문구, 다른 스레드의
// 단계 계수, 하위 작업이 섞이지 않음, 끝난 시작 작업.
TEST(StartupProgress_SplashSourceFollowsSteps) {
    Fixture fixture;
    REQUIRE(fixture.ready);
    auto startup = fixture.Begin();
    REQUIRE(startup);
    StartupProgress& progress = startup.Value();
    auto made = progress.MakeSplashSource(fixture.allocator, nullptr);
    REQUIRE(made);
    const SplashStatusSource source = made.Value();
    Poller poller{source, {}};

    Polled first = poller.Next();
    CHECK(first.changed);
    CHECK(!first.status.determinate);
    CHECK(!first.status.finished);
    CHECK(first.status.messageChanged);
    CHECK(first.Message() == StringView("Loading\xE2\x80\xA6"));
    CHECK(!poller.Next().changed);

    progress.Enter(StartupStep::HostServices);
    Polled services = poller.Next();
    CHECK(services.Message() == StringView("Starting services"));
    CHECK(!services.status.determinate);

    progress.Enter(StartupStep::Engine);
    {
        Polled engine = poller.Next();
        CHECK(engine.status.determinate);
        CHECK(Near(engine.status.fraction, 10.0 / SandboxTotal));
        CHECK(engine.Message() == StringView("Starting the engine"));

        auto innerText = Text::Invariant(fixture.allocator, "Starting module World");
        REQUIRE(innerText);
        ProgressScope inner(2.0);
        inner.EnterStep(1.0, innerText.Value());
        inner.EnterStep(1.0);
        Polled nested = poller.Next();
        CHECK(Near(nested.status.fraction, (10.0 + 40.0 * 0.5) / SandboxTotal));
        // 둘째 단계는 문구가 없어 바깥(단계) 문구로 돌아간다.
        CHECK(nested.Message() == StringView("Starting the engine"));
        inner.EnterStep(0.0, innerText.Value());
        CHECK(poller.Next().Message() == StringView("Starting module World"));
    }

    // 안쪽 범위가 닫히면 그 단계 몫을 채운다.
    CHECK(Near(poller.Next().status.fraction, 50.0 / SandboxTotal));

    progress.Enter(StartupStep::FirstFrame);
    auto counter = progress.MakeStepCounter(4);
    REQUIRE(counter);
    CHECK(progress.MakeStepCounter(4).Error() == ErrorCode::AlreadyExists);
    REQUIRE(test::RunOnOsThread(AddTwoOnThread, &counter.Value()));
    CHECK(Near(poller.Next().status.fraction, (50.0 + 5.0 * 0.5) / SandboxTotal));

    // 시작 작업의 하위 작업은 비율에 섞이지 않는다(원천은 자기 진행만 본다).
    {
        auto childTitle = Text::Invariant(fixture.allocator, "Child");
        REQUIRE(childTitle);
        ProgressTaskDesc child;
        child.title = &childTitle.Value();
        child.parent = progress.TaskId();
        auto task = ProgressHub::Get().Begin(child);
        REQUIRE(task);
        CHECK(Near(poller.Next().status.fraction, (50.0 + 5.0 * 0.5) / SandboxTotal));
        CHECK(task.Value().Finish(ProgressOutcome::Succeeded));
        CHECK(Near(poller.Next().status.fraction, (50.0 + 5.0 * 0.5) / SandboxTotal));
    }

    counter = Result<ProgressCounter>::Success(ProgressCounter());
    progress.Finish();
    Polled done = poller.Next();
    CHECK(done.changed);
    CHECK(done.status.finished);
    CHECK(Near(done.status.fraction, 1.0));
    CHECK(!poller.Next().changed);
    source.release(source.context);
}

// 시험 5: 잘림은 UTF-8 경계에서 한다(한 줄 MaxSplashLineBytes).
TEST(StartupProgress_LongMessagesAreCutAtCodePoints) {
    Fixture fixture;
    REQUIRE(fixture.ready);
    auto startup = fixture.Begin();
    REQUIRE(startup);
    auto made = startup.Value().MakeSplashSource(fixture.allocator, nullptr);
    REQUIRE(made);
    const SplashStatusSource source = made.Value();
    Poller poller{source, {}};
    startup.Value().Enter(StartupStep::HostServices);
    String long3(fixture.allocator);
    for (uint32 index = 0; index < MaxSplashLineBytes; ++index) {
        // "가"(U+AC00)는 UTF-8 3 byte다.
        REQUIRE(long3.TryAppend(StringView("\xEA\xB0\x80")));
    }

    auto text = Text::Invariant(fixture.allocator, long3.View());
    REQUIRE(text);
    {
        ProgressScope scope(1.0, text.Value());
        const Polled polled = poller.Next();
        CHECK(polled.status.messageBytes <= MaxSplashLineBytes);
        CHECK(polled.status.messageBytes % 3 == 0);
        CHECK(polled.status.messageBytes + 3 > MaxSplashLineBytes);
    }

    startup.Value().Finish();
    source.release(source.context);
}

// 단계 시간(가짜 시계), 잘못된 설명, 선언하지 않은 단계·두 번 들어가기는 무시, Finish 없이 소멸하면
// 원천은 끝남을 보고한다.
TEST(StartupProgress_StepTimesAndMisuse) {
    Fixture fixture;
    REQUIRE(fixture.ready);
    {
        StartupProgressDesc empty;
        CHECK(StartupProgress::Begin(fixture.allocator, empty).Error() ==
              ErrorCode::InvalidArgument);
        const StartupStep twice[] = {StartupStep::Engine, StartupStep::Engine};
        StartupProgressDesc duplicate;
        duplicate.appTitle = &fixture.title;
        duplicate.steps = Span<const StartupStep>(twice);
        CHECK(StartupProgress::Begin(fixture.allocator, duplicate).Error() ==
              ErrorCode::InvalidArgument);
    }

    SplashStatusSource source;
    Poller poller{source, {}};
    {
        auto startup = fixture.Begin();
        REQUIRE(startup);
        StartupProgress& progress = startup.Value();
        auto made = progress.MakeSplashSource(fixture.allocator, nullptr);
        REQUIRE(made);
        source = made.Value();
        CHECK(!progress.MakeStepCounter(1).Value().IsActive());
        progress.Enter(StartupStep::HostServices);
        fixture.clock.now += 2 * NanosecondsPerSecond;
        progress.Enter(StartupStep::Engine);
        fixture.clock.now += 3 * NanosecondsPerSecond;
        progress.Enter(StartupStep::Engine);
        progress.Enter(StartupStep::Shaders);
        CHECK(Near(poller.Next().status.fraction, 10.0 / SandboxTotal));
        const StartupStepTimes times = progress.StepTimes();
        CHECK(Near(times.seconds[static_cast<uint32>(StartupStep::HostServices)], 2.0));
        CHECK(Near(times.seconds[static_cast<uint32>(StartupStep::Engine)], 3.0));
        CHECK(Near(times.seconds[static_cast<uint32>(StartupStep::Shaders)], 0.0));
    }

    CHECK(ProgressHub::Get().Stats().abandonedTasks == 1);
    CHECK(poller.Next().status.finished);
    source.release(source.context);
}

// 시험 5: 카탈로그를 주면 문구를 그 문화로 풀고(원천이 사본을 쥔다), hub가 닫히면 끝남이다.
TEST(StartupProgress_SourceUsesCatalogAndEndsWithTheHub) {
    Fixture fixture;
    REQUIRE(fixture.ready);
    constexpr TextId MessageId{0x1234, 0x5678};
    constexpr uint64 MessageRevision = 1;
    const CatalogEntry entries[] = {{MessageId, MessageRevision, "ìì§"}};
    auto catalog =
        CatalogSnapshot::Create(fixture.allocator, "ko", 1, Span<const CatalogEntry>(entries));
    REQUIRE(catalog);
    auto startup = fixture.Begin();
    REQUIRE(startup);
    auto made = startup.Value().MakeSplashSource(fixture.allocator, &catalog.Value());
    REQUIRE(made);
    // 원천은 카탈로그 사본을 쥔다(넘긴 쪽이 먼저 놓아도 된다).
    catalog.Value().Reset();
    const SplashStatusSource source = made.Value();
    Poller poller{source, {}};
    startup.Value().Enter(StartupStep::HostServices);
    auto localized = Text::Localized(fixture.allocator, MessageId, MessageRevision, "Engine");
    REQUIRE(localized);
    {
        ProgressScope scope(1.0, localized.Value());
        CHECK(poller.Next().Message() == StringView("ìì§"));
    }

    ProgressHub::Get().Shutdown();
    const Polled ended = poller.Next();
    CHECK(ended.status.finished);
    CHECK(Near(ended.status.fraction, 1.0));
    source.release(source.context);
}

} // namespace startup_progress_tests_file
