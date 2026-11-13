#pragma once

#include <Platform/File.h>

#include <Windows.h>

// Win32 오류 코드를 PlatformBase 공통 상태(FileStatus)로 바꾸는 규칙이다. 파일·디렉터리 감시·
// 프로세스·문자열 변환 backend가 같은 규칙을 쓴다. 공개 헤더에 Windows 타입을 노출하지 않는다.
namespace rocks::platform::detail {

inline FileStatus NativeError(DWORD error) noexcept {
    FileCode code = FileCode::SystemError;
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        code = FileCode::NotFound;
        break;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        code = FileCode::AlreadyExists;
        break;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
        code = FileCode::AccessDenied;
        break;
    case ERROR_INVALID_NAME:
    case ERROR_INVALID_PARAMETER:
        code = FileCode::InvalidArgument;
        break;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        code = FileCode::OutOfMemory;
        break;
    case ERROR_NOT_SUPPORTED:
    case ERROR_NOT_SAME_DEVICE:
        code = FileCode::Unsupported;
        break;
    case ERROR_DIR_NOT_EMPTY:
        code = FileCode::DirectoryNotEmpty;
        break;
    case ERROR_DIRECTORY:
        code = FileCode::WrongType;
        break;
    }

    return {code, static_cast<uint32>(error)};
}

} // namespace rocks::platform::detail
