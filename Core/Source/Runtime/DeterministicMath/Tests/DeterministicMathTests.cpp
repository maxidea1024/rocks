#include "DeterministicMathReference.generated.h"

#include <Core/Source/Runtime/DeterministicMath/Private/SrgbTables.generated.h>
#include <DeterministicMath/DeterministicMath.h>
#include <Platform/FloatingPointEnvironment.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>
#include <stdio.h>

using namespace rocks;
using namespace rocks::deterministic;
using platform::BitCast;

// 결정적 수학의 특수값·정확한 경우·정확도(생성한 고정밀 기준값 대비)·sRGB 표의 정확성을 본다. 결과
// 비트 고정은 DeterministicMath_GoldenDigest가 맡는다(Version과 함께 바뀌어야 한다).
namespace {

constexpr uint64 CanonicalNaN = 0x7FF8000000000000ull;
constexpr uint64 SignBit = 0x8000000000000000ull;

uint64 Bits(double value) noexcept {
    return BitCast<uint64>(value);
}

// 두 유한 double 사이의 ulp 거리다(부호가 다르면 0을 사이에 둔 거리의 합).
uint64 UlpDistance(double left, double right) noexcept {
    const uint64 a = Bits(left);
    const uint64 b = Bits(right);
    if ((a ^ b) & SignBit) {
        return (a & ~SignBit) + (b & ~SignBit);
    }

    return a > b ? a - b : b - a;
}

// 64-bit FNV-1a: 출력 비트 열의 고정용 digest.
uint64 Fnv(uint64 hash, uint64 value) noexcept {
    for (int index = 0; index < 8; ++index) {
        hash ^= (value >> (8 * index)) & 0xFF;
        hash *= 0x100000001B3ull;
    }

    return hash;
}

double Double(uint64 bits) noexcept {
    return BitCast<double>(bits);
}

// 기준값 표 하나를 검사하고 최대 ulp 거리를 돌려준다.
template <usize Count, class Function>
uint64 CheckReference(const char* name, const reference::ReferenceCase (&cases)[Count],
                      uint64 bound, Function function) noexcept {
    uint64 worst = 0;
    for (const reference::ReferenceCase& item : cases) {
        const double actual = function(Double(item.input), Double(item.second));
        const uint64 distance = UlpDistance(actual, Double(item.expected));
        if (distance > bound) {
            printf("%s(%.17g, %.17g) = %.17g, expected %.17g (%llu ulp)\n", name,
                   Double(item.input), Double(item.second), actual, Double(item.expected),
                   static_cast<unsigned long long>(distance));
            CHECK(distance <= bound);
        }

        worst = distance > worst ? distance : worst;
    }

    printf("%s: %llu cases, worst %llu ulp (bound %llu)\n", name,
           static_cast<unsigned long long>(Count), static_cast<unsigned long long>(worst),
           static_cast<unsigned long long>(bound));
    return worst;
}

// 결과가 비정규 수인 Exp2 표본의 digest다. FTZ·FZ가 켜져 있으면 결과가 0이 되어 달라진다. 시작값을
// volatile로 읽어 컴파일 시점(LTO 포함) 계산을 막는다.
uint64 SubnormalDigest() noexcept {
    volatile double start = -1074.0;
    uint64 hash = 0xCBF29CE484222325ull;
    for (int step = 0; step < 64; ++step) {
        hash = Fnv(hash, Bits(Exp2(start + 0.375 * step))); // 2^-1074 … 2^-1050.4
    }

    return hash;
}

// 기본 환경에서의 SubnormalDigest 값이다(Version과 함께 고정 — 비정규 결과 비트도 계약에 든다).
constexpr uint64 SubnormalDigestValue = 0x05879A39715B3F0Dull;

} // namespace

TEST(DeterministicMath_RoundingFunctions) {
    CHECK(Trunc(2.75) == 2.0 && Trunc(-2.75) == -2.0 && Bits(Trunc(-0.5)) == Bits(-0.0));
    CHECK(Floor(-2.25) == -3.0 && Floor(2.25) == 2.0 && Floor(-3.0) == -3.0);
    CHECK(RoundHalfEven(2.5) == 2.0 && RoundHalfEven(3.5) == 4.0 && RoundHalfEven(-2.5) == -2.0);
    CHECK(RoundHalfEven(1e300) == 1e300 && Bits(RoundHalfEven(-0.4)) == Bits(-0.0));
    CHECK(RoundHalfEven(4503599627370495.5) == 4503599627370496.0);
    CHECK(Bits(Trunc(BitCast<double>(uint64{0x7FF0000000000123}))) == CanonicalNaN);
}

TEST(DeterministicMath_ExactCasesAndSpecialValues) {
    for (int n = -1074; n <= 1023; ++n) {
        const double expected = n >= -1022 ? BitCast<double>(static_cast<uint64>(n + 1023) << 52)
                                           : BitCast<double>(uint64{1} << (n + 1074));
        if (Bits(Exp2(static_cast<double>(n))) != Bits(expected)) {
            printf("Exp2(%d) is not exact\n", n);
            CHECK(false);
            break;
        }

        if (Log2(expected) != static_cast<double>(n)) {
            printf("Log2(2^%d) is not exact\n", n);
            CHECK(false);
            break;
        }
    }

    CHECK(Exp2(-1075.0) == 0.0 && Exp2(1024.0) > 1e308 && Exp2(-1100.0) == 0.0);
    CHECK(Pow(2.0, 10.0) == 1024.0 && Pow(3.0, 3.0) == 27.0 && Pow(5.0, 0.0) == 1.0);
    // C pow처럼 지수 0과 밑 1은 NaN 입력에도 1이다.
    const double payload = BitCast<double>(uint64{0xFFF8000000000ABC});
    CHECK(Pow(payload, 0.0) == 1.0 && Pow(1.0, payload) == 1.0 && Pow(1.0, 1e300) == 1.0);
    CHECK(Bits(Pow(payload, 2.0)) == CanonicalNaN && Bits(Pow(2.0, payload)) == CanonicalNaN);
    CHECK(Bits(Pow(-1.0, 0.5)) == CanonicalNaN && Bits(Pow(-1e300 * 1e300, 2.0)) == CanonicalNaN);
    CHECK(Bits(Pow(0.0, 2.0)) == Bits(0.0) && Bits(Pow(-0.0, 3.0)) == Bits(0.0));
    CHECK(Pow(0.0, -1.0) > 1e308 && Pow(-0.0, -3.0) > 1e308);
    CHECK(Pow(1e300 * 1e300, 2.0) > 1e308 && Bits(Pow(1e300 * 1e300, -2.0)) == Bits(0.0));
    const double infinity = 1e300 * 1e300;
    CHECK(Pow(2.0, infinity) > 1e308 && Pow(0.5, infinity) == 0.0 && Pow(2.0, -infinity) == 0.0);
    CHECK(Pow(0.5, -infinity) > 1e308 && Pow(10.0, 400.0) > 1e308 && Pow(10.0, -400.0) == 0.0);
    CHECK(Bits(Log2(-1.0)) == CanonicalNaN && Log2(0.0) < -1e308 && Log2(infinity) > 1e308);
    // 입력 NaN의 payload와 무관하게 고정 NaN이다.
    CHECK(Bits(Exp2(payload)) == CanonicalNaN && Bits(SinPi(payload)) == CanonicalNaN);
    CHECK(Bits(CosPi(infinity)) == CanonicalNaN && Bits(Sinc(payload)) == CanonicalNaN);
    // sin(πx)·cos(πx)의 정수·반정수와 0의 부호(IEEE 754-2019: sinPi(±n) = ±0, cosPi(n + ½) = +0).
    CHECK(SinPi(0.5) == 1.0 && SinPi(1.5) == -1.0 && SinPi(-0.5) == -1.0);
    CHECK(Bits(SinPi(0.0)) == Bits(0.0) && Bits(SinPi(-0.0)) == Bits(-0.0));
    CHECK(Bits(SinPi(1.0)) == Bits(0.0) && Bits(SinPi(3.0)) == Bits(0.0));
    CHECK(Bits(SinPi(-1.0)) == Bits(-0.0) && Bits(SinPi(-2.0)) == Bits(-0.0));
    CHECK(Bits(SinPi(4503599627370496.0)) == Bits(0.0) &&
          Bits(SinPi(-4503599627370496.0)) == Bits(-0.0));
    CHECK(CosPi(0.0) == 1.0 && CosPi(1.0) == -1.0 && CosPi(2.0) == 1.0 && CosPi(-3.0) == -1.0);
    CHECK(Bits(CosPi(0.5)) == Bits(0.0) && Bits(CosPi(-0.5)) == Bits(0.0) &&
          Bits(CosPi(1.5)) == Bits(0.0));
    CHECK(CosPi(4503599627370497.0) == -1.0 && CosPi(9007199254740994.0) == 1.0);
    // Lanczos: 중심 1, 지지 경계와 그 밖 0, 비트 단위로 대칭.
    CHECK(Sinc(0.0) == 1.0 && Lanczos(0.0, 3) == 1.0 && Lanczos(3.0, 3) == 0.0 &&
          Lanczos(-3.5, 3) == 0.0);
    const double symmetric[] = {0.75, 1.0, 1.5, 2.25, 2.9990459354349017};
    for (double x : symmetric) {
        CHECK(Bits(Lanczos(x, 3)) == Bits(Lanczos(-x, 3)));
    }

    CHECK(Bits(Lanczos(1.0, 3)) == Bits(0.0) && Bits(Lanczos(2.0, 2)) == Bits(0.0));
    CHECK(Bits(Lanczos(0.5, 0)) == CanonicalNaN);
}

TEST(DeterministicMath_AccuracyAgainstReferenceTables) {
    // 기준값은 400비트 고정소수점으로 계산해 가장 가까운 double로 반올림한
    // 값이다(MathReference.cs). 상한은 헤더와 CoreDataFoundations.ko.md 2.2절에 적은 값이다.
    CheckReference("Exp2", reference::Exp2Reference, 1, [](double x, double) { return Exp2(x); });
    CheckReference("Log2", reference::Log2Reference, 1, [](double x, double) { return Log2(x); });
    CheckReference("Pow", reference::PowReference, 2, [](double x, double y) { return Pow(x, y); });
    CheckReference("SinPi", reference::SinPiReference, 2,
                   [](double x, double) { return SinPi(x); });
    CheckReference("CosPi", reference::CosPiReference, 2,
                   [](double x, double) { return CosPi(x); });
    CheckReference("SrgbToLinear", reference::SrgbToLinearReference, 2,
                   [](double x, double) { return SrgbToLinear(x); });
    CheckReference("LinearToSrgb", reference::LinearToSrgbReference, 2,
                   [](double x, double) { return LinearToSrgb(x); });
    CheckReference("Gamma22ToLinear", reference::Gamma22Reference, 2,
                   [](double x, double) { return Gamma22ToLinear(x); });
}

TEST(DeterministicMath_SrgbByteTablesAreExact) {
    // 256개 byte 모두 왕복하고 decode 표는 단조 증가한다.
    for (int value = 0; value < 256; ++value) {
        CHECK(LinearToSrgbByte(SrgbByteToLinear(static_cast<uint8>(value))) == value);
        if (value != 0) {
            CHECK(SrgbByteToLinear(static_cast<uint8>(value)) >
                  SrgbByteToLinear(static_cast<uint8>(value - 1)));
        }

        // 표 값은 double 구간 함수를 float로 반올림한 값과 같다(경계에서 충분히 떨어져 있다).
        CHECK(SrgbByteToLinear(static_cast<uint8>(value)) ==
              static_cast<float>(SrgbToLinear(static_cast<double>(value) / 255.0)));
    }

    // 경계 t_k와 그 바로 아래 float: 부호화 값이 k + 0.5 이상/미만이고 byte가 k + 1/k다. 정확한 값
    // 기준 최소 여유(약 2e-9)가 double 오차보다 훨씬 커서 double 구간 함수로 판정할 수 있다.
    for (int k = 0; k < 255; ++k) {
        const float threshold = BitCast<float>(detail::SrgbEncodeThresholdBits[k]);
        const float below = BitCast<float>(detail::SrgbEncodeThresholdBits[k] - 1u);
        CHECK(LinearToSrgb(threshold) * 255.0 >= k + 0.5);
        CHECK(LinearToSrgb(below) * 255.0 < k + 0.5);
        CHECK(LinearToSrgbByte(threshold) == k + 1 && LinearToSrgbByte(below) == k);
    }

    CHECK(LinearToSrgbByte(-1.0f) == 0 && LinearToSrgbByte(0.0f) == 0 &&
          LinearToSrgbByte(1.0f) == 255);
    CHECK(LinearToSrgbByte(BitCast<float>(0x7FC00000u)) == 0 && LinearToSrgbByte(2.0f) == 255);
    CHECK(LinearToSrgbByte(BitCast<float>(0x7F800000u)) == 255 &&
          LinearToSrgbByte(BitCast<float>(1u)) == 0);
}

TEST(DeterministicMath_GoldenDigest) {
    // 모든 함수와 분기(비정규 결과·입력, 큰 인자, 지지 경계, 여러 lobes, 범위 밖 sRGB, byte 표)를
    // 덮는 고정 시드 표본의 출력 비트 digest다.
    uint64 hash = 0xCBF29CE484222325ull;
    uint32 state = 2026;
    auto next = [&state]() noexcept {
        state = state * 1664525u + 1013904223u;
        return static_cast<double>(state) / 4294967296.0; // [0, 1)
    };
    for (int sample = 0; sample < 4096; ++sample) {
        const double x = next();
        const double y = next();
        hash = Fnv(hash, Bits(Exp2(x * 2110.0 - 1080.0)));
        hash = Fnv(hash, Bits(Exp2(x * 40.0 - 20.0)));
        hash = Fnv(hash, Bits(Log2(Exp2(y * 2090.0 - 1074.0) * (1.0 + x))));
        hash = Fnv(hash, Bits(Pow(x * 4.0, y * 20.0 - 10.0)));
        hash = Fnv(hash, Bits(Pow(Exp2(x * 600.0 - 300.0), y * 4.0 - 2.0)));
        hash = Fnv(hash, Bits(SinPi(x * 8.0 - 4.0)));
        hash = Fnv(hash, Bits(CosPi(x * 8.0 - 4.0)));
        hash = Fnv(hash, Bits(SinPi((y - 0.5) * 1e16)));
        hash = Fnv(hash, Bits(CosPi((y - 0.5) * 1e16)));
        hash = Fnv(hash, Bits(Sinc(x * 20.0 - 10.0)));
        hash = Fnv(hash, Bits(Lanczos(x * 6.0 - 3.0, 3)));
        hash = Fnv(hash, Bits(Lanczos(x * 8.0 - 4.0, 1 + static_cast<int>(y * 4.0))));
        hash = Fnv(hash, Bits(SrgbToLinear(x * 2.0 - 0.5)));
        hash = Fnv(hash, Bits(LinearToSrgb(x * 2.0 - 0.5)));
        hash = Fnv(hash, Bits(Gamma22ToLinear(x * 2.0 - 0.25)));
        hash = Fnv(hash, Bits(RoundHalfEven((x - 0.5) * 1e6)));
        hash = Fnv(hash, LinearToSrgbByte(static_cast<float>(x * 1.25 - 0.1)));
    }

    for (int value = 0; value < 256; ++value) {
        hash = Fnv(hash, BitCast<uint32>(SrgbByteToLinear(static_cast<uint8>(value))));
    }

    // 결과 비트가 바뀌면(Version을 올려야 하는 변경) 이 값과 Version을 함께 갱신한다.
    printf("DeterministicMath digest (Version %u): 0x%016llX\n", Version,
           static_cast<unsigned long long>(hash));
    CHECK(Version == 1);
    CHECK(hash == 0xD39825CFC3A07088ull);
}

TEST(DeterministicMath_EnvironmentRunRestoresResultsUnderFlushToZero) {
    const uint64 original = platform::detail::ReadFloatingPointControl();
    const uint64 expected = SubnormalDigest();
    printf("DeterministicMath subnormal digest: 0x%016llX\n",
           static_cast<unsigned long long>(expected));
    CHECK(expected == SubnormalDigestValue);

    // 다른 코드가 FTZ를 켜 놓은 상황: 기본 환경 밖에서 계산하면 비정규 결과가 0이 되어 digest가
    // 달라지고(이 시험의 민감도), RunWithDeterministicFloatingPoint 안에서 계산하면 기본 환경과
    // 같은 비트가 나온다.
    platform::detail::WriteFloatingPointControl(original | platform::detail::FlushToZeroBit());
    const uint64 flushed = SubnormalDigest();
    uint64 deterministic = 0;
    const bool changed = platform::RunWithDeterministicFloatingPoint(
        [](void* context) noexcept { *static_cast<uint64*>(context) = SubnormalDigest(); },
        &deterministic);
    const bool flushStillOn =
        (platform::detail::ReadFloatingPointControl() & platform::detail::FlushToZeroBit()) != 0;
    platform::detail::WriteFloatingPointControl(original);

    CHECK(flushed != expected);
    CHECK(changed && flushStillOn);
    CHECK(deterministic == expected);
    CHECK(platform::IsDeterministicFloatingPointEnvironment());
}
