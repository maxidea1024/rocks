// 글리프 cache의 CPU 부분이다(Docs/Design/Font 6.6절). 배치는 UE FSlateFontCache::AddNewEntry
// (FontCache.cpp 1077–1222행), flush 계수는 FSlateFlushableAtlasCache(TextureAtlas.cpp 412–476행)와
// 같다.
#include <TextLayout/GlyphCache.h>

#include <Core/Diagnostics/Log.h>
#include <Core/Hash.h>
#include <Platform/Memory.h>
#include <string.h>

namespace rocks::text {

namespace {

inline constexpr LogCategory LogTextLayout{"TextLayout"};

uint32 ClampAtlasSize(uint32 size) noexcept {
    return size < MinFontAtlasSize ? MinFontAtlasSize
                                   : (size > MaxFontAtlasSize ? MaxFontAtlasSize : size);
}

uint64 FloatBits(float value) noexcept {
    return platform::BitCast<uint32>(value);
}

} // namespace

uint64 GlyphCache::KeyHash::operator()(const GlyphKey& key) const noexcept {
    uint64 hash = MixHash(static_cast<uint64>(reinterpret_cast<uptr>(key.face)));
    hash = HashCombine(hash, MixHash(key.glyphIndex));
    hash = HashCombine(hash, MixHash(FloatBits(key.size)));
    hash = HashCombine(hash, MixHash(FloatBits(key.scale)));
    hash = HashCombine(hash, MixHash(FloatBits(key.skew)));
    hash = HashCombine(hash, MixHash(FloatBits(key.outline.size)));
    const uint64 flags =
        (key.outline.mitered ? 1u : 0u) | (key.outline.separateFillAlpha ? 2u : 0u);
    return HashCombine(hash, MixHash(flags));
}

GlyphCache::GlyphCache(Allocator& allocator, const GlyphCacheSettings& settings) noexcept
    : allocator_(allocator), settings_(settings), glyphs_(allocator), textures_(allocator),
      alphaPages_(allocator), colorPages_(allocator), distanceFieldPages_(allocator),
      nonAtlasTextures_(allocator) {
    settings_.alphaAtlasSize = ClampAtlasSize(settings.alphaAtlasSize);
    settings_.colorAtlasSize = ClampAtlasSize(settings.colorAtlasSize);
    settings_.distanceFieldAtlasSize = ClampAtlasSize(settings.distanceFieldAtlasSize);
    ResetFlushCounters();
    lastFlushRequestFrame_ = 0;
}

Result<UniquePtr<GlyphCache>> GlyphCache::TryCreate(Allocator& allocator,
                                                    const GlyphCacheSettings& settings) noexcept {
    return UniquePtr<GlyphCache>::TryMake(allocator, allocator, settings);
}

Result<CachedGlyph> GlyphCache::FindOrAdd(const GlyphKey& key) noexcept {
    if (key.face == nullptr || !(key.size > 0.0f) || !(key.scale > 0.0f)) {
        return Result<CachedGlyph>::Failure(ErrorCode::InvalidArgument);
    }

    platform::LockGuard guard(mutex_);
    if (const CachedGlyph* found = glyphs_.Find(key)) {
        return Result<CachedGlyph>::Success(*found);
    }

    GlyphRasterRequest request;
    request.glyphIndex = key.glyphIndex;
    request.size = key.size;
    request.scale = key.scale;
    request.skew = key.skew;
    request.outline = key.outline;
    request.antialiasing = settings_.antialiasing;
    request.spanRasterizer = settings_.spanRasterizer;
    GlyphBitmap bitmap(allocator_);
    CachedGlyph entry;
    if (auto rasterized = key.face->Rasterize(request, bitmap); rasterized) {
        auto placed = Place(bitmap);
        if (!placed) {
            return placed;
        }

        entry = placed.Value();
    } else if (rasterized.Error() == ErrorCode::OutOfMemory) {
        return Result<CachedGlyph>::Failure(ErrorCode::OutOfMemory);
    }

    if (auto inserted = glyphs_.Insert(GlyphKey(key), CachedGlyph(entry)); !inserted) {
        return Result<CachedGlyph>::Failure(inserted.Error());
    }

    return Result<CachedGlyph>::Success(entry);
}

Result<CachedGlyph> GlyphCache::Place(const GlyphBitmap& bitmap) noexcept {
    CachedGlyph entry;
    entry.content = bitmap.content;
    entry.bitmapRenderScale = bitmap.bitmapRenderScale;
    entry.supportsOutline = bitmap.supportsOutline;
    const uint32 width = static_cast<uint32>(bitmap.width);
    const uint32 height = static_cast<uint32>(bitmap.height);
    const uint32 atlasSize = AtlasSizeOf(bitmap.content);

    // 한 변이 atlas보다 크면 atlas 밖 텍스처다(UE FontCache.cpp 1100–1143행).
    if (width > atlasSize || height > atlasSize) {
        Log::Warning(LogTextLayout,
                     "Glyph {Width}x{Height} does not fit a {Size} px font atlas; using a "
                     "separate texture",
                     width, height, atlasSize);
        FontTexture texture(allocator_);
        texture.content = bitmap.content;
        texture.width = width;
        texture.height = height;
        texture.dirty = true;
        if (auto copied = texture.pixels.TryAppend(bitmap.pixels.View()); !copied) {
            return Result<CachedGlyph>::Failure(copied.Error());
        }

        uint32 index = 0;
        auto added = AddTexture(Move(texture), index);
        if (!added) {
            return Result<CachedGlyph>::Failure(added.Error());
        }

        if (!added.Value()) {
            return Result<CachedGlyph>::Success(CachedGlyph{});
        }

        if (auto listed = nonAtlasTextures_.TryPushBack(index); !listed) {
            (void)textures_.TryPopBack();
            return Result<CachedGlyph>::Failure(listed.Error());
        }

        if (!flushRequested_) {
            UpdateFlushCounters();
        }

        entry.valid = true;
        entry.texture = index;
        entry.rect = {0, 0, width, height};
        entry.bearingX = bitmap.bearingX;
        entry.bearingY = bitmap.bearingY;
        return Result<CachedGlyph>::Success(entry);
    }

    const auto fill = [&](uint32 index, const AtlasRect& slot) noexcept {
        entry.valid = true;
        entry.texture = index;
        entry.rect = slot;
        entry.bearingX = bitmap.bearingX - static_cast<int32>(FontAtlasPadding);
        entry.bearingY = bitmap.bearingY + static_cast<int32>(FontAtlasPadding);
    };

    Array<uint32>& pages = PagesOf(bitmap.content);
    for (const uint32 index : pages) {
        AtlasRect slot;
        auto added = textures_[index].page->Add(width, height, bitmap.pixels.View(), slot);
        if (!added) {
            return Result<CachedGlyph>::Failure(added.Error());
        }

        if (added.Value()) {
            fill(index, slot);
            return Result<CachedGlyph>::Success(entry);
        }
    }

    // 새 페이지다.
    auto page = FontAtlasPage::TryCreate(allocator_, atlasSize, atlasSize, bitmap.content);
    if (!page) {
        return Result<CachedGlyph>::Failure(page.Error());
    }

    AtlasRect slot;
    auto added = page.Value()->Add(width, height, bitmap.pixels.View(), slot);
    if (!added) {
        return Result<CachedGlyph>::Failure(added.Error());
    }

    FontTexture texture(allocator_);
    texture.content = bitmap.content;
    texture.width = atlasSize;
    texture.height = atlasSize;
    texture.dirty = true;
    texture.page = Move(page.Value());
    uint32 index = 0;
    auto appended = AddTexture(Move(texture), index);
    if (!appended) {
        return Result<CachedGlyph>::Failure(appended.Error());
    }

    if (!appended.Value()) {
        return Result<CachedGlyph>::Success(CachedGlyph{});
    }

    if (auto listed = pages.TryPushBack(index); !listed) {
        (void)textures_.TryPopBack();
        return Result<CachedGlyph>::Failure(listed.Error());
    }

    if (!flushRequested_) {
        UpdateFlushCounters();
    }

    if (added.Value()) {
        fill(index, slot);
    }

    return Result<CachedGlyph>::Success(entry);
}

Result<bool> GlyphCache::AddTexture(FontTexture&& texture, uint32& outIndex) noexcept {
    if (textures_.Size() >= MaxFontTextures) {
        Log::Warning(LogTextLayout, "Font cache has reached {Count} textures", MaxFontTextures);
        return Result<bool>::Success(false);
    }

    if (auto added = textures_.TryPushBack(Move(texture)); !added) {
        return Result<bool>::Failure(added.Error());
    }

    outIndex = static_cast<uint32>(textures_.Size() - 1);
    return Result<bool>::Success(true);
}

Array<uint32>& GlyphCache::PagesOf(FontAtlasContent content) noexcept {
    switch (content) {
    case FontAtlasContent::Color:
        return colorPages_;
    case FontAtlasContent::DistanceField:
        return distanceFieldPages_;
    case FontAtlasContent::Alpha:
    default:
        return alphaPages_;
    }
}

uint32 GlyphCache::AtlasSizeOf(FontAtlasContent content) const noexcept {
    switch (content) {
    case FontAtlasContent::Color:
        return settings_.colorAtlasSize;
    case FontAtlasContent::DistanceField:
        return settings_.distanceFieldAtlasSize;
    case FontAtlasContent::Alpha:
    default:
        return settings_.alphaAtlasSize;
    }
}

void GlyphCache::UpdateFlushCounters() noexcept {
    bool requested = false;
    requested |= UpdateCounter(static_cast<int32>(alphaPages_.Size()), alphaCounter_,
                               settings_.maxAtlasPagesBeforeFlush, settings_.growAtlasFrameWindow);
    requested |= UpdateCounter(static_cast<int32>(colorPages_.Size()), colorCounter_,
                               settings_.maxAtlasPagesBeforeFlush, settings_.growAtlasFrameWindow);
    requested |=
        UpdateCounter(static_cast<int32>(distanceFieldPages_.Size()), distanceFieldCounter_,
                      settings_.maxAtlasPagesBeforeFlush, settings_.growAtlasFrameWindow);
    requested |=
        UpdateCounter(static_cast<int32>(nonAtlasTextures_.Size()), nonAtlasCounter_,
                      settings_.maxNonAtlasTexturesBeforeFlush, settings_.growNonAtlasFrameWindow);
    if (requested) {
        ResetFlushCounters();
    }
}

bool GlyphCache::UpdateCounter(int32 current, FlushCounter& counter, int32 initialMax,
                               int32 frameWindow) noexcept {
    if (current <= counter.currentMax) {
        return false;
    }

    if (current <= initialMax) {
        // 초기 한도가 바뀌었을 수 있다(UE와 같다).
        counter.currentMax = initialMax;
        return false;
    }

    const uint64 sinceRequest = frame_ - lastFlushRequestFrame_;
    if (sinceRequest <= static_cast<uint64>(frameWindow)) {
        // flush 직후 다시 찼으면 실제로 많이 쓰는 것이므로 한도를 올린다.
        counter.currentMax = current;
        Log::Warning(LogTextLayout,
                     "Raising the font cache flush threshold to {Count} textures; a lot of font "
                     "data is cached",
                     current);
        return false;
    }

    Log::Info(LogTextLayout, "Font atlases out of space ({Count}/{Max} textures); flush requested",
              current, counter.currentMax);
    flushRequested_ = true;
    return true;
}

void GlyphCache::ResetFlushCounters() noexcept {
    alphaCounter_.currentMax = settings_.maxAtlasPagesBeforeFlush;
    colorCounter_.currentMax = settings_.maxAtlasPagesBeforeFlush;
    distanceFieldCounter_.currentMax = settings_.maxAtlasPagesBeforeFlush;
    nonAtlasCounter_.currentMax = settings_.maxNonAtlasTexturesBeforeFlush;
    lastFlushRequestFrame_ = frame_;
}

uint32 GlyphCache::TextureCount() const noexcept {
    platform::LockGuard guard(mutex_);
    return static_cast<uint32>(textures_.Size());
}

Result<FontTextureInfo> GlyphCache::Texture(uint32 index) const noexcept {
    platform::LockGuard guard(mutex_);
    if (index >= textures_.Size()) {
        return Result<FontTextureInfo>::Failure(ErrorCode::InvalidArgument);
    }

    const FontTexture& texture = textures_[index];
    FontTextureInfo info;
    info.content = texture.content;
    info.atlas = texture.page.Get() != nullptr;
    info.width = texture.width;
    info.height = texture.height;
    info.pixels = info.atlas ? texture.page->Pixels() : texture.pixels.View();
    return Result<FontTextureInfo>::Success(info);
}

bool GlyphCache::TakeDirtyRegion(uint32 index, AtlasRect& out) noexcept {
    platform::LockGuard guard(mutex_);
    if (index >= textures_.Size()) {
        return false;
    }

    FontTexture& texture = textures_[index];
    if (texture.dirty) {
        // 새 텍스처는 전체를 올린다(페이지의 칸 단위 기록은 함께 비운다).
        texture.dirty = false;
        if (texture.page.Get() != nullptr) {
            AtlasRect ignored;
            (void)texture.page->TakeDirtyRegion(ignored);
        }

        out = {0, 0, texture.width, texture.height};
        return true;
    }

    return texture.page.Get() != nullptr && texture.page->TakeDirtyRegion(out);
}

void GlyphCache::RequestFlush() noexcept {
    platform::LockGuard guard(mutex_);
    flushRequested_ = true;
}

bool GlyphCache::AdvanceFrame() noexcept {
    platform::LockGuard guard(mutex_);
    const bool flush = flushRequested_;
    if (flush) {
        Flush();
        flushRequested_ = false;
    }

    ++frame_;
    return flush;
}

void GlyphCache::Flush() noexcept {
    glyphs_.Clear();
    textures_.Clear();
    alphaPages_.Clear();
    colorPages_.Clear();
    distanceFieldPages_.Clear();
    nonAtlasTextures_.Clear();
}

GlyphCacheStats GlyphCache::Stats() const noexcept {
    platform::LockGuard guard(mutex_);
    GlyphCacheStats stats;
    stats.glyphs = glyphs_.Count();
    stats.alphaPages = static_cast<uint32>(alphaPages_.Size());
    stats.colorPages = static_cast<uint32>(colorPages_.Size());
    stats.distanceFieldPages = static_cast<uint32>(distanceFieldPages_.Size());
    stats.nonAtlasTextures = static_cast<uint32>(nonAtlasTextures_.Size());
    for (const FontTexture& texture : textures_) {
        stats.textureBytes += static_cast<usize>(texture.width) * texture.height *
                              FontAtlasBytesPerPixel(texture.content);
    }

    return stats;
}

} // namespace rocks::text
