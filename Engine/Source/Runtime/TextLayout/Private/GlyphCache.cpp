// 글리프 cache의 CPU 부분이다(Docs/Design/Font 6.6절). 배치는 UE FSlateFontCache::AddNewEntry
// (FontCache.cpp 1077–1222행), flush 계수는 FSlateFlushableAtlasCache(TextureAtlas.cpp 412–476행)와
// 같다.
#include <TextLayout/GlyphCache.h>

#include <Core/Diagnostics/Log.h>
#include <Core/Hash/TypeHash.h>
#include <Core/Math/MathUtility.h>
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
    hash = HashCombine(hash, MixHash(flags));
    hash = HashCombine(hash, MixHash(static_cast<uint64>(key.mode)));
    return HashCombine(hash, MixHash(key.distanceFieldPpem));
}

uint64 GlyphCache::DistanceFieldKeyHash::operator()(const DistanceFieldKey& key) const noexcept {
    uint64 hash = MixHash(static_cast<uint64>(reinterpret_cast<uptr>(key.face)));
    hash = HashCombine(hash, MixHash(key.glyphIndex));
    hash = HashCombine(hash, MixHash(key.multichannel ? 1u : 0u));
    hash = HashCombine(hash, MixHash(key.ppem));
    hash = HashCombine(hash, MixHash(FloatBits(key.emOuterSpread)));
    return HashCombine(hash, MixHash(FloatBits(key.emInnerSpread)));
}

uint64 GlyphCache::GeometryKeyHash::operator()(const GeometryKey& key) const noexcept {
    const uint64 hash = MixHash(static_cast<uint64>(reinterpret_cast<uptr>(key.face)));
    return HashCombine(hash, MixHash(key.glyphIndex));
}

GlyphCache::GlyphCache(Allocator& allocator, const GlyphCacheSettings& settings,
                       const TextBackend* backend) noexcept
    : allocator_(allocator), settings_(settings), backend_(backend), glyphs_(allocator),
      distanceFields_(allocator), geometries_(allocator), pendingJobs_(allocator),
      textures_(allocator), alphaPages_(allocator), colorPages_(allocator),
      distanceFieldPages_(allocator), nonAtlasTextures_(allocator) {
    settings_.alphaAtlasSize = ClampAtlasSize(settings.alphaAtlasSize);
    settings_.colorAtlasSize = ClampAtlasSize(settings.colorAtlasSize);
    settings_.distanceFieldAtlasSize = ClampAtlasSize(settings.distanceFieldAtlasSize);
    ResetFlushCounters();
    lastFlushRequestFrame_ = 0;
}

Result<UniquePtr<GlyphCache>> GlyphCache::TryCreate(Allocator& allocator,
                                                    const GlyphCacheSettings& settings,
                                                    const TextBackend* backend) noexcept {
    return UniquePtr<GlyphCache>::TryMake(allocator, allocator, settings, backend);
}

Result<CachedGlyph> GlyphCache::FindOrAdd(const GlyphKey& key) noexcept {
    if (key.face == nullptr || !(key.size > 0.0f) || !(key.scale > 0.0f)) {
        return Result<CachedGlyph>::Failure(ErrorCode::InvalidArgument);
    }

    platform::LockGuard guard(mutex_);
    if (key.mode != FontRasterMode::Bitmap && backend_ != nullptr && key.distanceFieldPpem > 0) {
        return FindOrAddDistanceField(key);
    }

    return FindOrAddBitmap(key);
}

Result<CachedGlyph> GlyphCache::FindOrAddBitmap(const GlyphKey& key) noexcept {
    // 비트맵은 거리장 설정과 무관하다.
    GlyphKey bitmapKey = key;
    bitmapKey.mode = FontRasterMode::Bitmap;
    bitmapKey.distanceFieldPpem = 0;
    if (const CachedGlyph* found = glyphs_.Find(bitmapKey)) {
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

    if (auto inserted = glyphs_.Insert(GlyphKey(bitmapKey), CachedGlyph(entry)); !inserted) {
        return Result<CachedGlyph>::Failure(inserted.Error());
    }

    return Result<CachedGlyph>::Success(entry);
}

Result<SharedPtr<const GlyphGeometry>> GlyphCache::GeometryOf(BackendFace& face,
                                                              uint32 glyphIndex) noexcept {
    const GeometryKey key{&face, glyphIndex};
    if (const SharedPtr<const GlyphGeometry>* found = geometries_.Find(key)) {
        return Result<SharedPtr<const GlyphGeometry>>::Success(*found);
    }

    auto made = SharedPtr<GlyphGeometry>::TryMake(allocator_, allocator_);
    if (!made) {
        return Result<SharedPtr<const GlyphGeometry>>::Failure(made.Error());
    }

    SharedPtr<const GlyphGeometry> geometry;
    if (auto extracted = face.ExtractGeometry(glyphIndex, *made.Value()); extracted) {
        geometry = SharedPtr<const GlyphGeometry>(Move(made.Value()));
    } else if (extracted.Error() == ErrorCode::OutOfMemory) {
        return Result<SharedPtr<const GlyphGeometry>>::Failure(ErrorCode::OutOfMemory);
    }

    if (auto inserted =
            geometries_.Insert(GeometryKey(key), SharedPtr<const GlyphGeometry>(geometry));
        !inserted) {
        return Result<SharedPtr<const GlyphGeometry>>::Failure(inserted.Error());
    }

    return Result<SharedPtr<const GlyphGeometry>>::Success(Move(geometry));
}

Result<CachedGlyph> GlyphCache::FindOrAddDistanceField(const GlyphKey& key) noexcept {
    // 목표 ppem·spread다(UE GetSdfGlyphFontAtlasData 1339–1349·1420–1421행).
    const float targetPpem = static_cast<float>(ComputeFontPixelSize(key.size, key.scale)) > 1.0f
                                 ? static_cast<float>(ComputeFontPixelSize(key.size, key.scale))
                                 : 1.0f;
    const float sdfPpem = static_cast<float>(key.distanceFieldPpem);
    const float scale = targetPpem / sdfPpem;
    const float emOutline = math::Floor(key.outline.size * key.scale + 0.5f) / targetPpem;
    float emInner = DistanceFieldInnerSpreadPixels / targetPpem;
    emInner = emInner < MinDistanceFieldInnerSpread
                  ? MinDistanceFieldInnerSpread
                  : (emInner > MaxDistanceFieldInnerSpread ? MaxDistanceFieldInnerSpread : emInner);
    float emOuter = emInner + emOutline;
    emOuter = emOuter > MaxDistanceFieldOuterSpread ? MaxDistanceFieldOuterSpread : emOuter;
    const auto ceilHalfSpread = [sdfPpem](float emSpread) noexcept {
        float pixels = MinDistanceFieldHalfSpreadPixels;
        while (pixels < sdfPpem * emSpread) {
            pixels *= 2.0f;
        }

        return pixels / sdfPpem;
    };

    const bool multichannel = key.mode == FontRasterMode::Msdf;
    const DistanceFieldKey fieldKey{key.face,
                                    key.glyphIndex,
                                    multichannel,
                                    key.distanceFieldPpem,
                                    ceilHalfSpread(emOuter),
                                    ceilHalfSpread(emInner)};

    const auto finalize = [scale](const CachedGlyph& stored) noexcept {
        // 가장자리 반 화소를 뺀 사각형이다(UE FinalizeEntry).
        CachedGlyph glyph = stored;
        glyph.quadBearingX = scale * (static_cast<float>(stored.bearingX) + 0.5f);
        glyph.quadBearingY = scale * (static_cast<float>(stored.bearingY) - 0.5f);
        glyph.quadWidth = scale * static_cast<float>(stored.rect.width - 1);
        glyph.quadHeight = scale * static_cast<float>(stored.rect.height - 1);
        return glyph;
    };

    if (const DistanceFieldEntry* found = distanceFields_.Find(fieldKey)) {
        if (!found->supported) {
            return FindOrAddBitmap(key);
        }

        return Result<CachedGlyph>::Success(found->glyph.valid ? finalize(found->glyph)
                                                               : found->glyph);
    }

    auto geometry = GeometryOf(*key.face, key.glyphIndex);
    if (!geometry) {
        return Result<CachedGlyph>::Failure(geometry.Error());
    }

    DistanceFieldRequest request;
    request.multichannel = multichannel;
    request.approximate = true;
    request.ppem = key.distanceFieldPpem;
    request.emOuterSpread = fieldKey.emOuterSpread;
    request.emInnerSpread = fieldKey.emInnerSpread;
    GlyphBitmap placeholder(allocator_);
    DistanceFieldEntry entry;
    if (geometry.Value().Get() != nullptr) {
        if (auto made = backend_->GenerateDistanceField(*geometry.Value(), request, placeholder);
            made) {
            entry.supported = true;
        } else if (made.Error() == ErrorCode::OutOfMemory) {
            return Result<CachedGlyph>::Failure(ErrorCode::OutOfMemory);
        }
    }

    if (!entry.supported) {
        if (auto inserted =
                distanceFields_.Insert(DistanceFieldKey(fieldKey), DistanceFieldEntry(entry));
            !inserted) {
            return Result<CachedGlyph>::Failure(inserted.Error());
        }

        return FindOrAddBitmap(key);
    }

    auto placed = Place(placeholder);
    if (!placed) {
        return placed;
    }

    CachedGlyph glyph = placed.Value();
    glyph.distanceField = true;
    glyph.supportsOutline = true;
    glyph.emOuterSpread = fieldKey.emOuterSpread;
    glyph.emInnerSpread = fieldKey.emInnerSpread;
    if (glyph.valid && glyph.texture < textures_.Size() &&
        textures_[glyph.texture].page.Get() != nullptr) {
        // 거리장은 여백을 뺀 칸과 거리장 bearing을 쓴다(UE TryAddToAtlas).
        glyph.rect = {glyph.rect.x + FontAtlasPadding, glyph.rect.y + FontAtlasPadding,
                      glyph.rect.width - FontAtlasPadding * 2,
                      glyph.rect.height - FontAtlasPadding * 2};
    }

    glyph.bearingX = placeholder.bearingX;
    glyph.bearingY = placeholder.bearingY;
    // SdfApproximation은 자리표시만 쓴다(UE bPlaceholderOnly).
    glyph.pending = glyph.valid && key.mode != FontRasterMode::SdfApproximation;
    if (glyph.pending) {
        DistanceFieldJob job;
        job.key = fieldKey;
        job.geometry = geometry.Value();
        job.request = request;
        job.request.approximate = false;
        job.texture = glyph.texture;
        job.inner = glyph.rect;
        job.generation = flushGeneration_;
        if (auto queued = pendingJobs_.TryPushBack(Move(job)); !queued) {
            return Result<CachedGlyph>::Failure(queued.Error());
        }
    }

    entry.glyph = glyph;
    if (auto inserted =
            distanceFields_.Insert(DistanceFieldKey(fieldKey), DistanceFieldEntry(entry));
        !inserted) {
        if (glyph.pending) {
            (void)pendingJobs_.TryPopBack();
        }

        return Result<CachedGlyph>::Failure(inserted.Error());
    }

    return Result<CachedGlyph>::Success(glyph.valid ? finalize(glyph) : glyph);
}

Result<uint32> GlyphCache::RunPendingDistanceFields(uint32 maxJobs) noexcept {
    uint32 done = 0;
    while (done < maxJobs) {
        DistanceFieldJob job;
        {
            platform::LockGuard guard(mutex_);
            if (pendingJobs_.Size() == 0) {
                break;
            }

            job = Move(pendingJobs_[0]);
            (void)pendingJobs_.TryRemoveAt(0);
        }

        // 생성은 잠금 밖에서 한다(geometry는 불변, backend 생성은 스레드 안전 — 6.7절).
        GlyphBitmap field(allocator_);
        const Result<void> generated =
            backend_->GenerateDistanceField(*job.geometry, job.request, field);
        if (!generated && generated.Error() == ErrorCode::OutOfMemory) {
            return Result<uint32>::Failure(ErrorCode::OutOfMemory);
        }

        platform::LockGuard guard(mutex_);
        ++done;
        if (job.generation != flushGeneration_) {
            continue;
        }

        DistanceFieldEntry* entry = distanceFields_.Find(job.key);
        if (entry == nullptr) {
            continue;
        }

        if (generated && static_cast<uint32>(field.width) == job.inner.width &&
            static_cast<uint32>(field.height) == job.inner.height &&
            job.texture < textures_.Size() && textures_[job.texture].page.Get() != nullptr) {
            if (auto replaced =
                    textures_[job.texture].page->Replace(job.inner, field.pixels.View());
                !replaced) {
                Log::Warning(LogTextLayout, "Cannot replace a distance field placeholder");
            }
        }

        // 실패해도 자리표시를 그대로 쓴다(UE와 같이 다시 시도하지 않는다).
        entry->glyph.pending = false;
    }

    return Result<uint32>::Success(done);
}

uint32 GlyphCache::PendingDistanceFieldCount() const noexcept {
    platform::LockGuard guard(mutex_);
    return static_cast<uint32>(pendingJobs_.Size());
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
    distanceFields_.Clear();
    geometries_.Clear();
    pendingJobs_.Clear();
    ++flushGeneration_;
    textures_.Clear();
    alphaPages_.Clear();
    colorPages_.Clear();
    distanceFieldPages_.Clear();
    nonAtlasTextures_.Clear();
}

GlyphCacheStats GlyphCache::Stats() const noexcept {
    platform::LockGuard guard(mutex_);
    GlyphCacheStats stats;
    stats.glyphs = glyphs_.Count() + distanceFields_.Count();
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
