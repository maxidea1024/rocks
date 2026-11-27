#pragma once

#include <Core/Math/Vector4.h>
#include <UiCore/Element.h>
#include <UiCore/Styles/StyleBrush.h>

namespace rocks::ui {

// 편집 상자의 오류 팝업 내용이다(Docs/Design/UiToolkit 22.6절 — UE SErrorText). 바탕 brush는 그릴
// 때 테마로 풀어 테마가 바뀌어도 따라간다(Frame은 푼 Brush만 받는다). 여백 안에 자식(오류 글자
// 레이블) 하나를 놓는다.
class EditableTextErrorPopup final : public Element {
    ROCKS_UI_ELEMENT(EditableTextErrorPopup, Element)

public:
    EditableTextErrorPopup(Allocator& allocator, const StyleBrush& background,
                           Vec4 padding) noexcept;

    Vec2 ComputeDesiredSize(float scale) const noexcept override;
    Result<void> Arrange(const Geometry& geometry) noexcept override;
    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;

private:
    StyleBrush background_;
    Vec4 padding_;
};

} // namespace rocks::ui
