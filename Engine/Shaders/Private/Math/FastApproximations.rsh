// 빠른 근사 함수다(Docs/Design/EngineShaders 4.2절 A02·7.3절, 3.5절 Fast 접두). 오차 상한은 정확한
// 함수와 비교한 근사 자체의 상한(CPU double 측정)이고, 셰이더 결과는 여기에 float 연산 오차가 더해진다
// (8.3절 "근사 함수"). 시험이 정의역 전체에서 이 상한을 확인한다.
#pragma once

#include "/Engine/Private/Math/ScalarMath.rsh"

// FastRsqrt의 bit 근사 상수다(Lomont, "Fast Inverse Square Root", 2003).
static const uint FastRsqrtMagic = 0x5f3759dfu;
// FastExp2 입력 범위(정규수 결과 2^-126 – 2^128 미만)다.
static const float FastExp2MinInput = -126.0;
static const float FastExp2MaxInput = 127.99999;
// float 지수 편향과 가수 bit 수다.
static const int FloatExponentBias = 127;
static const uint FloatMantissaBits = 23u;
static const uint FloatMantissaMask = 0x007fffffu;

// acos(x), x ∈ [-1, 1]. Abramowitz, Stegun, Handbook of Mathematical Functions 4.4.45(3차 다항식 ×
// √(1 − |x|)). 근사 오차 ≤ 6.8e-5 라디안.
float FastAcos(float x) {
    const float a = abs(x);
    const float r = sqrt(1.0 - a) * (1.5707288 + a * (-0.2121144 + a * (0.0742610 - 0.0187293 * a)));
    return x >= 0.0 ? r : Pi - r;
}

// asin(x) = π/2 − acos(x). 근사 오차 ≤ 6.8e-5 라디안.
float FastAsin(float x) {
    return HalfPi - FastAcos(x);
}

// atan(x). |x| ≤ 1에서 Abramowitz, Stegun 4.4.47(9차 홀수 다항식), |x| > 1은 π/2 − atan(1/x).
// 근사 오차 ≤ 1.2e-5 라디안.
float FastAtan(float x) {
    const float a = abs(x);
    const float t = a > 1.0 ? 1.0 / a : a;
    const float t2 = t * t;
    float r = t * (0.9998660 + t2 * (-0.3302995 + t2 * (0.1801410 + t2 * (-0.0851330 + t2 * 0.0208351))));
    if (a > 1.0) {
        r = HalfPi - r;
    }

    return x >= 0.0 ? r : -r;
}

// atan2(y, x) ∈ [-π, π]. (0, 0)은 0이다. 근사 오차 ≤ 1.2e-5 라디안(+ 나눗셈 오차).
float FastAtan2(float y, float x) {
    const float ax = abs(x);
    const float ay = abs(y);
    const float largest = max(ax, ay);
    const float t = min(ax, ay) / (largest == 0.0 ? 1.0 : largest);
    const float t2 = t * t;
    float r = t * (0.9998660 + t2 * (-0.3302995 + t2 * (0.1801410 + t2 * (-0.0851330 + t2 * 0.0208351))));
    if (ay > ax) {
        r = HalfPi - r;
    }

    if (x < 0.0) {
        r = Pi - r;
    }

    if (largest == 0.0) {
        r = 0.0;
    }

    return y < 0.0 ? -r : r;
}

// 1/√x, x > 0 정규수. bit 근사 + Newton 한 번(Lomont 2003). 상대 오차 ≤ 1.76e-3.
float FastRsqrt(float x) {
    const float y = asfloat(FastRsqrtMagic - (asuint(x) >> 1));
    return y * (1.5 - 0.5 * x * y * y);
}

// 2^x, x ∈ [FastExp2MinInput, FastExp2MaxInput](범위 밖은 잘라낸다). 정수부는 지수 bit, 소수부는
// 4차 다항식(Rocks가 [0, 1)에서 상대 오차 최소로 맞춘 계수, p(0) = 1·p(1) = 2로 이어진다). 근사
// 상대 오차 ≤ 6.5e-6.
float FastExp2(float x) {
    const float clamped = clamp(x, FastExp2MinInput, FastExp2MaxInput);
    const float whole = floor(clamped);
    const float f = clamped - whole;
    const float p = 1.0 + f * (0.693067901 + f * (0.241304175 + f * (0.052010153 + f * 0.013617771)));
    const float scale = asfloat((uint)((int)whole + FloatExponentBias) << FloatMantissaBits);
    return p * scale;
}

// log2(x), x > 0 정규수. 지수 bit + 가수 m ∈ [1, 2)의 5차 다항식(t = m − 1, q(0) = 0·q(1) = 1로
// 이어진다). 근사 절대 오차 ≤ 2.8e-5.
float FastLog2(float x) {
    const uint bits = asuint(x);
    const float exponent = (float)((int)(bits >> FloatMantissaBits) - FloatExponentBias);
    const float t = asfloat((bits & FloatMantissaMask) | ((uint)FloatExponentBias << FloatMantissaBits)) - 1.0;
    const float q = t * (1.442114234 + t * (-0.709797354 + t * (0.415494732 + t * (-0.191702105 + t * 0.043890492))));
    return exponent + q;
}
