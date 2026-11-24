#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <TextLayout/FontAtlas.h>
#include <TextLayout/GlyphCache.h>
#include <string.h>

using namespace rocks;
namespace glyph_cache_tests_file {

// atlas 칸 할당과 글리프 cache 규칙 시험이다(Docs/Design/Font 6.6·6.8절 FT2d). backend 대신 가짜
// face가 정해진 크기의 비트맵을 내므로 FreeType 없이 UE 규칙(guillotine 분할 순서, 여백 보정,
// atlas 밖 텍스처, flush 계수)을 정확히 단언한다.

namespace {

// 가장 작은 atlas 한 변(설정은 이 값으로 잘린다)이다.
constexpr uint32 SmallAtlas = text::MinFontAtlasSize;

// 가짜 face의 글리프 번호 규칙: 번호 = 한 변(화소). 아래 번호는 특별한 뜻이다.
constexpr uint32 FailingGlyph = 9000;
constexpr uint32 OutOfMemoryGlyph = 9001;
constexpr uint32 ColorGlyphBase = 5000;
constexpr int32 FakeBearingX = 3;
constexpr int32 FakeBearingY = 7;
constexpr uint8 FakeCoverage = 200;

// 결과가 모두 뻔한 가짜 face다. 래스터 호출 수를 센다.
class FakeFace final : public text::BackendFace {
public:
    Result<void> Info(text::FontFaceInfo&) const noexcept override {
        return Result<void>::Success();
    }

    Result<text::FontMetrics> Metrics(float, float) noexcept override {
        return Result<text::FontMetrics>::Success({});
    }

    uint32 GlyphIndex(uint32 codePoint) noexcept override {
        return codePoint;
    }

    Result<void> Shape(const text::ShapeRunRequest&, Array<text::ShapedGlyph>&) noexcept override {
        return Result<void>::Success();
    }

    Result<void> Rasterize(const text::GlyphRasterRequest& request,
                           text::GlyphBitmap& out) noexcept override {
        ++rasterCount;
        if (request.glyphIndex == FailingGlyph) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        if (request.glyphIndex == OutOfMemoryGlyph) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        const bool color = request.glyphIndex >= ColorGlyphBase;
        const uint32 side = color ? request.glyphIndex - ColorGlyphBase : request.glyphIndex;
        out.content = color ? text::FontAtlasContent::Color : text::FontAtlasContent::Alpha;
        out.width = static_cast<int32>(side);
        out.height = static_cast<int32>(side);
        out.bearingX = FakeBearingX;
        out.bearingY = FakeBearingY;
        out.supportsOutline = true;
        const usize bytes =
            static_cast<usize>(side) * side * text::FontAtlasBytesPerPixel(out.content);
        if (!out.pixels.TryResize(bytes)) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        memset(out.pixels.Data(), FakeCoverage, bytes);
        return Result<void>::Success();
    }

    int32 rasterCount = 0;
};

constexpr text::AtlasRect Rect(uint32 x, uint32 y, uint32 width, uint32 height) noexcept {
    return {x, y, width, height};
}

text::GlyphKey Key(FakeFace& face, uint32 glyph) noexcept {
    return {.face = &face, .glyphIndex = glyph, .size = 24.0f, .scale = 1.0f};
}

UniquePtr<text::GlyphCache> MakeCache(SystemAllocator& allocator) noexcept {
    auto cache = text::GlyphCache::TryCreate(
        allocator, {.alphaAtlasSize = SmallAtlas, .colorAtlasSize = SmallAtlas});
    return cache ? Move(cache.Value()) : UniquePtr<text::GlyphCache>();
}

} // namespace

TEST(TextLayout_AtlasPageSplitsLikeUnreal) {
    SystemAllocator allocator;
    auto page = text::FontAtlasPage::TryCreate(allocator, SmallAtlas, SmallAtlas,
                                               text::FontAtlasContent::Alpha);
    CHECK(static_cast<bool>(page));
    if (!page) {
        return;
    }

    uint8 pixels[20 * 20];
    memset(pixels, FakeCoverage, sizeof(pixels));
    const Span<const uint8> tenByTen(pixels, 100);
    text::AtlasRect slot;
    // 10×10(여백 포함 12×12) 첫 칸은 원점이다. 남는 높이 116 ≤ 너비 116이라 세로로 나뉘어
    // 오른쪽 (12,0,116,128)과 아래 (0,12,12,116)가 남는다.
    auto first = page.Value()->Add(10, 10, tenByTen, slot);
    CHECK(first && first.Value() && slot == Rect(0, 0, 12, 12));
    // 같은 너비 구간(16)의 아래 조각이 먼저 쓰인다.
    auto second = page.Value()->Add(10, 10, tenByTen, slot);
    CHECK(second && second.Value() && slot == Rect(0, 12, 12, 12));
    auto third = page.Value()->Add(10, 10, tenByTen, slot);
    CHECK(third && third.Value() && slot == Rect(0, 24, 12, 12));
    // 22 너비는 구간 32부터 찾으므로 오른쪽 큰 조각에 간다.
    auto wide = page.Value()->Add(20, 20, Span<const uint8>(pixels, sizeof(pixels)), slot);
    CHECK(wide && wide.Value() && slot == Rect(12, 0, 22, 22));

    // 화소는 여백 1 px 안쪽에 복사되고 여백은 0이다.
    const Span<const uint8> atlas = page.Value()->Pixels();
    CHECK(atlas[0] == 0 && atlas[1] == 0 && atlas[SmallAtlas] == 0);
    CHECK(atlas[SmallAtlas + 1] == FakeCoverage);
    CHECK(atlas[10 * SmallAtlas + 10] == FakeCoverage && atlas[11 * SmallAtlas + 11] == 0);

    // 바뀐 사각형은 넣은 칸의 합이고, 가져가면 비워진다.
    text::AtlasRect dirty;
    CHECK(page.Value()->TakeDirtyRegion(dirty) && dirty == Rect(0, 0, 34, 36));
    CHECK(!page.Value()->TakeDirtyRegion(dirty));
}

TEST(TextLayout_AtlasPageEdgeCases) {
    SystemAllocator allocator;
    CHECK(text::FontAtlasPage::TryCreate(allocator, 0, SmallAtlas, text::FontAtlasContent::Alpha)
              .Error() == ErrorCode::InvalidArgument);
    auto page = text::FontAtlasPage::TryCreate(allocator, SmallAtlas, SmallAtlas,
                                               text::FontAtlasContent::Color);
    if (!page) {
        CHECK(false);
        return;
    }

    text::AtlasRect slot;
    // 크기 0(공백)도 여백만 있는 칸을 받고 화소는 바뀌지 않는다.
    auto empty = page.Value()->Add(0, 0, {}, slot);
    CHECK(empty && empty.Value() && slot == Rect(0, 0, 2, 2));
    text::AtlasRect dirty;
    CHECK(!page.Value()->TakeDirtyRegion(dirty));
    // Color는 화소 4 byte다 — 수가 맞지 않으면 InvalidArgument다.
    uint8 one[4] = {1, 2, 3, 4};
    CHECK(page.Value()->Add(1, 1, Span<const uint8>(one, 3), slot).Error() ==
          ErrorCode::InvalidArgument);
    auto fitted = page.Value()->Add(1, 1, Span<const uint8>(one, 4), slot);
    CHECK(fitted && fitted.Value());
    // 여백을 더하면 페이지보다 큰 글리프는 자리가 없다.
    Array<uint8> big(allocator);
    CHECK(static_cast<bool>(big.TryResize(static_cast<usize>(SmallAtlas) * SmallAtlas * 4)));
    auto tooBig = page.Value()->Add(SmallAtlas, SmallAtlas, big.View(), slot);
    CHECK(tooBig && !tooBig.Value());

    // 꽉 차면 false다(126×126은 첫 페이지에 하나만 들어간다).
    auto fresh = text::FontAtlasPage::TryCreate(allocator, SmallAtlas, SmallAtlas,
                                                text::FontAtlasContent::Alpha);
    if (fresh) {
        Array<uint8> large(allocator);
        CHECK(static_cast<bool>(large.TryResize(126 * 126)));
        auto once = fresh.Value()->Add(126, 126, large.View(), slot);
        auto twice = fresh.Value()->Add(126, 126, large.View(), slot);
        CHECK(once && once.Value() && twice && !twice.Value());
    }
}

TEST(TextLayout_GlyphCacheFindsAndRemembers) {
    SystemAllocator allocator;
    FakeFace face;
    auto cache = MakeCache(allocator);
    if (cache.Get() == nullptr) {
        CHECK(false);
        return;
    }

    auto glyph = cache->FindOrAdd(Key(face, 10));
    CHECK(glyph && glyph.Value().valid);
    if (glyph) {
        // bearing은 여백만큼 보정한다(UE: x − 여백, y + 여백).
        CHECK(glyph.Value().rect == Rect(0, 0, 12, 12));
        CHECK(glyph.Value().bearingX == FakeBearingX - 1 &&
              glyph.Value().bearingY == FakeBearingY + 1);
        CHECK(glyph.Value().content == text::FontAtlasContent::Alpha && glyph.Value().texture == 0);
    }

    // 같은 키는 다시 래스터하지 않는다. 기울임·외곽선이 다르면 다른 키다.
    auto again = cache->FindOrAdd(Key(face, 10));
    CHECK(again && again.Value().rect == Rect(0, 0, 12, 12) && face.rasterCount == 1);
    text::GlyphKey skewed = Key(face, 10);
    skewed.skew = 0.2f;
    text::GlyphKey outlined = Key(face, 10);
    outlined.outline.size = 1.0f;
    CHECK(cache->FindOrAdd(skewed) && cache->FindOrAdd(outlined) && face.rasterCount == 3);

    // 래스터 실패는 invalid로 기억한다. 할당 실패는 기억하지 않고 오류다.
    auto failed = cache->FindOrAdd(Key(face, FailingGlyph));
    CHECK(failed && !failed.Value().valid);
    CHECK(cache->FindOrAdd(Key(face, FailingGlyph)) && face.rasterCount == 4);
    CHECK(cache->FindOrAdd(Key(face, OutOfMemoryGlyph)).Error() == ErrorCode::OutOfMemory);
    CHECK(cache->FindOrAdd(Key(face, OutOfMemoryGlyph)).Error() == ErrorCode::OutOfMemory);
    CHECK(face.rasterCount == 6);

    // 컬러는 따로 페이지를 쓴다.
    auto color = cache->FindOrAdd(Key(face, ColorGlyphBase + 4));
    CHECK(color && color.Value().valid && color.Value().content == text::FontAtlasContent::Color);
    CHECK(color && color.Value().texture == 1);
    const text::GlyphCacheStats stats = cache->Stats();
    CHECK(stats.glyphs == 5 && stats.alphaPages == 1 && stats.colorPages == 1);
    CHECK(stats.textureBytes == SmallAtlas * SmallAtlas * 5);

    // 잘못된 키다.
    CHECK(cache->FindOrAdd({.face = nullptr, .size = 24.0f}).Error() == ErrorCode::InvalidArgument);
    CHECK(cache->FindOrAdd({.face = &face, .size = 0.0f}).Error() == ErrorCode::InvalidArgument);
    CHECK(cache->Texture(cache->TextureCount()).Error() == ErrorCode::InvalidArgument);
}

TEST(TextLayout_GlyphCacheTexturesAndDirtyRegions) {
    SystemAllocator allocator;
    FakeFace face;
    // atlas 크기는 128–2048로 잘린다.
    auto cache = text::GlyphCache::TryCreate(allocator, {.alphaAtlasSize = 16});
    if (!cache) {
        CHECK(false);
        return;
    }

    CHECK(static_cast<bool>(cache.Value()->FindOrAdd(Key(face, 10))));
    auto info = cache.Value()->Texture(0);
    CHECK(info && info.Value().atlas && info.Value().width == SmallAtlas);
    CHECK(info && info.Value().pixels.Size() == SmallAtlas * SmallAtlas);
    // 새 텍스처는 전체를 올리고, 그다음은 넣은 칸만이다.
    text::AtlasRect dirty;
    CHECK(cache.Value()->TakeDirtyRegion(0, dirty) && dirty == Rect(0, 0, SmallAtlas, SmallAtlas));
    CHECK(!cache.Value()->TakeDirtyRegion(0, dirty));
    CHECK(static_cast<bool>(cache.Value()->FindOrAdd(Key(face, 11))));
    // 13 너비는 아래 조각(너비 12)에 맞지 않아 오른쪽 조각에 간다.
    CHECK(cache.Value()->TakeDirtyRegion(0, dirty) && dirty == Rect(12, 0, 13, 13));
    CHECK(!cache.Value()->TakeDirtyRegion(7, dirty));

    // 한 변이 atlas보다 크면 atlas 밖 텍스처다(여백·bearing 보정 없음).
    auto big = cache.Value()->FindOrAdd(Key(face, SmallAtlas + 1));
    CHECK(big && big.Value().valid && big.Value().texture == 1);
    CHECK(big && big.Value().rect == Rect(0, 0, SmallAtlas + 1, SmallAtlas + 1));
    CHECK(big && big.Value().bearingX == FakeBearingX && big.Value().bearingY == FakeBearingY);
    auto bigInfo = cache.Value()->Texture(1);
    CHECK(bigInfo && !bigInfo.Value().atlas && bigInfo.Value().width == SmallAtlas + 1);
    CHECK(cache.Value()->Stats().nonAtlasTextures == 1);
    // 한 변이 atlas와 같으면 여백 때문에 새 페이지에도 들어가지 않는다(UE와 같이 invalid).
    auto edge = cache.Value()->FindOrAdd(Key(face, SmallAtlas));
    CHECK(edge && !edge.Value().valid && cache.Value()->Stats().alphaPages == 2);
}

TEST(TextLayout_GlyphCacheFlushCountersMatchUnreal) {
    SystemAllocator allocator;
    FakeFace face;
    auto cache = MakeCache(allocator);
    if (cache.Get() == nullptr) {
        CHECK(false);
        return;
    }

    // 100×100은 128 페이지에 하나씩만 들어간다. 프레임 0에 두 번째 페이지가 생기면 마지막 요청
    // (생성 때 = 프레임 0)에서 성장 창(1) 안이라 한도를 2로 올리고 비우지 않는다.
    CHECK(static_cast<bool>(cache->FindOrAdd(Key(face, 100))));
    CHECK(static_cast<bool>(cache->FindOrAdd(Key(face, 101))));
    CHECK(cache->Stats().alphaPages == 2 && !cache->AdvanceFrame());
    CHECK(!cache->AdvanceFrame() && !cache->AdvanceFrame());
    // 프레임 3에 세 번째 페이지는 창 밖이라 flush를 요청한다. 그 프레임 안에서는 글리프가 남는다.
    auto third = cache->FindOrAdd(Key(face, 102));
    CHECK(third && third.Value().valid && cache->Stats().alphaPages == 3);
    CHECK(cache->AdvanceFrame());
    CHECK(cache->Stats().glyphs == 0 && cache->TextureCount() == 0);

    // 요청(프레임 3) 직후 창 안(프레임 4)에서 다시 늘면 한도를 올리고 비우지 않는다.
    CHECK(static_cast<bool>(cache->FindOrAdd(Key(face, 100))));
    CHECK(static_cast<bool>(cache->FindOrAdd(Key(face, 101))));
    CHECK(!cache->AdvanceFrame());

    // 명시 요청은 다음 프레임 경계에서 비운다.
    cache->RequestFlush();
    CHECK(cache->Stats().glyphs == 2);
    CHECK(cache->AdvanceFrame() && cache->Stats().glyphs == 0 && !cache->AdvanceFrame());
}

TEST(TextLayout_GlyphCacheTextureLimit) {
    SystemAllocator allocator;
    FakeFace face;
    auto cache = MakeCache(allocator);
    if (cache.Get() == nullptr) {
        CHECK(false);
        return;
    }

    // atlas 밖 텍스처 255개(UE uint8 번호)까지 받고 그다음은 invalid다.
    for (uint32 index = 0; index < text::MaxFontTextures; ++index) {
        auto glyph = cache->FindOrAdd(Key(face, SmallAtlas + 1 + index % 2 + 2 * (index / 2)));
        CHECK(glyph && glyph.Value().valid);
    }

    CHECK(cache->TextureCount() == text::MaxFontTextures);
    auto over = cache->FindOrAdd(Key(face, SmallAtlas + 1000));
    CHECK(over && !over.Value().valid && cache->TextureCount() == text::MaxFontTextures);
}

} // namespace glyph_cache_tests_file
