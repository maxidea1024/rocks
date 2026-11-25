#include <UiFramework/Viewport.h>
#include <UiCore/PaintList.h>

namespace rocks::ui {

Result<void> Viewport::Construct(Args&& args) noexcept {
    if (!args.background.value.IsFinite()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    auto attached = args.content.AttachTo(*this);
    if (attached) {
        scene_.Assign(Move(args.scene));
        background_.Assign(Move(args.background));
    }
    return attached;
}

WidgetSurface* Viewport::GetWidgetSurface() noexcept {
    return this;
}

WidgetSurfaceKind Viewport::SurfaceKind() const noexcept {
    return WidgetSurfaceKind::Scene;
}

bool Viewport::WantsManagedSurface() const noexcept {
    return scene_.Get().IsNull();
}

SlotHandle Viewport::ManagedSurface() const noexcept {
    return managed_;
}

void Viewport::SetManagedSurface(SlotHandle texture) noexcept {
    if (managed_ != texture) {
        managed_ = texture;
        InvalidatePaint();
    }
}

Viewport::Viewport(Allocator& allocator) noexcept
    : SizeBox(allocator), scene_(*this, {}), background_(*this, {0, 0, 0, 1}) {
}

void Viewport::SetSceneSource(SharedPtr<WidgetSurfaceScene> scene) noexcept {
    if (source_ != scene) {
        source_ = Move(scene);
        InvalidatePaintPlan();
    }
}
SharedPtr<WidgetSurfaceScene> Viewport::SceneSource() const noexcept {
    return source_;
}

void Viewport::SetSceneTexture(SlotHandle texture) noexcept {
    scene_.Set(texture);
}

Result<void> Viewport::SetBackground(LinearColor color) noexcept {
    if (!color.IsFinite()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    background_.Set(color);
    return Result<void>::Success();
}

Result<void> Viewport::OnPaint(PaintList& list, int32 layer) const noexcept {
    const SlotHandle texture = scene_.Get().IsNull() ? managed_ : scene_.Get();
    auto filled = list.TryAddBox({GetGeometry(), background_.Get(), layer});
    if (!filled || texture.IsNull()) {
        return filled;
    }
    PaintBox box{GetGeometry(), {1, 1, 1, 1}, layer};
    box.resource = texture;
    return list.TryAddBox(box);
}

} // namespace rocks::ui
