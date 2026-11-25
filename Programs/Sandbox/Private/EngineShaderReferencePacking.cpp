#include "EngineShaderProbe.h"

// A02 패킹·인코딩·정수 함수의 CPU 기준 구현과 대조다(Docs/Design/EngineShaders 7.3절, 8.1절 (G)).
// 정수 결과는 bit 단위로 같아야 하고 CPU 결과 지문은 golden(아래 상수)과 같아야 한다 — golden은 CPU
// 기준 구현을 고친 커밋에서만 바꾼다(8.6절). 작은 float·half·RGB9E5 기준은 셰이더와 다른
// 방법(double 나눗셈·rint 반올림, 명세 식)으로 계산한다(결정 SLD10).
namespace sandbox::engine_shaders {

namespace {

constexpr uint32 Width = 256;
constexpr uint32 Height = 256;
constexpr uint32 BigSide = 1024;

// golden 지문(CPU 기준 구현 결과의 64 bit FNV-1a, 8.1절 (G))이다.
struct Golden {
    const char* label;
    uint64 fingerprint;
};

constexpr Golden Goldens[] = {
#include "EngineShaderGoldens.inl"
};

uint64 GoldenFor(const char* label) noexcept {
    for (const Golden& golden : Goldens) {
        if (strcmp(golden.label, label) == 0) {
            return golden.fingerprint;
        }
    }

    return 0;
}

template <typename Expected>
bool CheckGolden(ProbeContext& context, const char* label, const ProbeRequest& request,
                 Expected&& expected) noexcept {
    return CheckUintProbe(context, label, request, GoldenFor(label), expected);
}

// --- 정수 기준 구현(문헌·명세 식) ---

uint32 Reverse(uint32 x) noexcept {
    uint32 result = 0;
    for (uint32 bit = 0; bit < 32; ++bit) {
        result |= ((x >> bit) & 1u) << (31 - bit);
    }

    return result;
}

uint32 PcgOutput(uint32 state) noexcept {
    const uint32 word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

uint32 HashPcg(uint32 v) noexcept {
    return PcgOutput(v * 747796405u + 2891336453u);
}

void HashPcg2D(uint32 v[2]) noexcept {
    for (uint32 k = 0; k < 2; ++k) {
        v[k] = v[k] * 1664525u + 1013904223u;
    }

    for (uint32 round = 0; round < 2; ++round) {
        v[0] += v[1] * 1664525u;
        v[1] += v[0] * 1664525u;
        v[0] ^= v[0] >> 16;
        v[1] ^= v[1] >> 16;
    }
}

void HashPcg3D(uint32 v[3]) noexcept {
    for (uint32 k = 0; k < 3; ++k) {
        v[k] = v[k] * 1664525u + 1013904223u;
    }

    v[0] += v[1] * v[2];
    v[1] += v[2] * v[0];
    v[2] += v[0] * v[1];
    for (uint32 k = 0; k < 3; ++k) {
        v[k] ^= v[k] >> 16;
    }

    v[0] += v[1] * v[2];
    v[1] += v[2] * v[0];
    v[2] += v[0] * v[1];
}

void HashPcg4D(uint32 v[4]) noexcept {
    for (uint32 k = 0; k < 4; ++k) {
        v[k] = v[k] * 1664525u + 1013904223u;
    }

    for (uint32 round = 0; round < 2; ++round) {
        v[0] += v[1] * v[3];
        v[1] += v[2] * v[0];
        v[2] += v[0] * v[1];
        v[3] += v[1] * v[2];
        if (round == 0) {
            for (uint32 k = 0; k < 4; ++k) {
                v[k] ^= v[k] >> 16;
            }
        }
    }
}

uint32 HashMix32(uint32 h) noexcept {
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

uint32 HashCombine(uint32 seed, uint32 value) noexcept {
    return seed ^ (HashMix32(value) + 0x9e3779b9u + (seed << 6) + (seed >> 2));
}

uint32 LaineKarras(uint32 x, uint32 seed) noexcept {
    x += seed;
    x ^= x * 0x6c50b47cu;
    x ^= x * 0xb82f1e52u;
    x ^= x * 0xc7afe638u;
    x ^= x * 0x8d22f6e6u;
    return x;
}

uint32 OwenScramble(uint32 x, uint32 seed) noexcept {
    return Reverse(LaineKarras(Reverse(x), seed));
}

// Sobol 2차원(Joe, Kuo 방향수 — 다항식 x + 1: vₖ = vₖ₋₁ ⊕ vₖ₋₁ >> 1)이다.
uint32 Sobol2(uint32 index) noexcept {
    uint32 result = 0;
    uint32 direction = 0x80000000u;
    for (uint32 bit = 0; bit < 32; ++bit) {
        if ((index >> bit) & 1u) {
            result ^= direction;
        }

        direction ^= direction >> 1;
    }

    return result;
}

void SobolOwen(uint32 index, uint32 seed, uint32 out[2]) noexcept {
    const uint32 shuffled = OwenScramble(index, HashMix32(seed));
    out[0] = OwenScramble(Reverse(shuffled), HashCombine(seed, 0));
    out[1] = OwenScramble(Sobol2(shuffled), HashCombine(seed, 1));
}

void Halton3(uint32 index, uint32& numerator, uint32& denominator) noexcept {
    numerator = 0;
    denominator = 1;
    while (index > 0) {
        numerator = numerator * 3 + index % 3;
        denominator *= 3;
        index /= 3;
    }
}

uint32 MortonInterleave(const uint32* components, uint32 count, uint32 bitsPerComponent) noexcept {
    uint32 code = 0;
    for (uint32 bit = 0; bit < bitsPerComponent; ++bit) {
        for (uint32 axis = 0; axis < count; ++axis) {
            code |= ((components[axis] >> bit) & 1u) << (bit * count + axis);
        }
    }

    return code;
}

void MortonDeinterleave(uint32 code, uint32 count, uint32 bitsPerComponent,
                        uint32* components) noexcept {
    for (uint32 axis = 0; axis < count; ++axis) {
        components[axis] = 0;
        for (uint32 bit = 0; bit < bitsPerComponent; ++bit) {
            components[axis] |= ((code >> (bit * count + axis)) & 1u) << bit;
        }
    }
}

// --- 실수 ↔ 정수 기준(명세 식, double) ---

uint32 UnormMax(uint32 bits) noexcept {
    return (1u << bits) - 1u;
}

// D3D UNORM 변환(가장 가까운 정수, 0.5는 위로)을 셰이더와 같은 float 단계로 한다(precise — 축약
// 없음).
uint32 EncodeUnormReference(float value, uint32 bits) noexcept {
    const float saturated = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    const float scaled = saturated * static_cast<float>(UnormMax(bits));
    const float shifted = scaled + 0.5f;
    return static_cast<uint32>(floorf(shifted));
}

uint32 EncodeSnormReference(float value, uint32 bits) noexcept {
    const float maximum = static_cast<float>((1u << (bits - 1)) - 1u);
    const float clamped = value < -1.0f ? -1.0f : (value > 1.0f ? 1.0f : value);
    const float scaled = clamped * maximum;
    const float rounded = scaled >= 0.0f ? floorf(scaled + 0.5f) : -floorf(-scaled + 0.5f);
    return static_cast<uint32>(static_cast<int>(rounded)) & UnormMax(bits);
}

double DecodeSnormReference(uint32 code, uint32 bits) noexcept {
    const int value = static_cast<int>(code << (32 - bits)) >> (32 - bits);
    const double maximum = static_cast<double>((1u << (bits - 1)) - 1u);
    return fmax(value / maximum, -1.0);
}

// 부호 없는 작은 float(지수 5 bit 편향 15, 가수 m bit — EXT_packed_float) 부호화 기준이다.
uint32 EncodeSmallFloatReference(float value, uint32 m) noexcept {
    const uint32 infinity = 0x1fu << m;
    const uint32 maxFinite = infinity - 1u;
    if (isnan(value)) {
        return infinity | (1u << (m - 1));
    }

    if (signbit(value) || value == 0.0f) {
        return 0;
    }

    if (isinf(value)) {
        return infinity;
    }

    const double a = value;
    int k = 0;
    (void)frexp(a, &k);
    const int e = k - 1;
    double code;
    if (e < -14) {
        code = rint(a / ldexp(1.0, -14 - static_cast<int>(m)));
    } else {
        const double q = rint(a / ldexp(1.0, e - static_cast<int>(m)));
        code = static_cast<double>(e + 15) * ldexp(1.0, static_cast<int>(m)) +
               (q - ldexp(1.0, static_cast<int>(m)));
    }

    return code >= maxFinite ? maxFinite : static_cast<uint32>(code);
}

double DecodeSmallFloatReference(uint32 code, uint32 m) noexcept {
    const uint32 exponent = code >> m;
    const uint32 mantissa = code & ((1u << m) - 1u);
    if (exponent == 0x1fu) {
        return mantissa == 0 ? INFINITY : NAN;
    }

    if (exponent == 0) {
        return ldexp(static_cast<double>(mantissa), -14 - static_cast<int>(m));
    }

    return ldexp(1.0 + mantissa / ldexp(1.0, static_cast<int>(m)), static_cast<int>(exponent) - 15);
}

// half 부호화 기준(IEEE 754 binary16, 가장 가까운 짝수, 넘침 → Inf, NaN → 조용한 NaN 0x7e00)이다.
uint32 EncodeHalfReference(float value) noexcept {
    const uint32 sign = signbit(value) ? 0x8000u : 0u;
    if (isnan(value)) {
        return sign | 0x7e00u;
    }

    const double a = fabs(static_cast<double>(value));
    if (isinf(value) || a >= 65520.0) {
        return sign | 0x7c00u;
    }

    if (a == 0.0) {
        return sign;
    }

    int k = 0;
    (void)frexp(a, &k);
    const int e = k - 1;
    if (e < -14) {
        return sign | static_cast<uint32>(rint(a / ldexp(1.0, -24)));
    }

    const double q = rint(a / ldexp(1.0, e - 10));
    return sign | static_cast<uint32>((e + 15) * 1024 + (q - 1024.0));
}

double DecodeHalfReference(uint32 code) noexcept {
    const double sign = (code & 0x8000u) != 0 ? -1.0 : 1.0;
    const uint32 exponent = (code >> 10) & 0x1fu;
    const uint32 mantissa = code & 0x3ffu;
    if (exponent == 0x1fu) {
        // NaN의 부호도 남긴다(EncodeHalf는 부호 | 조용한 NaN).
        return mantissa == 0 ? sign * INFINITY : copysign(NAN, sign);
    }

    if (exponent == 0) {
        return sign * ldexp(static_cast<double>(mantissa), -24);
    }

    return sign * ldexp(1.0 + mantissa / 1024.0, static_cast<int>(exponent) - 15);
}

// RGB9E5(EXT_texture_shared_exponent 2절 식, N = 9, B = 15, Emax = 31)이다. NaN·음수는 0이다.
uint32 PackRgb9e5Reference(const float color[3]) noexcept {
    constexpr int N = 9;
    constexpr int B = 15;
    const double maxValue = ldexp(511.0 / 512.0, 31 - B);
    double c[3];
    double maxc = 0.0;
    for (uint32 k = 0; k < 3; ++k) {
        const double v = isnan(color[k]) ? 0.0 : static_cast<double>(color[k]);
        c[k] = v < 0.0 ? 0.0 : (v > maxValue ? maxValue : v);
        maxc = fmax(maxc, c[k]);
    }

    int floorLog2 = -B - 1;
    if (maxc >= 1.1754943508222875e-38) {
        int k = 0;
        (void)frexp(maxc, &k);
        floorLog2 = k - 1;
    }

    int shared = (floorLog2 > -B - 1 ? floorLog2 : -B - 1) + 1 + B;
    const double maxs = floor(maxc / ldexp(1.0, shared - B - N) + 0.5);
    if (maxs >= 512.0) {
        ++shared;
    }

    uint32 result = static_cast<uint32>(shared) << 27;
    for (uint32 k = 0; k < 3; ++k) {
        const double s = floor(c[k] / ldexp(1.0, shared - B - N) + 0.5);
        result |= static_cast<uint32>(s > 511.0 ? 511.0 : s) << (9 * k);
    }

    return result;
}

uint32 InsertBitsReference(uint32 value, uint32 insert, uint32 offset, uint32 count) noexcept {
    const uint64 mask = (((uint64{1} << count) - 1u) << offset) & 0xffffffffull;
    return static_cast<uint32>((value & ~mask) | ((static_cast<uint64>(insert) << offset) & mask));
}

// --- 대조 ---

void RunIntegerProbes(ProbeContext& context) noexcept {
    // IsFiniteValue·ReplaceNonFinite(bit 판정 — 앞 16개는 0·±1·Inf·NaN·최대·최소 정규수 등).
    ProbeRequest finite = MakeProbeRequest(ProbeFiniteBits, 0, Width, Height);
    CheckGolden(context, "ScalarMath.IsFiniteValue", finite, [](uint32 c) {
        const uint32 bits = ProbeFloatBits(c, 0, 0);
        return (bits & 0x7f800000u) != 0x7f800000u ? 1u : 0u;
    });
    finite.selector = 1;
    CheckGolden(context, "ScalarMath.ReplaceNonFinite", finite, [](uint32 c) {
        const uint32 bits = ProbeFloatBits(c, 0, 0);
        return (bits & 0x7f800000u) != 0x7f800000u ? bits : FloatToBits(7.0f);
    });

    // unorm·snorm 전 부호 왕복(8·10·16 bit — 7.3절)과 무작위 부호화.
    char label[96];
    const uint32 widths[3] = {8, 10, 16};
    for (const uint32 bits : widths) {
        const uint32 count = 1u << bits;
        ProbeRequest trip =
            MakeProbeRequest(ProbeNormalizedRoundTrip, 0, count < Width ? count : Width,
                             count < Width ? 1 : count / Width, bits);
        snprintf(label, sizeof(label), "BitPacking.UnormRoundTrip%u", bits);
        CheckGolden(context, label, trip, [](uint32 c) { return c; });
        trip.selector = 1;
        snprintf(label, sizeof(label), "BitPacking.SnormRoundTrip%u", bits);
        const uint32 minimum = 1u << (bits - 1);
        CheckGolden(context, label, trip, [minimum](uint32 c) { return c == minimum ? c + 1 : c; });

        ProbeRequest encode = MakeProbeRequest(ProbeNormalizedEncode, 0, Width, Height, bits);
        snprintf(label, sizeof(label), "BitPacking.EncodeUnorm%u", bits);
        CheckGolden(context, label, encode, [bits](uint32 c) {
            return EncodeUnormReference(ProbeSigned(c, 0, 1, bits), bits);
        });
        encode.selector = 1;
        snprintf(label, sizeof(label), "BitPacking.EncodeSnorm%u", bits);
        CheckGolden(context, label, encode, [bits](uint32 c) {
            return EncodeSnormReference(ProbeSigned(c, 0, 1, bits), bits);
        });
    }

    ProbeRequest pack = MakeProbeRequest(ProbePackColor, 0, Width, Height);
    auto packColor = [](uint32 c, bool tenBit) {
        float v[4];
        for (uint32 k = 0; k < 4; ++k) {
            v[k] = ProbeUnit(c, k, 0) * 2.0f - 0.5f;
        }

        if (!tenBit) {
            return EncodeUnormReference(v[0], 8) | (EncodeUnormReference(v[1], 8) << 8) |
                   (EncodeUnormReference(v[2], 8) << 16) | (EncodeUnormReference(v[3], 8) << 24);
        }

        return EncodeUnormReference(v[0], 10) | (EncodeUnormReference(v[1], 10) << 10) |
               (EncodeUnormReference(v[2], 10) << 20) | (EncodeUnormReference(v[3], 2) << 30);
    };
    CheckGolden(context, "BitPacking.PackRGBA8Unorm", pack,
                [&packColor](uint32 c) { return packColor(c, false); });
    pack.selector = 1;
    CheckGolden(context, "BitPacking.PackRGB10A2Unorm", pack,
                [&packColor](uint32 c) { return packColor(c, true); });

    // 11·10 bit float·RG11B10Ufloat(EXT_packed_float), 전 부호 왕복.
    ProbeRequest small = MakeProbeRequest(ProbeSmallFloatEncode, 0, Width, Height);
    CheckGolden(context, "BitPacking.EncodeFloat11", small, [](uint32 c) {
        return EncodeSmallFloatReference(BitsToFloat(ProbeFloatBits(c, 0, 0)), 6);
    });
    small.selector = 1;
    CheckGolden(context, "BitPacking.EncodeFloat10", small, [](uint32 c) {
        return EncodeSmallFloatReference(BitsToFloat(ProbeFloatBits(c, 0, 0)), 5);
    });
    small.selector = 2;
    CheckGolden(context, "BitPacking.PackRG11B10Ufloat", small, [](uint32 c) {
        const float g = ProbeWidePositive(c, 1, 0);
        const float b = ProbeWidePositive(c, 2, 0) * 0.001f;
        return EncodeSmallFloatReference(BitsToFloat(ProbeFloatBits(c, 0, 0)), 6) |
               (EncodeSmallFloatReference(g, 6) << 11) | (EncodeSmallFloatReference(b, 5) << 22);
    });
    ProbeRequest smallTrip = MakeProbeRequest(ProbeSmallFloatRoundTrip, 0, 2048, 1);
    CheckGolden(context, "BitPacking.Float11RoundTrip", smallTrip, [](uint32 c) {
        return EncodeSmallFloatReference(
            static_cast<float>(DecodeSmallFloatReference(c & 0x7ffu, 6)), 6);
    });
    smallTrip.selector = 1;
    CheckGolden(context, "BitPacking.Float10RoundTrip", smallTrip, [](uint32 c) {
        return EncodeSmallFloatReference(
            static_cast<float>(DecodeSmallFloatReference(c & 0x3ffu, 5)), 5);
    });

    ProbeRequest shared = MakeProbeRequest(ProbeRgb9e5Encode, 0, Width, Height);
    CheckGolden(context, "BitPacking.PackRGB9E5Ufloat", shared, [](uint32 c) {
        const float color[3] = {BitsToFloat(ProbeFloatBits(c, 0, 0)),
                                ProbeWidePositive(c, 1, 0) * 0.01f, ProbeUnit(c, 2, 0) * 4.0f};
        return PackRgb9e5Reference(color);
    });

    // half(정수 변환 — 형식마다 반올림이 같다), 전 부호 왕복 65536.
    ProbeRequest half = MakeProbeRequest(ProbeHalfEncode, 0, Width, Height);
    CheckGolden(context, "BitPacking.EncodeHalf", half,
                [](uint32 c) { return EncodeHalfReference(BitsToFloat(ProbeFloatBits(c, 0, 0))); });
    half.selector = 1;
    CheckGolden(context, "BitPacking.PackHalf2", half, [](uint32 c) {
        return EncodeHalfReference(BitsToFloat(ProbeFloatBits(c, 0, 0))) |
               (EncodeHalfReference(ProbeSigned(c, 1, 17, 0)) << 16);
    });
    ProbeRequest halfTrip = MakeProbeRequest(ProbeHalfRoundTrip, 0, Width, Height);
    CheckGolden(context, "BitPacking.HalfRoundTrip", halfTrip, [](uint32 c) {
        return EncodeHalfReference(static_cast<float>(DecodeHalfReference(c & 0xffffu)));
    });

    // 비트 필드·지그재그.
    ProbeRequest fields = MakeProbeRequest(ProbeBitFields, 0, Width, Height);
    auto fieldInputs = [](uint32 c, uint32& offset, uint32& count) {
        const uint32 other = ProbeBits(c, 1, 0);
        offset = other & 0x1fu;
        count = ((other >> 5) & 0x3fu) % (33u - offset);
    };
    CheckGolden(context, "BitPacking.InsertBits", fields, [&fieldInputs](uint32 c) {
        uint32 offset;
        uint32 count;
        fieldInputs(c, offset, count);
        return InsertBitsReference(ProbeBits(c, 0, 0), ProbeBits(c, 2, 0), offset, count);
    });
    fields.selector = 1;
    CheckGolden(context, "BitPacking.ExtractBits", fields, [&fieldInputs](uint32 c) {
        uint32 offset;
        uint32 count;
        fieldInputs(c, offset, count);
        return count == 0 ? 0u
                          : static_cast<uint32>((ProbeBits(c, 0, 0) >> offset) &
                                                ((uint64{1} << count) - 1u));
    });
    fields.selector = 2;
    CheckGolden(context, "BitPacking.PackSignedToUnsigned", fields, [](uint32 c) {
        const rocks::int32 v = static_cast<rocks::int32>(ProbeBits(c, 0, 0));
        return v >= 0 ? static_cast<uint32>(v) * 2u
                      : static_cast<uint32>(-(static_cast<rocks::int64>(v) + 1)) * 2u + 1u;
    });
    fields.selector = 3;
    CheckGolden(context, "BitPacking.UnpackUnsignedToSigned", fields, [](uint32 c) {
        const uint32 u = ProbeBits(c, 0, 0);
        const rocks::int64 value = (u & 1u) != 0 ? -static_cast<rocks::int64>(u >> 1) - 1
                                                 : static_cast<rocks::int64>(u >> 1);
        return static_cast<uint32>(static_cast<rocks::int32>(value));
    });

    // Morton(비트 반복 기준 — 마스크 자리 이동과 다른 방법).
    ProbeRequest morton = MakeProbeRequest(ProbeMorton, 0, Width, Height);
    CheckGolden(context, "MortonOrder.Encode2D", morton, [](uint32 c) {
        const uint32 bits = ProbeBits(c, 0, 0);
        const uint32 xy[2] = {bits & 0xffffu, bits >> 16};
        return MortonInterleave(xy, 2, 16);
    });
    morton.selector = 1;
    CheckGolden(context, "MortonOrder.Decode2D", morton, [](uint32 c) {
        uint32 xy[2];
        MortonDeinterleave(ProbeBits(c, 0, 0), 2, 16, xy);
        return xy[0] | (xy[1] << 16);
    });
    morton.selector = 2;
    CheckGolden(context, "MortonOrder.Encode3D", morton, [](uint32 c) {
        const uint32 bits = ProbeBits(c, 0, 0);
        const uint32 xyz[3] = {bits & 0x3ffu, (bits >> 10) & 0x3ffu, (bits >> 20) & 0x3ffu};
        return MortonInterleave(xyz, 3, 10);
    });
    morton.selector = 3;
    CheckGolden(context, "MortonOrder.Decode3D", morton, [](uint32 c) {
        uint32 xyz[3];
        MortonDeinterleave(ProbeBits(c, 0, 0), 3, 10, xyz);
        return xyz[0] | (xyz[1] << 10) | (xyz[2] << 20);
    });

    // 해시(입력 0–255·경계값·무작위 — 7.3절 golden 표).
    const char* hashNames[12] = {"HashPcg",     "HashPcg2D.x", "HashPcg2D.y", "HashPcg3D.x",
                                 "HashPcg3D.y", "HashPcg3D.z", "HashPcg4D.x", "HashPcg4D.y",
                                 "HashPcg4D.z", "HashPcg4D.w", "HashMix32",   "HashCombine"};
    for (uint32 selector = 0; selector < 12; ++selector) {
        ProbeRequest hash = MakeProbeRequest(ProbeHash, selector, Width, Height);
        snprintf(label, sizeof(label), "IntegerHash.%s", hashNames[selector]);
        CheckGolden(context, label, hash, [selector](uint32 c) {
            const uint32 bits = ProbeBits(c, 0, 0);
            const uint32 other = ProbeBits(c, 1, 0);
            const uint32 v = c < 256 ? c : (c < 272 ? ProbeSpecialBits[c - 256] : bits);
            if (selector == 0) {
                return HashPcg(v);
            }

            if (selector <= 2) {
                uint32 w[2] = {v, other};
                HashPcg2D(w);
                return w[selector - 1];
            }

            if (selector <= 5) {
                uint32 w[3] = {v, other, c};
                HashPcg3D(w);
                return w[selector - 3];
            }

            if (selector <= 9) {
                uint32 w[4] = {v, other, c, bits};
                HashPcg4D(w);
                return w[selector - 6];
            }

            return selector == 10 ? HashMix32(v) : HashCombine(v, other);
        });
    }

    // 상태 있는 PCG 수열(O'Neill pcg32 one-sequence RXS-M-XS).
    for (uint32 selector = 0; selector < 2; ++selector) {
        ProbeRequest stream = MakeProbeRequest(ProbeRandomStreamBits, selector, Width, Height);
        CheckGolden(context,
                    selector == 0 ? "RandomNumbers.RandomPcgFirst" : "RandomNumbers.RandomPcgLater",
                    stream, [selector](uint32 c) {
                        uint32 state = HashPcg(c);
                        uint32 out[4];
                        for (uint32 k = 0; k < 4; ++k) {
                            out[k] = PcgOutput(state);
                            state = state * 747796405u + 2891336453u;
                        }

                        return selector == 0 ? out[0] : (out[1] ^ out[2] ^ out[3]);
                    });
    }

    // 저불일치 수열의 정수부.
    const char* lowNames[8] = {
        "RadicalInverseBase2Bits", "Halton3Fraction.numerator", "Halton3Fraction.denominator",
        "SobolOwen2DBits.x",       "SobolOwen2DBits.y",         "R2SequenceBits.x",
        "R2SequenceBits.y",        "SobolSecondDimension"};
    constexpr uint32 SobolSeed = 1234567u;
    for (uint32 selector = 0; selector < 8; ++selector) {
        ProbeRequest low =
            MakeProbeRequest(ProbeLowDiscrepancyBits, selector, Width, Height, SobolSeed);
        snprintf(label, sizeof(label), "LowDiscrepancy.%s", lowNames[selector]);
        CheckGolden(context, label, low, [selector](uint32 c) {
            uint32 numerator;
            uint32 denominator;
            uint32 sobol[2];
            switch (selector) {
            case 0:
                return Reverse(c);
            case 1:
            case 2:
                Halton3(c, numerator, denominator);
                return selector == 1 ? numerator : denominator;
            case 3:
            case 4:
                SobolOwen(c, SobolSeed, sobol);
                return sobol[selector - 3];
            case 5:
                return 0x80000000u + c * 3242174889u;
            case 6:
                return 0x80000000u + c * 2447445414u;
            default:
                return Sobol2(c);
            }
        });
    }

    // 파란 잡음 texel 좌표(프레임 R2 오프셋, 64 × 64 타일).
    ProbeRequest blue = MakeProbeRequest(ProbeBlueNoise, 0, Width, Height);
    blue.parameters[0] = 37.0f;
    blue.parameters[1] = 6.0f;
    CheckGolden(context, "ScreenNoise.BlueNoiseTexel", blue, [](uint32 c) {
        const uint32 x = c % Width;
        const uint32 y = c / Width;
        const uint32 frame = 37;
        const uint32 offsetX = (frame * 3242174889u) >> (32 - 6);
        const uint32 offsetY = (frame * 2447445414u) >> (32 - 6);
        return ((x + offsetX) & 63u) | (((y + offsetY) & 63u) << 16);
    });
}

void RunDecodeProbes(ProbeContext& context) noexcept {
    // unorm·snorm 복호화(나눗셈 1번 — 2.5 ulp).
    ProbeRequest decode = MakeProbeRequest(ProbeNormalizedDecode, 0, Width, Height);
    const char* names[4] = {"BitPacking.DecodeNormalized", "BitPacking.DecodeSnormWide",
                            "BitPacking.UnpackRGBA8Unorm", "BitPacking.UnpackRGB10A2Unorm"};
    for (uint32 selector = 0; selector < 4; ++selector) {
        decode.selector = selector;
        CheckFloatProbe(context, names[selector], decode, UniformTolerance(0.0, Arithmetic(1)),
                        [selector](uint32 c, const float*, ProbeExpectation& e) {
                            const uint32 bits = ProbeBits(c, 0, 0);
                            switch (selector) {
                            case 0:
                                e.value[0] = (bits & 0xffu) / 255.0;
                                e.value[1] = (bits & 0x3ffu) / 1023.0;
                                e.value[2] = (bits & 0xffffu) / 65535.0;
                                e.value[3] = DecodeSnormReference(bits & 0xffu, 8);
                                break;
                            case 1:
                                e.value[0] = DecodeSnormReference(bits & 0x3ffu, 10);
                                e.value[1] = DecodeSnormReference(bits & 0xffffu, 16);
                                e.mask = 0x3u;
                                break;
                            case 2:
                                for (uint32 k = 0; k < 4; ++k) {
                                    e.value[k] = ((bits >> (8 * k)) & 0xffu) / 255.0;
                                }

                                break;
                            default:
                                for (uint32 k = 0; k < 3; ++k) {
                                    e.value[k] = ((bits >> (10 * k)) & 0x3ffu) / 1023.0;
                                }

                                e.value[3] = (bits >> 30) / 3.0;
                                break;
                            }
                        });
    }

    // 작은 float·RGB9E5·half 복호화(정확 — 2의 거듭제곱 곱).
    ProbeRequest small = MakeProbeRequest(ProbeSmallFloatDecode, 0, 2048, 1);
    CheckFloatProbe(context, "BitPacking.DecodeSmallFloat", small, UniformTolerance(0.0, 0.0),
                    [](uint32 c, const float*, ProbeExpectation& e) {
                        e.value[0] = DecodeSmallFloatReference(c & 0x7ffu, 6);
                        e.value[1] = DecodeSmallFloatReference(c & 0x3ffu, 5);
                        e.mask = 0x3u;
                    });
    small = MakeProbeRequest(ProbeSmallFloatDecode, 1, Width, Height);
    CheckFloatProbe(context, "BitPacking.UnpackRG11B10Ufloat", small, UniformTolerance(0.0, 0.0),
                    [](uint32 c, const float*, ProbeExpectation& e) {
                        const uint32 bits = ProbeBits(c, 0, 0);
                        e.value[0] = DecodeSmallFloatReference(bits & 0x7ffu, 6);
                        e.value[1] = DecodeSmallFloatReference((bits >> 11) & 0x7ffu, 6);
                        e.value[2] = DecodeSmallFloatReference(bits >> 22, 5);
                        e.mask = 0x7u;
                    });
    ProbeRequest shared = MakeProbeRequest(ProbeRgb9e5Decode, 0, Width, Height);
    CheckFloatProbe(context, "BitPacking.UnpackRGB9E5Ufloat", shared, UniformTolerance(0.0, 0.0),
                    [](uint32 c, const float*, ProbeExpectation& e) {
                        const uint32 bits = ProbeBits(c, 0, 0);
                        const int exponent = static_cast<int>(bits >> 27) - 15 - 9;
                        for (uint32 k = 0; k < 3; ++k) {
                            e.value[k] =
                                ldexp(static_cast<double>((bits >> (9 * k)) & 0x1ffu), exponent);
                        }

                        e.mask = 0x7u;
                    });
    ProbeRequest half = MakeProbeRequest(ProbeHalfDecode, 0, Width, Height);
    CheckFloatProbe(context, "BitPacking.DecodeHalf", half, UniformTolerance(0.0, 0.0),
                    [](uint32 c, const float*, ProbeExpectation& e) {
                        const uint32 bits = ProbeBits(c, 0, 0);
                        e.value[0] = DecodeHalfReference(c & 0xffffu);
                        e.value[1] = DecodeHalfReference(bits & 0xffffu);
                        e.value[2] = DecodeHalfReference(bits >> 16);
                        e.mask = 0x7u;
                    });
}

// 팔면체 복호화 기준(Cigolle 2014 식, double)이다.
Double3 DecodeOctahedralReference(double ex, double ey) noexcept {
    Double3 n{ex, ey, 1.0 - fabs(ex) - fabs(ey)};
    if (n.z < 0.0) {
        const double x = (1.0 - fabs(n.y)) * (n.x >= 0.0 ? 1.0 : -1.0);
        const double y = (1.0 - fabs(n.x)) * (n.y >= 0.0 ? 1.0 : -1.0);
        n.x = x;
        n.y = y;
    }

    return Normalize(n);
}

void EncodeOctahedralReference(const Double3& n, double& ex, double& ey) noexcept {
    const double l1 = fabs(n.x) + fabs(n.y) + fabs(n.z);
    ex = n.x / l1;
    ey = n.y / l1;
    if (n.z < 0.0) {
        const double x = (1.0 - fabs(ey)) * (ex >= 0.0 ? 1.0 : -1.0);
        const double y = (1.0 - fabs(ex)) * (ey >= 0.0 ? 1.0 : -1.0);
        ex = x;
        ey = y;
    }
}

double AngleBetween(const Double3& a, const Double3& b) noexcept {
    const Double3 d = Subtract(a, b);
    const Double3 s = Add(a, b);
    return 2.0 * atan2(sqrt(Dot(d, d)), sqrt(Dot(s, s)));
}

void RunEncodingProbes(ProbeContext& context) noexcept {
    ProbeRequest octahedral = MakeProbeRequest(ProbeOctahedral, 0, Width, Height, 8);
    const uint32 seed = octahedral.seed;
    CheckFloatProbe(context, "UnitVectorEncoding.Encode", octahedral, UniformTolerance(2.0e-6, 0.0),
                    [seed](uint32 c, const float*, ProbeExpectation& e) {
                        const Double3 n = ProbeDirection(c, 0, seed);
                        double ex;
                        double ey;
                        EncodeOctahedralReference(n, ex, ey);
                        const double l1 = fabs(n.x) + fabs(n.y) + fabs(n.z);
                        const double px = n.x / l1;
                        const double py = n.y / l1;
                        e.value[0] = ex;
                        e.value[1] = ey;
                        e.value[2] = px + py;
                        e.value[3] = px - py;
                    });
    for (uint32 selector = 1; selector < 5; ++selector) {
        octahedral.selector = selector;
        const char* names[5] = {"", "UnitVectorEncoding.OctahedralRoundTrip",
                                "UnitVectorEncoding.HemiOctahedralRoundTrip",
                                "UnitVectorEncoding.ReconstructNormalZ",
                                "UnitVectorEncoding.DequantizeOctahedral"};
        CheckFloatProbe(context, names[selector], octahedral, UniformTolerance(3.0e-6, 0.0),
                        [seed, selector](uint32 c, const float*, ProbeExpectation& e) {
                            const Double3 n = ProbeDirection(c, 0, seed);
                            Double3 result;
                            if (selector == 1) {
                                result = n;
                            } else if (selector == 2) {
                                result = {n.x, n.y, fabs(n.z)};
                            } else if (selector == 3) {
                                const Double3 v = ProbeVector(c, 3, seed);
                                const double x = v.x * 0.75;
                                const double y = v.y * 0.75;
                                const double zz = 1.0 - x * x - y * y;
                                result = {x, y, zz > 0.0 ? sqrt(zz) : 0.0};
                                // 0 근처 sqrt는 입력 반올림 오차를 √ 배로 키운다.
                                const double z = result.z;
                                e.absolute[2] = fmin(sqrt(8.0 * FloatUlp),
                                                     8.0 * FloatUlp / fmax(z, 1.0e-12) + 1.0e-7);
                                e.absolute[0] = e.absolute[1] = 1.0e-7;
                            } else {
                                const uint32 mask = (1u << seed) - 1u;
                                const double maxCode = static_cast<double>(mask);
                                const uint32 cx = ProbeBits(c, 4, seed) & mask;
                                const uint32 cy = ProbeBits(c, 5, seed) & mask;
                                result = DecodeOctahedralReference(cx * (2.0 / maxCode) - 1.0,
                                                                   cy * (2.0 / maxCode) - 1.0);
                            }

                            e.value[0] = result.x;
                            e.value[1] = result.y;
                            e.value[2] = result.z;
                            e.mask = 0x7u;
                        });
    }

    // 정밀 양자화(Cigolle 2014 4절): 무작위 방향 2^20개의 최대 각 오차를 CPU double 같은 방법과
    // 비교한다.
    const uint32 quantizeBits[3] = {8, 12, 16};
    for (const uint32 bits : quantizeBits) {
        ProbeRequest quantize =
            MakeProbeRequest(ProbeQuantizeOctahedral, 0, BigSide, BigSide, bits);
        Array<uint32> codes(context.allocator);
        char label[96];
        snprintf(label, sizeof(label), "UnitVectorEncoding.QuantizeOctahedral%u", bits);
        if (!context.device.RunUint(quantize, codes)) {
            ReportProbe(context, label, false, "probe draw or readback failed");
            continue;
        }

        const double maxCode = static_cast<double>((1u << bits) - 1u);
        double gpuWorst = 0.0;
        double cpuWorst = 0.0;
        uint32 different = 0;
        for (uint32 c = 0; c < CaseCount(quantize); ++c) {
            const Double3 n = ProbeDirection(c, 0, bits);
            const uint32 code = codes[c];
            gpuWorst = fmax(gpuWorst, AngleBetween(n, DecodeOctahedralReference(
                                                          (code & 0xffffu) * (2.0 / maxCode) - 1.0,
                                                          (code >> 16) * (2.0 / maxCode) - 1.0)));
            double ex;
            double ey;
            EncodeOctahedralReference(n, ex, ey);
            const double fx = floor((ex * 0.5 + 0.5) * maxCode);
            const double fy = floor((ey * 0.5 + 0.5) * maxCode);
            double best = 10.0;
            uint32 bestCode = 0;
            for (uint32 corner = 0; corner < 4; ++corner) {
                const double qx = fmin(fx + (corner & 1u), maxCode);
                const double qy = fmin(fy + (corner >> 1), maxCode);
                const double angle =
                    AngleBetween(n, DecodeOctahedralReference(qx * (2.0 / maxCode) - 1.0,
                                                              qy * (2.0 / maxCode) - 1.0));
                if (angle < best) {
                    best = angle;
                    bestCode = static_cast<uint32>(qx) | (static_cast<uint32>(qy) << 16);
                }
            }

            cpuWorst = fmax(cpuWorst, best);
            different += bestCode != code ? 1u : 0u;
        }

        char detail[192];
        snprintf(detail, sizeof(detail),
                 "%u directions, max angular error %.4g deg (double same method %.4g deg), codes "
                 "differing %u",
                 CaseCount(quantize), gpuWorst * 57.29577951308232, cpuWorst * 57.29577951308232,
                 different);
        ReportProbe(context, label, gpuWorst <= cpuWorst * 1.05 + 1.0e-6, detail);
    }

    // dither 양자화: 결정적 잡음 입력의 부호 = CPU(float 단계 같음), 행마다 평균 오차 기대값 0(통계
    // — 고정 seed, 5σ).
    ProbeRequest dither = MakeProbeRequest(ProbeDither, 0, BigSide, 64);
    Array<float> gpu(context.allocator);
    if (RunFloatProbe(context, "DitheredQuantization.QuantizeWithDither", dither, gpu)) {
        uint32 codeErrors = 0;
        double worstMean = 0.0;
        for (uint32 row = 0; row < dither.height; ++row) {
            const float value = ProbeUnit(row, 9, 0);
            double sum = 0.0;
            for (uint32 x = 0; x < dither.width; ++x) {
                const uint32 c = row * dither.width + x;
                const float noise = ProbeUnit(c, 1, 0);
                const float* v = gpu.Data() + c * 4u;
                const float scaled8 = value * 255.0f;
                const float shifted8 = scaled8 + noise;
                const double expected8 = fmin(floorf(shifted8), 255.0);
                const float scaled10 = value * 1023.0f;
                const float shifted10 = scaled10 + noise;
                const double expected10 = fmin(floorf(shifted10), 1023.0);
                if (rint(v[0] * 255.0) != expected8 || rint(v[1] * 1023.0) != expected10) {
                    ++codeErrors;
                }

                sum += v[0];
            }

            const double mean = sum / dither.width;
            worstMean =
                fmax(worstMean,
                     fabs(mean - value) / (0.5 / 255.0 / sqrt(static_cast<double>(dither.width))));
        }

        char detail[160];
        snprintf(detail, sizeof(detail),
                 "%u samples, code mismatches %u, worst row mean error %.2f sigma (limit 5)",
                 CaseCount(dither), codeErrors, worstMean);
        ReportProbe(context, "DitheredQuantization.Statistics", codeErrors == 0 && worstMean <= 5.0,
                    detail);
    }
}

} // namespace

void RunPackingProbes(ProbeContext& context) noexcept {
    RunIntegerProbes(context);
    RunDecodeProbes(context);
    RunEncodingProbes(context);
}

} // namespace sandbox::engine_shaders
