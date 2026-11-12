#pragma once

#include <Platform/File.h>

#include <errno.h>

// POSIX errno를 FileStatus로 바꾸는 공용 규칙이다. 동기 File과 AsyncFile이 같은 규칙을 쓴다.
namespace rocks::platform::detail {

inline FileStatus PosixFileError(int error) noexcept {
    FileCode code = FileCode::SystemError;
    switch (error) {
    case ENOENT:
    case ENOTDIR:
        code = FileCode::NotFound;
        break;
    case EEXIST:
        code = FileCode::AlreadyExists;
        break;
    case EACCES:
    case EPERM:
        code = FileCode::AccessDenied;
        break;
    case EINVAL:
        code = FileCode::InvalidArgument;
        break;
    case ENOMEM:
        code = FileCode::OutOfMemory;
        break;
    case EOVERFLOW:
        code = FileCode::Overflow;
        break;
    case ENOTSUP:
    case EXDEV:
        code = FileCode::Unsupported;
        break;
    case ENOTEMPTY:
        code = FileCode::DirectoryNotEmpty;
        break;
    case EISDIR:
        code = FileCode::WrongType;
        break;
    default:
        break;
    }
    return {code, static_cast<uint32>(error)};
}

} // namespace rocks::platform::detail
