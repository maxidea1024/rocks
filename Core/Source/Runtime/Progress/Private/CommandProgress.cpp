#include <Core/Commandlet/Commandlet.h>
#include <Core/Move.h>
#include <Platform/Clock.h>
#include <Platform/Sync.h>
#include <Progress/CommandProgress.h>
#include <Progress/ProgressHub.h>
#include <Progress/ProgressPumpThread.h>

// 명령의 진행 연결이다(Docs/Design/ProgressReporting 9.5절). 규칙은 공개 헤더 주석을 따른다.
namespace rocks {

static_assert(ProgressCancelledExitCode == CommandletCancelledExitCode,
              "the progress cancel exit code must match the commandlet registry");

namespace {

constexpr StringView ProgressOptionPrefix = "--progress=";
constexpr StringView ConsoleModeName = "console";
constexpr StringView JsonlModeName = "jsonl";
constexpr StringView NoProgressOption = "--no-progress";
constexpr StringView ParentControlOption = "--parent-control";

} // namespace

CommandProgressArgument ParseCommandProgressArgument(StringView argument,
                                                     CommandProgressOptions& options) noexcept {
    if (argument == ParentControlOption) {
        options.parentControl = true;
        return CommandProgressArgument::Consumed;
    }

    CommandProgressMode mode = CommandProgressMode::Console;
    if (argument == NoProgressOption) {
        mode = CommandProgressMode::Off;
    } else if (argument.StartsWith(ProgressOptionPrefix, StringComparison::CaseSensitive)) {
        const StringView value =
            argument.SliceBytes(ProgressOptionPrefix.ByteCount(),
                                argument.ByteCount() - ProgressOptionPrefix.ByteCount());
        if (value == ConsoleModeName) {
            mode = CommandProgressMode::Console;
        } else if (value == JsonlModeName) {
            mode = CommandProgressMode::Jsonl;
        } else {
            return CommandProgressArgument::Invalid;
        }
    } else {
        return CommandProgressArgument::NotProgress;
    }

    if (options.modeGiven) {
        return CommandProgressArgument::Invalid;
    }

    options.mode = mode;
    options.modeGiven = true;
    return CommandProgressArgument::Consumed;
}

// 보고기·스레드다(따로 할당 — 보고기와 스레드는 이동할 수 없다).
struct CommandProgress::State {
    State(Allocator& allocator, const CommandProgressOptions& options,
          const CommandProgressDesc& desc) noexcept
        : mode(options.mode), console(allocator, ConsoleDesc(desc)),
          jsonl(allocator, JsonlDesc(desc)) {
    }

    static ConsoleProgressReporterDesc ConsoleDesc(const CommandProgressDesc& desc) noexcept {
        ConsoleProgressReporterDesc console;
        console.write = desc.consoleWrite;
        console.context = desc.consoleContext;
        return console;
    }

    static ProgressJsonlWriterDesc JsonlDesc(const CommandProgressDesc& desc) noexcept {
        ProgressJsonlWriterDesc jsonl;
        jsonl.write = desc.jsonlWrite;
        jsonl.context = desc.jsonlContext;
        return jsonl;
    }

    CommandProgressMode mode;
    // 보고기는 펌프 스레드와 Flush(명령 스레드)가 함께 쓴다 — 한 번에 하나만.
    platform::Mutex gate;
    ConsoleProgressReporter console;
    ProgressJsonlWriter jsonl;
    ProgressPumpThread pump;
    ParentControlListener control;
    bool pumping = false;
    bool controlling = false;
    bool ownsHub = false;
};

CommandProgress::CommandProgress() noexcept = default;

CommandProgress::~CommandProgress() noexcept {
    Stop();
}

void CommandProgress::Pump(void* context, uint64 nowNanoseconds) noexcept {
    State& state = *static_cast<State*>(context);
    platform::LockGuard lock(state.gate);
    switch (state.mode) {
    case CommandProgressMode::Console:
        (void)state.console.Pump(nowNanoseconds);
        return;
    case CommandProgressMode::Jsonl:
        (void)state.jsonl.Pump(nowNanoseconds);
        return;
    case CommandProgressMode::Off:
        return;
    }
}

Result<void> CommandProgress::Start(Allocator& allocator, const CommandProgressOptions& options,
                                    const CommandProgressDesc& desc) noexcept {
    if (state_ != nullptr) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    auto memory = allocator.Allocate(sizeof(State), alignof(State));
    if (!memory) {
        return Result<void>::Failure(memory.Error());
    }

    allocator_ = &allocator;
    state_ = ConstructAt<State>(memory.Value(), allocator, options, desc);
    // hub가 꺼져 있으면 켠다. 이미 켜져 있으면(프로세스 안 시험·다른 호스트) 그 hub를 쓴다.
    if (auto initialized = ProgressHub::Get().Initialize(allocator, {}); initialized) {
        state_->ownsHub = true;
    } else if (initialized.Error() != ErrorCode::AlreadyExists) {
        Stop();
        return initialized;
    }

    // 첫 Pump는 사건 cursor를 지금으로 맞춘다(cursor 0 = "지금부터"). 명령이 작업을 열기 전에 이
    // 스레드에서 해 두어야 펌프 스레드의 첫 Tick 전에 시작·끝난 작업의 사건도 줄로 나간다.
    Pump(state_, platform::MonotonicNanoseconds());
    // Off는 종료 요청을 볼 때만 펌프 스레드가 필요하다(펌프는 아무 일도 하지 않는다).
    if (options.mode != CommandProgressMode::Off || desc.terminationRequested != nullptr) {
        ProgressPumpThreadDesc pump;
        pump.pump = &Pump;
        pump.context = state_;
        pump.terminationRequested = desc.terminationRequested;
        pump.terminationContext = desc.terminationContext;
        pump.intervalMilliseconds = ChildControlPollMilliseconds;
        if (auto started = state_->pump.Start(allocator, pump); !started) {
            Stop();
            return started;
        }

        state_->pumping = true;
    }

    if (options.parentControl) {
        ParentControlListenerDesc control;
        control.read = desc.controlRead;
        control.context = desc.controlContext;
        if (auto started = state_->control.Start(allocator, control); !started) {
            Stop();
            return started;
        }

        state_->controlling = true;
    }

    return Result<void>::Success();
}

void CommandProgress::Stop() noexcept {
    if (state_ == nullptr) {
        return;
    }

    // 수신기를 먼저 멈춘다(그 뒤 취소 요청이 끝 줄 뒤에 오지 않게). 펌프 스레드는 마지막 Tick에서
    // 끝 사건 줄을 쓴다. 남은 progress 줄은 간격과 상관없이 Flush한다(스레드를 회수한 뒤라 이
    // 스레드가 writer를 쓴다).
    if (state_->controlling) {
        state_->control.Stop();
    }

    if (state_->pumping) {
        state_->pump.Stop();
    }

    Flush();

    if (state_->ownsHub) {
        ProgressHub::Get().Shutdown();
    }

    state_->~State();
    (void)allocator_->Deallocate(state_);
    state_ = nullptr;
    allocator_ = nullptr;
}

void CommandProgress::Flush() noexcept {
    if (state_ == nullptr || state_->mode != CommandProgressMode::Jsonl) {
        return;
    }

    platform::LockGuard lock(state_->gate);
    (void)state_->jsonl.Flush(platform::MonotonicNanoseconds());
}

bool CommandProgress::OwnsHub() const noexcept {
    return state_ != nullptr && state_->ownsHub;
}

} // namespace rocks
