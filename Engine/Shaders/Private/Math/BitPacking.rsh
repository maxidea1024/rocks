// 비트 패킹이다(Docs/Design/EngineShaders 4.2절 A02·7.3절): unorm·snorm 부호화, 렌더 타깃 형식
// RGBA8Unorm·RGB10A2Unorm·RG11B10Ufloat·RGB9E5Ufloat·half 묶기(형식 이름은 PixelFormat 문서), 비트 필드,
// 지그재그 부호 변환.
//
// 결정성(7.1절): 결과 정수는 모든 형식에서 bit 단위로 같아야 한다. 그래서 실수 → 정수 변환의 곱·
// 덧셈을 precise로 두어 FMA 축약을 막고(축약하면 반올림 경계에서 결과가 달라진다), half·작은 float
// 변환은 정수 연산으로 직접 한다(GLSL packHalf2x16·Vulkan의 반올림 방식은 명세가 정하지 않는다).
// 반올림 규칙: unorm·snorm은 가장 가까운 정수(0.5는 위로 — floor(x + 0.5)), half·11·10 bit float는
// 가장 가까운 짝수(IEEE 754 기본).
//
// 근거: D3D11.3 Functional Specification 3.2.3(UNORM·SNORM 변환), Khronos
// EXT_texture_shared_exponent(RGB9E5), EXT_packed_float(11·10 bit float), IEEE 754-2008(half).
#pragma once

#include "/Engine/Private/Math/MathConstants.rsh"

// unorm·snorm bit 수 상한이다(16 bit 넘으면 float 가수 24 bit에서 정확하지 않다).
static const uint MaxNormalizedBits = 16u;
// 32 bit 전체 mask다.
static const uint AllBits = 0xffffffffu;

// half(IEEE binary16) 상수다.
static const uint HalfExponentBias = 15u;
static const uint HalfMantissaBits = 10u;
static const uint HalfInfinity = 0x7c00u;
static const uint HalfQuietNan = 0x7e00u;

// 11·10 bit 부호 없는 float(EXT_packed_float) 상수다. 지수 5 bit(편향 15), 가수 6·5 bit다.
static const uint SmallFloatExponentBias = 15u;
static const uint Float11MantissaBits = 6u;
static const uint Float10MantissaBits = 5u;

// RGB9E5(EXT_texture_shared_exponent) 상수다: 가수 9 bit, 지수 5 bit(편향 15).
static const uint Rgb9e5MantissaBits = 9u;
static const int Rgb9e5ExponentBias = 15;
static const int Rgb9e5MaxExponent = 31;
// 표현 가능한 최대값 (2^9 − 1) / 2^9 × 2^(31 − 15) = 65408이다.
static const float Rgb9e5MaxValue = 65408.0;

// bits(1–16) unorm 최댓값 2^bits − 1이다.
uint UnormMaxCode(uint bits) {
    return (1u << bits) - 1u;
}

// [0, 1] 실수 → unorm 부호(NaN은 0)다. 범위 밖은 자른다.
uint EncodeUnorm(float value, uint bits) {
    precise float scaled = saturate(value) * (float)UnormMaxCode(bits) + 0.5;
    return (uint)floor(scaled);
}

// unorm 부호 → [0, 1] 실수다.
float DecodeUnorm(uint code, uint bits) {
    return (float)code / (float)UnormMaxCode(bits);
}

// [-1, 1] 실수 → snorm 부호(2의 보수, 하위 bits bit)다. -1은 −(2^(bits−1) − 1)이다(D3D 규칙 —
// 최솟값 부호는 쓰지 않는다).
uint EncodeSnorm(float value, uint bits) {
    const float maxMagnitude = (float)((1u << (bits - 1u)) - 1u);
    precise float scaled = clamp(value, -1.0, 1.0) * maxMagnitude;
    precise float rounded = scaled >= 0.0 ? floor(scaled + 0.5) : -floor(-scaled + 0.5);
    return (uint)(int)rounded & UnormMaxCode(bits);
}

// snorm 부호(하위 bits bit) → [-1, 1] 실수다. 최솟값 부호는 −1로 자른다(D3D 규칙).
float DecodeSnorm(uint code, uint bits) {
    const uint shift = 32u - bits;
    const int value = (int)(code << shift) >> shift;
    const float maxMagnitude = (float)((1u << (bits - 1u)) - 1u);
    return max((float)value / maxMagnitude, -1.0);
}

// RGBA8Unorm: r가 하위 byte다.
uint PackRGBA8Unorm(float4 color) {
    return EncodeUnorm(color.r, 8u) | (EncodeUnorm(color.g, 8u) << 8) |
           (EncodeUnorm(color.b, 8u) << 16) | (EncodeUnorm(color.a, 8u) << 24);
}

float4 UnpackRGBA8Unorm(uint packed) {
    return float4(DecodeUnorm(packed & 0xffu, 8u), DecodeUnorm((packed >> 8) & 0xffu, 8u),
                  DecodeUnorm((packed >> 16) & 0xffu, 8u), DecodeUnorm(packed >> 24, 8u));
}

// RGB10A2Unorm: r 하위 10 bit, g 다음 10, b 다음 10, a 상위 2 bit다.
uint PackRGB10A2Unorm(float4 color) {
    return EncodeUnorm(color.r, 10u) | (EncodeUnorm(color.g, 10u) << 10) |
           (EncodeUnorm(color.b, 10u) << 20) | (EncodeUnorm(color.a, 2u) << 30);
}

float4 UnpackRGB10A2Unorm(uint packed) {
    return float4(DecodeUnorm(packed & 0x3ffu, 10u), DecodeUnorm((packed >> 10) & 0x3ffu, 10u),
                  DecodeUnorm((packed >> 20) & 0x3ffu, 10u), DecodeUnorm(packed >> 30, 2u));
}

// 부호 없는 작은 float(지수 5 bit, 가수 mantissaBits bit)로 바꾼다(가장 가까운 짝수). 음수·−0 → 0,
// +Inf → Inf, NaN → NaN(가수 최상위 1), 최대 유한값을 넘는 유한값 → 최대 유한값(D3D 규칙 — Inf로
// 넘치지 않는다), 작은 값은 비정규 부호로 간다.
uint EncodeSmallUnsignedFloat(float value, uint mantissaBits) {
    const uint bits = asuint(value);
    const uint exponentMask = 0x1fu << mantissaBits;
    const uint infinity = exponentMask;
    // 지수 30·가수 전부 1이다.
    const uint maxFinite = exponentMask - 1u;
    const uint magnitude = bits & 0x7fffffffu;
    const uint dropped = 23u - mantissaBits;
    const int exponent = (int)(magnitude >> 23) - 127 + (int)SmallFloatExponentBias;
    uint result = 0u;
    if (exponent >= 1) {
        // 정규수: 지수 다시 편향, 가수 RNE.
        const uint rebased = magnitude - ((127u - SmallFloatExponentBias) << 23);
        const uint halfway = 1u << (dropped - 1u);
        const uint odd = (rebased >> dropped) & 1u;
        result = (rebased + halfway - 1u + odd) >> dropped;
    } else {
        // 비정규수: 숨은 1을 붙여 오른쪽으로 민다(1 − exponent bit 더). 31 bit 넘게 밀면 0이다.
        const uint shift = min(dropped + (uint)(1 - exponent), 31u);
        const uint mantissa = (magnitude & 0x007fffffu) | 0x00800000u;
        const uint halfway = 1u << (shift - 1u);
        const uint odd = (mantissa >> shift) & 1u;
        result = (mantissa + halfway - 1u + odd) >> shift;
    }

    result = min(result, maxFinite);
    if (magnitude == 0x7f800000u) {
        result = infinity;
    }

    if ((bits >> 31) != 0u) {
        result = 0u;
    }

    if (magnitude > 0x7f800000u) {
        result = infinity | (1u << (mantissaBits - 1u));
    }

    return result;
}

// EncodeSmallUnsignedFloat 부호 → float(정확)다.
float DecodeSmallUnsignedFloat(uint code, uint mantissaBits) {
    const uint exponent = code >> mantissaBits;
    const uint mantissa = code & ((1u << mantissaBits) - 1u);
    float result = asfloat(((exponent + 127u - SmallFloatExponentBias) << 23) | (mantissa << (23u - mantissaBits)));
    if (exponent == 0x1fu) {
        result = asfloat(0x7f800000u | (mantissa << (23u - mantissaBits)));
    } else if (exponent == 0u) {
        // 비정규: mantissa × 2^(1 − 편향 − mantissaBits). 2의 거듭제곱 곱이라 정확하다.
        result = (float)mantissa * asfloat((127u + 1u - SmallFloatExponentBias - mantissaBits) << 23);
    }

    return result;
}

// RG11B10Ufloat: r 하위 11 bit, g 다음 11, b 상위 10 bit다.
uint PackRG11B10Ufloat(float3 color) {
    return EncodeSmallUnsignedFloat(color.r, Float11MantissaBits) |
           (EncodeSmallUnsignedFloat(color.g, Float11MantissaBits) << 11) |
           (EncodeSmallUnsignedFloat(color.b, Float10MantissaBits) << 22);
}

float3 UnpackRG11B10Ufloat(uint packed) {
    return float3(DecodeSmallUnsignedFloat(packed & 0x7ffu, Float11MantissaBits),
                  DecodeSmallUnsignedFloat((packed >> 11) & 0x7ffu, Float11MantissaBits),
                  DecodeSmallUnsignedFloat(packed >> 22, Float10MantissaBits));
}

// floor(log2(x)) (x > 0 정규수, 지수 bit — 정확)다.
int FloorLog2Exponent(float x) {
    return (int)(asuint(x) >> 23) - 127;
}

// 2^exponent(exponent ∈ [-126, 127])다(정확).
float ExactPowerOfTwo(int exponent) {
    return asfloat((uint)(exponent + 127) << 23);
}

// RGB9E5Ufloat(EXT_texture_shared_exponent 2절 식): r 하위 9 bit, g 다음 9, b 다음 9, 공유 지수 상위
// 5 bit다. 음수·NaN → 0, 65408 초과 → 65408. 2의 거듭제곱 나눗셈은 정확한 곱이라 bit 단위로
// 결정적이다.
uint PackRGB9E5Ufloat(float3 color) {
    // max(NaN, 0)은 형식마다 다를 수 있으므로 NaN을 먼저 0으로 바꾼다(x == x는 NaN에서 거짓 — bit 판정).
    float3 c;
    c.r = (asuint(color.r) & 0x7fffffffu) > 0x7f800000u ? 0.0 : color.r;
    c.g = (asuint(color.g) & 0x7fffffffu) > 0x7f800000u ? 0.0 : color.g;
    c.b = (asuint(color.b) & 0x7fffffffu) > 0x7f800000u ? 0.0 : color.b;
    c = clamp(c, 0.0, Rgb9e5MaxValue);
    const float maxChannel = max(c.r, max(c.g, c.b));
    // 명세: exp_shared = max(−B − 1, floor(log2(maxc))) + 1 + B. maxc가 비정규·0이면 −B − 1 쪽이다.
    const int floorLog2 = maxChannel >= FloatMinNormal ? FloorLog2Exponent(maxChannel) : -128;
    int sharedExponent = max(-Rgb9e5ExponentBias - 1, floorLog2) + 1 + Rgb9e5ExponentBias;
    precise float scale = ExactPowerOfTwo(-(sharedExponent - Rgb9e5ExponentBias - (int)Rgb9e5MantissaBits));
    precise float maxScaled = floor(maxChannel * scale + 0.5);
    if (maxScaled >= (float)(1u << Rgb9e5MantissaBits)) {
        sharedExponent += 1;
        scale = ExactPowerOfTwo(-(sharedExponent - Rgb9e5ExponentBias - (int)Rgb9e5MantissaBits));
    }

    sharedExponent = min(sharedExponent, Rgb9e5MaxExponent);
    precise float3 scaled = floor(c * scale + 0.5);
    const uint3 mantissa = min((uint3)scaled, (1u << Rgb9e5MantissaBits) - 1u);
    return mantissa.r | (mantissa.g << 9) | (mantissa.b << 18) | ((uint)sharedExponent << 27);
}

float3 UnpackRGB9E5Ufloat(uint packed) {
    const int exponent = (int)(packed >> 27) - Rgb9e5ExponentBias - (int)Rgb9e5MantissaBits;
    const float scale = ExactPowerOfTwo(exponent);
    const uint3 mantissa = uint3(packed & 0x1ffu, (packed >> 9) & 0x1ffu, (packed >> 18) & 0x1ffu);
    return (float3)mantissa * scale;
}

// float → half bit(가장 가까운 짝수, 넘침 → ±Inf, NaN → 조용한 NaN, 작은 값은 비정규 half)다.
uint EncodeHalf(float value) {
    const uint bits = asuint(value);
    const uint sign = (bits >> 16) & 0x8000u;
    const uint magnitude = bits & 0x7fffffffu;
    const int exponent = (int)(magnitude >> 23) - 127 + (int)HalfExponentBias;
    uint result = 0u;
    if (exponent >= 1) {
        const uint rebased = magnitude - ((127u - HalfExponentBias) << 23);
        const uint odd = (rebased >> 13) & 1u;
        result = (rebased + 0xfffu + odd) >> 13;
    } else {
        // 24 bit 넘게 밀면 0이다(가장 작은 비정규 half의 반 미만).
        const uint shift = min(13u + (uint)(1 - exponent), 25u);
        const uint mantissa = (magnitude & 0x007fffffu) | 0x00800000u;
        const uint halfway = 1u << (shift - 1u);
        const uint odd = (mantissa >> shift) & 1u;
        result = (mantissa + halfway - 1u + odd) >> shift;
    }

    if (magnitude >= 0x477ff000u) {
        // 65520 이상(반올림하면 65536)은 Inf다.
        result = HalfInfinity;
    }

    if (magnitude > 0x7f800000u) {
        result = HalfQuietNan;
    }

    return sign | result;
}

// half bit → float(정확 — 비정규 half도 float 정규수로 정확히 간다)다.
float DecodeHalf(uint code) {
    const uint sign = (code & 0x8000u) << 16;
    const uint exponent = (code >> HalfMantissaBits) & 0x1fu;
    const uint mantissa = code & 0x3ffu;
    uint result = sign | ((exponent + 127u - HalfExponentBias) << 23) | (mantissa << 13);
    if (exponent == 0x1fu) {
        result = sign | 0x7f800000u | (mantissa << 13);
    } else if (exponent == 0u) {
        const float magnitude = (float)mantissa * asfloat((127u - 14u - HalfMantissaBits) << 23);
        result = sign | asuint(magnitude);
    }

    return asfloat(result);
}

// float2 → half 둘(x 하위 16 bit)이다.
uint PackHalf2(float2 value) {
    return EncodeHalf(value.x) | (EncodeHalf(value.y) << 16);
}

float2 UnpackHalf2(uint packed) {
    return float2(DecodeHalf(packed & 0xffffu), DecodeHalf(packed >> 16));
}

// value의 [offset, offset + count) bit를 insert의 하위 count bit로 바꾼다(count 0–32, offset + count ≤
// 32 — HLSL·GLSL bitfieldInsert와 같은 의미, count 32는 insert 전체).
uint InsertBits(uint value, uint insert, uint offset, uint count) {
    const uint mask = count >= 32u ? AllBits : ((1u << count) - 1u) << offset;
    return (value & ~mask) | ((insert << offset) & mask);
}

// value의 [offset, offset + count) bit를 하위로 꺼낸다(count 0–32).
uint ExtractBits(uint value, uint offset, uint count) {
    const uint mask = count >= 32u ? AllBits : (1u << count) - 1u;
    return count == 0u ? 0u : (value >> offset) & mask;
}

// 부호 있는 정수 → 부호 없는 정수(지그재그: 0, −1, 1, −2 → 0, 1, 2, 3 — 작은 크기가 작은 부호)다.
uint PackSignedToUnsigned(int value) {
    return ((uint)value << 1) ^ (uint)(value >> 31);
}

int UnpackUnsignedToSigned(uint value) {
    return (int)(value >> 1) ^ -(int)(value & 1u);
}
