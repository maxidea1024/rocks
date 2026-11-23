#include <ShaderCompiler/ShaderWorkerChannel.h>

#include <Base/PlacementNew.h>
#include <Platform/File.h>
#include <Platform/Path.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>
#include <stdio.h>
#include <string.h>

// 워커 채널(Docs/Design/ShaderSystem.ko.md 11.1절 SC4·SC7)이다. 규칙은 공개 헤더 주석을 따른다.
namespace rocks::shader {

namespace worker_channel_detail {

// 경로 버퍼 크기다(작업 디렉터리 + 파일 이름).
constexpr usize MaxChannelPathBytes = 4096;
// 파일 이름·제어 줄 버퍼다.
constexpr usize ChannelNameBytes = 48;
// 출력 pipe 읽기 단위다.
constexpr usize ReadChunkBytes = 512;
// 워커 인자 수의 상한이다.
constexpr usize MaxWorkerArguments = 10;
// 10진 숫자 버퍼다.
constexpr usize NumberBytes = 16;

constexpr StringView DonePrefix("done ");
constexpr StringView HeldPrefix("held ");
constexpr StringView ChildPrefix("child ");

bool ParseNumber(StringView text, uint32& value) noexcept {
    if (text.IsEmpty() || text.ByteCount() >= NumberBytes) {
        return false;
    }

    uint64 parsed = 0;
    for (const char digit : text) {
        if (digit < '0' || digit > '9') {
            return false;
        }

        parsed = parsed * 10 + static_cast<uint64>(digit - '0');
    }

    if (parsed > NumericLimits<uint32>::Max()) {
        return false;
    }

    value = static_cast<uint32>(parsed);
    return true;
}

} // namespace worker_channel_detail

// 맡긴 묶음 하나다(Run을 부른 스레드의 스택에 있다).
struct ChannelPending {
    Span<const uint8> request;
    ShaderWorkerBatchResult* out = nullptr;
    platform::ManualResetEvent done;
};

struct ShaderWorkerChannel::State {
    explicit State(Allocator& allocator) noexcept
        : allocator(allocator), executable(allocator), directory(allocator), queue(allocator),
          line(allocator) {
    }

    Allocator& allocator;
    String executable;
    String directory;
    uint32 worker = 0;
    bool keepInput = false;
    bool testHooks = false;
    ShaderWorkerChannelHooks hooks;
    platform::Thread thread;
    platform::Mutex mutex;
    platform::ConditionVariable changed;
    Array<ChannelPending*> queue;
    bool stopping = false;
    bool running = false;
    // 아래는 채널 스레드만 쓴다.
    platform::Process process;
    uint32 sequence = 0;
    Array<char> line;

    static void Entry(void* context) noexcept {
        static_cast<State*>(context)->Loop();
    }

    void Loop() noexcept {
        while (true) {
            ChannelPending* pending = nullptr;
            {
                platform::LockGuard lock(mutex);
                while (queue.IsEmpty() && !stopping) {
                    (void)changed.Wait(mutex);
                }

                if (queue.IsEmpty()) {
                    break;
                }

                pending = queue[0];
                (void)queue.TryRemoveAt(0);
            }

            Batch(*pending->out, pending->request);
            pending->done.Signal();
        }

        // 입력 pipe를 닫으면 워커는 EOF로 끝난다(11절 6·7항).
        if (process.IsStarted()) {
            (void)process.CloseInput();
            (void)process.Wait();
            process.Close();
        }
    }

    bool PathOf(const char* name,
                char (&out)[worker_channel_detail::MaxChannelPathBytes]) const noexcept {
        const int written =
            snprintf(out, sizeof(out), "%.*s/%s", static_cast<int>(directory.ByteCount()),
                     directory.Data(), name);
        return written > 0 && static_cast<usize>(written) < sizeof(out);
    }

    static void Fail(ShaderWorkerBatchResult& out, ShaderWorkerBatchOutcome outcome,
                     StringView message) noexcept {
        out.outcome = outcome;
        out.diagnostics.Clear();
        (void)out.diagnostics.TryAppend(message);
    }

    bool StartWorker(ShaderWorkerBatchResult& out) noexcept {
        using namespace worker_channel_detail;
        if (!platform::MakeDirectoryTree(platform::PhysicalFileSystem(), directory.Data(),
                                         directory.ByteCount())) {
            Fail(out, ShaderWorkerBatchOutcome::ChannelFailed,
                 "cannot create the shader worker directory");
            return false;
        }

        char parent[NumberBytes];
        char index[NumberBytes];
        const int parentBytes =
            snprintf(parent, sizeof(parent), "%u", platform::CurrentProcessId());
        const int indexBytes = snprintf(index, sizeof(index), "%u", worker);
        platform::ProcessArgument arguments[MaxWorkerArguments];
        usize count = 0;
        arguments[count++] = {"--work-dir", 10};
        arguments[count++] = {directory.Data(), directory.ByteCount()};
        arguments[count++] = {"--parent-pid", 12};
        arguments[count++] = {parent, static_cast<usize>(parentBytes)};
        arguments[count++] = {"--worker", 8};
        arguments[count++] = {index, static_cast<usize>(indexBytes)};
        if (keepInput) {
            arguments[count++] = {"--keep-input", 12};
        }

        if (testHooks) {
            arguments[count++] = {"--test-hooks", 12};
        }

        platform::ProcessStartInfo info;
        info.executable = executable.Data();
        info.executableBytes = executable.ByteCount();
        info.arguments = arguments;
        info.argumentCount = count;
        info.workingDirectory = directory.Data();
        info.workingDirectoryBytes = directory.ByteCount();
        info.input = platform::ProcessStreamMode::Pipe;
        info.output = platform::ProcessStreamMode::Pipe;
        info.error = platform::ProcessStreamMode::Null;
        info.priority = platform::ProcessPriority::Low;
        if (!process.Start(info)) {
            Fail(out, ShaderWorkerBatchOutcome::ChannelFailed, "cannot start ShaderCompileWorker");
            return false;
        }

        line.Clear();
        return true;
    }

    // 워커가 끝났다(EOF·강제 종료). 종료를 회수해 보고하고 다음 묶음에서 다시 띄운다.
    void WorkerEnded(ShaderWorkerBatchResult& out, const char* input) noexcept {
        (void)process.Wait();
        platform::ProcessExit exit;
        (void)process.GetExit(exit);
        process.Close();
        out.outcome = ShaderWorkerBatchOutcome::WorkerExited;
        out.exit = exit;
        out.diagnostics.Clear();
        (void)out.diagnostics.TryAppend(
            exit.kind == platform::ProcessExitKind::Exited
                ? StringView(ShaderWorkerExitDiagnostic(static_cast<int32>(exit.code)))
                : StringView("ShaderCompileWorker crashed or was terminated"));
        // 워커가 지우기 전에 끝났으면 입력이 남는다(다음 seq와 겹치지 않지만 정리한다).
        bool exists = false;
        if (platform::PhysicalFileSystem().FileExists(input, strlen(input), exists) && exists) {
            (void)platform::PhysicalFileSystem().Delete(input, strlen(input));
        }
    }

    // 출력 pipe에서 줄 하나를 읽는다(끝 \r 제거). EOF·오류면 false다.
    bool ReadLine(StringView& text) noexcept {
        using namespace worker_channel_detail;
        while (true) {
            for (usize index = 0; index < line.Size(); ++index) {
                if (line[index] != '\n') {
                    continue;
                }

                usize end = index;
                while (end > 0 && line[end - 1] == '\r') {
                    --end;
                }

                lineText.Clear();
                if (!lineText.TryAppend(Span<const char>(line.Data(), end)) ||
                    !line.TryRemoveAt(0, index + 1)) {
                    return false;
                }

                text = StringView(lineText.Data(), lineText.Size());
                return true;
            }

            char chunk[ReadChunkBytes];
            const platform::FileTransfer read =
                process.Read(platform::ProcessStream::Output, chunk, sizeof(chunk));
            if (!read.status || read.bytes == 0 ||
                !line.TryAppend(Span<const char>(chunk, read.bytes))) {
                return false;
            }
        }
    }

    bool WriteControl(const char* text) noexcept {
        const usize bytes = strlen(text);
        const platform::FileTransfer written = process.WriteInput(text, bytes);
        return static_cast<bool>(written.status) && written.bytes == bytes;
    }

    void Batch(ShaderWorkerBatchResult& out, Span<const uint8> request) noexcept {
        using namespace worker_channel_detail;
        out.outcome = ShaderWorkerBatchOutcome::ChannelFailed;
        out.diagnostics.Clear();
        out.children.Clear();
        out.exit = platform::ProcessExit{};
        if (!process.IsStarted() && !StartWorker(out)) {
            return;
        }

        out.workerProcessId = process.Id();
        const uint32 current = ++sequence;
        char name[ChannelNameBytes];
        char temporary[MaxChannelPathBytes];
        char input[MaxChannelPathBytes];
        char output[MaxChannelPathBytes];
        snprintf(name, sizeof(name), "input.%u.tmp", current);
        const bool pathTemporary = PathOf(name, temporary);
        snprintf(name, sizeof(name), "input.%u", current);
        const bool pathInput = PathOf(name, input);
        snprintf(name, sizeof(name), "output.%u", current);
        if (!pathTemporary || !pathInput || !PathOf(name, output)) {
            Fail(out, ShaderWorkerBatchOutcome::ChannelFailed, "shader worker path is too long");
            return;
        }

        // 입력을 임시 이름에 다 쓰고 no-replace 이동으로 놓는다.
        {
            platform::File file;
            if (!file.Open(temporary, platform::FileOpenMode::CreateAlways)) {
                Fail(out, ShaderWorkerBatchOutcome::ChannelFailed, "cannot write the worker input");
                return;
            }

            const platform::FileTransfer written = file.WriteAll(request.Data(), request.Size());
            if (!file.Close() || !written.status ||
                !platform::PhysicalFileSystem().Move(temporary, strlen(temporary), input,
                                                     strlen(input), false)) {
                Fail(out, ShaderWorkerBatchOutcome::ChannelFailed, "cannot place the worker input");
                return;
            }
        }

        char control[ChannelNameBytes];
        snprintf(control, sizeof(control), "%u\n", current);
        if (!WriteControl(control)) {
            WorkerEnded(out, input);
            return;
        }

        while (true) {
            StringView text;
            if (!ReadLine(text)) {
                WorkerEnded(out, input);
                return;
            }

            uint32 value = 0;
            if (text.StartsWith(DonePrefix, StringComparison::CaseSensitive) &&
                ParseNumber(text.SliceBytes(DonePrefix.ByteCount(),
                                            text.ByteCount() - DonePrefix.ByteCount()),
                            value) &&
                value == current) {
                ReadOutput(out, output);
                return;
            }

            if (text.StartsWith(ChildPrefix, StringComparison::CaseSensitive) &&
                ParseNumber(text.SliceBytes(ChildPrefix.ByteCount(),
                                            text.ByteCount() - ChildPrefix.ByteCount()),
                            value)) {
                (void)out.children.TryPushBack(value);
                continue;
            }

            if (text.StartsWith(HeldPrefix, StringComparison::CaseSensitive) &&
                ParseNumber(text.SliceBytes(HeldPrefix.ByteCount(),
                                            text.ByteCount() - HeldPrefix.ByteCount()),
                            value) &&
                value == current) {
                const ShaderWorkerHeldAction action =
                    hooks.held != nullptr
                        ? hooks.held(hooks.context, current, process.Id(), out.children.View())
                        : ShaderWorkerHeldAction::Release;
                if (action == ShaderWorkerHeldAction::Terminate) {
                    (void)process.Terminate();
                    WorkerEnded(out, input);
                    return;
                }

                if (!WriteControl("release\n")) {
                    WorkerEnded(out, input);
                    return;
                }

                continue;
            }

            // 모르는 줄은 워커와 채널의 버전이 다르다는 뜻이다.
            (void)process.Terminate();
            WorkerEnded(out, input);
            Fail(out, ShaderWorkerBatchOutcome::ChannelFailed,
                 "ShaderCompileWorker wrote an unknown control line; rebuild ShaderCompileWorker");
            return;
        }
    }

    void ReadOutput(ShaderWorkerBatchResult& out, const char* path) noexcept {
        Array<uint8> bytes(allocator);
        platform::File file;
        uint64 size = 0;
        bool read = static_cast<bool>(file.Open(path, platform::FileOpenMode::ReadExisting)) &&
                    static_cast<bool>(file.Size(size)) && size <= MaxShaderWorkerFileBytes &&
                    static_cast<bool>(bytes.TryResize(static_cast<usize>(size)));
        if (read) {
            const platform::FileTransfer transfer = file.ReadExact(bytes.Data(), bytes.Size());
            read = static_cast<bool>(transfer.status);
        }

        (void)file.Close();
        (void)platform::PhysicalFileSystem().Delete(path, strlen(path));
        if (!read || !DecodeShaderWorkerResponse(bytes.View(), out.response)) {
            Fail(out, ShaderWorkerBatchOutcome::ChannelFailed,
                 "cannot read the ShaderCompileWorker output");
            return;
        }

        out.outcome = ShaderWorkerBatchOutcome::Completed;
    }

    Array<char> lineText{allocator};
};

const char* ShaderWorkerBatchOutcomeName(ShaderWorkerBatchOutcome outcome) noexcept {
    switch (outcome) {
    case ShaderWorkerBatchOutcome::Completed:
        return "Completed";
    case ShaderWorkerBatchOutcome::WorkerExited:
        return "WorkerExited";
    case ShaderWorkerBatchOutcome::ChannelFailed:
        return "ChannelFailed";
    case ShaderWorkerBatchOutcome::ShutDown:
        return "ShutDown";
    }

    return "unknown";
}

ShaderWorkerChannel::ShaderWorkerChannel(Allocator& allocator) noexcept : allocator_(allocator) {
}

ShaderWorkerChannel::~ShaderWorkerChannel() noexcept {
    Shutdown();
}

Result<void> ShaderWorkerChannel::Start(const ShaderWorkerLaunch& launch,
                                        const ShaderWorkerChannelHooks& hooks) noexcept {
    if (state_ != nullptr) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    if (launch.executable.IsEmpty() || launch.workDirectory.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto memory = allocator_.Allocate(sizeof(State), alignof(State));
    if (!memory) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    State* state = ::new (memory.Value()) State(allocator_);
    state->worker = launch.worker;
    state->keepInput = launch.keepInput;
    state->testHooks = launch.testHooks;
    state->hooks = hooks;
    if (!state->executable.TryAssign(launch.executable) ||
        !state->directory.TryAssign(launch.workDirectory)) {
        state->~State();
        (void)allocator_.Deallocate(state);
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    platform::ThreadOptions options;
    options.name = "ShaderWorkerChannel";
    if (state->thread.Start(&State::Entry, state, options) != platform::ThreadStatus::Success) {
        state->~State();
        (void)allocator_.Deallocate(state);
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    state->running = true;
    state_ = state;
    return Result<void>::Success();
}

void ShaderWorkerChannel::Run(Span<const uint8> request, ShaderWorkerBatchResult& out) noexcept {
    out.outcome = ShaderWorkerBatchOutcome::ShutDown;
    if (state_ == nullptr) {
        return;
    }

    ChannelPending pending;
    pending.request = request;
    pending.out = &out;
    {
        platform::LockGuard lock(state_->mutex);
        if (state_->stopping || !state_->queue.TryPushBack(&pending)) {
            return;
        }

        state_->changed.NotifyAll();
    }

    (void)pending.done.Wait();
}

void ShaderWorkerChannel::Shutdown() noexcept {
    if (state_ == nullptr) {
        return;
    }

    {
        platform::LockGuard lock(state_->mutex);
        state_->stopping = true;
        state_->changed.NotifyAll();
    }

    (void)state_->thread.Join();
    state_->~State();
    (void)allocator_.Deallocate(state_);
    state_ = nullptr;
}

} // namespace rocks::shader
