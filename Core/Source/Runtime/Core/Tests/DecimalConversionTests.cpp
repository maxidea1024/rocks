#include <Core/Source/Runtime/Core/Private/Text/DecimalConversion.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NumberText.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/FloatingPointEnvironment.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>
#include <stdio.h>

using namespace rocks;
namespace decimal = rocks::detail::decimal;
using platform::BitCast;

// CRT 없는 10진 ↔ 2진 변환(DecimalConversion)의 시험이다. 파서는 알려진 어려운 사례와, ToBinary와
// 독립인 이분 탐색 기준값(중간점 정확 비교만 쓴다)으로 본다. 최단 표기는 정확한 전개로
// 최단성·최근접성을 따로 확인한다.
namespace {

constexpr uint64 DoubleInfinity = 0x7FF0000000000000ull;
constexpr uint64 FloatInfinity = 0x7F800000ull;

// 고정 시드 xorshift(시험 재현용). 시드를 splitmix로 섞고 1을 켜 상태가 0이 되지 않게 한다(0이면
// 0만 낸다).
struct Random {
    uint64 state;

    explicit Random(uint64 seed) noexcept {
        seed += 0x9E3779B97F4A7C15ull;
        seed = (seed ^ (seed >> 30)) * 0xBF58476D1CE4E5B9ull;
        seed = (seed ^ (seed >> 27)) * 0x94D049BB133111EBull;
        state = (seed ^ (seed >> 31)) | 1u;
    }

    uint64 Next() noexcept {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
};

// ASCII 숫자열과 소수점 위치로 DecimalNumber를 만든다(앞의 0은 건너뛴다).
void MakeNumber(const char* digits, usize count, int64 pointPosition,
                decimal::DecimalNumber& out) noexcept {
    out = decimal::DecimalNumber{};
    out.pointPosition = pointPosition;
    for (usize index = 0; index < count; ++index) {
        const uint8 digit = static_cast<uint8>(digits[index] - '0');
        if (out.count == 0 && digit == 0) {
            --out.pointPosition;
            continue;
        }

        if (out.count < decimal::MaxSignificantDigits) {
            out.digits[out.count++] = digit;
        } else if (digit != 0) {
            out.truncated = true;
        }
    }
}

// ToBinary와 독립인 기준값: 중간점 비교만으로 이분 탐색한다(가장 가까운 값, 동점은 짝수).
uint64 ReferenceToBinary(const decimal::DecimalNumber& number,
                         decimal::BinaryFormat format) noexcept {
    const uint64 infinity = static_cast<uint64>((1 << format.exponentBits) - 1)
                            << format.mantissaBits;
    if (number.count == 0) {
        return 0;
    }

    uint64 low = 0;
    uint64 high = infinity;
    while (low < high) {
        const uint64 middle = low + (high - low) / 2;
        if (decimal::CompareWithHalfway(number, middle, format) <= 0) {
            high = middle;
        } else {
            low = middle + 1;
        }
    }

    if (low < infinity && (low & 1u) != 0 &&
        decimal::CompareWithHalfway(number, low, format) == 0) {
        ++low;
    }

    return low;
}

// 해석 결과 비트. Overflow는 무한대 비트, 그 밖의 오류는 InvalidBits로 구별한다.
constexpr uint64 InvalidBits = ~0ull;

uint64 ParseBits(const char* text) noexcept {
    auto parsed = TryParseDouble(text);
    if (!parsed) {
        return parsed.Error() == ErrorCode::Overflow ? DoubleInfinity : InvalidBits;
    }

    return BitCast<uint64>(parsed.Value());
}

uint64 ParseFloatBits(const char* text) noexcept {
    auto parsed = TryParseFloat(text);
    if (!parsed) {
        return parsed.Error() == ErrorCode::Overflow ? FloatInfinity : InvalidBits;
    }

    return BitCast<uint32>(parsed.Value());
}

// 정확한 전개(ASCII)를 DecimalNumber로 옮긴다.
void FromExact(const decimal::ExactDecimal& exact, decimal::DecimalNumber& out) noexcept {
    MakeNumber(exact.digits, exact.count, exact.pointPosition, out);
}

// 두 정확한 값의 중간점(정확한 10진 전개)을 만든다: (a + b)/2. a ≤ b, 같은 소수점 기준으로 맞춰
// 더한 뒤 2로 나눈다.
void ExactMidpoint(const decimal::ExactDecimal& a, const decimal::ExactDecimal& b, char* out,
                   usize& count, int64& pointPosition) noexcept {
    // 공통 소수점: 높은 자리 = max(point), 낮은 자리 = max(point - count) 기준의 자리 수.
    const int64 top = (b.count == 0 ? a.pointPosition : b.pointPosition) + 1;
    int64 bottom = 0;
    bottom = static_cast<int64>(a.pointPosition) - static_cast<int64>(a.count);
    const int64 bottomB = static_cast<int64>(b.pointPosition) - static_cast<int64>(b.count);
    if (bottomB < bottom) {
        bottom = bottomB;
    }

    --bottom; // 2로 나눌 때 한 자리 더
    const int64 width = top - bottom;
    static char sum[2400];
    for (int64 index = 0; index < width; ++index) {
        sum[index] = 0;
    }

    auto add = [&](const decimal::ExactDecimal& value) noexcept {
        for (usize index = 0; index < value.count; ++index) {
            // 자리 지수 = pointPosition − 1 − index(0.d1 → 10^(point−1)).
            const int64 position =
                top - 1 - (static_cast<int64>(value.pointPosition) - 1 - static_cast<int64>(index));
            sum[position] = static_cast<char>(sum[position] + (value.digits[index] - '0'));
        }
    };
    add(a);
    add(b);
    for (int64 index = width - 1; index > 0; --index) {
        if (sum[index] >= 10) {
            sum[index] = static_cast<char>(sum[index] - 10);
            ++sum[index - 1];
        }
    }

    int remainder = 0;
    for (int64 index = 0; index < width; ++index) {
        const int current = remainder * 10 + sum[index];
        out[index] = static_cast<char>('0' + current / 2);
        remainder = current % 2;
    }

    count = static_cast<usize>(width);
    pointPosition = top;
}

// 최단 표기의 독립 검증: 다시 읽으면 같은 값이고, 한 자리 짧은 후보(정확한 값의 내림·올림)는
// 왕복하지 않으며, 같은 길이에서 가장 가까운 값(정확히 반올림한 값이 왕복하면 그것)이다. 정확히
// 반올림한 값이 왕복하지 않는 경우(2의 거듭제곱의 좁은 쪽)는 그 자릿수에서 왕복하는 후보가
// 하나뿐이라 앞의 두 조건이 답을 정한다(교차검증에서 double 46개·float 3개 전부 확인).
bool CheckShortest(uint64 bits, decimal::BinaryFormat format) noexcept {
    const decimal::ShortestDecimal shortest = decimal::Shortest(bits, format);
    char digits[24];
    int count = 0;
    char reversed[24];
    uint64 significand = shortest.significand;
    int32 exponent = shortest.exponent;
    while (significand % 10 == 0) {
        significand /= 10;
        ++exponent;
    }

    while (significand != 0) {
        reversed[count++] = static_cast<char>('0' + significand % 10);
        significand /= 10;
    }

    for (int index = 0; index < count; ++index) {
        digits[index] = reversed[count - 1 - index];
    }

    const int64 point = exponent + count;
    decimal::DecimalNumber number;
    MakeNumber(digits, static_cast<usize>(count), point, number);
    if (decimal::ToBinary(number, format) != bits) {
        return false;
    }

    const uint64 doubleBits = format.mantissaBits == 52
                                  ? bits
                                  : decimal::FloatBitsToDoubleBits(static_cast<uint32>(bits));
    decimal::ExactDecimal exact;
    decimal::ExpandExact(doubleBits, exact);
    auto roundTrips = [&](const decimal::ExactDecimal& candidate) noexcept {
        decimal::DecimalNumber parsed;
        FromExact(candidate, parsed);
        return decimal::ToBinary(parsed, format) == bits;
    };
    // 한 자리 짧은 내림(자르기)·올림.
    if (count > 1) {
        const usize keep = static_cast<usize>(count - 1);
        decimal::ExactDecimal down = exact;
        if (down.count > keep) {
            down.count = keep;
        }

        decimal::ExactDecimal up = down;
        if (exact.count > keep) {
            usize index = up.count;
            while (index > 0 && up.digits[index - 1] == '9') {
                --index;
            }

            if (index == 0) {
                up.digits[0] = '1';
                up.count = 1;
                ++up.pointPosition;
            } else {
                ++up.digits[index - 1];
                up.count = index;
            }
        }

        if (roundTrips(down) || roundTrips(up)) {
            return false;
        }
    }

    // 같은 길이에서 정확히 반올림한 값이 왕복하면 그것이어야 한다.
    decimal::ExactDecimal nearest = exact;
    decimal::RoundExact(nearest, count);
    if (roundTrips(nearest)) {
        if (nearest.pointPosition != point || nearest.count > static_cast<usize>(count)) {
            return false;
        }

        for (usize index = 0; index < nearest.count; ++index) {
            if (nearest.digits[index] != digits[index]) {
                return false;
            }
        }

        for (usize index = nearest.count; index < static_cast<usize>(count); ++index) {
            if (digits[index] != '0') {
                return false;
            }
        }
    }

    return true;
}

bool FormatsAs(const char* expected, const char* format, double value) noexcept {
    SystemAllocator allocator;
    StringBuilder<64> builder(allocator);
    if (!builder.TryAppendFormat(format, value)) {
        return false;
    }

    if (builder.View() != StringView(expected)) {
        printf("format %s: got '%.*s', expected '%s'\n", format,
               static_cast<int>(builder.ByteCount()), builder.View().Data(), expected);
        return false;
    }

    return true;
}

} // namespace

TEST(DecimalConversion_HardParseCases) {
    struct Case {
        const char* text;
        uint64 bits;
    };
    const Case cases[] = {
        {"0.1", 0x3FB999999999999Aull},
        {"1e23", 0x44B52D02C7E14AF6ull},
        {"9007199254740993", 0x4340000000000000ull}, // 2^53 + 1: 동점 → 짝수
        {"9007199254740995", 0x4340000000000002ull}, // 2^53 + 3: 동점 → 짝수(+4)
        {"9007199254740993.0000000000000000000000000000001", 0x4340000000000001ull},
        {"2.2250738585072011e-308", 0x000FFFFFFFFFFFFFull},
        {"2.2250738585072012e-308", 0x0010000000000000ull},
        {"4.9406564584124654e-324", 0x0000000000000001ull},
        {"2.4703282292062327e-324", 0x0000000000000000ull},
        {"2.4703282292062328e-324", 0x0000000000000001ull},
        {"1.7976931348623157e308", 0x7FEFFFFFFFFFFFFFull},
        {"1.7976931348623158e308", 0x7FEFFFFFFFFFFFFFull},
        {"1.7976931348623159e308", DoubleInfinity},
        {"8.98846567431158e307", 0x7FE0000000000000ull},
        {"1e-400", 0x0000000000000000ull},
        {"1e400", DoubleInfinity},
        {"0.000000000000000000000000000000000000000000000000000000000000000000000000001e75",
         0x3FF0000000000000ull},
        {"100000000000000000000000000000000000000000000000000000000000e-59", 0x3FF0000000000000ull},
        {"100000000000000000000000000000000000000000000000000000000000e-60", 0x3FB999999999999Aull},
        {"1e999999999999999999999", DoubleInfinity},
        {"0e999999999999999999999", 0x0000000000000000ull},
    };
    for (const Case& item : cases) {
        const uint64 actual = ParseBits(item.text);
        if (actual != item.bits) {
            printf("parse %s: 0x%016llX, expected 0x%016llX\n", item.text,
                   static_cast<unsigned long long>(actual),
                   static_cast<unsigned long long>(item.bits));
        }

        CHECK(actual == item.bits);
    }

    const Case floats[] = {
        {"1.17549435e-38", 0x00800000u},
        {"3.4028235e38", 0x7F7FFFFFu},
        {"3.4028236e38", FloatInfinity},
        {"1.4e-45", 0x00000001u},
        {"7.0e-46", 0x00000000u},
        {"16777217", 0x4B800000u},
        {"0.1", 0x3DCCCCCDu},
        {"1.00000005960464477539062500000000001", 0x3F800001u},
        {"1.000000059604644775390625", 0x3F800000u},
    };
    for (const Case& item : floats) {
        const uint64 actual = ParseFloatBits(item.text);
        if (actual != item.bits) {
            printf("parse float %s: 0x%08llX, expected 0x%08llX\n", item.text,
                   static_cast<unsigned long long>(actual),
                   static_cast<unsigned long long>(item.bits));
        }

        CHECK(actual == item.bits);
    }

    CHECK(BitCast<uint64>(TryParseDouble("-0").Value()) == 0x8000000000000000ull);
    CHECK(BitCast<uint64>(TryParseDouble("-0.0e-5").Value()) == 0x8000000000000000ull);
}

TEST(DecimalConversion_LongHalfwayInputs) {
    // 0과 최소 비정규 수의 중간점 2^-1075(정확한 10진 전개, 750자리 이상)는 짝수(0)로, 아주 먼
    // 자리에 0이 아닌 숫자를 더하면 최소 비정규 수로 간다. 769자리를 넘는 꼬리는 0이 아닌지만 본다.
    decimal::ExactDecimal zero;
    decimal::ExactDecimal smallest;
    decimal::ExpandExact(0, zero);
    decimal::ExpandExact(1, smallest);
    static char midpoint[2400];
    usize count = 0;
    int64 point = 0;
    ExactMidpoint(zero, smallest, midpoint, count, point);
    decimal::DecimalNumber number;
    MakeNumber(midpoint, count, point, number);
    CHECK(number.count > 700);
    CHECK(decimal::ToBinary(number, decimal::DoubleFormat) == 0);
    static char longer[3000];
    platform::MemoryCopy(longer, midpoint, count);
    for (usize index = count; index < 2900; ++index) {
        longer[index] = '0';
    }

    longer[2899] = '1';
    MakeNumber(longer, 2900, point, number);
    CHECK(number.truncated);
    CHECK(decimal::ToBinary(number, decimal::DoubleFormat) == 1);
    // 같은 수를 문자열로: "0.<0…>…"는 길이 제한 없이 읽는다.
    static char text[3200];
    usize length = 0;
    text[length++] = '0';
    text[length++] = '.';
    for (int64 zeroes = 0; zeroes < -point; ++zeroes) {
        text[length++] = '0';
    }

    platform::MemoryCopy(text + length, longer, 2900);
    length += 2900;
    auto parsed = TryParseDouble(StringView(text, length));
    CHECK(parsed && BitCast<uint64>(parsed.Value()) == 1);
}

TEST(DecimalConversion_ParserMatchesBisectionReference) {
    Random random{0xD0C1A55Eull ^ test::CurrentSeed()};
    usize checked = 0;
    // 1) 무작위 10진 수(1–40자리, 넓은 지수).
    for (int iteration = 0; iteration < 1500; ++iteration) {
        char digits[64];
        const usize count = 1 + random.Next() % 40;
        for (usize index = 0; index < count; ++index) {
            digits[index] = static_cast<char>('0' + random.Next() % 10);
        }

        const int64 point = static_cast<int64>(random.Next() % 700) - 350;
        decimal::DecimalNumber number;
        MakeNumber(digits, count, point, number);
        const decimal::BinaryFormat format =
            (iteration & 3) == 0 ? decimal::FloatFormat : decimal::DoubleFormat;
        CHECK(decimal::ToBinary(number, format) == ReferenceToBinary(number, format));
        ++checked;
    }

    // 2) 두 이웃 double의 정확한 중간점과 그 위아래(마지막 자리 ±, 먼 꼬리) — 느린 경로를 강제한다.
    for (int iteration = 0; iteration < 300; ++iteration) {
        uint64 bits = random.Next() & 0x7FFFFFFFFFFFFFFFull;
        if ((bits >> 52) == 0x7FF) {
            bits ^= 0x4000000000000000ull;
        }

        // 1/8은 가수를 전부 1로(2의 거듭제곱 바로 아래 — 다음 값과의 중간점이 비대칭 경계, 최대
        // 유한 값은 넘침 경계).
        if (iteration % 8 == 0) {
            bits |= 0x000FFFFFFFFFFFFFull;
        }

        if (bits == 0x7FEFFFFFFFFFFFFFull) {
            continue; // 다음 값이 무한대라 정확한 전개가 없다(넘침 경계는 HardParseCases가 본다)
        }

        decimal::ExactDecimal low;
        decimal::ExactDecimal high;
        decimal::ExpandExact(bits, low);
        decimal::ExpandExact(bits + 1, high);
        static char midpoint[2400];
        usize count = 0;
        int64 point = 0;
        ExactMidpoint(low, high, midpoint, count, point);
        decimal::DecimalNumber number;
        MakeNumber(midpoint, count, point, number);
        const uint64 expected = (bits & 1u) == 0 ? bits : bits + 1;
        CHECK(decimal::ToBinary(number, decimal::DoubleFormat) == expected);
        CHECK(ReferenceToBinary(number, decimal::DoubleFormat) == expected);
        // 마지막 유효 숫자를 하나 줄이면 아래, 아주 먼 0이 아닌 꼬리를 붙이면 위.
        usize last = count;
        while (last > 0 && midpoint[last - 1] == '0') {
            --last;
        }

        if (last != 0 && midpoint[last - 1] != '0') {
            --midpoint[last - 1];
            MakeNumber(midpoint, count, point, number);
            CHECK(decimal::ToBinary(number, decimal::DoubleFormat) == bits);
            ++midpoint[last - 1];
        }

        midpoint[count] = '1';
        MakeNumber(midpoint, count + 1, point, number);
        CHECK(decimal::ToBinary(number, decimal::DoubleFormat) == bits + 1);
        checked += 3;
    }

    CHECK(checked > 2000);
}

TEST(DecimalConversion_ShortestIsShortestAndNearest) {
    Random random{0x5407E57ull ^ test::CurrentSeed()};
    usize checked = 0;
    for (int iteration = 0; iteration < 4000; ++iteration) {
        uint64 bits = random.Next() & 0x7FFFFFFFFFFFFFFFull;
        if ((bits >> 52) == 0x7FF || bits == 0) {
            continue;
        }

        if (!CheckShortest(bits, decimal::DoubleFormat)) {
            printf("shortest double 0x%016llX failed\n", static_cast<unsigned long long>(bits));
            CHECK(false);
        }

        ++checked;
        uint32 single = static_cast<uint32>(random.Next()) & 0x7FFFFFFFu;
        if ((single >> 23) == 0xFF || single == 0) {
            continue;
        }

        if (!CheckShortest(single, decimal::FloatFormat)) {
            printf("shortest float 0x%08X failed\n", single);
            CHECK(false);
        }

        ++checked;
    }

    CHECK(checked > 7000);
    // 경계: 2의 거듭제곱(비대칭 구간), 최소·최대, 비정규, .NET 10이 틀리는 2^-25·2^-958.
    const uint64 doubles[] = {0x0000000000000001ull, 0x000FFFFFFFFFFFFFull, 0x0010000000000000ull,
                              0x7FEFFFFFFFFFFFFFull, 0x3FF0000000000000ull, 0x4340000000000000ull,
                              0x0020000000000000ull, 0x44B52D02C7E14AF6ull, 0x3E60000000000000ull,
                              0x0410000000000000ull};
    for (uint64 bits : doubles) {
        CHECK(CheckShortest(bits, decimal::DoubleFormat));
    }

    const uint32 floats[] = {0x00000001u, 0x007FFFFFu, 0x00800000u,
                             0x7F7FFFFFu, 0x3F800000u, 0x4B800000u};
    for (uint32 bits : floats) {
        CHECK(CheckShortest(bits, decimal::FloatFormat));
    }
}

TEST(DecimalConversion_PrecisionFormattingRoundsExactValue) {
    // 정확한 2진 값에서 가장 가까운 값(동점은 짝수): 0.125·0.375·2.5는 정확한 동점, 2.675는
    // 실제로 2.67499…다.
    CHECK(FormatsAs("0.10000000000000000555", "{:.20f}", 0.1));
    CHECK(FormatsAs("0", "{:.0f}", 0.5) && FormatsAs("2", "{:.0f}", 1.5) &&
          FormatsAs("2", "{:.0f}", 2.5));
    CHECK(FormatsAs("4", "{:.0f}", 3.5) && FormatsAs("0.12", "{:.2f}", 0.125) &&
          FormatsAs("0.38", "{:.2f}", 0.375));
    CHECK(FormatsAs("2.67", "{:.2f}", 2.675) && FormatsAs("1.000e+300", "{:.3e}", 1e300));
    CHECK(FormatsAs("4.940656e-324", "{:e}", BitCast<double>(uint64{1})));
    CHECK(FormatsAs("0.10000000000000001", "{:.17g}", 0.1) &&
          FormatsAs("100000", "{:g}", 100000.0));
    CHECK(FormatsAs("1e+06", "{:g}", 1e6) && FormatsAs("0.0001", "{:g}", 0.0001) &&
          FormatsAs("1e-05", "{:g}", 0.00001));
    CHECK(FormatsAs("0", "{:g}", 0.0) && FormatsAs("10", "{:.3g}", 9.9951) &&
          FormatsAs("1E-10", "{:G}", 1e-10));
    CHECK(FormatsAs("9.99e+05", "{:.3g}", 999499.0) && FormatsAs("1e+06", "{:.3g}", 999500.0));
    CHECK(FormatsAs("0.000", "{:.3f}", 0.0004999) && FormatsAs("0.001", "{:.3f}", 0.0005000001));
    CHECK(FormatsAs("123456789012345677877719597056.000", "{:.3f}",
                    123456789012345677877719597056.0));
    // 최소 비정규 수의 정확한 전개는 1074자리이고 5^1074로 끝난다(…625).
    SystemAllocator allocator;
    StringBuilder<> builder(allocator);
    REQUIRE(builder.TryAppendFormat("{:.1074f}", BitCast<double>(uint64{1})));
    CHECK(builder.ByteCount() == 2 + 1074 && builder.View().EndsWith("625"));
    CHECK(builder.View().StartsWith("0.000000"));
}

TEST(DecimalConversion_FloatBitsWidenExactly) {
    // 모든 float 지수 구간의 경계와 비정규 수: 정수 연산 결과가 기본 환경의 float → double 변환과
    // 같다.
    const uint32 samples[] = {0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u,
                              0x00400000u, 0x007FFFFFu, 0x00800000u, 0x3F800000u,
                              0x7F7FFFFFu, 0x7F800000u, 0xFF800000u, 0x3DCCCCCDu};
    for (uint32 bits : samples) {
        CHECK(decimal::FloatBitsToDoubleBits(bits) ==
              BitCast<uint64>(static_cast<double>(BitCast<float>(bits))));
    }

    Random random{0xF10A7ull ^ test::CurrentSeed()};
    for (int iteration = 0; iteration < 100000; ++iteration) {
        const uint32 bits = static_cast<uint32>(random.Next());
        if ((bits & 0x7F800000u) == 0x7F800000u && (bits & 0x007FFFFFu) != 0) {
            CHECK((decimal::FloatBitsToDoubleBits(bits) & 0x7FF8000000000000ull) ==
                  0x7FF8000000000000ull);
            continue; // NaN은 quiet NaN이면 된다
        }

        CHECK(decimal::FloatBitsToDoubleBits(bits) ==
              BitCast<uint64>(static_cast<double>(BitCast<float>(bits))));
    }
}

TEST(DecimalConversion_IndependentOfFlushToZeroAndDenormalsAreZero) {
    // 해석·서식은 정수 연산만 쓰므로 FTZ·DAZ가 켜진 스레드에서도 비정규 수를 그대로
    // 다룬다(교차검증에서 FTZ의 float 서식 무한 루프와 DAZ의 "0" 출력을 재현해 고쳤다).
    SystemAllocator allocator;
    StringBuilder<64> builder(allocator);
    const char* const parseTexts[] = {"1e-45", "1.1754942e-38", "-3e-40", "5.877472e-39"};
    uint32 expectedFloats[4] = {};
    for (usize index = 0; index < 4; ++index) {
        expectedFloats[index] = BitCast<uint32>(TryParseFloat(parseTexts[index]).Value());
    }

    const uint64 expectedDouble = BitCast<uint64>(TryParseDouble("4.9e-324").Value());
    const float subnormalFloat = BitCast<float>(0x00400000u);
    const double subnormalDouble = BitCast<double>(uint64{1});
    char expectedText[4][64] = {};
    auto format = [&](usize slot, const char* pattern, auto value) noexcept {
        builder.Clear();
        const bool ok = static_cast<bool>(builder.TryAppendFormat(pattern, value));
        snprintf(expectedText[slot], sizeof(expectedText[slot]), "%.*s",
                 static_cast<int>(builder.ByteCount()), builder.View().Data());
        return ok;
    };
    REQUIRE(format(0, "{}", subnormalFloat) && format(1, "{:.3e}", subnormalFloat));
    REQUIRE(format(2, "{}", subnormalDouble) && format(3, "{:e}", subnormalDouble));
    CHECK(StringView(expectedText[0]) == StringView("5.877472e-39"));
    CHECK(StringView(expectedText[2]) == StringView("5e-324"));

    const uint64 original = platform::detail::ReadFloatingPointControl();
    platform::detail::WriteFloatingPointControl(original | platform::detail::FlushToZeroBit() |
                                                platform::detail::DenormalsAreZeroBit());
    bool same = true;
    for (usize index = 0; index < 4; ++index) {
        auto parsed = TryParseFloat(parseTexts[index]);
        same = same && parsed && BitCast<uint32>(parsed.Value()) == expectedFloats[index];
    }

    auto parsedDouble = TryParseDouble("4.9e-324");
    same = same && parsedDouble && BitCast<uint64>(parsedDouble.Value()) == expectedDouble;
    const char* const patterns[] = {"{}", "{:.3e}"};
    for (usize index = 0; index < 2; ++index) {
        builder.Clear();
        same = same && builder.TryAppendFormat(patterns[index], subnormalFloat) &&
               builder.View() == StringView(expectedText[index]);
    }

    const char* const doublePatterns[] = {"{}", "{:e}"};
    for (usize index = 0; index < 2; ++index) {
        builder.Clear();
        same = same && builder.TryAppendFormat(doublePatterns[index], subnormalDouble) &&
               builder.View() == StringView(expectedText[2 + index]);
    }

    platform::detail::WriteFloatingPointControl(original);
    CHECK(same);
}
