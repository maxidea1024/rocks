#include <UiWidget/WidgetSlot.generated.h>
#include <UiWidget/Widget.generated.h>

namespace rocks::ui {

ObjectHandle WidgetSlot::Content() const noexcept {
    return content_.Handle();
}

Result<ObjectHandle> WidgetSlot::Parent(ObjectRegistry& objects) const noexcept {
    if (Content().IsNull()) {
        return Result<ObjectHandle>::Success({});
    }

    auto child = objects.Resolve<Widget>(objects.OwnerAccess(), Content());
    if (!child) {
        return Result<ObjectHandle>::Failure(child.Error());
    }

    if (child.Value().Get().Value()->Slot() != Handle()) {
        return Result<ObjectHandle>::Success({});
    }

    return objects.GetOuter(objects.OwnerAccess(), Handle());
}

} // namespace rocks::ui
