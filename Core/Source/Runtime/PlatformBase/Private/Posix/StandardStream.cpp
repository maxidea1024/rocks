#include <Platform/StandardStream.h>
#include "PosixFileError.h"
#include <errno.h>
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

} // namespace rocks::platform
