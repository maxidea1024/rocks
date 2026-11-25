#include <Core/Containers/HashSet.h>
#include <UiWidget/PanelWidget.generated.h>
#include <UiWidget/UserWidget.generated.h>
#include <UiWidget/Widget.generated.h>
#include <UiWidget/WidgetSlot.generated.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

Result<void> WidgetTree::ForEachWidget(ObjectRegistry& objects, const Visitor& visitor,
                                       bool includeNested) const noexcept {
    if (!visitor.IsBound()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    if (RootWidget().IsNull()) {
        return Result<void>::Success();
    }
    struct Edge {
        ObjectHandle widget;
        ObjectHandle slot;
        ObjectHandle tree;
    };
    auto& allocator = objects.ObjectAllocator();
    const auto access = objects.OwnerAccess();
    Array<Edge> stack(allocator);
    HashSet<ObjectHandle> visited(allocator);
    auto added = stack.TryPushBack({RootWidget(), {}, Handle()});
    if (!added) {
        return added;
    }
    while (!stack.IsEmpty()) {
        const auto edge = stack.TryPopBack().Value();
        auto outer = objects.GetOuter(access, edge.widget);
        if (!outer || outer.Value() != edge.tree) {
            return Result<void>::Failure(outer ? ErrorCode::WrongContext : outer.Error());
        }
        auto borrowed = objects.Resolve<Widget>(access, edge.widget);
        if (!borrowed) {
            return Result<void>::Failure(borrowed.Error());
        }
        const auto* widget = borrowed.Value().Get().Value();
        if (widget->Slot() != edge.slot || widget->StableId() == Guid{}) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        auto seen = visited.Add(edge.widget);
        if (!seen || seen.Value() != InsertOutcome::Inserted) {
            return Result<void>::Failure(seen ? ErrorCode::CycleDetected : seen.Error());
        }
        auto next = visitor.Execute(edge.widget);
        if (!next || !next.Value()) {
            return next ? Result<void>::Success() : Result<void>::Failure(next.Error());
        }
        if (includeNested) {
            auto screen = objects.Resolve<UserWidget>(access, edge.widget);
            if (screen) {
                auto nested = screen.Value().Get().Value()->Tree();
                if (!nested.IsNull()) {
                    auto tree = objects.Resolve<WidgetTree>(access, nested);
                    if (!tree) {
                        return Result<void>::Failure(tree.Error());
                    }
                    const auto root = tree.Value().Get().Value()->RootWidget();
                    if (!root.IsNull()) {
                        added = stack.TryPushBack({root, {}, nested});
                        if (!added) {
                            return added;
                        }
                    }
                }
            } else if (screen.Error() != ErrorCode::TypeMismatch) {
                return Result<void>::Failure(screen.Error());
            }
        }
        auto panel = objects.Resolve<PanelWidget>(access, edge.widget);
        if (!panel) {
            if (panel.Error() == ErrorCode::TypeMismatch) {
                continue;
            }
            return Result<void>::Failure(panel.Error());
        }
        const auto* parent = panel.Value().Get().Value();
        for (usize i = parent->ChildCount(); i > 0; --i) {
            const auto slot = parent->SlotAt(i - 1);
            auto slotOuter = objects.GetOuter(access, slot);
            if (!slotOuter || slotOuter.Value() != edge.widget) {
                return Result<void>::Failure(slotOuter ? ErrorCode::WrongContext
                                                       : slotOuter.Error());
            }
            auto link = objects.Resolve<WidgetSlot>(access, slot);
            if (!link) {
                return Result<void>::Failure(link.Error());
            }
            added = stack.TryPushBack({link.Value().Get().Value()->Content(), slot, edge.tree});
            if (!added) {
                return added;
            }
        }
    }
    return Result<void>::Success();
}

Result<Array<ObjectHandle>> WidgetTree::GetAllWidgets(ObjectRegistry& objects,
                                                      bool includeNested) const noexcept {
    Array<ObjectHandle> widgets(objects.ObjectAllocator());
    auto visited = ForEachWidget(objects, Visitor::CreateLambda([&](ObjectHandle widget) noexcept {
                                     auto added = widgets.TryPushBack(widget);
                                     return added ? Result<bool>::Success(true)
                                                  : Result<bool>::Failure(added.Error());
                                 }),
                                 includeNested);
    return visited ? Result<Array<ObjectHandle>>::Success(Move(widgets))
                   : Result<Array<ObjectHandle>>::Failure(visited.Error());
}

Result<HashMap<Guid, ObjectHandle>>
WidgetTree::IndexWidgets(ObjectRegistry& objects) const noexcept {
    using Index = HashMap<Guid, ObjectHandle>;
    Index index(objects.ObjectAllocator());
    auto visited =
        ForEachWidget(objects, Visitor::CreateLambda([&](ObjectHandle handle) noexcept {
                          auto widget = objects.Resolve<Widget>(objects.OwnerAccess(), handle);
                          if (!widget) {
                              return Result<bool>::Failure(widget.Error());
                          }
                          auto inserted =
                              index.Insert(widget.Value().Get().Value()->StableId(), handle);
                          if (!inserted || inserted.Value() != InsertOutcome::Inserted) {
                              return Result<bool>::Failure(inserted ? ErrorCode::AlreadyExists
                                                                    : inserted.Error());
                          }
                          return Result<bool>::Success(true);
                      }));
    return visited ? Result<Index>::Success(Move(index)) : Result<Index>::Failure(visited.Error());
}

Result<ObjectHandle> WidgetTree::FindWidget(ObjectRegistry& objects, Guid stableId) const noexcept {
    auto index = IndexWidgets(objects);
    if (!index) {
        return Result<ObjectHandle>::Failure(index.Error());
    }
    const auto* handle = index.Value().Find(stableId);
    return handle ? Result<ObjectHandle>::Success(*handle)
                  : Result<ObjectHandle>::Failure(ErrorCode::NotFound);
}

Result<ObjectHandle> WidgetTree::FindWidget(ObjectRegistry& objects,
                                            StringView name) const noexcept {
    ObjectHandle found;
    String candidate(objects.ObjectAllocator());
    auto visited = ForEachWidget(objects, Visitor::CreateLambda([&](ObjectHandle handle) noexcept {
                                     // GetName은 뒤에 붙이므로 매번 비운다.
                                     candidate.Clear();
                                     auto read =
                                         objects.GetName(objects.OwnerAccess(), handle, candidate);
                                     if (!read) {
                                         return Result<bool>::Failure(read.Error());
                                     }
                                     if (EqualsIgnoringAsciiCase(candidate.View(), name)) {
                                         found = handle;
                                     }
                                     return Result<bool>::Success(found.IsNull());
                                 }));
    if (!visited) {
        return Result<ObjectHandle>::Failure(visited.Error());
    }
    return found.IsNull() ? Result<ObjectHandle>::Failure(ErrorCode::NotFound)
                          : Result<ObjectHandle>::Success(found);
}

Result<ObjectHandle> WidgetTree::FindWidget(ObjectRegistry& objects,
                                            const Element& element) const noexcept {
    ObjectHandle found;
    auto visited =
        ForEachWidget(objects, Visitor::CreateLambda([&](ObjectHandle handle) noexcept {
                          auto widget = objects.Resolve<Widget>(objects.OwnerAccess(), handle);
                          if (!widget) {
                              return Result<bool>::Failure(widget.Error());
                          }
                          if (widget.Value().Get().Value()->CachedElement().Get() == &element) {
                              found = handle;
                          }
                          return Result<bool>::Success(found.IsNull());
                      }));
    if (!visited) {
        return Result<ObjectHandle>::Failure(visited.Error());
    }
    return found.IsNull() ? Result<ObjectHandle>::Failure(ErrorCode::NotFound)
                          : Result<ObjectHandle>::Success(found);
}

Result<void> WidgetTree::RemoveWidget(ObjectRegistry& objects, ObjectHandle handle) noexcept {
    const auto access = objects.OwnerAccess();
    auto outer = objects.GetOuter(access, handle);
    if (!outer || outer.Value() != Handle()) {
        return Result<void>::Failure(outer ? ErrorCode::WrongContext : outer.Error());
    }
    if (RootWidget() == handle) {
        return SetRoot(objects, {});
    }
    auto widget = objects.ResolveForWrite<Widget>(access, handle);
    if (!widget) {
        return Result<void>::Failure(widget.Error());
    }
    auto* child = widget.Value().Get().Value();
    return child->Slot().IsNull() ? Result<void>::Failure(ErrorCode::NotFound)
                                  : child->RemoveFromParent(objects);
}

} // namespace rocks::ui
