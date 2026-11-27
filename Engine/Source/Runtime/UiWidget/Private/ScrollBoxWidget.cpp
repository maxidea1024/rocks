#include <UiWidget/ScrollBoxWidgetSlot.generated.h>
#include <UiWidget/ScrollBoxWidget.h>
#include <UiFramework/Layout/VerticalStack.h>
#include <UiFramework/Scrolling/ScrollView.h>

namespace rocks::ui {

Result<void> ScrollBoxWidget::ApplySlotSize(Element& panel, Element& child, StackSizeRule rule,
                                            float weight) noexcept {
    auto& stack = StaticElementCast<StackPanel>(ChildHost(panel));
    // 여백·정렬은 자식의 slot 배치라 슬롯의 직접 자식이 그 요소다(20.10절).
    if (child.Parent() != &stack) {
        return Result<void>::Failure(ErrorCode::WrongContext);
    }

    return stack.SetSlotLayout(child, {.rule = rule, .weight = weight});
}

Result<void> ScrollBoxWidget::AttachChild(ObjectRegistry& objects, ObjectHandle slot,
                                          Element& panel,
                                          const SharedPtr<Element>& child) noexcept {
    auto data = objects.Resolve<ScrollBoxWidgetSlot>(objects.OwnerAccess(), slot);
    if (!data) {
        return data.Error() == ErrorCode::TypeMismatch
                   ? PanelWidget::AttachChild(objects, slot, panel, child)
                   : Result<void>::Failure(data.Error());
    }

    const auto rule = data.Value().Get().Value()->SizeRule();
    const auto weight = data.Value().Get().Value()->FillWeight();
    if (rule > StackSizeRule::FillContent || !math::IsFinite(weight) || weight < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto attached = PanelWidget::AttachChild(objects, slot, panel, child);
    if (!attached) {
        return attached;
    }

    return ApplySlotSize(panel, *child, rule, weight);
}

TypeId ScrollBoxWidget::SlotType() const noexcept {
    return GeneratedType<ScrollBoxWidgetSlot>::Id();
}

ScrollBoxWidget::ScrollBoxWidget(Allocator& allocator) noexcept
    : PanelWidget(allocator), scrolled_(allocator) {
}

Element& ScrollBoxWidget::ChildHost(Element& panel) noexcept {
    return *panel.ChildAt(0);
}

Result<SharedPtr<Element>> ScrollBoxWidget::CreatePanel(Allocator& allocator) noexcept {
    ElementBuildScope scope(allocator);
    auto scroll = Build<ScrollView>({.content = Make<VerticalStack>({})});
    if (!scroll) {
        return Result<SharedPtr<Element>>::Failure(scroll.Error());
    }

    auto offset = scroll.Value()->SetScrollOffset(initialOffset_);
    if (!offset) {
        return Result<SharedPtr<Element>>::Failure(offset.Error());
    }

    return Result<SharedPtr<Element>>::Success(scroll.Value().AsSharedPtr());
}

Result<void> ScrollBoxWidget::SetScrollOffset(float offset) noexcept {
    if (!math::IsFinite(offset) || offset < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    endRequested_ = false;
    initialOffset_ = offset;
    if (auto live = CachedElement()) {
        return StaticElementCast<ScrollView>(*live).SetScrollOffset(offset);
    }

    return Result<void>::Success();
}

float ScrollBoxWidget::ScrollOffset() const noexcept {
    auto live = CachedElement();
    return live ? StaticElementCast<ScrollView>(*live).GetScrollOffset() : initialOffset_;
}

} // namespace rocks::ui
