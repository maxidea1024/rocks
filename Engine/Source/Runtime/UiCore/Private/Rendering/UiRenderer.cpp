#include <UiCore/Rendering/UiRenderer.h>

namespace rocks::ui {

UiRenderer::UiRenderer(Allocator& allocator) noexcept : buffer_(allocator) {
}

Result<UiTextureId> UiRenderer::CreateRenderTarget(uint32 width, uint32 height,
                                                   UiTextureSampling sampling) noexcept {
    UiTextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.format = UiTextureFormat::Rgba8Srgb;
    desc.usage = UiTextureUsage::RenderTarget;
    desc.sampling = sampling;
    return CreateTexture(desc, {});
}

Result<UiDrawBuffer*> UiRenderer::AcquireDrawBuffer() noexcept {
    if (acquired_) {
        return Result<UiDrawBuffer*>::Failure(ErrorCode::Busy);
    }

    acquired_ = true;
    buffer_.Clear();
    return Result<UiDrawBuffer*>::Success(&buffer_);
}

void UiRenderer::ReleaseDrawBuffer(UiDrawBuffer& buffer) noexcept {
    if (&buffer == &buffer_) {
        buffer_.Clear();
        acquired_ = false;
    }
}

} // namespace rocks::ui
