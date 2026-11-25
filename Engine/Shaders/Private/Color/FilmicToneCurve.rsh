// 파라미터 filmic 곡선과 그 앞뒤 색 경로다(Docs/Design/EngineShaders 04.03절 A03, Docs/Design/RenderBase
// 18.5절 3항). UE 5.8 TonemapCommon.ush FilmToneMap과 PostProcessCombineLUTs.usf의 Filmic 색
// 경로(넓은 색역 확장 → blue correction → FilmToneMap → ToneCurveAmount → blue correction 되돌림)를
// LUT 없이 픽셀마다 계산한다. 색 보정(ColorCorrectAll)은 기본값에서 항등이라 넣지 않는다(SL8).
// 행렬은 UE와 같은 행 나열이고 mul(M, v)로 쓴다(열 벡터). 작업 색 공간은 선형 sRGB(Rec.709 원색,
// D65)다. 원색·순응 행렬은 ColorSpaces·ChromaticAdaptation 상수를 쓴다(같은 행렬을 두 벌 두지 않는다
// — 엔진 셰이더 7.1절). 이름의 Rocks 접두는 RB 18.5절에서 먼저 넣은 것으로 TonemapPass·
// TextureInspector가 쓰므로 그대로 둔다.

#pragma once

#include "/Engine/Private/Color/ChromaticAdaptation.rsh"
#include "/Engine/Private/Color/ColorSpaces.rsh"

// 넓은 색역(P3와 AP1 사이) → XYZ다(UE PostProcessCombineLUTs.usf Wide_2_XYZ_MAT).
static const float3x3 RocksWideToXyz = {
    0.5441691, 0.2395926, 0.1666943,
    0.2394656, 0.7021530, 0.0583814,
    -0.0023439, 0.0361834, 1.0552183};
// UE blue correction 행렬과 그 역이다(AP0에서 정의).
static const float3x3 RocksBlueCorrect = {
    0.9404372683, -0.0183068787, 0.0778696104,
    0.0083786969, 0.8286599939, 0.1629613092,
    0.0005471261, -0.0008833746, 1.0003362486};
static const float3x3 RocksBlueCorrectInverse = {
    1.06318, 0.0233956, -0.0865726,
    -0.0106337, 1.20632, -0.19569,
    -0.000590887, 0.00105248, 0.999538};

// filmic 곡선 파라미터다(UE FPostProcessSettings Film*·BlueCorrection·ExpandGamut·ToneCurveAmount).
struct RocksFilmicParameters {
    float slope;
    float toe;
    float shoulder;
    float blackClip;
    float whiteClip;
    float blueCorrection;
    float expandGamut;
    float toneCurveAmount;
};

float RocksAcesSaturation(float3 rgb) {
    const float minimum = min(min(rgb.r, rgb.g), rgb.b);
    const float maximum = max(max(rgb.r, rgb.g), rgb.b);
    return (max(maximum, 1e-10) - max(minimum, 1e-10)) / max(maximum, 1e-2);
}

// 기하 색상각(도, 0–360)이다. 무채색은 0이다(UE rgb_2_hue).
float RocksAcesHue(float3 rgb) {
    float hue = 0.0;
    if (rgb.r != rgb.g || rgb.g != rgb.b) {
        hue = (180.0 / 3.14159265358979) *
              atan2(sqrt(3.0) * (rgb.g - rgb.b), 2.0 * rgb.r - rgb.g - rgb.b);
    }

    if (hue < 0.0) {
        hue += 360.0;
    }

    return clamp(hue, 0.0, 360.0);
}

float RocksAcesCenterHue(float hue, float center) {
    float centered = hue - center;
    if (centered < -180.0) {
        centered += 360.0;
    } else if (centered > 180.0) {
        centered -= 360.0;
    }

    return centered;
}

// 휘도 대용 YC다(UE rgb_2_yc, 반지름 가중 1.75).
float RocksAcesYc(float3 rgb) {
    const float chroma =
        sqrt(rgb.b * (rgb.b - rgb.g) + rgb.g * (rgb.g - rgb.r) + rgb.r * (rgb.r - rgb.b));
    return (rgb.b + rgb.g + rgb.r + 1.75 * chroma) / 3.0;
}

float RocksAcesSigmoidShaper(float x) {
    const float t = max(1.0 - abs(0.5 * x), 0.0);
    const float y = 1.0 + sign(x) * (1.0 - t * t);
    return 0.5 * y;
}

float RocksAcesGlowForward(float ycIn, float glowGain, float glowMid) {
    if (ycIn <= 2.0 / 3.0 * glowMid) {
        return glowGain;
    }

    if (ycIn >= 2.0 * glowMid) {
        return 0.0;
    }

    return glowGain * (glowMid / ycIn - 0.5);
}

// 곡선의 한 채널이다(UE FilmToneMap의 toe·직선·shoulder 합성, 채널마다 같은 식).
float RocksFilmToneChannel(float logColor, RocksFilmicParameters film, float toeMatch,
                           float straightMatch, float shoulderMatch) {
    const float toeScale = 1.0 + film.blackClip - film.toe;
    const float shoulderScale = 1.0 + film.whiteClip - film.shoulder;
    const float straight = film.slope * (logColor + straightMatch);
    float toe = -film.blackClip +
                (2.0 * toeScale) /
                    (1.0 + exp((-2.0 * film.slope / toeScale) * (logColor - toeMatch)));
    float shoulder =
        (1.0 + film.whiteClip) -
        (2.0 * shoulderScale) /
            (1.0 + exp((2.0 * film.slope / shoulderScale) * (logColor - shoulderMatch)));
    if (!(logColor < toeMatch)) {
        toe = straight;
    }

    if (!(logColor > shoulderMatch)) {
        shoulder = straight;
    }

    float t = saturate((logColor - toeMatch) / (shoulderMatch - toeMatch));
    if (shoulderMatch < toeMatch) {
        t = 1.0 - t;
    }

    t = (3.0 - 2.0 * t) * t * t;
    return lerp(toe, shoulder, t);
}

// UE FilmToneMap이다. 입력·출력은 AP1 원색 선형 값이다(출력은 0 이상).
float3 RocksFilmToneMap(float3 colorAP1, RocksFilmicParameters film) {
    const float3x3 ap1ToAP0 = mul(XyzToAP0, AP1ToXyz);
    float3 colorAP0 = mul(ap1ToAP0, colorAP1);

    // ACES "glow" 단계다.
    const float saturation = RocksAcesSaturation(colorAP0);
    const float ycIn = RocksAcesYc(colorAP0);
    const float s = RocksAcesSigmoidShaper((saturation - 0.4) / 0.2);
    colorAP0 *= 1.0 + RocksAcesGlowForward(ycIn, 0.05 * s, 0.08);

    // ACES red modifier다.
    const float hue = RocksAcesHue(colorAP0);
    const float centeredHue = RocksAcesCenterHue(hue, 0.0);
    const float weight = smoothstep(0.0, 1.0, 1.0 - abs(2.0 * centeredHue / 135.0));
    colorAP0.r += weight * weight * saturation * (0.03 - colorAP0.r) * (1.0 - 0.82);

    float3 working = max(0.0, mul(AP0ToAP1, colorAP0));
    working = lerp(dot(working, AP1LuminanceWeights), working, 0.96);

    const float toeScale = 1.0 + film.blackClip - film.toe;
    const float inMatch = 0.18;
    const float outMatch = 0.18;
    float toeMatch;
    if (film.toe > 0.8) {
        toeMatch = (1.0 - film.toe - outMatch) / film.slope + log10(inMatch);
    } else {
        const float bt = (outMatch + film.blackClip) / toeScale - 1.0;
        toeMatch = log10(inMatch) - 0.5 * log((1.0 + bt) / (1.0 - bt)) * (toeScale / film.slope);
    }

    const float straightMatch = (1.0 - film.toe) / film.slope - toeMatch;
    const float shoulderMatch = film.shoulder / film.slope - straightMatch;
    // UE는 0인 채널에서 log10이 -무한이라 곡선이 NaN이 된다(UE-29935 우회 주석). 아주 작은 양수로
    // 막아 검정이 0으로 가게 한다.
    const float3 logColor = log10(max(working, 1e-10));
    float3 tone;
    tone.r = RocksFilmToneChannel(logColor.r, film, toeMatch, straightMatch, shoulderMatch);
    tone.g = RocksFilmToneChannel(logColor.g, film, toeMatch, straightMatch, shoulderMatch);
    tone.b = RocksFilmToneChannel(logColor.b, film, toeMatch, straightMatch, shoulderMatch);
    tone = lerp(dot(tone, AP1LuminanceWeights), tone, 0.93);
    return max(0.0, tone);
}

// 선형 sRGB 장면 색 → filmic 표시 색(선형 sRGB, 0 이상)이다. UE Filmic 경로의 출력 부호화 앞까지다
// (출력 장치 sRGB·작업 색 공간 sRGB의 기본 경우). 장면 색은 0 이상이어야 한다.
float3 RocksFilmicDisplayColor(float3 linearSrgb, RocksFilmicParameters film) {
    const float3x3 srgbToAP1 = mul(XyzToAP1, mul(BradfordD65ToD60, Rec709ToXyz));
    const float3x3 ap1ToSrgb = mul(XyzToRec709, mul(BradfordD60ToD65, AP1ToXyz));
    const float3x3 ap1ToAP0 = mul(XyzToAP0, AP1ToXyz);
    const float3x3 ap0ToAP1 = mul(XyzToAP1, AP0ToXyz);
    float3 colorAP1 = mul(srgbToAP1, linearSrgb);

    // 밝고 채도 높은 색을 sRGB 밖으로 넓힌다(UE ExpandGamut). 검정은 휘도 0이라 넓히지 않는다.
    const float luma = dot(colorAP1, AP1LuminanceWeights);
    if (luma > 0.0) {
        const float3 chroma = colorAP1 / luma;
        const float chromaDistance = dot(chroma - 1.0, chroma - 1.0);
        const float expandAmount = (1.0 - exp2(-4.0 * chromaDistance)) *
                                   (1.0 - exp2(-4.0 * film.expandGamut * luma * luma));
        const float3x3 expand = mul(mul(XyzToAP1, RocksWideToXyz), ap1ToSrgb);
        colorAP1 = lerp(colorAP1, mul(expand, colorAP1), expandAmount);
    }

    const float3x3 blueCorrectAP1 = mul(ap0ToAP1, mul(RocksBlueCorrect, ap1ToAP0));
    const float3x3 blueCorrectInverseAP1 = mul(ap0ToAP1, mul(RocksBlueCorrectInverse, ap1ToAP0));
    colorAP1 = lerp(colorAP1, mul(blueCorrectAP1, colorAP1), film.blueCorrection);
    colorAP1 = lerp(colorAP1, RocksFilmToneMap(colorAP1, film), film.toneCurveAmount);
    colorAP1 = lerp(colorAP1, mul(blueCorrectInverseAP1, colorAP1), film.blueCorrection);
    return max(0.0, mul(ap1ToSrgb, colorAP1));
}
