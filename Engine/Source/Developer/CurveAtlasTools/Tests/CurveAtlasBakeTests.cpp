#include <Base/Compiler.h>
#include <Base/InitializerList.h>
#include <Core/Math/Float16.h>
#include <Core/Memory/SystemAllocator.h>
#include <CurveAtlas/ColorCurveAtlas.h>
#include <CurveAtlas/CurveAtlasConstants.h>
#include <CurveAtlasTools/ColorCurveAtlasBake.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 색 커브 atlas 굽기 시험이다(Docs/Design/CurveAssets 4.3절 — CA6): 표본 시간(UE 역수 곱 — 나눗셈과
// 갈리는 너비), 너비 0·1 → 2, 높이 max(커브 수, 1), null 커브·커브 없음 흰색, 조정 적용·원값,
// float16 반올림(UE FFloat16 Set 규칙 — RNE)과 byte 배치(행 우선·little-endian), 크기 상한, 할당
// 실패의 out 불변, atlas 기본 설정(UE 생성자 값)과 RowOf.
using namespace rocks;

namespace curve_atlas_bake_tests_file {

namespace {

// texel (x, row)의 성분 channel(0 r, 1 g, 2 b, 3 a) half bit다.
uint16 HalfAt(const ColorCurveAtlasPixels& pixels, uint32 x, uint32 row, uint32 channel) noexcept {
    const usize at = (static_cast<usize>(row) * pixels.sizeX + x) * ColorCurveAtlasTexelBytes +
                     static_cast<usize>(channel) * 2;
    return static_cast<uint16>(pixels.bytes[at] | (pixels.bytes[at + 1] << 8));
}

uint16 HalfOf(float value) noexcept {
    return Float16::FromFloat(value).bits;
}

// 시간 0에서 0, 1에서 1인 선형 R 채널(G·B·A 키 없음)이다.
bool MakeRamp(ColorCurveAsset& curve) noexcept {
    return curve.r.TryAddKey(0.0f, 0.0f) && curve.r.TryAddKey(1.0f, 1.0f) &&
           curve.r.TrySetKeyInterp(0, CurveInterp::Linear);
}

} // namespace

// 표본 시간은 float(x) × (1 / float(W − 1))이다(UE PushToSourceData). W = 11, x = 9는 역수 곱
// 0.90000004와 나눗셈 0.89999998이 갈린다 — 그 시간에 둔 Constant 키가 역수 곱에서만 보인다.
TEST(CurveAtlasBake_SampleTimes) {
    SystemAllocator allocator;
    constexpr uint32 Width = 11;
    constexpr uint32 Probe = 9;
    const float keyTime = static_cast<float>(Probe) * (1.0f / static_cast<float>(Width - 1));
    REQUIRE(static_cast<float>(Probe) / static_cast<float>(Width - 1) < keyTime);
    ColorCurveAsset step(allocator);
    REQUIRE(step.r.TryAddKey(0.0f, 0.0f) && step.r.TryAddKey(keyTime, 1.0f));
    REQUIRE(step.r.TrySetKeyInterp(0, CurveInterp::Constant));
    ColorCurveAsset ramp(allocator);
    REQUIRE(MakeRamp(ramp));
    const ColorCurveAsset* curves[] = {&step, &ramp};
    ColorCurveAtlasPixels pixels(allocator);
    REQUIRE(BakeColorCurveAtlas(Span<const ColorCurveAsset* const>(curves), Width, true, pixels));
    REQUIRE(pixels.sizeX == Width && pixels.sizeY == 2 &&
            pixels.bytes.Size() == Width * 2 * ColorCurveAtlasTexelBytes);
    CHECK(HalfAt(pixels, Probe - 1, 0, 0) == HalfOf(0.0f));
    CHECK(HalfAt(pixels, Probe, 0, 0) == HalfOf(1.0f));
    for (uint32 x = 0; x < Width; ++x) {
        const float time = static_cast<float>(x) * (1.0f / static_cast<float>(Width - 1));
        CHECK(HalfAt(pixels, x, 1, 0) == HalfOf(ramp.EvaluateUnadjusted(time).r));
        // 키 없는 G·B는 0, 알파 키가 없으면 A = 1(UE GetUnadjustedLinearColorValue).
        CHECK(HalfAt(pixels, x, 1, 1) == HalfOf(0.0f) && HalfAt(pixels, x, 1, 2) == HalfOf(0.0f));
        CHECK(HalfAt(pixels, x, 1, 3) == HalfOf(1.0f));
    }

    // 끝 texel은 시간 1이다.
    CHECK(HalfAt(pixels, Width - 1, 1, 0) == HalfOf(1.0f));
}

// 너비 0·1은 2로 올린다(UE max(TextureSize, 2)). 커브가 없으면 흰 한 줄, null 커브는 흰 행이다.
TEST(CurveAtlasBake_SizesAndWhiteRows) {
    SystemAllocator allocator;
    ColorCurveAsset ramp(allocator);
    REQUIRE(MakeRamp(ramp));
    for (uint32 width : {0u, 1u, 2u}) {
        const ColorCurveAsset* curves[] = {&ramp};
        ColorCurveAtlasPixels pixels(allocator);
        REQUIRE(
            BakeColorCurveAtlas(Span<const ColorCurveAsset* const>(curves), width, true, pixels));
        CHECK(pixels.sizeX == MinColorCurveAtlasWidth && pixels.sizeY == 1);
        CHECK(HalfAt(pixels, 0, 0, 0) == HalfOf(0.0f) && HalfAt(pixels, 1, 0, 0) == HalfOf(1.0f));
    }

    ColorCurveAtlasPixels empty(allocator);
    REQUIRE(BakeColorCurveAtlas(Span<const ColorCurveAsset* const>(), 4, false, empty));
    REQUIRE(empty.sizeX == 4 && empty.sizeY == 1);
    for (uint32 x = 0; x < 4; ++x) {
        for (uint32 channel = 0; channel < 4; ++channel) {
            CHECK(HalfAt(empty, x, 0, channel) == HalfOf(1.0f));
        }
    }

    const ColorCurveAsset* mixed[] = {nullptr, &ramp, nullptr};
    ColorCurveAtlasPixels rows(allocator);
    REQUIRE(BakeColorCurveAtlas(Span<const ColorCurveAsset* const>(mixed), 3, false, rows));
    REQUIRE(rows.sizeX == 3 && rows.sizeY == 3);
    for (uint32 row : {0u, 2u}) {
        for (uint32 x = 0; x < 3; ++x) {
            for (uint32 channel = 0; channel < 4; ++channel) {
                CHECK(HalfAt(rows, x, row, channel) == HalfOf(1.0f));
            }
        }
    }

    CHECK(HalfAt(rows, 2, 1, 0) == HalfOf(ramp.Evaluate(1.0f).r));
}

// 조정 적용(UE GetLinearColorValue — Evaluate)과 원값(disableAdjustments — EvaluateUnadjusted).
TEST(CurveAtlasBake_Adjustments) {
    SystemAllocator allocator;
    ColorCurveAsset curve(allocator);
    REQUIRE(curve.r.TryAddKey(0.0f, 0.5f) && curve.g.TryAddKey(0.0f, 0.25f) &&
            curve.b.TryAddKey(0.0f, -0.5f) && curve.a.TryAddKey(0.0f, 0.5f));
    curve.adjustBrightness = 0.5f;
    curve.adjustMinAlpha = 0.2f;
    const ColorCurveAsset* curves[] = {&curve};
    ColorCurveAtlasPixels adjusted(allocator);
    ColorCurveAtlasPixels raw(allocator);
    REQUIRE(BakeColorCurveAtlas(Span<const ColorCurveAsset* const>(curves), 2, false, adjusted));
    REQUIRE(BakeColorCurveAtlas(Span<const ColorCurveAsset* const>(curves), 2, true, raw));
    const LinearColor expectedAdjusted = curve.Evaluate(0.0f);
    const LinearColor expectedRaw = curve.EvaluateUnadjusted(0.0f);
    const float adjustedChannels[] = {expectedAdjusted.r, expectedAdjusted.g, expectedAdjusted.b,
                                      expectedAdjusted.a};
    const float rawChannels[] = {expectedRaw.r, expectedRaw.g, expectedRaw.b, expectedRaw.a};
    for (uint32 channel = 0; channel < 4; ++channel) {
        CHECK(HalfAt(adjusted, 0, 0, channel) == HalfOf(adjustedChannels[channel]));
        CHECK(HalfAt(raw, 0, 0, channel) == HalfOf(rawChannels[channel]));
    }

    // 원값은 음수를 보존한다(UE bDisableAllAdjustments의 목적).
    CHECK(HalfAt(raw, 0, 0, 2) == HalfOf(-0.5f));
    CHECK(HalfAt(adjusted, 0, 0, 3) != HalfAt(raw, 0, 0, 3));
}

// float16은 round-to-nearest-even이고 byte는 little-endian이다. 2 = 0x4000, 1 + 2^-11(1과 다음
// 수의 정확한 중간)은 짝수 쪽 1 = 0x3C00, 1 + 3 × 2^-11은 0x3C02, 65520 이상은 Inf.
TEST(CurveAtlasBake_HalfEncoding) {
    SystemAllocator allocator;
    const float values[] = {2.0f, 1.0f + 1.0f / 2048.0f, 1.0f + 3.0f / 2048.0f, 70000.0f};
    const uint16 expected[] = {0x4000, 0x3C00, 0x3C02, 0x7C00};
    for (usize at = 0; at < 4; ++at) {
        ColorCurveAsset curve(allocator);
        REQUIRE(curve.r.TryAddKey(0.0f, values[at]));
        const ColorCurveAsset* curves[] = {&curve};
        ColorCurveAtlasPixels pixels(allocator);
        REQUIRE(BakeColorCurveAtlas(Span<const ColorCurveAsset* const>(curves), 2, true, pixels));
        CHECK(HalfAt(pixels, 0, 0, 0) == expected[at]);
        CHECK(pixels.bytes[0] == (expected[at] & 0xFF) && pixels.bytes[1] == (expected[at] >> 8));
    }
}

// 크기 상한: 너비·높이가 MaxColorCurveAtlasDimension을 넘으면 CapacityExceeded, 상한 자체는
// 된다. 할당 실패는 OutOfMemory다. 둘 다 out 그대로다.
TEST(CurveAtlasBake_LimitsKeepOutput) {
    SystemAllocator allocator;
    ColorCurveAtlasPixels out(allocator);
    REQUIRE(BakeColorCurveAtlas(Span<const ColorCurveAsset* const>(), 3, false, out));
    REQUIRE(out.sizeX == 3 && out.sizeY == 1);
    CHECK(BakeColorCurveAtlas(Span<const ColorCurveAsset* const>(), MaxColorCurveAtlasDimension + 1,
                              false, out)
              .Error() == ErrorCode::CapacityExceeded);
    CHECK(out.sizeX == 3 && out.sizeY == 1 && out.bytes.Size() == 3 * ColorCurveAtlasTexelBytes);
    Array<const ColorCurveAsset*> many(allocator);
    REQUIRE(many.TryResize(MaxColorCurveAtlasDimension + 1));
    CHECK(BakeColorCurveAtlas(many.View(), 2, false, out).Error() == ErrorCode::CapacityExceeded);
    CHECK(out.sizeX == 3 && out.sizeY == 1);
    ColorCurveAtlasPixels widest(allocator);
    CHECK(BakeColorCurveAtlas(Span<const ColorCurveAsset* const>(), MaxColorCurveAtlasDimension,
                              false, widest));
    CHECK(widest.sizeX == MaxColorCurveAtlasDimension);

    test::NFailAllocator failing(allocator);
    failing.FailOnAttempt(1);
    ColorCurveAtlasPixels refused(failing);
    CHECK(BakeColorCurveAtlas(Span<const ColorCurveAsset* const>(), 4, false, refused).Error() ==
          ErrorCode::OutOfMemory);
    CHECK(refused.sizeX == 0 && refused.sizeY == 0 && refused.bytes.IsEmpty());
}

// atlas 기본 설정은 UE 생성자 값이다. RowOf는 첫 같은 원소이고 null·없는 커브는 IndexNone이다
// (행 목록은 Registry 참조 배열이라 hook 시험이 본다 — 여기서는 빈 목록).
TEST(CurveAtlasBake_AtlasDefaults) {
    SystemAllocator allocator;
    ColorCurveAtlas atlas(allocator);
    CHECK(atlas.width == DefaultColorCurveAtlasWidth && atlas.EffectiveWidth() == 256);
    CHECK(atlas.filter == TextureFilter::Bilinear && atlas.srgb == 0);
    CHECK(atlas.addressX == TextureAddress::Clamp && atlas.addressY == TextureAddress::Clamp);
    CHECK(atlas.compressionSettings == TextureCompressionSettings::HDR);
#if ROCKS_WITH_EDITOR_DATA
    CHECK(atlas.mipGenSettings == TextureMipGenSettings::NoMipmaps && !atlas.disableAdjustments);
#endif
    CHECK(atlas.curves.IsEmpty() && atlas.RowOf(ObjectHandle{}) == IndexNone);
    atlas.width = 1;
    CHECK(atlas.EffectiveWidth() == MinColorCurveAtlasWidth);
}

} // namespace curve_atlas_bake_tests_file
