#pragma once

#include <UiWidget/WidgetSlot.h>

namespace rocks::ui {

// 머리와 본문은 자식 순서와 관계없이 역할을 보존한다. 본문만 있는 에셋도 저장할 수 있다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API ExpandableAreaSlot final : public WidgetSlot {
    ROCKS_GENERATED_BODY()
public:
    bool IsHeader() const noexcept;

private:
    friend class ExpandableAreaWidget;
    ROCKS_PROPERTY(ReadOnly, Name = "header") bool header_ = false;
};

} // namespace rocks::ui
