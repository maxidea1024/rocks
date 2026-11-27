#pragma once

#include <UiWidget/Widget.h>

namespace rocks::ui {

// 그리지 않는 빈 영역이다. SetSize로 지정한 논리 크기를 상위 레이아웃 측정에 제공한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API SpacerWidget final : public Widget {
    ROCKS_GENERATED_BODY()
public:
    Result<void> SetSize(Vec2 value) noexcept;
    Vec2 Size() const noexcept;

protected:
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept override;
    void ApplyProperties(Element& element) noexcept override;

private:
    ROCKS_PROPERTY(Name = "size") Vec2 size_ {};
};

} // namespace rocks::ui
