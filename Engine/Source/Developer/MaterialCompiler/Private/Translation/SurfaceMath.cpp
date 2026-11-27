#include "SurfaceMath.h"

#include "LoweringContext.h"

// 표면 식 도우미다(Docs/Design/MaterialSystem 6.11절 MS2d2).
namespace rocks::material::detail {

namespace {

// 세 성분 평균의 무게다(UE dot(v, 0.33.xxx) — S:3233, 1/3이 아니라 UE 값 그대로).
constexpr float SurfaceMathThird = 0.33f;

} // namespace

IrValueId SurfaceMath::Check(Result<IrValueId> value) noexcept {
    if (failed_) {
        return NoIrValue;
    }

    if (!value) {
        failed_ = true;
        error_ = value.Error();
        return NoIrValue;
    }

    return value.Value();
}

IrValueId SurfaceMath::Unary(IrOp op, IrValueId value) noexcept {
    if (failed_ || value == NoIrValue) {
        return NoIrValue;
    }

    return Check(context_.Unary(op, value, NumberRule::FloatOnly));
}

IrValueId SurfaceMath::Binary(IrOp op, IrValueId left, IrValueId right) noexcept {
    if (failed_ || left == NoIrValue || right == NoIrValue) {
        return NoIrValue;
    }

    return Check(context_.Binary(op, left, right, NumberRule::FloatOnly));
}

IrValueId SurfaceMath::Scalar(float value) noexcept {
    if (failed_) {
        return NoIrValue;
    }

    return Check(context_.FloatScalar(value));
}

IrValueId SurfaceMath::Vector3(float value) noexcept {
    return Vector3(value, value, value);
}

IrValueId SurfaceMath::Vector3(float x, float y, float z) noexcept {
    if (failed_) {
        return NoIrValue;
    }

    const float components[] = {x, y, z};
    return Check(context_.FloatConstant(Span<const float>(components, 3), StringView()));
}

IrValueId SurfaceMath::UInt(uint32 value) noexcept {
    if (failed_) {
        return NoIrValue;
    }

    return Check(context_.Builder().UIntConstant(Span<const uint32>(&value, 1)));
}

IrValueId SurfaceMath::Bool(bool value) noexcept {
    if (failed_) {
        return NoIrValue;
    }

    return Check(context_.Builder().BoolConstant(Span<const bool>(&value, 1)));
}

IrValueId SurfaceMath::Add(IrValueId left, IrValueId right) noexcept {
    return Binary(IrOp::Add, left, right);
}

IrValueId SurfaceMath::Sub(IrValueId left, IrValueId right) noexcept {
    return Binary(IrOp::Subtract, left, right);
}

IrValueId SurfaceMath::Mul(IrValueId left, IrValueId right) noexcept {
    return Binary(IrOp::Multiply, left, right);
}

IrValueId SurfaceMath::Div(IrValueId left, IrValueId right) noexcept {
    return Binary(IrOp::Divide, left, right);
}

IrValueId SurfaceMath::Min(IrValueId left, IrValueId right) noexcept {
    return Binary(IrOp::Min, left, right);
}

IrValueId SurfaceMath::Max(IrValueId left, IrValueId right) noexcept {
    return Binary(IrOp::Max, left, right);
}

IrValueId SurfaceMath::Pow(IrValueId base, IrValueId exponent) noexcept {
    return Binary(IrOp::Pow, base, exponent);
}

IrValueId SurfaceMath::Lerp(IrValueId from, IrValueId to, IrValueId alpha) noexcept {
    if (failed_ || from == NoIrValue || to == NoIrValue || alpha == NoIrValue) {
        return NoIrValue;
    }

    // 무게가 상수 0·1이면 그 끝이다(정확하다 — 표지가 상수인 흔한 경우).
    float weight = 0.0f;
    if (context_.ConstantFloat(alpha, weight) && (weight == 0.0f || weight == 1.0f) &&
        context_.TypeOf(from) == context_.TypeOf(to)) {
        return weight == 0.0f ? from : to;
    }

    // 셋 다 상수면 HLSL 정의 lerp(x, y, s) = x + s·(y − x)로 펼쳐 접는다(상수 표지·기본값 칸이
    // 상수로 남는다).
    const IrBuilder& builder = context_.Builder();
    if (builder.Value(from).op == IrOp::Constant && builder.Value(to).op == IrOp::Constant &&
        builder.Value(alpha).op == IrOp::Constant) {
        return Add(from, Mul(alpha, Sub(to, from)));
    }

    return Check(context_.Ternary(IrOp::Lerp, from, to, alpha, NumberRule::FloatOnly));
}

IrValueId SurfaceMath::NodeLerp(IrValueId from, IrValueId to, IrValueId alpha) noexcept {
    // 두 끝이 같은 값이면 그 끝이다(UE 번역기 Lerp — HLSLMaterialTranslator.cpp:10378: 같은 값
    // 번호, hash-consing으로 같은 상수도 같은 번호다).
    if (!failed_ && from != NoIrValue && from == to && alpha != NoIrValue) {
        return from;
    }

    return Lerp(from, to, alpha);
}

IrValueId SurfaceMath::Clamp(IrValueId value, IrValueId low, IrValueId high) noexcept {
    if (failed_ || value == NoIrValue || low == NoIrValue || high == NoIrValue) {
        return NoIrValue;
    }

    return Check(context_.Ternary(IrOp::Clamp, value, low, high, NumberRule::FloatOnly));
}

IrValueId SurfaceMath::Smoothstep(IrValueId low, IrValueId high, IrValueId value) noexcept {
    if (failed_ || value == NoIrValue || low == NoIrValue || high == NoIrValue) {
        return NoIrValue;
    }

    return Check(context_.Ternary(IrOp::Smoothstep, low, high, value, NumberRule::FloatOnly));
}

IrValueId SurfaceMath::Saturate(IrValueId value) noexcept {
    return Unary(IrOp::Saturate, value);
}

IrValueId SurfaceMath::Sqrt(IrValueId value) noexcept {
    return Unary(IrOp::Sqrt, value);
}

IrValueId SurfaceMath::Exp(IrValueId value) noexcept {
    return Unary(IrOp::Exponential, value);
}

IrValueId SurfaceMath::Log(IrValueId value) noexcept {
    return Unary(IrOp::Logarithm, value);
}

IrValueId SurfaceMath::Abs(IrValueId value) noexcept {
    return Unary(IrOp::Abs, value);
}

IrValueId SurfaceMath::Cos(IrValueId value) noexcept {
    return Unary(IrOp::Cos, value);
}

IrValueId SurfaceMath::Sin(IrValueId value) noexcept {
    return Unary(IrOp::Sin, value);
}

IrValueId SurfaceMath::Cross(IrValueId left, IrValueId right) noexcept {
    return Binary(IrOp::Cross, left, right);
}

IrValueId SurfaceMath::Length(IrValueId value) noexcept {
    return Unary(IrOp::Length, value);
}

IrValueId SurfaceMath::Dot(IrValueId left, IrValueId right) noexcept {
    return Binary(IrOp::Dot, left, right);
}

IrValueId SurfaceMath::Normalize(IrValueId value) noexcept {
    return Unary(IrOp::Normalize, value);
}

IrValueId SurfaceMath::OneMinus(IrValueId value) noexcept {
    return Sub(Scalar(1.0f), value);
}

IrValueId SurfaceMath::Component(IrValueId value, uint8 index) noexcept {
    if (failed_ || value == NoIrValue) {
        return NoIrValue;
    }

    return Check(context_.Swizzle(value, Span<const uint8>(&index, 1)));
}

IrValueId SurfaceMath::Max3(IrValueId value) noexcept {
    return Max(Max(Component(value, 0), Component(value, 1)), Component(value, 2));
}

IrValueId SurfaceMath::Min3(IrValueId value) noexcept {
    return Min(Min(Component(value, 0), Component(value, 1)), Component(value, 2));
}

IrValueId SurfaceMath::Average3(IrValueId value) noexcept {
    return Dot(value, Vector3(SurfaceMathThird));
}

IrValueId SurfaceMath::Pow5(IrValueId value) noexcept {
    const IrValueId square = Mul(value, value);
    return Mul(Mul(square, square), value);
}

IrValueId SurfaceMath::Splat3(IrValueId value) noexcept {
    if (failed_ || value == NoIrValue) {
        return NoIrValue;
    }

    return Check(context_.Splat(value, 3));
}

IrValueId SurfaceMath::Greater(IrValueId left, IrValueId right) noexcept {
    if (failed_ || left == NoIrValue || right == NoIrValue) {
        return NoIrValue;
    }

    return Check(context_.Binary(IrOp::GreaterThan, left, right, NumberRule::Arithmetic));
}

IrValueId SurfaceMath::Less(IrValueId left, IrValueId right) noexcept {
    if (failed_ || left == NoIrValue || right == NoIrValue) {
        return NoIrValue;
    }

    return Check(context_.Binary(IrOp::LessThan, left, right, NumberRule::Arithmetic));
}

IrValueId SurfaceMath::NotEqual(IrValueId left, IrValueId right) noexcept {
    if (failed_ || left == NoIrValue || right == NoIrValue) {
        return NoIrValue;
    }

    return Check(context_.Binary(IrOp::NotEquals, left, right, NumberRule::Equality));
}

IrValueId SurfaceMath::Equal(IrValueId left, IrValueId right) noexcept {
    if (failed_ || left == NoIrValue || right == NoIrValue) {
        return NoIrValue;
    }

    return Check(context_.Binary(IrOp::Equals, left, right, NumberRule::Equality));
}

IrValueId SurfaceMath::Or(IrValueId left, IrValueId right) noexcept {
    bool constant = false;
    if (ConstantBool(left, constant)) {
        return constant ? left : right;
    }

    if (ConstantBool(right, constant)) {
        return constant ? right : left;
    }

    if (failed_ || left == NoIrValue || right == NoIrValue) {
        return NoIrValue;
    }

    return Check(context_.Binary(IrOp::Or, left, right, NumberRule::Logical));
}

IrValueId SurfaceMath::And(IrValueId left, IrValueId right) noexcept {
    bool constant = false;
    if (ConstantBool(left, constant)) {
        return constant ? right : left;
    }

    if (ConstantBool(right, constant)) {
        return constant ? left : right;
    }

    if (failed_ || left == NoIrValue || right == NoIrValue) {
        return NoIrValue;
    }

    return Check(context_.Binary(IrOp::And, left, right, NumberRule::Logical));
}

IrValueId SurfaceMath::Not(IrValueId value) noexcept {
    if (failed_ || value == NoIrValue) {
        return NoIrValue;
    }

    return Check(context_.Unary(IrOp::Not, value, NumberRule::Logical));
}

IrValueId SurfaceMath::MaxUInt(IrValueId left, IrValueId right) noexcept {
    if (failed_ || left == NoIrValue || right == NoIrValue) {
        return NoIrValue;
    }

    return Check(context_.Binary(IrOp::Max, left, right, NumberRule::NoDouble));
}

IrValueId SurfaceMath::MinUInt(IrValueId left, IrValueId right) noexcept {
    if (failed_ || left == NoIrValue || right == NoIrValue) {
        return NoIrValue;
    }

    return Check(context_.Binary(IrOp::Min, left, right, NumberRule::NoDouble));
}

IrValueId SurfaceMath::Select(IrValueId condition, IrValueId whenTrue,
                              IrValueId whenFalse) noexcept {
    bool constant = false;
    if (ConstantBool(condition, constant)) {
        return constant ? whenTrue : whenFalse;
    }

    if (failed_ || condition == NoIrValue || whenTrue == NoIrValue || whenFalse == NoIrValue) {
        return NoIrValue;
    }

    return Check(context_.Select(condition, whenTrue, whenFalse));
}

bool SurfaceMath::ConstantBool(IrValueId value, bool& out) const noexcept {
    return context_.ConstantBool(value, out);
}

} // namespace rocks::material::detail
