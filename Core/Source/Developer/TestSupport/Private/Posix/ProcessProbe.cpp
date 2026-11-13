#include <TestSupport/ProcessProbe.h>

#include <Platform/Process.h>
#include <Platform/Thread.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

namespace rocks::test {

bool ProcessSpawnExpected() noexcept {
    // macOS와 Android가 이 구현을 함께 쓴다. 지원 여부는 실행 중인 커널로 가린다(Darwin만 지원).
    utsname name{};
    return uname(&name) == 0 && strcmp(name.sysname, "Darwin") == 0;
}

usize ReadUtf8Arguments(int argc, char** argv, char* buffer, usize capacity, const char** arguments,
                        usize maxArguments) noexcept {
    if (argc < 0 || static_cast<usize>(argc) > maxArguments) {
        return 0;
    }

    usize used = 0;
    for (int i = 0; i < argc; ++i) {
        usize bytes = 0;
        while (argv[i][bytes] != '\0') {
            ++bytes;
        }

        if (bytes + 1u > capacity - used) {
            return 0;
        }

        for (usize j = 0; j < bytes; ++j) {
            buffer[used + j] = argv[i][j];
        }

        buffer[used + bytes] = '\0';
        arguments[i] = buffer + used;
        used += bytes + 1u;
    }

    return static_cast<usize>(argc);
}

bool WriteStandardStream(int stream, const void* data, usize bytes) noexcept {
    const int fd = stream == 2 ? STDERR_FILENO : STDOUT_FILENO;
    const auto* source = static_cast<const uint8*>(data);
    while (bytes != 0) {
        const ssize_t written = write(fd, source, bytes);
        if (written < 0 && errno == EINTR) {
            continue;
        }

        if (written <= 0) {
            return false;
        }

        source += written;
        bytes -= static_cast<usize>(written);
    }

    return true;
}

int64 ReadStandardInput(void* buffer, usize capacity) noexcept {
    for (;;) {
        const ssize_t got = read(STDIN_FILENO, buffer, capacity);
        if (got < 0 && errno == EINTR) {
            continue;
        }

        return got;
    }
}

uint32 SpawnSleepingDescendant() noexcept {
    char path[4096];
    usize bytes = 0;
    if (!platform::GetExecutablePath(path, sizeof(path), bytes)) {
        return 0;
    }

    char flag[] = "--process-probe";
    char mode[] = "sleep";
    char duration[] = "60000";
    char* arguments[] = {path, flag, mode, duration, nullptr};
    // 같은 프로세스 그룹에 남고 표준 fd를 물려받는다. fork 뒤에는 async-signal-safe 호출만 한다.
    const pid_t pid = fork();
    if (pid == 0) {
        execv(path, arguments);
        _exit(127);
    }

    return pid > 0 ? static_cast<uint32>(pid) : 0;
}

ProcessWatch::ProcessWatch(uint32 processId) noexcept : processId_(processId) {
}

ProcessWatch::~ProcessWatch() noexcept = default;

bool ProcessWatch::WaitGone(uint32 milliseconds) noexcept {
    // POSIX PID는 순서대로 다시 쓰이므로 짧은 시험 시간 안의 재사용은 드물다.
    for (uint32 waited = 0;; waited += 20) {
        if (!platform::IsProcessRunning(processId_)) {
            return true;
        }

        if (waited >= milliseconds) {
            return false;
        }

        platform::SleepFor(20);
    }
}

bool MakeHardLink(const char* existing, const char* link) noexcept {
    return ::link(existing, link) == 0;
}

int32 CurrentPriorityRank() noexcept {
    errno = 0;
    const int nice = getpriority(PRIO_PROCESS, 0);
    return errno == 0 ? nice : 0;
}

bool OpenInheritableFile(const char* path, uptr& handle, uint64& identity) noexcept {
    // O_CLOEXEC 없이 연다. 부모의 이런 fd를 자식이 물려받지 않는지 시험한다.
    const int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return false;
    }

    struct stat information{};
    if (fstat(fd, &information) != 0) {
        close(fd);
        return false;
    }

    unlink(path);
    handle = static_cast<uptr>(fd);
    identity = static_cast<uint64>(information.st_ino);
    return true;
}

void CloseInheritableFile(uptr handle) noexcept {
    close(static_cast<int>(handle));
}

bool HandleRefersToFile(uptr handle, uint64 identity) noexcept {
    struct stat information{};
    return fstat(static_cast<int>(handle), &information) == 0 &&
           static_cast<uint64>(information.st_ino) == identity;
}

bool RaiseTerminateSignal() noexcept {
    return kill(getpid(), SIGTERM) == 0;
}

} // namespace rocks::test
