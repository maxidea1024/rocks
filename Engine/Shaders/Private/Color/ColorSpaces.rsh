// 색 공간이다(Docs/Design/EngineShaders 4.3절 A03·7.4절). 원색·백색점 상수, RGB ↔ XYZ 행렬, 휘도,
// xyY, YCoCg, HSV, Oklab. 행렬은 RGB 열 벡터 기준 행 나열이고 mul(M, rgb)로 쓴다(7.1절 — 색 문헌
// 표기). 작업 색 공간은 선형 Rec.709 원색·D65다(3.4절).
//
// RGB → XYZ 행렬은 SMPTE RP 177-1993 절차로 원색·백색 xy에서 CPU double로 계산해 소수 10자리로 적었다
// (원색 열에 백색이 (1,1,1)이 되는 배율을 곱한다). 원색·백색: ITU-R BT.709-6, BT.2020-2, SMPTE EG 432-1
// (Display P3), SMPTE ST 2065-1(ACES AP0)·Academy S-2014-004(ACEScg AP1), CIE 15:2004(D65 0.3127·0.3290),
// ACES 백색(0.32168·0.33767).
// 근거: Malvar, Sullivan, "YCoCg-R: A Color Space with RGB Reversibility and Low Dynamic Range", JVT-I014
// 2003; Smith, "Color Gamut Transform Pairs", SIGGRAPH 1978(HSV); Ottosson, "A perceptual color space for
// image processing", 2020(Oklab 행렬).
#pragma once

#include "/Engine/Private/Math/ScalarMath.rsh"

// 원색·백색 색도(xy)다.
static const float2 Rec709RedXy = float2(0.64, 0.33);
static const float2 Rec709GreenXy = float2(0.30, 0.60);
static const float2 Rec709BlueXy = float2(0.15, 0.06);
static const float2 Rec2020RedXy = float2(0.708, 0.292);
static const float2 Rec2020GreenXy = float2(0.170, 0.797);
static const float2 Rec2020BlueXy = float2(0.131, 0.046);
static const float2 DisplayP3RedXy = float2(0.680, 0.320);
static const float2 DisplayP3GreenXy = float2(0.265, 0.690);
static const float2 DisplayP3BlueXy = float2(0.150, 0.060);
static const float2 AP0RedXy = float2(0.7347, 0.2653);
static const float2 AP0GreenXy = float2(0.0, 1.0);
static const float2 AP0BlueXy = float2(0.0001, -0.0770);
static const float2 AP1RedXy = float2(0.713, 0.293);
static const float2 AP1GreenXy = float2(0.165, 0.830);
static const float2 AP1BlueXy = float2(0.128, 0.044);
static const float2 WhiteD65Xy = float2(0.3127, 0.3290);
static const float2 WhiteAcesXy = float2(0.32168, 0.33767);

// Rec.709(sRGB 원색, D65) → XYZ다.
static const float3x3 Rec709ToXyz = {
    0.4123907993, 0.3575843394, 0.1804807884,
    0.2126390059, 0.7151686788, 0.0721923154,
    0.0193308187, 0.1191947798, 0.9505321522};
// XYZ → Rec.709(sRGB 원색, D65)다.
static const float3x3 XyzToRec709 = {
    3.2409699419, -1.5373831776, -0.4986107603,
    -0.9692436363, 1.8759675015, 0.0415550574,
    0.0556300797, -0.2039769589, 1.0569715142};
// Rec.2020(D65) → XYZ다.
static const float3x3 Rec2020ToXyz = {
    0.6369580483, 0.1446169036, 0.1688809752,
    0.2627002120, 0.6779980715, 0.0593017165,
    0.0000000000, 0.0280726930, 1.0609850577};
// XYZ → Rec.2020(D65)다.
static const float3x3 XyzToRec2020 = {
    1.7166511880, -0.3556707838, -0.2533662814,
    -0.6666843518, 1.6164812366, 0.0157685458,
    0.0176398574, -0.0427706133, 0.9421031212};
// Display P3(D65) → XYZ다.
static const float3x3 DisplayP3ToXyz = {
    0.4865709486, 0.2656676932, 0.1982172852,
    0.2289745641, 0.6917385218, 0.0792869141,
    0.0000000000, 0.0451133819, 1.0439443689};
// XYZ → Display P3(D65)다.
static const float3x3 XyzToDisplayP3 = {
    2.4934969119, -0.9313836179, -0.4027107845,
    -0.8294889696, 1.7626640603, 0.0236246858,
    0.0358458302, -0.0761723893, 0.9568845240};
// ACES AP0(ACES 백색 ≈ D60) → XYZ다.
static const float3x3 AP0ToXyz = {
    0.9525523959, 0.0000000000, 0.0000936786,
    0.3439664498, 0.7281660966, -0.0721325464,
    0.0000000000, 0.0000000000, 1.0088251844};
// XYZ → ACES AP0(ACES 백색 ≈ D60)다.
static const float3x3 XyzToAP0 = {
    1.0498110175, 0.0000000000, -0.0000974845,
    -0.4959030231, 1.3733130458, 0.0982400361,
    0.0000000000, 0.0000000000, 0.9912520182};
// ACES AP1(ACES 백색 ≈ D60) → XYZ다.
static const float3x3 AP1ToXyz = {
    0.6624541811, 0.1340042065, 0.1561876870,
    0.2722287168, 0.6740817658, 0.0536895174,
    -0.0055746495, 0.0040607335, 1.0103391003};
// XYZ → ACES AP1(ACES 백색 ≈ D60)다.
static const float3x3 XyzToAP1 = {
    1.6410233797, -0.3248032942, -0.2364246952,
    -0.6636628587, 1.6153315917, 0.0167563477,
    0.0117218943, -0.0082844420, 0.9883948585};
// AP0 → AP1(같은 백색이라 순응 없음)다.
static const float3x3 AP0ToAP1 = {
    1.4514393161, -0.2365107469, -0.2149285693,
    -0.0765537734, 1.1762296998, -0.0996759264,
    0.0083161484, -0.0060324498, 0.9977163014};
// AP1 → AP0다.
static const float3x3 AP1ToAP0 = {
    0.6954522414, 0.1406786965, 0.1638690622,
    0.0447945634, 0.8596711185, 0.0955343182,
    -0.0055258826, 0.0040252103, 1.0015006723};
// Rec.709 → Rec.2020(같은 D65)다.
static const float3x3 Rec709ToRec2020 = {
    0.6274038959, 0.3292830384, 0.0433130657,
    0.0690972894, 0.9195403951, 0.0113623156,
    0.0163914389, 0.0880133079, 0.8955952532};
// Rec.2020 → Rec.709다.
static const float3x3 Rec2020ToRec709 = {
    1.6604910021, -0.5876411388, -0.0728498633,
    -0.1245504745, 1.1328998971, -0.0083494226,
    -0.0181507634, -0.1005788980, 1.1187296614};

// 선형 Rec.709 휘도 가중(Rec709ToXyz의 Y 행)이다.
static const float3 Rec709LuminanceWeights = float3(0.2126390059, 0.7151686788, 0.0721923154);
// AP1 휘도 가중(AP1ToXyz의 Y 행)이다.
static const float3 AP1LuminanceWeights = float3(0.2722287168, 0.6740817658, 0.0536895174);

// 작업 색 공간(선형 Rec.709)의 휘도 Y다.
float Luminance(float3 linearRgb) {
    return dot(linearRgb, Rec709LuminanceWeights);
}

// XYZ → xyY다. X + Y + Z = 0이면 (D65 xy, 0)이다.
float3 XyzToXyY(float3 xyz) {
    const float sum = xyz.x + xyz.y + xyz.z;
    const float safeSum = sum == 0.0 ? 1.0 : sum;
    return sum == 0.0 ? float3(WhiteD65Xy, 0.0) : float3(xyz.x / safeSum, xyz.y / safeSum, xyz.y);
}

// xyY → XYZ다. y = 0이면 0이다.
float3 XyYToXyz(float3 xyY) {
    const float scale = xyY.y == 0.0 ? 0.0 : xyY.z / xyY.y;
    return xyY.y == 0.0 ? float3(0.0, 0.0, 0.0) : float3(xyY.x * scale, xyY.z, (1.0 - xyY.x - xyY.y) * scale);
}

// RGB → YCoCg(Malvar 2003, 실수 판 — Y [0,1], Co·Cg [-0.5, 0.5])다.
float3 RgbToYCoCg(float3 rgb) {
    const float y = 0.25 * rgb.r + 0.5 * rgb.g + 0.25 * rgb.b;
    const float co = 0.5 * rgb.r - 0.5 * rgb.b;
    const float cg = -0.25 * rgb.r + 0.5 * rgb.g - 0.25 * rgb.b;
    return float3(y, co, cg);
}

float3 YCoCgToRgb(float3 yCoCg) {
    const float t = yCoCg.x - yCoCg.z;
    return float3(t + yCoCg.y, yCoCg.x + yCoCg.z, t - yCoCg.y);
}

// RGB([0, 1]) → HSV(h ∈ [0, 1) 한 바퀴, s·v ∈ [0, 1], Smith 1978)다. 무채색은 h = 0·s = 0이다.
float3 RgbToHsv(float3 rgb) {
    const float maximum = max(rgb.r, max(rgb.g, rgb.b));
    const float minimum = min(rgb.r, min(rgb.g, rgb.b));
    const float chroma = maximum - minimum;
    float hue = 0.0;
    if (chroma > 0.0) {
        if (maximum == rgb.r) {
            hue = (rgb.g - rgb.b) / chroma;
            if (hue < 0.0) {
                hue += 6.0;
            }
        } else if (maximum == rgb.g) {
            hue = (rgb.b - rgb.r) / chroma + 2.0;
        } else {
            hue = (rgb.r - rgb.g) / chroma + 4.0;
        }
    }

    const float saturation = maximum > 0.0 ? chroma / maximum : 0.0;
    return float3(hue / 6.0, saturation, maximum);
}

float3 HsvToRgb(float3 hsv) {
    const float3 offsets = float3(0.0, 4.0, 2.0);
    const float3 k = frac(hsv.x + offsets / 6.0) * 6.0;
    const float3 ramp = saturate(abs(k - 3.0) - 1.0);
    return hsv.z * lerp(float3(1.0, 1.0, 1.0), ramp, hsv.y);
}

// 실수 세제곱근(부호 보존)이다.
float SignedCubeRoot(float x) {
    return x >= 0.0 ? PowPositive(x, 1.0 / 3.0) : -PowPositive(-x, 1.0 / 3.0);
}

// 선형 Rec.709(sRGB 원색) → Oklab(L·a·b)다(Ottosson 2020).
float3 LinearRgbToOklab(float3 rgb) {
    const float l = 0.4122214708 * rgb.r + 0.5363325363 * rgb.g + 0.0514459929 * rgb.b;
    const float m = 0.2119034982 * rgb.r + 0.6806995451 * rgb.g + 0.1073969566 * rgb.b;
    const float s = 0.0883024619 * rgb.r + 0.2817188376 * rgb.g + 0.6299787005 * rgb.b;
    const float lc = SignedCubeRoot(l);
    const float mc = SignedCubeRoot(m);
    const float sc = SignedCubeRoot(s);
    return float3(0.2104542553 * lc + 0.7936177850 * mc - 0.0040720468 * sc,
                  1.9779984951 * lc - 2.4285922050 * mc + 0.4505937099 * sc,
                  0.0259040371 * lc + 0.7827717662 * mc - 0.8086757660 * sc);
}

float3 OklabToLinearRgb(float3 lab) {
    const float lc = lab.x + 0.3963377774 * lab.y + 0.2158037573 * lab.z;
    const float mc = lab.x - 0.1055613458 * lab.y - 0.0638541728 * lab.z;
    const float sc = lab.x - 0.0894841775 * lab.y - 1.2914855480 * lab.z;
    const float l = lc * lc * lc;
    const float m = mc * mc * mc;
    const float s = sc * sc * sc;
    return float3(4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
                  -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
                  -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s);
}
