#include "../Private/SplashScreenTestAccess.h"

#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/File.h>
#include <Platform/SystemDialog.h>
#include <Platform/Thread.h>
#include <SplashScreen/SplashImage.h>
#include <SplashScreen/SplashLayout.h>
#include <SplashScreen/SplashModel.h>
#include <SplashScreen/SplashScreen.h>
#include <TestSupport/OsThread.h>
#include <TestSupport/Test.h>
#include <TestSupport/TestGate.h>

using namespace rocks;
using namespace rocks::splash;
namespace splash_screen_tests_file {

namespace {

constexpr uint64 Millisecond = 1'000'000;

// 모든 조건이 띄우기를 허락하는 입력이다(각 시험이 칸 하나씩 바꾼다).
SplashVisibilityInput AllowedInput() noexcept {
    SplashVisibilityInput input;
    input.platformSupported = true;
    input.interactiveHost = true;
    input.opensMainWindow = true;
    input.settingsEnabled = true;
    input.desktop = platform::DesktopSessionState::Connected;
    return input;
}

// 작업 디렉터리 아래 시험 전용 폴더다(소멸 때 지운다).
struct ScratchDirectory {
    ScratchDirectory() noexcept {
        static uint32 counter = 0;
        FixedStringBuilder text{Span<char>(root)};
        (void)text.TryAppendFormat(
            "rocks-splash-{}-{}", platform::MonotonicNanoseconds(),
            platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&counter, 1u));
        length = text.ByteCount();
    }

    ~ScratchDirectory() noexcept {
        (void)platform::DeleteDirectoryTree(platform::PhysicalFileSystem(), root, length);
    }

    StringView Path() const noexcept {
        return StringView(root, length);
    }

    // root/relative에 빈 파일을 만든다(폴더도 만든다).
    bool Touch(StringView relative) noexcept {
        char path[512];
        FixedStringBuilder text{Span<char>(path)};
        if (!text.TryAppendFormat("{}/{}", Path(), relative)) {
            return false;
        }

        usize folderEnd = text.ByteCount();
        while (folderEnd > 0 && path[folderEnd - 1] != '/') {
            --folderEnd;
        }

        if (!platform::MakeDirectoryTree(platform::PhysicalFileSystem(), path, folderEnd - 1)) {
            return false;
        }

        platform::File file;
        return static_cast<bool>(file.Open(platform::PhysicalFileSystem(), path, text.ByteCount(),
                                           platform::FileOpenMode::CreateAlways)) &&
               static_cast<bool>(file.Close());
    }

    char root[128] = {};
    usize length = 0;
};

bool EndsWith(StringView text, StringView suffix) noexcept {
    return text.ByteCount() >= suffix.ByteCount() &&
           text.SliceBytes(text.ByteCount() - suffix.ByteCount(), suffix.ByteCount()) == suffix;
}

} // namespace

// 표시 판정은 4절 표의 순서로 처음 맞는 사유를 돌려준다.
TEST(SplashScreen_VisibilityFollowsTheReasonOrder) {
    CHECK(DecideSplashVisibility(AllowedInput()) == SplashSkipReason::None);
    struct Case {
        void (*change)(SplashVisibilityInput&) noexcept;
        SplashSkipReason expected;
    };
    const Case cases[] = {
        {[](SplashVisibilityInput& input) noexcept { input.platformSupported = false; },
         SplashSkipReason::UnsupportedPlatform},
        {[](SplashVisibilityInput& input) noexcept { input.interactiveHost = false; },
         SplashSkipReason::NotInteractiveHost},
        {[](SplashVisibilityInput& input) noexcept { input.opensMainWindow = false; },
         SplashSkipReason::NoMainWindow},
        {[](SplashVisibilityInput& input) noexcept { input.noSplashOption = true; },
         SplashSkipReason::DisabledByCommandLine},
        {[](SplashVisibilityInput& input) noexcept { input.headless = true; },
         SplashSkipReason::Headless},
        {[](SplashVisibilityInput& input) noexcept { input.unattended = true; },
         SplashSkipReason::Unattended},
        {[](SplashVisibilityInput& input) noexcept { input.settingsEnabled = false; },
         SplashSkipReason::DisabledBySettings},
        {[](SplashVisibilityInput& input) noexcept {
             input.desktop = platform::DesktopSessionState::Disconnected;
         },
         SplashSkipReason::NoDesktopSession},
        {[](SplashVisibilityInput& input) noexcept { input.requiresMainThreadHost = true; },
         SplashSkipReason::NoMainThreadHost},
    };
    for (const Case& item : cases) {
        SplashVisibilityInput input = AllowedInput();
        item.change(input);
        CHECK(DecideSplashVisibility(input) == item.expected);
    }

    // 앞 사유가 뒤 사유를 이긴다.
    SplashVisibilityInput both = AllowedInput();
    both.noSplashOption = true;
    both.settingsEnabled = false;
    CHECK(DecideSplashVisibility(both) == SplashSkipReason::DisabledByCommandLine);
    // Unknown 세션(조회 실패·macOS)은 막지 않고, 메인 스레드 호스트가 있으면 macOS도 띄운다.
    SplashVisibilityInput unknown = AllowedInput();
    unknown.desktop = platform::DesktopSessionState::Unknown;
    unknown.requiresMainThreadHost = true;
    unknown.mainThreadHost = true;
    CHECK(DecideSplashVisibility(unknown) == SplashSkipReason::None);
    // 기본 입력은 게임 호스트·주 창이 없어 띄우지 않는다.
    CHECK((DecideSplashVisibility(SplashVisibilityInput{}) != SplashSkipReason::None));
    CHECK(SplashSkipReasonName(SplashSkipReason::NoMainWindow) == "NoMainWindow");
}

// 이 빌드(Windows)의 플랫폼 값이다.
TEST(SplashScreen_PlatformInputOnWindows) {
    const SplashVisibilityInput input = MakeSplashVisibilityInput();
    CHECK(input.platformSupported);
    CHECK(!input.requiresMainThreadHost);
}

// 프로젝트 자리가 엔진 자리를 이기고, 각 자리에서 확장자 순서·@2x를 본다.
TEST(SplashScreen_ImageLookupOrder) {
    SystemAllocator allocator;
    ScratchDirectory project;
    ScratchDirectory engine;
    REQUIRE(engine.Touch("Content/Splash/Splash.png"));
    REQUIRE(engine.Touch("Content/Splash/Splash@2x.png"));
    SplashImageQuery query;
    query.engineDirectory = engine.Path();
    query.projectDirectory = project.Path();
    {
        auto found = ResolveSplashImage(allocator, platform::PhysicalFileSystem(), query);
        REQUIRE(found);
        CHECK(EndsWith(found.Value().standard.View(), "Content/Splash/Splash.png"));
        CHECK(EndsWith(found.Value().doubled.View(), "Content/Splash/Splash@2x.png"));
    }

    // 프로젝트에 .jpg·.bmp만 있으면 .jpg(확장자 순서), 프로젝트가 엔진을 이긴다.
    REQUIRE(project.Touch("Content/Splash/Splash.bmp"));
    REQUIRE(project.Touch("Content/Splash/Splash.jpg"));
    {
        auto found = ResolveSplashImage(allocator, platform::PhysicalFileSystem(), query);
        REQUIRE(found);
        CHECK(EndsWith(found.Value().standard.View(), "Splash.jpg"));
        CHECK(found.Value().standard.View().ByteCount() > project.Path().ByteCount());
        CHECK(found.Value().doubled.IsEmpty());
    }

    // Studio는 StudioSplash를 찾는다. @2x만 있어도 찾는다.
    REQUIRE(engine.Touch("Content/Splash/StudioSplash@2x.png"));
    query.kind = SplashKind::Studio;
    {
        auto found = ResolveSplashImage(allocator, platform::PhysicalFileSystem(), query);
        REQUIRE(found);
        CHECK(found.Value().standard.IsEmpty());
        CHECK(EndsWith(found.Value().doubled.View(), "StudioSplash@2x.png"));
    }

    // 어디에도 없으면 NotFound다.
    query.projectDirectory = {};
    query.engineDirectory = project.Path();
    CHECK(ResolveSplashImage(allocator, platform::PhysicalFileSystem(), query).Error() ==
          ErrorCode::NotFound);
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 설정 경로는 그 경로만 보고({EngineDir} 펼침), 없으면 다른 자리로 넘어가지 않는다.
TEST(SplashScreen_ConfiguredImageIsTheOnlyPlace) {
    SystemAllocator allocator;
    ScratchDirectory engine;
    REQUIRE(engine.Touch("Content/Splash/Splash.png"));
    REQUIRE(engine.Touch("Art/Custom.png"));
    REQUIRE(engine.Touch("Art/Custom@2x.png"));
    SplashImageQuery query;
    query.engineDirectory = engine.Path();
    query.configuredPath = "{EngineDir}/Art/Custom.png";
    {
        auto found = ResolveSplashImage(allocator, platform::PhysicalFileSystem(), query);
        REQUIRE(found);
        CHECK(EndsWith(found.Value().standard.View(), "Art/Custom.png"));
        CHECK(EndsWith(found.Value().doubled.View(), "Art/Custom@2x.png"));
    }

    query.configuredPath = "{EngineDir}/Art/Missing.png";
    CHECK(ResolveSplashImage(allocator, platform::PhysicalFileSystem(), query).Error() ==
          ErrorCode::NotFound);
    query.configuredPath = "{GameDir}/Art/Custom.png";
    CHECK(ResolveSplashImage(allocator, platform::PhysicalFileSystem(), query).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 논리 크기 규칙(5.3절).
TEST(SplashScreen_LogicalSizeRules) {
    CHECK((SplashLogicalSize({768, 432}, {}).Value() == SplashSize{768, 432}));
    CHECK((SplashLogicalSize({}, {1536, 864}).Value() == SplashSize{768, 432}));
    CHECK((SplashLogicalSize({768, 432}, {1536, 864}).Value() == SplashSize{768, 432}));
    CHECK((!SplashLogicalSize({}, {})));
    CHECK((!SplashLogicalSize({768, 432}, {1536, 866}))); // 두 배가 아니다
    CHECK((!SplashLogicalSize({}, {1535, 864})));         // @2x만 있을 때 홀수
    CHECK((!SplashLogicalSize({100, 432}, {})));          // 최소 미만
    CHECK((SplashLogicalSize({MaxSplashImageDimension, 432}, {})));
    CHECK((!SplashLogicalSize({MaxSplashImageDimension + 1, 432}, {})));
    CHECK((SplashLogicalSize({2048, 432}, {4096, 864})));  // @2x가 상한과 같으면 받는다
    CHECK((!SplashLogicalSize({2049, 432}, {4098, 864}))); // @2x가 상한을 넘는다
}

// 배율 1.0을 넘고 @2x가 있으면 @2x, 1x가 없으면 늘 @2x다(5.3절).
TEST(SplashScreen_VariantAndWindowSize) {
    CHECK(ChooseSplashVariant(1.0, true, true) == SplashImageVariant::Standard);
    CHECK(ChooseSplashVariant(1.25, true, true) == SplashImageVariant::Double);
    CHECK(ChooseSplashVariant(2.0, true, false) == SplashImageVariant::Standard);
    CHECK(ChooseSplashVariant(1.0, false, true) == SplashImageVariant::Double);
    CHECK((SplashWindowSize({768, 432}, 1.0) == SplashSize{768, 432}));
    CHECK((SplashWindowSize({768, 432}, 1.25) == SplashSize{960, 540}));
    CHECK((SplashWindowSize({768, 432}, 1.5) == SplashSize{1152, 648}));
    CHECK((SplashWindowSize({768, 432}, 2.0) == SplashSize{1536, 864}));
    CHECK((SplashWindowSize({768, 432}, 0.0) == SplashSize{768, 432}));
    CHECK((SplashWindowSize({768, 432}, 100.0) == SplashSize{768 * 8, 432 * 8}));
}

// 기본 배치(6.2절)가 논리 px와 배율에 맞는다.
TEST(SplashScreen_DefaultLayout) {
    const SplashLayout one = ComputeSplashLayout({768, 432}, 1.0);
    CHECK((one.window == SplashSize{768, 432}));
    CHECK((one.text[0] == SplashRect{32, 348, 736, 376}));  // Title
    CHECK((one.text[1] == SplashRect{32, 377, 736, 395}));  // Version
    CHECK((one.text[2] == SplashRect{32, 398, 476, 416}));  // Progress(폭은 0.62까지)
    CHECK((one.text[3] == SplashRect{484, 399, 736, 415})); // Copyright
    CHECK((one.bar == SplashRect{32, 420, 736, 422}));
    CHECK(one.style[0].fontPixels == 20 && one.style[0].semibold);
    CHECK(one.style[3].align == SplashTextAlign::Right);
    const SplashLayout two = ComputeSplashLayout({768, 432}, 2.0);
    CHECK((two.text[0] == SplashRect{64, 696, 1472, 752}));
    CHECK((two.bar == SplashRect{64, 840, 1472, 844}));
    CHECK(two.style[1].fontPixels == 24);
    // 아래 띠는 SplashTextBandHeight 안에 있다.
    CHECK(one.text[0].top >= 432 - SplashTextBandHeight);
}

// 칸 글자는 UTF-8 문자 경계에서 잘리고, 바뀔 때만 dirty다.
TEST(SplashScreen_ModelTextAndDirty) {
    SplashModel model(0);
    CHECK(model.ConsumeDirty());
    CHECK(!model.ConsumeDirty());
    model.SetText(SplashTextSlot::Title, "Rocks");
    CHECK(model.ConsumeDirty());
    model.SetText(SplashTextSlot::Title, "Rocks");
    CHECK(!model.ConsumeDirty());
    char longText[MaxSplashLineBytes + 3];
    for (usize index = 0; index + 3 <= sizeof(longText); index += 3) {
        longText[index] = '\xEA'; // "가"(3 byte)를 반복
        longText[index + 1] = '\xB0';
        longText[index + 2] = '\x80';
    }

    model.SetText(SplashTextSlot::Version, StringView(longText, sizeof(longText)));
    const StringView stored = model.Text(SplashTextSlot::Version);
    CHECK(stored.ByteCount() <= MaxSplashLineBytes);
    CHECK(stored.ByteCount() % 3 == 0);
    CHECK(stored.IsValidUtf8());
}

// 진행 문구 서식: 백분율 + 문구, 끝나기 전 99% 상한, 끝나면 100%, 문구 없으면 숫자만.
TEST(SplashScreen_ModelProgressText) {
    SplashModel model(0);
    SplashStatus status;
    status.determinate = true;
    status.fraction = 0.426;
    status.messageChanged = true;
    const StringView message = "Starting modules";
    for (usize index = 0; index < message.ByteCount(); ++index) {
        status.message[index] = message.Data()[index];
    }

    status.messageBytes = static_cast<uint32>(message.ByteCount());
    model.ApplyStatus(status);
    CHECK(model.Text(SplashTextSlot::Progress) == "42%  Starting modules");
    status.messageChanged = false;
    status.fraction = 0.9999;
    model.ApplyStatus(status);
    CHECK(model.Text(SplashTextSlot::Progress) == "99%  Starting modules");
    status.finished = true;
    model.ApplyStatus(status);
    CHECK(model.Text(SplashTextSlot::Progress) == "100%  Starting modules");

    SplashModel bare(0);
    SplashStatus numbers;
    numbers.determinate = true;
    numbers.fraction = 0.5;
    bare.ApplyStatus(numbers);
    CHECK(bare.Text(SplashTextSlot::Progress) == "50%");
    // SetText가 덮어도 다음 보고가 다시 원천의 문구로 돌린다.
    bare.SetText(SplashTextSlot::Progress, "Custom");
    CHECK(bare.Text(SplashTextSlot::Progress) == "Custom");
    numbers.fraction = 0.6;
    bare.ApplyStatus(numbers);
    CHECK(bare.Text(SplashTextSlot::Progress) == "60%");
    // 불확정이면 문구만이다.
    SplashModel loading(0);
    SplashStatus indeterminate;
    indeterminate.messageChanged = true;
    indeterminate.message[0] = 'L';
    indeterminate.messageBytes = 1;
    loading.ApplyStatus(indeterminate);
    CHECK(loading.Text(SplashTextSlot::Progress) == "L");
    CHECK(loading.BarMode() == SplashBarMode::Indeterminate);
}

// 막대는 주입 시계로 목표를 향해 단조롭게 다가가 settle하고, 목표는 뒤로 가지 않는다.
TEST(SplashScreen_ModelBarEasesMonotonically) {
    SplashModel model(0);
    CHECK(model.BarMode() == SplashBarMode::Indeterminate);
    SplashStatus status;
    status.determinate = true;
    status.fraction = 0.5;
    model.ApplyStatus(status);
    CHECK(model.BarMode() == SplashBarMode::Determinate);
    CHECK(model.BarValue() == 0.0);
    double previous = 0.0;
    uint64 now = 0;
    for (int step = 0; step < 200; ++step) {
        now += 16 * Millisecond;
        model.Advance(now);
        CHECK(model.BarValue() >= previous);
        CHECK(model.BarValue() <= 0.5);
        previous = model.BarValue();
    }

    CHECK(model.BarValue() == 0.5);
    CHECK(model.NextWake(now, now + 100 * Millisecond) == now + 100 * Millisecond);
    status.fraction = 0.3; // 뒤로 가는 보고는 무시한다
    model.ApplyStatus(status);
    CHECK(model.BarTarget() == 0.5);
    status.fraction = 0.8;
    model.ApplyStatus(status);
    model.Advance(now + 16 * Millisecond);
    CHECK(model.BarValue() > 0.5);
    CHECK(model.NextWake(now + 16 * Millisecond, now + 1000 * Millisecond) ==
          now + 16 * Millisecond + SplashAnimationIntervalNanoseconds);
    // 숨긴 막대는 Hidden이고 깨울 일이 없다.
    model.SetBarVisible(false);
    CHECK(model.BarMode() == SplashBarMode::Hidden);
}

// 불확정 조각 위치는 주기로 돈다.
TEST(SplashScreen_ModelMarqueePhase) {
    SplashModel model(1000);
    model.Advance(1000 + SplashMarqueePeriodNanoseconds / 4);
    CHECK(model.MarqueePhase() == 0.25);
    model.Advance(1000 + SplashMarqueePeriodNanoseconds + SplashMarqueePeriodNanoseconds / 2);
    CHECK(model.MarqueePhase() == 0.5);
    // 시계가 뒤로 가도 앞 시각을 쓴다.
    model.Advance(0);
    CHECK(model.MarqueePhase() == 0.5);
}

namespace {

// 가짜 presenter다: Create가 gate에서 기다리게 할 수 있고, 불린 일을 센다.
struct FakeRecord {
    test::TestGate createEntered;
    test::TestGate allowCreate;
    test::TestGate presented;
    test::TestGate destroyed;
    platform::ManualResetEvent wake;
    platform::Atomic<uint32> creates{0};
    platform::Atomic<uint32> destroys{0};
    platform::Atomic<uint32> presents{0};
    platform::Atomic<uint32> suspended{0};
    SplashSkipReason failWith = SplashSkipReason::None;
    platform::Mutex mutex;
    char lastTitle[MaxSplashLineBytes] = {};
    usize lastTitleBytes = 0;
};

class FakePresenter final : public SplashPresenter {
public:
    explicit FakePresenter(FakeRecord& record) noexcept : record_(record) {
    }

    SplashCreateReport Create(const SplashPresenterSetup& setup) noexcept override {
        (void)setup;
        record_.creates.FetchAdd<platform::MemoryOrder::AcquireRelease>(1);
        record_.createEntered.Open();
        record_.allowCreate.Wait();
        SplashCreateReport report;
        report.reason = record_.failWith;
        return report;
    }

    void Present(const SplashModel& model) noexcept override {
        const StringView title = model.Text(SplashTextSlot::Title);
        {
            platform::LockGuard guard(record_.mutex);
            for (usize index = 0; index < title.ByteCount(); ++index) {
                record_.lastTitle[index] = title.Data()[index];
            }

            record_.lastTitleBytes = title.ByteCount();
        }

        record_.presents.FetchAdd<platform::MemoryOrder::AcquireRelease>(1);
        record_.presented.Open();
    }

    void SetSuspended(bool suspended) noexcept override {
        record_.suspended.Store<platform::MemoryOrder::Release>(suspended ? 1u : 0u);
    }

    void Wait(uint64 timeoutNanoseconds) noexcept override {
        // 깨움이나 제한 시간(애니메이션 간격)까지다. 시험은 이 시간에 기대지 않는다(끝 상태만
        // 본다).
        const uint64 milliseconds = timeoutNanoseconds / Millisecond + 1;
        (void)record_.wake.Wait(static_cast<uint32>(milliseconds));
        record_.wake.Reset();
    }

    void Wake() noexcept override {
        record_.wake.Signal();
    }

    void Destroy() noexcept override {
        record_.destroys.FetchAdd<platform::MemoryOrder::AcquireRelease>(1);
        record_.destroyed.Open();
    }

private:
    FakeRecord& record_;
};

Result<UniquePtr<SplashPresenter>> CreateFake(Allocator& allocator, void* context) noexcept {
    auto created = UniquePtr<FakePresenter>::TryMake(allocator, *static_cast<FakeRecord*>(context));
    if (!created) {
        return Result<UniquePtr<SplashPresenter>>::Failure(created.Error());
    }

    return Result<UniquePtr<SplashPresenter>>::Success(
        UniquePtr<SplashPresenter>(Move(created.Value())));
}

struct ReleaseCounter {
    platform::Atomic<uint32> releases{0};
    platform::Atomic<uint32> polls{0};
};

SplashStatusSource CountingSource(ReleaseCounter& counter) noexcept {
    SplashStatusSource source;
    source.context = &counter;
    source.poll = [](void* context, SplashStatus& out) noexcept {
        static_cast<ReleaseCounter*>(context)
            ->polls.FetchAdd<platform::MemoryOrder::AcquireRelease>(1);
        out.determinate = true;
        out.fraction = 0.25;
        return true;
    };
    source.release = [](void* context) noexcept {
        static_cast<ReleaseCounter*>(context)
            ->releases.FetchAdd<platform::MemoryOrder::AcquireRelease>(1);
    };
    return source;
}

// 엔진 폴더 = 이미지 하나가 있는 시험 폴더다.
struct ShowEnvironment {
    ShowEnvironment() noexcept {
        ready = engine.Touch("Content/Splash/Splash.png");
    }

    SplashShowDesc Desc() const noexcept {
        SplashShowDesc desc;
        desc.engineDirectory = engine.Path();
        return desc;
    }

    ScratchDirectory engine;
    bool ready = false;
};

} // namespace

// 창 생성이 끝나기 전에 Hide해도 교착 없이 끝나고(생성 뒤 정리), 원천 release와 Destroy가 한
// 번씩이다.
TEST(SplashScreen_HideDuringCreateDoesNotDeadlock) {
    SystemAllocator allocator;
    ShowEnvironment environment;
    REQUIRE(environment.ready);
    FakeRecord record;
    ReleaseCounter counter;
    SplashShowDesc desc = environment.Desc();
    desc.source = CountingSource(counter);
    const SplashPresenterFactory factory{&CreateFake, &record};
    auto shown = SplashScreenTestAccess::Show(allocator, desc, factory);
    REQUIRE(shown);
    record.createEntered.Wait();
    CHECK(shown.Value().State() == SplashState::Starting);
    // 다른 스레드가 생성을 풀어 준다. Hide는 그보다 먼저든 나중이든 스레드 끝까지 기다린다(교착
    // 없음).
    platform::Thread opener;
    REQUIRE(
        opener.Start(
            [](void* context) noexcept { static_cast<FakeRecord*>(context)->allowCreate.Open(); },
            &record) == platform::ThreadStatus::Success);
    shown.Value().Hide();
    (void)opener.Join();
    CHECK(shown.Value().State() == SplashState::Hidden);
    CHECK(record.creates.Load<platform::MemoryOrder::Acquire>() == 1);
    CHECK(record.destroys.Load<platform::MemoryOrder::Acquire>() == 1);
    CHECK(counter.releases.Load<platform::MemoryOrder::Acquire>() == 1);
    shown.Value().Hide(); // 멱등
    CHECK(counter.releases.Load<platform::MemoryOrder::Acquire>() == 1);
}

// 떠 있는 동안: 글자가 다른 스레드에서 전달되고, 원천을 당기며, 두 번째 Show는 AlreadyExists다.
TEST(SplashScreen_ShownSplashTakesTextAndPolls) {
    SystemAllocator allocator;
    ShowEnvironment environment;
    REQUIRE(environment.ready);
    FakeRecord record;
    record.allowCreate.Open();
    ReleaseCounter counter;
    SplashShowDesc desc = environment.Desc();
    desc.source = CountingSource(counter);
    const SplashTextValue texts[] = {{SplashTextSlot::Title, "First"}};
    desc.texts = texts;
    const SplashPresenterFactory factory{&CreateFake, &record};
    auto shown = SplashScreenTestAccess::Show(allocator, desc, factory);
    REQUIRE(shown);
    record.presented.Wait();
    CHECK(shown.Value().State() == SplashState::Shown);

    // 두 번째 Show는 거부되고 그 원천은 바로 release된다.
    ReleaseCounter second;
    SplashShowDesc again = environment.Desc();
    again.source = CountingSource(second);
    CHECK(SplashScreenTestAccess::Show(allocator, again, factory).Error() ==
          ErrorCode::AlreadyExists);
    CHECK(second.releases.Load<platform::MemoryOrder::Acquire>() == 1);

    // 다른 스레드의 SetText가 다음 그리기에 보인다.
    record.presented.Reset();
    REQUIRE(test::RunOnOsThread(
        [](void* context) noexcept {
            static_cast<SplashScreen*>(context)->SetText(SplashTextSlot::Title, "Second");
        },
        &shown.Value()));
    bool seen = false;
    while (!seen) {
        record.presented.Wait();
        record.presented.Reset();
        platform::LockGuard guard(record.mutex);
        seen = StringView(record.lastTitle, record.lastTitleBytes) == "Second";
    }

    CHECK(counter.polls.Load<platform::MemoryOrder::Acquire>() >= 1);
    shown.Value().Suspend();
    shown.Value().Resume();
    shown.Value().Hide();
    CHECK(counter.releases.Load<platform::MemoryOrder::Acquire>() == 1);
    CHECK(record.destroys.Load<platform::MemoryOrder::Acquire>() == 1);
    // Hide 뒤에는 다시 띄울 수 있다.
    FakeRecord next;
    next.allowCreate.Open();
    auto reopened = SplashScreenTestAccess::Show(allocator, environment.Desc(),
                                                 SplashPresenterFactory{&CreateFake, &next});
    CHECK(reopened);
    reopened.Value().Hide();
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 창 생성 실패는 Failed 상태가 되고, Hide는 정상으로 끝난다.
TEST(SplashScreen_CreateFailureIsReportedAsFailed) {
    SystemAllocator allocator;
    ShowEnvironment environment;
    REQUIRE(environment.ready);
    FakeRecord record;
    record.failWith = SplashSkipReason::WindowFailed;
    record.allowCreate.Open();
    ReleaseCounter counter;
    SplashShowDesc desc = environment.Desc();
    desc.source = CountingSource(counter);
    auto shown =
        SplashScreenTestAccess::Show(allocator, desc, SplashPresenterFactory{&CreateFake, &record});
    REQUIRE(shown);
    record.destroyed.Wait();

    CHECK(shown.Value().State() == SplashState::Failed);
    shown.Value().Hide();
    CHECK(counter.releases.Load<platform::MemoryOrder::Acquire>() == 1);
    CHECK(record.destroys.Load<platform::MemoryOrder::Acquire>() == 1);
}

// 입력 오류: 엔진·설정 경로가 없으면 InvalidArgument, 이미지가 없으면 NotFound — 둘 다 원천을
// release한다.
TEST(SplashScreen_ShowInputErrorsReleaseTheSource) {
    SystemAllocator allocator;
    FakeRecord record;
    ReleaseCounter counter;
    SplashShowDesc desc;
    desc.source = CountingSource(counter);
    const SplashPresenterFactory factory{&CreateFake, &record};
    CHECK(SplashScreenTestAccess::Show(allocator, desc, factory).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(counter.releases.Load<platform::MemoryOrder::Acquire>() == 1);
    ScratchDirectory empty;
    desc.engineDirectory = empty.Path();
    desc.source = CountingSource(counter);
    CHECK(SplashScreenTestAccess::Show(allocator, desc, factory).Error() == ErrorCode::NotFound);
    CHECK(counter.releases.Load<platform::MemoryOrder::Acquire>() == 2);
    CHECK(record.creates.Load<platform::MemoryOrder::Acquire>() == 0);
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 최소 표시 시간(9.7절): 주입 시계로 창이 뜬 뒤 1.9초에는 거짓, 2.0초에는 참이다. 생성 중에는 거짓,
// 최소 0이면 뜨자마자 참, 생성 실패면 참, 음수는 Show가 거부한다.
TEST(SplashScreen_MinimumDisplayTimeGatesCanHide) {
    struct FakeClock {
        platform::Atomic<uint64> now{1'000'000};

        static uint64 Read(void* context) noexcept {
            return static_cast<FakeClock*>(context)->now.Load<platform::MemoryOrder::Acquire>();
        }
    };
    constexpr uint64 Second = 1'000'000'000;
    SystemAllocator allocator;
    ShowEnvironment environment;
    REQUIRE(environment.ready);
    {
        FakeRecord record;
        FakeClock clock;
        SplashShowDesc desc = environment.Desc();
        desc.nowNanoseconds = &FakeClock::Read;
        desc.clockContext = &clock;
        auto shown = SplashScreenTestAccess::Show(allocator, desc,
                                                  SplashPresenterFactory{&CreateFake, &record});
        REQUIRE(shown);
        record.createEntered.Wait();
        CHECK(!shown.Value().CanHide()); // 아직 만드는 중
        record.allowCreate.Open();
        record.presented.Wait(); // Shown 뒤 첫 그리기 — 창이 뜬 시각은 시계를 움직이기 전 값이다
        CHECK(shown.Value().State() == SplashState::Shown);
        CHECK(!shown.Value().CanHide());
        clock.now.Store<platform::MemoryOrder::Release>(1'000'000 + 19 * Second / 10);
        CHECK(!shown.Value().CanHide());
        clock.now.Store<platform::MemoryOrder::Release>(1'000'000 + 2 * Second);
        CHECK(shown.Value().CanHide());
        shown.Value().Hide();
        CHECK(shown.Value().CanHide());
    }

    {
        FakeRecord record;
        record.allowCreate.Open();
        FakeClock clock;
        SplashShowDesc desc = environment.Desc();
        desc.nowNanoseconds = &FakeClock::Read;
        desc.clockContext = &clock;
        desc.minimumDisplaySeconds = 0.0;
        auto shown = SplashScreenTestAccess::Show(allocator, desc,
                                                  SplashPresenterFactory{&CreateFake, &record});
        REQUIRE(shown);
        record.presented.Wait();
        CHECK(shown.Value().CanHide());
        shown.Value().Hide();
    }

    {
        FakeRecord record;
        record.allowCreate.Open();
        record.failWith = SplashSkipReason::WindowFailed;
        auto shown = SplashScreenTestAccess::Show(allocator, environment.Desc(),
                                                  SplashPresenterFactory{&CreateFake, &record});
        REQUIRE(shown);
        record.destroyed.Wait();
        CHECK(shown.Value().CanHide()); // 창이 없으면 기다리지 않는다
        shown.Value().Hide();
    }

    FakeRecord unused;
    SplashShowDesc negative = environment.Desc();
    negative.minimumDisplaySeconds = -1.0;
    CHECK(SplashScreenTestAccess::Show(allocator, negative,
                                       SplashPresenterFactory{&CreateFake, &unused})
              .Error() == ErrorCode::InvalidArgument);
    CHECK(unused.creates.Load<platform::MemoryOrder::Acquire>() == 0);
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 떠 있는 동안 스플래시가 대화상자 hook을 갖고, Hide가 푼다(hook은 하나 — 9.5절).
TEST(SplashScreen_DialogHookIsHeldWhileShown) {
    SystemAllocator allocator;
    ShowEnvironment environment;
    REQUIRE(environment.ready);
    FakeRecord record;
    record.allowCreate.Open();
    const platform::SystemDialogHook other = []() noexcept {};
    auto shown = SplashScreenTestAccess::Show(allocator, environment.Desc(),
                                              SplashPresenterFactory{&CreateFake, &record});
    REQUIRE(shown);
    CHECK(!platform::SetSystemDialogHook(other));
    platform::NotifySystemDialogOpening(); // 창이 없는 가짜 — 아무 일도 하지 않는다
    shown.Value().Hide();
    CHECK(platform::SetSystemDialogHook(other));
    CHECK(platform::SetSystemDialogHook(other)); // 같은 hook은 다시 걸어도 된다
    platform::ClearSystemDialogHook(other);
    CHECK(platform::SetSystemDialogHook(other));
    platform::ClearSystemDialogHook(other);
    CHECK(!platform::SetSystemDialogHook(nullptr));
}

} // namespace splash_screen_tests_file
