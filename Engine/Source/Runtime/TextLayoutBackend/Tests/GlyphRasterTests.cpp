#include "TextLibrariesTestSupport.h"

#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <TextLayout/GlyphCache.h>
#include <TextLayoutBackend/TextLayoutBackend.h>

using namespace rocks;
namespace glyph_raster_tests_file {

// FreeType 래스터 시험이다(Docs/Design/Font 6.5·6.8절 FT2d — UE SlateFontRenderer 규칙). 화소 값은
// 플랫폼·FreeType 판마다 다를 수 있어 golden 대신 구조(크기·내용 종류·coverage 범위·외곽선과
// 기울임의 효과·strike 축소 비율)를 단언한다.

namespace {

constexpr const char* RobotoPath = "Engine/Source/ThirdParty/TestFontRoboto/"
                                   "roboto-3-classic-v3.016/hinted/static/Roboto-Regular.ttf";
constexpr const char* EmojiPath =
    "Engine/Source/ThirdParty/TestFontGenerated/generated-2026-11-24/NotoColorEmoji-U1F600.ttf";
constexpr const char* NanumPath =
    "Engine/Source/ThirdParty/TestFontNanumGothic/google-fonts-06bfe0d62a98/ofl/nanumgothic/"
    "NanumGothic-Regular.ttf";

constexpr float TestSize = 24.0f;
constexpr uint32 GrinningFace = 0x1F600;
constexpr uint32 HangulHan = 0xD55C;
constexpr uint8 FullCoverage = 255;
// span·비트맵 경로 coverage 합의 허용 차이(1/50 = 2%)다.
constexpr uint64 CoverageToleranceDivisor = 50;

struct Fixture {
    SystemAllocator allocator;
    UniquePtr<text::TextBackend> backend;
    UniquePtr<text::BackendFace> face;

    bool Open(const char* path) noexcept {
        if (!textlayouttest::EnsureTextLibraries()) {
            return false;
        }

        auto created = text::CreateTextBackend({.allocator = &allocator});
        if (!created) {
            return false;
        }

        backend = Move(created.Value());
        Array<uint8> bytes(allocator);
        if (!textlayouttest::ReadTestFile(path, bytes)) {
            return false;
        }

        auto data =
            text::FontFaceData::TryCreate(allocator, Span<const uint8>(bytes.Data(), bytes.Size()));
        if (!data) {
            return false;
        }

        auto opened = backend->OpenFace({.data = Move(data.Value())});
        if (!opened) {
            return false;
        }

        face = Move(opened.Value());
        return true;
    }

    text::GlyphRasterRequest Request(uint32 codePoint, float scale = 1.0f) noexcept {
        return {.glyphIndex = face->GlyphIndex(codePoint), .size = TestSize, .scale = scale};
    }
};

bool AllZeroOrFull(const text::GlyphBitmap& bitmap) noexcept {
    for (const uint8 pixel : bitmap.pixels) {
        if (pixel != 0 && pixel != FullCoverage) {
            return false;
        }
    }

    return true;
}

bool HasFullCoverage(const text::GlyphBitmap& bitmap) noexcept {
    for (const uint8 pixel : bitmap.pixels) {
        if (pixel == FullCoverage) {
            return true;
        }
    }

    return false;
}

} // namespace

TEST(TextLayoutBackend_RasterizesAlphaGlyph) {
    Fixture fixture;
    CHECK(fixture.Open(RobotoPath));
    if (fixture.face.Get() == nullptr) {
        return;
    }

    text::GlyphBitmap bitmap(fixture.allocator);
    CHECK(static_cast<bool>(fixture.face->Rasterize(fixture.Request('A'), bitmap)));
    CHECK(bitmap.content == text::FontAtlasContent::Alpha && bitmap.supportsOutline);
    // 32 px 'A'는 대문자 높이(약 23 px)쯤이고 기준선 위에 있다.
    CHECK(bitmap.width > 15 && bitmap.width < 30 && bitmap.height > 18 && bitmap.height < 28);
    CHECK(bitmap.pixels.Size() == static_cast<usize>(bitmap.width) * bitmap.height);
    CHECK(bitmap.bearingY == bitmap.height && bitmap.bearingX >= 0);
    CHECK(bitmap.bitmapRenderScale == 1.0f && HasFullCoverage(bitmap) && !AllZeroOrFull(bitmap));

    // 안티앨리어싱을 끄면 MONO라 0·255뿐이다(num_grays 2 → 255배).
    text::GlyphRasterRequest mono = fixture.Request('A');
    mono.antialiasing = false;
    text::GlyphBitmap monoBitmap(fixture.allocator);
    CHECK(static_cast<bool>(fixture.face->Rasterize(mono, monoBitmap)));
    CHECK(monoBitmap.width > 0 && AllZeroOrFull(monoBitmap) && HasFullCoverage(monoBitmap));

    // 공백은 크기 0이다. span 경로는 UE와 같이 빈 상자를 1×1로 낸다.
    text::GlyphBitmap space(fixture.allocator);
    CHECK(static_cast<bool>(fixture.face->Rasterize(fixture.Request(' '), space)));
    CHECK(space.width == 0 && space.height == 0 && space.pixels.Size() == 0);
    text::GlyphRasterRequest spanSpace = fixture.Request(' ');
    spanSpace.spanRasterizer = true;
    CHECK(static_cast<bool>(fixture.face->Rasterize(spanSpace, space)));
    CHECK(space.width == 1 && space.height == 1 && space.pixels[0] == 0);

    // 잘못된 요청이다.
    text::GlyphRasterRequest bad = fixture.Request('A');
    bad.size = 0.0f;
    CHECK(fixture.face->Rasterize(bad, bitmap).Error() == ErrorCode::InvalidArgument);
    bad = fixture.Request('A');
    bad.glyphIndex = 1u << 20;
    CHECK(fixture.face->Rasterize(bad, bitmap).Error() == ErrorCode::InvalidArgument);
}

TEST(TextLayoutBackend_SpanRasterMatchesStandardPath) {
    // 외곽선 없이 span 경로(UE OutlineFontRenderMethod 1)로 그려도 같은 coverage다.
    Fixture fixture;
    CHECK(fixture.Open(RobotoPath));
    if (fixture.face.Get() == nullptr) {
        return;
    }

    text::GlyphBitmap standard(fixture.allocator);
    text::GlyphBitmap spans(fixture.allocator);
    text::GlyphRasterRequest request = fixture.Request('o');
    CHECK(static_cast<bool>(fixture.face->Rasterize(request, standard)));
    request.spanRasterizer = true;
    CHECK(static_cast<bool>(fixture.face->Rasterize(request, spans)));
    // span 경로는 coverage가 있는 span의 상자, 표준 경로는 윤곽 cbox라 가장자리 한 줄이 다를 수
    // 있다(UE도 같다). 직접 모드 span과 비트맵 모드는 coverage 반올림이 조금 달라(Roboto 'o'
    // 32 px에서 0.6%) 합이 2% 안에서 같다.
    const auto near = [](int32 left, int32 right) noexcept {
        return left - right <= 1 && right - left <= 1;
    };
    CHECK(near(spans.width, standard.width) && near(spans.height, standard.height));
    uint64 standardSum = 0;
    uint64 spanSum = 0;
    for (const uint8 pixel : standard.pixels) {
        standardSum += pixel;
    }

    for (const uint8 pixel : spans.pixels) {
        spanSum += pixel;
    }

    const uint64 difference = spanSum > standardSum ? spanSum - standardSum : standardSum - spanSum;
    CHECK(standardSum > 0 && difference * CoverageToleranceDivisor <= standardSum);
}

TEST(TextLayoutBackend_OutlineAndSkew) {
    Fixture fixture;
    CHECK(fixture.Open(RobotoPath));
    if (fixture.face.Get() == nullptr) {
        return;
    }

    text::GlyphBitmap plain(fixture.allocator);
    CHECK(static_cast<bool>(fixture.face->Rasterize(fixture.Request('H'), plain)));

    // 외곽선 2 px(배율 1)은 사방으로 약 2 px 키우고 bearing을 그만큼 옮긴다.
    text::GlyphRasterRequest outlined = fixture.Request('H');
    outlined.outline = {.size = 2.0f};
    text::GlyphBitmap outline(fixture.allocator);
    CHECK(static_cast<bool>(fixture.face->Rasterize(outlined, outline)));
    CHECK(outline.width >= plain.width + 3 && outline.width <= plain.width + 5);
    CHECK(outline.height >= plain.height + 3 && outline.height <= plain.height + 5);
    CHECK(outline.bearingX < plain.bearingX && outline.bearingY > plain.bearingY);

    // 배율이 외곽선 두께에도 곱해진다.
    text::GlyphRasterRequest scaled = outlined;
    scaled.scale = 2.0f;
    text::GlyphBitmap scaledOutline(fixture.allocator);
    text::GlyphBitmap scaledPlain(fixture.allocator);
    CHECK(static_cast<bool>(fixture.face->Rasterize(scaled, scaledOutline)));
    CHECK(static_cast<bool>(fixture.face->Rasterize(fixture.Request('H', 2.0f), scaledPlain)));
    CHECK(scaledOutline.width >= scaledPlain.width + 7 &&
          scaledOutline.width <= scaledPlain.width + 9);

    // separateFillAlpha는 채움 안쪽을 비운다(가운데 화소가 0에 가깝다), 아니면 채움이 255다.
    text::GlyphRasterRequest separated = outlined;
    separated.outline.separateFillAlpha = true;
    text::GlyphBitmap hollow(fixture.allocator);
    CHECK(static_cast<bool>(fixture.face->Rasterize(separated, hollow)));
    CHECK(hollow.width == outline.width && hollow.height == outline.height);
    usize differing = 0;
    for (usize index = 0; index < hollow.pixels.Size(); ++index) {
        differing += hollow.pixels[index] != outline.pixels[index] ? 1 : 0;
    }

    CHECK(differing > 0);

    // mitered 모서리는 둥근 모서리보다 모서리 화소가 많다(같거나 크다).
    text::GlyphRasterRequest mitered = outlined;
    mitered.outline.mitered = true;
    text::GlyphBitmap square(fixture.allocator);
    CHECK(static_cast<bool>(fixture.face->Rasterize(mitered, square)));
    CHECK(square.width >= outline.width && square.height >= outline.height);

    // 기울임은 위쪽을 오른쪽으로 밀어 너비를 키우고, shaping advance에는 남지 않는다.
    Array<text::ShapedGlyph> before(fixture.allocator);
    Array<text::ShapedGlyph> after(fixture.allocator);
    const text::ShapeRunRequest shape{.text = "H", .size = TestSize, .scale = 1.0f};
    CHECK(static_cast<bool>(fixture.face->Shape(shape, before)));
    text::GlyphRasterRequest skewed = fixture.Request('H');
    skewed.skew = 0.25f;
    text::GlyphBitmap slanted(fixture.allocator);
    CHECK(static_cast<bool>(fixture.face->Rasterize(skewed, slanted)));
    CHECK(slanted.width >= plain.width + 4 && slanted.height == plain.height);
    CHECK(static_cast<bool>(fixture.face->Shape(shape, after)));
    CHECK(before.Size() == 1 && after.Size() == 1 && before[0].advance == after[0].advance);
}

TEST(TextLayoutBackend_ColorBitmapStrikeScaling) {
    // CBDT 이모지(비 scalable): Color(BGRA) 내용이다. 목표가 strike보다 작으면 atlas에 넣기 전에
    // 줄이고(UE GetBitmapAtlasScale), 크면 그대로 두고 bitmapRenderScale로 GPU가 키운다.
    Fixture fixture;
    CHECK(fixture.Open(EmojiPath));
    if (fixture.face.Get() == nullptr) {
        return;
    }

    text::GlyphBitmap native(fixture.allocator);
    // 배율 8(256 px)은 strike보다 크다.
    CHECK(static_cast<bool>(fixture.face->Rasterize(fixture.Request(GrinningFace, 8.0f), native)));
    CHECK(native.content == text::FontAtlasContent::Color && !native.supportsOutline);
    CHECK(native.bitmapRenderScale > 1.0f);
    CHECK(native.pixels.Size() == static_cast<usize>(native.width) * native.height * 4);

    text::GlyphBitmap small(fixture.allocator);
    CHECK(static_cast<bool>(fixture.face->Rasterize(fixture.Request(GrinningFace), small)));
    CHECK(small.content == text::FontAtlasContent::Color && small.bitmapRenderScale == 1.0f);
    // strike ppem = 256 / bitmapRenderScale이므로 줄인 너비는 strike 너비 × 32 / ppem을 버린
    // 값(= 너비 × renderScale / 8)이다(16.16 반올림 차이 1 허용).
    const int32 expected =
        static_cast<int32>(static_cast<float>(native.width) * native.bitmapRenderScale / 8.0f);
    CHECK(small.width < native.width && small.width >= expected - 1 && small.width <= expected + 1);
    CHECK(small.pixels.Size() == static_cast<usize>(small.width) * small.height * 4);
    // 이모지의 가운데는 불투명하다(alpha 255 근처).
    const usize center =
        (static_cast<usize>(small.height / 2) * small.width + small.width / 2) * 4 + 3;
    CHECK(small.pixels[center] > 200);
}

TEST(TextLayoutBackend_GlyphCacheWithRealFaces) {
    Fixture fixture;
    CHECK(fixture.Open(NanumPath));
    if (fixture.face.Get() == nullptr) {
        return;
    }

    auto cache = text::GlyphCache::TryCreate(fixture.allocator, {});
    if (!cache) {
        CHECK(false);
        return;
    }

    const text::GlyphKey key{.face = fixture.face.Get(),
                             .glyphIndex = fixture.face->GlyphIndex(HangulHan),
                             .size = TestSize,
                             .scale = 1.0f};
    auto glyph = cache.Value()->FindOrAdd(key);
    CHECK(glyph && glyph.Value().valid && glyph.Value().content == text::FontAtlasContent::Alpha);
    if (glyph) {
        auto info = cache.Value()->Texture(glyph.Value().texture);
        CHECK(info && info.Value().atlas && info.Value().width == text::DefaultAlphaAtlasSize);
        // 칸 안쪽(여백 1 px 안)에 coverage가 있다.
        const text::AtlasRect rect = glyph.Value().rect;
        uint32 covered = 0;
        for (uint32 row = rect.y + 1; info && row + 1 < rect.y + rect.height; ++row) {
            for (uint32 column = rect.x + 1; column + 1 < rect.x + rect.width; ++column) {
                covered += info.Value().pixels[row * info.Value().width + column] != 0 ? 1 : 0;
            }
        }

        CHECK(covered > 20);
    }

    // 범위 밖 글리프는 invalid로 기억한다.
    text::GlyphKey bad = key;
    bad.glyphIndex = 1u << 20;
    auto invalid = cache.Value()->FindOrAdd(bad);
    CHECK(invalid && !invalid.Value().valid && cache.Value()->Stats().glyphs == 2);
}

} // namespace glyph_raster_tests_file
