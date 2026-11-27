#pragma once

#include <UiWidget/MultiLineEditableTextWidget.h>

namespace rocks::ui {

// 여러 줄을 편집할 수 있는 입력 상자다. 포커스를 잃으면 원문 전체를 확정한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API MultiLineEditableTextBoxWidget final : public MultiLineEditableTextWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit MultiLineEditableTextBoxWidget(Allocator& allocator) noexcept;

protected:
    bool HasBox() const noexcept override;
};

} // namespace rocks::ui
