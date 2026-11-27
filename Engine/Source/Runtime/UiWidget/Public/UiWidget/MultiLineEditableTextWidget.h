#pragma once

#include <UiWidget/EditableTextWidget.h>

namespace rocks::ui {

// Enter를 줄바꿈으로 입력하고 문단 단위 텍스트를 편집한다. 할당된 폭에 맞춰 자동 줄바꿈한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API MultiLineEditableTextWidget : public EditableTextWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit MultiLineEditableTextWidget(Allocator& allocator) noexcept;

protected:
    bool IsMultiline() const noexcept override;
};

} // namespace rocks::ui
