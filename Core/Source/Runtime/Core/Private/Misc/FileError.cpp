#include <Core/FileError.h>

namespace rocks {

ErrorCode FileErrorCode(platform::FileStatus status) noexcept {
    switch (status.code) {
    case platform::FileCode::Success:
        return ErrorCode::None;
    case platform::FileCode::InvalidArgument:
    case platform::FileCode::WrongType:
    case platform::FileCode::EndOfFile:
        return ErrorCode::InvalidArgument;
    case platform::FileCode::AlreadyOpen:
    case platform::FileCode::Busy:
        return ErrorCode::Busy;
    case platform::FileCode::NotFound:
        return ErrorCode::NotFound;
    case platform::FileCode::AlreadyExists:
    case platform::FileCode::DirectoryNotEmpty:
        return ErrorCode::AlreadyExists;
    case platform::FileCode::OutOfMemory:
        return ErrorCode::OutOfMemory;
    case platform::FileCode::Overflow:
        return ErrorCode::Overflow;
    case platform::FileCode::Unsupported:
        return ErrorCode::Unsupported;
    case platform::FileCode::Cancelled:
        return ErrorCode::Cancelled;
    case platform::FileCode::NoProgress:
    case platform::FileCode::NotOpen:
    case platform::FileCode::AccessDenied:
    case platform::FileCode::SystemError:
    case platform::FileCode::NotDurable:
        return ErrorCode::DependencyFailed;
    }
    return ErrorCode::DependencyFailed;
}

} // namespace rocks
