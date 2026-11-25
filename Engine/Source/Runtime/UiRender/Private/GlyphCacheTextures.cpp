#include <UiRender/GlyphCacheTextures.h>

// 글리프 cache 텍스처의 GPU 쪽이다(Docs/Design/UiToolkit 9.1절 UW2).
namespace rocks::ui {

namespace {

graphics::PixelFormat FormatOf(text::FontAtlasContent content) noexcept {
    switch (content) {
    case text::FontAtlasContent::Color:
        return graphics::PixelFormat::BGRA8UnormSrgb;
    case text::FontAtlasContent::DistanceField:
        return graphics::PixelFormat::RGBA8Unorm;
    case text::FontAtlasContent::Alpha:
    default:
        return graphics::PixelFormat::R8Unorm;
    }
}

} // namespace

GlyphCacheTextures::GlyphCacheTextures(Allocator& allocator, graphics::Device& device,
                                       SlotArray<BrushTexture>& textures) noexcept
    : device_(device), textures_(textures), entries_(allocator) {
}

GlyphCacheTextures::~GlyphCacheTextures() noexcept {
    Reset();
}

Result<void> GlyphCacheTextures::Sync(text::GlyphCache& cache,
                                      graphics::PipelineHandle maskPipeline,
                                      graphics::PipelineHandle imagePipeline) noexcept {
    if (maskPipeline.IsNull() || imagePipeline.IsNull()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const uint32 count = cache.TextureCount();
    for (uint32 index = 0; index < count; ++index) {
        auto info = cache.Texture(index);
        if (!info) {
            return Result<void>::Failure(info.Error());
        }

        const text::FontTextureInfo& texture = info.Value();
        text::AtlasRect dirty;
        const bool changed = cache.TakeDirtyRegion(index, dirty);
        if (index < entries_.Size()) {
            if (changed) {
                if (auto updated =
                        device_.UpdateTexture(entries_[index].texture, 0, texture.pixels);
                    !updated) {
                    return updated;
                }
            }

            continue;
        }

        // 새 텍스처는 cache의 현재 화소를 초기 내용으로 만든다(바뀐 사각형 기록은 위에서 비웠다).
        graphics::TextureDesc desc;
        desc.width = texture.width;
        desc.height = texture.height;
        desc.format = FormatOf(texture.content);
        desc.usage = graphics::TextureUsage::Sampled;
        auto created = device_.CreateTexture(desc, texture.pixels);
        if (!created) {
            return Result<void>::Failure(created.Error());
        }

        const graphics::PipelineHandle pipeline =
            texture.content == text::FontAtlasContent::Alpha ? maskPipeline : imagePipeline;
        auto slot = textures_.TryAdd({created.Value(), pipeline});
        if (!slot) {
            device_.Retire(created.Value());
            return Result<void>::Failure(slot.Error());
        }

        if (auto added = entries_.TryPushBack({created.Value(), slot.Value()}); !added) {
            (void)textures_.TryRemove(slot.Value());
            device_.Retire(created.Value());
            return added;
        }
    }

    return Result<void>::Success();
}

void GlyphCacheTextures::Reset() noexcept {
    for (const Entry& entry : entries_) {
        (void)textures_.TryRemove(entry.slot);
        device_.Retire(entry.texture);
    }

    entries_.Clear();
}

SlotHandle GlyphCacheTextures::Resource(uint32 textureIndex) const noexcept {
    return textureIndex < entries_.Size() ? entries_[textureIndex].slot : SlotHandle{};
}

} // namespace rocks::ui
