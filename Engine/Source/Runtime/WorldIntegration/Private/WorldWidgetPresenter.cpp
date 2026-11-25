#include <WorldIntegration/WorldWidgetPresenter.h>

namespace rocks::ui {

WorldWidgetPresenter::WorldWidgetPresenter(Allocator& allocator, graphics::Device& device,
                                           render::ShaderSet& shaders,
                                           SlotArray<BrushTexture>& textures,
                                           graphics::PixelFormat format) noexcept
    : device_(device), shaders_(shaders), textures_(textures), format_(format),
      children_(allocator, device, shaders, textures, format), batch_(allocator),
      buffers_(device, shaders, batch_, format), surface_(device, textures) {
}

WorldWidgetPresenter::~WorldWidgetPresenter() noexcept {
    if (!solid_.IsNull()) {
        device_.Retire(solid_);
    }

    if (!composite_.IsNull()) {
        device_.Retire(composite_);
    }
}

SlotHandle WorldWidgetPresenter::Resource() const noexcept {
    return surface_.Resource();
}

Result<bool> WorldWidgetPresenter::Prepare(WorldWidgetComponent& component, uint64 frame) noexcept {
    auto arranged = component.PrepareWindow();
    if (!arranged) {
        return Result<bool>::Failure(arranged.Error());
    }

    if (solid_.IsNull()) {
        auto made = DrawBatchRenderer::CreateSolidPipeline(device_, shaders_, format_);
        if (!made) {
            return Result<bool>::Failure(made.Error());
        }

        solid_ = made.Value();
    }

    if (composite_.IsNull()) {
        auto made = DrawBatchRenderer::CreateRetainedPipeline(device_, shaders_, format_);
        if (!made) {
            return Result<bool>::Failure(made.Error());
        }

        composite_ = made.Value();
    }

    const auto root = component.WindowRoot();
    const Vec2 size = component.DrawSize();
    const uint32 width = static_cast<uint32>(size.x), height = static_cast<uint32>(size.y);
    auto prepared = children_.Prepare(root, frame, width, height, solid_);
    if (!prepared) {
        return Result<bool>::Failure(prepared.Error());
    }

    auto cache = component.WindowApplication().UpdateArrangedPaintCache();
    if (!cache) {
        return Result<bool>::Failure(cache.Error());
    }

    if (root_.Lock().Get() == root.Get() && size_ == size &&
        revision_ == cache.Value()->Revision() && !prepared.Value()) {
        return Result<bool>::Success(false);
    }

    auto status = batch_.Update(*cache.Value());
    if (status) {
        status = buffers_.Upload();
    }

    if (status) {
        status = surface_.Render(buffers_, width, height, format_, solid_, composite_);
    }

    if (!status) {
        return Result<bool>::Failure(status.Error());
    }

    root_ = root;
    size_ = size;
    revision_ = cache.Value()->Revision();
    return Result<bool>::Success(true);
}

Result<WorldWidgetClipPlane>
WorldWidgetPresenter::Submit(graphics::CommandContext& commands, WorldWidgetPlane& plane,
                             const WorldWidgetComponent& component, const Mat4d& viewProjection,
                             const WorldWidgetClipPlane* previous) noexcept {
    const auto root = component.WindowRoot();
    if (!component.IsEnabled() || component.IsScreenSpace() || !root ||
        root_.Lock().Get() != root.Get() || size_ != component.DrawSize()) {
        return Result<WorldWidgetClipPlane>::Failure(ErrorCode::NotReady);
    }

    const auto* texture = textures_.Find(Resource());
    if (!texture) {
        return Result<WorldWidgetClipPlane>::Failure(ErrorCode::StaleHandle);
    }

    Vec3d tl, tr, bl;
    component.PlaneCorners(tl, tr, bl);
    auto current = WorldWidgetClipPlane::Project(tl, tr, bl, viewProjection);
    if (!current) {
        return current;
    }

    auto drawn = plane.Draw(commands, texture->texture, current.Value(),
                            previous ? *previous : current.Value());
    if (!drawn) {
        return Result<WorldWidgetClipPlane>::Failure(drawn.Error());
    }

    return current;
}

} // namespace rocks::ui
