#include <UiRender/GlyphAtlasTexture.h>

namespace rocks::ui {

GlyphAtlasTexture::GlyphAtlasTexture(graphics::Device& device,
                                     SlotArray<BrushTexture>& textures) noexcept
    : device_(device), textures_(textures) {
}

GlyphAtlasTexture::~GlyphAtlasTexture() noexcept {
    if (!resource_.IsNull()) {
        (void)textures_.TryRemove(resource_);
        device_.Retire(texture_);
    }
}

SlotHandle GlyphAtlasTexture::Resource() const noexcept {
    return resource_;
}

Result<void> GlyphAtlasTexture::Upload(const text::GlyphAtlasSnapshot& page,
                                       graphics::PipelineHandle pipeline) noexcept {
    if (pipeline.IsNull() || page.revision == 0 || page.size < text::MinGlyphAtlasSize ||
        page.size > text::MaxGlyphAtlasSize) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    graphics::TextureDesc desc;
    desc.width = page.size;
    desc.height = page.size;
    switch (page.format) {
    case text::GlyphAtlasFormat::Alpha:
        desc.format = graphics::PixelFormat::R8Unorm;
        break;
    case text::GlyphAtlasFormat::Color:
        desc.format = graphics::PixelFormat::RGBA8UnormSrgb;
        break;
    case text::GlyphAtlasFormat::DistanceField:
        desc.format = graphics::PixelFormat::RGBA8Unorm;
        break;
    default:
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    const usize channels = page.format == text::GlyphAtlasFormat::Alpha ? 1 : 4;
    if (page.pixels.Size() != static_cast<usize>(page.size) * page.size * channels) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!resource_.IsNull()) {
        if (size_ != page.size || format_ != page.format) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        BrushTexture* brush = textures_.Find(resource_);
        if (!brush) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }
        if (page.revision > revision_) {
            auto updated = device_.UpdateTexture(texture_, 0, page.pixels.View());
            if (!updated) {
                return updated;
            }
            revision_ = page.revision;
        }
        brush->pipeline = pipeline;
        return Result<void>::Success();
    }

    auto created = device_.CreateTexture(desc, page.pixels.View());
    if (!created) {
        return Result<void>::Failure(created.Error());
    }
    auto registered = textures_.TryAdd({created.Value(), pipeline});
    if (!registered) {
        device_.Retire(created.Value());
        return Result<void>::Failure(registered.Error());
    }
    texture_ = created.Value();
    resource_ = registered.Value();
    revision_ = page.revision;
    size_ = page.size;
    format_ = page.format;
    return Result<void>::Success();
}

} // namespace rocks::ui
