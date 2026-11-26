#include <UiWidget/WidgetSwitcherWidgetSlot.generated.h>
#include <UiWidget/WidgetSwitcherWidget.h>
#include "MakeWidgetElement.h"
#include <UiFramework/Layout/Switcher.h>
#include <UiWidget/WidgetSlot.generated.h>

namespace rocks::ui {

TypeId WidgetSwitcherWidget::SlotType() const noexcept {
    return GeneratedType<WidgetSwitcherWidgetSlot>::Id();
}

WidgetSwitcherWidget::WidgetSwitcherWidget(Allocator& allocator) noexcept
    : PanelWidget(allocator), changed_(allocator) {
}

Result<SharedPtr<Element>> WidgetSwitcherWidget::CreatePanel(Allocator& allocator) noexcept {
    return MakeWidgetElement<Switcher>(allocator);
}

Result<void> WidgetSwitcherWidget::SetActiveIndex(uint32 index) noexcept {
    if (index >= ChildCount()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (activeIndex_ == index) {
        return Result<void>::Success();
    }

    activeIndex_ = index;
    if (auto live = CachedElement()) {
        ApplyProperties(*live);
    }

    changed_.Broadcast(activeIndex_);
    return Result<void>::Success();
}

MulticastDelegate<void(uint32)>& WidgetSwitcherWidget::OnActivePageChanged() noexcept {
    return changed_;
}

uint32 WidgetSwitcherWidget::ActiveIndex() const noexcept {
    return activeIndex_;
}

void WidgetSwitcherWidget::ApplyProperties(Element& element) noexcept {
    if (activeIndex_ < element.ChildCount()) {
        (void)static_cast<Switcher&>(element).SetActiveIndex(activeIndex_);
    }
}

Result<void> WidgetSwitcherWidget::RemoveChild(ObjectRegistry& objects,
                                               ObjectHandle child) noexcept {
    usize index = 0;
    for (; index < ChildCount(); ++index) {
        auto slot = objects.Resolve<WidgetSlot>(objects.OwnerAccess(), SlotAt(index));
        if (!slot) {
            return Result<void>::Failure(slot.Error());
        }

        if (slot.Value().Get().Value()->Content() == child) {
            break;
        }
    }

    auto removed = PanelWidget::RemoveChild(objects, child);
    if (removed && index <= activeIndex_) {
        if (activeIndex_) {
            --activeIndex_;
        }

        changed_.Broadcast(activeIndex_);
    }

    return removed;
}

} // namespace rocks::ui
