#pragma once

#include <UiWidget/ContentWidget.h>

namespace rocks::ui {

// 자식의 최소 크기를 보장한다. 상위 레이아웃은 이 크기를 자식 측정 결과와 함께 고려한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API SizeBoxWidget final : public ContentWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit SizeBoxWidget(Allocator& allocator) noexcept;
    Result<void> SetMinimumSize(Vec2 value) noexcept;
    Result<void> SetMaximumSize(Vec2 value) noexcept;
    // 한 축의 고정 크기를 지정한다. Clear 호출 시 자식의 자연 크기를 다시 사용한다.
    Result<void> SetWidthOverride(float width) noexcept;
    Result<void> SetHeightOverride(float height) noexcept;
    void ClearWidthOverride() noexcept;
    void ClearHeightOverride() noexcept;
    // 최솟값과 최댓값 중 0인 쪽은 제한을 해제한다. 비율은 너비/높이다.
    Result<void> SetAspectRatioRange(float minimum, float maximum) noexcept;
    Vec2 MinimumSize() const noexcept;

protected:
    TypeId SlotType() const noexcept override;
    Result<SharedPtr<Element>> CreatePanel(Allocator& allocator) noexcept override;
    void ApplyProperties(Element& element) noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "maximumSize")
    Vec2 maximumSize_{NumericLimits<float>::Max(), NumericLimits<float>::Max()};
    ROCKS_PROPERTY(ReadOnly, Name = "widthOverride") float widthOverride_ = -1;
    ROCKS_PROPERTY(ReadOnly, Name = "heightOverride") float heightOverride_ = -1;
    ROCKS_PROPERTY(ReadOnly, Name = "aspectRange") Vec2 aspectRange_ {};
    ROCKS_PROPERTY(Name = "minimumSize") Vec2 minimumSize_ {};
};

} // namespace rocks::ui
