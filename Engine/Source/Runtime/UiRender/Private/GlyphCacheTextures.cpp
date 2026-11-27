#include <UiRender/GlyphCacheTextures.h>

// 글리프 cache 텍스처의 렌더러 쪽이다(Docs/Design/UiToolkit 9.1절 UW2·18.4절).
namespace rocks::ui {

namespace {

UiTextureDesc DescOf(const text::FontTextureInfo& texture) noexcept {
    UiTextureDesc desc;
    desc.width = texture.width;
    desc.height = texture.height;
    desc.usage = UiTextureUsage::Dynamic;
    switch (texture.content) {
    case text::FontAtlasContent::Color:
        desc.format = UiTextureFormat::Bgra8Srgb;
        desc.sampling = UiTextureSampling::Image;
        break;
    case text::FontAtlasContent::DistanceField:
        desc.format = UiTextureFormat::Rgba8;
        desc.sampling = UiTextureSampling::Image;
        break;
    case text::FontAtlasContent::Alpha:
    default:
        desc.format = UiTextureFormat::R8;
        desc.sampling = UiTextureSampling::Mask;
        break;
    }

    return desc;
}

} // namespace

GlyphCacheTextures::GlyphCacheTextures(Allocator& allocator, UiRenderer& renderer) noexcept
    : renderer_(renderer), textures_(allocator) {
}

GlyphCacheTextures::~GlyphCacheTextures() noexcept {
    Reset();
}

Result<void> GlyphCacheTextures::Upload(text::GlyphCache& cache) noexcept {
    const uint32 count = cache.TextureCount();
    for (uint32 index = 0; index < count; ++index) {
        auto info = cache.Texture(index);
        if (!info) {
            return Result<void>::Failure(info.Error());
        }

        const text::FontTextureInfo& texture = info.Value();
        text::AtlasRect dirty;
        const bool changed = cache.TakeDirtyRegion(index, dirty);
        if (index < textures_.Size()) {
            if (changed) {
                if (auto updated = renderer_.UpdateTexture(textures_[index], texture.pixels);
                    !updated) {
                    return updated;
                }
            }

            continue;
        }

        // 새 텍스처는 cache의 현재 화소를 초기 내용으로 만든다(바뀐 사각형 기록은 위에서 비웠다).
        auto created = renderer_.CreateTexture(DescOf(texture), texture.pixels);
        if (!created) {
            return Result<void>::Failure(created.Error());
        }

        if (auto added = textures_.TryPushBack(created.Value()); !added) {
            renderer_.ReleaseTexture(created.Value());
            return added;
        }
    }

    return Result<void>::Success();
}

void GlyphCacheTextures::Reset() noexcept {
    for (UiTextureId texture : textures_) {
        renderer_.ReleaseTexture(texture);
    }

    textures_.Clear();
}

UiTextureId GlyphCacheTextures::Resource(uint32 textureIndex) const noexcept {
    return textureIndex < textures_.Size() ? textures_[textureIndex] : UiTextureId{};
}

} // namespace rocks::ui
