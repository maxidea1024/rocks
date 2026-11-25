// 색 순응·색온도다(Docs/Design/EngineShaders 4.3절 A03·7.4절). 행렬은 XYZ 열 벡터에 mul(M, xyz)로 쓴다.
// 근거: Lindbloom, "Chromatic Adaptation"(von Kries 형 — 원추 응답 비율 대각 행렬), Lam, "Metamerism and
// Colour Constancy", 1985(Bradford), CIE 159:2004(CAT02), Kang, Moon, Hong, Lee, Cho, Kim, "Design of
// Advanced Color Temperature Control System for HDTV Applications", J. Korean Physical Society 41(6) 2002
// (Planck 궤적 3차 근사, 1667–25000 K), CIE 15:2004 식 3.3(CIE 주광 궤적, 4000–25000 K), CIE 1960 UCS
// (u, v)(tint 방향).
#pragma once

#include "/Engine/Private/Color/ColorSpaces.rsh"

// 색온도 근사의 정의역(K)과 구간 경계다.
static const float ColorTemperatureMinKelvin = 1667.0;
static const float ColorTemperatureMaxKelvin = 25000.0;
static const float PlanckSplitLowKelvin = 2222.0;
static const float PlanckSplitKelvin = 4000.0;
static const float DaylightSplitKelvin = 7000.0;
// tint 1이 옮기는 CIE 1960 uv 거리다(Rocks 규칙 — 궤적 수직 방향, 양수가 +v 쪽(녹색)).
static const float WhiteBalanceTintScale = 0.05;
// tint 방향 유한 차분의 온도 간격(K)이다.
static const float TintDerivativeKelvin = 10.0;

// Bradford 원추 응답 행렬(Lam 1985, Lindbloom 표)이다.
static const float3x3 BradfordMatrix = {
    0.8951000000, 0.2664000000, -0.1614000000,
    -0.7502000000, 1.7135000000, 0.0367000000,
    0.0389000000, -0.0685000000, 1.0296000000};
// Bradford 역행렬이다.
static const float3x3 BradfordInverse = {
    0.9869929055, -0.1470542564, 0.1599626517,
    0.4323052697, 0.5183602715, 0.0492912282,
    -0.0085286646, 0.0400428217, 0.9684866958};
// CIECAM02 CAT02 행렬(CIE 159:2004)이다.
static const float3x3 Cat02Matrix = {
    0.7328000000, 0.4296000000, -0.1624000000,
    -0.7036000000, 1.6975000000, 0.0061000000,
    0.0030000000, 0.0136000000, 0.9834000000};
// CAT02 역행렬이다.
static const float3x3 Cat02Inverse = {
    1.0961238208, -0.2788690002, 0.1827451794,
    0.4543690420, 0.4735331543, 0.0720978037,
    -0.0096276087, -0.0056980312, 1.0153256400};
// Bradford D65 → ACES 백색(XYZ)이다.
static const float3x3 BradfordD65ToD60 = {
    1.0130349146, 0.0061052578, -0.0149709436,
    0.0076982301, 0.9981633521, -0.0050320385,
    -0.0028413174, 0.0046851567, 0.9245061375};
// Bradford ACES 백색 → D65(XYZ)다.
static const float3x3 BradfordD60ToD65 = {
    0.9872240087, -0.0061132286, 0.0159532883,
    -0.0075983718, 1.0018614847, 0.0053300358,
    0.0030725771, -0.0050959615, 1.0816806031};

// 색도 xy → XYZ(Y = 1)다.
float3 XyToXyz(float2 xy) {
    return float3(xy.x / xy.y, 1.0, (1.0 - xy.x - xy.y) / xy.y);
}

// von Kries 형 순응 행렬 inverse · diag(cone(dst) / cone(src)) · cone이다(XYZ → XYZ).
float3x3 ChromaticAdaptationMatrix(float3x3 cone, float3x3 coneInverse, float2 sourceWhite,
                                   float2 destinationWhite) {
    const float3 source = mul(cone, XyToXyz(sourceWhite));
    const float3 destination = mul(cone, XyToXyz(destinationWhite));
    const float3 ratio = destination / source;
    const float3x3 scale = float3x3(ratio.x, 0.0, 0.0, 0.0, ratio.y, 0.0, 0.0, 0.0, ratio.z);
    return mul(coneInverse, mul(scale, cone));
}

float3x3 BradfordAdaptation(float2 sourceWhite, float2 destinationWhite) {
    return ChromaticAdaptationMatrix(BradfordMatrix, BradfordInverse, sourceWhite, destinationWhite);
}

float3x3 Cat02Adaptation(float2 sourceWhite, float2 destinationWhite) {
    return ChromaticAdaptationMatrix(Cat02Matrix, Cat02Inverse, sourceWhite, destinationWhite);
}

// Planck 궤적(흑체) 색도다(Kang 2002, 1667–25000 K — 범위 밖은 자른다).
float2 PlanckianLocusXy(float kelvin) {
    const float t = clamp(kelvin, ColorTemperatureMinKelvin, ColorTemperatureMaxKelvin);
    const float inverse = 1.0 / t;
    const float inverse2 = inverse * inverse;
    const float inverse3 = inverse2 * inverse;
    float x;
    if (t <= PlanckSplitKelvin) {
        x = -0.2661239e9 * inverse3 - 0.2343589e6 * inverse2 + 0.8776956e3 * inverse + 0.179910;
    } else {
        x = -3.0258469e9 * inverse3 + 2.1070379e6 * inverse2 + 0.2226347e3 * inverse + 0.240390;
    }

    const float x2 = x * x;
    const float x3 = x2 * x;
    float y;
    if (t <= PlanckSplitLowKelvin) {
        y = -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683;
    } else if (t <= PlanckSplitKelvin) {
        y = -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867;
    } else {
        y = 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;
    }

    return float2(x, y);
}

// CIE 주광 궤적 색도다(CIE 15:2004, 4000–25000 K — 범위 밖은 자른다).
float2 DaylightLocusXy(float kelvin) {
    const float t = clamp(kelvin, PlanckSplitKelvin, ColorTemperatureMaxKelvin);
    const float inverse = 1.0 / t;
    const float inverse2 = inverse * inverse;
    const float inverse3 = inverse2 * inverse;
    float x;
    if (t <= DaylightSplitKelvin) {
        x = -4.6070e9 * inverse3 + 2.9678e6 * inverse2 + 0.09911e3 * inverse + 0.244063;
    } else {
        x = -2.0064e9 * inverse3 + 1.9018e6 * inverse2 + 0.24748e3 * inverse + 0.237040;
    }

    return float2(x, -3.0 * x * x + 2.870 * x - 0.275);
}

// 색온도(K) → 백색 색도다. daylight가 참이고 4000 K 이상이면 주광 궤적, 아니면 Planck 궤적이다.
float2 ColorTemperatureToXy(float kelvin, bool daylight) {
    return daylight && kelvin >= PlanckSplitKelvin ? DaylightLocusXy(kelvin) : PlanckianLocusXy(kelvin);
}

// xy → CIE 1960 uv다.
float2 XyToUv1960(float2 xy) {
    const float d = -2.0 * xy.x + 12.0 * xy.y + 3.0;
    return float2(4.0 * xy.x, 6.0 * xy.y) / d;
}

// CIE 1960 uv → xy다.
float2 Uv1960ToXy(float2 uv) {
    const float d = 2.0 * uv.x - 8.0 * uv.y + 4.0;
    return float2(3.0 * uv.x, 2.0 * uv.y) / d;
}

// 색온도·tint의 백색(색도)이다: 주광 궤적(4000 K 미만은 Planck) 점을 CIE 1960 uv에서 Planck 궤적에
// 수직으로 tint × WhiteBalanceTintScale만큼 옮긴다.
float2 WhiteBalanceWhiteXy(float kelvin, float tint) {
    const float2 white = ColorTemperatureToXy(kelvin, true);
    const float2 warmer = XyToUv1960(PlanckianLocusXy(kelvin - TintDerivativeKelvin));
    const float2 cooler = XyToUv1960(PlanckianLocusXy(kelvin + TintDerivativeKelvin));
    const float2 along = normalize(cooler - warmer);
    const float2 normal = float2(-along.y, along.x) * (along.x < 0.0 ? -1.0 : 1.0);
    return Uv1960ToXy(XyToUv1960(white) + normal * (tint * WhiteBalanceTintScale));
}

// 선형 Rec.709 색에 적용하는 white balance 행렬이다: 조명 백색(색온도·tint)을 D65로 옮긴다(Bradford).
// 6504 K·tint 0이면 거의 단위 행렬이다.
float3x3 WhiteBalanceMatrix(float kelvin, float tint) {
    const float3x3 adaptation = BradfordAdaptation(WhiteBalanceWhiteXy(kelvin, tint), WhiteD65Xy);
    return mul(XyzToRec709, mul(adaptation, Rec709ToXyz));
}
