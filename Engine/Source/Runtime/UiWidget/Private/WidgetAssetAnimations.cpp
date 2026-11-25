#include <Core/Guid.h>
#include <UiWidget/WidgetAnimation.generated.h>
#include <UiWidget/WidgetAsset.generated.h>

namespace rocks::ui {

Guid WidgetAnimation::StableId() const noexcept {
    return stableId_;
}

Span<const ObjectHandle> WidgetAsset::Animations() const noexcept {
    return animations_.View();
}

Result<RootRef<WidgetAnimation>> WidgetAsset::CreateAnimation(ObjectRegistry& objects,
                                                              StringView name) noexcept {
    if (name.IsEmpty()) {
        return Result<RootRef<WidgetAnimation>>::Failure(ErrorCode::InvalidArgument);
    }

    auto id = TryNewGuid();
    if (!id) {
        return Result<RootRef<WidgetAnimation>>::Failure(id.Error());
    }

    const auto access = objects.OwnerAccess();
    auto clip = objects.NewObject<WidgetAnimation>(access, GeneratedType<WidgetAnimation>::Id(),
                                                   Handle(), name);
    if (!clip) {
        return clip;
    }

    auto borrowed = objects.ResolveForWrite<WidgetAnimation>(access, clip.Value().Handle());
    if (!borrowed) {
        return Result<RootRef<WidgetAnimation>>::Failure(borrowed.Error());
    }

    auto* animation = borrowed.Value().Get().Value();
    animation->stableId_ = id.Value();
    auto added = objects.AddReference(access, Handle(), animations_, animation->Handle());
    if (!added) {
        (void)animation->Destroy();
        return Result<RootRef<WidgetAnimation>>::Failure(added.Error());
    }

    return clip;
}

} // namespace rocks::ui
