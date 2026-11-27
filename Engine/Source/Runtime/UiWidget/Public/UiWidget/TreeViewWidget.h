#pragma once

#include <UiWidget/ListViewWidget.h>

namespace rocks::ui {

// 모델의 키로 노드를 펼치고 닫는다. 표시 중인 행만 생성하고 접은 하위 노드의 행은 반납한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API TreeViewWidget final : public ListViewWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit TreeViewWidget(Allocator& allocator) noexcept;
    Result<void> SetExpanded(uint64 key, bool expanded) noexcept;
    Result<void> ExpandAll() noexcept;
    void CollapseAll() noexcept;
    bool IsExpanded(uint64 key) const noexcept;

protected:
    Result<SharedPtr<Element>>
    CreateView(Allocator& allocator, Application& app,
               const SharedRef<TableRowSource>& source) noexcept override;
};

} // namespace rocks::ui
