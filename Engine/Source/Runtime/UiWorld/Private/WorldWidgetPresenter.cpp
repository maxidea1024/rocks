#include <UiWorld/WorldWidgetPresenter.h>

namespace rocks::ui {

namespace {

// 보관 표면은 투명 바탕에 그린다(합성 셰이더가 미리 곱한 알파로 읽는다).
constexpr LinearColor WorldWidgetClear{0, 0, 0, 0};

} // namespace

WorldWidgetPresenter::WorldWidgetPresenter(Allocator& allocator, UiRenderer& renderer) noexcept
    : renderer_(renderer), children_(allocator, renderer), batch_(allocator) {
}

WorldWidgetPresenter::~WorldWidgetPresenter() noexcept {
    if (!surface_.IsNull()) {
        renderer_.ReleaseTexture(surface_);
    }
}

UiTextureId WorldWidgetPresenter::Resource() const noexcept {
    return surface_;
}

Result<bool> WorldWidgetPresenter::Prepare(WorldWidgetComponent& component, uint64 frame,
                                           UiDrawBuffer& buffer) noexcept {
    auto arranged = component.PrepareWindow();
    if (!arranged) {
        return Result<bool>::Failure(arranged.Error());
    }

    const auto root = component.WindowRoot();
    const Vec2 size = component.DrawSize();
    const uint32 width = static_cast<uint32>(size.x), height = static_cast<uint32>(size.y);
    if (!root || width == 0 || height == 0) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    const usize pendingBefore = buffer.PendingPassCount();
    auto prepared = children_.Record(root, frame, width, height, buffer);
    if (!prepared) {
        return Result<bool>::Failure(prepared.Error());
    }

    auto cache = component.WindowApplication().UpdateArrangedPaintCache();
    if (!cache) {
        return Result<bool>::Failure(cache.Error());
    }

    if (root_.Lock().Get() == root.Get() && size_ == size &&
        revision_ == cache.Value()->Revision() && !prepared.Value() &&
        buffer.PendingPassCount() == pendingBefore) {
        return Result<bool>::Success(false);
    }

    if (auto updated = batch_.Update(*cache.Value()); !updated) {
        return Result<bool>::Failure(updated.Error());
    }

    if (surface_.IsNull()) {
        auto made = renderer_.CreateRenderTarget(width, height, UiTextureSampling::Premultiplied);
        if (!made) {
            return Result<bool>::Failure(made.Error());
        }

        surface_ = made.Value();
    } else if (size_ != size) {
        if (auto resized = renderer_.ResizeRenderTarget(surface_, width, height); !resized) {
            return Result<bool>::Failure(resized.Error());
        }
    }

    if (auto added = buffer.AddRetainedPass(surface_, width, height, WorldWidgetClear, batch_);
        !added) {
        return Result<bool>::Failure(added.Error());
    }

    root_ = root;
    size_ = size;
    revision_ = cache.Value()->Revision();
    return Result<bool>::Success(true);
}

Result<WorldWidgetClipPlane>
WorldWidgetPresenter::Project(const WorldWidgetComponent& component,
                              const Mat4d& viewProjection) const noexcept {
    const auto root = component.WindowRoot();
    if (!component.IsEnabled() || component.IsScreenSpace() || !root || surface_.IsNull() ||
        root_.Lock().Get() != root.Get() || size_ != component.DrawSize()) {
        return Result<WorldWidgetClipPlane>::Failure(ErrorCode::NotReady);
    }

    Vec3d tl, tr, bl;
    component.PlaneCorners(tl, tr, bl);
    return WorldWidgetClipPlane::Project(tl, tr, bl, viewProjection);
}

} // namespace rocks::ui
