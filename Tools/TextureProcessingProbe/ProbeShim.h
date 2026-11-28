// UE 5.8 TextureCompressorModule.cpp 함수 구간을 Rocks 밖에서 컴파일하기 위한 대체 정의다
// (Docs/Design/AssetSystem 12.18 TP-D4). make_golden.py가 UE 클론에서 잘라 온 구간(TcmExtract.inc)
// 앞에 이 헤더를 둔다. UE 헤더는 쓰지 않는다 — 구간이 쓰는 타입·함수만 같은 의미로 정의한다.
//
// 초월 함수 판:
// - 기준 판(ROCKS_PROBE_UE_LIBM = 0): float 인자를 double로 넓혀 계산하고 float로 반올림한다. Rocks
//   빌드(DeterministicMath)와 같은 값이다 — 이 판의 출력이 시험 golden이다.
// - UE 판(ROCKS_PROBE_UE_LIBM = 1): UE FMath처럼 호스트 float libm을 부른다. 두 판의 차이가 UE와
//   Rocks의 차이다.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#ifndef ROCKS_PROBE_UE_LIBM
#error "Define ROCKS_PROBE_UE_LIBM to 0 (reference build) or 1 (UE libm build)."
#endif

using int32 = std::int32_t;
using uint32 = std::uint32_t;
using int64 = std::int64_t;
using uint64 = std::uint64_t;
using uint8 = std::uint8_t;

#define check(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            std::fprintf(stderr, "probe check failed: %s (%s:%d)\n", #expression, __FILE__,        \
                         __LINE__);                                                                \
            std::_Exit(3);                                                                         \
        }                                                                                          \
    } while (0)
#define checkSlow(expression) check(expression)
#define checkf(expression, ...) check(expression)
#define TEXT(text) text
#define TRACE_CPUPROFILER_EVENT_SCOPE(name)
#define UE_LOGF(...)

#undef PI
constexpr float PI = 3.1415926535897932f;
constexpr float SMALL_NUMBER = 1.e-8f;
constexpr float KINDA_SMALL_NUMBER = 1.e-4f;

struct FMath {
    template <class T>
    static T Square(T value) {
        return value * value;
    }

    template <class T>
    static T Clamp(T value, T minimum, T maximum) {
        return value < minimum ? minimum : (value < maximum ? value : maximum);
    }

    template <class T>
    static T Min(T a, T b) {
        return a <= b ? a : b;
    }

    template <class T>
    static T Max(T a, T b) {
        return a >= b ? a : b;
    }

    template <class T>
    static bool IsPowerOfTwo(T value) {
        return value > 0 && (value & (value - 1)) == 0;
    }

#if ROCKS_PROBE_UE_LIBM
    static float Sqrt(float value) {
        return sqrtf(value);
    }

    static float Exp(float value) {
        return expf(value);
    }

    static float Pow(float base, float exponent) {
        return powf(base, exponent);
    }
#else
    static float Sqrt(float value) {
        return static_cast<float>(std::sqrt(static_cast<double>(value)));
    }

    static float Exp(float value) {
        return static_cast<float>(std::exp(static_cast<double>(value)));
    }

    static float Pow(float base, float exponent) {
        return static_cast<float>(
            std::pow(static_cast<double>(base), static_cast<double>(exponent)));
    }
#endif
};

struct FLinearColor {
    float R = 0.0f;
    float G = 0.0f;
    float B = 0.0f;
    float A = 0.0f;

    FLinearColor() = default;

    FLinearColor(float r, float g, float b, float a = 1.0f) : R(r), G(g), B(b), A(a) {
    }

    float& Component(int32 index) {
        return (&R)[index];
    }

    const float& Component(int32 index) const {
        return (&R)[index];
    }
};

// RGBA32F 그림 하나다(구간이 쓰는 멤버만).
struct FImage {
    int64 SizeX = 0;
    int64 SizeY = 0;
    int64 NumSlices = 1;
    std::vector<FLinearColor> Pixels;

    FImage() = default;

    FImage(int64 x, int64 y, int64 slices) : SizeX(x), SizeY(y), NumSlices(slices) {
        Pixels.resize(static_cast<size_t>(x * y * slices));
    }

    FLinearColor* AsRGBA32F() {
        return Pixels.data();
    }

    const FLinearColor* AsRGBA32F() const {
        return Pixels.data();
    }
};
