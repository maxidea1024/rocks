#include <UiWidget/SafeZoneWidgetSlot.h>
#include <UiWidget/SafeZoneWidget.generated.h>

namespace rocks::ui {

Result<void> SafeZoneWidgetSlot::SetSafety(ObjectRegistry& objects, bool titleSafe,
                                           Vec4 scale) noexcept {
    auto outer = objects.GetOuter(objects.OwnerAccess(), Handle());
    if (!outer) {
        return Result<void>::Failure(outer.Error());
    }
    auto owner = objects.ResolveForWrite<SafeZoneWidget>(objects.OwnerAccess(), outer.Value());
    if (!owner) {
        return Result<void>::Failure(owner.Error());
    }
    auto applied = owner.Value().Get().Value()->UpdateSafety(Handle(), titleSafe, scale);
    if (!applied) {
        return applied;
    }
    titleSafe_ = titleSafe;
    safeAreaScale_ = scale;
    return Result<void>::Success();
}

Result<void> SafeZoneWidgetSlot::SetIsTitleSafe(ObjectRegistry& objects, bool enabled) noexcept {
    return SetSafety(objects, enabled, safeAreaScale_);
}

Result<void> SafeZoneWidgetSlot::SetSafeAreaScale(ObjectRegistry& objects, Vec4 scale) noexcept {
    return SetSafety(objects, titleSafe_, scale);
}

bool SafeZoneWidgetSlot::IsTitleSafe() const noexcept {
    return titleSafe_;
}

Vec4 SafeZoneWidgetSlot::SafeAreaScale() const noexcept {
    return safeAreaScale_;
}

} // namespace rocks::ui
