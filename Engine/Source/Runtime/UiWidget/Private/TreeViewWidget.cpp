#include "MakeWidgetElement.h"
#include <UiWidget/TreeViewWidget.h>
#include <UiFramework/Lists/TreeView.h>

namespace rocks::ui {

TreeViewWidget::TreeViewWidget(Allocator& allocator) noexcept : ListViewWidget(allocator) {
}

Result<SharedPtr<Element>>
TreeViewWidget::CreateView(Allocator& allocator, Application& app,
                           const SharedRef<TableRowSource>& source) noexcept {
    return MakeWidgetElement<TreeView>(allocator, app, source);
}

Result<void> TreeViewWidget::SetExpanded(uint64 key, bool expanded) noexcept {
    auto live = CachedElement();
    return live ? StaticElementCast<TreeView>(*live).SetExpanded(key, expanded)
                : Result<void>::Failure(ErrorCode::NotReady);
}

Result<void> TreeViewWidget::ExpandAll() noexcept {
    auto live = CachedElement();
    return live ? StaticElementCast<TreeView>(*live).ExpandAll()
                : Result<void>::Failure(ErrorCode::NotReady);
}

void TreeViewWidget::CollapseAll() noexcept {
    if (auto live = CachedElement()) {
        StaticElementCast<TreeView>(*live).CollapseAll();
    }
}

bool TreeViewWidget::IsExpanded(uint64 key) const noexcept {
    auto live = CachedElement();
    return live && StaticElementCast<TreeView>(*live).IsExpanded(key);
}

} // namespace rocks::ui
