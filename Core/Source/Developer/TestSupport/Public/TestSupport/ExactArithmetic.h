#pragma once

#include <Base/Types.h>
#include <Platform/Memory.h>

// 정확 산술 시험 도우미다. 모든 유한 double은 dyadic 유리수(정수 · 2^지수)이므로, 공통 2의
// 거듭제곱을 곱해 정수로 바꾸면 기하 조건(예: 경계 간격² · |축|² >= r² · (축 성분²의 합))을 반올림
// 없이 큰 정수로 판정할 수 있다. 표본 점 시험으로는 잡을 수 없는 포함 위반을 계산으로 확인할 때
// 쓴다.
//
// 사용 예:
//   const int scale = test::DyadicScale(values);              // 모든 값을 정수로 만드는 지수
//   const test::BigSigned gap = test::BigSignedSubtract(test::BigFromDouble(a, scale),
//                                                       test::BigFromDouble(b, scale));
//   if (test::BigCompare(test::BigSquare(gap), required) >= 0) { ... }
//
// 용량을 넘으면 결과의 overflow 표시가 켜지고 이후 연산으로 전파된다. 시험은 이를 실패로 센다.
namespace rocks::test {

// 큰 정수 하나의 32비트 limb 수 상한이다(10,240비트). 가장 작은 비정규 double(2^-1074)과 1e30
// 규모 값의 네 제곱 곱까지 담는다.
inline constexpr int BigLimbCapacity = 320;
inline constexpr int BigLimbBits = 32;

// 부호 없는 큰 정수다(32비트 limb, 낮은 자리부터). count는 쓰는 limb 수이며 맨 위 limb는 0이
// 아니다.
struct BigUnsigned {
    uint32 limbs[BigLimbCapacity] = {};
    int count = 0;
    bool overflow = false;
};

inline void BigTrim(BigUnsigned& value) noexcept {
    while (value.count > 0 && value.limbs[value.count - 1] == 0) {
        --value.count;
    }
}

// mantissa · 2^shift다.
inline BigUnsigned BigFromMantissa(uint64 mantissa, int shift) noexcept {
    BigUnsigned result;
    const int limbShift = shift / BigLimbBits;
    const int bitShift = shift % BigLimbBits;
    if (shift < 0 || limbShift + 3 > BigLimbCapacity) {
        result.overflow = true;
        return result;
    }
    const uint32 parts[2] = {static_cast<uint32>(mantissa),
                             static_cast<uint32>(mantissa >> BigLimbBits)};
    for (int index = 0; index < 2; ++index) {
        const uint64 moved = static_cast<uint64>(parts[index]) << bitShift;
        result.limbs[limbShift + index] |= static_cast<uint32>(moved);
        result.limbs[limbShift + index + 1] |= static_cast<uint32>(moved >> BigLimbBits);
    }
    result.count = limbShift + 3;
    BigTrim(result);
    return result;
}

// left < right면 음수, 같으면 0, 크면 양수다.
inline int BigCompare(const BigUnsigned& left, const BigUnsigned& right) noexcept {
    if (left.count != right.count) {
        return left.count < right.count ? -1 : 1;
    }
    for (int index = left.count - 1; index >= 0; --index) {
        if (left.limbs[index] != right.limbs[index]) {
            return left.limbs[index] < right.limbs[index] ? -1 : 1;
        }
    }
    return 0;
}

inline BigUnsigned BigAdd(const BigUnsigned& left, const BigUnsigned& right) noexcept {
    BigUnsigned result;
    const int count = (left.count > right.count ? left.count : right.count) + 1;
    if (left.overflow || right.overflow || count > BigLimbCapacity) {
        result.overflow = true;
        return result;
    }
    uint64 carry = 0;
    for (int index = 0; index < count; ++index) {
        const uint64 sum = (index < left.count ? left.limbs[index] : 0u) +
                           static_cast<uint64>(index < right.count ? right.limbs[index] : 0u) +
                           carry;
        result.limbs[index] = static_cast<uint32>(sum);
        carry = sum >> BigLimbBits;
    }
    result.count = count;
    BigTrim(result);
    return result;
}

// left - right다. left >= right를 전제한다.
inline BigUnsigned BigSubtract(const BigUnsigned& left, const BigUnsigned& right) noexcept {
    BigUnsigned result;
    if (left.overflow || right.overflow) {
        result.overflow = true;
        return result;
    }
    int64 borrow = 0;
    for (int index = 0; index < left.count; ++index) {
        int64 difference = static_cast<int64>(left.limbs[index]) -
                           (index < right.count ? static_cast<int64>(right.limbs[index]) : 0) -
                           borrow;
        borrow = 0;
        if (difference < 0) {
            difference += int64{1} << BigLimbBits;
            borrow = 1;
        }
        result.limbs[index] = static_cast<uint32>(difference);
    }
    result.count = left.count;
    BigTrim(result);
    return result;
}

inline BigUnsigned BigMultiply(const BigUnsigned& left, const BigUnsigned& right) noexcept {
    BigUnsigned result;
    if (left.overflow || right.overflow || left.count + right.count > BigLimbCapacity) {
        result.overflow = true;
        return result;
    }
    for (int i = 0; i < left.count; ++i) {
        uint64 carry = 0;
        for (int j = 0; j < right.count; ++j) {
            const uint64 current =
                result.limbs[i + j] + static_cast<uint64>(left.limbs[i]) * right.limbs[j] + carry;
            result.limbs[i + j] = static_cast<uint32>(current);
            carry = current >> BigLimbBits;
        }
        result.limbs[i + right.count] = static_cast<uint32>(carry);
    }
    result.count = left.count + right.count;
    BigTrim(result);
    return result;
}

// 부호 있는 큰 정수다. 0은 negative = false다.
struct BigSigned {
    bool negative = false;
    BigUnsigned magnitude;
};

inline BigSigned BigSignedAdd(const BigSigned& left, const BigSigned& right) noexcept {
    BigSigned result;
    if (left.negative == right.negative) {
        result.negative = left.negative;
        result.magnitude = BigAdd(left.magnitude, right.magnitude);
        return result;
    }
    if (BigCompare(left.magnitude, right.magnitude) >= 0) {
        result.negative = left.negative;
        result.magnitude = BigSubtract(left.magnitude, right.magnitude);
    } else {
        result.negative = right.negative;
        result.magnitude = BigSubtract(right.magnitude, left.magnitude);
    }
    if (result.magnitude.count == 0) {
        result.negative = false;
    }
    return result;
}

inline BigSigned BigSignedSubtract(const BigSigned& left, const BigSigned& right) noexcept {
    BigSigned negated = right;
    negated.negative = !right.negative && right.magnitude.count != 0;
    return BigSignedAdd(left, negated);
}

inline BigUnsigned BigSquare(const BigSigned& value) noexcept {
    return BigMultiply(value.magnitude, value.magnitude);
}

// double = ±mantissa · 2^exponent(mantissa 홀수, 0이면 mantissa 0)다.
inline constexpr int DoubleFractionBits = 52;
inline constexpr int DoubleExponentBias = 1075;
inline constexpr int DoubleLowestExponent = -1074;
inline constexpr int DoubleSignBit = 63;
inline constexpr uint64 DoubleExponentFieldMask = (uint64{1} << 11) - 1;

struct DyadicParts {
    bool negative;
    uint64 mantissa;
    int exponent;
};

inline DyadicParts DecomposeDouble(double value) noexcept {
    const uint64 raw = platform::BitCast<uint64>(value);
    const bool negative = (raw >> DoubleSignBit) != 0;
    const uint64 field = (raw >> DoubleFractionBits) & DoubleExponentFieldMask;
    uint64 mantissa = raw & ((uint64{1} << DoubleFractionBits) - 1);
    int exponent = DoubleLowestExponent;
    if (field != 0) {
        mantissa |= uint64{1} << DoubleFractionBits;
        exponent = static_cast<int>(field) - DoubleExponentBias;
    }
    if (mantissa == 0) {
        return DyadicParts{false, 0, 0};
    }
    while ((mantissa & 1) == 0) {
        mantissa >>= 1;
        ++exponent;
    }
    return DyadicParts{negative, mantissa, exponent};
}

// 모든 값에 2^scale을 곱하면 정수가 되는 가장 작은 0 이상의 scale이다(유한값만).
template <int Count>
int DyadicScale(const double (&values)[Count]) noexcept {
    int scale = 0;
    for (const double value : values) {
        const DyadicParts parts = DecomposeDouble(value);
        if (parts.mantissa != 0 && -parts.exponent > scale) {
            scale = -parts.exponent;
        }
    }
    return scale;
}

// value · 2^scale(정수여야 한다)이다. 정수가 아니면 overflow 표시로 실패를 알린다.
inline BigSigned BigFromDouble(double value, int scale) noexcept {
    const DyadicParts parts = DecomposeDouble(value);
    BigSigned result;
    if (parts.mantissa == 0) {
        return result;
    }
    result.negative = parts.negative;
    result.magnitude = BigFromMantissa(parts.mantissa, parts.exponent + scale);
    return result;
}

} // namespace rocks::test
