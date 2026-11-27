#pragma once

#include <Core/Containers/HashSet.h>
#include <UiFramework/Lists/ListView.h>

namespace rocks::ui {

class ROCKS_UIFRAMEWORK_API TreeView final : public ListView {
    ROCKS_UI_ELEMENT(TreeView, ListView)

public:
    TreeView(Allocator& allocator, Application& app, TableRowSource& source) noexcept;
    TreeView(Allocator& allocator, Application& app,
             const SharedRef<TableRowSource>& source) noexcept;
    Result<void> SetExpanded(uint64 key, bool expanded) noexcept;
    Result<void> ExpandAll() noexcept;
    void CollapseAll() noexcept;
    bool IsExpanded(uint64 key) const noexcept;
    void Refresh() noexcept override;

protected:
    Result<void> PrepareItems() noexcept override;
    uint32 ItemCount() const noexcept override;
    TableItem ItemAt(uint32 index) const noexcept override;

private:
    Result<void> RebuildHierarchy(bool expandAll) noexcept;
    HashSet<uint64> expanded_;
    Array<TableItem> visible_;
    bool hierarchyDirty_ = true;
};

} // namespace rocks::ui
