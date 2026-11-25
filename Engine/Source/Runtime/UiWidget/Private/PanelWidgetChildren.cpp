#include <UiWidget/PanelWidget.h>
#include <UiWidget/WidgetSlot.generated.h>
#include <UiWidget/Widget.generated.h>

namespace rocks::ui {
Result<ObjectHandle> PanelWidget::GetChildAt(ObjectRegistry& objects, usize index) const noexcept {
    if (index >= ChildCount()) {return Result<ObjectHandle>::Failure(ErrorCode::InvalidArgument);}
    auto slot = objects.Resolve<WidgetSlot>(objects.OwnerAccess(), SlotAt(index));
    return slot ? Result<ObjectHandle>::Success(slot.Value().Get().Value()->Content()) : Result<ObjectHandle>::Failure(slot.Error());
}
Result<Array<ObjectHandle>> PanelWidget::GetAllChildren(ObjectRegistry& objects) const noexcept {
    Array<ObjectHandle> result(objects.ObjectAllocator());
    auto reserved=result.TryReserve(ChildCount());
    if (!reserved) {return Result<Array<ObjectHandle>>::Failure(reserved.Error());}
    for (usize index=0; index<ChildCount(); ++index) {
        auto child=GetChildAt(objects,index);
        if (!child) {return Result<Array<ObjectHandle>>::Failure(child.Error());}
        (void)result.TryPushBack(child.Value());
    }
    return Result<Array<ObjectHandle>>::Success(Move(result));
}
Result<usize> PanelWidget::GetChildIndex(ObjectRegistry& objects, ObjectHandle child) const noexcept {
    auto widget=objects.Resolve<Widget>(objects.OwnerAccess(),child);
    if (!widget) {return Result<usize>::Failure(widget.Error());}
    const auto slot=widget.Value().Get().Value()->Slot();
    for (usize i=0; i<ChildCount(); ++i) {if (SlotAt(i)==slot) {return Result<usize>::Success(i);}}
    return Result<usize>::Failure(ErrorCode::NotFound);
}
bool PanelWidget::HasChild(ObjectRegistry& objects,ObjectHandle child) const noexcept {
    return static_cast<bool>(GetChildIndex(objects,child));
}
bool PanelWidget::HasAnyChildren() const noexcept {return ChildCount()!=0;}
Result<void> PanelWidget::RemoveChildAt(ObjectRegistry& objects,usize index) noexcept {
    auto child=GetChildAt(objects,index);
    return child ? RemoveChild(objects,child.Value()) : Result<void>::Failure(child.Error());
}
Result<void> PanelWidget::ClearChildren(ObjectRegistry& objects) noexcept {
    while (ChildCount()) {
        auto removed=RemoveChildAt(objects,ChildCount()-1);
        if (!removed) {return removed;}
    }
    return Result<void>::Success();
}
} // namespace rocks::ui
