#pragma once

#include <Base/Types.h>

// 결정적 수학: 같은 입력에 대해 Windows x64·macOS arm64·Android arm64에서 같은 결과 비트를 내는
// 함수다 (UE에는 대응 기능이 없다 — UE 텍스처 빌드는 CRT 수학을 쓴다).
//
// 언제 쓰나: 결과가 빌드 캐시 키·쿠킹 패키지 바이트에 들어가는 계산(텍스처 색 변환·크기 조정 필터
// 등). 런타임 게임 코드는 빠른 PlatformMath를 쓴다.
//
// 보장과 전제:
// - IEEE 754 기본 연산(+ − × ÷)과 정수·비트 연산만 쓰고 CRT 수학 함수(pow·exp·log·sin·floor 등)를
//   부르지 않는다. 빌드 전체가 -ffp-contract=off이고 fast-math를 쓰지 않는다. 정밀도가 필요한 곳은
//   오차 없는 변환(Dekker·Knuth)으로 두 double의 합을 쓴다.
// - 부동소수점 환경이 기본(nearest, 비정규 수 보존)이어야 한다. 결정성이 필요한 작업은
//   platform::RunWithDeterministicFloatingPoint 안에서
//   계산한다(Platform/FloatingPointEnvironment.h).
// - 결과가 NaN이면 항상 고정 quiet NaN(double 0x7FF8000000000000)이다(CPU마다 다른 기본 NaN 비트를
//   쓰지 않는다).
// - 정확도는 결정성과 별개다. 상한(가장 가까운 double까지의 비트 거리)은 생성한 고정밀 기준값 표로
//   시험한다: Exp2·Log2 1, Pow·sRGB·Gamma22 2, SinPi·CosPi 2(비정규 결과는 1 더). 결과 비트를
//   바꾸는 변경은 Version을 올린다(빌드 캐시 키·cookKey에 들어간다).
//
// 사용 예:
//   double linear = deterministic::SrgbToLinear(0.5);          // sRGB 부호화 값 → 선형
//   uint8 byte = deterministic::LinearToSrgbByte(0.21404114f); // 선형 → 8비트 sRGB(정확한 반올림)
//   double weight = deterministic::Lanczos(0.75, 3);            // Lanczos3 필터 가중치
namespace rocks::deterministic {

// 결과 비트가 바뀌는 변경마다 올린다.
inline constexpr uint32 Version = 1;

// 정수 부분·내림·짝수 반올림이다(±0·±Inf는 그대로, NaN은 고정 NaN).
double Trunc(double x) noexcept;
double Floor(double x) noexcept;
double RoundHalfEven(double x) noexcept;

// 2^x·log2(x)·x^y다.
// - Exp2: x ≥ 1024이면 +Inf, 결과가 비정규 수 범위면 비정규 수, 너무 작으면 +0.
// - Log2: x < 0·NaN은 고정 NaN, 0은 −Inf, +Inf는 +Inf.
// - Pow: Pow(x, ±0) = 1과 Pow(1, y) = 1은 다른 입력이 NaN이어도 성립한다(C pow와 같다). 그 밖의 NaN
//   입력과 base < 0(−Inf 포함, 이 모듈은 음수 밑을 다루지 않는다)은 고정 NaN. ±0 밑은 부호를 보지
//   않는다: Pow(±0, y > 0) = +0, Pow(±0, y < 0) = +Inf. Pow(+Inf, y > 0) = +Inf, Pow(+Inf, y < 0) =
//   +0. y = ±Inf면 base > 1과 y > 0이 같을 때 +Inf, 다를 때 +0.
double Exp2(double x) noexcept;
double Log2(double x) noexcept;
double Pow(double base, double exponent) noexcept;

// sin(πx)·cos(πx)다. 정수·반정수에서 정확히 0·±1이며 0의 부호는 IEEE 754-2019 권고를 따른다
// (SinPi(±n) = ±0, CosPi(n + ½) = +0). ±Inf·NaN은 고정 NaN.
double SinPi(double x) noexcept;
double CosPi(double x) noexcept;

// sinc(x) = sin(πx)/(πx), sinc(0) = 1. Lanczos(x, lobes) = sinc(x)·sinc(x/lobes)(|x| < lobes), 그
// 밖 0.
double Sinc(double x) noexcept;
double Lanczos(double x, int lobes) noexcept;

// IEC 61966-2-1 sRGB 구간 함수다. 범위 밖 값은 같은 식을 이어서 쓴다(음수는 선형 구간).
double SrgbToLinear(double encoded) noexcept;
double LinearToSrgb(double linear) noexcept;
// x^(11/5)(옛 감마 2.2 해석, UE bUseLegacyGamma — 지수는 정확한 2.2다). 0 이하는 0.
double Gamma22ToLinear(double encoded) noexcept;

// 8비트 sRGB → 선형: decode(value/255)를 가장 가까운 float로 반올림한 값(생성 표, 정확).
float SrgbByteToLinear(uint8 value) noexcept;
// 선형 → 8비트 sRGB: round-half-up(encode(linear)·255)를 정확히 계산한다(경계 표 이분 탐색).
// NaN·0 이하는 0, 1 이상은 255.
uint8 LinearToSrgbByte(float linear) noexcept;

} // namespace rocks::deterministic
