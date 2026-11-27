#pragma once
#include <UiWidget/WidgetSlot.h>
#include <UiCore/StackLayout.h>
#include <Core/Math/Vector4.h>

namespace rocks::ui {

// 스택의 주축 가중치와 크기 제한, 교차축 정렬을 저장한다. 주축의 기준 크기는 자식을 측정해 얻는다.
// 여백은 주축 시작·끝, 교차축 시작·끝 순서여서 패널의 방향을 바꾸어도 같은 배치 규칙을 쓴다.
ROCKS_CLASS(Abstract)
class ROCKS_UIWIDGET_API StackWidgetSlot : public WidgetSlot {
    ROCKS_GENERATED_BODY()
public:
    // 반환받은 슬롯에 새 배치 규칙을 적용한다. 이미 표시 중이면 다음 배치부터 반영한다.
    Result<void> SetLayout(ObjectRegistry& objects, const StackSlot& layout) noexcept;
    StackSlot Layout() const noexcept;

private:
    friend class StackPanelWidget;
    Result<void> Configure(const StackSlot& layout) noexcept;
    ROCKS_PROPERTY(ReadOnly, Name = "rule") uint8 rule_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "weight") float weight_ = 1;
    ROCKS_PROPERTY(ReadOnly, Name = "minSize") float minSize_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "maxSize") float maxSize_ = NumericLimits<float>::Max();
    ROCKS_PROPERTY(ReadOnly, Name = "padding") Vec4 padding_ {};
    ROCKS_PROPERTY(ReadOnly, Name = "alignment") uint8 alignment_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "mainAlignment") uint8 mainAlignment_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "shrinkWeight") float shrinkWeight_ = 1;
};
} // namespace rocks::ui
