#include <UiWidget/SafeZoneWidgetSlot.generated.h>
#include <UiWidget/SafeZoneWidget.h>
#include <UiFramework/Layout/Frame.h>

namespace rocks::ui {

namespace {

bool ValidInsets(Vec4 insets) noexcept {
    return insets.IsFinite() && insets.x >= 0 && insets.y >= 0 && insets.z >= 0 && insets.w >= 0;
}
} // namespace

Result<void> SafeZoneWidget::UpdateSafety(ObjectHandle slot, bool titleSafe, Vec4 scale) noexcept {
    if (slot != SlotAt(0)) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    if (!ValidInsets(scale)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!(titleInsets_ * scale).IsFinite() || !(actionInsets_ * scale).IsFinite()) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    titleSafe_ = titleSafe;
    scale_ = scale;
    if (auto live = CachedElement()) {
        ApplyProperties(*live);
    }

    return Result<void>::Success();
}

Result<void> SafeZoneWidget::AttachChild(ObjectRegistry& objects, ObjectHandle slot, Element& panel,
                                         const SharedPtr<Element>& child) noexcept {
    auto data = objects.Resolve<SafeZoneWidgetSlot>(objects.OwnerAccess(), slot);
    if (!data && data.Error() != ErrorCode::TypeMismatch) {
        return Result<void>::Failure(data.Error());
    }

    const bool titleSafe = !data || data.Value().Get().Value()->IsTitleSafe();
    const Vec4 scale = data ? data.Value().Get().Value()->SafeAreaScale() : Vec4{1, 1, 1, 1};
    if (!ValidInsets(scale) || !(titleInsets_ * scale).IsFinite() ||
        !(actionInsets_ * scale).IsFinite()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto attached = PanelWidget::AttachChild(objects, slot, panel, child);
    if (!attached) {
        return attached;
    }

    titleSafe_ = titleSafe;
    scale_ = scale;
    ApplyProperties(panel);
    return Result<void>::Success();
}

TypeId SafeZoneWidget::SlotType() const noexcept {
    return GeneratedType<SafeZoneWidgetSlot>::Id();
}

SafeZoneWidget::SafeZoneWidget(Allocator& allocator) noexcept : ContentWidget(allocator) {
}

Result<SharedPtr<Element>> SafeZoneWidget::CreatePanel(Allocator& allocator) noexcept {
    ElementBuildScope scope(allocator);
    auto frame = Build<Frame>({.brush = {.mode = BrushMode::None}});
    if (!frame) {
        return Result<SharedPtr<Element>>::Failure(frame.Error());
    }

    return Result<SharedPtr<Element>>::Success(frame.Value().AsSharedPtr());
}

Result<void> SafeZoneWidget::SetSafeInsets(Vec4 insets) noexcept {
    return SetSafeInsets(insets, insets);
}

Result<void> SafeZoneWidget::SetSafeInsets(Vec4 titleInsets, Vec4 actionInsets) noexcept {
    if (!ValidInsets(titleInsets) || !ValidInsets(actionInsets)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!(titleInsets * scale_).IsFinite() || !(actionInsets * scale_).IsFinite()) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    titleInsets_ = titleInsets;
    actionInsets_ = actionInsets;
    if (auto live = CachedElement()) {
        ApplyProperties(*live);
    }

    return Result<void>::Success();
}

void SafeZoneWidget::SetSidesToPad(bool left, bool top, bool right, bool bottom) noexcept {
    left_ = left;
    top_ = top;
    right_ = right;
    bottom_ = bottom;
    if (auto live = CachedElement()) {
        ApplyProperties(*live);
    }
}

void SafeZoneWidget::ApplyProperties(Element& element) noexcept {
    const Vec4 insets = (titleSafe_ ? titleInsets_ : actionInsets_) * scale_;
    (void)StaticElementCast<Frame>(element).SetPadding(
        {left_ ? insets.x : 0, top_ ? insets.y : 0, right_ ? insets.z : 0, bottom_ ? insets.w : 0});
}

} // namespace rocks::ui
