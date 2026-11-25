#include <Base/NumericLimits.h>
#include <UiFramework/TreeView.h>

namespace rocks::ui {

TreeView::TreeView(Allocator& allocator, Application& app, TableRowSource& source) noexcept
    : ListView(allocator, app, source), expanded_(allocator), visible_(allocator) {
}

TreeView::TreeView(Allocator& allocator, Application& app,
                   const SharedRef<TableRowSource>& source) noexcept
    : ListView(allocator, app, source), expanded_(allocator), visible_(allocator) {
}

Result<void> TreeView::SetExpanded(uint64 key, bool expanded) noexcept {
    if (expanded == IsExpanded(key)) {
        return Result<void>::Success();
    }

    if (expanded) {
        auto added = expanded_.Add(key);
        if (!added) {
            return Result<void>::Failure(added.Error());
        }
    } else {
        expanded_.Erase(key);
    }

    Refresh();
    return Result<void>::Success();
}

bool TreeView::IsExpanded(uint64 key) const noexcept {
    return expanded_.Contains(key);
}

void TreeView::Refresh() noexcept {
    hierarchyDirty_ = true;
    ListView::Refresh();
}

uint32 TreeView::ItemCount() const noexcept {
    return static_cast<uint32>(visible_.Size());
}

TableItem TreeView::ItemAt(uint32 index) const noexcept {
    return visible_[index];
}

Result<void> TreeView::PrepareItems() noexcept {
    if (!hierarchyDirty_) {
        return Result<void>::Success();
    }

    return RebuildHierarchy(false);
}

Result<void> TreeView::ExpandAll() noexcept {
    auto result = RebuildHierarchy(true);
    if (result) {
        ListView::Refresh();
    }

    return result;
}

void TreeView::CollapseAll() noexcept {
    expanded_.Clear();
    Refresh();
}

Result<void> TreeView::RebuildHierarchy(bool expandAll) noexcept {
    struct Cursor {
        uint64 parent;
        uint32 next;
        uint32 count;
    };
    Array<Cursor> stack(allocator_);
    Array<TableItem> ordered(allocator_);
    HashSet<uint64> seen(allocator_);
    HashSet<uint64> allExpanded(allocator_);
    auto pushed = stack.TryPushBack({0, 0, source_.RootCount()});
    if (!pushed) {
        return pushed;
    }

    // 펼친 마디만 전위 순회한다. 순환 또는 중복 키가 있으면 이전에 표시하던 목록을 보존한다.
    while (!stack.IsEmpty()) {
        auto& cursor = stack[stack.Size() - 1];
        if (cursor.next == cursor.count) {
            (void)stack.TryRemoveAt(stack.Size() - 1);
            continue;
        }

        if (ordered.Size() == NumericLimits<uint32>::Max()) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }

        const auto depth = static_cast<uint32>(stack.Size() - 1);
        const auto index = cursor.next++;
        const uint64 key =
            depth == 0 ? source_.RootKey(index) : source_.ChildKey(cursor.parent, index);
        auto inserted = seen.Add(key);
        if (!inserted) {
            return Result<void>::Failure(inserted.Error());
        }

        if (inserted.Value() != InsertOutcome::Inserted) {
            return Result<void>::Failure(ErrorCode::AlreadyExists);
        }

        auto stored = ordered.TryPushBack({key, depth});
        if (!stored) {
            return stored;
        }

        if (expandAll || expanded_.Contains(key)) {
            const auto children = source_.ChildCount(key);
            if (children) {
                if (expandAll) {
                    auto marked = allExpanded.Add(key);
                    if (!marked) {
                        return Result<void>::Failure(marked.Error());
                    }
                }

                auto step = stack.TryPushBack({key, 0, children});
                if (!step) {
                    return step;
                }
            }
        }
    }

    if (expandAll) {
        expanded_ = Move(allExpanded);
    }

    visible_ = Move(ordered);
    hierarchyDirty_ = false;
    return Result<void>::Success();
}

} // namespace rocks::ui
