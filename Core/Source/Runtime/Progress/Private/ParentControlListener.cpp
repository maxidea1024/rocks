#include "ProgressInternal.h"

#include <Platform/Memory.h>
#include <Platform/StandardStream.h>
#include <Platform/Thread.h>
#include <Progress/ParentControlListener.h>
#include <Progress/ProgressHub.h>

namespace rocks {

namespace {

constexpr const char* ControlThreadName = "ParentControl";
// 제어 줄 한 줄의 상한이다(넘는 줄은 모르는 줄로 버린다 — 제어 줄은 짧은 낱말이다).
constexpr usize ControlLineBytes = 256;
constexpr StringView ControlCancelLine = "cancel";

platform::FileTransfer ControlReadStandardInput(void* context, void* buffer, usize capacity,
                                                uint32 timeoutMilliseconds) noexcept {
    (void)context;
    return platform::ReadStandardInput(buffer, capacity, timeoutMilliseconds);
}

} // namespace

// 스레드와 줄 버퍼·멈춤 표지다.
struct ParentControlListener::State {
    platform::Thread thread;
    platform::Atomic<uint32> stopping{0};
    char line[ControlLineBytes];
    usize lineBytes = 0;
    bool overflow = false;
};

ParentControlListener::ParentControlListener() noexcept = default;

ParentControlListener::~ParentControlListener() noexcept {
    Stop();
}

Result<void> ParentControlListener::Start(Allocator& allocator,
                                          const ParentControlListenerDesc& desc) noexcept {
    if (state_ != nullptr) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    desc_ = desc;
    if (desc_.read == nullptr) {
        desc_.read = &ControlReadStandardInput;
    }

    allocator_ = &allocator;
    auto memory = allocator.Allocate(sizeof(State), alignof(State));
    if (!memory) {
        return Result<void>::Failure(memory.Error());
    }

    state_ = InitializeAt<State>(memory.Value());
    platform::ThreadOptions options;
    options.name = ControlThreadName;
    if (state_->thread.Start(&Run, this, options) != platform::ThreadStatus::Success) {
        state_->~State();
        (void)allocator.Deallocate(state_);
        state_ = nullptr;
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    return Result<void>::Success();
}

void ParentControlListener::Stop() noexcept {
    if (state_ == nullptr) {
        return;
    }

    state_->stopping.Store<platform::MemoryOrder::Release>(1u);
    (void)state_->thread.Join();
    state_->~State();
    (void)allocator_->Deallocate(state_);
    state_ = nullptr;
}

void ParentControlListener::Cancel() noexcept {
    ++cancelRequests_;
    ProgressHub::Get().RequestCancelAllTopLevel();
}

void ParentControlListener::HandleLine(StringView line) noexcept {
    // 줄 끝의 CR은 뗀다(Windows 부모가 CRLF로 써도 같은 줄이다).
    if (line.EndsWith("\r")) {
        line = line.SliceBytes(0, line.ByteCount() - 1);
    }

    if (line.IsEmpty()) {
        return;
    }

    if (line == ControlCancelLine) {
        Cancel();
        return;
    }

    if (unknownLines_++ == 0) {
        Log::Warning(detail::LogProgress,
                     "Unknown parent control line ignored (only 'cancel' is understood)");
    }
}

void ParentControlListener::Run(void* context) noexcept {
    auto& self = *static_cast<ParentControlListener*>(context);
    State& state = *self.state_;
    char chunk[ControlLineBytes];
    while (state.stopping.Load<platform::MemoryOrder::Acquire>() == 0) {
        const platform::FileTransfer read =
            self.desc_.read(self.desc_.context, chunk, sizeof(chunk), ChildControlPollMilliseconds);
        if (read.status.code == platform::FileCode::NoProgress) {
            continue;
        }

        if (read.status.code == platform::FileCode::Unsupported) {
            // 표준 입력이 콘솔이다(사람이 손으로 띄움) — 부모가 없으므로 취소하지 않고 수신을 끈다.
            Log::Warning(detail::LogProgress,
                         "Parent control needs a pipe on standard input; it is off for this run");
            return;
        }

        if (read.status.code == platform::FileCode::EndOfFile || !read.status) {
            // 부모가 사라졌다(또는 입력을 더 읽을 수 없다) — 취소하고 읽기를 멈춘다.
            self.Cancel();
            return;
        }

        for (usize index = 0; index < read.bytes; ++index) {
            const char c = chunk[index];
            if (c == '\n') {
                if (!state.overflow) {
                    self.HandleLine(StringView(state.line, state.lineBytes));
                } else {
                    self.HandleLine("(overflow)");
                }

                state.lineBytes = 0;
                state.overflow = false;
                continue;
            }

            if (state.lineBytes == sizeof(state.line)) {
                state.overflow = true;
                continue;
            }

            state.line[state.lineBytes++] = c;
        }
    }
}

} // namespace rocks
