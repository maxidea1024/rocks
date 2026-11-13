#pragma once

#include "WindowsError.h"

#include <Platform/File.h>
#include <Platform/Memory.h>

#include <Windows.h>

// Win64 backend가 함께 쓰는 UTF-16 문자열 도구다. 공개 API는 UTF-8을 쓰고 Win32 "W" 함수는 UTF-16을
// 쓰므로, 두 표현 사이의 변환과 실행 파일 경로 읽기를 여기에만 둔다(파일·경로·프로세스가 공유한다).
namespace rocks::platform::detail {

// NUL로 끝나는 UTF-16 문자열을 소유한다. 복사할 수 없다. 실패한 Assign 뒤에는 빈 상태(Value
// null)다.
class WideString {
public:
    WideString() noexcept = default;
    ~WideString() noexcept {
        Reset();
    }

    WideString(const WideString&) = delete;
    WideString& operator=(const WideString&) = delete;

    // UTF-8 text(bytes개, NUL 종료 불필요)를 바꾼다. 빈 문자열도 된다. 잘못된 UTF-8은
    // InvalidArgument (nativeError에 Win32 오류), 2^31 byte 이상은 Overflow, 메모리 부족은
    // OutOfMemory다. NUL 문자 검사는 하지 않는다(경로·인자 검사는 호출자가 먼저 한다).
    FileStatus AssignUtf8(const char* text, usize bytes) noexcept {
        Reset();
        if (bytes > 0x7fffffffu) {
            return {FileCode::Overflow, 0};
        }

        int required = 0;
        if (bytes != 0) {
            required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text,
                                           static_cast<int>(bytes), nullptr, 0);
            if (required == 0) {
                return {FileCode::InvalidArgument, GetLastError()};
            }
        }

        const usize units = static_cast<usize>(required) + 1u;
        auto* value =
            static_cast<wchar_t*>(AlignedAllocate(units * sizeof(wchar_t), alignof(wchar_t)));
        if (value == nullptr) {
            return {FileCode::OutOfMemory, 0};
        }

        if (required != 0 &&
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, static_cast<int>(bytes), value,
                                required) != required) {
            const DWORD error = GetLastError();
            AlignedFree(value);
            return {FileCode::InvalidArgument, error};
        }

        value[required] = L'\0';
        Adopt(value, static_cast<usize>(required));
        return {};
    }

    // 현재 실행 파일의 전체 경로를 읽는다. 긴 경로를 위해 버퍼를 키워 가며 읽고 마지막 시도는
    // Windows 경로 최대 길이(32767 + NUL)다. 확장 경로 접두사(\\?\, \\?\UNC\)는 떼서 일반 Win32
    // 경로로 둔다 (긴 경로는 파일 API가 다시 확장한다). 실패는 OS 원인 코드다.
    FileStatus AssignModuleFileName() noexcept {
        Reset();
        constexpr DWORD MaxUnits = 32768;
        for (DWORD size = MAX_PATH;; size = size > MaxUnits / 2 ? MaxUnits : size * 2) {
            auto* path =
                static_cast<wchar_t*>(AlignedAllocate(size * sizeof(wchar_t), alignof(wchar_t)));
            if (path == nullptr) {
                return {FileCode::OutOfMemory, 0};
            }

            const DWORD length = GetModuleFileNameW(nullptr, path, size);
            if (length == 0) {
                const DWORD error = GetLastError();
                AlignedFree(path);
                return NativeError(error);
            }

            if (length < size) {
                Adopt(path, length);
                RemoveExtendedPrefix();
                return {};
            }

            AlignedFree(path);
            if (size == MaxUnits) {
                return {FileCode::SystemError, ERROR_INSUFFICIENT_BUFFER};
            }
        }
    }

    // AlignedAllocate로 할당하고 value[units]가 NUL인 buffer의 소유권을 넘겨받는다.
    void Adopt(wchar_t* value, usize units) noexcept {
        Reset();
        value_ = value;
        units_ = units;
    }

    void Reset() noexcept {
        AlignedFree(value_);
        value_ = nullptr;
        units_ = 0;
    }

    // Win32 API가 쓰기 가능한 버퍼를 요구하는 경우(CreateProcessW 명령줄)가 있어 non-const를 준다.
    wchar_t* Value() const noexcept {
        return value_;
    }

    // NUL을 뺀 UTF-16 단위 수다.
    usize Units() const noexcept {
        return units_;
    }

private:
    // "\\?\C:\..." → "C:\...", "\\?\UNC\server\share" → "\\server\share". 그 밖의 확장 경로
    // ("\\?\Volume{GUID}\...", "\\?\GLOBALROOT\...")는 접두사를 떼면 상대 경로가 되므로 그대로
    // 둔다.
    void RemoveExtendedPrefix() noexcept {
        const auto startsWith = [this](const wchar_t* prefix, usize count) noexcept {
            if (units_ < count) {
                return false;
            }

            for (usize i = 0; i < count; ++i) {
                if (value_[i] != prefix[i]) {
                    return false;
                }
            }

            return true;
        };

        const auto driveAt = [this](usize index) noexcept {
            if (units_ < index + 3u) {
                return false;
            }

            const wchar_t letter = value_[index];
            const bool alphabetic =
                (letter >= L'A' && letter <= L'Z') || (letter >= L'a' && letter <= L'z');
            return alphabetic && value_[index + 1u] == L':' &&
                   (value_[index + 2u] == L'\\' || value_[index + 2u] == L'/');
        };

        usize skip = 0;
        if (startsWith(L"\\\\?\\UNC\\", 8)) {
            // "\\?\UNC\"의 마지막 '\'와 앞에 둘 '\'로 "\\server"를 만든다.
            skip = 6;
            value_[skip] = L'\\';
        } else if (startsWith(L"\\\\?\\", 4) && driveAt(4)) {
            skip = 4;
        }

        if (skip != 0) {
            MemoryMove(value_, value_ + skip, (units_ - skip + 1u) * sizeof(wchar_t));
            units_ -= skip;
        }
    }

    wchar_t* value_ = nullptr;
    usize units_ = 0;
};

// UTF-16을 UTF-8로 바꿔 buffer에 쓴다(NUL 포함). 실패하면 false다. buffer가 부족하면 bytes에 필요한
// 길이(NUL 제외)를 두고, 바꿀 수 없는 UTF-16(짝 없는 대리)이면 bytes는 0이다.
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

// 공개 API가 문자열을 호출자 버퍼로 돌려주는 규칙(GetStandardDirectory·GetExecutablePath)이다.
// UTF-16 units개를 UTF-8로 buffer에 쓰고(NUL 포함) 길이를 bytes에 둔다(NUL 제외). buffer가 부족하면
// Overflow이며 bytes에 필요한 길이를 둔다. 바꿀 수 없는 UTF-16이면 SystemError
// (ERROR_NO_UNICODE_TRANSLATION)다.
inline FileStatus WriteUtf8(const wchar_t* text, usize units, char* buffer, usize capacity,
                            usize& bytes) noexcept {
    if (units > 0x7fffffffu) {
        return {FileCode::Overflow, 0};
    }

    usize written = 0;
    if (!WideToUtf8(text, static_cast<int>(units), buffer, capacity, written)) {
        if (written != 0) {
            bytes = written;
            return {FileCode::Overflow, 0};
        }

        return {FileCode::SystemError, ERROR_NO_UNICODE_TRANSLATION};
    }

    bytes = written;
    return {};
}

} // namespace rocks::platform::detail
