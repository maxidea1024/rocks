#include <UiWidget/UserWidget.h>
#include <UiWidget/WidgetAnimation.generated.h>

namespace rocks::ui {

Result<ObjectHandle> UserWidget::FindAnimation(ObjectRegistry& objects, Guid id) const noexcept {
    for (auto handle : animations_.View()) {
        auto animation = objects.Resolve<WidgetAnimation>(objects.OwnerAccess(), handle);
        if (!animation) {
            return Result<ObjectHandle>::Failure(animation.Error());
        }
        if (animation.Value().Get().Value()->StableId() == id) {
            return Result<ObjectHandle>::Success(handle);
        }
    }
    return Result<ObjectHandle>::Failure(ErrorCode::NotFound);
}

Result<ObjectHandle> UserWidget::FindAnimation(ObjectRegistry& objects,
                                               StringView name) const noexcept {
    String stored(objects.ObjectAllocator());
    for (auto handle : animations_.View()) {
        auto read = objects.GetName(objects.OwnerAccess(), handle, stored);
        if (!read) {
            return Result<ObjectHandle>::Failure(read.Error());
        }
        if (EqualsIgnoringAsciiCase(stored.View(), name)) {
            return Result<ObjectHandle>::Success(handle);
        }
    }
    return Result<ObjectHandle>::Failure(ErrorCode::NotFound);
}

Result<SharedPtr<UserWidgetAnimation>>
UserWidget::PlayAnimation(ObjectRegistry& objects, Application& app, Guid id,
                          WidgetAnimationPlayback options) noexcept {
    auto found = FindAnimation(objects, id);
    if (!found) {
        return Result<SharedPtr<UserWidgetAnimation>>::Failure(found.Error());
    }
    auto clip = objects.Resolve<WidgetAnimation>(objects.OwnerAccess(), found.Value());
    if (!clip) {
        return Result<SharedPtr<UserWidgetAnimation>>::Failure(clip.Error());
    }
    return PlayAnimation(objects, app, *clip.Value().Get().Value(), Move(options));
}

} // namespace rocks::ui
