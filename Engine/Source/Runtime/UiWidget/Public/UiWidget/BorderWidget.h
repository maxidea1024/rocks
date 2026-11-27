#pragma once

#include <UiWidget/ContentWidget.h>

namespace rocks::ui {

// 배경색을 그리고 그 안에 자식 하나를 배치한다. SetTint는 이미 표시 중인 배경에도 적용된다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API BorderWidget final : public ContentWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit BorderWidget(Allocator& allocator) noexcept;
    Result<void> SetTint(LinearColor value) noexcept;
    Result<void> SetBrush(const Brush& brush) noexcept;
    const Brush& GetBrush() const noexcept;
    Result<void> SetPadding(Vec4 padding) noexcept;
    Vec4 Padding() const noexcept;
    LinearColor Tint() const noexcept;

protected:
    TypeId SlotType() const noexcept override;
    Result<SharedPtr<Element>> CreatePanel(Allocator& allocator) noexcept override;
    void ApplyProperties(Element& element) noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "padding") Vec4 padding_ {};
    ROCKS_NATIVE() Brush brush_ {};
    ROCKS_PROPERTY(Name = "tint") LinearColor tint_ { 1, 1, 1, 1 };
};

} // namespace rocks::ui
