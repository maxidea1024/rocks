#pragma once

#include <Platform/File.h>
#include <Platform/Memory.h>
#include "../Common/FileInternal.h"
#include "WindowsError.h"
#include "WindowsPathGuard.h"
#include "WindowsString.h"
#include <Windows.h>

// Windows 동기·비동기 파일 backend가 함께 쓰는 비공개 도구다. 공개 헤더에 Windows 타입을 노출하지
// 않는다. 오류 변환은 WindowsError.h, UTF-8·UTF-16 변환은 WindowsString.h에 있다.
namespace rocks::platform::detail {

// UTF-8 경로를 검증하고 NUL로 끝나는 UTF-16으로 바꾼다. 장치 namespace·레거시 DOS 장치 이름은
// Unsupported, 잘못된 UTF-8·embedded NUL은 InvalidArgument다. 변환 버퍼는 소멸 시 해제한다.
class WidePath {
public:
    FileStatus Initialize(const char* path, usize bytes) noexcept {
        if (!detail::ValidFilePath(path, bytes)) {
            return {FileCode::InvalidArgument, 0};
        }

        // 일반 파일 인터페이스에서 device namespace나 레거시 DOS 장치를 열지 않는다.
        if (detail::IsWindowsDevicePath(path, bytes)) {
            return {FileCode::Unsupported, 0};
        }

        auto status = value_.AssignUtf8(path, bytes);
        if (!status) {
            return status;
        }

        // 긴 경로: Win32가 실제로 쓰는 전체 경로가 MAX_PATH(260, 디렉터리 생성은 248) 근처를 넘으면
        // 확장 경로 접두사를 붙인다. 상대 경로("a\b", "\a", "C:a")는 입력이 짧아도 현재
        // 디렉터리를 붙이면 넘을 수 있으므로 전체 경로 길이로 판정한다. 확장 경로는 OS가
        // '/'·'.'·'..'를 해석하지 않으므로 GetFullPathNameW로 먼저 정규화한다. 짧은 경로는 기존
        // Win32 해석을 그대로 쓴다.
        if (value_.Units() >= LongPathThreshold) {
            return Extend();
        }

        if (!IsFullyQualified(value_.Value(), value_.Units())) {
            // 반환값은 NUL을 포함한 필요 단위 수다(0은 실패이며 실패는 아래 열기가 보고한다).
            const DWORD fullUnits = GetFullPathNameW(value_.Value(), 0, nullptr, nullptr);
            if (fullUnits != 0 && static_cast<usize>(fullUnits) - 1u >= LongPathThreshold) {
                return Extend();
            }
        }

        return {};
    }

    const wchar_t* Value() const noexcept {
        return value_.Value();
    }

    // 이 길이(UTF-16 단위) 이상이면 확장 경로로 바꾼다.
    static constexpr usize LongPathThreshold = 240;

private:
    // 현재 디렉터리와 무관한 경로인지다: "X:\"·"X:/" 드라이브 절대 경로 또는 "\\"·"//" UNC.
    // 장치 namespace는 Initialize가 먼저 거부한다.
    static bool IsFullyQualified(const wchar_t* path, usize units) noexcept {
        const auto separator = [](wchar_t value) noexcept {
            return value == L'\\' || value == L'/';
        };

        if (units >= 2 && separator(path[0]) && separator(path[1])) {
            return true;
        }

        const bool letter =
            (path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z');
        return units >= 3 && letter && path[1] == L':' && separator(path[2]);
    }

    FileStatus Extend() noexcept {
        const DWORD fullUnits = GetFullPathNameW(value_.Value(), 0, nullptr, nullptr);
        if (fullUnits == 0) {
            return NativeError(GetLastError());
        }

        auto* full =
            static_cast<wchar_t*>(AlignedAllocate(fullUnits * sizeof(wchar_t), alignof(wchar_t)));
        if (full == nullptr) {
            return {FileCode::OutOfMemory, 0};
        }

        const DWORD written = GetFullPathNameW(value_.Value(), fullUnits, full, nullptr);
        if (written == 0 || written >= fullUnits) {
            AlignedFree(full);
            return NativeError(GetLastError());
        }

        // "\\server\share\..." → "\\?\UNC\server\share\...", "C:\..." → "\\?\C:\...".
        const bool unc = written >= 2 && full[0] == L'\\' && full[1] == L'\\';
        const wchar_t* prefix = unc ? L"\\\\?\\UNC\\" : L"\\\\?\\";
        const usize prefixUnits = unc ? 8u : 4u;
        const usize skip = unc ? 2u : 0u;
        const usize units = prefixUnits + written - skip;
        auto* extended = static_cast<wchar_t*>(
            AlignedAllocate((units + 1u) * sizeof(wchar_t), alignof(wchar_t)));
        if (extended == nullptr) {
            AlignedFree(full);
            return {FileCode::OutOfMemory, 0};
        }

        platform::MemoryCopy(extended, prefix, prefixUnits * sizeof(wchar_t));
        platform::MemoryCopy(extended + prefixUnits, full + skip,
                             (written - skip) * sizeof(wchar_t));
        extended[units] = L'\0';
        AlignedFree(full);
        value_.Adopt(extended, units);
        return {};
    }

    WideString value_;
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

// 두 경로가 같은 파일 객체(같은 volume·file id)를 가리키는지 확인한다. followLinks가 false면 link
// 자체를 비교한다. ReFS는 64-bit file index가 유일하지 않을 수 있으므로 128-bit FILE_ID_INFO를
// 비교하고, 지원하지 않는 파일 시스템(FAT 등)에서만 64-bit index로 비교한다. 어느 한쪽이라도 열 수
// 없으면 false다.
inline bool SameFileObject(const wchar_t* first, const wchar_t* second,
                           bool followLinks = false) noexcept {
    FILE_ID_INFO wide[2]{};
    BY_HANDLE_FILE_INFORMATION identity[2]{};
    bool hasWide = true;
    const wchar_t* paths[2] = {first, second};
    const DWORD flags =
        FILE_FLAG_BACKUP_SEMANTICS | (followLinks ? 0 : FILE_FLAG_OPEN_REPARSE_POINT);
    for (int i = 0; i < 2; ++i) {
        HANDLE handle = CreateFileW(paths[i], FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, flags, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return false;
        }

        hasWide = hasWide &&
                  GetFileInformationByHandleEx(handle, FileIdInfo, &wide[i], sizeof(wide[i])) != 0;
        const bool queried = GetFileInformationByHandle(handle, &identity[i]) != 0;
        CloseHandle(handle);
        if (!queried) {
            return false;
        }
    }

    if (hasWide) {
        if (wide[0].VolumeSerialNumber != wide[1].VolumeSerialNumber) {
            return false;
        }

        for (usize i = 0; i < sizeof(wide[0].FileId.Identifier); ++i) {
            if (wide[0].FileId.Identifier[i] != wide[1].FileId.Identifier[i]) {
                return false;
            }
        }

        return true;
    }

    return identity[0].dwVolumeSerialNumber == identity[1].dwVolumeSerialNumber &&
           identity[0].nFileIndexHigh == identity[1].nFileIndexHigh &&
           identity[0].nFileIndexLow == identity[1].nFileIndexLow;
}

} // namespace rocks::platform::detail
