#pragma once

#include <UiCore/Styles/ToolTipStyle.h>
#include <UiFramework/Layout/Frame.h>
#include <UiFramework/Text/TextLabel.h>

namespace rocks::ui {

// 툴팁 상자다(Docs/Design/UiToolkit 9.1절 UW4t, UE SToolTip). 스타일 바탕과 여백 안에 내용을
// 담는다. 세 가지 중 하나로 만든다(둘 이상 주면 InvalidArgument):
// - text: 글자 툴팁. 레이블에 스타일 글자색과 줄바꿈 폭을 적용한다.
// - title·shortcut·description: 제목(굵게)과 그 오른쪽 단축키 표지, 아래 설명으로 짠 툴팁. UE는
//   에디터 문서 모듈의 SDocumentationToolTip에만 있다. 셋 중 하나 이상이면 된다.
// - content: 임의 부품. 자식은 스타일 글자색을 전경색으로 물려받는다.
// 레이블은 호출자가 스타일의 font(text·title·shortcut·description)로 shaping해 넘긴다.
//
// 사용 예(Element::SetToolTipFactory 안에서):
//   auto label = shape("Save the current level", DefaultToolTipStyle().text.font);
//   auto tip = SharedPtr<ToolTip>::TryMake(allocator, allocator);
//   tip.Value()->Construct({.text = label.Value()});
//   return tip;   // 상자 하나가 툴팁 층에 놓인다
class ROCKS_UIFRAMEWORK_API ToolTip final : public Frame {
    ROCKS_UI_ELEMENT(ToolTip, Frame)

public:
    struct Args {
        SharedPtr<TextLabel> text{};
        SharedPtr<TextLabel> title{};
        SharedPtr<TextLabel> shortcut{};
        SharedPtr<TextLabel> description{};
        ElementChild content{};
        // 없으면 text일 때 DefaultToolTipStyle(), content일 때 ContentToolTipStyle()이다.
        const ToolTipStyle* style = nullptr;
        ElementArgs common{};
    };

    explicit ToolTip(Allocator& allocator) noexcept;
    Result<void> Construct(Args&& args) noexcept;

    // 바탕·여백·글자 색·줄바꿈 폭을 바꾼다. 여백이나 폭이 유한한 0 이상이 아니면 InvalidArgument다.
    Result<void> SetStyle(const ToolTipStyle& style) noexcept;
    const ToolTipStyle& GetStyle() const noexcept;

    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;
    // 자식이 물려받는 전경색은 스타일 글자색이다(밝은 바탕의 내용 툴팁도 읽히게 — UE SBorder
    // ForegroundColor).
    ColorRef GetForegroundColor() const noexcept override;

private:
    Result<void> ApplyTextStyle() noexcept;
    Result<SharedPtr<Element>> BuildParts(const Args& args, const ToolTipStyle& style) noexcept;

    ToolTipStyle style_{};
    SharedPtr<TextLabel> text_;
    SharedPtr<TextLabel> title_;
    SharedPtr<TextLabel> description_;
    SharedPtr<TextLabel> shortcutText_;
    SharedPtr<ToolTip> shortcut_;
    Allocator& allocator_;
};

} // namespace rocks::ui
