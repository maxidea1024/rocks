#pragma once

#include <PlatformWindow/TextInputClient.h>
#include <UiCore/TextInputContext.h>

namespace rocks::ui {

// 활성 UI 문맥을 플랫폼 문맥으로 보이는 어댑터다(Docs/Design/UiToolkit 19.2절 — DemoTextInput이
// 창마다 하나 둔다). 문맥이 없으면 빈 글자로 답한다. UI 창 좌표가 client 물리 픽셀이라 사각형은
// 바꾸지 않는다.
class DemoTextInputClient final : public platform::TextInputClient {
public:
    TextInputContext* context = nullptr;

    bool AllowsInputMethod() const noexcept override;
    void GetSelection(uint32& begin, uint32& length) const noexcept override;
    uint32 GetTextLength() const noexcept override;
    Result<void> GetText(uint32 begin, uint32 length, Array<char16_t>& out) const noexcept override;
    void SetSelection(uint32 begin, uint32 length) noexcept override;
    void SetText(uint32 begin, uint32 length, Span<const char16_t> text) noexcept override;
    bool GetTextBounds(uint32 begin, uint32 length, Box2<float>& client) const noexcept override;
    bool GetScreenBounds(Box2<float>& client) const noexcept override;
    Optional<uint32> GetCharacterIndexFromPoint(Vec2 client) const noexcept override;
    void BeginComposition() noexcept override;
    void UpdateCompositionRange(uint32 begin, uint32 length) noexcept override;
    void EndComposition() noexcept override;
};

} // namespace rocks::ui
