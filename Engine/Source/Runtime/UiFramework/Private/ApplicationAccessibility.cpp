#include <UiFramework/Application.h>

namespace rocks::ui {

Result<Array<AccessibleNode>> Application::AccessibleTree(uint32 user) const noexcept {
    Array<AccessibleNode> nodes(allocator_);
    if (!root_) {
        return Result<Array<AccessibleNode>>::Success(Move(nodes));
    }

    struct Pending {
        SharedPtr<Element> element;
        usize parent;
        bool enabled;
    };
    Array<Pending> pending(allocator_);
    auto start = pending.TryPushBack({root_, IndexNone, true});
    if (!start) {
        return Result<Array<AccessibleNode>>::Failure(start.Error());
    }

    auto focus = GetFocus(user);
    while (!pending.IsEmpty()) {
        auto current = Move(pending[pending.Size() - 1]);
        (void)pending.TryRemoveAt(pending.Size() - 1);
        auto& element = *current.element;
        if (element.GetVisibility() == Visibility::Hidden ||
            element.GetVisibility() == Visibility::Collapsed) {
            continue;
        }

        auto parent = current.parent;
        bool enabled = current.enabled && element.IsEnabled();
        if (element.IsAccessible()) {
            AccessibleNode node(allocator_);
            node.parent = parent;
            node.role = element.GetAccessibleRole();
            node.enabled = enabled;
            node.focusable = element.SupportsKeyboardFocus();
            node.focused = focus.Get() == &element;
            auto bounds = element.GetGeometry().WindowBounds();
            if (!bounds) {
                return Result<Array<AccessibleNode>>::Failure(bounds.Error());
            }

            node.bounds = bounds.Value();
            node.native = WeakPtr<Element>(current.element);
            auto name = element.GetAccessibleText(node.name);
            if (!name) {
                return Result<Array<AccessibleNode>>::Failure(name.Error());
            }

            auto summary = element.GetAccessibleText(node.summary, true);
            if (!summary) {
                return Result<Array<AccessibleNode>>::Failure(summary.Error());
            }

            parent = nodes.Size();
            auto stored = nodes.TryPushBack(Move(node));
            if (!stored) {
                return Result<Array<AccessibleNode>>::Failure(stored.Error());
            }
        }

        if (element.AllowsAccessibleChildren()) {
            auto children = element.ActiveChildren();
            for (usize i = children.Size(); i > 0; --i) {
                auto added = pending.TryPushBack({children[i - 1], parent, enabled});
                if (!added) {
                    return Result<Array<AccessibleNode>>::Failure(added.Error());
                }
            }
        }
    }

    return Result<Array<AccessibleNode>>::Success(Move(nodes));
}
} // namespace rocks::ui
