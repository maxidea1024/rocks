#include "Tests/Support/Test.h"
#include <Platform/PlatformMath.h>

#include <float.h>

// CPU별 PlatformMath·simd 구현이 Generic/scalar 기준 구현과 같은 계약을 지키는지 차등 검사한다.
// 정확 함수는 bit 단위(NaN은 NaN끼리) 일치, 근사 함수는 문서화한 상대 오차 이내를 요구한다.
namespace rocks::test {
namespace {

using platform::GenericPlatformMath;
using platform::PlatformMath;

// 재현 가능한 64-bit xorshift. 실패 시 seed와 반복 번호로 같은 입력을 재생성할 수 있다.
struct TestRandom {
    uint64 state;

    uint64 Next() noexcept {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }

    uint32 Next32() noexcept {
        return static_cast<uint32>(Next() >> 32);
    }
};

float FloatBits(uint32 bits) {
    return __builtin_bit_cast(float, bits);
}

double DoubleBits(uint64 bits) {
    return __builtin_bit_cast(double, bits);
}

// 같은 bit이거나 둘 다 NaN이면 같은 결과로 본다. NaN payload는 CPU 계약에 포함하지 않는다.
bool SameFloat(float a, float b) {
    if (GenericPlatformMath::IsNaN(a) || GenericPlatformMath::IsNaN(b)) {
        return GenericPlatformMath::IsNaN(a) && GenericPlatformMath::IsNaN(b);
    }
    return __builtin_bit_cast(uint32, a) == __builtin_bit_cast(uint32, b);
}

bool SameDouble(double a, double b) {
    if (GenericPlatformMath::IsNaN(a) || GenericPlatformMath::IsNaN(b)) {
        return GenericPlatformMath::IsNaN(a) && GenericPlatformMath::IsNaN(b);
    }
    return __builtin_bit_cast(uint64, a) == __builtin_bit_cast(uint64, b);
}

constexpr float Infinity = __builtin_huge_valf();
const float QuietNaN = FloatBits(0x7fc00000u);

const float SpecialFloats[] = {
    0.0f,           -0.0f,         0.5f,           -0.5f,        1.5f,        -1.5f,
    2.5f,           -2.5f,         0.49999997f,    -0.49999997f, 0.50000006f, 1.0f,
    -1.0f,          8388607.5f,    -8388607.5f,    8388608.0f,   16777217.0f, 2147483520.0f,
    -2147483648.0f, 2147483648.0f, -2147483904.0f, 1.0e-45f,     -1.0e-45f,   FLT_MIN,
    FLT_MAX,        -FLT_MAX,      Infinity,       -Infinity};

const double SpecialDoubles[] = {0.0,
                                 -0.0,
                                 0.5,
                                 -0.5,
                                 2.5,
                                 -2.5,
                                 0.49999999999999994,
                                 -0.49999999999999994,
                                 2147483647.0,
                                 2147483647.4,
                                 2147483647.5,
                                 2147483647.9999998,
                                 2147483648.0,
                                 -2147483648.0,
                                 -2147483648.4,
                                 -2147483648.5,
                                 -2147483649.0,
                                 4503599627370495.5,
                                 9223372036854774784.0,
                                 9223372036854775808.0,
                                 -9223372036854775808.0,
                                 4.9e-324,
                                 DBL_MAX,
                                 -DBL_MAX};

// 범위 밖·NaN은 Fatal 경로이므로 Try로 유효성을 먼저 판정하고, 유효할 때만 두 구현을 비교한다.
void CompareInt32Conversions(double value) {
    int32 expected = 0;
    if (GenericPlatformMath::TryFloorToInt32(value, expected)) {
        CHECK(PlatformMath::FloorToInt32(value) == expected);
        if (static_cast<double>(static_cast<float>(value)) == value) {
            CHECK(PlatformMath::FloorToInt32(static_cast<float>(value)) == expected);
        }
    }
    if (GenericPlatformMath::TryCeilToInt32(value, expected)) {
        CHECK(PlatformMath::CeilToInt32(value) == expected);
        if (static_cast<double>(static_cast<float>(value)) == value) {
            CHECK(PlatformMath::CeilToInt32(static_cast<float>(value)) == expected);
        }
    }
    if (GenericPlatformMath::TryRoundToInt32(value, expected)) {
        CHECK(PlatformMath::RoundToInt32(value) == expected);
        if (static_cast<double>(static_cast<float>(value)) == value) {
            CHECK(PlatformMath::RoundToInt32(static_cast<float>(value)) == expected);
        }
    }
    if (GenericPlatformMath::TryTruncToInt32(value, expected)) {
        CHECK(PlatformMath::TruncToInt32(value) == expected);
        if (static_cast<double>(static_cast<float>(value)) == value) {
            CHECK(PlatformMath::TruncToInt32(static_cast<float>(value)) == expected);
        }
    }
}

void CompareInt64Conversions(double value) {
    int64 expected = 0;
    if (GenericPlatformMath::TryFloorToInt64(value, expected)) {
        CHECK(PlatformMath::FloorToInt64(value) == expected);
    }
    if (GenericPlatformMath::TryCeilToInt64(value, expected)) {
        CHECK(PlatformMath::CeilToInt64(value) == expected);
    }
    if (GenericPlatformMath::TryRoundToInt64(value, expected)) {
        CHECK(PlatformMath::RoundToInt64(value) == expected);
    }
    if (GenericPlatformMath::TryTruncToInt64(value, expected)) {
        CHECK(PlatformMath::TruncToInt64(value) == expected);
    }
}

} // namespace

TEST(PlatformMath_BackendIsNamed) {
    CHECK(platform::MathBackendName != nullptr && platform::MathBackendName[0] != '\0');
}

TEST(PlatformMath_ClassificationUsesBitPatterns) {
    CHECK(PlatformMath::IsNaN(QuietNaN));
    CHECK(PlatformMath::IsNaN(FloatBits(0xffc00001u)));
    CHECK(PlatformMath::IsNaN(FloatBits(0x7f800001u))); // signaling NaN
    CHECK(!PlatformMath::IsNaN(Infinity));
    CHECK(!PlatformMath::IsFinite(Infinity));
    CHECK(!PlatformMath::IsFinite(QuietNaN));
    CHECK(PlatformMath::IsFinite(FLT_MAX));
    CHECK(PlatformMath::IsFinite(1.0e-45f));
    CHECK(PlatformMath::IsInfinite(-Infinity));
    CHECK(PlatformMath::SignBit(-0.0f));
    CHECK(!PlatformMath::SignBit(0.0f));
    CHECK(PlatformMath::IsNaN(DoubleBits(0x7ff8000000000000ull)));
    CHECK(!PlatformMath::IsFinite(DoubleBits(0x7ff0000000000000ull)));
    CHECK(PlatformMath::IsFinite(DBL_MIN));
    static_assert(GenericPlatformMath::IsNaN(__builtin_nanf("")));
    static_assert(!GenericPlatformMath::IsFinite(__builtin_huge_val()));
}

TEST(PlatformMath_MinMaxFollowComparisonSelectContract) {
    // (a < b) ? a : b: NaN이 섞이면 두 번째 인자, ±0은 두 번째 인자를 반환한다.
    CHECK(PlatformMath::IsNaN(PlatformMath::Min(1.0f, QuietNaN)));
    CHECK(PlatformMath::Min(QuietNaN, 1.0f) == 1.0f);
    CHECK(PlatformMath::SignBit(PlatformMath::Min(0.0f, -0.0f)));
    CHECK(!PlatformMath::SignBit(PlatformMath::Min(-0.0f, 0.0f)));
    CHECK(PlatformMath::Max(2.0, 3.0) == 3.0);
    CHECK(PlatformMath::IsNaN(PlatformMath::Max(1.0, __builtin_nan(""))));
}

TEST(PlatformMath_AbsAndCopySignHandleSignedZeroAndNaN) {
    CHECK(!PlatformMath::SignBit(PlatformMath::Abs(-0.0f)));
    CHECK(PlatformMath::Abs(-3.5) == 3.5);
    CHECK(PlatformMath::IsNaN(PlatformMath::Abs(FloatBits(0xffc00000u))));
    CHECK(!PlatformMath::SignBit(PlatformMath::Abs(FloatBits(0xffc00000u))));
    CHECK(PlatformMath::CopySign(2.0f, -0.0f) == -2.0f);
    CHECK(PlatformMath::CopySign(-2.0, 1.0) == 2.0);
}

TEST(PlatformMath_SqrtMatchesGenericBitwise) {
    for (float value : SpecialFloats) {
        CHECK(SameFloat(PlatformMath::Sqrt(value), GenericPlatformMath::Sqrt(value)));
        CHECK(SameFloat(PlatformMath::InvSqrt(value), GenericPlatformMath::InvSqrt(value)));
    }
    CHECK(SameFloat(PlatformMath::Sqrt(-0.0f), -0.0f));
    CHECK(PlatformMath::IsNaN(PlatformMath::Sqrt(-1.0f)));
    CHECK(PlatformMath::InvSqrt(0.0f) == Infinity);
    TestRandom random{0x9e3779b97f4a7c15ull ^ CurrentSeed()};
    for (int iteration = 0; iteration < 200000; ++iteration) {
        const float value = FloatBits(random.Next32() & 0x7fffffffu);
        CHECK(SameFloat(PlatformMath::Sqrt(value), GenericPlatformMath::Sqrt(value)));
        const double wide = DoubleBits(random.Next() & 0x7fffffffffffffffull);
        CHECK(SameDouble(PlatformMath::Sqrt(wide), GenericPlatformMath::Sqrt(wide)));
        CHECK(SameDouble(PlatformMath::InvSqrt(wide), GenericPlatformMath::InvSqrt(wide)));
    }
}

TEST(PlatformMath_InvSqrtEstimateStaysWithinDocumentedError) {
    TestRandom random{0x1234567887654321ull ^ CurrentSeed()};
    double worst = 0.0;
    for (int iteration = 0; iteration < 200000; ++iteration) {
        // 양의 정규 유한 float: exponent 1..254
        const uint32 exponent = 1u + random.Next32() % 254u;
        const float value = FloatBits((exponent << 23) | (random.Next32() & 0x7fffffu));
        const double reference = 1.0 / GenericPlatformMath::Sqrt(static_cast<double>(value));
        const double estimate = PlatformMath::InvSqrtEstimate(value);
        const double error = GenericPlatformMath::Abs(estimate - reference) / reference;
        worst = error > worst ? error : worst;
    }
    CHECK(worst <= static_cast<double>(GenericPlatformMath::InvSqrtEstimateRelativeError));
    // 근사 경로 밖의 입력은 정확한 InvSqrt와 같다.
    const float outside[] = {0.0f, -0.0f, -1.0f, 1.0e-40f, Infinity, QuietNaN};
    for (float value : outside) {
        CHECK(SameFloat(PlatformMath::InvSqrtEstimate(value), GenericPlatformMath::InvSqrt(value)));
    }
}

TEST(PlatformMath_IntegerConversionsMatchGenericOnEdges) {
    for (float value : SpecialFloats) {
        CompareInt32Conversions(static_cast<double>(value));
        CompareInt32Conversions(-static_cast<double>(value));
    }
    for (double value : SpecialDoubles) {
        CompareInt32Conversions(value);
        CompareInt32Conversions(-value);
        CompareInt64Conversions(value);
        CompareInt64Conversions(-value);
    }
}

TEST(PlatformMath_IntegerConversionsMatchGenericOnRandomInputs) {
    TestRandom random{0x0badc0ffee123457ull ^ CurrentSeed()};
    for (int iteration = 0; iteration < 300000; ++iteration) {
        // 지수를 int32 경계 근처까지 고르게 섞는다.
        const uint32 exponent = 100u + random.Next32() % 60u;
        const uint32 bits =
            ((random.Next32() & 1u) << 31) | (exponent << 23) | (random.Next32() & 0x7fffffu);
        CompareInt32Conversions(static_cast<double>(FloatBits(bits)));
        const uint64 wideExponent = 1000ull + random.Next() % 70ull;
        const uint64 wideBits = ((random.Next() & 1ull) << 63) | (wideExponent << 52) |
                                (random.Next() & 0xfffffffffffffull);
        CompareInt32Conversions(DoubleBits(wideBits));
        CompareInt64Conversions(DoubleBits(wideBits));
    }
}

TEST(PlatformMath_RoundingDirectionContract) {
    // 0.5 경계는 0에서 멀어진다. UE RoundToInt(+무한대 방향)와 음수 x.5에서 다르다.
    CHECK(PlatformMath::RoundToInt32(2.5f) == 3);
    CHECK(PlatformMath::RoundToInt32(-2.5f) == -3);
    CHECK(PlatformMath::RoundToInt32(-0.5) == -1);
    // 0.49999997f + 0.5f가 1.0f로 반올림되는 단순 구현의 오류를 막는다.
    CHECK(PlatformMath::RoundToInt32(0.49999997f) == 0);
    CHECK(PlatformMath::FloorToInt32(-0.0f) == 0);
    CHECK(PlatformMath::FloorToInt32(-1.0e-45f) == -1);
    CHECK(PlatformMath::CeilToInt32(1.0e-45f) == 1);
    CHECK(PlatformMath::FloorToInt32(-2147483648.0f) == -2147483647 - 1);
    CHECK(PlatformMath::CeilToInt32(2147483647.0) == 2147483647);
    CHECK(PlatformMath::TruncToInt32(-2147483648.9) == -2147483647 - 1);
}

TEST(PlatformMath_TryConversionsRejectWithoutChangingOutput) {
    int32 output = 77;
    CHECK(!PlatformMath::TryFloorToInt32(__builtin_nan(""), output));
    CHECK(!PlatformMath::TryRoundToInt32(2147483647.5, output));
    CHECK(!PlatformMath::TryCeilToInt32(2147483647.1, output));
    CHECK(!PlatformMath::TryTruncToInt32(-2147483649.0, output));
    CHECK(!PlatformMath::TryFloorToInt32(static_cast<double>(Infinity), output));
    CHECK(output == 77);
    int64 wide = 5;
    CHECK(!PlatformMath::TryRoundToInt64(9223372036854775808.0, wide));
    CHECK(PlatformMath::TryFloorToInt64(-9223372036854775808.0, wide));
    CHECK(wide == -9223372036854775807ll - 1);
}

TEST(PlatformMath_BitOperationsMatchGeneric) {
    const uint64 values[] = {0ull,
                             1ull,
                             2ull,
                             0x80000000ull,
                             0xffffffffull,
                             0x100000000ull,
                             0x8000000000000000ull,
                             0xffffffffffffffffull};
    for (uint64 value : values) {
        const auto low = static_cast<uint32>(value);
        CHECK(PlatformMath::CountLeadingZeros32(low) ==
              GenericPlatformMath::CountLeadingZeros32(low));
        CHECK(PlatformMath::CountTrailingZeros32(low) ==
              GenericPlatformMath::CountTrailingZeros32(low));
        CHECK(PlatformMath::PopCount32(low) == GenericPlatformMath::PopCount32(low));
        CHECK(PlatformMath::CountLeadingZeros64(value) ==
              GenericPlatformMath::CountLeadingZeros64(value));
        CHECK(PlatformMath::CountTrailingZeros64(value) ==
              GenericPlatformMath::CountTrailingZeros64(value));
        CHECK(PlatformMath::PopCount64(value) == GenericPlatformMath::PopCount64(value));
    }
    CHECK(PlatformMath::CountLeadingZeros32(0) == 32);
    CHECK(PlatformMath::CountTrailingZeros64(0) == 64);
    TestRandom random{0x5555aaaa5555aaaaull ^ CurrentSeed()};
    for (int iteration = 0; iteration < 10000; ++iteration) {
        const uint64 value = random.Next() >> (random.Next32() % 64u);
        CHECK(PlatformMath::CountLeadingZeros64(value) ==
              GenericPlatformMath::CountLeadingZeros64(value));
        CHECK(PlatformMath::PopCount64(value) == GenericPlatformMath::PopCount64(value));
    }
}

// ---- 4-lane 레지스터: 선택된 simd와 scalar 기준 구현 차등 검사 ----
namespace {

float RandomLane(TestRandom& random) {
    const uint32 selector = random.Next32() % 16u;
    if (selector == 0) {
        return SpecialFloats[random.Next32() % (sizeof(SpecialFloats) / sizeof(float))];
    }
    if (selector == 1) {
        return QuietNaN;
    }
    // 대부분은 overflow하지 않는 크기의 유한값으로 곱·합 경로를 검사한다.
    const uint32 exponent = 100u + random.Next32() % 56u;
    return FloatBits(((random.Next32() & 1u) << 31) | (exponent << 23) |
                     (random.Next32() & 0x7fffffu));
}

void StoreBoth(platform::simd::Register4f simdValue, platform::scalar::Register4f scalarValue,
               float (&simdLanes)[4], float (&scalarLanes)[4]) {
    platform::simd::Store(simdLanes, simdValue);
    platform::scalar::Store(scalarLanes, scalarValue);
}

bool SameLanes(const float (&a)[4], const float (&b)[4]) {
    for (int lane = 0; lane < 4; ++lane) {
        if (!SameFloat(a[lane], b[lane])) {
            return false;
        }
    }
    return true;
}

bool NearLanes(const float (&a)[4], const float (&b)[4]) {
    for (int lane = 0; lane < 4; ++lane) {
        if (GenericPlatformMath::IsNaN(a[lane]) || GenericPlatformMath::IsNaN(b[lane])) {
            if (!(GenericPlatformMath::IsNaN(a[lane]) && GenericPlatformMath::IsNaN(b[lane]))) {
                return false;
            }
            continue;
        }
        const float scale = GenericPlatformMath::Max(GenericPlatformMath::Abs(a[lane]), 1.0f);
        if (!(GenericPlatformMath::Abs(a[lane] - b[lane]) <= scale * 1.0e-5f) &&
            a[lane] != b[lane]) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST(VectorRegister_ElementwiseOperationsAreBitExact) {
    namespace simd = platform::simd;
    namespace scalar = platform::scalar;
    TestRandom random{0x3141592653589793ull ^ CurrentSeed()};
    for (int iteration = 0; iteration < 50000; ++iteration) {
        float a[4];
        float b[4];
        float c[4];
        for (int lane = 0; lane < 4; ++lane) {
            a[lane] = RandomLane(random);
            b[lane] = RandomLane(random);
            c[lane] = RandomLane(random);
        }
        const auto sa = simd::Load(a);
        const auto sb = simd::Load(b);
        const auto sc = simd::Load(c);
        const auto ra = scalar::Load(a);
        const auto rb = scalar::Load(b);
        const auto rc = scalar::Load(c);
        float left[4];
        float right[4];
        StoreBoth(simd::Add(sa, sb), scalar::Add(ra, rb), left, right);
        CHECK(SameLanes(left, right));
        StoreBoth(simd::Subtract(sa, sb), scalar::Subtract(ra, rb), left, right);
        CHECK(SameLanes(left, right));
        StoreBoth(simd::Multiply(sa, sb), scalar::Multiply(ra, rb), left, right);
        CHECK(SameLanes(left, right));
        StoreBoth(simd::Divide(sa, sb), scalar::Divide(ra, rb), left, right);
        CHECK(SameLanes(left, right));
        StoreBoth(simd::MultiplyAdd(sa, sb, sc), scalar::MultiplyAdd(ra, rb, rc), left, right);
        CHECK(SameLanes(left, right));
        StoreBoth(simd::Min(sa, sb), scalar::Min(ra, rb), left, right);
        CHECK(SameLanes(left, right));
        StoreBoth(simd::Max(sa, sb), scalar::Max(ra, rb), left, right);
        CHECK(SameLanes(left, right));
        StoreBoth(simd::Sqrt(sa), scalar::Sqrt(ra), left, right);
        CHECK(SameLanes(left, right));
        StoreBoth(simd::Abs(sa), scalar::Abs(ra), left, right);
        CHECK(SameLanes(left, right));
        StoreBoth(simd::Negate(sa), scalar::Negate(ra), left, right);
        CHECK(SameLanes(left, right));
    }
}

TEST(VectorRegister_ReductionsMatchScalarOrder) {
    namespace simd = platform::simd;
    namespace scalar = platform::scalar;
    TestRandom random{0x2718281828459045ull ^ CurrentSeed()};
    for (int iteration = 0; iteration < 50000; ++iteration) {
        float a[4];
        float b[4];
        float matrix[16];
        float other[16];
        for (int lane = 0; lane < 4; ++lane) {
            a[lane] = RandomLane(random);
            b[lane] = RandomLane(random);
        }
        for (int index = 0; index < 16; ++index) {
            matrix[index] = RandomLane(random);
            other[index] = RandomLane(random);
        }
        const auto sa = simd::Load(a);
        const auto sb = simd::Load(b);
        const auto ra = scalar::Load(a);
        const auto rb = scalar::Load(b);
        const float dot3[4] = {simd::Dot3(sa, sb), simd::Dot4(sa, sb), 0.0f, 0.0f};
        const float dot3Reference[4] = {scalar::Dot3(ra, rb), scalar::Dot4(ra, rb), 0.0f, 0.0f};
        float left[4];
        float right[4];
        float simdMatrix[16];
        float scalarMatrix[16];
        simd::MatrixMultiply(matrix, other, simdMatrix);
        scalar::MatrixMultiply(matrix, other, scalarMatrix);
        bool matrixSame = true;
        bool matrixNear = true;
        for (int column = 0; column < 4; ++column) {
            float simdColumn[4];
            float scalarColumn[4];
            for (int row = 0; row < 4; ++row) {
                simdColumn[row] = simdMatrix[column * 4 + row];
                scalarColumn[row] = scalarMatrix[column * 4 + row];
            }
            matrixSame = matrixSame && SameLanes(simdColumn, scalarColumn);
            matrixNear = matrixNear && NearLanes(simdColumn, scalarColumn);
        }
        if constexpr (platform::ScalarSimdBitExact) {
            CHECK(SameLanes(dot3, dot3Reference));
            StoreBoth(simd::Cross3(sa, sb), scalar::Cross3(ra, rb), left, right);
            CHECK(SameLanes(left, right));
            StoreBoth(simd::QuaternionMultiply(sa, sb), scalar::QuaternionMultiply(ra, rb), left,
                      right);
            CHECK(SameLanes(left, right));
            StoreBoth(simd::TransformVector4(matrix, sa), scalar::TransformVector4(matrix, ra),
                      left, right);
            CHECK(SameLanes(left, right));
            CHECK(matrixSame);
        } else {
            CHECK(NearLanes(dot3, dot3Reference));
            StoreBoth(simd::QuaternionMultiply(sa, sb), scalar::QuaternionMultiply(ra, rb), left,
                      right);
            CHECK(NearLanes(left, right));
            CHECK(matrixNear);
        }
    }
}

TEST(VectorRegister_CrossProductOrientationAndZeroW) {
    namespace simd = platform::simd;
    float result[4];
    // 오른손: X × Y = Z. w는 입력과 무관하게 +0이다.
    simd::Store(result, simd::Cross3(simd::Set(1.0f, 0.0f, 0.0f, QuietNaN),
                                     simd::Set(0.0f, 1.0f, 0.0f, Infinity)));
    CHECK(result[0] == 0.0f && result[1] == 0.0f && result[2] == 1.0f);
    CHECK(result[3] == 0.0f && !GenericPlatformMath::SignBit(result[3]));
}

TEST(VectorRegister_MatrixMultiplyAllowsAliasedOutput) {
    namespace simd = platform::simd;
    float a[16];
    float b[16];
    for (int index = 0; index < 16; ++index) {
        a[index] = static_cast<float>(index + 1);
        b[index] = static_cast<float>(16 - index);
    }
    float expected[16];
    platform::scalar::MatrixMultiply(a, b, expected);
    float aliased[16];
    for (int index = 0; index < 16; ++index) {
        aliased[index] = a[index];
    }
    simd::MatrixMultiply(aliased, b, aliased);
    bool same = true;
    for (int index = 0; index < 16; ++index) {
        same = same && aliased[index] == expected[index];
    }
    CHECK(same);
    // 열 우선: (a*b)(0,0) = sum_k a(0,k) * b(k,0) = 1*16 + 5*15 + 9*14 + 13*13.
    CHECK(expected[0] == 386.0f);
}

TEST(VectorRegister_GetAndSplatLanes) {
    namespace simd = platform::simd;
    const auto value = simd::Set(1.0f, 2.0f, 3.0f, 4.0f);
    CHECK(simd::Get<0>(value) == 1.0f && simd::Get<1>(value) == 2.0f);
    CHECK(simd::Get<2>(value) == 3.0f && simd::Get<3>(value) == 4.0f);
    CHECK(simd::Get<3>(simd::Splat(-7.0f)) == -7.0f);
    CHECK(simd::Get<1>(simd::Zero()) == 0.0f);
}

} // namespace rocks::test
