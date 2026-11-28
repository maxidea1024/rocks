#include "DemoTextInputClient.h"

namespace rocks::ui {

bool DemoTextInputClient::AllowsInputMethod() const noexcept {
    return context && context->AllowsInputMethod();
}

void DemoTextInputClient::GetSelection(uint32& begin, uint32& length) const noexcept {
    begin = 0;
    length = 0;
    if (context) {
        context->GetSelection(begin, length);
    }
}

uint32 DemoTextInputClient::GetTextLength() const noexcept {
    return context ? context->GetTextLength() : 0;
}

Result<void> DemoTextInputClient::GetText(uint32 begin, uint32 length,
                                          Array<char16_t>& out) const noexcept {
    if (!context) {
        return length == 0 && begin == 0 ? Result<void>::Success()
                                         : Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return context->GetText(begin, length, out);
}

void DemoTextInputClient::SetSelection(uint32 begin, uint32 length) noexcept {
    if (context) {
        context->SetSelection(begin, length);
    }
}

void DemoTextInputClient::SetText(uint32 begin, uint32 length, Span<const char16_t> text) noexcept {
    if (context) {
        context->SetText(begin, length, text);
    }
}

bool DemoTextInputClient::GetTextBounds(uint32 begin, uint32 length,
                                        Box2<float>& client) const noexcept {
    // UI 창 좌표가 client 물리 픽셀이다(19.2절 DPI).
    return context && context->GetTextBounds(begin, length, client);
}

bool DemoTextInputClient::GetScreenBounds(Box2<float>& client) const noexcept {
    return context && context->GetScreenBounds(client);
}

Optional<uint32> DemoTextInputClient::GetCharacterIndexFromPoint(Vec2 client) const noexcept {
    return context ? context->GetCharacterIndexFromPoint(client) : Optional<uint32>{};
}

void DemoTextInputClient::BeginComposition() noexcept {
    if (context) {
        context->BeginComposition();
    }
}

void DemoTextInputClient::UpdateCompositionRange(uint32 begin, uint32 length) noexcept {
    if (context) {
        context->UpdateCompositionRange(begin, length);
    }
}

void DemoTextInputClient::EndComposition() noexcept {
    if (context) {
        context->EndComposition();
    }
}

} // namespace rocks::ui
