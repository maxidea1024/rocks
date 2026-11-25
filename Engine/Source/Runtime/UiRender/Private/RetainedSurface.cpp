#include <UiRender/RetainedSurface.h>
#include <UiRender/DrawBatchBuffers.h>

namespace rocks::ui {

RetainedSurface::RetainedSurface(graphics::Device& device,
                                 SlotArray<BrushTexture>& textures) noexcept
    : device_(device), textures_(textures) {
}

RetainedSurface::~RetainedSurface() noexcept {
    if (!resource_.IsNull()) {
        (void)textures_.TryRemove(resource_);
    }
    if (!visible_.texture.IsNull()) {
        device_.Retire(visible_.texture);
    }
    if (!scratch_.texture.IsNull()) {
        device_.Retire(scratch_.texture);
    }
}

SlotHandle RetainedSurface::Resource() const noexcept {
    return resource_;
}

Result<void> RetainedSurface::Render(const DrawBatchBuffers& buffers, uint32 width, uint32 height,
                                     graphics::PixelFormat format, graphics::PipelineHandle solid,
                                     graphics::PipelineHandle composite) noexcept {
    if (width == 0 || height == 0 || solid.IsNull() || composite.IsNull()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    BrushTexture* existing = textures_.Find(resource_);
    if (!resource_.IsNull() && !existing) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }
    if (scratch_.texture.IsNull() || scratch_.width != width || scratch_.height != height ||
        scratch_.format != format) {
        graphics::TextureDesc desc;
        desc.width = width;
        desc.height = height;
        desc.format = format;
        desc.usage = graphics::TextureUsage::RenderTarget;
        auto created = device_.CreateTexture(desc, {});
        if (!created) {
            return Result<void>::Failure(created.Error());
        }
        if (!scratch_.texture.IsNull()) {
            device_.Retire(scratch_.texture);
        }
        scratch_ = {created.Value(), width, height, format};
    }

    auto pass = device_.BeginOffscreen(scratch_.texture, {0, 0, 0, 0});
    if (!pass) {
        return Result<void>::Failure(pass.Error());
    }
    auto drawn = DrawBatchRenderer::Draw(*pass.Value(), solid, buffers,
                                         {static_cast<float>(width), static_cast<float>(height)},
                                         &textures_);
    auto ended = device_.EndFrame();
    if (!drawn) {
        return drawn;
    }
    if (!ended) {
        return Result<void>::Failure(ended.Error());
    }

    const BrushTexture brush{scratch_.texture, composite};
    if (existing) {
        *existing = brush;
    } else {
        auto registered = textures_.TryAdd(brush);
        if (!registered) {
            return Result<void>::Failure(registered.Error());
        }
        resource_ = registered.Value();
    }
    const Target previous = visible_;
    visible_ = scratch_;
    scratch_ = previous;
    return Result<void>::Success();
}

} // namespace rocks::ui
