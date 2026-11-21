#pragma once

#include <Base/Checked.h>
#include <Core/Math/MathUtility.h>
#include <Platform/Memory.h>
#include <SparksVm/Opcode.h>
#include <SparksVm/Vm.h>

// VM의 숫자 연산과 검사다(Docs/Design/SparksVm.ko.md 3절, LC5a). Scalar 레지스터는 원시 64비트이고
// 정수는 그 타입 범위의 부호·0 확장, 실수는 double 비트다(SparksBytecode 2절). 결과가 범위 밖이면
// fault 종류를 돌려주고 out은 바꾸지 않는다(같은 레지스터를 읽고 쓰는 명령이 있다).

namespace rocks::sparks::detail {

// 정수 산술 opcode 한 군(ADD…MOD)의 폭 수다(I8·I16·I32·I64·U8·U16·U32·U64 — Opcode.h 순서).
inline constexpr uint32 IntegerWidthCount = 8;

// 실수 산술 opcode 한 타입의 연산 수다(ADD·SUB·MUL·DIV·NEG — Opcode.h 순서).
inline constexpr uint32 FloatOperationCount = 5;

// 실수 slot 비교(EQN.F)다: 비트가 같고 NaN이 아니거나, 둘 다 ±0이다. IEEE 같음과 같지만 FTZ·DAZ
// 환경과 무관하다(있음 표지 1은 비정규 수 비트다).
inline bool VmFloatSlotsEqual(uint64 left, uint64 right) noexcept {
    constexpr uint64 Exponent = 0x7FF0000000000000ull;
    constexpr uint64 Mantissa = 0x000FFFFFFFFFFFFFull;
    if (left == right) {
        return (left & Exponent) != Exponent || (left & Mantissa) == 0;
    }

    return ((left | right) << 1) == 0;
}

// 정수 → 실수 변환의 정확성 검사 경계(2^63·2^64)다.
inline constexpr double TwoPower63 = 9223372036854775808.0;
inline constexpr double TwoPower64 = 18446744073709551616.0;

enum class VmArith : uint32 {
    Add,
    Subtract,
    Multiply,
    Divide,
    Modulo,
    Negate // 실수만(정수 부정은 NEG.I* — 따로)
};

inline double VmDouble(uint64 bits) noexcept {
    return platform::BitCast<double>(bits);
}

inline uint64 VmBits(double value) noexcept {
    return platform::BitCast<uint64>(value);
}

// 정수 값을 레지스터 표현(부호·0 확장)으로 둔다.
template <typename T>
constexpr uint64 VmExtend(T value) noexcept {
    if constexpr (IsSignedV<T>) {
        return static_cast<uint64>(static_cast<int64>(value));
    } else {
        return static_cast<uint64>(value);
    }
}

template <typename T>
VmFaultKind VmIntegerArith(VmArith arith, uint64 leftBits, uint64 rightBits, uint64& out) noexcept {
    const T left = static_cast<T>(leftBits);
    const T right = static_cast<T>(rightBits);
    T value = 0;
    switch (arith) {
    case VmArith::Add:
        if (!TryAdd(left, right, value)) {
            return VmFaultKind::IntegerOverflow;
        }

        break;
    case VmArith::Subtract:
        if (!TrySubtract(left, right, value)) {
            return VmFaultKind::IntegerOverflow;
        }

        break;
    case VmArith::Multiply:
        if (!TryMultiply(left, right, value)) {
            return VmFaultKind::IntegerOverflow;
        }

        break;
    case VmArith::Divide:
    case VmArith::Modulo:
        if (right == 0) {
            return VmFaultKind::DivideByZero;
        }

        // 부호 있는 최솟값 / -1은 범위 밖이다. 나머지는 0으로 범위 안이다(SL 3.3절 — 결과가 범위를
        // 넘을 때만 fault).
        if constexpr (IsSignedV<T>) {
            if (left == NumericLimits<T>::Min() && right == static_cast<T>(-1)) {
                if (arith == VmArith::Divide) {
                    return VmFaultKind::IntegerOverflow;
                }

                value = 0;
                break;
            }
        }

        // 내림 방식이다(SL 3.3절 — `a == Math.Div(a, b) * b + a % b`, 나머지는 오른쪽 부호). C++은
        // 0 쪽 자름이므로 나머지가 0이 아니고 부호가 오른쪽과 다르면 고친다(몫 − 1은 넘치지 않는다
        // — 몫이 최솟값이면 나머지가 0이다).
        {
            T quotient = static_cast<T>(left / right);
            T remainder = static_cast<T>(left % right);
            if constexpr (IsSignedV<T>) {
                if (remainder != 0 && ((remainder < 0) != (right < 0))) {
                    quotient = static_cast<T>(quotient - 1);
                    remainder = static_cast<T>(remainder + right);
                }
            }

            value = arith == VmArith::Divide ? quotient : remainder;
        }

        break;
    case VmArith::Negate:
        return VmFaultKind::Unreachable;
    }

    out = VmExtend(value);
    return VmFaultKind::None;
}

// ADD.I8…MOD.U64(index = opcode − ADD.I8)다.
inline VmFaultKind VmIntegerOperation(uint32 index, uint64 left, uint64 right,
                                      uint64& out) noexcept {
    const VmArith arith = static_cast<VmArith>(index / IntegerWidthCount);
    switch (index % IntegerWidthCount) {
    case 0:
        return VmIntegerArith<int8>(arith, left, right, out);
    case 1:
        return VmIntegerArith<int16>(arith, left, right, out);
    case 2:
        return VmIntegerArith<int32>(arith, left, right, out);
    case 3:
        return VmIntegerArith<int64>(arith, left, right, out);
    case 4:
        return VmIntegerArith<uint8>(arith, left, right, out);
    case 5:
        return VmIntegerArith<uint16>(arith, left, right, out);
    case 6:
        return VmIntegerArith<uint32>(arith, left, right, out);
    default:
        return VmIntegerArith<uint64>(arith, left, right, out);
    }
}

template <typename T>
VmFaultKind VmIntegerNegate(uint64 bits, uint64& out) noexcept {
    const T value = static_cast<T>(bits);
    if (value == NumericLimits<T>::Min()) {
        return VmFaultKind::IntegerOverflow;
    }

    out = VmExtend(static_cast<T>(-value));
    return VmFaultKind::None;
}

// NEG.I8…NEG.I64(index = opcode − NEG.I8)다.
inline VmFaultKind VmIntegerNegation(uint32 index, uint64 bits, uint64& out) noexcept {
    switch (index) {
    case 0:
        return VmIntegerNegate<int8>(bits, out);
    case 1:
        return VmIntegerNegate<int16>(bits, out);
    case 2:
        return VmIntegerNegate<int32>(bits, out);
    default:
        return VmIntegerNegate<int64>(bits, out);
    }
}

template <typename T>
T VmFloatApply(VmArith arith, T left, T right) noexcept {
    switch (arith) {
    case VmArith::Add:
        return left + right;
    case VmArith::Subtract:
        return left - right;
    case VmArith::Multiply:
        return left * right;
    case VmArith::Divide:
        return left / right;
    default:
        return -left;
    }
}

// ADD.F32…NEG.F64(index = opcode − ADD.F32)다. F32는 float로 계산한다(입력은 float 값이다 — 결과는
// double로 계산한 뒤 float로 반올림한 것과 같다). 결과가 유한하지 않으면 NonFinite.
inline VmFaultKind VmFloatOperation(uint32 index, uint64 left, uint64 right, uint64& out) noexcept {
    const bool single = index < FloatOperationCount;
    const VmArith arith = static_cast<VmArith>(index % FloatOperationCount);
    const VmArith mapped =
        arith == static_cast<VmArith>(FloatOperationCount - 1) ? VmArith::Negate : arith;
    if (single) {
        const float value = VmFloatApply(mapped, static_cast<float>(VmDouble(left)),
                                         static_cast<float>(VmDouble(right)));
        if (!math::IsFinite(value)) {
            return VmFaultKind::NonFinite;
        }

        out = VmBits(static_cast<double>(value));
        return VmFaultKind::None;
    }

    const double value = VmFloatApply(mapped, VmDouble(left), VmDouble(right));
    if (!math::IsFinite(value)) {
        return VmFaultKind::NonFinite;
    }

    out = VmBits(value);
    return VmFaultKind::None;
}

inline bool VmIsSignedKind(ScalarKind kind) noexcept {
    return kind >= ScalarKind::I8 && kind <= ScalarKind::I64;
}

inline bool VmIsUnsignedKind(ScalarKind kind) noexcept {
    return kind >= ScalarKind::U8 && kind <= ScalarKind::U64;
}

inline bool VmIsFloatKind(ScalarKind kind) noexcept {
    return kind == ScalarKind::F32 || kind == ScalarKind::F64;
}

template <typename To, typename From>
VmFaultKind VmNarrow(From value, uint64& out) noexcept {
    To narrowed = 0;
    if (!TryNarrow(value, narrowed)) {
        return VmFaultKind::IntegerOverflow;
    }

    out = VmExtend(narrowed);
    return VmFaultKind::None;
}

template <typename From>
VmFaultKind VmConvertInteger(From value, ScalarKind to, uint64& out) noexcept {
    switch (to) {
    case ScalarKind::I8:
        return VmNarrow<int8>(value, out);
    case ScalarKind::I16:
        return VmNarrow<int16>(value, out);
    case ScalarKind::I32:
        return VmNarrow<int32>(value, out);
    case ScalarKind::I64:
        return VmNarrow<int64>(value, out);
    case ScalarKind::U8:
        return VmNarrow<uint8>(value, out);
    case ScalarKind::U16:
        return VmNarrow<uint16>(value, out);
    case ScalarKind::U32:
        return VmNarrow<uint32>(value, out);
    case ScalarKind::U64:
        return VmNarrow<uint64>(value, out);
    case ScalarKind::F64: {
        // 정확히 표현될 때만(2^53을 넘는 홀수 등은 IntegerOverflow — SL 3.3절).
        const double converted = static_cast<double>(value);
        bool exact = false;
        if constexpr (IsSignedV<From>) {
            exact = converted >= -TwoPower63 && converted < TwoPower63 &&
                    static_cast<int64>(converted) == value;
        } else {
            exact = converted < TwoPower64 && static_cast<uint64>(converted) == value;
        }

        if (!exact) {
            return VmFaultKind::IntegerOverflow;
        }

        out = VmBits(converted);
        return VmFaultKind::None;
    }
    case ScalarKind::F32: {
        const float converted = static_cast<float>(value);
        const double widened = static_cast<double>(converted);
        bool exact = false;
        if constexpr (IsSignedV<From>) {
            exact = widened >= -TwoPower63 && widened < TwoPower63 &&
                    static_cast<int64>(widened) == value;
        } else {
            exact = widened < TwoPower64 && static_cast<uint64>(widened) == value;
        }

        if (!exact) {
            return VmFaultKind::IntegerOverflow;
        }

        out = VmBits(widened);
        return VmFaultKind::None;
    }
    default:
        return VmFaultKind::Unreachable;
    }
}

// CONV(C = 원래 종류 << 4 | 목적 종류)다. 정수 사이·정수 → 실수·실수 사이만 정의한다. 실수 → 정수와
// 숫자가 아닌 종류는 컴파일러가 내지 않으므로 Unreachable이다(SparksVm 3절).
inline VmFaultKind VmConvert(uint32 kinds, uint64 bits, uint64& out) noexcept {
    const ScalarKind from = static_cast<ScalarKind>(kinds >> 4);
    const ScalarKind to = static_cast<ScalarKind>(kinds & 0xF);
    if (VmIsSignedKind(from) && (VmIsSignedKind(to) || VmIsUnsignedKind(to) || VmIsFloatKind(to))) {
        return VmConvertInteger(static_cast<int64>(bits), to, out);
    }

    if (VmIsUnsignedKind(from) &&
        (VmIsSignedKind(to) || VmIsUnsignedKind(to) || VmIsFloatKind(to))) {
        return VmConvertInteger(bits, to, out);
    }

    if (from == ScalarKind::F32 && to == ScalarKind::F64) {
        out = bits; // F32 레지스터는 이미 float 값의 double이다
        return VmFaultKind::None;
    }

    if (from == ScalarKind::F64 && to == ScalarKind::F32) {
        // 최근접 짝수로 반올림, float 범위 밖은 NonFinite(SL 3.3절).
        const float narrowed = static_cast<float>(VmDouble(bits));
        if (!math::IsFinite(narrowed)) {
            return VmFaultKind::NonFinite;
        }

        out = VmBits(static_cast<double>(narrowed));
        return VmFaultKind::None;
    }

    return VmFaultKind::Unreachable;
}

} // namespace rocks::sparks::detail
