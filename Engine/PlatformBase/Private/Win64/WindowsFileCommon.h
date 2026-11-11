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

// FILETIME(1601-01-01 UTC부터의 100ns)을 FileInfo tick(0001-01-01 UTC부터의 100ns)으로 바꾼다.
// 0001-01-01부터 1601-01-01까지는 584388일이다.
inline int64 FileTimeToTicks(const FILETIME& time) noexcept {
    constexpr int64 Offset = 584388ll * 864000000000ll;
    // 9999-12-31 이후(UE FDateTime 범위 밖)는 알 수 없음(0)으로 둔다. overflow도 막는다.
    constexpr uint64 MaxTicks = 3155378975999999999ull;
    const uint64 value = (static_cast<uint64>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    if (value == 0 || value > MaxTicks - static_cast<uint64>(Offset)) {
        return 0;
    }
    return static_cast<int64>(value) + Offset;
}

// symbolic link·junction처럼 이름을 다른 경로로 대신하는 reparse point인지다. OneDrive placeholder·
// dedup 같은 다른 reparse point는 link가 아니다.
inline bool IsLinkReparse(DWORD attributes, DWORD reparseTag) noexcept {
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
           IsReparseTagNameSurrogate(reparseTag);
}

// 경로 자체가 link(이름 대체 reparse point)인지 link를 따라가지 않고 handle로 확인한다(부모
// 디렉터리 목록 권한이 필요 없다). 열 수 없으면 false다.
inline bool PathIsLink(const wchar_t* path) noexcept {
    HANDLE handle = CreateFileW(
        path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO tag{};
    const bool queried =
        GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) != 0;
    CloseHandle(handle);
    return queried && IsLinkReparse(tag.FileAttributes, tag.ReparseTag);
}

// 두 경로가 같은 파일 객체(같은 volume·file id)를 가리키는지 link를 따라가지 않고 확인한다.
// 어느 한쪽이라도 열 수 없으면 false다.
inline bool SameFileObject(const wchar_t* first, const wchar_t* second) noexcept {
    BY_HANDLE_FILE_INFORMATION identity[2]{};
    const wchar_t* paths[2] = {first, second};
    for (int i = 0; i < 2; ++i) {
        HANDLE handle = CreateFileW(
            paths[i], FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return false;
        }
        const bool queried = GetFileInformationByHandle(handle, &identity[i]) != 0;
        CloseHandle(handle);
        if (!queried) {
            return false;
        }
    }
    return identity[0].dwVolumeSerialNumber == identity[1].dwVolumeSerialNumber &&
           identity[0].nFileIndexHigh == identity[1].nFileIndexHigh &&
           identity[0].nFileIndexLow == identity[1].nFileIndexLow;
}

// UTF-16을 UTF-8로 바꿔 buffer에 쓴다(NUL 포함). 실패하면 false다.
inline bool WideToUtf8(const wchar_t* text, int units, char* buffer, usize capacity,
                       usize& bytes) noexcept {
    if (units == 0) {
        if (capacity == 0) {
            return false;
        }
        buffer[0] = '\0';
        bytes = 0;
        return true;
    }
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, units, nullptr, 0,
                                             nullptr, nullptr);
    if (required <= 0 || static_cast<usize>(required) >= capacity) {
        bytes = required > 0 ? static_cast<usize>(required) : 0;
        return false;
    }
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, units, buffer, required, nullptr,
                            nullptr) != required) {
        return false;
    }
    buffer[required] = '\0';
    bytes = static_cast<usize>(required);
    return true;
}

} // namespace rocks::platform::detail
