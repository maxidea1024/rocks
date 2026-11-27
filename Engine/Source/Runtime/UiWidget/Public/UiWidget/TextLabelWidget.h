#pragma once

#include <UiWidget/TextLayoutWidget.h>
#include <UiFramework/Text/TextShaper.h>

namespace rocks::ui {

// 지역화 가능한 Text와 논리 글꼴 크기를 애셋에 저장한다. 화면을 열기 전에
// WidgetTree::SetTextShaper로 해당 화면의 글꼴 서비스를 지정한다. 서비스가 준비되지 않은
// 상태에서 비어 있지 않은 텍스트를 그리려 하면 NotReady를 반환한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API TextLabelWidget final : public TextLayoutWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit TextLabelWidget(Allocator& allocator) noexcept;
    Result<void> SetText(const Text& text) noexcept;
    const Text& GetText() const noexcept;
    Result<void> SetFontSize(float size) noexcept;
    // 보이는 마지막 줄의 끝에 같은 글꼴의 말줄임표를 표시한다. 접근성에는 원문을 제공한다.
    Result<void> SetEllipsis(bool enabled) noexcept;
    Result<void> SetTint(LinearColor tint) noexcept;
    // 로캘이나 글꼴 아틀라스가 바뀌었을 때 같은 원문을 새로 셰이핑한다.
    Result<void> RefreshText() noexcept;
    void ReleaseElements() noexcept override;

protected:
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept override;

private:
    Result<void> Shape(TextLabel& label, const Text& text, float size) const noexcept;
    ROCKS_PROPERTY(ReadOnly, Name = "text") Text text_;
    ROCKS_PROPERTY(ReadOnly, Name = "fontSize") float fontSize_ = 16;
    ROCKS_PROPERTY(Name = "tint") LinearColor tint_ { 1, 1, 1, 1 };
    ROCKS_PROPERTY(ReadOnly, Name = "ellipsis") bool ellipsis_ = false;
    ROCKS_NATIVE() Allocator& allocator_;
    ROCKS_NATIVE() SharedPtr<TextShaper> shaper_;
};

} // namespace rocks::ui
