// Android의 clipboard·IME 조합 창·drag 이미지다. Android backend는 아직 이 기능을 구현하지 않아
// Unsupported로 끝난다(동작하는 척하지 않는다 — Docs/Design/UiToolkit 7.2절 PW 항목).
#include <PlatformWindow/Clipboard.h>
#include <PlatformWindow/InlineComposition.h>
#include <PlatformWindow/Window.h>

namespace rocks::platform {

Result<void> ReadClipboardText(String&) noexcept {
    return Result<void>::Failure(ErrorCode::Unsupported);
}

Result<void> WriteClipboardText(StringView) noexcept {
    return Result<void>::Failure(ErrorCode::Unsupported);
}

void SetInlineComposition(bool) noexcept {
}

void MoveCompositionWindow(int32, int32) noexcept {
}

Result<UniquePtr<DragImage>> DragImage::Create(Allocator&, const Window&, IntVec2, IntVec2,
                                               Span<const uint8>, uint8) noexcept {
    return Result<UniquePtr<DragImage>>::Failure(ErrorCode::Unsupported);
}

} // namespace rocks::platform
