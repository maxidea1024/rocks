#include <Platform/Win64/WideString.h>

#include "WindowsError.h"

#include <Base/NumericLimits.h>
#include <Platform/Memory.h>

#include <Windows.h>

namespace rocks::platform::windows {

namespace {

// Win32 변환 함수의 길이 인자는 int다.
constexpr usize MaxConvertibleLength = static_cast<usize>(NumericLimits<int>::Max());
// 실행 파일 경로를 읽는 마지막 버퍼 크기다(Windows 경로 최대 32767 + NUL).
constexpr DWORD MaxModulePathUnits = 32768;

// UTF-8 lead byte로 sequence 길이를 정한다. 입력은 이미 검사했으므로 continuation byte가 lead
// 자리에 오지 않는다.
usize Utf8SequenceBytes(uint8 lead) noexcept {
    if (lead < 0x80u) {
        return 1;
    }

    if (lead < 0xE0u) {
        return 2;
    }

    return lead < 0xF0u ? 3 : 4;
}

bool IsHighSurrogate(wchar_t unit) noexcept {
    return unit >= 0xD800 && unit <= 0xDBFF;
}

// UTF-16 글자 하나(units 단위)의 UTF-8 byte 수다. 입력은 이미 검사했으므로 high surrogate 뒤에는
// low surrogate가 있다.
usize Utf8BytesOf(const wchar_t* text, usize& units) noexcept {
    const wchar_t unit = text[0];
    units = 1;
    if (unit < 0x80) {
        return 1;
    }

    if (unit < 0x800) {
        return 2;
    }

    if (IsHighSurrogate(unit)) {
        units = 2;
        return 4;
    }

    return 3;
}

bool IsLowSurrogate(wchar_t unit) noexcept {
    return unit >= 0xDC00 && unit <= 0xDFFF;
}

// UTF-16 text[0, units)를 검사하고 UTF-8 길이를 usize로 센다. WideCharToMultiByte는 결과 길이를
// int로 돌려주므로 2^31 byte 이상인 결과(UTF-16 약 7억 단위부터)를 잘못된 입력과 구분하지 못한다.
// 짝 없는 surrogate면 거짓이다.
bool MeasureUtf8(const wchar_t* text, usize units, usize& bytes) noexcept {
    bytes = 0;
    for (usize index = 0; index < units;) {
        const wchar_t unit = text[index];
        if (IsLowSurrogate(unit) ||
            (IsHighSurrogate(unit) && (index + 1 >= units || !IsLowSurrogate(text[index + 1])))) {
            return false;
        }

        usize characterUnits = 0;
        bytes += Utf8BytesOf(text + index, characterUnits);
        index += characterUnits;
    }

    return true;
}

TextConversion Failed(FileCode code, uint32 nativeError) noexcept {
    TextConversion result;
    result.status = {code, nativeError};
    return result;
}

// 검사를 마친 UTF-8 text[0, bytes)를 buffer에 바꿔 쓴다. 바꾼 단위 수는 units다.
bool WriteWide(const char* text, usize bytes, wchar_t* buffer, usize units) noexcept {
    if (bytes == 0) {
        return true;
    }

    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, static_cast<int>(bytes), buffer,
                               static_cast<int>(units)) == static_cast<int>(units);
}

// 검사를 마친 UTF-16 text[0, units)를 buffer에 바꿔 쓴다. 바꾼 byte 수는 bytes다.
bool WriteUtf8(const wchar_t* text, usize units, char* buffer, usize bytes) noexcept {
    if (units == 0) {
        return true;
    }

    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, static_cast<int>(units), buffer,
                               static_cast<int>(bytes), nullptr,
                               nullptr) == static_cast<int>(bytes);
}

} // namespace

FileStatus WideString::AssignUtf8(const char* text, usize bytes) noexcept {
    Reset();
    if (text == nullptr && bytes != 0) {
        return {FileCode::InvalidArgument, 0};
    }

    if (bytes > MaxConvertibleLength) {
        return {FileCode::Overflow, 0};
    }

    int required = 0;
    if (bytes != 0) {
        required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, static_cast<int>(bytes),
                                       nullptr, 0);
        if (required == 0) {
            return {FileCode::InvalidArgument, GetLastError()};
        }
    }

    const usize units = static_cast<usize>(required) + 1u;
    auto* value = static_cast<wchar_t*>(AlignedAllocate(units * sizeof(wchar_t), alignof(wchar_t)));
    if (value == nullptr) {
        return {FileCode::OutOfMemory, 0};
    }

    if (!WriteWide(text, bytes, value, static_cast<usize>(required))) {
        const DWORD error = GetLastError();
        AlignedFree(value);
        return {FileCode::InvalidArgument, error};
    }

    value[required] = L'\0';
    Adopt(value, static_cast<usize>(required));
    return {};
}

FileStatus WideString::AssignModuleFileName() noexcept {
    Reset();
    for (DWORD size = MAX_PATH;;
         size = size > MaxModulePathUnits / 2 ? MaxModulePathUnits : size * 2) {
        auto* path =
            static_cast<wchar_t*>(AlignedAllocate(size * sizeof(wchar_t), alignof(wchar_t)));
        if (path == nullptr) {
            return {FileCode::OutOfMemory, 0};
        }

        const DWORD length = GetModuleFileNameW(nullptr, path, size);
        if (length == 0) {
            const DWORD error = GetLastError();
            AlignedFree(path);
            return detail::NativeError(error);
        }

        if (length < size) {
            Adopt(path, length);
            RemoveExtendedPrefix();
            return {};
        }

        AlignedFree(path);
        if (size == MaxModulePathUnits) {
            return {FileCode::SystemError, ERROR_INSUFFICIENT_BUFFER};
        }
    }
}

void WideString::Adopt(wchar_t* value, usize units) noexcept {
    Reset();
    value_ = value;
    units_ = units;
}

void WideString::Reset() noexcept {
    AlignedFree(value_);
    value_ = nullptr;
    units_ = 0;
}

// "\\?\C:\..." → "C:\...", "\\?\UNC\server\share" → "\\server\share". 그 밖의 확장 경로
// ("\\?\Volume{GUID}\...", "\\?\GLOBALROOT\...")는 접두사를 떼면 상대 경로가 되므로 그대로 둔다.
void WideString::RemoveExtendedPrefix() noexcept {
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

FileStatus Utf8String::AssignWide(const wchar_t* text, usize units) noexcept {
    Reset();
    if (text == nullptr && units != 0) {
        return {FileCode::InvalidArgument, 0};
    }

    usize required = 0;
    if (!MeasureUtf8(text, units, required)) {
        return {FileCode::InvalidArgument, ERROR_NO_UNICODE_TRANSLATION};
    }

    if (required > MaxConvertibleLength) {
        return {FileCode::Overflow, 0};
    }

    auto* value = static_cast<char*>(AlignedAllocate(required + 1u, alignof(char)));
    if (value == nullptr) {
        return {FileCode::OutOfMemory, 0};
    }

    if (!WriteUtf8(text, units, value, required)) {
        const DWORD error = GetLastError();
        AlignedFree(value);
        return {FileCode::InvalidArgument, error};
    }

    value[required] = '\0';
    value_ = value;
    bytes_ = required;
    return {};
}

void Utf8String::Reset() noexcept {
    AlignedFree(value_);
    value_ = nullptr;
    bytes_ = 0;
}

TextConversion ConvertUtf8ToWide(const char* text, usize bytes, wchar_t* buffer, usize capacity,
                                 TextFit fit) noexcept {
    if (buffer != nullptr && capacity != 0) {
        buffer[0] = L'\0';
    }

    if (text == nullptr && bytes != 0) {
        return Failed(FileCode::InvalidArgument, 0);
    }

    if (bytes > MaxConvertibleLength) {
        return Failed(FileCode::Overflow, 0);
    }

    // 전체를 먼저 검사하고 필요한 길이를 얻는다.
    int required = 0;
    if (bytes != 0) {
        required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, static_cast<int>(bytes),
                                       nullptr, 0);
        if (required == 0) {
            return Failed(FileCode::InvalidArgument, GetLastError());
        }
    }

    TextConversion result;
    usize used = bytes;
    usize units = static_cast<usize>(required);
    if (buffer == nullptr || capacity == 0 || (units > capacity - 1u && fit == TextFit::Exact)) {
        result = Failed(FileCode::Overflow, 0);
        result.required = units;
        return result;
    }

    const usize room = capacity - 1u;
    if (units > room) {
        // 들어가는 가장 긴 앞부분을 글자 단위로 찾는다(4 byte sequence는 surrogate 쌍 2단위).
        used = 0;
        units = 0;
        while (used < bytes) {
            const usize sequence = Utf8SequenceBytes(static_cast<uint8>(text[used]));
            const usize sequenceUnits = sequence == 4 ? 2 : 1;
            if (units + sequenceUnits > room) {
                break;
            }

            used += sequence;
            units += sequenceUnits;
        }

        result.truncated = true;
    }

    if (!WriteWide(text, used, buffer, units)) {
        const DWORD error = GetLastError();
        buffer[0] = L'\0';
        return Failed(FileCode::InvalidArgument, error);
    }

    buffer[units] = L'\0';
    result.length = units;
    return result;
}

TextConversion ConvertWideToUtf8(const wchar_t* text, usize units, char* buffer, usize capacity,
                                 TextFit fit) noexcept {
    if (buffer != nullptr && capacity != 0) {
        buffer[0] = '\0';
    }

    if (text == nullptr && units != 0) {
        return Failed(FileCode::InvalidArgument, 0);
    }

    // 전체를 먼저 검사하고 필요한 길이를 얻는다.
    usize required = 0;
    if (!MeasureUtf8(text, units, required)) {
        return Failed(FileCode::InvalidArgument, ERROR_NO_UNICODE_TRANSLATION);
    }

    // 한 번에 쓸 수 있는 길이는 int 범위다. Exact는 그보다 긴 결과를 Overflow로, Truncate는 그
    // 안에서 자른다.
    TextConversion result;
    usize used = units;
    usize bytes = required;
    const bool fits = buffer != nullptr && capacity != 0 && bytes <= capacity - 1u &&
                      bytes <= MaxConvertibleLength;
    if (buffer == nullptr || capacity == 0 || (!fits && fit == TextFit::Exact)) {
        result = Failed(FileCode::Overflow, 0);
        result.required = bytes;
        return result;
    }

    const usize room = capacity - 1u < MaxConvertibleLength ? capacity - 1u : MaxConvertibleLength;
    if (bytes > room) {
        // 들어가는 가장 긴 앞부분을 글자 단위로 찾는다(surrogate 쌍은 4 byte 한 글자).
        used = 0;
        bytes = 0;
        while (used < units) {
            usize characterUnits = 0;
            const usize characterBytes = Utf8BytesOf(text + used, characterUnits);
            if (bytes + characterBytes > room) {
                break;
            }

            used += characterUnits;
            bytes += characterBytes;
        }

        result.truncated = true;
    }

    if (!WriteUtf8(text, used, buffer, bytes)) {
        const DWORD error = GetLastError();
        buffer[0] = '\0';
        return Failed(FileCode::InvalidArgument, error);
    }

    buffer[bytes] = '\0';
    result.length = bytes;
    return result;
}

} // namespace rocks::platform::windows
