#pragma once

#include <Platform/File.h>
#include <Platform/Memory.h>
#include "../Common/FileInternal.h"
#include "WindowsPathGuard.h"
#include <Windows.h>

// Windows 동기·비동기 파일 backend가 함께 쓰는 비공개 도구다. 공개 헤더에 Windows 타입을 노출하지
// 않는다.
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
        code = FileCode::Unsupported;
        break;
    }
    return {code, static_cast<uint32>(error)};
}

// UTF-8 경로를 검증하고 NUL로 끝나는 UTF-16으로 바꾼다. 장치 namespace·레거시 DOS 장치 이름은
// Unsupported, 잘못된 UTF-8·embedded NUL은 InvalidArgument다. 변환 버퍼는 소멸 시 해제한다.
class WidePath {
public:
    ~WidePath() noexcept {
        AlignedFree(value_);
    }
    FileStatus Initialize(const char* path, usize bytes) noexcept {
        if (!detail::ValidFilePath(path, bytes)) {
            return {FileCode::InvalidArgument, 0};
        }
        // 일반 파일 인터페이스에서 device namespace나 레거시 DOS 장치를 열지 않는다.
        if (detail::IsWindowsDevicePath(path, bytes)) {
            return {FileCode::Unsupported, 0};
        }
        if (bytes > 0x7fffffffU) {
            return {FileCode::Overflow, 0};
        }
        const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path,
                                                 static_cast<int>(bytes), nullptr, 0);
        if (required == 0) {
            return NativeError(GetLastError());
        }
        const usize units = static_cast<usize>(required) + 1;
        if (units > MaxSize / sizeof(wchar_t)) {
            return {FileCode::Overflow, 0};
        }
        value_ = static_cast<wchar_t*>(AlignedAllocate(units * sizeof(wchar_t), alignof(wchar_t)));
        if (!value_) {
            return {FileCode::OutOfMemory, 0};
        }
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, static_cast<int>(bytes),
                                value_, required) != required) {
            return NativeError(GetLastError());
        }
        value_[required] = L'\0';
        return {};
    }
    const wchar_t* Value() const noexcept {
        return value_;
    }

private:
    wchar_t* value_ = nullptr;
};

} // namespace rocks::platform::detail
