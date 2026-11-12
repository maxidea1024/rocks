#include "DecimalConversion.h"

#include "DecimalTables.generated.h"

#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <Platform/PlatformMath.h>

namespace rocks::detail::decimal {
namespace {

using platform::PlatformMath;

// ---- 정수 도구 ----

struct UInt128 {
    uint64 low;
    uint64 high;
};

// 64 × 64 → 128비트 곱. 32비트 조각으로 계산한다(compiler 128비트 정수·intrinsic에 기대지 않는다).
UInt128 Multiply64(uint64 left, uint64 right) noexcept {
    const uint64 leftLow = left & 0xFFFFFFFFu;
    const uint64 leftHigh = left >> 32;
    const uint64 rightLow = right & 0xFFFFFFFFu;
    const uint64 rightHigh = right >> 32;
    const uint64 lowLow = leftLow * rightLow;
    const uint64 lowHigh = leftLow * rightHigh;
    const uint64 highLow = leftHigh * rightLow;
    const uint64 highHigh = leftHigh * rightHigh;
    const uint64 middle = (lowLow >> 32) + (lowHigh & 0xFFFFFFFFu) + (highLow & 0xFFFFFFFFu);
    return {(middle << 32) | (lowLow & 0xFFFFFFFFu),
            highHigh + (lowHigh >> 32) + (highLow >> 32) + (middle >> 32)};
}

// 192비트 부호 없는 정수(word[0]이 하위).
struct UInt192 {
    uint64 word[3];
};

// value × factor(128비트, {하위, 상위}).
UInt192 Multiply(uint64 value, const uint64* factor) noexcept {
    const UInt128 low = Multiply64(value, factor[0]);
    const UInt128 high = Multiply64(value, factor[1]);
    UInt192 result{};
    result.word[0] = low.low;
    result.word[1] = low.high + high.low;
    result.word[2] = high.high + (result.word[1] < low.high ? 1u : 0u);
    return result;
}

void Add(UInt192& value, uint64 addend) noexcept {
    value.word[0] += addend;
    if (value.word[0] >= addend) {
        return;
    }

    if (++value.word[1] == 0) {
        ++value.word[2];
    }
}

int32 BitLength(const UInt192& value) noexcept {
    for (int32 index = 2; index >= 0; --index) {
        if (value.word[index] != 0) {
            return index * 64 + 64 -
                   static_cast<int32>(PlatformMath::CountLeadingZeros64(value.word[index]));
        }
    }

    return 0;
}

bool Bit(const UInt192& value, int32 index) noexcept {
    return index >= 0 && index < 192 && ((value.word[index / 64] >> (index % 64)) & 1u) != 0;
}

// index보다 아래(0 … index - 1)에 켜진 비트가 있는지 본다.
bool AnyBitBelow(const UInt192& value, int32 index) noexcept {
    if (index <= 0) {
        return false;
    }

    if (index >= 192) {
        return (value.word[0] | value.word[1] | value.word[2]) != 0;
    }

    const int32 whole = index / 64;
    for (int32 word = 0; word < whole; ++word) {
        if (value.word[word] != 0) {
            return true;
        }
    }

    const int32 rest = index % 64;
    return rest != 0 && (value.word[whole] & ((uint64{1} << rest) - 1u)) != 0;
}

// value >> shift의 하위 64비트(shift는 0 이상).
uint64 ShiftRightTo64(const UInt192& value, int32 shift) noexcept {
    if (shift >= 192) {
        return 0;
    }

    const int32 word = shift / 64;
    const int32 rest = shift % 64;
    uint64 result = value.word[word] >> rest;
    if (rest != 0 && word + 1 < 3) {
        result |= value.word[word + 1] << (64 - rest);
    }

    return result;
}

// value / 2^shift를 가장 가까운 정수(동점은 짝수)로. 결과가 64비트에 든다고 호출자가 보장한다.
uint64 ShiftRightRounded(const UInt192& value, int32 shift) noexcept {
    if (shift <= 0) {
        return value.word[0] << -shift;
    }

    uint64 result = ShiftRightTo64(value, shift);
    if (Bit(value, shift - 1) && (AnyBitBelow(value, shift - 1) || (result & 1u) != 0)) {
        ++result;
    }

    return result;
}

int32 Bias(BinaryFormat format) noexcept {
    return (1 << (format.exponentBits - 1)) - 1;
}

// value × 2^exponent2(value ≠ 0)를 format으로 반올림한 비트(부호 제외). 넘치면 무한대 비트다.
uint64 RoundToBinary(const UInt192& value, int32 exponent2, BinaryFormat format) noexcept {
    const int32 mantissaBits = format.mantissaBits;
    const int32 maximumBiased = (1 << format.exponentBits) - 1;
    const int32 bits = BitLength(value);
    int32 biased = exponent2 + bits - 1 + Bias(format);
    if (biased >= maximumBiased) {
        return static_cast<uint64>(maximumBiased) << mantissaBits;
    }

    if (biased <= 0) {
        // 비정규 수: 지수를 1 - bias로 고정한다. 반올림이 2^mantissaBits에 닿으면 그 비트 패턴이 곧
        // 가장 작은 정규 수다.
        return ShiftRightRounded(value, bits - mantissaBits - biased);
    }

    uint64 significand = ShiftRightRounded(value, bits - 1 - mantissaBits);
    if (significand == (uint64{1} << (mantissaBits + 1))) {
        significand >>= 1;
        ++biased;
        if (biased >= maximumBiased) {
            return static_cast<uint64>(maximumBiased) << mantissaBits;
        }
    }

    return (static_cast<uint64>(biased) << mantissaBits) |
           (significand & ((uint64{1} << mantissaBits) - 1u));
}

// ---- 고정 용량 큰 정수 ----

[[noreturn]] void CapacityExceeded() noexcept {
    platform::Fatal("decimal conversion exceeded its big integer capacity");
}

// 32비트 limb(하위부터)의 부호 없는 큰 정수. ToBinary 경로의 최대 크기는 약 2700비트(769자리 10진
// 수 × 5^1093 비교)이고 CompareWithHalfway를 임의의 bits로 부르면 최악 약 4650비트라 5120비트를
// 둔다. 넘으면 구현 불변식 위반이다.
class BigUnsigned {
public:
    static constexpr usize Capacity = 160;

    void Set(uint64 value) noexcept {
        count_ = 0;
        while (value != 0) {
            limbs_[count_++] = static_cast<uint32>(value);
            value >>= 32;
        }
    }

    bool IsZero() const noexcept {
        return count_ == 0;
    }

    void MultiplyAdd(uint32 factor, uint32 addend) noexcept {
        uint64 carry = addend;
        for (usize index = 0; index < count_; ++index) {
            const uint64 product = static_cast<uint64>(limbs_[index]) * factor + carry;
            limbs_[index] = static_cast<uint32>(product);
            carry = product >> 32;
        }

        if (carry != 0) {
            Push(static_cast<uint32>(carry));
        }

        Trim();
    }

    void MultiplyPow5(uint32 exponent) noexcept {
        constexpr uint32 Pow5To13 = 1220703125u; // 5^13 < 2^32
        while (exponent >= 13) {
            MultiplyAdd(Pow5To13, 0);
            exponent -= 13;
        }

        uint32 factor = 1;
        while (exponent-- > 0) {
            factor *= 5;
        }

        MultiplyAdd(factor, 0);
    }

    void ShiftLeft(uint32 bits) noexcept {
        if (count_ == 0) {
            return;
        }

        const usize words = bits / 32;
        const uint32 rest = bits % 32;
        if (count_ + words + 1 > Capacity) {
            CapacityExceeded();
        }

        if (rest == 0) {
            for (usize index = count_; index-- > 0;) {
                limbs_[index + words] = limbs_[index];
            }

            limbs_[count_ + words] = 0;
        } else {
            limbs_[count_ + words] = limbs_[count_ - 1] >> (32 - rest);
            for (usize index = count_ - 1; index > 0; --index) {
                limbs_[index + words] =
                    (limbs_[index] << rest) | (limbs_[index - 1] >> (32 - rest));
            }

            limbs_[words] = limbs_[0] << rest;
        }

        for (usize index = 0; index < words; ++index) {
            limbs_[index] = 0;
        }

        count_ += words + 1;
        Trim();
    }

    // this /= divisor, 나머지를 돌려준다.
    uint32 DivideSmall(uint32 divisor) noexcept {
        uint64 remainder = 0;
        for (usize index = count_; index-- > 0;) {
            const uint64 current = (remainder << 32) | limbs_[index];
            limbs_[index] = static_cast<uint32>(current / divisor);
            remainder = current % divisor;
        }

        Trim();
        return static_cast<uint32>(remainder);
    }

    static int32 Compare(const BigUnsigned& left, const BigUnsigned& right) noexcept {
        if (left.count_ != right.count_) {
            return left.count_ < right.count_ ? -1 : 1;
        }

        for (usize index = left.count_; index-- > 0;) {
            if (left.limbs_[index] != right.limbs_[index]) {
                return left.limbs_[index] < right.limbs_[index] ? -1 : 1;
            }
        }

        return 0;
    }

private:
    void Push(uint32 limb) noexcept {
        if (count_ == Capacity) {
            CapacityExceeded();
        }

        limbs_[count_++] = limb;
    }

    void Trim() noexcept {
        while (count_ != 0 && limbs_[count_ - 1] == 0) {
            --count_;
        }
    }

    uint32 limbs_[Capacity];
    usize count_ = 0;
};

// 큰 정수의 10진 표기를 out에 쓰고 자릿수를 돌려준다(값 0이면 0자리). value는 0이 된다.
usize WriteDecimal(BigUnsigned& value, char* out) noexcept {
    // 5120비트 < 10^1542 → 9자리 묶음 172개 이하.
    uint32 chunks[180];
    usize chunkCount = 0;
    while (!value.IsZero()) {
        if (chunkCount == sizeof(chunks) / sizeof(chunks[0])) {
            CapacityExceeded();
        }

        chunks[chunkCount++] = value.DivideSmall(1000000000u);
    }

    usize length = 0;
    for (usize index = chunkCount; index-- > 0;) {
        char group[9];
        uint32 chunk = chunks[index];
        for (int32 position = 8; position >= 0; --position) {
            group[position] = static_cast<char>('0' + chunk % 10);
            chunk /= 10;
        }

        // 가장 높은 묶음은 앞의 0을 쓰지 않는다.
        int32 start = 0;
        if (index + 1 == chunkCount) {
            while (start < 8 && group[start] == '0') {
                ++start;
            }
        }

        for (int32 position = start; position < 9; ++position) {
            out[length++] = group[position];
        }
    }

    return length;
}

// ---- Ryu 보조 ----

// 0 ≤ e ≤ 3528에서 5^e의 비트 길이(e = 0이면 1).
int32 Pow5Bits(int32 e) noexcept {
    return static_cast<int32>(((static_cast<uint32>(e) * 1217359u) >> 19) + 1u);
}

// 0 ≤ e ≤ 1650에서 ⌊log10(2^e)⌋.
uint32 Log10Pow2(int32 e) noexcept {
    return (static_cast<uint32>(e) * 78913u) >> 18;
}

// 0 ≤ e ≤ 2620에서 ⌊log10(5^e)⌋.
uint32 Log10Pow5(int32 e) noexcept {
    return (static_cast<uint32>(e) * 732923u) >> 20;
}

bool MultipleOfPowerOf5(uint64 value, uint32 power) noexcept {
    uint32 count = 0;
    while (value != 0 && value % 5 == 0 && count < power) {
        value /= 5;
        ++count;
    }

    return count >= power;
}

bool MultipleOfPowerOf2(uint64 value, uint32 power) noexcept {
    return power < 64 && (value & ((uint64{1} << power) - 1u)) == 0;
}

// ⌊m × factor / 2^shift⌋(factor 128비트, 64 ≤ shift < 128). 결과가 64비트에 든다(Ryu 불변식).
uint64 MultiplyShift(uint64 m, const uint64* factor, int32 shift) noexcept {
    const UInt128 low = Multiply64(m, factor[0]);
    const UInt128 high = Multiply64(m, factor[1]);
    const uint64 sumLow = low.high + high.low;
    const uint64 sumHigh = high.high + (sumLow < low.high ? 1u : 0u);
    const int32 rest = shift - 64;
    return rest == 0 ? sumLow : (sumHigh << (64 - rest)) | (sumLow >> rest);
}

} // namespace

uint64 ToBinary(const DecimalNumber& number, BinaryFormat format) noexcept {
    if (number.count == 0) {
        return 0;
    }

    // 10^309 이상은 어떤 형식에서도 무한대, 10^-324 미만은 0이다(double 최소 비정규 수의 절반
    // ≈ 2.47e-324). 이 범위가 표의 q ∈ [-342, 308]을 보장한다.
    const uint64 infinity = static_cast<uint64>((1 << format.exponentBits) - 1)
                            << format.mantissaBits;
    if (number.pointPosition > 309) {
        return infinity;
    }

    if (number.pointPosition < -323) {
        return 0;
    }

    // 앞 19자리 w(< 10^19 < 2^64)와 q: 값 ∈ [w, w + 1)·10^q(잘렸으면) 또는 = w·10^q.
    const usize kept = number.count < 19 ? number.count : 19;
    uint64 significand = 0;
    for (usize index = 0; index < kept; ++index) {
        significand = significand * 10 + number.digits[index];
    }

    const bool inexactDigits = number.count > kept || number.truncated;
    const int64 q = number.pointPosition - static_cast<int64>(kept);
    const usize tableIndex = static_cast<usize>(q - SmallestPowerOfTen);
    const uint64* power = Pow5Significand[tableIndex];
    const int32 exponent2 = Pow5Exponent[tableIndex] + static_cast<int32>(q);

    // 표 값 X는 5^q·2^s의 내림이다(q ∈ [0, 55]면 정확). 참값 ∈ [w·X, wUpper·(X + 1)).
    const bool exactPower = q >= 0 && q <= 55;
    const uint64 upperSignificand = inexactDigits ? significand + 1 : significand;
    const UInt192 lower = Multiply(significand, power);
    UInt192 upper = Multiply(upperSignificand, power);
    if (!exactPower) {
        Add(upper, upperSignificand);
    }

    const uint64 low = RoundToBinary(lower, exponent2, format);
    const uint64 high = RoundToBinary(upper, exponent2, format);
    if (low == high) {
        return low;
    }

    // 두 경계 사이 폭은 1 ulp보다 훨씬 작으므로 답은 low 또는 low + 1이다. 중간점과 정확히
    // 비교한다.
    const int32 comparison = CompareWithHalfway(number, low, format);
    if (comparison < 0) {
        return low;
    }

    if (comparison > 0) {
        return low + 1;
    }

    return (low & 1u) == 0 ? low : low + 1;
}

int32 CompareWithHalfway(const DecimalNumber& number, uint64 bits, BinaryFormat format) noexcept {
    // bits = M·2^E, 중간점 = (2M + 1)·2^(E - 1).
    const int32 mantissaBits = format.mantissaBits;
    const uint64 fraction = bits & ((uint64{1} << mantissaBits) - 1u);
    const int32 biased = static_cast<int32>(bits >> mantissaBits);
    const uint64 m = biased == 0 ? fraction : fraction | (uint64{1} << mantissaBits);
    const int32 e = (biased == 0 ? 1 : biased) - Bias(format) - mantissaBits;

    // number = D·10^q = D·5^q·2^q.
    BigUnsigned left;
    left.Set(0);
    for (usize index = 0; index < number.count; ++index) {
        left.MultiplyAdd(10, number.digits[index]);
    }

    const int64 q = number.pointPosition - static_cast<int64>(number.count);
    BigUnsigned right;
    right.Set(2 * m + 1);
    if (q >= 0) {
        left.MultiplyPow5(static_cast<uint32>(q));
    } else {
        right.MultiplyPow5(static_cast<uint32>(-q));
    }

    const int64 leftPower2 = q;
    const int64 rightPower2 = static_cast<int64>(e) - 1;
    if (leftPower2 > rightPower2) {
        left.ShiftLeft(static_cast<uint32>(leftPower2 - rightPower2));
    } else {
        right.ShiftLeft(static_cast<uint32>(rightPower2 - leftPower2));
    }

    const int32 comparison = BigUnsigned::Compare(left, right);
    return comparison == 0 && number.truncated ? 1 : comparison;
}

ShortestDecimal Shortest(uint64 bits, BinaryFormat format) noexcept {
    if (bits == 0) {
        platform::Fatal("decimal::Shortest requires a nonzero value");
    }

    // Ryu d2d를 형식 매개변수로 일반화했다. 같은 e2에서 계산이 double과 같으므로(가수 범위만 더
    // 작다) double용 125비트 표가 float에도 충분하다. 끝 0 판정은 범위를 자르지 않고 정확히
    // 계산한다.
    const int32 mantissaBits = format.mantissaBits;
    const uint64 ieeeMantissa = bits & ((uint64{1} << mantissaBits) - 1u);
    const uint32 ieeeExponent = static_cast<uint32>(bits >> mantissaBits);
    int32 e2 = 0;
    uint64 m2 = 0;
    if (ieeeExponent == 0) {
        e2 = 1 - Bias(format) - mantissaBits - 2;
        m2 = ieeeMantissa;
    } else {
        e2 = static_cast<int32>(ieeeExponent) - Bias(format) - mantissaBits - 2;
        m2 = (uint64{1} << mantissaBits) | ieeeMantissa;
    }

    const bool acceptBounds = (m2 & 1u) == 0;
    const uint64 mv = 4 * m2;
    const uint64 mmShift = ieeeMantissa != 0 || ieeeExponent <= 1 ? 1u : 0u;
    const uint64 mp = mv + 2;
    const uint64 mm = mv - 1 - mmShift;

    uint64 vr = 0;
    uint64 vp = 0;
    uint64 vm = 0;
    int32 e10 = 0;
    bool vmIsTrailingZeros = false;
    bool vrIsTrailingZeros = false;
    if (e2 >= 0) {
        const uint32 q = Log10Pow2(e2) - (e2 > 3 ? 1u : 0u);
        e10 = static_cast<int32>(q);
        const int32 k = RyuBitCount + Pow5Bits(static_cast<int32>(q)) - 1;
        const int32 i = -e2 + static_cast<int32>(q) + k;
        vr = MultiplyShift(mv, Pow5InvSplit[q], i);
        vp = MultiplyShift(mp, Pow5InvSplit[q], i);
        vm = MultiplyShift(mm, Pow5InvSplit[q], i);
        // mv·2^e2/10^q의 버린 부분이 0인지: e2 ≥ q이므로 5^q로 나누어떨어지는지와 같다. mp·mv·mm 중
        // 하나만 5의 배수일 수 있다.
        if (mv % 5 == 0) {
            vrIsTrailingZeros = MultipleOfPowerOf5(mv, q);
        } else if (acceptBounds) {
            vmIsTrailingZeros = MultipleOfPowerOf5(mm, q);
        } else {
            vp -= MultipleOfPowerOf5(mp, q) ? 1u : 0u;
        }
    } else {
        const uint32 q = Log10Pow5(-e2) - (-e2 > 1 ? 1u : 0u);
        e10 = static_cast<int32>(q) + e2;
        const int32 i = -e2 - static_cast<int32>(q);
        const int32 k = Pow5Bits(i) - RyuBitCount;
        const int32 j = static_cast<int32>(q) - k;
        vr = MultiplyShift(mv, Pow5Split[i], j);
        vp = MultiplyShift(mp, Pow5Split[i], j);
        vm = MultiplyShift(mm, Pow5Split[i], j);
        // mv·5^i/2^q의 버린 부분이 0인지: 2^q로 나누어떨어지는지와 같다. mv = 4·m2는 끝 0이 둘
        // 이상, mm은 mmShift가 1일 때만 하나, mp는 정확히 하나다.
        if (q <= 1) {
            vrIsTrailingZeros = true;
            if (acceptBounds) {
                vmIsTrailingZeros = mmShift == 1;
            } else {
                --vp;
            }
        } else {
            vrIsTrailingZeros = MultipleOfPowerOf2(mv, q);
        }
    }

    // 구간 (vm, vp) 안에서 가장 짧은 표기를 찾는다.
    int32 removed = 0;
    uint32 lastRemovedDigit = 0;
    uint64 output = 0;
    if (vmIsTrailingZeros || vrIsTrailingZeros) {
        while (vp / 10 > vm / 10) {
            vmIsTrailingZeros &= vm % 10 == 0;
            vrIsTrailingZeros &= lastRemovedDigit == 0;
            lastRemovedDigit = static_cast<uint32>(vr % 10);
            vr /= 10;
            vp /= 10;
            vm /= 10;
            ++removed;
        }

        if (vmIsTrailingZeros) {
            while (vm % 10 == 0) {
                vrIsTrailingZeros &= lastRemovedDigit == 0;
                lastRemovedDigit = static_cast<uint32>(vr % 10);
                vr /= 10;
                vp /= 10;
                vm /= 10;
                ++removed;
            }
        }

        if (vrIsTrailingZeros && lastRemovedDigit == 5 && vr % 2 == 0) {
            // 정확히 …50…0이면 짝수로 내린다.
            lastRemovedDigit = 4;
        }

        output =
            vr + (((vr == vm && (!acceptBounds || !vmIsTrailingZeros)) || lastRemovedDigit >= 5)
                      ? 1u
                      : 0u);
    } else {
        while (vp / 10 > vm / 10) {
            lastRemovedDigit = static_cast<uint32>(vr % 10);
            vr /= 10;
            vp /= 10;
            vm /= 10;
            ++removed;
        }

        output = vr + ((vr == vm || lastRemovedDigit >= 5) ? 1u : 0u);
    }

    return {output, e10 + removed};
}

uint64 FloatBitsToDoubleBits(uint32 bits) noexcept {
    const uint64 sign = static_cast<uint64>(bits >> 31) << 63;
    const uint32 exponent = (bits >> 23) & 0xFF;
    uint64 fraction = bits & 0x7FFFFF;
    if (exponent == 0xFF) {
        // 무한대·NaN(NaN은 quiet 비트를 켠다).
        return sign | 0x7FF0000000000000ull |
               (fraction == 0 ? 0 : (0x0008000000000000ull | (fraction << 29)));
    }

    if (exponent == 0) {
        if (fraction == 0) {
            return sign;
        }

        // 비정규 float는 정규 double이다: 가장 높은 1을 숨은 비트 자리로 옮긴다.
        int32 shift = 0;
        while ((fraction & 0x800000) == 0) {
            fraction <<= 1;
            ++shift;
        }

        fraction &= 0x7FFFFF;
        return sign | (static_cast<uint64>(1023 - 126 - shift) << 52) | (fraction << 29);
    }

    return sign | (static_cast<uint64>(exponent - 127 + 1023) << 52) | (fraction << 29);
}

void ExpandExact(uint64 doubleBits, ExactDecimal& out) noexcept {
    out.count = 0;
    out.pointPosition = 0;
    const uint64 bits = doubleBits & ~(uint64{1} << 63);
    if (bits == 0) {
        return;
    }

    // value = m·2^e, m은 홀수로 줄인다.
    const uint32 biased = static_cast<uint32>(bits >> 52);
    uint64 m = bits & ((uint64{1} << 52) - 1u);
    int32 e = -1074;
    if (biased != 0) {
        m |= uint64{1} << 52;
        e = static_cast<int32>(biased) - 1075;
    }

    const uint32 zeros = PlatformMath::CountTrailingZeros64(m);
    m >>= zeros;
    e += static_cast<int32>(zeros);

    BigUnsigned number;
    if (e >= 0) {
        number.Set(m);
        number.ShiftLeft(static_cast<uint32>(e));
        out.count = WriteDecimal(number, out.digits);
        out.pointPosition = static_cast<int32>(out.count);
    } else {
        // 정수부 m >> k와 소수부 (m mod 2^k)/2^k = (m mod 2^k)·5^k/10^k(정확히 k자리).
        const uint32 k = static_cast<uint32>(-e);
        const uint64 integer = k < 64 ? m >> k : 0;
        const uint64 fraction = k < 64 ? m & ((uint64{1} << k) - 1u) : m;
        number.Set(integer);
        const usize integerDigits = WriteDecimal(number, out.digits);

        char fractionDigits[1100];
        BigUnsigned scaled;
        scaled.Set(fraction);
        scaled.MultiplyPow5(k);
        const usize fractionLength = WriteDecimal(scaled, fractionDigits);
        if (integerDigits != 0) {
            // 소수부는 앞을 0으로 채워 정확히 k자리다.
            usize length = integerDigits;
            for (usize zero = fractionLength; zero < k; ++zero) {
                out.digits[length++] = '0';
            }

            platform::MemoryCopy(out.digits + length, fractionDigits, fractionLength);
            out.count = length + fractionLength;
            out.pointPosition = static_cast<int32>(integerDigits);
        } else {
            platform::MemoryCopy(out.digits, fractionDigits, fractionLength);
            out.count = fractionLength;
            out.pointPosition = -static_cast<int32>(k - fractionLength);
        }
    }

    while (out.count != 0 && out.digits[out.count - 1] == '0') {
        --out.count;
    }
}

void RoundExact(ExactDecimal& out, int64 keep) noexcept {
    if (keep >= static_cast<int64>(out.count)) {
        return;
    }

    if (keep < 0) {
        out.count = 0;
        return;
    }

    const usize position = static_cast<usize>(keep);
    const char next = out.digits[position];
    bool up = next > '5';
    if (next == '5') {
        // 끝 0이 없으므로 뒤에 숫자가 더 있으면 0이 아니다. 정확히 절반이면 앞 숫자가 홀수일 때만
        // 올린다(앞 숫자가 없으면 0으로 본다).
        up = position + 1 < out.count ||
             (position > 0 && ((out.digits[position - 1] - '0') & 1) != 0);
    }

    out.count = position;
    if (up) {
        usize index = position;
        while (index > 0 && out.digits[index - 1] == '9') {
            --index;
        }

        if (index == 0) {
            out.digits[0] = '1';
            out.count = 1;
            ++out.pointPosition;
        } else {
            ++out.digits[index - 1];
            out.count = index;
        }
    }

    while (out.count != 0 && out.digits[out.count - 1] == '0') {
        --out.count;
    }
}

} // namespace rocks::detail::decimal
