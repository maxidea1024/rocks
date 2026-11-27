#pragma once

#include <UiWidget/EditableTextWidget.h>

namespace rocks::ui {

// 배경과 안쪽 여백을 가진 한 줄 입력란이다. 검색어나 이름을 받을 때 쓴다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API EditableTextBoxWidget final : public EditableTextWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit EditableTextBoxWidget(Allocator& allocator) noexcept;

protected:
    bool HasBox() const noexcept override;
};

} // namespace rocks::ui
