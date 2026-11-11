#include "Tests/Support/Test.h"
#include "Tests/Support/MathTestSupport.h"
#include <Core/Math/Float16.h>

namespace rocks::test {
namespace {

using platform::GenericPlatformMath;
using platform::PlatformMath;

float Bits(uint32 value) {
    return __builtin_bit_cast(float, value);
}

// 독립 oracle: 모든 유한 half 값 중 가장 가까운 값을 double로 직접 찾고, 동률이면 짝수 mantissa를
// 고른다. 65504보다 반 ulp(16) 이상 크면 Inf다. RTNE 구현과 다른 방법이다.
uint16 OracleFloatToHalf(float value) {
    const double target = GenericPlatformMath::Abs(static_cast<double>(value));
    const uint16 sign = GenericPlatformMath::SignBit(value) ? 0x8000u : 0u;
    if (target >= 65520.0) {
        return static_cast<uint16>(sign | 0x7c00u);
    }
    // 단조 증가하는 half 인코딩에서 이진 탐색으로 target 이하의 최대 값을 찾는다.
    uint32 low = 0;
    uint32 high = 0x7bffu;
    while (low < high) {
        const uint32 middle = (low + high + 1) / 2;
        if (static_cast<double>(GenericPlatformMath::HalfToFloat(static_cast<uint16>(middle))) <=
            target) {
            low = middle;
        } else {
            high = middle - 1;
        }
    }
    uint32 best = low;
    if (low < 0x7bffu) {
        const double below = GenericPlatformMath::HalfToFloat(static_cast<uint16>(low));
        const double above = GenericPlatformMath::HalfToFloat(static_cast<uint16>(low + 1));
        const double belowDistance = target - below;
        const double aboveDistance = above - target;
        if (aboveDistance < belowDistance || (aboveDistance == belowDistance && (low & 1u) != 0)) {
            best = low + 1;
        }
    } else if (target - 65504.0 >= 16.0) {
        return static_cast<uint16>(sign | 0x7c00u);
    }
    return static_cast<uint16>(sign | best);
}

} // namespace

TEST(Float16_EveryHalfRoundTripsExactly) {
    int nanCount = 0;
    for (uint32 value = 0; value <= 0xffffu; ++value) {
        const auto half = static_cast<uint16>(value);
        const float wide = PlatformMath::HalfToFloat(half);
        CHECK(GenericPlatformMath::HalfToFloat(half) == wide || math::IsNaN(wide));
        if (math::IsNaN(wide)) {
            ++nanCount;
            CHECK(Float16::FromBits(half).IsNaN());
            CHECK(Float16::FromFloat(wide).IsNaN());
            CHECK(math::CopySign(1.0f, wide) == ((half & 0x8000u) != 0 ? -1.0f : 1.0f));
            continue;
        }
        CHECK(PlatformMath::FloatToHalf(wide) == half); // ±0·subnormal·Inf 포함 정확 왕복
    }
    CHECK(nanCount == 2 * 1023);
}

TEST(Float16_RoundToNearestEvenBoundaries) {
    CHECK(Float16::FromFloat(65504.0f).bits == 0x7bffu);
    CHECK(Float16::FromFloat(65519.996f).bits == 0x7bffu);
    CHECK(Float16::FromFloat(65520.0f).bits == 0x7c00u); // 중간점 → 짝수 쪽인 Inf
    CHECK(Float16::FromFloat(-1.0e9f).bits == 0xfc00u);
    CHECK(Float16::FromFloat(5.9604645e-8f).bits == 0x0001u);     // 2^-24 최소 subnormal
    CHECK(Float16::FromFloat(Bits(0x33000000u)).bits == 0x0000u); // 2^-25: 0과 1의 중간 → 0
    CHECK(Float16::FromFloat(Bits(0x33000001u)).bits == 0x0001u);
    CHECK(Float16::FromFloat(Bits(0x33c00000u)).bits == 0x0002u); // 1.5 * 2^-24 → 2(짝수)
    CHECK(Float16::FromFloat(Bits(0x387fe000u)).bits ==
          0x0400u); // 최대 subnormal 반올림 → 최소 정규
    CHECK(Float16::FromFloat(1.0f + 1.0f / 2048.0f).bits == 0x3c00u); // 1 + 반 ulp → 짝수 1
    CHECK(Float16::FromFloat(1.0f + 3.0f / 2048.0f).bits == 0x3c02u);
    CHECK(Float16::FromFloat(-0.0f).bits == 0x8000u);
    CHECK(Float16::FromFloatClamped(1.0e9f).bits == 0x7bffu);
    CHECK(Float16::FromFloatClamped(-__builtin_huge_valf()).bits == 0xfbffu);
    CHECK(Float16::FromFloatClamped(__builtin_nanf("")).IsNaN());
    CHECK(!Float16::FromFloat(__builtin_huge_valf()).IsFinite());
    CHECK(Float16::FromFloat(0.0f) != Float16::FromFloat(-0.0f)); // bit 비교
}

TEST(Float16_RandomFloatsMatchIndependentOracle) {
    MathRandom random(0x16161616abcdef01ull ^ CurrentSeed());
    for (int iteration = 0; iteration < 20000; ++iteration) {
        // half 표현 범위 주변 지수(2^-30 ~ 2^17)를 고르게 섞는다.
        const uint32 exponent = 97u + static_cast<uint32>(random.Next() % 48u);
        const uint32 bits = (static_cast<uint32>(random.Next() & 1u) << 31) | (exponent << 23) |
                            static_cast<uint32>(random.Next() & 0x7fffffu);
        const float value = Bits(bits);
        const uint16 expected = OracleFloatToHalf(value);
        CHECK(GenericPlatformMath::FloatToHalf(value) == expected);
        CHECK(PlatformMath::FloatToHalf(value) == expected);
    }
}

TEST(Float16_NaNPayloadAndQuietBit) {
    // payload가 하위 13 bit에만 있는 float NaN도 half NaN이어야 한다(Inf로 바뀌면 안 된다).
    const uint32 lowPayloadNaNs[] = {0x7f800001u, 0xff800001u, 0x7f801fffu, 0x7fc00000u};
    for (uint32 bits : lowPayloadNaNs) {
        const uint16 half = GenericPlatformMath::FloatToHalf(Bits(bits));
        CHECK((half & 0x7c00u) == 0x7c00u && (half & 0x03ffu) != 0 && (half & 0x0200u) != 0);
        CHECK((half & 0x8000u) == ((bits >> 16) & 0x8000u));
        CHECK(PlatformMath::FloatToHalf(Bits(bits)) == half);
    }
    // signaling half NaN(0x7c01)은 quiet float NaN이 된다.
    const uint32 widened = __builtin_bit_cast(uint32, GenericPlatformMath::HalfToFloat(0x7c01u));
    CHECK((widened & 0x7f800000u) == 0x7f800000u && (widened & 0x00400000u) != 0);
    CHECK((widened & 0x003fffffu) == (0x001u << 13));
    // 정확한 half → float 값을 독립 계산(지수·가수 직접 조립)과 대조한다.
    CHECK(GenericPlatformMath::HalfToFloat(0x3c00u) == 1.0f);
    CHECK(GenericPlatformMath::HalfToFloat(0xc000u) == -2.0f);
    CHECK(GenericPlatformMath::HalfToFloat(0x7bffu) == 65504.0f);
    CHECK(GenericPlatformMath::HalfToFloat(0x0001u) == 5.9604644775390625e-8f);
    CHECK(GenericPlatformMath::HalfToFloat(0x03ffu) == 6.0975551605224609375e-5f);
    CHECK(GenericPlatformMath::HalfToFloat(0x3555u) == 0.333251953125f);
}

} // namespace rocks::test
