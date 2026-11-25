#include <Core/Text/StringBuilder.h>
#include <Core/ScopeExit.h>
#include <UiWidget/PanelWidget.generated.h>
#include <UiWidget/WidgetSlot.generated.h>
#include <UiWidget/AlignedWidgetSlot.generated.h>
#include <UiWidget/Widget.generated.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

PanelWidget::PanelWidget(Allocator& allocator) noexcept : slots_(allocator) {
}

usize PanelWidget::MaxChildren() const noexcept {
    return MaxSize;
}

ObjectHandle PanelWidget::SlotAt(usize index) const noexcept {
    return index < slots_.Size() ? slots_.View()[index] : ObjectHandle{};
}

usize PanelWidget::ChildCount() const noexcept {
    return slots_.Size();
}

Result<SharedPtr<Element>> PanelWidget::BuildElement(ObjectRegistry& objects) noexcept {
    auto panel = CreatePanel(objects.ObjectAllocator());
    if (!panel) {
        return panel;
    }

    const auto access = objects.OwnerAccess();
    for (const auto handle : slots_.View()) {
        auto slot = objects.Resolve<WidgetSlot>(access, handle);
        if (!slot) {
            return Result<SharedPtr<Element>>::Failure(slot.Error());
        }

        auto content = objects.Resolve<Widget>(access, slot.Value().Get().Value()->Content());
        if (!content) {
            return Result<SharedPtr<Element>>::Failure(content.Error());
        }

        auto child = content.Value().Get().Value()->TakeElement(objects);
        if (!child) {
            return child;
        }

        auto added = AttachChild(objects, handle, *panel.Value(), child.Value());
        if (!added) {
            return Result<SharedPtr<Element>>::Failure(added.Error());
        }
    }

    registry_ = &objects;
    return panel;
}

Result<ObjectHandle> PanelWidget::AddChild(ObjectRegistry& objects, ObjectHandle handle) noexcept {
    return AddChildWithSlot(objects, handle, {});
}

Element& PanelWidget::ChildHost(Element& panel) noexcept {
    return panel;
}

TypeId PanelWidget::SlotType() const noexcept {
    return GeneratedType<WidgetSlot>::Id();
}

Result<ObjectHandle> PanelWidget::AddAlignedChild(ObjectRegistry& objects, ObjectHandle child,
                                                  const SlotPlacement& layout) noexcept {
    return AddChildWithSlot(
        objects, child,
        ConfigureSlot::CreateLambda([&layout](ObjectRegistry& registry, ObjectHandle id) noexcept {
            auto slot = registry.ResolveForWrite<AlignedWidgetSlot>(registry.OwnerAccess(), id);
            return slot ? slot.Value().Get().Value()->SetLayout(layout)
                        : Result<void>::Failure(slot.Error());
        }));
}

Result<void> PanelWidget::UpdateAlignedSlot(ObjectRegistry& objects, ObjectHandle, Element& panel,
                                            Element& child, const SlotPlacement& layout) noexcept {
    auto& host = ChildHost(panel);
    if (child.Parent() == &host) {
        if (layout.padding == Vec4{} && layout.horizontal == StackAlignment::Fill &&
            layout.vertical == StackAlignment::Fill) {
            return Result<void>::Success();
        }

        return SlotPresenter::WrapChild(objects.ObjectAllocator(), child, layout);
    }

    if (!child.Parent() || child.Parent()->Parent() != &host) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }

    static_cast<SlotPresenter&>(*child.Parent()).SetPlacement(layout);
    return Result<void>::Success();
}

Result<void> PanelWidget::AttachChild(ObjectRegistry& objects, ObjectHandle id, Element& panel,
                                      const SharedPtr<Element>& child) noexcept {
    auto slot = objects.Resolve<AlignedWidgetSlot>(objects.OwnerAccess(), id);
    if (!slot) {
        return slot.Error() == ErrorCode::TypeMismatch ? ChildHost(panel).TryAddChild(child)
                                                       : Result<void>::Failure(slot.Error());
    }

    const auto layout = slot.Value().Get().Value()->Layout();
    if (!layout.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (layout.padding == Vec4{} && layout.horizontal == StackAlignment::Fill &&
        layout.vertical == StackAlignment::Fill) {
        return ChildHost(panel).TryAddChild(child);
    }

    auto& allocator = objects.ObjectAllocator();
    auto presenter = SharedPtr<SlotPresenter>::TryMake(allocator, allocator, layout);
    if (!presenter) {
        return Result<void>::Failure(presenter.Error());
    }

    auto attached = presenter.Value()->TryAddChild(child);
    if (!attached) {
        return attached;
    }

    return ChildHost(panel).TryAddChild(presenter.Value());
}

Result<void> PanelWidget::ValidateNewChild(ObjectRegistry& objects,
                                           const Widget& child) const noexcept {
    const auto access = objects.OwnerAccess();
    const auto handle = child.Handle();
    if (!child.slot_.Handle().IsNull()) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    // 위젯은 모두 같은 WidgetTree가 outer이고, 슬롯은 각 패널이 outer다.
    auto tree = objects.GetOuter(access, Handle());
    auto childTree = objects.GetOuter(access, handle);
    if (!tree || !childTree || tree.Value() != childTree.Value() || tree.Value().IsNull()) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }

    auto owningTree = objects.Resolve<WidgetTree>(access, tree.Value());
    if (!owningTree || owningTree.Value().Get().Value()->RootWidget() == handle) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (ObjectHandle p = Handle(); !p.IsNull();) {
        if (p == handle) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        auto ancestor = objects.Resolve<Widget>(access, p);
        if (!ancestor) {
            return Result<void>::Failure(ancestor.Error());
        }

        const auto parentSlot = ancestor.Value().Get().Value()->slot_.Handle();
        if (parentSlot.IsNull()) {
            break;
        }

        auto parent = objects.GetOuter(access, parentSlot);
        if (!parent) {
            return Result<void>::Failure(parent.Error());
        }

        p = parent.Value();
    }

    return Result<void>::Success();
}

Result<ObjectHandle> PanelWidget::AddChildWithSlot(ObjectRegistry& objects, ObjectHandle handle,
                                                   ConfigureSlot configure) noexcept {
    if (exchanging_) {
        return Result<ObjectHandle>::Failure(ErrorCode::Busy);
    }

    if (ChildCount() >= MaxChildren()) {
        return Result<ObjectHandle>::Failure(ErrorCode::CapacityExceeded);
    }

    const auto access = objects.OwnerAccess();
    auto borrowed = objects.ResolveForWrite<Widget>(access, handle);
    if (!borrowed) {
        return Result<ObjectHandle>::Failure(borrowed.Error());
    }

    auto* child = borrowed.Value().Get().Value();
    auto valid = ValidateNewChild(objects, *child);
    if (!valid) {
        return Result<ObjectHandle>::Failure(valid.Error());
    }

    exchanging_ = true;
    const auto guard = MakeScopeExit([this]() noexcept { exchanging_ = false; });
    StringBuilder<> name(objects.ObjectAllocator());
    for (;;) {
        name.Clear();
        auto formatted = name.TryAppendFormat("Slot{}", nextSlot_++);
        if (!formatted) {
            return Result<ObjectHandle>::Failure(formatted.Error());
        }

        auto taken = objects.IsNameTaken(access, Handle(), name.View());
        if (!taken) {
            return Result<ObjectHandle>::Failure(taken.Error());
        }

        if (!taken.Value()) {
            break;
        }
    }

    auto made = objects.NewObject<WidgetSlot>(access, SlotType(), Handle(), name.View());
    if (!made) {
        return Result<ObjectHandle>::Failure(made.Error());
    }

    const auto slotHandle = made.Value().Handle();
    auto slot = objects.ResolveForWrite<WidgetSlot>(access, slotHandle);
    if (!slot) {
        return Result<ObjectHandle>::Failure(slot.Error());
    }

    if (configure) {
        auto configured = configure.Execute(objects, slotHandle);
        if (!configured) {
            return Result<ObjectHandle>::Failure(configured.Error());
        }
    }

    auto set =
        objects.SetReference(access, slotHandle, slot.Value().Get().Value()->content_, handle);
    if (!set) {
        return Result<ObjectHandle>::Failure(set.Error());
    }

    set = objects.AddReference(access, Handle(), slots_, slotHandle);
    if (!set) {
        return Result<ObjectHandle>::Failure(set.Error());
    }

    set = objects.SetReference(access, handle, child->slot_, slotHandle);
    if (auto panel = CachedElement(); set && panel) {
        auto element = child->TakeElement(objects);
        set = element ? AttachChild(objects, slotHandle, *panel, element.Value())
                      : Result<void>::Failure(element.Error());
    }

    if (!set) {
        (void)objects.SetReference(access, handle, child->slot_, {});
        (void)objects.RemoveReference(access, Handle(), slots_, slotHandle);
        return Result<ObjectHandle>::Failure(set.Error());
    }

    return Result<ObjectHandle>::Success(slotHandle);
}

Result<void> PanelWidget::RemoveChild(ObjectRegistry& objects, ObjectHandle handle) noexcept {
    if (exchanging_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    const auto access = objects.OwnerAccess();
    auto borrowed = objects.ResolveForWrite<Widget>(access, handle);
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }

    auto* child = borrowed.Value().Get().Value();
    const auto slot = child->slot_.Handle();
    exchanging_ = true;
    const auto guard = MakeScopeExit([this]() noexcept { exchanging_ = false; });
    auto removed = objects.RemoveReference(access, Handle(), slots_, slot);
    if (!removed) {
        return removed;
    }

    auto cleared = objects.SetReference(access, handle, child->slot_, {});
    if (!cleared) {
        (void)objects.AddReference(access, Handle(), slots_, slot);
        return cleared;
    }

    if (auto panel = CachedElement()) {
        if (auto element = child->CachedElement()) {
            // 슬롯이 배치용 컨테이너를 두는 패널에서는 실제 부모에서 자식을 분리한다.
            if (element->Parent() && element->IsWithin(*panel)) {
                auto* owner = element->Parent();
                auto& host = ChildHost(*panel);
                if (owner != &host && owner->Parent() == &host &&
                    objects.Resolve<AlignedWidgetSlot>(access, slot)) {
                    auto keepAlive = host.FindChild(owner);
                    (void)owner->RemoveChild(*element);
                    (void)host.RemoveChild(*keepAlive);
                } else {
                    (void)owner->RemoveChild(*element);
                }
            }
        }

        child->ReleaseElements();
    }

    return Result<void>::Success();
}

void PanelWidget::ReleaseElements() noexcept {
    if (registry_) {
        const auto access = registry_->OwnerAccess();
        for (const auto handle : slots_.View()) {
            auto slot = registry_->Resolve<WidgetSlot>(access, handle);
            if (!slot) {
                continue;
            }

            auto child = registry_->Resolve<Widget>(access, slot.Value().Get().Value()->Content());
            if (child) {
                child.Value().Get().Value()->ReleaseElements();
            }
        }
    }

    registry_ = nullptr;
    Widget::ReleaseElements();
}

} // namespace rocks::ui
