#include <UiWidget/ExpandableAreaWidget.generated.h>
#include <UiWidget/ExpandableAreaSlot.generated.h>
#include <UiFramework/Controls/Button.h>
#include <UiFramework/Layout/SizeBox.h>
#include <UiFramework/Layout/VerticalStack.h>

namespace rocks::ui {

namespace {

// 펼침 머리 버튼의 안쪽 여백이다.
constexpr Vec4 ExpanderHeaderPadding{4, 4, 4, 4};

} // namespace

ExpandableAreaWidget::ExpandableAreaWidget(Allocator& allocator) noexcept
    : PanelWidget(allocator), changed_(allocator) {
}

usize ExpandableAreaWidget::MaxChildren() const noexcept {
    return 2;
}

TypeId ExpandableAreaWidget::SlotType() const noexcept {
    return GeneratedType<ExpandableAreaSlot>::Id();
}

Result<ObjectHandle> ExpandableAreaWidget::AddSection(ObjectRegistry& objects, ObjectHandle child,
                                                      bool header) noexcept {
    for (usize i = 0; i < ChildCount(); ++i) {
        auto slot = objects.Resolve<ExpandableAreaSlot>(objects.OwnerAccess(), SlotAt(i));
        if (!slot) {
            return Result<ObjectHandle>::Failure(slot.Error());
        }

        if (slot.Value().Get().Value()->IsHeader() == header) {
            return Result<ObjectHandle>::Failure(ErrorCode::AlreadyExists);
        }
    }

    return AddChildWithSlot(
        objects, child,
        ConfigureSlot::CreateLambda([header](ObjectRegistry& registry, ObjectHandle id) noexcept {
            auto slot = registry.ResolveForWrite<ExpandableAreaSlot>(registry.OwnerAccess(), id);
            if (!slot) {
                return Result<void>::Failure(slot.Error());
            }

            slot.Value().Get().Value()->header_ = header;
            return Result<void>::Success();
        }));
}

Result<ObjectHandle> ExpandableAreaWidget::AddHeader(ObjectRegistry& objects,
                                                     ObjectHandle child) noexcept {
    return AddSection(objects, child, true);
}

Result<ObjectHandle> ExpandableAreaWidget::AddBody(ObjectRegistry& objects,
                                                   ObjectHandle child) noexcept {
    return AddSection(objects, child, false);
}

Result<ObjectHandle> ExpandableAreaWidget::AddChild(ObjectRegistry& objects,
                                                    ObjectHandle child) noexcept {
    if (ChildCount() == 0) {
        return AddHeader(objects, child);
    }

    auto slot = objects.Resolve<ExpandableAreaSlot>(objects.OwnerAccess(), SlotAt(0));
    return slot ? AddSection(objects, child, !slot.Value().Get().Value()->IsHeader())
                : Result<ObjectHandle>::Failure(slot.Error());
}

Result<SharedPtr<Element>> ExpandableAreaWidget::CreatePanel(Allocator& allocator) noexcept {
    auto body = SharedPtr<SizeBox>::TryMake(allocator, allocator);
    if (!body) {
        return Result<SharedPtr<Element>>::Failure(body.Error());
    }

    ElementBuildScope scope(allocator);
    const StackPanel::SlotArgs slots[] = {
        {.content = Make<Button>(
             {.style = NoBorderButtonStyle(), .contentPadding = ExpanderHeaderPadding})},
        {.content = body.Value()},
    };
    auto root = Build<VerticalStack>({.slots = slots});
    if (!root) {
        return Result<SharedPtr<Element>>::Failure(root.Error());
    }

    return Result<SharedPtr<Element>>::Success(root.Value().AsSharedPtr());
}

Result<SharedPtr<Element>> ExpandableAreaWidget::BuildElement(ObjectRegistry& objects) noexcept {
    auto root = PanelWidget::BuildElement(objects);
    if (!root) {
        return root;
    }

    auto* panel = root.Value().Get();
    auto& header = StaticElementCast<Button>(*panel->ChildAt(0));
    header.SetOnClicked(Delegate<void()>::CreateLambda([&objects, owner = Handle(),
                                                        panel]() noexcept {
        auto widget = objects.ResolveForWrite<ExpandableAreaWidget>(objects.OwnerAccess(), owner);
        if (widget) {
            auto* area = widget.Value().Get().Value();
            if (area->CachedElement().Get() == panel) {
                area->SetExpanded(!area->IsExpanded());
                // 사용자가 머리를 눌렀다(UE SExpandableArea OnAreaExpansionChanged).
                area->changed_.Broadcast({objects, objects.OwnerAccess(), owner},
                                         area->IsExpanded());
            }
        }
    }));
    return root;
}

Result<void> ExpandableAreaWidget::AttachChild(ObjectRegistry& objects, ObjectHandle id,
                                               Element& panel,
                                               const SharedPtr<Element>& child) noexcept {
    auto slot = objects.Resolve<ExpandableAreaSlot>(objects.OwnerAccess(), id);
    return slot ? panel.ChildAt(slot.Value().Get().Value()->IsHeader() ? 0 : 1)->TryAddChild(child)
                : Result<void>::Failure(slot.Error());
}

void ExpandableAreaWidget::ApplyProperties(Element& element) noexcept {
    element.ChildAt(1)->SetVisibility(expanded_ ? Visibility::SelfHitTestInvisible
                                                : Visibility::Collapsed);
}

bool ExpandableAreaWidget::IsExpanded() const noexcept {
    return expanded_;
}

void ExpandableAreaWidget::SetExpanded(bool expanded) noexcept {
    if (expanded_ == expanded) {
        return;
    }

    expanded_ = expanded;
    if (auto live = CachedElement()) {
        ApplyProperties(*live);
    }

    // 프로그램 변경은 필드 통지만 한다(UE UExpandableArea::SetIsExpanded — 21.5a절).
    NotifyPropertyChanged("expanded");
}

Event<bool>& ExpandableAreaWidget::OnExpansionChanged() noexcept {
    return changed_;
}

} // namespace rocks::ui
