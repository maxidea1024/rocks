#pragma once

#include <UiWidget/ContentWidget.h>

namespace rocks::ui {

// 외부 위젯을 끼워 넣을 자리를 만든다. WidgetTree에서 이름을 지정하고 AddChild로 내용을 채운다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API NamedSlotWidget final : public ContentWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit NamedSlotWidget(Allocator& allocator) noexcept;

protected:
    Result<SharedPtr<Element>> CreatePanel(Allocator& allocator) noexcept override;
};

} // namespace rocks::ui
