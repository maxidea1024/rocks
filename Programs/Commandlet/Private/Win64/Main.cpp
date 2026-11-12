#include "CommandletHost.h"
#include <Windows.h>

namespace {

// 진입 인수는 프로세스 종료까지 보존한다. 대형 argv도 stack을 고갈시키지 않는다.
char argumentStorage[rocks::CommandletRegistry::MaxArguments]
                    [rocks::CommandletRegistry::MaxArgumentBytes + 1]{};

rocks::Result<void> Write(void*, rocks::CommandletStream stream, rocks::StringView text) noexcept {
    if (!text.ValidateUtf8())
        return rocks::Result<void>::Failure(rocks::ErrorCode::InvalidArgument);
    if (text.ByteCount() > rocks::CommandletRegistry::MaxArgumentBytes) {
        return rocks::Result<void>::Failure(rocks::ErrorCode::CapacityExceeded);
    }
    HANDLE handle = GetStdHandle(stream == rocks::CommandletStream::Output ? STD_OUTPUT_HANDLE
                                                                           : STD_ERROR_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return rocks::Result<void>::Failure(rocks::ErrorCode::DependencyFailed);
    }
    if (text.ByteCount() == 0)
        return rocks::Result<void>::Success();
    DWORD mode = 0;
    if (GetConsoleMode(handle, &mode)) {
        wchar_t converted[rocks::CommandletRegistry::MaxArgumentBytes + 1]{};
        int count = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, text.Data(), static_cast<int>(text.ByteCount()),
            converted, static_cast<int>(rocks::CommandletRegistry::MaxArgumentBytes));
        if (count <= 0)
            return rocks::Result<void>::Failure(rocks::ErrorCode::InvalidArgument);
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

int wmain(int argc, wchar_t** argv) {
    if (argc < 1 || argv == nullptr ||
        argc - 1 > static_cast<int>(rocks::CommandletRegistry::MaxArguments)) {
        return Write(nullptr, rocks::CommandletStream::Error, "Too many or invalid arguments.\n")
                   ? 2
                   : 1;
    }
    rocks::StringView args[rocks::CommandletRegistry::MaxArguments];
    for (int index = 1; index < argc; ++index) {
        if (!argv[index])
            return Write(nullptr, rocks::CommandletStream::Error, "Invalid argument.\n") ? 2 : 1;
        int bytes = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, argv[index], -1, argumentStorage[index - 1],
            static_cast<int>(rocks::CommandletRegistry::MaxArgumentBytes + 1), nullptr, nullptr);
        if (bytes <= 0) {
            return Write(nullptr, rocks::CommandletStream::Error,
                         "Invalid or oversized UTF-16 argument.\n")
                       ? 2
                       : 1;
        }
        args[index - 1] =
            rocks::StringView(argumentStorage[index - 1], static_cast<rocks::usize>(bytes - 1));
    }
    return rocks::commandlet_host::Run({args, static_cast<rocks::usize>(argc - 1)},
                                       {&Write, nullptr});
}
