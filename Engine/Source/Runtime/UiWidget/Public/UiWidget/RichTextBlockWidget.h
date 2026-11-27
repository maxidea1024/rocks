#pragma once

#include <UiWidget/RichTextDecorator.h>
#include <UiWidget/RichTextStyle.h>
#include <UiWidget/TextLayoutWidget.h>
#include <UiFramework/Text/TextShaper.h>

namespace rocks::ui {

// 퀘스트 설명이나 아이템 툴팁처럼 문장 안에서 색과 이미지를 섞을 때 쓴다.
// SetStyles에 등록한 이름은 <rarity>…</> 또는 <rarity>…</rarity>로 참조한다.
// <img id="coin"/> 같은 자체 닫힘 태그는 SetDecorator에 연결한 객체가 해석한다.
// 본문에는 &lt; &gt; &amp;를 쓸 수 있다. 마크업을 벗긴 한 문단을 함께 셰이핑하므로
// 색 경계에서 결합 글자나 아랍 문자 연결을 나누지 않는다. 경계가 클러스터 안에 있으면
// 그 클러스터 첫 바이트의 색을 쓴다. 문장이 비어 있지 않으면 WidgetTree의 셰이퍼가 필요하다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API RichTextBlockWidget final : public TextLayoutWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit RichTextBlockWidget(Allocator& allocator) noexcept;
    const Text& GetText() const noexcept;
    Result<void> SetText(const Text& markup) noexcept;
    Result<void> SetStyles(Span<const RichTextStyle> styles) noexcept;
    Result<void> SetDecorator(const SharedPtr<RichTextDecorator>& decorator) noexcept;
    Result<void> SetFontSize(float size) noexcept;
    // 보이는 마지막 줄의 끝에 같은 글꼴의 말줄임표를 표시한다. 접근성에는 원문을 제공한다.
    Result<void> SetEllipsis(bool enabled) noexcept;
    Result<void> SetTint(LinearColor tint) noexcept;
    // 로캘 또는 데코레이터 자원이 바뀌었을 때 다시 평가한다. 실패하면 기존 표시를 유지한다.
    Result<void> RefreshText() noexcept;
    void ReleaseElements() noexcept override;

protected:
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept override;

private:
    Result<void> Shape(TextLabel& target, const Text& markup, float size,
                       Span<const RichTextStyle> styles,
                       RichTextDecorator* decorator) const noexcept;

    ROCKS_PROPERTY(ReadOnly, Name = "text") Text text_;
    ROCKS_PROPERTY(ReadOnly, Name = "fontSize") float fontSize_ = 16;
    ROCKS_PROPERTY(Name = "tint") LinearColor tint_ { 1, 1, 1, 1 };
    ROCKS_PROPERTY(ReadOnly, Name = "styles") Array<RichTextStyle> styles_;
    ROCKS_PROPERTY(ReadOnly, Name = "ellipsis") bool ellipsis_ = false;
    ROCKS_NATIVE() Allocator& allocator_;
    ROCKS_NATIVE() SharedPtr<RichTextDecorator> decorator_;
    ROCKS_NATIVE() SharedPtr<TextShaper> shaper_;
};

} // namespace rocks::ui
