// 수학 상수와 작은 값 하한이다(Docs/Design/EngineShaders 4.2절 A02·7.3절). 값은 CPU double을 float로
// 반올림한 값이다(시험이 대조한다). 의미 있는 수치는 모두 이 파일이나 쓰는 파일의 이름 있는 상수다.
#pragma once

static const float Pi = 3.14159265358979323846;
static const float TwoPi = 6.28318530717958647692;
static const float HalfPi = 1.57079632679489661923;
static const float InversePi = 0.31830988618379067154;
static const float InverseTwoPi = 0.15915494309189533577;
static const float DegreesToRadiansScale = 0.01745329251994329577;
static const float RadiansToDegreesScale = 57.2957795130823208768;

// float 최대 유한값·최소 정규수·기계 엡실론(1과 다음 수의 차)이다.
static const float FloatMaxValue = 3.402823466e+38;
static const float FloatMinNormal = 1.175494351e-38;
static const float FloatEpsilon = 1.192092896e-07;

// 0 나눗셈을 피하는 절댓값 하한이다. 역수 1e30이 유한하고 정규수 범위 안이다(비정규수에 기대지
// 않는다 — 7.1절).
static const float SmallestSafeDivisor = 1.0e-30;
// 정규화할 수 있는 최소 길이 제곱이다(rsqrt 1e15가 유한).
static const float SmallestSafeSquaredLength = 1.0e-30;

// 도 → 라디안이다.
float DegreesToRadians(float degrees) {
    return degrees * DegreesToRadiansScale;
}

// 라디안 → 도다.
float RadiansToDegrees(float radians) {
    return radians * RadiansToDegreesScale;
}
