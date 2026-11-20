#include "CommandletHost.h"
#include <Platform/Win64/WideString.h>
#include <Windows.h>

// Windows 표준 출력 채널이다. 콘솔이면 UTF-16 WriteConsoleW, 리디렉션이면 UTF-8 byte 그대로다.
namespace {

rocks::Result<void> Write(void*, rocks::CommandletStream stream, rocks::StringView text) noexcept {
    if (!text.ValidateUtf8())
        return rocks::Result<void>::Failure(rocks::ErrorCode::InvalidArgument);
    HANDLE handle = GetStdHandle(stream == rocks::CommandletStream::Output ? STD_OUTPUT_HANDLE
                                                                           : STD_ERROR_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return rocks::Result<void>::Failure(rocks::ErrorCode::DependencyFailed);
    }
    if (text.ByteCount() == 0)
        return rocks::Result<void>::Success();
    DWORD mode = 0;
    if (GetConsoleMode(handle, &mode)) {
        // 출력(config-dump 등)은 인자 상한보다 클 수 있다. 글자 경계에서 ConsoleChunkBytes씩 나눠
        // 바꾼다. UTF-8 n byte는 UTF-16 n 단위를 넘지 않으므로 조각 크기 + 1 단위면 충분하다.
        constexpr rocks::usize ConsoleChunkBytes = rocks::CommandletRegistry::MaxArgumentBytes;
        wchar_t converted[ConsoleChunkBytes + 1]{};
        rocks::StringView rest = text;
        while (rest.ByteCount() > 0) {
            const rocks::StringView chunk = rest.LeftBytesAtCodePoint(ConsoleChunkBytes);
            if (chunk.ByteCount() == 0) {
                return rocks::Result<void>::Failure(rocks::ErrorCode::InvalidArgument);
            }

            const rocks::platform::windows::TextConversion wide =
                rocks::platform::windows::ConvertUtf8ToWide(
                    chunk.Data(), chunk.ByteCount(), converted, ConsoleChunkBytes + 1,
                    rocks::platform::windows::TextFit::Exact);
            if (!wide)
                return rocks::Result<void>::Failure(rocks::ErrorCode::InvalidArgument);
            const int count = static_cast<int>(wide.length);
            int offset = 0;
            while (offset < count) {
                DWORD written = 0;
                if (!WriteConsoleW(handle, converted + offset, static_cast<DWORD>(count - offset),
                                   &written, nullptr) ||
                    written == 0) {
                    return rocks::Result<void>::Failure(rocks::ErrorCode::DependencyFailed);
                }
                offset += static_cast<int>(written);
            }

            rest = rocks::StringView(rest.Data() + chunk.ByteCount(),
                                     rest.ByteCount() - chunk.ByteCount());
        }
    } else {
        rocks::usize offset = 0;
        while (offset < text.ByteCount()) {
            DWORD written = 0;
            if (!WriteFile(handle, text.Data() + offset,
                           static_cast<DWORD>(text.ByteCount() - offset), &written, nullptr) ||
                written == 0) {
                return rocks::Result<void>::Failure(rocks::ErrorCode::DependencyFailed);
            }
            offset += written;
        }
    }
    return rocks::Result<void>::Success();
}

} // namespace

namespace rocks::commandlet_host {

CommandletOutput StandardOutput() noexcept {
    return {&Write, nullptr};
}

bool PrepareStandardOutput() noexcept {
    return true;
}

} // namespace rocks::commandlet_host
