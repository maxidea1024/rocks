#include "Win64InlineComposition.h"

#include <Core/ScopeExit.h>
#include <Platform/Win64/WideString.h>
#include <PlatformWindow/InlineComposition.h>
#include <Platform/Win64/WindowsHeadersBegin.h>
#include <imm.h>
#include <Platform/Win64/WindowsHeadersEnd.h>

namespace rocks::platform {
namespace {
thread_local HWND inlineWindow = nullptr;
constexpr wchar_t InlineProperty[] = L"Rocks.InlineComposition";

Result<input::InputEvent> ReadComposition(HIMC context, DWORD kind, Allocator& allocator) noexcept {
    const LONG bytes = ImmGetCompositionStringW(context, kind, nullptr, 0);
    if (bytes < 0 || bytes % sizeof(wchar_t) != 0) {
        return Result<input::InputEvent>::Failure(ErrorCode::InvalidArgument);
    }
    const usize units = static_cast<usize>(bytes) / sizeof(wchar_t);
    Array<wchar_t> buffer(allocator);
    auto resized = buffer.TryResize(units + 1);
    if (!resized) {
        return Result<input::InputEvent>::Failure(resized.Error());
    }
    if (bytes && ImmGetCompositionStringW(context, kind, buffer.Data(),
                                          static_cast<DWORD>(bytes)) != bytes) {
        return Result<input::InputEvent>::Failure(ErrorCode::NotReady);
    }
    const bool commit = kind == GCS_RESULTSTR;
    windows::Utf8String text;
    if (!text.AssignWide(buffer.Data(), units)) {
        return Result<input::InputEvent>::Failure(ErrorCode::InvalidArgument);
    }
    auto storage = SharedPtr<String>::TryMake(allocator, allocator);
    if (!storage) {
        return Result<input::InputEvent>::Failure(storage.Error());
    }
    auto stored = storage.Value()->TryAssign({text.Value(), text.Bytes()});
    if (!stored) {
        return Result<input::InputEvent>::Failure(stored.Error());
    }
    usize caret = text.Bytes();
    if (!commit) {
        const LONG position = ImmGetCompositionStringW(context, GCS_CURSORPOS, nullptr, 0);
        if (position >= 0 && static_cast<usize>(position) < units) {
            windows::Utf8String prefix;
            if (prefix.AssignWide(buffer.Data(), static_cast<usize>(position))) {
                caret = prefix.Bytes();
            }
        }
    }
    return Result<input::InputEvent>::Success(
        input::InputEvent::Composition(Move(storage.Value()), caret, commit));
}
} // namespace

bool IsInlineCompositionWindow(HWND window) noexcept {
    return window && window == inlineWindow && GetPropW(window, InlineProperty);
}

void SetInlineComposition(bool enabled) noexcept {
    const HWND next = enabled ? GetFocus() : nullptr;
    if (IsInlineCompositionWindow(inlineWindow)) {
        if (next == inlineWindow) {
            return;
        }
        if (HIMC context = ImmGetContext(inlineWindow)) {
            ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
            ImmReleaseContext(inlineWindow, context);
        }
        RemovePropW(inlineWindow, InlineProperty);
    }
    inlineWindow = nullptr;
    if (next && SetPropW(next, InlineProperty, next)) {
        inlineWindow = next;
    }
}

void MoveCompositionWindow(int32 x, int32 y) noexcept {
    if (!IsInlineCompositionWindow(inlineWindow)) {
        return;
    }
    if (HIMC context = ImmGetContext(inlineWindow)) {
        CANDIDATEFORM candidates{};
        candidates.dwStyle = CFS_CANDIDATEPOS;
        candidates.ptCurrentPos = {x, y};
        ImmSetCandidateWindow(context, &candidates);
        ImmReleaseContext(inlineWindow, context);
    }
}

bool RouteInlineComposition(HWND window, LPARAM flags, Allocator& allocator,
                            input::InputQueue& queue) noexcept {
    HIMC context = ImmGetContext(window);
    if (!context) {
        return false;
    }
    ROCKS_ON_SCOPE_EXIT {
        ImmReleaseContext(window, context);
    };
    // 동일 메시지에서 직전 조합을 확정하고 새 음절 조합을 시작할 수 있다.
    // 둘 다 읽은 뒤 순서대로 큐에 넣어 오류 시 기본 처리와 중복 입력되지 않게 한다.
    const bool commits = (flags & GCS_RESULTSTR) != 0;
    const bool updates = (flags & (GCS_COMPSTR | GCS_CURSORPOS)) != 0;
    input::InputEvent commit, update;
    if (commits) {
        auto value = ReadComposition(context, GCS_RESULTSTR, allocator);
        if (!value) {
            return false;
        }
        commit = Move(value.Value());
    }
    if (updates) {
        auto value = ReadComposition(context, GCS_COMPSTR, allocator);
        if (!value) {
            return false;
        }
        update = Move(value.Value());
    }
    if (commits) {
        queue.Push(commit);
    }
    if (updates) {
        queue.Push(update);
    }
    if (!flags) {
        queue.Push(input::InputEvent::Composition());
    }
    return commits || updates || !flags;
}

} // namespace rocks::platform
