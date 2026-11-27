#include <Platform/StandardStream.h>
#include "PosixFileError.h"
#include <errno.h>
#include <poll.h>
#include <unistd.h>

// POSIX 표준 스트림이다(SparksTools T2): descriptor 0·1·2에 read·write(EINTR 재시도)로 byte
// 그대로다(터미널도 UTF-8을 그대로 받는다).
namespace rocks::platform {

FileTransfer WriteStandardStream(StandardStream stream, const char* bytes, usize count) noexcept {
    FileTransfer result;
    if (bytes == nullptr && count != 0) {
        result.status.code = FileCode::InvalidArgument;
        return result;
    }

    const int descriptor = stream == StandardStream::Output ? STDOUT_FILENO : STDERR_FILENO;
    while (result.bytes < count) {
        const ssize_t written = write(descriptor, bytes + result.bytes, count - result.bytes);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }

            result.status = errno == EPIPE   ? FileStatus{FileCode::EndOfFile, uint32(EPIPE)}
                            : errno == EBADF ? FileStatus{FileCode::NotOpen, uint32(EBADF)}
                                             : detail::PosixFileError(errno);
            return result;
        }

        if (written == 0) {
            result.status.code = FileCode::NoProgress;
            return result;
        }

        result.bytes += static_cast<usize>(written);
    }

    return result;
}

FileTransfer ReadStandardInput(void* buffer, usize capacity) noexcept {
    FileTransfer result;
    if (buffer == nullptr || capacity == 0) {
        result.status.code = FileCode::InvalidArgument;
        return result;
    }

    for (;;) {
        const ssize_t read = ::read(STDIN_FILENO, buffer, capacity);
        if (read < 0) {
            if (errno == EINTR) {
                continue;
            }

            result.status = errno == EBADF ? FileStatus{FileCode::NotOpen, uint32(EBADF)}
                                           : detail::PosixFileError(errno);
            return result;
        }

        result.bytes = static_cast<usize>(read);
        if (read == 0) {
            result.status.code = FileCode::EndOfFile;
        }

        return result;
    }
}

FileTransfer ReadStandardInput(void* buffer, usize capacity, uint32 timeoutMilliseconds) noexcept {
    if (timeoutMilliseconds == InfiniteTimeout || buffer == nullptr || capacity == 0) {
        return ReadStandardInput(buffer, capacity);
    }

    FileTransfer result;
    pollfd descriptor{};
    descriptor.fd = STDIN_FILENO;
    descriptor.events = POLLIN;
    // poll의 시간 인자는 int 밀리초다(InfiniteTimeout은 위에서 걸렀다).
    constexpr uint32 MaxPollMilliseconds = static_cast<uint32>(NumericLimits<int>::Max());
    const int timeout = static_cast<int>(
        timeoutMilliseconds < MaxPollMilliseconds ? timeoutMilliseconds : MaxPollMilliseconds);
    for (;;) {
        const int ready = ::poll(&descriptor, 1, timeout);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }

            result.status = detail::PosixFileError(errno);
            return result;
        }

        if (ready == 0) {
            result.status.code = FileCode::NoProgress;
            return result;
        }

        if ((descriptor.revents & POLLNVAL) != 0) {
            result.status = FileStatus{FileCode::NotOpen, uint32(EBADF)};
            return result;
        }

        // 읽을 것이 있거나 쓰는 쪽이 닫혔다(POLLHUP — read가 EOF를 준다).
        return ReadStandardInput(buffer, capacity);
    }
}

} // namespace rocks::platform
