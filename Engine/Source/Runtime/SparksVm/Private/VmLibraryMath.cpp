#include <Core/Math/MathUtility.h>
#include <Core/Math/Matrix4.h>
#include <Core/Math/Quaternion.h>
#include <Core/Math/Rotator.h>
#include <Core/Math/Transform.h>
#include <Core/Math/Vector3.h>

#include "VmArithmetic.h"
#include "VmLibrary.h"

// 수학 언어 함수다(Docs/Design/SparksVm.ko.md 6a절, SL 부록 B): Math(숫자 공용·정수·실수), 벡터,
// quat, Rotator, Transform, mat4. 초월 함수는 Core Math 함수를 부른다(VM·AOT 같은 함수 —
// SL 12.5절). 실수 결과가 NaN·Inf면 NonFinite다.

namespace rocks::sparks::detail {

namespace {

// `Normalize()`가 0 벡터를 돌려주는 길이 제곱의 상한이다(UE GetSafeNormal의 SMALL_NUMBER).
constexpr double VmSafeNormalTolerance = 1.0e-8;

// 정수 → int 변환 경계(2^63)다.
constexpr double VmTwoPower63 = 9223372036854775808.0;

// Transform slot 배치(Location 3·Rotation 4·Scale 3 — SparksBytecode 2절)다.
constexpr uint32 VmTransformRotation = 3;
constexpr uint32 VmTransformScale = 7;
constexpr uint32 VmMat4Slots = 16;

using F = VmLibraryContext;

bool Finite(double value) noexcept {
    return math::IsFinite(value);
}

// 결과 실수 하나를 Scalar 창 0번에 둔다(결과 종류가 F32면 float로 반올림).
VmFaultKind PutReal(F& call, double value) noexcept {
    if (call.ResultNode().code == VmTypeCode::F32) {
        const float narrowed = static_cast<float>(value);
        if (!math::IsFinite(narrowed)) {
            return VmFaultKind::NonFinite;
        }

        call.s[0] = VmBits(static_cast<double>(narrowed));
        return VmFaultKind::None;
    }

    if (!Finite(value)) {
        return VmFaultKind::NonFinite;
    }

    call.s[0] = VmBits(value);
    return VmFaultKind::None;
}

// 결과 실수 묶음(count개)을 Scalar 창 0번부터 둔다.
VmFaultKind PutReals(F& call, const double* values, uint32 count) noexcept {
    for (uint32 index = 0; index < count; ++index) {
        if (!Finite(values[index])) {
            return VmFaultKind::NonFinite;
        }
    }

    for (uint32 index = 0; index < count; ++index) {
        call.s[index] = VmBits(values[index]);
    }

    return VmFaultKind::None;
}

double Component(const F& call, uint32 operand, uint32 slot) noexcept {
    return VmDouble(call.Scalars(operand)[slot]);
}

Vector3<double> Vector(const F& call, uint32 operand, uint32 first = 0) noexcept {
    return Vector3<double>(Component(call, operand, first), Component(call, operand, first + 1),
                           Component(call, operand, first + 2));
}

Quaternion<double> Quat(const F& call, uint32 operand, uint32 first = 0) noexcept {
    return Quaternion<double>(Component(call, operand, first), Component(call, operand, first + 1),
                              Component(call, operand, first + 2),
                              Component(call, operand, first + 3));
}

Rotator<double> Rotation(const F& call, uint32 operand) noexcept {
    return Rotator<double>(Component(call, operand, 0), Component(call, operand, 1),
                           Component(call, operand, 2));
}

Transform3<double> Trs(const F& call, uint32 operand) noexcept {
    return Transform3<double>(Quat(call, operand, VmTransformRotation), Vector(call, operand, 0),
                              Vector(call, operand, VmTransformScale));
}

VmFaultKind PutVector(F& call, const Vector3<double>& value) noexcept {
    const double values[] = {value.x, value.y, value.z};
    return PutReals(call, values, 3);
}

VmFaultKind PutQuat(F& call, const Quaternion<double>& value) noexcept {
    const double values[] = {value.x, value.y, value.z, value.w};
    return PutReals(call, values, 4);
}

// 정수 값(부호 있는 종류는 확장값)을 그 종류로 결과에 둔다.
void PutInteger(F& call, uint64 bits) noexcept {
    call.s[0] = bits;
}

// ---- Math(숫자 공용) ----

VmFaultKind MathMinMax(F& call, bool minimum) noexcept {
    const VmTypeCode code = call.Operand(0).code;
    const uint64 a = call.Scalars(0)[0];
    const uint64 b = call.Scalars(1)[0];
    bool pickA = false;
    if (VmIsSigned(code)) {
        pickA = minimum ? int64(a) <= int64(b) : int64(a) >= int64(b);
    } else if (VmIsUnsigned(code)) {
        pickA = minimum ? a <= b : a >= b;
    } else {
        pickA = minimum ? VmDouble(a) <= VmDouble(b) : VmDouble(a) >= VmDouble(b);
    }

    call.s[0] = pickA ? a : b;
    return VmFaultKind::None;
}

VmFaultKind MathMin(F& call) noexcept {
    return MathMinMax(call, true);
}

VmFaultKind MathMax(F& call) noexcept {
    return MathMinMax(call, false);
}

// 부호 있는 정수 code의 최솟값(확장값)이다.
int64 SignedMinimum(VmTypeCode code) noexcept {
    switch (code) {
    case VmTypeCode::I8:
        return NumericLimits<int8>::Min();
    case VmTypeCode::I16:
        return NumericLimits<int16>::Min();
    case VmTypeCode::I32:
        return NumericLimits<int32>::Min();
    default:
        return NumericLimits<int64>::Min();
    }
}

VmFaultKind MathAbs(F& call) noexcept {
    const VmTypeCode code = call.Operand(0).code;
    if (VmIsSigned(code)) {
        const int64 value = call.Integer(0);
        if (value == SignedMinimum(code)) {
            return VmFaultKind::IntegerOverflow; // 부록 B: 최솟값의 Abs
        }

        PutInteger(call, uint64(value < 0 ? -value : value));
        return VmFaultKind::None;
    }

    return PutReal(call, math::Abs(call.Real(0)));
}

VmFaultKind MathSign(F& call) noexcept {
    if (VmIsSigned(call.Operand(0).code)) {
        const int64 value = call.Integer(0);
        PutInteger(call, uint64(value > 0 ? 1 : value < 0 ? -1 : 0));
        return VmFaultKind::None;
    }

    const double value = call.Real(0);
    return PutReal(call, value > 0.0 ? 1.0 : value < 0.0 ? -1.0 : 0.0);
}

VmFaultKind MathClamp(F& call) noexcept {
    // lo > hi는 AssertFailed(부록 B).
    const VmTypeCode code = call.Operand(0).code;
    const uint64 x = call.Scalars(0)[0];
    const uint64 lo = call.Scalars(1)[0];
    const uint64 hi = call.Scalars(2)[0];
    if (VmIsSigned(code)) {
        if (int64(lo) > int64(hi)) {
            return VmFaultKind::AssertFailed;
        }

        call.s[0] = int64(x) < int64(lo) ? lo : int64(x) > int64(hi) ? hi : x;
    } else if (VmIsUnsigned(code)) {
        if (lo > hi) {
            return VmFaultKind::AssertFailed;
        }

        call.s[0] = x < lo ? lo : x > hi ? hi : x;
    } else {
        if (VmDouble(lo) > VmDouble(hi)) {
            return VmFaultKind::AssertFailed;
        }

        call.s[0] = VmDouble(x) < VmDouble(lo) ? lo : VmDouble(x) > VmDouble(hi) ? hi : x;
    }

    return VmFaultKind::None;
}

VmFaultKind MathDiv(F& call) noexcept {
    // 내림 몫(SL 3.3절) — DIV 명령과 같은 검사(0은 DivideByZero, 최솟값 / -1은 IntegerOverflow).
    const VmTypeCode code = call.Operand(0).code;
    const uint32 width = static_cast<uint32>(code) - static_cast<uint32>(VmTypeCode::I8);
    constexpr uint32 DivideRow = 3; // ADD·SUB·MUL·DIV·MOD 중 DIV
    uint64 out = 0;
    const VmFaultKind fault = VmIntegerOperation(DivideRow * IntegerWidthCount + width,
                                                 call.Scalars(0)[0], call.Scalars(1)[0], out);
    if (fault == VmFaultKind::None) {
        call.s[0] = out;
    }

    return fault;
}

// ---- Math(실수) ----

VmFaultKind MathFloor(F& call) noexcept {
    return PutReal(call, math::Floor(call.Real(0)));
}

VmFaultKind MathCeil(F& call) noexcept {
    return PutReal(call, math::Ceil(call.Real(0)));
}

VmFaultKind MathRound(F& call) noexcept {
    return PutReal(call, math::Round(call.Real(0))); // 0.5는 0에서 먼 쪽(부록 B)
}

VmFaultKind MathTrunc(F& call) noexcept {
    return PutReal(call, math::Trunc(call.Real(0)));
}

// 정수값 실수를 int로 둔다(범위 밖은 IntegerOverflow).
VmFaultKind PutWhole(F& call, double whole) noexcept {
    if (!(whole >= -VmTwoPower63 && whole < VmTwoPower63)) {
        return VmFaultKind::IntegerOverflow;
    }

    PutInteger(call, uint64(static_cast<int64>(whole)));
    return VmFaultKind::None;
}

VmFaultKind MathFloorToInt(F& call) noexcept {
    return PutWhole(call, math::Floor(call.Real(0)));
}

VmFaultKind MathCeilToInt(F& call) noexcept {
    return PutWhole(call, math::Ceil(call.Real(0)));
}

VmFaultKind MathRoundToInt(F& call) noexcept {
    return PutWhole(call, math::Round(call.Real(0)));
}

VmFaultKind MathTruncToInt(F& call) noexcept {
    return PutWhole(call, math::Trunc(call.Real(0)));
}

VmFaultKind MathSqrt(F& call) noexcept {
    return PutReal(call, math::Sqrt(call.Real(0)));
}

VmFaultKind MathPow(F& call) noexcept {
    return PutReal(call, platform::PlatformMath::Pow(call.Real(0), call.Real(1)));
}

VmFaultKind MathExp(F& call) noexcept {
    return PutReal(call, platform::PlatformMath::Exp(call.Real(0)));
}

VmFaultKind MathLn(F& call) noexcept {
    return PutReal(call, platform::PlatformMath::Log(call.Real(0)));
}

VmFaultKind MathSin(F& call) noexcept {
    return PutReal(call, math::Sin(call.Real(0)));
}

VmFaultKind MathCos(F& call) noexcept {
    return PutReal(call, math::Cos(call.Real(0)));
}

VmFaultKind MathTan(F& call) noexcept {
    return PutReal(call, math::Tan(call.Real(0)));
}

VmFaultKind MathAsin(F& call) noexcept {
    return PutReal(call, math::Asin(call.Real(0)));
}

VmFaultKind MathAcos(F& call) noexcept {
    return PutReal(call, math::Acos(call.Real(0)));
}

VmFaultKind MathAtan(F& call) noexcept {
    return PutReal(call, math::Atan(call.Real(0)));
}

VmFaultKind MathAtan2(F& call) noexcept {
    return PutReal(call, math::Atan2(call.Real(0), call.Real(1)));
}

VmFaultKind MathRadians(F& call) noexcept {
    return PutReal(call, math::DegreesToRadians(call.Real(0)));
}

VmFaultKind MathDegrees(F& call) noexcept {
    return PutReal(call, math::RadiansToDegrees(call.Real(0)));
}

VmFaultKind MathLerp(F& call) noexcept {
    return PutReal(call, math::Lerp(call.Real(0), call.Real(1), call.Real(2)));
}

VmFaultKind MathInverseLerp(F& call) noexcept {
    const double a = call.Real(0);
    const double b = call.Real(1);
    if (a == b) {
        return VmFaultKind::DivideByZero; // 부록 B
    }

    return PutReal(call, (call.Real(2) - a) / (b - a));
}

double Clamp01(double t) noexcept {
    return t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
}

// 2차 easing(UE InterpEaseIn·Out·InOut의 지수 2)이다. t는 [0, 1]로 자른다.
VmFaultKind MathEaseIn(F& call) noexcept {
    const double t = Clamp01(call.Real(0));
    return PutReal(call, t * t);
}

VmFaultKind MathEaseOut(F& call) noexcept {
    const double t = Clamp01(call.Real(0));
    return PutReal(call, 1.0 - (1.0 - t) * (1.0 - t));
}

VmFaultKind MathEaseInOut(F& call) noexcept {
    const double t = Clamp01(call.Real(0));
    return PutReal(call, t < 0.5 ? 2.0 * t * t : 1.0 - 2.0 * (1.0 - t) * (1.0 - t));
}

VmFaultKind MathNearlyEqual(F& call) noexcept {
    // 실수 판과 벡터 판(성분마다 |a - b| <= tolerance — UE Equals).
    const uint32 slots = call.Operand(0).slots;
    const double tolerance = VmDouble(call.Scalars(2)[0]);
    bool near = true;
    for (uint32 slot = 0; slot < slots && near; ++slot) {
        near = math::Abs(Component(call, 0, slot) - Component(call, 1, slot)) <= tolerance;
    }

    call.s[0] = near ? 1 : 0;
    return VmFaultKind::None;
}

VmFaultKind MathScale(F& call) noexcept {
    const uint32 slots = call.Operand(0).slots;
    double values[4] = {};
    for (uint32 slot = 0; slot < slots; ++slot) {
        values[slot] = Component(call, 0, slot) * Component(call, 1, slot);
    }

    return PutReals(call, values, slots);
}

// ---- 벡터 ----

double Dot(const F& call, uint32 left, uint32 right, uint32 slots) noexcept {
    double sum = 0.0;
    for (uint32 slot = 0; slot < slots; ++slot) {
        sum += Component(call, left, slot) * Component(call, right, slot);
    }

    return sum;
}

VmFaultKind VectorDot(F& call) noexcept {
    return PutReal(call, Dot(call, 0, 1, call.Operand(0).slots));
}

VmFaultKind VectorLengthSquared(F& call) noexcept {
    return PutReal(call, Dot(call, 0, 0, call.Operand(0).slots));
}

VmFaultKind VectorLength(F& call) noexcept {
    return PutReal(call, math::Sqrt(Dot(call, 0, 0, call.Operand(0).slots)));
}

VmFaultKind VectorDistance(F& call) noexcept {
    const uint32 slots = call.Operand(0).slots;
    double sum = 0.0;
    for (uint32 slot = 0; slot < slots; ++slot) {
        const double delta = Component(call, 1, slot) - Component(call, 0, slot);
        sum += delta * delta;
    }

    return PutReal(call, math::Sqrt(sum));
}

VmFaultKind VectorNormalize(F& call) noexcept {
    // 길이 제곱이 1e-8 미만이면 0 벡터(UE GetSafeNormal — 부록 B).
    const uint32 slots = call.Operand(0).slots;
    const double squared = Dot(call, 0, 0, slots);
    double values[4] = {};
    if (squared >= VmSafeNormalTolerance) {
        const double scale = 1.0 / math::Sqrt(squared);
        for (uint32 slot = 0; slot < slots; ++slot) {
            values[slot] = Component(call, 0, slot) * scale;
        }
    }

    return PutReals(call, values, slots);
}

VmFaultKind VectorCross(F& call) noexcept {
    return PutVector(call, rocks::Cross(Vector(call, 0), Vector(call, 1)));
}

// ---- quat·Rotator·Transform·mat4 ----

VmFaultKind QuatFromAxisAngle(F& call) noexcept {
    // 0 길이 axis는 단위 quat(부록 B).
    auto rotation = Quaternion<double>::TryFromAxisAngle(Vector(call, 0), call.Real(1));
    return PutQuat(call, rotation ? rotation.Value() : Quaternion<double>::Identity());
}

VmFaultKind QuatRotate(F& call) noexcept {
    return PutVector(call, Quat(call, 0).RotateVector(Vector(call, 1)));
}

VmFaultKind QuatInverse(F& call) noexcept {
    auto inverse = Quat(call, 0).TryInverse();
    if (!inverse) {
        return VmFaultKind::NonFinite;
    }

    return PutQuat(call, inverse.Value());
}

VmFaultKind QuatToRotator(F& call) noexcept {
    auto rotator = Rotator<double>::TryFromQuaternion(Quat(call, 0));
    if (!rotator) {
        return VmFaultKind::NonFinite;
    }

    const double values[] = {rotator.Value().pitch, rotator.Value().yaw, rotator.Value().roll};
    return PutReals(call, values, 3);
}

VmFaultKind RotatorToQuat(F& call) noexcept {
    return PutQuat(call, Rotation(call, 0).ToQuaternion());
}

VmFaultKind RotatorForward(F& call) noexcept {
    return PutVector(call, Rotation(call, 0).ToQuaternion().GetForwardVector());
}

VmFaultKind RotatorRight(F& call) noexcept {
    return PutVector(call, Rotation(call, 0).ToQuaternion().GetRightVector());
}

VmFaultKind RotatorUp(F& call) noexcept {
    return PutVector(call, Rotation(call, 0).ToQuaternion().GetUpVector());
}

VmFaultKind TransformPosition(F& call) noexcept {
    return PutVector(call, Trs(call, 0).TransformPoint(Vector(call, 1)));
}

VmFaultKind TransformInversePosition(F& call) noexcept {
    // 0 scale의 역변환은 NonFinite(부록 B).
    auto point = Trs(call, 0).TryInverseTransformPoint(Vector(call, 1));
    if (!point) {
        return VmFaultKind::NonFinite;
    }

    return PutVector(call, point.Value());
}

VmFaultKind TransformVector(F& call) noexcept {
    return PutVector(call, Trs(call, 0).TransformVector(Vector(call, 1)));
}

VmFaultKind TransformToMatrix(F& call) noexcept {
    const Matrix4<double> matrix = Trs(call, 0).ToMatrix();
    return PutReals(call, matrix.m, VmMat4Slots);
}

VmFaultKind MatrixTransformPosition(F& call) noexcept {
    Matrix4<double> matrix;
    for (uint32 slot = 0; slot < VmMat4Slots; ++slot) {
        matrix.m[slot] = Component(call, 0, slot);
    }

    return PutVector(call, matrix.TransformPoint(Vector(call, 1)));
}

constexpr VmLibraryEntry VmMathTable[] = {
    {"Math.Min", "N,N>N", MathMin},
    {"Math.Max", "N,N>N", MathMax},
    {"Math.Abs", "N>N", MathAbs},
    {"Math.Sign", "N>N", MathSign},
    {"Math.Clamp", "N,N,N>N", MathClamp},
    {"Math.Div", "I,I>I", MathDiv},
    {"Math.Floor", "F>F", MathFloor},
    {"Math.Ceil", "F>F", MathCeil},
    {"Math.Round", "F>F", MathRound},
    {"Math.Trunc", "F>F", MathTrunc},
    {"Math.FloorToInt", "F>i64", MathFloorToInt},
    {"Math.CeilToInt", "F>i64", MathCeilToInt},
    {"Math.RoundToInt", "F>i64", MathRoundToInt},
    {"Math.TruncToInt", "F>i64", MathTruncToInt},
    {"Math.Sqrt", "F>F", MathSqrt},
    {"Math.Pow", "F,F>F", MathPow},
    {"Math.Exp", "F>F", MathExp},
    {"Math.Ln", "F>F", MathLn},
    {"Math.Sin", "F>F", MathSin},
    {"Math.Cos", "F>F", MathCos},
    {"Math.Tan", "F>F", MathTan},
    {"Math.Asin", "F>F", MathAsin},
    {"Math.Acos", "F>F", MathAcos},
    {"Math.Atan", "F>F", MathAtan},
    {"Math.Atan2", "F,F>F", MathAtan2},
    {"Math.Radians", "F>F", MathRadians},
    {"Math.Degrees", "F>F", MathDegrees},
    {"Math.Lerp", "F,F,F>F", MathLerp},
    {"Math.InverseLerp", "F,F,F>F", MathInverseLerp},
    {"Math.EaseIn", "F>F", MathEaseIn},
    {"Math.EaseOut", "F>F", MathEaseOut},
    {"Math.EaseInOut", "F>F", MathEaseInOut},
    {"Math.NearlyEqual", "F,F,F>b", MathNearlyEqual},
    {"Math.NearlyEqual", "V,V,F>b", MathNearlyEqual},
    {"Math.Scale", "V,V>V", MathScale},
    {"vec2.Dot", "v2,v2>F", VectorDot},
    {"vec3.Dot", "v3,v3>F", VectorDot},
    {"vec4.Dot", "v4,v4>F", VectorDot},
    {"vec2.Length", "v2>F", VectorLength},
    {"vec3.Length", "v3>F", VectorLength},
    {"vec4.Length", "v4>F", VectorLength},
    {"vec2.LengthSquared", "v2>F", VectorLengthSquared},
    {"vec3.LengthSquared", "v3>F", VectorLengthSquared},
    {"vec4.LengthSquared", "v4>F", VectorLengthSquared},
    {"vec2.Distance", "v2,v2>F", VectorDistance},
    {"vec3.Distance", "v3,v3>F", VectorDistance},
    {"vec4.Distance", "v4,v4>F", VectorDistance},
    {"vec2.Normalize", "v2>v2", VectorNormalize},
    {"vec3.Normalize", "v3>v3", VectorNormalize},
    {"vec4.Normalize", "v4>v4", VectorNormalize},
    {"vec3.Cross", "v3,v3>v3", VectorCross},
    {"quat.FromAxisAngle", "v3,F>q", QuatFromAxisAngle},
    {"quat.Rotate", "q,v3>v3", QuatRotate},
    {"quat.Inverse", "q>q", QuatInverse},
    {"quat.ToRotator", "q>r", QuatToRotator},
    {"Rotator.ToQuat", "r>q", RotatorToQuat},
    {"Rotator.Forward", "r>v3", RotatorForward},
    {"Rotator.Right", "r>v3", RotatorRight},
    {"Rotator.Up", "r>v3", RotatorUp},
    {"Transform.TransformPosition", "t,v3>v3", TransformPosition},
    {"Transform.InverseTransformPosition", "t,v3>v3", TransformInversePosition},
    {"Transform.TransformVector", "t,v3>v3", TransformVector},
    {"Transform.ToMatrix", "t>m", TransformToMatrix},
    {"mat4.TransformPosition", "m,v3>v3", MatrixTransformPosition},
};

} // namespace

Span<const VmLibraryEntry> VmMathEntries() noexcept {
    return Span<const VmLibraryEntry>(VmMathTable, sizeof(VmMathTable) / sizeof(VmMathTable[0]));
}

} // namespace rocks::sparks::detail
