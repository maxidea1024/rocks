#pragma once
#include <Base/Types.h>

namespace rocks::platform::detail {

inline bool WindowsPathSeparator(char value) noexcept {
    return value == '/' || value == '\\';
}

inline char AsciiUpper(char value) noexcept {
    return value >= 'a' && value <= 'z' ? static_cast<char>(value - 'a' + 'A') : value;
}

inline bool WindowsDeviceName(const char* path, usize first, usize last) noexcept {
    // DOS 디바이스 이름은 확장자 뒤에도 디바이스로 해석될 수 있다.
    usize nameEnd = first;
    while (nameEnd < last && path[nameEnd] != '.' && path[nameEnd] != ':') {
        ++nameEnd;
    }
    while (nameEnd > first && path[nameEnd - 1] == ' ') {
        --nameEnd;
    }
    const usize size = nameEnd - first;
    if ((size == 6 || size == 7) && AsciiUpper(path[first]) == 'C' &&
        AsciiUpper(path[first + 1]) == 'O' && AsciiUpper(path[first + 2]) == 'N') {
        if (size == 6 && AsciiUpper(path[first + 3]) == 'I' && AsciiUpper(path[first + 4]) == 'N' &&
            path[first + 5] == '$') {
            return true;
        }
        if (size == 7 && AsciiUpper(path[first + 3]) == 'O' && AsciiUpper(path[first + 4]) == 'U' &&
            AsciiUpper(path[first + 5]) == 'T' && path[first + 6] == '$') {
            return true;
        }
    }
    if (size == 3) {
        const char a = AsciiUpper(path[first]);
        const char b = AsciiUpper(path[first + 1]);
        const char c = AsciiUpper(path[first + 2]);
        return (a == 'C' && b == 'O' && c == 'N') || (a == 'P' && b == 'R' && c == 'N') ||
               (a == 'A' && b == 'U' && c == 'X') || (a == 'N' && b == 'U' && c == 'L');
    }
    if (size == 4 || size == 5) {
        const char a = AsciiUpper(path[first]);
        const char b = AsciiUpper(path[first + 1]);
        const char c = AsciiUpper(path[first + 2]);
        const bool port = (a == 'C' && b == 'O' && c == 'M') || (a == 'L' && b == 'P' && c == 'T');
        if (port && size == 4 && path[first + 3] >= '1' && path[first + 3] <= '9') {
            return true;
        }
        // Win32는 위첨자 1·2·3도 COM/LPT 포트 번호로 취급한다.
        if (port && size == 5 && static_cast<uint8>(path[first + 3]) == 0xC2) {
            const auto digit = static_cast<uint8>(path[first + 4]);
            if (digit == 0xB9 || digit == 0xB2 || digit == 0xB3) {
                return true;
            }
        }
    }
    return false;
}

inline bool IsWindowsDevicePath(const char* path, usize size) noexcept {
    if (size >= 4 && WindowsPathSeparator(path[0]) && path[1] == '?' && path[2] == '?' &&
        WindowsPathSeparator(path[3])) {
        return true;
    }
    if (size >= 4 && WindowsPathSeparator(path[0]) && WindowsPathSeparator(path[1]) &&
        (path[2] == '.' || path[2] == '?') && WindowsPathSeparator(path[3])) {
        return true;
    }
    const bool drive = size >= 2 &&
                       ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
                       path[1] == ':';
    usize start = drive ? 2 : 0;
    usize uncComponents =
        size >= 2 && WindowsPathSeparator(path[0]) && WindowsPathSeparator(path[1]) ? 2 : 0;
    while (start < size) {
        while (start < size && WindowsPathSeparator(path[start])) {
            ++start;
        }
        const usize first = start;
        while (start < size && !WindowsPathSeparator(path[start])) {
            ++start;
        }
        if (first == start) {
            break;
        }
        if (uncComponents != 0) {
            --uncComponents;
        } else if (WindowsDeviceName(path, first, start)) {
            return true;
        }
    }
    return false;
}

} // namespace rocks::platform::detail
