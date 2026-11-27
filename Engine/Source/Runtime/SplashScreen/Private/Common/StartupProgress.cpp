#include "StartupProfile.h"

#include <Core/Diagnostics/Log.h>
#include <Core/Move.h>
#include <Localization/Catalog.h>
#include <Localization/Text.h>
#include <Platform/Clock.h>
#include <Platform/Memory.h>
#include <Progress/ProgressBinding.h>
#include <Progress/ProgressHub.h>
#include <Progress/ProgressScope.h>
#include <SplashScreen/StartupProgress.h>

namespace rocks::splash {

namespace {

constexpr LogCategory LogStartupProgress{"Startup"};

constexpr double StartupNanosecondsPerSecond = 1'000'000'000.0;

// 단계 이름과 단계 문구다(8.1절). 문구는 지역화 전 원문이다(Invariant).
constexpr const char* StartupStepNames[StartupStepCount] = {
    "HostServices", "Engine", "Assets", "Shaders", "Content", "FirstFrame"};
constexpr const char* StartupStepMessages[StartupStepCount] = {
    "Starting services", "Starting the engine", "Loading assets",
    "Loading shaders",   "Loading content",     "Showing the first frame"};

// 시작 작업 제목 형식이다(Text::Format — {App}은 앱 이름).
constexpr StringView StartupTitlePattern = "Starting {App}";
constexpr StringView StartupTitleArgument = "App";

// 첫 구간 문구가 나오기 전의 기본 문구다(7.2절).
constexpr StringView StartupDefaultMessage = "Loading\xE2\x80\xA6";

uint64 StartupDefaultClock(void* context) noexcept {
    (void)context;
    return platform::MonotonicNanoseconds();
}

// 스플래시 상태 원천의 문맥이다(7.1절). 시작 작업 id·자기 스냅샷·cursor·카탈로그 사본만 갖는다.
struct StartupSourceContext {
    explicit StartupSourceContext(Allocator& memory) noexcept
        : allocator(memory), snapshot(memory), resolved(memory) {
    }

    Allocator& allocator;
    ProgressTaskId task = NoProgressTask;
    ProgressSnapshot snapshot;
    uint64 cursor = 0;
    CatalogSnapshot catalog;
    String resolved;
    // 마지막으로 보고한 값이다.
    double fraction = 0.0;
    bool finished = false;
    bool reported = false;
    char message[MaxSplashLineBytes]{};
    uint32 messageBytes = 0;
};

// 시작 작업의 한 줄 문구를 고른다(7.2절 3): 가장 안쪽의 비어 있지 않은 구간 문구, 없으면 작업
// 문구, 없으면 null(기본 문구). 제목은 쓰지 않는다.
const Text* StartupPickMessage(const ProgressTaskView& view) noexcept {
    for (uint32 depth = view.messageCount; depth != 0; --depth) {
        if (view.messages[depth - 1].IsValid()) {
            return &view.messages[depth - 1];
        }
    }

    return view.taskMessage.IsValid() ? &view.taskMessage : nullptr;
}

bool StartupPoll(void* context, SplashStatus& out) noexcept {
    auto& source = *static_cast<StartupSourceContext*>(context);
    double fraction = source.fraction;
    bool finished = source.finished;
    StringView message = StartupDefaultMessage;
    const ProgressTaskView* view = nullptr;
    if (!finished) {
        // 사건은 쓰지 않으므로 매번 "지금부터"로 당긴다(사건 사본을 만들지 않는다).
        source.cursor = 0;
        auto taken = ProgressHub::Get().Snapshot(source.snapshot, source.cursor);
        if (!taken) {
            // hub가 켜지기 전이거나 닫혔으면 시작은 끝난 것이다. 할당 실패 같은 일시 오류는 지난
            // 상태를 그대로 둔다(다음 poll이 다시 당긴다).
            if (taken.Error() != ErrorCode::NotReady && taken.Error() != ErrorCode::ShuttingDown) {
                return false;
            }
        } else {
            for (const ProgressTaskView& task : source.snapshot.tasks) {
                if (task.id == source.task) {
                    view = &task;
                    break;
                }
            }
        }

        // 작업이 끝났거나(스냅샷에 없음) hub가 닫혔으면 끝남이다.
        finished = view == nullptr;
    }

    if (view != nullptr) {
        // 시작 작업 자기 진행만 본다(하위 작업은 단계 밖의 항이라 섞지 않는다 — 7.2절). 뒤로 가지
        // 않게 지난 값과 큰 쪽을 쓴다.
        fraction = view->selfFraction > fraction ? view->selfFraction : fraction;
        if (const Text* picked = StartupPickMessage(*view)) {
            const CatalogSnapshot& catalog = source.catalog;
            if (auto resolved = ResolveText(source.allocator, *picked, catalog)) {
                source.resolved = Move(resolved.Value().value);
                message = source.resolved.View();
            }
        }
    } else if (finished) {
        fraction = 1.0;
        message = StringView(source.message, source.messageBytes);
    }

    message = message.LeftBytesAtCodePoint(MaxSplashLineBytes);
    const bool messageChanged =
        !source.reported || message != StringView(source.message, source.messageBytes);
    const bool changed = messageChanged || fraction != source.fraction ||
                         finished != source.finished || !source.reported;
    if (messageChanged) {
        if (!message.IsEmpty()) {
            platform::MemoryCopy(source.message, message.Data(), message.ByteCount());
        }

        source.messageBytes = static_cast<uint32>(message.ByteCount());
    }

    source.fraction = fraction;
    source.finished = finished;
    source.reported = true;
    // 스냅샷의 Text 사본(다른 작업의 문구 포함)은 다음 poll까지 쥐지 않는다 — 그 Text를 만든
    // allocator의 수명 계약을 넓히지 않게.
    source.snapshot.tasks.Clear();
    source.snapshot.events.Clear();
    if (!changed) {
        return false;
    }

    out.determinate = fraction > 0.0 || finished;
    out.fraction = fraction;
    out.finished = finished;
    out.messageChanged = messageChanged;
    if (messageChanged) {
        if (source.messageBytes != 0) {
            platform::MemoryCopy(out.message, source.message, source.messageBytes);
        }

        out.messageBytes = source.messageBytes;
    }

    return true;
}

void StartupReleaseSource(void* context) noexcept {
    auto* source = static_cast<StartupSourceContext*>(context);
    Allocator& allocator = source->allocator;
    source->~StartupSourceContext();
    (void)allocator.Deallocate(source);
}

} // namespace

const char* StartupStepName(StartupStep step) noexcept {
    const auto index = static_cast<uint32>(step);
    return index < StartupStepCount ? StartupStepNames[index] : "Unknown";
}

struct StartupProgress::State {
    explicit State(Allocator& memory) noexcept
        : allocator(memory), profilePath(memory), profileTarget(memory), profileText(memory) {
    }

    Allocator& allocator;
    ProgressTaskHandle task;
    alignas(ProgressBinding) unsigned char bindingStorage[sizeof(ProgressBinding)];
    ProgressBinding* binding = nullptr;
    alignas(ProgressScope) unsigned char scopeStorage[sizeof(ProgressScope)];
    ProgressScope* scope = nullptr;
    Text stepMessages[StartupStepCount];
    double weights[StartupStepCount]{};
    bool included[StartupStepCount]{};
    bool entered[StartupStepCount]{};
    uint32 current = StartupStepCount;
    uint64 stepStarted = 0;
    StartupStepTimes times;
    uint64 (*clock)(void*) noexcept = StartupDefaultClock;
    void* clockContext = nullptr;
    bool warnedMisuse = false;
    bool finished = false;
    // MarkReady 뒤에는 단계 시간을 재지 않는다.
    bool ready = false;
    // 보정 프로필(8.2절)이다. 경로가 비면 끈다.
    String profilePath;
    String profileTarget;
    String profileText;
    detail::StartupProfileEntry saved;

    uint64 Now() const noexcept {
        return clock(clockContext);
    }

    // 지금 단계의 시간을 닫는다.
    void CloseStep() noexcept {
        if (!ready && current < StartupStepCount) {
            const uint64 now = Now();
            times.seconds[current] +=
                static_cast<double>(now - stepStarted) / StartupNanosecondsPerSecond;
            stepStarted = now;
        }
    }

    // 구간·바인딩을 연 역순으로 닫는다(호스트 스레드).
    void CloseScopeAndBinding() noexcept {
        if (scope != nullptr) {
            scope->~ProgressScope();
            scope = nullptr;
        }

        if (binding != nullptr) {
            binding->~ProgressBinding();
            binding = nullptr;
        }
    }
};

StartupProgress::StartupProgress(StartupProgress&& other) noexcept : state_(other.state_) {
    other.state_ = nullptr;
}

StartupProgress& StartupProgress::operator=(StartupProgress&& other) noexcept {
    if (this != &other) {
        Release();
        state_ = other.state_;
        other.state_ = nullptr;
    }

    return *this;
}

StartupProgress::~StartupProgress() noexcept {
    Release();
}

void StartupProgress::Release() noexcept {
    State* state = state_;
    state_ = nullptr;
    if (state == nullptr) {
        return;
    }

    // Finish 없이 끝나면 구간·바인딩을 닫고 작업은 handle 소멸이 Abandoned로 끝낸다.
    state->CloseScopeAndBinding();
    Allocator& allocator = state->allocator;
    state->~State();
    (void)allocator.Deallocate(state);
}

Result<StartupProgress> StartupProgress::Begin(Allocator& allocator,
                                               const StartupProgressDesc& desc) noexcept {
    if (desc.appTitle == nullptr || !desc.appTitle->IsValid()) {
        return Result<StartupProgress>::Failure(ErrorCode::InvalidArgument);
    }

    bool included[StartupStepCount]{};
    for (const StartupStep step : desc.steps) {
        const auto index = static_cast<uint32>(step);
        if (index >= StartupStepCount || included[index]) {
            return Result<StartupProgress>::Failure(ErrorCode::InvalidArgument);
        }

        included[index] = true;
    }

    if (desc.steps.IsEmpty()) {
        for (bool& flag : included) {
            flag = true;
        }
    }

    if (!desc.profilePath.IsEmpty() &&
        (desc.profileTarget.IsEmpty() || desc.profileTarget == StringView("v"))) {
        return Result<StartupProgress>::Failure(ErrorCode::InvalidArgument);
    }

    auto memory = allocator.Allocate(sizeof(State), alignof(State));
    if (!memory) {
        return Result<StartupProgress>::Failure(memory.Error());
    }

    StartupProgress progress(InitializeAt<State>(memory.Value(), allocator));
    State& state = *progress.state_;
    if (desc.nowNanoseconds != nullptr) {
        state.clock = desc.nowNanoseconds;
        state.clockContext = desc.clockContext;
    }

    // 보정 프로필(8.2절): 지난 실행의 단계 시간이 가중치다.
    if (!desc.profilePath.IsEmpty()) {
        if (auto copied = state.profilePath.TryAppend(desc.profilePath); !copied) {
            return Result<StartupProgress>::Failure(copied.Error());
        }

        if (auto copied = state.profileTarget.TryAppend(desc.profileTarget); !copied) {
            return Result<StartupProgress>::Failure(copied.Error());
        }

        const detail::StartupProfileLoad loaded = detail::LoadStartupProfile(
            allocator, desc.profilePath, desc.profileTarget, state.saved, state.profileText);
        if (loaded == detail::StartupProfileLoad::Invalid) {
            Log::Warning(LogStartupProgress,
                         "Startup profile {Path} is invalid for {Target}; default step weights are "
                         "used and the entry is rewritten after a successful start",
                         desc.profilePath, desc.profileTarget);
        } else if (loaded == detail::StartupProfileLoad::ReadFailed) {
            // 다른 대상 항목을 모르는 채 덮어쓰지 않도록 이번 실행은 쓰지 않는다.
            Log::Warning(LogStartupProgress,
                         "Startup profile {Path} could not be read; default step weights are used "
                         "and the profile is not updated by this run",
                         desc.profilePath);
            state.profilePath.Clear();
        }
    }

    detail::ComputeStartupWeights(included, state.saved, state.weights);
    double total = 0.0;
    for (uint32 index = 0; index < StartupStepCount; ++index) {
        state.included[index] = included[index];
        total += state.weights[index];
        if (!included[index]) {
            continue;
        }

        auto message = Text::Invariant(allocator, StringView(StartupStepMessages[index]));
        if (!message) {
            return Result<StartupProgress>::Failure(message.Error());
        }

        state.stepMessages[index] = Move(message.Value());
    }

    auto pattern = Text::Invariant(allocator, StartupTitlePattern);
    if (!pattern) {
        return Result<StartupProgress>::Failure(pattern.Error());
    }

    const TextArgument arguments[] = {{StartupTitleArgument, desc.appTitle}};
    auto title = Text::Format(allocator, pattern.Value(), Span<const TextArgument>(arguments));
    if (!title) {
        return Result<StartupProgress>::Failure(title.Error());
    }

    ProgressTaskDesc task;
    task.title = &title.Value();
    task.presentation = ProgressPresentation::Hidden;
    auto begun = ProgressHub::Get().Begin(task);
    if (!begun) {
        return Result<StartupProgress>::Failure(begun.Error());
    }

    state.task = Move(begun.Value());
    auto bound = ProgressBinding::Bind(state.task.Id(), ProgressBindingRole::Primary);
    if (!bound) {
        (void)state.task.Finish(ProgressOutcome::Failed);
        return Result<StartupProgress>::Failure(bound.Error());
    }

    state.binding = InitializeAt<ProgressBinding>(state.bindingStorage, Move(bound.Value()));
    state.scope = InitializeAt<ProgressScope>(state.scopeStorage, total);
    return Result<StartupProgress>::Success(Move(progress));
}

void StartupProgress::Enter(StartupStep step) noexcept {
    if (state_ == nullptr || state_->finished) {
        return;
    }

    State& state = *state_;
    const auto index = static_cast<uint32>(step);
    if (index >= StartupStepCount || !state.included[index] || state.entered[index] ||
        state.ready) {
        if (!state.warnedMisuse) {
            state.warnedMisuse = true;
            Log::Warning(LogStartupProgress,
                         "Startup step {Step} was not declared, was entered twice or came after "
                         "MarkReady; it is ignored",
                         StringView(StartupStepName(step)));
        }

        return;
    }

    state.CloseStep();
    state.stepStarted = state.Now();
    state.current = index;
    state.entered[index] = true;
    state.scope->EnterStep(state.weights[index], state.stepMessages[index]);
}

Result<ProgressCounter> StartupProgress::MakeStepCounter(uint64 total) noexcept {
    if (state_ == nullptr || state_->finished || state_->current >= StartupStepCount) {
        return Result<ProgressCounter>::Success(ProgressCounter());
    }

    return state_->scope->MakeCounter(total);
}

Result<SplashStatusSource>
StartupProgress::MakeSplashSource(Allocator& allocator, const CatalogSnapshot* catalog) noexcept {
    auto memory = allocator.Allocate(sizeof(StartupSourceContext), alignof(StartupSourceContext));
    if (!memory) {
        return Result<SplashStatusSource>::Failure(memory.Error());
    }

    auto* context = InitializeAt<StartupSourceContext>(memory.Value(), allocator);
    context->task = TaskId();
    if (catalog != nullptr && catalog->IsValid()) {
        auto copy = catalog->Clone();
        if (!copy) {
            StartupReleaseSource(context);
            return Result<SplashStatusSource>::Failure(copy.Error());
        }

        context->catalog = Move(copy.Value());
    }

    SplashStatusSource source;
    source.poll = StartupPoll;
    source.release = StartupReleaseSource;
    source.context = context;
    return Result<SplashStatusSource>::Success(source);
}

void StartupProgress::Finish(ProgressOutcome outcome) noexcept {
    if (state_ == nullptr || state_->finished) {
        return;
    }

    State& state = *state_;
    state.CloseStep();
    state.current = StartupStepCount;
    state.finished = true;
    state.CloseScopeAndBinding();
    (void)state.task.Finish(outcome);
    // 단계 시간은 끝까지 간 시작만 남긴다(SP5 보정도 실패·중단 실행은 쓰지 않는다 — 8.2절).
    if (outcome != ProgressOutcome::Succeeded) {
        return;
    }

    const StartupStepTimes& times = state.times;
    if (!state.profilePath.IsEmpty()) {
        const detail::StartupProfileEntry blended =
            detail::BlendStartupProfile(state.saved, times, state.entered);
        if (auto written = detail::WriteStartupProfile(state.allocator, state.profilePath.View(),
                                                       state.profileTarget.View(), blended,
                                                       state.profileText.View());
            !written) {
            Log::Warning(LogStartupProgress, "Startup profile {Path} could not be written: {Error}",
                         state.profilePath.View(), ErrorCodeName(written.Error()));
        }
    }

    Log::Info(LogStartupProgress,
              "Startup steps (s): HostServices {HostServices:.3f}, Engine {Engine:.3f}, Assets "
              "{Assets:.3f}, Shaders {Shaders:.3f}, Content {Content:.3f}, FirstFrame "
              "{FirstFrame:.3f}",
              times.seconds[0], times.seconds[1], times.seconds[2], times.seconds[3],
              times.seconds[4], times.seconds[5]);
}

void StartupProgress::MarkReady() noexcept {
    if (state_ == nullptr || state_->finished || state_->ready) {
        return;
    }

    state_->CloseStep();
    state_->ready = true;
}

StartupStepTimes StartupProgress::StepTimes() const noexcept {
    if (state_ == nullptr) {
        return {};
    }

    StartupStepTimes times = state_->times;
    if (!state_->ready && state_->current < StartupStepCount) {
        times.seconds[state_->current] +=
            static_cast<double>(state_->Now() - state_->stepStarted) / StartupNanosecondsPerSecond;
    }

    return times;
}

ProgressTaskId StartupProgress::TaskId() const noexcept {
    return state_ != nullptr ? state_->task.Id() : NoProgressTask;
}

} // namespace rocks::splash
