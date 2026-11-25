#include "TextLibrariesTestSupport.h"

#include <Base/InitializerList.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <TextLayout/GlyphCache.h>
#include <TextLayout/GlyphGeometry.h>
#include <TextLayoutBackend/TextLayoutBackend.h>

using namespace rocks;
namespace distance_field_tests_file {

// 전처리 geometry·거리장 시험이다(Docs/Design/Font 6.5·6.8절 FT2e — UE SlateSdfGenerator 규칙).
// 거리 값은 플랫폼마다 다를 수 있어 구조(크기·채널·안팎 부호·결정성·자리표시 교체)를 단언한다.

namespace {

constexpr const char* RobotoPath = "Engine/Source/ThirdParty/TestFontRoboto/"
                                   "roboto-3-classic-v3.016/hinted/static/Roboto-Regular.ttf";
constexpr const char* EmojiPath =
    "Engine/Source/ThirdParty/TestFontGenerated/generated-2026-11-24/NotoColorEmoji-U1F600.ttf";

constexpr float TestSize = 24.0f;
constexpr uint32 TestPpem = 32;
// 32 px 목표·ppem 32의 안쪽·바깥 spread(2 화소 = 1/16 em)다.
constexpr float TestSpread = 2.0f / 32.0f;
constexpr uint32 RobotoUnitsPerEm = 2048;
constexpr uint8 DistanceMidpoint = 128;
constexpr usize MtsdfChannels = 4;
constexpr usize TrueDistanceChannel = 3;

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

    text::DistanceFieldRequest Request(bool multichannel, bool approximate = false) const noexcept {
        return {.multichannel = multichannel,
                .approximate = approximate,
                .ppem = TestPpem,
                .emOuterSpread = TestSpread,
                .emInnerSpread = TestSpread};
    }
};

// 화소 (x, y)의 거리 값(MTSDF는 실제 거리 채널)이다.
uint8 DistanceAt(const text::GlyphBitmap& bitmap, int32 x, int32 y) noexcept {
    const usize channels =
        bitmap.content == text::FontAtlasContent::DistanceField ? MtsdfChannels : 1;
    const usize channel = channels == MtsdfChannels ? TrueDistanceChannel : 0;
    return bitmap.pixels[(static_cast<usize>(y) * bitmap.width + x) * channels + channel];
}

} // namespace

TEST(TextLayoutBackend_ExtractsGlyphGeometry) {
    Fixture fixture;
    CHECK(fixture.Open(RobotoPath));
    if (fixture.face.Get() == nullptr) {
        return;
    }

    text::GlyphGeometry geometry(fixture.allocator);
    CHECK(
        static_cast<bool>(fixture.face->ExtractGeometry(fixture.face->GlyphIndex('O'), geometry)));
    // 'O'는 바깥·안쪽 윤곽 둘이고 TrueType이라 2차 곡선이 있다.
    CHECK(geometry.unitsPerEm == RobotoUnitsPerEm && geometry.contourEdgeEnds.Size() == 2);
    bool quadratic = false;
    usize points = 0;
    for (const text::GeometryEdgeKind kind : geometry.edgeKinds) {
        quadratic = quadratic || kind == text::GeometryEdgeKind::Quadratic;
        points += static_cast<usize>(kind) + 1;
    }

    CHECK(quadratic && points == geometry.points.Size());

    // 직렬화 왕복 뒤 같은 거리장이 나온다(geometry만으로 결정적이다).
    Array<uint8> bytes(fixture.allocator);
    CHECK(static_cast<bool>(text::WriteGlyphGeometry(geometry, bytes)));
    text::GlyphGeometry read(fixture.allocator);
    CHECK(static_cast<bool>(text::ReadGlyphGeometry(bytes.View(), read)));
    text::GlyphBitmap first(fixture.allocator);
    text::GlyphBitmap second(fixture.allocator);
    CHECK(static_cast<bool>(
        fixture.backend->GenerateDistanceField(geometry, fixture.Request(true), first)));
    CHECK(static_cast<bool>(
        fixture.backend->GenerateDistanceField(read, fixture.Request(true), second)));
    bool same = first.pixels.Size() == second.pixels.Size() && first.pixels.Size() > 0;
    for (usize index = 0; same && index < first.pixels.Size(); ++index) {
        same = first.pixels[index] == second.pixels[index];
    }

    CHECK(same);

    // 공백(윤곽 없음)·비트맵 글꼴은 Unsupported, 범위 밖 글리프는 InvalidArgument다.
    CHECK(fixture.face->ExtractGeometry(fixture.face->GlyphIndex(' '), geometry).Error() ==
          ErrorCode::Unsupported);
    CHECK(fixture.face->ExtractGeometry(1u << 20, geometry).Error() == ErrorCode::InvalidArgument);
    Fixture emoji;
    CHECK(emoji.Open(EmojiPath));
    if (emoji.face.Get() != nullptr) {
        CHECK(emoji.face->ExtractGeometry(emoji.face->GlyphIndex(0x1F600), geometry).Error() ==
              ErrorCode::Unsupported);
    }
}

TEST(TextLayoutBackend_GeneratesDistanceFields) {
    Fixture fixture;
    CHECK(fixture.Open(RobotoPath));
    if (fixture.face.Get() == nullptr) {
        return;
    }

    text::GlyphGeometry geometry(fixture.allocator);
    CHECK(
        static_cast<bool>(fixture.face->ExtractGeometry(fixture.face->GlyphIndex('I'), geometry)));

    text::GlyphBitmap mtsdf(fixture.allocator);
    text::GlyphBitmap sdf(fixture.allocator);
    text::GlyphBitmap approximate(fixture.allocator);
    CHECK(static_cast<bool>(
        fixture.backend->GenerateDistanceField(geometry, fixture.Request(true), mtsdf)));
    CHECK(static_cast<bool>(
        fixture.backend->GenerateDistanceField(geometry, fixture.Request(false), sdf)));
    CHECK(static_cast<bool>(fixture.backend->GenerateDistanceField(
        geometry, fixture.Request(true, true), approximate)));

    CHECK(mtsdf.content == text::FontAtlasContent::DistanceField &&
          sdf.content == text::FontAtlasContent::Alpha);
    CHECK(mtsdf.pixels.Size() == static_cast<usize>(mtsdf.width) * mtsdf.height * MtsdfChannels);
    CHECK(sdf.pixels.Size() == static_cast<usize>(sdf.width) * sdf.height);
    // 자리표시와 최종 결과는 크기·bearing이 같다(같은 칸을 덮어쓴다).
    CHECK(approximate.width == mtsdf.width && approximate.height == mtsdf.height);
    CHECK(sdf.width == mtsdf.width && sdf.bearingX == mtsdf.bearingX &&
          sdf.bearingY == mtsdf.bearingY);
    // 'I'는 32 px에서 약 23 px 높이·3 px 너비이고 spread 2 px + 반 화소씩 더해진다.
    CHECK(mtsdf.height >= 26 && mtsdf.height <= 32 && mtsdf.width >= 6 && mtsdf.width <= 12);
    CHECK(mtsdf.bearingY > 20 && mtsdf.bearingX <= 2);

    // 가운데는 안(> 128), 모서리는 밖(< 128)이다. 첫 행이 위다.
    for (const text::GlyphBitmap* bitmap : {&mtsdf, &sdf, &approximate}) {
        CHECK(DistanceAt(*bitmap, bitmap->width / 2, bitmap->height / 2) > DistanceMidpoint);
        CHECK(DistanceAt(*bitmap, 0, 0) < DistanceMidpoint);
        CHECK(DistanceAt(*bitmap, bitmap->width - 1, bitmap->height - 1) < DistanceMidpoint);
    }

    // 자리표시는 한 값을 네 채널에 쓴다(UE MakePlaceholder).
    const usize center =
        (static_cast<usize>(approximate.height / 2) * approximate.width + approximate.width / 2) *
        MtsdfChannels;
    CHECK(approximate.pixels[center] == approximate.pixels[center + 3]);

    // 잘못된 요청이다.
    text::DistanceFieldRequest bad = fixture.Request(true);
    bad.ppem = 0;
    CHECK(fixture.backend->GenerateDistanceField(geometry, bad, mtsdf).Error() ==
          ErrorCode::InvalidArgument);
    bad = fixture.Request(true);
    bad.emInnerSpread = 0.0f;
    CHECK(fixture.backend->GenerateDistanceField(geometry, bad, mtsdf).Error() ==
          ErrorCode::InvalidArgument);
    text::GlyphGeometry empty(fixture.allocator);
    CHECK(fixture.backend->GenerateDistanceField(empty, fixture.Request(true), mtsdf).Error() ==
          ErrorCode::Unsupported);
}

TEST(TextLayoutBackend_GlyphCacheDistanceFieldPlaceholders) {
    Fixture fixture;
    CHECK(fixture.Open(RobotoPath));
    if (fixture.face.Get() == nullptr) {
        return;
    }

    auto created = text::GlyphCache::TryCreate(fixture.allocator, {}, fixture.backend.Get());
    if (!created) {
        CHECK(false);
        return;
    }

    text::GlyphCache& cache = *created.Value();
    const auto key = [&](uint32 codePoint, text::FontRasterMode mode,
                         float size = TestSize) noexcept {
        return text::GlyphKey{.face = fixture.face.Get(),
                              .glyphIndex = fixture.face->GlyphIndex(codePoint),
                              .size = size,
                              .scale = 1.0f,
                              .mode = mode,
                              .distanceFieldPpem = TestPpem};
    };

    // Msdf: 자리표시를 넣고 작업 하나가 남는다.
    auto placeholder = cache.FindOrAdd(key('I', text::FontRasterMode::Msdf));
    CHECK(placeholder && placeholder.Value().valid && placeholder.Value().distanceField);
    CHECK(placeholder && placeholder.Value().pending && cache.PendingDistanceFieldCount() == 1);
    CHECK(placeholder && placeholder.Value().content == text::FontAtlasContent::DistanceField);
    CHECK(placeholder && placeholder.Value().emInnerSpread == TestSpread &&
          placeholder.Value().emOuterSpread == TestSpread);
    // 목표 32 px = ppem이라 사각형 배율은 1이다(가장자리 반 화소를 뺀다).
    CHECK(placeholder &&
          placeholder.Value().quadWidth == static_cast<float>(placeholder.Value().rect.width - 1));
    CHECK(cache.Stats().distanceFieldPages == 1);

    // 25 pt(33 px)도 spread가 같은 2 화소로 올라가 같은 거리장을 쓴다.
    auto nearby = cache.FindOrAdd(key('I', text::FontRasterMode::Msdf, 25.0f));
    CHECK(nearby && placeholder && nearby.Value().rect == placeholder.Value().rect);
    CHECK(cache.PendingDistanceFieldCount() == 1);

    // 작업을 하면 같은 칸이 바뀌고 pending이 풀린다.
    if (placeholder) {
        auto before = cache.Texture(placeholder.Value().texture);
        text::AtlasRect dirty;
        CHECK(cache.TakeDirtyRegion(placeholder.Value().texture, dirty));
        auto ran = cache.RunPendingDistanceFields(8);
        CHECK(ran && ran.Value() == 1 && cache.PendingDistanceFieldCount() == 0);
        CHECK(cache.TakeDirtyRegion(placeholder.Value().texture, dirty) &&
              dirty == placeholder.Value().rect);
        CHECK(before && before.Value().atlas);
    }

    auto finished = cache.FindOrAdd(key('I', text::FontRasterMode::Msdf));
    CHECK(finished && !finished.Value().pending && placeholder &&
          finished.Value().rect == placeholder.Value().rect);

    // Sdf는 Alpha 페이지, SdfApproximation은 자리표시만 쓴다(작업 없음).
    auto single = cache.FindOrAdd(key('I', text::FontRasterMode::Sdf));
    CHECK(single && single.Value().distanceField &&
          single.Value().content == text::FontAtlasContent::Alpha);
    CHECK(cache.PendingDistanceFieldCount() == 1);
    auto approximate = cache.FindOrAdd(key('L', text::FontRasterMode::SdfApproximation));
    CHECK(approximate && approximate.Value().distanceField && !approximate.Value().pending);
    CHECK(cache.PendingDistanceFieldCount() == 1);

    // 거리장을 만들 수 없는 글리프(공백)는 비트맵으로 그린다.
    auto space = cache.FindOrAdd(key(' ', text::FontRasterMode::Msdf));
    CHECK(space && space.Value().valid && !space.Value().distanceField);

    // 생성 전에 flush되면 작업을 버린다.
    cache.RequestFlush();
    CHECK(cache.AdvanceFrame() && cache.PendingDistanceFieldCount() == 0);
    auto none = cache.RunPendingDistanceFields(8);
    CHECK(none && none.Value() == 0 && cache.Stats().glyphs == 0);
}

} // namespace distance_field_tests_file
