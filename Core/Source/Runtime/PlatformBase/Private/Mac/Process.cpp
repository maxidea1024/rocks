#include "../Common/ProcessInternal.h"
#include "../Posix/PosixFileError.h"

#include <Platform/Clock.h>
#include <Platform/Memory.h>

#include <crt_externs.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <new>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/event.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace rocks::platform {

namespace {

constexpr usize ReadChunkBytes = 16u * 1024u;

// Low 우선순위가 올리는 nice 값이다(UE ShaderCompileWorker의 PriorityModifier -1을 Unix
// CreateProc이 바꾸는 값과 같다).
constexpr int LowPriorityNice = 5;
constexpr int MaxNice = 19;

// Close가 강제 종료한 자식의 끝남을 기다리는 상한이자, RunProcess가 자식이 끝난 뒤 남은 출력을
// 모으는 상한이다. 끝나지 않으면(커널 대기에 묶인 경우) 회수하지 못하고 handle만 놓는다.
constexpr uint32 CloseWaitMilliseconds = 10'000;

// RunProcess가 pipe를 기다리는 동안 직접 자식의 종료를 확인하는 간격이다. POSIX poll은 프로세스
// 종료를 함께 기다릴 수 없다.
constexpr int ExitCheckMilliseconds = 50;

FileStatus Errno() noexcept {
    return detail::PosixFileError(errno);
}

// posix_spawn 오류 번호를 상태로 바꾼다. 인자·환경이 너무 크면(E2BIG) Overflow다.
FileStatus SpawnError(int error) noexcept {
    if (error == E2BIG) {
        return {FileCode::Overflow, static_cast<uint32>(error)};
    }

    return detail::PosixFileError(error);
}

void CloseFd(int& fd) noexcept {
    if (fd >= 0) {
        close(fd);
    }

    fd = -1;
}

// 표준 fd 번호(0–2)를 차지한 pipe fd는 3 이상으로 옮긴다. 자식에서 dup2(fd, fd)는 close-on-exec를
// 지우지 않아 POSIX_SPAWN_CLOEXEC_DEFAULT에 닫히기 때문이다(부모의 표준 fd가 닫혀 있을 때만
// 생긴다).
bool MoveAboveStandard(int& fd) noexcept {
    if (fd > STDERR_FILENO) {
        return true;
    }

    const int moved = fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (moved < 0) {
        return false;
    }

    close(fd);
    fd = moved;
    return true;
}

// 소유하는 NUL 종료 문자열 묶음이다(argv·경로).
class CStringBlock {
public:
    CStringBlock() noexcept = default;

    ~CStringBlock() noexcept {
        AlignedFree(data_);
    }

    CStringBlock(const CStringBlock&) = delete;
    CStringBlock& operator=(const CStringBlock&) = delete;

    bool Allocate(usize bytes) noexcept {
        data_ = static_cast<char*>(AlignedAllocate(bytes, alignof(char*)));
        return data_ != nullptr;
    }

    char* Data() const noexcept {
        return data_;
    }

private:
    char* data_ = nullptr;
};

struct PosixPipe {
    int fd = -1;
    bool endOfStream = false;

    bool IsOpen() const noexcept {
        return fd >= 0;
    }

    void Close() noexcept {
        CloseFd(fd);
    }
};

int PollTimeout(uint32 milliseconds) noexcept {
    if (milliseconds == InfiniteTimeout) {
        return -1;
    }

    return milliseconds > static_cast<uint32>(INT_MAX) ? INT_MAX : static_cast<int>(milliseconds);
}

uint64 DeadlineAfter(uint32 milliseconds) noexcept {
    return MonotonicNanoseconds() + static_cast<uint64>(milliseconds) * 1'000'000u;
}

uint32 RemainingMilliseconds(uint64 deadline) noexcept {
    const uint64 now = MonotonicNanoseconds();
    if (now >= deadline) {
        return 0;
    }

    const uint64 remaining = (deadline - now + 999'999u) / 1'000'000u;
    return remaining >= InfiniteTimeout ? InfiniteTimeout - 1u : static_cast<uint32>(remaining);
}

// 종료 관측 결과다. Failed는 OS 오류(waitid·kqueue)다.
enum class ExitObservation : uint8 {
    Exited,
    Running,
    Failed
};

} // namespace

struct Process::State {
    pid_t pid = -1;
    // 종료를 관측했다(WNOWAIT로 회수하지 않았다). 회수 전에는 PID·그룹 번호가 재사용되지 않는다.
    bool exited = false;
    // 다른 코드가 이미 회수했다(ECHILD). PID·그룹 번호가 재사용됐을 수 있으므로 더는 신호를
    // 보내거나 회수하지 않는다.
    bool reapedElsewhere = false;
    bool terminateRequested = false;
    ProcessExit exit{};
    PosixPipe input;
    PosixPipe output;
    PosixPipe error;
};

namespace {

void RecordExit(Process::State& state, const siginfo_t& info) noexcept {
    state.exit.kind =
        info.si_code == CLD_EXITED ? ProcessExitKind::Exited : ProcessExitKind::Signaled;
    state.exit.code = static_cast<uint32>(info.si_status);
    if (state.terminateRequested) {
        state.exit.kind = ProcessExitKind::Terminated;
    }

    state.exited = true;
}

// 자식의 종료를 회수하지 않고 관측한다. block이면 끝날 때까지 기다린다. 다른 코드가 이미
// 회수했으면(ECHILD) 결과를 알 수 없으므로 code 0xFFFFFFFF인 종료로 기록하고 이후 신호를 막는다.
ExitObservation ObserveExit(Process::State& state, bool block) noexcept {
    if (state.exited) {
        return ExitObservation::Exited;
    }

    for (;;) {
        // WNOHANG에 바뀐 자식이 없으면 커널이 siginfo를 쓰지 않으므로 매번 0으로 둔다.
        siginfo_t info{};
        const int options = WEXITED | WNOWAIT | (block ? 0 : WNOHANG);
        if (waitid(P_PID, static_cast<id_t>(state.pid), &info, options) == 0) {
            if (info.si_pid == 0) {
                return ExitObservation::Running;
            }

            RecordExit(state, info);
            return ExitObservation::Exited;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == ECHILD) {
            state.exit.kind =
                state.terminateRequested ? ProcessExitKind::Terminated : ProcessExitKind::Exited;
            state.exit.code = 0xFFFFFFFFu;
            state.exited = true;
            state.reapedElsewhere = true;
            return ExitObservation::Exited;
        }

        return ExitObservation::Failed;
    }
}

// kqueue로 자식 종료를 최대 milliseconds 기다린다. XNU는 NOTE_EXIT를 보낸 뒤에 프로세스를 zombie로
// 바꾸므로, 종료 통지(또는 이미 종료 중이라 등록이 ESRCH로 실패한 경우)를 받은 뒤의 관측은 차단
// 대기로 확정한다(곧 끝나는 대기다). 기다리지 않는 관측으로 확인하면 "아직"이 나올 수 있다.
ExitObservation WaitExitFor(Process::State& state, uint32 milliseconds) noexcept {
    const ExitObservation first = ObserveExit(state, false);
    if (first != ExitObservation::Running) {
        return first;
    }

    const int queue = kqueue();
    if (queue < 0) {
        return ExitObservation::Failed;
    }

    struct kevent change{};
    EV_SET(&change, static_cast<uintptr_t>(state.pid), EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT,
           0, nullptr);
    const uint64 deadline = DeadlineAfter(milliseconds);
    bool registered = false;
    ExitObservation result = ExitObservation::Failed;
    for (;;) {
        const uint32 remaining = RemainingMilliseconds(deadline);
        timespec timeout{};
        timeout.tv_sec = static_cast<time_t>(remaining / 1000u);
        timeout.tv_nsec = static_cast<long>(remaining % 1000u) * 1'000'000L;
        struct kevent event{};
        const int count =
            kevent(queue, registered ? nullptr : &change, registered ? 0 : 1, &event, 1, &timeout);
        if (count < 0) {
            if (errno == EINTR) {
                // 등록이 반영되었는지 알 수 없으므로 다시 등록한다(EV_ADD는 중복해도 된다).
                continue;
            }

            result = errno == ESRCH ? ObserveExit(state, true) : ExitObservation::Failed;
            break;
        }

        registered = true;
        if (count == 0) {
            result = ObserveExit(state, false);
            break;
        }

        const bool failed = (event.flags & EV_ERROR) != 0 && event.data != 0 && event.data != ESRCH;
        result = failed ? ExitObservation::Failed : ObserveExit(state, true);
        break;
    }

    close(queue);
    return result;
}

// 남은 자손을 끝낸다. 자식을 아직 회수하지 않았으면 그룹 번호는 이 자식의 그룹이다.
void KillGroup(const Process::State& state) noexcept {
    if (!state.reapedElsewhere) {
        kill(-state.pid, SIGKILL);
    }
}

FileStatus OpenNull(int target, int flags, posix_spawn_file_actions_t& actions) noexcept {
    const int result = posix_spawn_file_actions_addopen(&actions, target, "/dev/null", flags, 0);
    return result == 0 ? FileStatus{} : detail::PosixFileError(result);
}

// 스트림 하나의 자식 쪽 연결을 file action으로 등록한다. Pipe면 parent에 부모 쪽 끝을 둔다.
FileStatus PrepareStream(ProcessStreamMode mode, int target, bool parentReads, PosixPipe& parent,
                         int& child, posix_spawn_file_actions_t& actions) noexcept {
    switch (mode) {
    case ProcessStreamMode::Null:
        return OpenNull(target, parentReads ? O_WRONLY : O_RDONLY, actions);
    case ProcessStreamMode::Inherit: {
        // 부모에 해당 fd가 없으면 Null과 같다.
        if (fcntl(target, F_GETFD) == -1) {
            return OpenNull(target, parentReads ? O_WRONLY : O_RDONLY, actions);
        }

        const int result = posix_spawn_file_actions_addinherit_np(&actions, target);
        return result == 0 ? FileStatus{} : detail::PosixFileError(result);
    }
    case ProcessStreamMode::Pipe: {
        int fds[2] = {-1, -1};
        if (pipe(fds) != 0) {
            return Errno();
        }

        int& parentEnd = parentReads ? fds[0] : fds[1];
        int& childEnd = parentReads ? fds[1] : fds[0];
        // 양쪽 모두 close-on-exec다(자식 쪽은 file action의 dup2가 표준 번호로 복제한다). macOS에는
        // pipe2가 없어 pipe()와 F_SETFD 사이에 다른 스레드가 fork하면 물려받을 수 있다(헤더 수명
        // 계약).
        const bool configured = fcntl(fds[0], F_SETFD, FD_CLOEXEC) == 0 &&
                                fcntl(fds[1], F_SETFD, FD_CLOEXEC) == 0 &&
                                MoveAboveStandard(parentEnd) && MoveAboveStandard(childEnd) &&
                                fcntl(parentEnd, F_SETFL, O_NONBLOCK) == 0 &&
                                // 자식이 입력을 닫은 뒤 쓰면 SIGPIPE 대신 EPIPE를 받는다.
                                (parentReads || fcntl(parentEnd, F_SETNOSIGPIPE, 1) == 0);
        if (!configured) {
            const FileStatus status = Errno();
            CloseFd(fds[0]);
            CloseFd(fds[1]);
            return status;
        }

        const int result = posix_spawn_file_actions_adddup2(&actions, childEnd, target);
        if (result != 0) {
            CloseFd(fds[0]);
            CloseFd(fds[1]);
            return detail::PosixFileError(result);
        }

        parent.fd = parentEnd;
        child = childEnd;
        return {};
    }
    }

    return {FileCode::InvalidArgument, 0};
}

// posix_spawn에 넘길 실행 파일 경로다. 상대 경로는 부모의 현재 디렉터리에 붙인다(자식의 chdir
// 뒤에 해석되지 않게). argv[0]은 이것과 별개로 받은 문자열 그대로다.
FileStatus BuildExecutablePath(const ProcessStartInfo& info, CStringBlock& path) noexcept {
    if (info.executable[0] == '/') {
        if (!path.Allocate(info.executableBytes + 1u)) {
            return {FileCode::OutOfMemory, 0};
        }

        MemoryCopy(path.Data(), info.executable, info.executableBytes);
        path.Data()[info.executableBytes] = '\0';
        return {};
    }

    char* current = getcwd(nullptr, 0);
    if (current == nullptr) {
        return Errno();
    }

    usize currentBytes = 0;
    while (current[currentBytes] != '\0') {
        ++currentBytes;
    }

    const bool allocated = path.Allocate(currentBytes + 1u + info.executableBytes + 1u);
    if (allocated) {
        MemoryCopy(path.Data(), current, currentBytes);
        path.Data()[currentBytes] = '/';
        MemoryCopy(path.Data() + currentBytes + 1u, info.executable, info.executableBytes);
        path.Data()[currentBytes + 1u + info.executableBytes] = '\0';
    }

    free(current);
    return allocated ? FileStatus{} : FileStatus{FileCode::OutOfMemory, 0};
}

// argv 배열과 문자열을 한 블록에 만든다: [char* argv[count + 2]][executable\0][arg\0]...
FileStatus BuildArguments(const ProcessStartInfo& info, CStringBlock& block,
                          char**& argv) noexcept {
    const usize pointerCount = info.argumentCount + 2u;
    if (pointerCount > MaxSize / sizeof(char*)) {
        return {FileCode::Overflow, 0};
    }

    usize total = pointerCount * sizeof(char*) + info.executableBytes + 1u;
    for (usize i = 0; i < info.argumentCount; ++i) {
        const usize bytes = info.arguments[i].bytes;
        if (bytes > MaxSize - total - 1u) {
            return {FileCode::Overflow, 0};
        }

        total += bytes + 1u;
    }

    if (!block.Allocate(total)) {
        return {FileCode::OutOfMemory, 0};
    }

    argv = reinterpret_cast<char**>(block.Data());
    char* cursor = block.Data() + pointerCount * sizeof(char*);
    argv[0] = cursor;
    MemoryCopy(cursor, info.executable, info.executableBytes);
    cursor[info.executableBytes] = '\0';
    cursor += info.executableBytes + 1u;
    for (usize i = 0; i < info.argumentCount; ++i) {
        argv[i + 1u] = cursor;
        MemoryCopy(cursor, info.arguments[i].text, info.arguments[i].bytes);
        cursor[info.arguments[i].bytes] = '\0';
        cursor += info.arguments[i].bytes + 1u;
    }

    argv[pointerCount - 1u] = nullptr;
    return {};
}

} // namespace

FileStatus Process::Start(const ProcessStartInfo& info) noexcept {
    if (state_ != nullptr) {
        return {FileCode::AlreadyOpen, 0};
    }

    auto status = detail::ValidateProcessStartInfo(info);
    if (!status) {
        return status;
    }

    CStringBlock executable;
    status = BuildExecutablePath(info, executable);
    if (!status) {
        return status;
    }

    CStringBlock arguments;
    char** argv = nullptr;
    status = BuildArguments(info, arguments, argv);
    if (!status) {
        return status;
    }

    CStringBlock directory;
    if (detail::HasWorkingDirectory(info)) {
        if (!directory.Allocate(info.workingDirectoryBytes + 1u)) {
            return {FileCode::OutOfMemory, 0};
        }

        MemoryCopy(directory.Data(), info.workingDirectory, info.workingDirectoryBytes);
        directory.Data()[info.workingDirectoryBytes] = '\0';
        // posix_spawn은 잘못된 작업 디렉터리를 ENOENT·ENOTDIR로 보고하므로 먼저 확인해 없음
        // (NotFound)과 디렉터리 아님(WrongType)을 구분한다.
        struct stat directoryStatus{};
        if (stat(directory.Data(), &directoryStatus) != 0) {
            return Errno();
        }

        if (!S_ISDIR(directoryStatus.st_mode)) {
            return {FileCode::WrongType, 0};
        }
    }

    void* memory = AlignedAllocate(sizeof(State), alignof(State));
    if (memory == nullptr) {
        return {FileCode::OutOfMemory, 0};
    }

    auto* state = new (memory) State;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    bool actionsReady = false;
    bool attributesReady = false;
    int childInput = -1;
    int childOutput = -1;
    int childError = -1;

    auto cleanup = [&]() noexcept {
        if (actionsReady) {
            posix_spawn_file_actions_destroy(&actions);
        }

        if (attributesReady) {
            posix_spawnattr_destroy(&attributes);
        }

        CloseFd(childInput);
        CloseFd(childOutput);
        CloseFd(childError);
    };

    auto fail = [&](FileStatus failure) noexcept {
        cleanup();
        state->input.Close();
        state->output.Close();
        state->error.Close();
        state->~State();
        AlignedFree(state);
        return failure;
    };

    int result = posix_spawn_file_actions_init(&actions);
    if (result != 0) {
        return fail(detail::PosixFileError(result));
    }

    actionsReady = true;
    result = posix_spawnattr_init(&attributes);
    if (result != 0) {
        return fail(detail::PosixFileError(result));
    }

    attributesReady = true;
    status = PrepareStream(info.input, STDIN_FILENO, false, state->input, childInput, actions);
    if (status) {
        status =
            PrepareStream(info.output, STDOUT_FILENO, true, state->output, childOutput, actions);
    }

    if (status) {
        if (info.mergeErrorIntoOutput) {
            // 표준 출력 연결이 끝난 뒤 fd 1을 2로 복제한다(file action은 등록 순서대로 실행된다).
            result = posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
            status = result == 0 ? FileStatus{} : detail::PosixFileError(result);
        } else {
            status =
                PrepareStream(info.error, STDERR_FILENO, true, state->error, childError, actions);
        }
    }

    if (status && detail::HasWorkingDirectory(info)) {
        result = posix_spawn_file_actions_addchdir_np(&actions, directory.Data());
        status = result == 0 ? FileStatus{} : detail::PosixFileError(result);
    }

    if (!status) {
        return fail(status);
    }

    // 새 프로세스 그룹(자손까지 한 번에 종료), 빈 신호 마스크와 기본 신호 처리(부모가 무시하는
    // 신호를 물려주지 않는다), file action으로 연결한 fd 말고는 모두 닫기(부모의 다른 fd를 물려주지
    // 않는다).
    sigset_t mask;
    sigemptyset(&mask);
    sigset_t defaults;
    sigfillset(&defaults);
    sigdelset(&defaults, SIGKILL);
    sigdelset(&defaults, SIGSTOP);
    const short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF |
                        POSIX_SPAWN_CLOEXEC_DEFAULT;
    result = posix_spawnattr_setflags(&attributes, flags);
    if (result == 0) {
        result = posix_spawnattr_setpgroup(&attributes, 0);
    }

    if (result == 0) {
        result = posix_spawnattr_setsigmask(&attributes, &mask);
    }

    if (result == 0) {
        result = posix_spawnattr_setsigdefault(&attributes, &defaults);
    }

    if (result != 0) {
        return fail(detail::PosixFileError(result));
    }

    pid_t pid = -1;
    result = posix_spawn(&pid, executable.Data(), &actions, &attributes, argv, *_NSGetEnviron());
    if (result != 0) {
        return fail(SpawnError(result));
    }

    cleanup();
    state->pid = pid;
    if (info.priority == ProcessPriority::Low) {
        // posix_spawn은 nice를 정할 수 없어 시작 직후 올린다(UE 5.6 Unix CreateProc도 같은 방식이며
        // 경합을 주석으로 남겼다). 실패해도 실행에는 영향이 없으므로 무시한다.
        errno = 0;
        const int current = getpriority(PRIO_PROCESS, 0);
        if (errno == 0) {
            const int lowered =
                current + LowPriorityNice > MaxNice ? MaxNice : current + LowPriorityNice;
            setpriority(PRIO_PROCESS, static_cast<id_t>(pid), lowered);
        }
    }

    state_ = state;
    return {};
}

uint32 Process::Id() const noexcept {
    return state_ != nullptr ? static_cast<uint32>(state_->pid) : 0;
}

WaitResult Process::Wait(uint32 timeoutMilliseconds) noexcept {
    if (state_ == nullptr) {
        return WaitResult::Failed;
    }

    ExitObservation observed = ObserveExit(*state_, false);
    if (observed == ExitObservation::Running && timeoutMilliseconds != 0) {
        observed = timeoutMilliseconds == InfiniteTimeout
                       ? ObserveExit(*state_, true)
                       : WaitExitFor(*state_, timeoutMilliseconds);
    }

    switch (observed) {
    case ExitObservation::Exited:
        return WaitResult::Signaled;
    case ExitObservation::Running:
        return WaitResult::TimedOut;
    case ExitObservation::Failed:
        return WaitResult::Failed;
    }

    return WaitResult::Failed;
}

bool Process::GetExit(ProcessExit& exit) const noexcept {
    if (state_ == nullptr || !state_->exited) {
        return false;
    }

    exit = state_->exit;
    return true;
}

FileStatus Process::Terminate() noexcept {
    if (state_ == nullptr) {
        return {FileCode::NotOpen, 0};
    }

    if (state_->terminateRequested) {
        return {};
    }

    // 스스로 끝났지만 아직 관측하지 않은 자식은 먼저 기록한다(종료 종류를 바꾸지 않는다).
    (void)ObserveExit(*state_, false);
    if (state_->reapedElsewhere) {
        return {};
    }

    // 자식을 아직 회수하지 않았으므로 그룹 번호는 이 자식의 그룹이다. 자식이 그룹을 바꿨어도 직접
    // 자식은 따로 끝낸다. macOS는 회수되지 않은 zombie만 남은 그룹에 신호를 보내면 ESRCH가 아니라
    // EPERM을 돌려준다. 그래서 직접 자식이 끝난 뒤의 EPERM은 살아 있는 구성원이 없다는 뜻으로 본다.
    const bool groupSignaled =
        kill(-state_->pid, SIGKILL) == 0 || errno == ESRCH || (state_->exited && errno == EPERM);
    const int groupError = errno;
    const bool childSignaled = state_->exited || kill(state_->pid, SIGKILL) == 0 || errno == ESRCH;
    if (!groupSignaled) {
        return detail::PosixFileError(groupError);
    }

    if (!childSignaled) {
        return Errno();
    }

    if (!state_->exited) {
        state_->terminateRequested = true;
    }

    return {};
}

FileTransfer Process::WriteInput(const void* data, usize bytes,
                                 uint32 timeoutMilliseconds) noexcept {
    if (state_ == nullptr || !state_->input.IsOpen()) {
        return {{FileCode::NotOpen, 0}, 0};
    }

    if (bytes != 0 && data == nullptr) {
        return {{FileCode::InvalidArgument, 0}, 0};
    }

    const bool infinite = timeoutMilliseconds == InfiniteTimeout;
    const uint64 deadline = infinite ? 0 : DeadlineAfter(timeoutMilliseconds);
    const auto* source = static_cast<const uint8*>(data);
    usize written = 0;
    while (written < bytes) {
        const ssize_t count = write(state_->input.fd, source + written, bytes - written);
        if (count > 0) {
            written += static_cast<usize>(count);
            continue;
        }

        if (count < 0 && errno == EINTR) {
            continue;
        }

        if (count < 0 && errno == EPIPE) {
            return {{FileCode::EndOfFile, 0}, written};
        }

        if (count < 0 && errno != EAGAIN) {
            return {Errno(), written};
        }

        pollfd entry{state_->input.fd, POLLOUT, 0};
        const int ready =
            poll(&entry, 1, infinite ? -1 : PollTimeout(RemainingMilliseconds(deadline)));
        if (ready < 0 && errno != EINTR) {
            return {Errno(), written};
        }

        if (ready == 0) {
            return {{FileCode::NoProgress, 0}, written};
        }
    }

    return {{}, written};
}

FileStatus Process::CloseInput() noexcept {
    if (state_ == nullptr || !state_->input.IsOpen()) {
        return {FileCode::NotOpen, 0};
    }

    state_->input.Close();
    return {};
}

FileTransfer Process::Read(ProcessStream stream, void* buffer, usize capacity,
                           uint32 timeoutMilliseconds) noexcept {
    if (state_ == nullptr) {
        return {{FileCode::NotOpen, 0}, 0};
    }

    if (stream != ProcessStream::Output && stream != ProcessStream::Error) {
        return {{FileCode::InvalidArgument, 0}, 0};
    }

    PosixPipe& pipe = stream == ProcessStream::Output ? state_->output : state_->error;
    if (!pipe.IsOpen()) {
        return {{FileCode::NotOpen, 0}, 0};
    }

    if (capacity == 0 || buffer == nullptr) {
        return {{FileCode::InvalidArgument, 0}, 0};
    }

    if (pipe.endOfStream) {
        return {{FileCode::EndOfFile, 0}, 0};
    }

    const bool infinite = timeoutMilliseconds == InfiniteTimeout;
    const uint64 deadline = infinite ? 0 : DeadlineAfter(timeoutMilliseconds);
    for (;;) {
        const ssize_t count = read(pipe.fd, buffer, capacity);
        if (count > 0) {
            return {{}, static_cast<usize>(count)};
        }

        if (count == 0) {
            pipe.endOfStream = true;
            return {{FileCode::EndOfFile, 0}, 0};
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno != EAGAIN) {
            return {Errno(), 0};
        }

        pollfd entry{pipe.fd, POLLIN, 0};
        const int ready =
            poll(&entry, 1, infinite ? -1 : PollTimeout(RemainingMilliseconds(deadline)));
        if (ready < 0 && errno != EINTR) {
            return {Errno(), 0};
        }

        if (ready == 0) {
            return {{FileCode::NoProgress, 0}, 0};
        }
    }
}

void Process::Close() noexcept {
    State* state = state_;
    if (state == nullptr) {
        return;
    }

    state_ = nullptr;
    if (!state->exited) {
        state->terminateRequested = true;
        KillGroup(*state);
        kill(state->pid, SIGKILL);
        (void)WaitExitFor(*state, CloseWaitMilliseconds);
    }

    if (state->exited && !state->reapedElsewhere) {
        // 직접 자식은 끝났지만 아직 회수하지 않았으므로 그룹 번호가 유효하다. 종료 신호와 동시에
        // fork된 자손이 있을 수 있어 회수 직전에 한 번 더 그룹을 끝낸다.
        KillGroup(*state);
        int status = 0;
        while (waitpid(state->pid, &status, 0) < 0 && errno == EINTR) {
        }
    }

    state->input.Close();
    state->output.Close();
    state->error.Close();
    state->~State();
    AlignedFree(state);
}

ProcessRunResult RunProcess(const ProcessStartInfo& info,
                            const ProcessRunOptions& options) noexcept {
    ProcessRunResult result;
    result.status = detail::ValidateProcessRunOptions(info, options);
    if (!result.status) {
        return result;
    }

    Process process;
    result.status = process.Start(info);
    if (!result.status) {
        return result;
    }

    Process::State& state = *process.state_;
    detail::RunSink sinks[2] = {{static_cast<uint8*>(options.output), options.outputCapacity},
                                {static_cast<uint8*>(options.error), options.errorCapacity}};
    PosixPipe* readers[2] = {&state.output, &state.error};
    const auto* input = static_cast<const uint8*>(options.input);
    usize inputLeft = options.inputBytes;
    const bool limited = options.timeoutMilliseconds != InfiniteTimeout;
    const uint64 deadline = limited ? DeadlineAfter(options.timeoutMilliseconds) : 0;
    bool timing = limited;
    // 직접 자식이 끝나거나 강제 종료한 뒤 남은 출력을 모으는 상한이다. 그룹 밖 자손이나 다른
    // 스레드의 fork가 물려받은 pipe 끝이 있으면 EOF가 오지 않는다.
    bool draining = false;
    uint64 drainDeadline = 0;
    const auto startDraining = [&]() noexcept {
        if (!draining) {
            draining = true;
            drainDeadline = DeadlineAfter(CloseWaitMilliseconds);
        }
    };

    uint8 chunk[ReadChunkBytes];
    for (;;) {
        if (state.input.IsOpen() && inputLeft == 0) {
            state.input.Close();
        }

        // 직접 자식의 종료와 기한은 매번 확인한다.
        if (!state.exited) {
            const ExitObservation observed = ObserveExit(state, false);
            if (observed == ExitObservation::Failed) {
                result.status = Errno();
                break;
            }

            if (observed == ExitObservation::Exited) {
                // pipe를 잡고 남은 자손을 끝낸다(자식을 아직 회수하지 않아 그룹 번호가 유효하다).
                KillGroup(state);
                startDraining();
            }
        }

        if (timing && RemainingMilliseconds(deadline) == 0) {
            result.timedOut = true;
            timing = false;
            const auto terminated = process.Terminate();
            if (!terminated) {
                result.status = terminated;
                break;
            }

            startDraining();
        }

        if (draining && RemainingMilliseconds(drainDeadline) == 0) {
            // 상한 안에 EOF가 오지 않은 스트림은 모으지 못한 출력이 있을 수 있다.
            for (int index = 0; index < 2; ++index) {
                if (readers[index]->IsOpen() && !readers[index]->endOfStream) {
                    sinks[index].truncated = true;
                }
            }

            break;
        }

        pollfd entries[3];
        int kinds[3];
        nfds_t count = 0;
        for (int index = 0; index < 2; ++index) {
            if (readers[index]->IsOpen() && !readers[index]->endOfStream) {
                entries[count] = {readers[index]->fd, POLLIN, 0};
                kinds[count++] = index;
            }
        }

        if (state.input.IsOpen()) {
            entries[count] = {state.input.fd, POLLOUT, 0};
            kinds[count++] = 2;
        }

        if (count == 0) {
            if (state.exited) {
                break;
            }

            // 모을 pipe가 없다. 자식의 끝남을 남은 기한(시간 초과 전) 또는 남은 수집 상한(강제 종료
            // 뒤)까지 기다린다. 기한·상한 처리는 다음 반복이 한다.
            uint32 wait = InfiniteTimeout;
            if (timing) {
                wait = RemainingMilliseconds(deadline);
            } else if (draining) {
                wait = RemainingMilliseconds(drainDeadline);
            }

            const ExitObservation observed =
                wait == InfiniteTimeout ? ObserveExit(state, true) : WaitExitFor(state, wait);
            if (observed == ExitObservation::Failed) {
                result.status = Errno();
                break;
            }

            continue;
        }

        int timeout = state.exited ? -1 : ExitCheckMilliseconds;
        if (timing || draining) {
            const int remaining =
                PollTimeout(RemainingMilliseconds(timing ? deadline : drainDeadline));
            timeout = timeout < 0 || remaining < timeout ? remaining : timeout;
        }

        const int ready = poll(entries, count, timeout);
        if (ready < 0 && errno != EINTR) {
            result.status = Errno();
            break;
        }

        for (nfds_t i = 0; ready > 0 && i < count; ++i) {
            if (entries[i].revents == 0) {
                continue;
            }

            const int kind = kinds[i];
            if (kind < 2) {
                PosixPipe& pipe = *readers[kind];
                const ssize_t got = read(pipe.fd, chunk, sizeof(chunk));
                if (got > 0) {
                    sinks[kind].Append(chunk, static_cast<usize>(got));
                } else if (got == 0) {
                    pipe.endOfStream = true;
                    pipe.Close();
                } else if (errno != EAGAIN && errno != EINTR) {
                    result.status = Errno();
                }

                continue;
            }

            const ssize_t sent = write(state.input.fd, input, inputLeft);
            if (sent > 0) {
                input += sent;
                inputLeft -= static_cast<usize>(sent);
            } else if (sent < 0 && errno == EPIPE) {
                // 자식이 입력을 닫았다. 남은 입력은 버린다(실패가 아니다).
                inputLeft = 0;
            } else if (sent < 0 && errno != EAGAIN && errno != EINTR) {
                result.status = Errno();
            }
        }

        if (!result.status) {
            break;
        }
    }

    if (result.status && !state.exited) {
        // 남은 수집 상한에서 끝났으면 강제 종료한 자식의 끝남도 상한까지만 기다린다.
        const ExitObservation observed = WaitExitFor(state, CloseWaitMilliseconds);
        if (observed != ExitObservation::Exited) {
            result.status = {FileCode::SystemError, static_cast<uint32>(errno)};
        }
    }

    if (result.status) {
        process.GetExit(result.exit);
    }

    result.outputBytes = sinks[0].bytes;
    result.outputTruncated = sinks[0].truncated;
    result.errorBytes = sinks[1].bytes;
    result.errorTruncated = sinks[1].truncated;
    process.Close();
    return result;
}

uint32 CurrentProcessId() noexcept {
    return static_cast<uint32>(getpid());
}

bool IsProcessRunning(uint32 processId) noexcept {
    if (processId == 0 || processId > static_cast<uint32>(INT_MAX)) {
        return false;
    }

    // 신호 0은 존재와 권한만 확인한다. 권한이 없으면(EPERM) 있는 프로세스다.
    return kill(static_cast<pid_t>(processId), 0) == 0 || errno == EPERM;
}

FileStatus GetExecutablePath(char* buffer, usize capacity, usize& bytes) noexcept {
    if (buffer == nullptr && capacity != 0) {
        return {FileCode::InvalidArgument, 0};
    }

    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    auto* raw = static_cast<char*>(AlignedAllocate(size + 1u, 1));
    if (raw == nullptr) {
        return {FileCode::OutOfMemory, 0};
    }

    if (_NSGetExecutablePath(raw, &size) != 0) {
        AlignedFree(raw);
        return {FileCode::SystemError, 0};
    }

    char resolved[PATH_MAX];
    const char* path = realpath(raw, resolved) != nullptr ? resolved : raw;
    usize length = 0;
    while (path[length] != '\0') {
        ++length;
    }

    FileStatus status;
    if (length + 1u > capacity) {
        bytes = length;
        status = {FileCode::Overflow, 0};
    } else {
        MemoryCopy(buffer, path, length);
        buffer[length] = '\0';
        bytes = length;
    }

    AlignedFree(raw);
    return status;
}

} // namespace rocks::platform
