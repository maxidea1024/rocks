#include <UiWidget/WidgetFieldBindings.h>
#include <UiWidget/Widget.generated.h>
#include <UiWidget/UiWidget.module.generated.h>

namespace rocks::ui {

Result<void> RegisterWidgetTypes(ModuleRegistration& registration) noexcept {
    return generated::UiWidget::RegisterModuleTypes(registration);
}

Result<SharedPtr<Element>> Widget::TakeElement(ObjectRegistry& objects) noexcept {
    if (auto existing = cached_.Lock()) {
        return Result<SharedPtr<Element>>::Success(Move(existing));
    }
    if (rebuilding_) {
        return Result<SharedPtr<Element>>::Failure(ErrorCode::Busy);
    }
    rebuilding_ = true;
    auto built = BuildElement(objects);
    if (built) {
        cached_ = WeakPtr<Element>(built.Value());
        built.Value()->SetEnabled(enabled_);
        ApplyProperties(*built.Value());
        if (visibility_ <= static_cast<uint8>(Visibility::SelfHitTestInvisible)) {
            built.Value()->SetVisibility(static_cast<Visibility>(visibility_));
        }
        if (clipToBounds_ <= 1) {
            built.Value()->SetClipToBounds(clipToBounds_ != 0);
        }
        (void)built.Value()->SetRenderOpacity(renderOpacity_);
        auto affine = transform_.ToAffine();
        if (affine && affine.Value() != AffineTransform2D{}) {
            (void)built.Value()->SetRenderTransform(affine.Value(), renderPivot_);
        }
        if (cursorOverride_ <= static_cast<uint8>(input::MouseCursor::Wait)) {
            built.Value()->SetCursorOverride(static_cast<input::MouseCursor>(cursorOverride_));
        }
        built.Value()->SetForceVolatile(forceVolatile_);
        auto semantics = ApplySemantics(objects, *built.Value());
        if (!semantics) {
            ReleaseElements();
            rebuilding_ = false;
            return Result<SharedPtr<Element>>::Failure(semantics.Error());
        }
        OnRebuilt();
    }
    rebuilding_ = false;
    return built;
}

void Widget::ReleaseElements() noexcept {
    cached_ = {};
}
void Widget::ApplyProperties(Element&) noexcept {
}
void Widget::OnRebuilt() noexcept {
}
bool Widget::IsEnabled() const noexcept {
    return enabled_;
}
SharedPtr<Element> Widget::CachedElement() const noexcept {
    return cached_.Lock();
}

void Widget::SetEnabled(bool enabled) noexcept {
    if (enabled_ != enabled) {
        enabled_ = enabled;
        if (auto element = cached_.Lock()) {
            element->SetEnabled(enabled);
        }
        NotifyPropertyChanged("enabled");
    }
}

Result<void> Widget::SetVisibility(Visibility visibility) noexcept {
    if (visibility > Visibility::SelfHitTestInvisible) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    visibility_ = static_cast<uint8>(visibility);
    if (auto live = CachedElement()) {
        live->SetVisibility(visibility);
    }
    return Result<void>::Success();
}

Visibility Widget::GetVisibility() const noexcept {
    if (auto live = CachedElement()) {
        return live->GetVisibility();
    }
    return visibility_ <= static_cast<uint8>(Visibility::SelfHitTestInvisible)
               ? static_cast<Visibility>(visibility_)
               : Visibility::Visible;
}

void Widget::SetForceVolatile(bool enabled) noexcept {
    if (forceVolatile_ == enabled) {
        return;
    }
    forceVolatile_ = enabled;
    if (auto live = CachedElement()) {
        live->SetForceVolatile(enabled);
    }
    NotifyPropertyChanged("forceVolatile");
}

bool Widget::IsForceVolatile() const noexcept {
    return forceVolatile_;
}

void Widget::SetClipToBounds(bool enabled) noexcept {
    clipToBounds_ = enabled ? 1 : 0;
    if (auto live = CachedElement()) {
        live->SetClipToBounds(enabled);
    }
}

ObjectHandle Widget::Slot() const noexcept {
    return slot_.Handle();
}

Result<SharedPtr<WidgetFieldBindings>>
Widget::FieldNotifications(ObjectRegistry& objects) noexcept {
    if (!fieldNotifications_) {
        auto created = SharedPtr<WidgetFieldBindings>::TryMake(objects.ObjectAllocator(), objects);
        if (!created) {
            return Result<SharedPtr<WidgetFieldBindings>>::Failure(created.Error());
        }
        fieldNotifications_ = Move(created.Value());
    }
    if (&fieldNotifications_->Registry() != &objects) {
        return Result<SharedPtr<WidgetFieldBindings>>::Failure(ErrorCode::WrongContext);
    }
    return Result<SharedPtr<WidgetFieldBindings>>::Success(fieldNotifications_);
}

void Widget::NotifyPropertyChanged(StringView name) noexcept {
    if (fieldNotifications_) {
        (void)fieldNotifications_->NotifyNamed(Handle(), name);
    }
}

Guid Widget::StableId() const noexcept {
    return stableId_;
}

} // namespace rocks::ui
