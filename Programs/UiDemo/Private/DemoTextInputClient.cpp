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
