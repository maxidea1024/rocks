#include <Base/InitializerList.h>
#include <Core/Memory/SystemAllocator.h>
#include <CurveAssets/ColorCurveAsset.generated.h>
#include <CurveAssets/CurveAsset.generated.h>
#include <CurveAssets/ColorCurveAsset.h>
#include <CurveAssets/ColorCurveValue.h>
#include <CurveAssets/CurveAssetsModule.h>
#include <CurveAssets/FloatCurveAsset.generated.h>
#include <CurveAssets/FloatCurveAsset.h>
#include <CurveAssets/VectorCurveAsset.generated.h>
#include <CurveAssets/VectorCurveAsset.h>
#include <CurveAssets/VectorCurveValue.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <float.h>
#include <math.h>
#include <string.h>

// 커브 에셋 클래스 시험이다(Docs/Design/CurveAssets 2장 — CA1): 채널 수·이름·평가, 범위 합집합,
// ResetCurve, 색 조정(UE 식 비트 대조와 손 계산), 평탄 값 복사.
using namespace rocks;

namespace curve_asset_tests_file {

namespace {

constexpr uint64 TestIdHigh = 0x43413141ull;

// 키 하나짜리(상수) 커브다.
void SetConstant(TangentCurve& curve, float value) noexcept {
    curve.Reset();
    (void)curve.TryAddKey(0.0f, value);
}

bool SameBits(float left, float right) noexcept {
    uint32 a = 0;
    uint32 b = 0;
    memcpy(&a, &left, sizeof(a));
    memcpy(&b, &right, sizeof(b));
    return a == b;
}

bool SameBits(const LinearColor& left, const LinearColor& right) noexcept {
    return SameBits(left.r, right.r) && SameBits(left.g, right.g) && SameBits(left.b, right.b) &&
           SameBits(left.a, right.a);
}

// ---- UE 5.8.4 원본을 그대로 옮긴 기준 식(Engine/Private/Curves/CurveLinearColor.cpp:81-142,
// Core/Private/Math/Color.cpp:383-443). 구현과 따로 둔다(구현이 바뀌어도 기준은 UE다). ----

// UE 5.8.4 GenericPlatformMath.h:944-972(Max·Min), UnrealMathUtility.h:551-561(Max3·Min3)·
// 592-595(Clamp).
float UeMax(float a, float b) {
    return (b < a) ? a : b;
}

float UeMin(float a, float b) {
    return (a < b) ? a : b;
}

float UeClamp(float x, float lo, float hi) {
    return UeMax(UeMin(x, hi), lo);
}

LinearColor UeRgbToHsv(LinearColor c) {
    const float RGBMin = UeMin(UeMin(c.r, c.g), c.b);
    const float RGBMax = UeMax(UeMax(c.r, c.g), c.b);
    const float RGBRange = RGBMax - RGBMin;
    if (RGBMax <= 0.f) {
        return LinearColor(0.f, 0.f, 0.f, c.a);
    }

    const float Hue = (RGBMax == RGBMin ? 0.0f
                       : RGBMax == c.r  ? fmodf((((c.g - c.b) / RGBRange) * 60.0f) + 360.0f, 360.0f)
                       : RGBMax == c.g  ? (((c.b - c.r) / RGBRange) * 60.0f) + 120.0f
                       : RGBMax == c.b  ? (((c.r - c.g) / RGBRange) * 60.0f) + 240.0f
                                        : 0.0f);
    const float Saturation = (RGBMax == 0.0f ? 0.0f : RGBRange / RGBMax);
    return LinearColor(Hue, Saturation, RGBMax, c.a);
}

LinearColor UeHsvToRgb(LinearColor c) {
    float Hue = fmodf(c.r, 360.f);
    if (Hue < 0.f) {
        Hue += 360.f;
    }

    const float Saturation = UeClamp(c.g, 0.f, 1.f);
    const float Value = c.b;
    const float HDiv60 = Hue / 60.0f;
    const uint32 HDiv60_Floor = (uint32)(HDiv60);
    const float HDiv60_Fraction = HDiv60 - (float)HDiv60_Floor;
    const float RGBValues[4] = {
        Value,
        Value * (1.0f - Saturation),
        Value * (1.0f - (HDiv60_Fraction * Saturation)),
        Value * (1.0f - ((1.0f - HDiv60_Fraction) * Saturation)),
    };
    const uint32 RGBSwizzle[6][3] = {
        {0, 3, 1}, {2, 0, 1}, {1, 0, 3}, {1, 2, 0}, {3, 1, 0}, {0, 1, 2},
    };
    const uint32 SwizzleIndex = HDiv60_Floor % 6;
    return LinearColor(RGBValues[RGBSwizzle[SwizzleIndex][0]],
                       RGBValues[RGBSwizzle[SwizzleIndex][1]],
                       RGBValues[RGBSwizzle[SwizzleIndex][2]], c.a);
}

LinearColor UeAdjusted(LinearColor OriginalColor, const ColorCurveAdjustments& adj,
                       bool clampAlways) {
    const bool bShouldClampValue = clampAlways || (UeMax(UeMax(OriginalColor.r, OriginalColor.g),
                                                         OriginalColor.b) < 1.0f + 1.e-4f);
    LinearColor HSVColor = UeRgbToHsv(OriginalColor);
    float& PixelHue = HSVColor.r;
    float& PixelSaturation = HSVColor.g;
    float& PixelValue = HSVColor.b;
    PixelValue *= adj.brightness;
    if (!(fabsf(adj.brightnessCurve - 1.0f) <= 1.e-4f) && adj.brightnessCurve != 0.0f) {
        PixelValue = powf(PixelValue, adj.brightnessCurve);
    }

    if (!(fabsf(adj.vibrance) <= 1.e-4f)) {
        const float SatRaisePow = 5.0f;
        const float InvSatRaised = powf(1.0f - PixelSaturation, SatRaisePow);
        const float ClampedVibrance = UeClamp(adj.vibrance, 0.0f, 1.0f);
        const float HalfVibrance = ClampedVibrance * 0.5f;
        const float SatProduct = HalfVibrance * InvSatRaised;
        PixelSaturation += SatProduct;
    }

    PixelSaturation *= adj.saturation;
    PixelHue += adj.hue;
    if (bShouldClampValue) {
        PixelValue = UeClamp(PixelValue, 0.0f, 1.0f);
    }

    LinearColor LinearColorOut = UeHsvToRgb(HSVColor);
    LinearColorOut.a = adj.minAlpha + OriginalColor.a * (adj.maxAlpha - adj.minAlpha);
    return LinearColorOut;
}

struct Env {
    SystemAllocator allocator;
    TypeRegistry types{allocator};
    ModuleRegistration modules{types, allocator};
    bool registered = RegisterCurveAssetsTypes(modules).IsOk();
};

} // namespace

// 2.2–2.4절: 클래스 셋의 채널 수·이름·평가(키 없음·기본값·외삽).
TEST(CurveAsset_ChannelsAndEvaluate) {
    SystemAllocator allocator;
    FloatCurveAsset scalar(allocator);
    VectorCurveAsset vector(allocator);
    ColorCurveAsset color(allocator);
    CHECK(scalar.ChannelCount() == 1 && vector.ChannelCount() == 3 && color.ChannelCount() == 4);
    CHECK(scalar.ChannelName(0) == StringView("Value") && scalar.ChannelName(1).IsEmpty());
    CHECK(vector.ChannelName(0) == StringView("X") && vector.ChannelName(2) == StringView("Z"));
    CHECK(color.ChannelName(3) == StringView("A") && color.ChannelName(4).IsEmpty());
    CHECK(&vector.Channel(1) == &vector.y && &color.MutableChannel(2) == &color.b);

    // 키 없음: 기본값, 없으면 0.
    CHECK(scalar.Evaluate(3.0f) == 0.0f);
    scalar.curve.defaultValue = 7.0f;
    CHECK(scalar.Evaluate(3.0f) == 7.0f);
    REQUIRE(scalar.curve.TryAddKey(0.0f, 0.0f));
    REQUIRE(scalar.curve.TryAddKey(1.0f, 10.0f));
    CHECK(scalar.Evaluate(0.5f) == 5.0f);
    // 외삽(기본 Constant) — 끝 키 값.
    CHECK(scalar.Evaluate(5.0f) == 10.0f);
    CHECK(!scalar.eventCurve);

    REQUIRE(vector.z.TryAddKey(0.0f, 2.0f));
    REQUIRE(vector.z.TryAddKey(2.0f, 4.0f));
    const Vec3d at = vector.Evaluate(1.0f);
    CHECK(at.x == 0.0 && at.y == 0.0 && at.z == 3.0);
}

// 2.1절: 범위 합집합은 키 있는 채널만(모두 비면 NotFound), ResetCurve는 기본값·외삽을 지킨다.
TEST(CurveAsset_RangesAndReset) {
    SystemAllocator allocator;
    VectorCurveAsset vector(allocator);
    CHECK(vector.TryGetTimeRange().Error() == ErrorCode::NotFound);
    CHECK(vector.TryGetValueRange().Error() == ErrorCode::NotFound);

    REQUIRE(vector.x.TryAddKey(2.0f, 5.0f));
    REQUIRE(vector.x.TryAddKey(3.0f, 6.0f));
    // y·z는 키가 없다: UE는 (0, 0)을 합치지만 Rocks는 합치지 않는다.
    auto time = vector.TryGetTimeRange();
    REQUIRE(time);
    CHECK(time.Value().min == 2.0f && time.Value().max == 3.0f);
    auto value = vector.TryGetValueRange();
    REQUIRE(value);
    CHECK(value.Value().min == 5.0f && value.Value().max == 6.0f);

    REQUIRE(vector.z.TryAddKey(-1.0f, -4.0f));
    time = vector.TryGetTimeRange();
    value = vector.TryGetValueRange();
    REQUIRE(time && value);
    CHECK(time.Value().min == -1.0f && time.Value().max == 3.0f);
    CHECK(value.Value().min == -4.0f && value.Value().max == 6.0f);

    // 비유한 값의 채널은 범위 오류다.
    REQUIRE(vector.y.TryAddKey(0.0f, INFINITY));
    CHECK(vector.TryGetValueRange().Error() == ErrorCode::InvalidArgument);

    vector.x.defaultValue = 1.5f;
    vector.x.postExtrapolation = CurveExtrapolation::Linear;
    vector.ResetCurve();
    CHECK(vector.x.KeyCount() == 0 && vector.y.KeyCount() == 0 && vector.z.KeyCount() == 0);
    CHECK(vector.x.defaultValue.HasValue() && vector.x.defaultValue.Value() == 1.5f);
    CHECK(vector.x.postExtrapolation == CurveExtrapolation::Linear);
}

// 2.4.1절: 알파 키가 없으면 A = 1(알파 기본값도 보지 않는다), RGB 범위 밖은 자르지 않는다.
TEST(ColorCurveAsset_Unadjusted) {
    SystemAllocator allocator;
    ColorCurveAsset color(allocator);
    color.a.defaultValue = 0.25f;
    SetConstant(color.r, 2.5f);
    SetConstant(color.g, -1.0f);
    const LinearColor raw = color.EvaluateUnadjusted(0.0f);
    CHECK(raw.r == 2.5f && raw.g == -1.0f && raw.b == 0.0f && raw.a == 1.0f);
    CHECK(!color.HasAlphaKeys());
    SetConstant(color.a, 0.5f);
    CHECK(color.HasAlphaKeys() && color.EvaluateUnadjusted(0.0f).a == 0.5f);
}

// 2.4.2·2.4.3절: 조정 열한 단계를 UE 원본 식(위 기준 — float 그대로)과 비트로 대조한다. 입력 격자는
// LDR·HDR·음수·회색·0·부호 있는 0·비유한·LDR 경계, 조정은 각 단계의 경계(밝기 곡선 1 근처·0, 활기 0
// 근처, 음수 명도, 큰 색상)를 지나고, 알파 키가 있을 때와 없을 때 모두 본다.
TEST(ColorCurveAsset_AdjustMatchesUe) {
    SystemAllocator allocator;
    ColorCurveAsset color(allocator);
    const LinearColor inputs[] = {
        {0.2f, 0.5f, 0.9f, 1.0f},
        {1.0f, 0.0f, 0.0f, 0.3f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 1.0f},
        {0.5f, 0.5f, 0.5f, 0.7f},
        {0.0f, 0.0f, 0.0f, 1.0f},
        {2.0f, 1.5f, 0.25f, 1.0f},
        {1.00005f, 0.2f, 0.1f, 1.0f},
        {-0.5f, 0.25f, 0.75f, 1.0f},
        {-1.0f, -2.0f, -3.0f, 0.5f},
        {0.9f, 0.9f, 0.1f, 1.0f},
        {0.3f, 0.1f, 0.6f, 0.2f},
        // LDR·HDR 경계 1 + 1e-4 그 값과 바로 위아래.
        {1.0001f, 0.5f, 0.0f, 1.0f},
        {nextafterf(1.0001f, 2.0f), 0.5f, 0.0f, 1.0f},
        {nextafterf(1.0001f, 0.0f), 0.5f, 0.0f, 1.0f},
        // 부호 있는 0과 비유한.
        {-0.0f, -0.0f, -0.0f, 1.0f},
        {0.0f, -0.0f, 0.5f, -0.0f},
        {NAN, 0.5f, 0.25f, 1.0f},
        {INFINITY, 0.5f, 0.25f, 1.0f},
        {-INFINITY, 0.5f, 0.25f, 1.0f},
        {0.5f, 0.25f, 0.75f, NAN},
    };
    ColorCurveAdjustments cases[12];
    cases[1].hue = 120.0f;
    cases[2].saturation = 0.25f;
    cases[2].brightness = 0.5f;
    cases[3].brightnessCurve = 0.5f;
    // 1 근처 — pow를 건너뛴다. 0 근처 — 활기를 건너뛴다.
    cases[4].brightnessCurve = 1.00005f;
    cases[4].vibrance = 0.00005f;
    cases[5].vibrance = 0.8f;
    // 조정 뒤 S > 1 — HSV → RGB가 자른다.
    cases[5].saturation = 1.5f;
    // 0 — pow를 건너뛴다.
    cases[6].brightnessCurve = 0.0f;
    cases[6].hue = 359.0f;
    cases[6].minAlpha = 0.2f;
    cases[6].maxAlpha = 0.6f;
    // 활기는 [0, 1]로 자른다.
    cases[7].vibrance = 2.0f;
    cases[7].brightness = 3.0f;
    cases[7].hue = -30.0f;
    // 명도 −0 — UE 5.8 Clamp(Max(Min(...)))가 +0을 낸다.
    cases[8].brightness = -0.0f;
    cases[9].brightness = -1.0f;
    cases[9].saturation = -0.5f;
    cases[10].hue = 720.0f;
    cases[10].brightnessCurve = 2.0f;
    cases[11].hue = -720.0f;
    cases[11].minAlpha = 1.0f;
    cases[11].maxAlpha = 0.0f;
    usize mismatches = 0;
    usize compared = 0;
    for (const LinearColor& input : inputs) {
        SetConstant(color.r, input.r);
        SetConstant(color.g, input.g);
        SetConstant(color.b, input.b);
        for (const bool alphaKeys : {true, false}) {
            LinearColor original = input;
            if (alphaKeys) {
                SetConstant(color.a, input.a);
            } else {
                // 알파 키 없음 — 원본 A = 1.
                color.a.Reset();
                original.a = 1.0f;
            }

            for (const ColorCurveAdjustments& adjustments : cases) {
                color.adjustHue = adjustments.hue;
                color.adjustSaturation = adjustments.saturation;
                color.adjustBrightness = adjustments.brightness;
                color.adjustBrightnessCurve = adjustments.brightnessCurve;
                color.adjustVibrance = adjustments.vibrance;
                color.adjustMinAlpha = adjustments.minAlpha;
                color.adjustMaxAlpha = adjustments.maxAlpha;
                for (const bool clamp : {false, true}) {
                    const LinearColor actual =
                        clamp ? color.EvaluateClamped(0.0f) : color.Evaluate(0.0f);
                    const LinearColor expected = UeAdjusted(original, adjustments, clamp);
                    ++compared;
                    if (!SameBits(actual, expected)) {
                        ++mismatches;
                    }
                }
            }
        }
    }

    CHECK(compared == 21 * 2 * 12 * 2);
    CHECK(mismatches == 0);
}

// 명도 −0(LDR 입력)은 UE 5.8 Clamp가 +0을 낸다 — 옛 삼항 Clamp는 −0을 남긴다.
TEST(ColorCurveAsset_NegativeZeroValueClampsToPositiveZero) {
    SystemAllocator allocator;
    ColorCurveAsset color(allocator);
    SetConstant(color.r, 0.2f);
    SetConstant(color.g, 0.5f);
    SetConstant(color.b, 0.9f);
    color.adjustBrightness = -0.0f;
    const LinearColor out = color.Evaluate(0.0f);
    CHECK(!signbit(out.r) && !signbit(out.g) && !signbit(out.b));
}

// 2.4.2절 손 계산: 색상 이동·채도 0·밝기·알파 재매핑·LDR 자르기·HDR 유지.
TEST(ColorCurveAsset_AdjustHandValues) {
    SystemAllocator allocator;
    ColorCurveAsset color(allocator);
    SetConstant(color.r, 1.0f);
    SetConstant(color.g, 0.0f);
    SetConstant(color.b, 0.0f);
    // 기본 조정은 항등(빨강 그대로, 알파 키 없음 → 1).
    LinearColor out = color.Evaluate(0.0f);
    CHECK(out.r == 1.0f && out.g == 0.0f && out.b == 0.0f && out.a == 1.0f);
    // 색상 +120도: 빨강 → 초록.
    color.adjustHue = 120.0f;
    out = color.Evaluate(0.0f);
    CHECK(out.r == 0.0f && out.g == 1.0f && out.b == 0.0f);
    // 채도 0: 회색 = 명도.
    color.adjustHue = 0.0f;
    color.adjustSaturation = 0.0f;
    color.adjustBrightness = 0.5f;
    out = color.Evaluate(0.0f);
    CHECK(out.r == 0.5f && out.g == 0.5f && out.b == 0.5f);
    // 알파 재매핑: lerp(0.2, 0.6, 1) = 0.6.
    color.adjustMinAlpha = 0.2f;
    color.adjustMaxAlpha = 0.6f;
    CHECK(color.Evaluate(0.0f).a == 0.6f);

    // HDR 입력(R = 2)은 고정 판이 아니면 명도를 자르지 않는다.
    color.adjustSaturation = 1.0f;
    color.adjustBrightness = 1.0f;
    SetConstant(color.r, 2.0f);
    CHECK(color.Evaluate(0.0f).r == 2.0f);
    CHECK(color.EvaluateClamped(0.0f).r == 1.0f);
    // LDR 입력에서 밝기 3배는 1로 잘린다.
    SetConstant(color.r, 0.5f);
    color.adjustBrightness = 3.0f;
    CHECK(color.Evaluate(0.0f).r == 1.0f);
}

// 2.4.3절(CAD10의 근거): RGB 최댓값 ≤ 0이면 UE 식은 (0, 0, 0)이고, Core LinearColor HSV 왕복은
// 다르다.
TEST(ColorCurveAsset_NegativeRgbFollowsUe) {
    SystemAllocator allocator;
    ColorCurveAsset color(allocator);
    SetConstant(color.r, -1.0f);
    SetConstant(color.g, -2.0f);
    SetConstant(color.b, -3.0f);
    const LinearColor out = color.Evaluate(0.0f);
    CHECK(out.r == 0.0f && out.g == 0.0f && out.b == 0.0f);
    const LinearColor core =
        LinearColor(-1.0f, -2.0f, -3.0f, 1.0f).LinearRGBToHSV().HSVToLinearRGB();
    CHECK(!(core.r == 0.0f && core.g == 0.0f && core.b == 0.0f));
}

// 2.5절: 평탄 값 평가·복사(실패하면 대상 그대로), 색 값의 adjusted 분기.
TEST(CurveValue_EvaluateAndCopy) {
    SystemAllocator allocator;
    test::NFailAllocator failing(allocator);
    VectorCurveValue source(allocator);
    REQUIRE(source.x.TryAddKey(0.0f, 1.0f));
    REQUIRE(source.y.TryAddKey(0.0f, 2.0f));
    REQUIRE(source.z.TryAddKey(0.0f, 3.0f));
    VectorCurveValue target(failing);
    REQUIRE(target.x.TryAddKey(5.0f, 9.0f));
    failing.FailOnAttempt(2);
    CHECK(target.TryCopyFrom(source).Error() == ErrorCode::OutOfMemory);
    CHECK(target.x.KeyCount() == 1 && target.x.Keys()[0].value == 9.0f && target.y.KeyCount() == 0);
    failing.DisableFailure();
    REQUIRE(target.TryCopyFrom(source));
    const Vec3d at = target.Evaluate(0.0f);
    CHECK(at.x == 1.0 && at.y == 2.0 && at.z == 3.0);

    ColorCurveValue color(allocator);
    SetConstant(color.r, 1.0f);
    color.adjustHue = 120.0f;
    // adjusted 거짓이면 조정을 보지 않는다(인라인 원천의 평탄화).
    CHECK(color.Evaluate(0.0f).r == 1.0f && color.Evaluate(0.0f).g == 0.0f);
    color.adjusted = true;
    CHECK(color.Evaluate(0.0f).g == 1.0f && color.EvaluateClamped(0.0f).g == 1.0f);
    ColorCurveValue copy(failing);
    failing.FailOnAttempt(1);
    CHECK(copy.TryCopyFrom(color).Error() == ErrorCode::OutOfMemory);
    CHECK(copy.r.KeyCount() == 0 && !copy.adjusted);
    failing.DisableFailure();
    REQUIRE(copy.TryCopyFrom(color));
    CHECK(copy.adjusted && copy.adjustHue == 120.0f && copy.Evaluate(0.0f).g == 1.0f);
}

// 등록: 클래스·struct가 리플렉션에 들어간다(추상 기반 CurveAsset 포함).
TEST(CurveAsset_RegisteredTypes) {
    Env env;
    REQUIRE(env.registered);
    ObjectRegistry objects(env.allocator, env.types, {TestIdHigh, 1});
    const auto access = objects.OwnerAccess();
    auto scalar = objects.NewObject<FloatCurveAsset>(access, TypeIdOf<FloatCurveAsset>());
    auto color = objects.NewObject<ColorCurveAsset>(access, TypeIdOf<ColorCurveAsset>());
    REQUIRE(scalar && color);
    // 추상 기반은 만들 수 없다(Abstract — Unsupported).
    auto abstract = objects.NewObject<CurveAsset>(access, TypeIdOf<CurveAsset>());
    CHECK(!abstract && abstract.Error() == ErrorCode::Unsupported);
}

} // namespace curve_asset_tests_file
