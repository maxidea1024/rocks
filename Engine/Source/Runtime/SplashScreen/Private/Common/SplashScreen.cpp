#include "../SplashPresenter.h"

#include <Base/PlacementNew.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Move.h>
#include <Core/Text/String.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/File.h>
#include <Platform/Memory.h>
#include <Platform/PlatformMath.h>
#include <Platform/Sync.h>
#include <Platform/SystemDialog.h>
#include <Platform/Thread.h>
#include <SplashScreen/SplashScreen.h>

namespace rocks::splash {

namespace {

constexpr LogCategory LogSplash{"Splash"};

// 스플래시 스레드 이름과 스택 크기다(설계 9.2절 — WIC·GDI 호출 여유).
constexpr const char* SplashThreadName = "Splash";
constexpr usize SplashThreadStackBytes = usize{256} * 1024;

constexpr double NanosecondsPerSecond = 1'000'000'000.0;

// 프로세스에 스플래시는 하나다(Show가 0 → 1, Hide가 1 → 0).
platform::Atomic<uint32> SplashActive{0};

// 시스템 대화상자 hook(설계 9.5절)이다. 창을 숨기는 비동기 요청만 보낸다.
void HideSplashForSystemDialog() noexcept {
    PostSplashDialogHide();
}

uint64 DefaultClock(void* context) noexcept {
    (void)context;
    return platform::MonotonicNanoseconds();
}

struct PendingLine {
    char bytes[MaxSplashLineBytes];
    uint32 count = 0;
    bool changed = false;
};

} // namespace

struct SplashScreen::Shared {
    Shared(Allocator& memory, UniquePtr<SplashPresenter> created) noexcept
        : allocator(memory), presenter(Move(created)), paths(memory), windowTitle(memory) {
    }

    Allocator& allocator;
    UniquePtr<SplashPresenter> presenter;
    platform::Thread thread;
    bool threadStarted = false;
    bool hookInstalled = false;
    platform::Atomic<uint32> state{static_cast<uint32>(SplashState::Starting)};
    // 창이 뜬 시각(스플래시 시계)이다. state를 Shown으로 바꾸기 전에 쓴다.
    platform::Atomic<uint64> shownAt{0};
    // 최소 표시 시간(나노초)이다.
    uint64 minimumNanoseconds = 0;
    platform::Atomic<uint32> closeRequested{0};
    platform::Atomic<uint32> suspendRequested{0};
    // 호스트 스레드·다른 스레드의 SetText와 스플래시 스레드 사이의 칸 글자다.
    platform::Mutex mutex;
    PendingLine pending[SplashTextSlotCount];
    // Show가 정하고 스플래시 스레드가 읽는다(스레드 시작 전에 쓰고 바꾸지 않는다).
    SplashKind kind = SplashKind::Game;
    bool visible[SplashTextSlotCount]{};
    bool barVisible = true;
    SplashImagePaths paths;
    String windowTitle;
    SplashStatusSource source;
    uint64 (*clock)(void*) noexcept = &DefaultClock;
    void* clockContext = nullptr;

    uint64 Now() const noexcept {
        return clock(clockContext);
    }

    void StorePending(SplashTextSlot slot, StringView utf8) noexcept {
        const StringView fitted = utf8.LeftBytesAtCodePoint(MaxSplashLineBytes);
        platform::LockGuard guard(mutex);
        PendingLine& line = pending[SlotIndex(slot)];
        if (!fitted.IsEmpty()) {
            platform::MemoryCopy(line.bytes, fitted.Data(), fitted.ByteCount());
        }

        line.count = static_cast<uint32>(fitted.ByteCount());
        line.changed = true;
    }

    // 바뀐 칸 글자를 모델로 옮긴다(스플래시 스레드).
    void TakePending(SplashModel& model) noexcept {
        PendingLine taken[SplashTextSlotCount];
        {
            platform::LockGuard guard(mutex);
            for (usize index = 0; index < SplashTextSlotCount; ++index) {
                if (pending[index].changed) {
                    taken[index] = pending[index];
                    pending[index].changed = false;
                }
            }
        }

        for (usize index = 0; index < SplashTextSlotCount; ++index) {
            if (taken[index].changed) {
                model.SetText(static_cast<SplashTextSlot>(index),
                              StringView(taken[index].bytes, taken[index].count));
            }
        }
    }

    void ReleaseSource() noexcept {
        if (source.release != nullptr) {
            source.release(source.context);
        }

        source = {};
    }

    // 스플래시 스레드 본문이다(아래 정의).
    static void Run(void* context) noexcept;
};

// 스플래시 스레드 본문이다(설계 9.2절): 창 생성 → (원천 당기기·모델·다시 그리기·대기) 반복 → 창
// 정리.
void SplashScreen::Shared::Run(void* context) noexcept {
    auto& shared = *static_cast<Shared*>(context);
    SplashPresenter& presenter = *shared.presenter;
    SplashPresenterSetup setup;
    setup.kind = shared.kind;
    setup.standardPath = shared.paths.standard.View();
    setup.doublePath = shared.paths.doubled.View();
    setup.windowTitle = shared.windowTitle.View();
    const SplashCreateReport report = presenter.Create(setup);
    if (report.reason != SplashSkipReason::None) {
        Log::Warning(LogSplash,
                     "The splash screen could not be shown ({Reason}) at {Call} (OS error "
                     "{OsError}); startup continues without it",
                     SplashSkipReasonName(report.reason), StringView(report.call), report.osError);
        shared.state.Store<platform::MemoryOrder::Release>(
            static_cast<uint32>(SplashState::Failed));
        presenter.Destroy();
        return;
    }

    shared.shownAt.Store<platform::MemoryOrder::Relaxed>(shared.Now());
    shared.state.Store<platform::MemoryOrder::Release>(static_cast<uint32>(SplashState::Shown));
    uint64 now = shared.Now();
    SplashModel model(now);
    for (usize index = 0; index < SplashTextSlotCount; ++index) {
        model.SetVisible(static_cast<SplashTextSlot>(index), shared.visible[index]);
    }

    model.SetBarVisible(shared.barVisible);
    bool suspended = false;
    uint64 nextPoll = now;
    while (shared.closeRequested.Load<platform::MemoryOrder::Acquire>() == 0) {
        now = shared.Now();
        shared.TakePending(model);
        const bool wantSuspended =
            shared.suspendRequested.Load<platform::MemoryOrder::Acquire>() != 0;
        if (wantSuspended != suspended) {
            suspended = wantSuspended;
            presenter.SetSuspended(suspended);
        }

        if (now >= nextPoll) {
            if (shared.source.poll != nullptr) {
                SplashStatus status;
                if (shared.source.poll(shared.source.context, status)) {
                    model.ApplyStatus(status);
                }
            }

            nextPoll = now + SplashPollIntervalNanoseconds;
        }

        model.Advance(now);
        if (model.ConsumeDirty()) {
            presenter.Present(model);
        }

        const uint64 wake = model.NextWake(now, nextPoll);
        presenter.Wait(wake > now ? wake - now : 0);
    }

    presenter.Destroy();
}

namespace {

Result<UniquePtr<SplashPresenter>> CreatePlatformPresenter(Allocator& allocator,
                                                           void* context) noexcept {
    (void)context;
    return CreatePlatformSplashPresenter(allocator);
}

} // namespace

SplashScreen::SplashScreen(Shared* shared) noexcept : shared_(shared) {
}

SplashScreen::SplashScreen(SplashScreen&& other) noexcept : shared_(other.shared_) {
    other.shared_ = nullptr;
}

SplashScreen& SplashScreen::operator=(SplashScreen&& other) noexcept {
    if (this != &other) {
        Hide();
        shared_ = other.shared_;
        other.shared_ = nullptr;
    }

    return *this;
}

SplashScreen::~SplashScreen() {
    Hide();
}

Result<SplashScreen> SplashScreen::Show(Allocator& allocator, const SplashShowDesc& desc) noexcept {
    const SplashPresenterFactory factory{&CreatePlatformPresenter, nullptr};
    return ShowWith(allocator, desc, factory);
}

Result<SplashScreen> SplashScreen::ShowWith(Allocator& allocator, const SplashShowDesc& desc,
                                            const SplashPresenterFactory& factory) noexcept {
    // 실패 경로에서도 원천의 소유를 넘겨받았으므로 release를 부른다.
    SplashStatusSource source = desc.source;
    auto fail = [&source](ErrorCode code) noexcept {
        if (source.release != nullptr) {
            source.release(source.context);
        }

        return Result<SplashScreen>::Failure(code);
    };
    if ((desc.engineDirectory.IsEmpty() && desc.configuredImage.IsEmpty()) ||
        factory.create == nullptr ||
        !platform::PlatformMath::IsFinite(desc.minimumDisplaySeconds) ||
        desc.minimumDisplaySeconds < 0.0) {
        return fail(ErrorCode::InvalidArgument);
    }

    uint32 expected = 0;
    if (!SplashActive.CompareExchangeStrong<platform::MemoryOrder::AcquireRelease,
                                            platform::MemoryOrder::Acquire>(expected, 1)) {
        return fail(ErrorCode::AlreadyExists);
    }

    auto deactivate = [](ErrorCode code) noexcept {
        SplashActive.Store<platform::MemoryOrder::Release>(0);
        return code;
    };
    SplashImageQuery query;
    query.kind = desc.kind;
    query.configuredPath = desc.configuredImage;
    query.projectDirectory = desc.projectDirectory;
    query.engineDirectory = desc.engineDirectory;
    auto paths = ResolveSplashImage(allocator, platform::PhysicalFileSystem(), query);
    if (!paths) {
        return fail(deactivate(paths.Error()));
    }

    auto presenter = factory.create(allocator, factory.context);
    if (!presenter) {
        return fail(deactivate(presenter.Error()));
    }

    auto memory = allocator.Allocate(sizeof(Shared), alignof(Shared));
    if (!memory) {
        return fail(deactivate(memory.Error()));
    }

    Shared& shared = *::new (memory.Value()) Shared(allocator, Move(presenter.Value()));
    // Show가 실패하면 Shared를 파괴한다(스레드를 띄우기 전까지).
    auto destroy = [&shared]() noexcept {
        Allocator& owner = shared.allocator;
        shared.~Shared();
        (void)owner.Deallocate(&shared);
    };
    shared.kind = desc.kind;
    shared.paths.standard.Swap(paths.Value().standard);
    shared.paths.doubled.Swap(paths.Value().doubled);
    const bool studio = desc.kind == SplashKind::Studio;
    shared.visible[SlotIndex(SplashTextSlot::Title)] = studio || desc.showTitle;
    shared.visible[SlotIndex(SplashTextSlot::Version)] = studio || desc.showVersion;
    shared.visible[SlotIndex(SplashTextSlot::Progress)] = studio || desc.showProgressText;
    shared.visible[SlotIndex(SplashTextSlot::Copyright)] = studio || desc.showCopyright;
    shared.barVisible = studio || desc.showProgressBar;
    for (const SplashTextValue& value : desc.texts) {
        shared.StorePending(value.slot, value.text);
        if (value.slot == SplashTextSlot::Title) {
            if (auto assigned = shared.windowTitle.TryAssign(
                    value.text.LeftBytesAtCodePoint(MaxSplashLineBytes));
                !assigned) {
                destroy();
                return fail(deactivate(assigned.Error()));
            }
        }
    }

    if (desc.nowNanoseconds != nullptr) {
        shared.clock = desc.nowNanoseconds;
        shared.clockContext = desc.clockContext;
    }

    const double minimumSeconds = desc.minimumDisplaySeconds > MaxSplashMinimumDisplaySeconds
                                      ? MaxSplashMinimumDisplaySeconds
                                      : desc.minimumDisplaySeconds;
    shared.minimumNanoseconds = static_cast<uint64>(minimumSeconds * NanosecondsPerSecond);

    // 원천의 소유를 Shared로 옮긴다(이후 release는 Hide가 부른다).
    shared.source = source;
    source = {};
    platform::ThreadOptions options;
    options.name = SplashThreadName;
    options.stackBytes = SplashThreadStackBytes;
    if (shared.thread.Start(&Shared::Run, &shared, options) != platform::ThreadStatus::Success) {
        shared.ReleaseSource();
        destroy();
        return Result<SplashScreen>::Failure(deactivate(ErrorCode::DependencyFailed));
    }

    shared.threadStarted = true;
    shared.hookInstalled = platform::SetSystemDialogHook(&HideSplashForSystemDialog);
    if (!shared.hookInstalled) {
        Log::Debug(LogSplash, "Another system dialog hook is installed; the splash screen will not "
                              "hide itself before system dialogs");
    }

    return Result<SplashScreen>::Success(SplashScreen(&shared));
}

void SplashScreen::SetText(SplashTextSlot slot, StringView utf8) noexcept {
    if (shared_ == nullptr) {
        return;
    }

    shared_->StorePending(slot, utf8);
    shared_->presenter->Wake();
}

bool SplashScreen::CanHide() const noexcept {
    if (shared_ == nullptr) {
        return true;
    }

    switch (static_cast<SplashState>(shared_->state.Load<platform::MemoryOrder::Acquire>())) {
    case SplashState::Starting:
        return false;
    case SplashState::Shown: {
        const uint64 shownAt = shared_->shownAt.Load<platform::MemoryOrder::Relaxed>();
        const uint64 now = shared_->Now();
        return now >= shownAt && now - shownAt >= shared_->minimumNanoseconds;
    }
    case SplashState::Failed:
    case SplashState::Hidden:
        return true;
    }

    return true;
}

SplashState SplashScreen::State() const noexcept {
    if (shared_ == nullptr) {
        return SplashState::Hidden;
    }

    return static_cast<SplashState>(shared_->state.Load<platform::MemoryOrder::Acquire>());
}

void SplashScreen::Suspend() noexcept {
    if (shared_ != nullptr) {
        shared_->suspendRequested.Store<platform::MemoryOrder::Release>(1);
        shared_->presenter->Wake();
    }
}

void SplashScreen::Resume() noexcept {
    if (shared_ != nullptr) {
        shared_->suspendRequested.Store<platform::MemoryOrder::Release>(0);
        shared_->presenter->Wake();
    }
}

void SplashScreen::Hide() noexcept {
    if (shared_ == nullptr) {
        return;
    }

    Shared* shared = shared_;
    shared_ = nullptr;
    if (shared->hookInstalled) {
        platform::ClearSystemDialogHook(&HideSplashForSystemDialog);
    }

    if (shared->threadStarted) {
        // 창 생성 중이면 스레드는 생성을 마친 뒤 이 표시를 보고 정리한다(UE Hide의 생성 대기와 같은
        // 뜻 — 교착 없음).
        shared->closeRequested.Store<platform::MemoryOrder::Release>(1);
        shared->presenter->Wake();
        (void)shared->thread.Join();
    }

    shared->ReleaseSource();
    Allocator& allocator = shared->allocator;
    shared->~Shared();
    (void)allocator.Deallocate(shared);
    SplashActive.Store<platform::MemoryOrder::Release>(0);
}

} // namespace rocks::splash
