#include <Core/ScopeExit.h>
#include <Platform/Win64/WideString.h>
#include <PlatformWindow/Clipboard.h>

#include <Platform/Win64/WindowsHeadersBegin.h>
#include <Windows.h>
#include <Platform/Win64/WindowsHeadersEnd.h>

namespace rocks::platform {

Result<void> ReadClipboardText(String& output) noexcept {
    if (!OpenClipboard(GetActiveWindow())) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    ROCKS_ON_SCOPE_EXIT {
        CloseClipboard();
    };
    const HANDLE memory = GetClipboardData(CF_UNICODETEXT);
    if (!memory) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }
    const usize capacity = GlobalSize(memory) / sizeof(wchar_t);
    const auto* data = static_cast<const wchar_t*>(GlobalLock(memory));
    if (!data) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }
    ROCKS_ON_SCOPE_EXIT {
        GlobalUnlock(memory);
    };
    const usize length = windows::WideLength(data, capacity);
    if (length == capacity) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    windows::Utf8String value;
    if (!value.AssignWide(data, length)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    return output.TryAssign({value.Value(), value.Bytes()});
}

Result<void> WriteClipboardText(StringView text) noexcept {
    if (text.Contains(StringView("\0", 1))) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    HWND owner = GetActiveWindow();
    if (!owner) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }
    windows::WideString value;
    if (!value.AssignUtf8(text.Data(), text.ByteCount())) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // CF_UNICODETEXT는 CRLF를 사용한다. 소유권을 넘기기 전에 전체 버퍼를 채운다.
    usize units = value.Units();
    for (usize i = 0; i < value.Units(); ++i) {
        if (value.Value()[i] == L'\n' && (i == 0 || value.Value()[i - 1] != L'\r')) {
            ++units;
        }
    }
    if (units >= MaxSize / sizeof(wchar_t)) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }
    const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (units + 1) * sizeof(wchar_t));
    if (!memory) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }
    auto cleanup = MakeScopeExit([&]() noexcept { GlobalFree(memory); });
    auto* data = static_cast<wchar_t*>(GlobalLock(memory));
    if (!data) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }
    usize written = 0;
    for (usize i = 0; i < value.Units(); ++i) {
        const wchar_t ch = value.Value()[i];
        if (ch == L'\n' && (i == 0 || value.Value()[i - 1] != L'\r')) {
            data[written++] = L'\r';
        }
        data[written++] = ch;
    }
    data[written] = L'\0';
    GlobalUnlock(memory);
    if (!OpenClipboard(owner)) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    ROCKS_ON_SCOPE_EXIT {
        CloseClipboard();
    };
    if (!EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT, memory)) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }
    cleanup.Dismiss();
    return Result<void>::Success();
}

} // namespace rocks::platform
