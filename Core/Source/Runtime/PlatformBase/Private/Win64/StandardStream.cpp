#include <Platform/StandardStream.h>
#include "WindowsError.h"

#include <Platform/Win64/WindowsHeadersBegin.h>

#include <Windows.h>

#include <Platform/Win64/WindowsHeadersEnd.h>

// Windows 표준 스트림이다(SparksTools T2): 콘솔이면 UTF-16 WriteConsoleW, pipe·파일이면 WriteFile·
// ReadFile로 byte 그대로다.
namespace rocks::platform {
namespace {

// 콘솔 출력 한 조각의 UTF-8 byte 수다(UTF-8 n byte는 UTF-16 n 단위를 넘지 않는다).
constexpr usize ConsoleChunkBytes = 4096;

HANDLE StandardHandle(DWORD which) noexcept {
    HANDLE handle = GetStdHandle(which);
    return handle == INVALID_HANDLE_VALUE ? nullptr : handle;
}

// bytes[0, limit) 안에서 글자 경계로 자른 길이다(UTF-8 이어짐 byte 앞에서 끊지 않는다).
usize CodePointPrefix(const char* bytes, usize count, usize limit) noexcept {
    if (count <= limit) {
        return count;
    }

    usize end = limit;
    while (end > 0 && (static_cast<unsigned char>(bytes[end]) & 0xC0) == 0x80) {
        --end;
    }

    return end;
}

FileTransfer WriteConsole(HANDLE handle, const char* bytes, usize count) noexcept {
    FileTransfer result;
    wchar_t converted[ConsoleChunkBytes + 1] = {};
    while (result.bytes < count) {
        const usize chunk =
            CodePointPrefix(bytes + result.bytes, count - result.bytes, ConsoleChunkBytes);
        if (chunk == 0) {
            result.status.code = FileCode::InvalidArgument;
            return result;
        }

        const int units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes + result.bytes,
                                              static_cast<int>(chunk), converted,
                                              static_cast<int>(ConsoleChunkBytes + 1));
        if (units <= 0) {
            result.status.code = FileCode::InvalidArgument;
            return result;
        }

        int offset = 0;
        while (offset < units) {
            DWORD written = 0;
            if (!WriteConsoleW(handle, converted + offset, static_cast<DWORD>(units - offset),
                               &written, nullptr) ||
                written == 0) {
                result.status = detail::NativeError(GetLastError());
                return result;
            }

            offset += static_cast<int>(written);
        }

        result.bytes += chunk;
    }

    return result;
}

} // namespace

FileTransfer WriteStandardStream(StandardStream stream, const char* bytes, usize count) noexcept {
    FileTransfer result;
    if (bytes == nullptr && count != 0) {
        result.status.code = FileCode::InvalidArgument;
        return result;
    }

    HANDLE handle =
        StandardHandle(stream == StandardStream::Output ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
    if (handle == nullptr) {
        result.status.code = FileCode::NotOpen;
        return result;
    }

    if (count == 0) {
        return result;
    }

    DWORD mode = 0;
    if (GetConsoleMode(handle, &mode)) {
        return WriteConsole(handle, bytes, count);
    }

    constexpr usize MaxWriteBytes = 0x40000000; // WriteFile 한 번의 상한(DWORD 안 — 1 GiB)
    while (result.bytes < count) {
        const usize part =
            count - result.bytes < MaxWriteBytes ? count - result.bytes : MaxWriteBytes;
        DWORD written = 0;
        if (!WriteFile(handle, bytes + result.bytes, static_cast<DWORD>(part), &written, nullptr)) {
            const DWORD error = GetLastError();
            result.status = error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA
                                ? FileStatus{FileCode::EndOfFile, static_cast<uint32>(error)}
                                : detail::NativeError(error);
            return result;
        }

        if (written == 0) {
            result.status.code = FileCode::NoProgress;
            return result;
        }

        result.bytes += written;
    }

    return result;
}

FileTransfer ReadStandardInput(void* buffer, usize capacity) noexcept {
    FileTransfer result;
    if (buffer == nullptr || capacity == 0) {
        result.status.code = FileCode::InvalidArgument;
        return result;
    }

    HANDLE handle = StandardHandle(STD_INPUT_HANDLE);
    if (handle == nullptr) {
        result.status.code = FileCode::NotOpen;
        return result;
    }

    constexpr usize MaxReadBytes = 0x40000000; // ReadFile 한 번의 상한(DWORD 안 — 1 GiB)
    DWORD read = 0;
    if (!ReadFile(handle, buffer,
                  static_cast<DWORD>(capacity < MaxReadBytes ? capacity : MaxReadBytes), &read,
                  nullptr)) {
        const DWORD error = GetLastError();
        result.status = error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF
                            ? FileStatus{FileCode::EndOfFile, static_cast<uint32>(error)}
                            : detail::NativeError(error);
        return result;
    }

    result.bytes = read;
    if (read == 0) {
        result.status.code = FileCode::EndOfFile;
    }

    return result;
}

} // namespace rocks::platform
