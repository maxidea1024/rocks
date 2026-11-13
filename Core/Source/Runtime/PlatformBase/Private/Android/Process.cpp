#include "../Common/ProcessInternal.h"
#include "../Posix/PosixFileError.h"

#include <Platform/Memory.h>

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <unistd.h>

// Android는 자식 프로세스 생성을 아직 지원하지 않는다(posix_spawn이 API 28부터이고 앱 sandbox에서
// 쓸 일이 없다). 프로세스 ID 조회·생존 확인·실행 파일 경로만 제공한다.
namespace rocks::platform {

struct Process::State {};

FileStatus Process::Start(const ProcessStartInfo& info) noexcept {
    if (state_ != nullptr) {
        return {FileCode::AlreadyOpen, 0};
    }

    const auto status = detail::ValidateProcessStartInfo(info);
    if (!status) {
        return status;
    }

    return {FileCode::Unsupported, 0};
}

uint32 Process::Id() const noexcept {
    return 0;
}

WaitResult Process::Wait(uint32) noexcept {
    return WaitResult::Failed;
}

bool Process::GetExit(ProcessExit&) const noexcept {
    return false;
}

FileStatus Process::Terminate() noexcept {
    return {FileCode::NotOpen, 0};
}

FileTransfer Process::WriteInput(const void*, usize, uint32) noexcept {
    return {{FileCode::NotOpen, 0}, 0};
}

FileStatus Process::CloseInput() noexcept {
    return {FileCode::NotOpen, 0};
}

FileTransfer Process::Read(ProcessStream, void*, usize, uint32) noexcept {
    return {{FileCode::NotOpen, 0}, 0};
}

void Process::Close() noexcept {
    state_ = nullptr;
}

ProcessRunResult RunProcess(const ProcessStartInfo& info,
                            const ProcessRunOptions& options) noexcept {
    ProcessRunResult result;
    result.status = detail::ValidateProcessStartInfo(info);
    if (result.status) {
        result.status = detail::ValidateProcessRunOptions(info, options);
    }

    if (result.status) {
        result.status = {FileCode::Unsupported, 0};
    }

    return result;
}

uint32 CurrentProcessId() noexcept {
    return static_cast<uint32>(getpid());
}

bool IsProcessRunning(uint32 processId) noexcept {
    if (processId == 0 || processId > static_cast<uint32>(INT_MAX)) {
        return false;
    }

    return kill(static_cast<pid_t>(processId), 0) == 0 || errno == EPERM;
}

FileStatus GetExecutablePath(char* buffer, usize capacity, usize& bytes) noexcept {
    if (buffer == nullptr && capacity != 0) {
        return {FileCode::InvalidArgument, 0};
    }

    // readlink는 잘린 길이를 그대로 돌려주므로, 결과가 버퍼를 다 채우면 버퍼를 키워 실제 길이를
    // 얻는다.
    for (usize size = PATH_MAX; size <= 1u << 20; size *= 2u) {
        auto* path = static_cast<char*>(AlignedAllocate(size, 1));
        if (path == nullptr) {
            return {FileCode::OutOfMemory, 0};
        }

        const ssize_t length = readlink("/proc/self/exe", path, size);
        if (length < 0) {
            const int error = errno;
            AlignedFree(path);
            return detail::PosixFileError(error);
        }

        if (static_cast<usize>(length) < size) {
            FileStatus status;
            bytes = static_cast<usize>(length);
            if (bytes + 1u > capacity) {
                status = {FileCode::Overflow, 0};
            } else {
                MemoryCopy(buffer, path, bytes);
                buffer[bytes] = '\0';
            }

            AlignedFree(path);
            return status;
        }

        AlignedFree(path);
    }

    return {FileCode::Overflow, 0};
}

} // namespace rocks::platform
