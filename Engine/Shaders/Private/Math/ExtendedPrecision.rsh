// float 둘 확장 정밀도다(Docs/Design/EngineShaders 4.2절 A02·7.3절, 결정 SLD6 — 큰 월드의 절대
// 위치·큰 시간값). 값 = high + low이고 |low| ≤ ½ ulp(high)다(정규화된 쌍). 근거: Dekker, "A
// Floating-Point Technique for Extending the Available Precision", Numer. Math. 18, 1971(TwoProduct의
// 분할), Knuth TAOCP 2권 4.2.2(TwoSum), Thall, "Extended-Precision Floating-Point Numbers for GPU
// Computation", SIGGRAPH 2006 Posters.
//
// 오차 없는 변환은 연산 순서에 기대므로 모든 중간값을 precise로 선언한다(컴파일러의 재결합·FMA
// 축약 금지 — DXIL·SPIR-V NoContraction, GLSL precise). FMA를 쓰지 않는 Dekker 분할이라 FMA가 없는
// 형식에서도 같은 결과다.
//
// 사용 예(카메라 상대 위치 — 3.4절):
//   ExtendedFloat3 world = MakeExtendedFloat3(positionHigh, positionLow);
//   float3 translated = ToTranslatedWorld(world, viewOrigin);
#pragma once

// Dekker 분할 상수 2^12 + 1(float 가수 24 bit를 12 bit 둘로 나눈다)이다.
static const float DekkerSplitter = 4097.0;

struct ExtendedFloat3 {
    float3 high;
    float3 low;
};

ExtendedFloat3 MakeExtendedFloat3(float3 high, float3 low) {
    ExtendedFloat3 value;
    value.high = high;
    value.low = low;
    return value;
}

// a + b = sum + error(정확). Knuth TwoSum(크기 순서와 무관).
void TwoSum(float a, float b, out float sum, out float error) {
    precise float s = a + b;
    precise float bVirtual = s - a;
    precise float aVirtual = s - bVirtual;
    precise float bRound = b - bVirtual;
    precise float aRound = a - aVirtual;
    sum = s;
    error = aRound + bRound;
}

// float3 성분별 TwoSum이다.
void TwoSum(float3 a, float3 b, out float3 sum, out float3 error) {
    precise float3 s = a + b;
    precise float3 bVirtual = s - a;
    precise float3 aVirtual = s - bVirtual;
    precise float3 bRound = b - bVirtual;
    precise float3 aRound = a - aVirtual;
    sum = s;
    error = aRound + bRound;
}

// a = high + low(각 12 bit 가수)로 나눈다(Dekker split).
void SplitFloat(float a, out float high, out float low) {
    precise float c = DekkerSplitter * a;
    precise float big = c - a;
    precise float h = c - big;
    high = h;
    low = a - h;
}

// a × b = product + error(정확, FMA 없이 — Dekker TwoProduct). |a·b| < 2^(127-12)이어야 분할이 넘치지
// 않는다.
void TwoProduct(float a, float b, out float product, out float error) {
    float aHigh;
    float aLow;
    float bHigh;
    float bLow;
    SplitFloat(a, aHigh, aLow);
    SplitFloat(b, bHigh, bLow);
    precise float p = a * b;
    precise float e = ((aHigh * bHigh - p) + aHigh * bLow + aLow * bHigh) + aLow * bLow;
    product = p;
    error = e;
}

// 아무 두 float의 합 high + low를 정규화된 쌍으로 만든다(TwoSum — 크기 순서와 무관하게 정확).
ExtendedFloat3 NormalizeExtended(float3 high, float3 low) {
    float3 sum;
    float3 error;
    TwoSum(high, low, sum, error);
    return MakeExtendedFloat3(sum, error);
}

// a + b(확장 정밀도 덧셈 — 상대 오차 ≈ 2^-44).
ExtendedFloat3 ExtendedAdd(ExtendedFloat3 a, ExtendedFloat3 b) {
    float3 sum;
    float3 error;
    TwoSum(a.high, b.high, sum, error);
    precise float3 low = error + (a.low + b.low);
    return NormalizeExtended(sum, low);
}

// a − b다.
ExtendedFloat3 ExtendedSubtract(ExtendedFloat3 a, ExtendedFloat3 b) {
    return ExtendedAdd(a, MakeExtendedFloat3(-b.high, -b.low));
}

// a × s(스칼라 float)다.
ExtendedFloat3 ExtendedScale(ExtendedFloat3 a, float s) {
    float3 product;
    float3 error;
    TwoProduct(a.high.x, s, product.x, error.x);
    TwoProduct(a.high.y, s, product.y, error.y);
    TwoProduct(a.high.z, s, product.z, error.z);
    precise float3 low = error + a.low * s;
    return NormalizeExtended(product, low);
}

// float 하나로 줄인다(high + low를 반올림).
float3 ExtendedToFloat3(ExtendedFloat3 a) {
    return a.high + a.low;
}

// 절대 world 위치(확장 정밀도) → 카메라 상대 위치(translated world, cm)다: world − viewOrigin을
// 확장 정밀도로 뺀 뒤 float로 줄인다. 카메라 근처에서 오차는 결과 크기의 ½ ulp 수준이다
// (1e7 cm 떨어진 월드에서도 카메라 상대 오차 ≤ 0.01 cm — 7.3절 시험).
float3 ToTranslatedWorld(ExtendedFloat3 world, ExtendedFloat3 viewOrigin) {
    return ExtendedToFloat3(ExtendedSubtract(world, viewOrigin));
}
