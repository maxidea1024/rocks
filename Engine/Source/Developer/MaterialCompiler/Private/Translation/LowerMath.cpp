#include <MaterialSchema/Nodes/Math/MathNodeEnums.h>

#include "LoweringContext.h"
#include "LoweringTable.h"

// 수학 노드 lowering이다(Docs/Design/MaterialSystem 6.8절 lowering 표). 근거는 UE
// MaterialExpressions.cpp의 Compile, MaterialExpressionsToMIR.cpp의 Build, 연산 원문은
// HLSLMaterialTranslator.cpp다.
namespace rocks::material::detail {

namespace {

// UE UE_PI(double)다. 주기 계수는 UE처럼 float로 바꿔 계산한다(2.0f * (float)UE_PI / Period).
constexpr double MaterialPi = 3.14159265358979323846;
// UE PositiveClampedPow의 밑 하한이다(SH/Private/Common.ush:811 — 이 값 이하의 밑은 0을 낸다).
constexpr float PositiveClampedPowFloor = 2.980233e-8f;
// UE InverseLinearInterpolate 결과 범위(0–1)다.
constexpr float UnitMin = 0.0f;
constexpr float UnitMax = 1.0f;
// Cross 입력 폭이다.
constexpr uint8 CrossComponents = 3;

// NaN·Inf가 아닌가다(V11).
bool MathIsFinite(float value) noexcept {
    return value == value && value - value == 0.0f;
}

// UE 기본 번역기가 첫 인자 타입을 결과로 쓰는 연산의 첫 인자다(6.8절 "첫 인자 타입"): Bool·행렬은
// V1, floatOnly면 Double·정수를 Float로, 아니면 Double만 Float로 바꾼다.
Result<IrValueId> FirstOperand(LoweringContext& context, IrValueId value, StringView pin,
                               bool floatOnly) noexcept {
    if (value == NoIrValue) {
        return Result<IrValueId>::Success(NoIrValue);
    }

    const IrType type = context.TypeOf(value);
    if (type.scalar == IrScalar::Bool || type.IsMatrix()) {
        if (auto reported = context.Error(MaterialDiagnosticCode::TypeMismatch, pin,
                                          "Operation needs a numeric scalar or vector");
            !reported) {
            return Result<IrValueId>::Failure(reported.Error());
        }

        return Result<IrValueId>::Success(NoIrValue);
    }

    const bool toFloat =
        type.scalar == IrScalar::Double ||
        (floatOnly && (type.scalar == IrScalar::Int || type.scalar == IrScalar::UInt));
    return toFloat ? context.Convert(value, IrScalar::Float) : Result<IrValueId>::Success(value);
}

// 상수인 모든 성분이 value인가(Float·정수 상수).
bool IsConstant(LoweringContext& context, IrValueId id, int32 value) noexcept {
    return id != NoIrValue && context.Builder().IsConstantEqualTo(id, value);
}

Result<void> Emit(LoweringContext& context, StringView key, Result<IrValueId> value) noexcept {
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    return context.Output(key, value.Value());
}

// 입력 key 하나의 단항 노드다.
template <IrOp Op, NumberRule Rule>
Result<void> LowerUnaryInput(LoweringContext& context) noexcept {
    auto input = context.Input("Input");
    if (!input) {
        return Result<void>::Failure(input.Error());
    }

    return Emit(context, "Result", context.Unary(Op, input.Value(), Rule));
}

// 입력 X의 단항 노드다(Logarithm2·Logarithm10).
template <IrOp Op>
Result<void> LowerUnaryX(LoweringContext& context) noexcept {
    auto input = context.Input("X");
    if (!input) {
        return Result<void>::Failure(input.Error());
    }

    return Emit(context, "Result", context.Unary(Op, input.Value(), NumberRule::FloatOnly));
}

// 입력 first·second의 이항 노드다.
template <IrOp Op, NumberRule Rule>
Result<void> LowerBinaryPins(LoweringContext& context, StringView first,
                             StringView second) noexcept {
    auto a = context.Input(first);
    auto b = context.Input(second);
    if (!a || !b) {
        return Result<void>::Failure(!a ? a.Error() : b.Error());
    }

    return Emit(context, "Result", context.Binary(Op, a.Value(), b.Value(), Rule));
}

template <IrOp Op, NumberRule Rule>
Result<void> LowerBinaryAB(LoweringContext& context) noexcept {
    return LowerBinaryPins<Op, Rule>(context, "A", "B");
}

// Fmod·Modulo: 결과는 A 타입이고 B를 A 타입으로 맞춘다(UE HLSLMaterialTranslator.cpp:5795, 5824 —
// CoerceParameter(B, A 타입)).
template <IrOp Op, NumberRule Rule>
Result<void> LowerFirstTyped(LoweringContext& context) noexcept {
    auto a = context.Input("A");
    auto b = context.Input("B");
    if (!a || !b) {
        return Result<void>::Failure(!a ? a.Error() : b.Error());
    }

    auto first = FirstOperand(context, a.Value(), "A", Rule == NumberRule::FloatOnly);
    if (!first || first.Value() == NoIrValue || b.Value() == NoIrValue) {
        return first ? Result<void>::Success() : Result<void>::Failure(first.Error());
    }

    auto second = context.CoerceTo(b.Value(), context.TypeOf(first.Value()), "B");
    if (!second || second.Value() == NoIrValue) {
        return second ? Result<void>::Success() : Result<void>::Failure(second.Error());
    }

    return Emit(context, "Result", context.Binary(Op, first.Value(), second.Value(), Rule));
}

// UE Arctangent2·Arctangent2Fast·Step: 입력 Y·X 순서다(Step(Y, X) = X ≥ Y ? 1 : 0).
template <IrOp Op>
Result<void> LowerBinaryYX(LoweringContext& context) noexcept {
    return LowerBinaryPins<Op, NumberRule::FloatOnly>(context, "Y", "X");
}

Result<void> LowerClamp(LoweringContext& context) noexcept {
    // UE FHLSLMaterialTranslator::Clamp(HLSLMaterialTranslator.cpp:10572): 결과는 Input 타입이고
    // Min·Max를 그 타입으로 맞춘다. Min·Max가 상수 0·1이면 saturate다.
    auto input = context.Input("Input");
    auto low = context.Input("Min");
    auto high = context.Input("Max");
    if (!input || !low || !high) {
        return Result<void>::Failure(!input ? input.Error() : !low ? low.Error() : high.Error());
    }

    int64 mode = 0;
    auto read = context.ReadInteger("clampMode", static_cast<int64>(ClampMode::Clamp), mode);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    switch (static_cast<ClampMode>(mode)) {
    case ClampMode::Clamp:
        break;
    case ClampMode::ClampMin:
        return Emit(context, "Result",
                    context.Binary(IrOp::Max, input.Value(), low.Value(), NumberRule::Arithmetic));
    case ClampMode::ClampMax:
        return Emit(context, "Result",
                    context.Binary(IrOp::Min, input.Value(), high.Value(), NumberRule::Arithmetic));
    default:
        return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                             "Unknown clamp mode");
    }

    auto value = FirstOperand(context, input.Value(), "Input", true);
    if (!value || value.Value() == NoIrValue || low.Value() == NoIrValue ||
        high.Value() == NoIrValue) {
        return value ? Result<void>::Success() : Result<void>::Failure(value.Error());
    }

    if (IsConstant(context, low.Value(), 0) && IsConstant(context, high.Value(), 1)) {
        return Emit(context, "Result",
                    context.Unary(IrOp::Saturate, value.Value(), NumberRule::FloatOnly));
    }

    const IrType type = context.TypeOf(value.Value());
    auto lo = context.CoerceTo(low.Value(), type, "Min");
    auto hi = context.CoerceTo(high.Value(), type, "Max");
    if (!lo || !hi) {
        return Result<void>::Failure(!lo ? lo.Error() : hi.Error());
    }

    return Emit(
        context, "Result",
        context.Ternary(IrOp::Clamp, value.Value(), lo.Value(), hi.Value(), NumberRule::NoDouble));
}

Result<void> LowerOneMinus(LoweringContext& context) noexcept {
    // UE Build: 입력과 같은 수 종류의 1에서 뺀다(ConstantOne).
    auto input = context.Input("Input");
    if (!input || input.Value() == NoIrValue) {
        return input ? Result<void>::Success() : Result<void>::Failure(input.Error());
    }

    const IrType type = context.TypeOf(input.Value());
    if (type.scalar == IrScalar::Bool || type.IsMatrix()) {
        return context.Error(MaterialDiagnosticCode::TypeMismatch, "Input",
                             "1-x needs a numeric scalar or vector");
    }

    auto one = context.One(IrType{type.scalar, 1, 1});
    if (!one) {
        return Result<void>::Failure(one.Error());
    }

    return Emit(context, "Result",
                context.Binary(IrOp::Subtract, one.Value(), input.Value(), NumberRule::Arithmetic));
}

// Sine·Cosine·Tangent: period > 0이면 입력 × (2π / period)다. UE 기본 번역기의 Cosine·Tangent는
// period ≤ 0에서 코드 번호 0을 곱하는 결함이 있어 IR Build(GetTrigonometricInputWithPeriod)를
// 따른다.
template <IrOp Op>
Result<void> LowerPeriodic(LoweringContext& context) noexcept {
    auto input = context.Input("Input");
    if (!input) {
        return Result<void>::Failure(input.Error());
    }

    float period = 1.0f;
    auto read = context.ReadFloat("period", 1.0f, period);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    if (!MathIsFinite(period)) {
        return context.Error(MaterialDiagnosticCode::NonFiniteConstant, StringView(),
                             "Period is NaN or infinite");
    }

    IrValueId argument = input.Value();
    if (period > 0.0f) {
        const float factor = 2.0f * static_cast<float>(MaterialPi) / period;
        auto scale = context.FloatConstant(Span<const float>(&factor, 1), StringView());
        if (!scale) {
            return Result<void>::Failure(scale.Error());
        }

        auto scaled =
            context.Binary(IrOp::Multiply, argument, scale.Value(), NumberRule::Arithmetic);
        if (!scaled) {
            return Result<void>::Failure(scaled.Error());
        }

        argument = scaled.Value();
    }

    return Emit(context, "Result", context.Unary(Op, argument, NumberRule::FloatOnly));
}

Result<void> LowerPower(LoweringContext& context) noexcept {
    // UE FHLSLMaterialTranslator::Power(HLSLMaterialTranslator.cpp:9819): 결과는 Base 타입이고
    // 지수는 float 스칼라로 맞춘다(CoerceParameter(Exponent, MCT_Float) — 벡터면 첫 성분). 상수
    // 지수 1이면 Base, 0이면 1, 상수 밑 0이면 0이다. 그 밖은 PositiveClampedPow(밑
    // ≤ 2.980233e-8이면 0)다.
    auto base = context.Input("Base");
    auto exponent = context.Input("Exponent");
    if (!base || !exponent) {
        return Result<void>::Failure(!base ? base.Error() : exponent.Error());
    }

    auto b = FirstOperand(context, base.Value(), "Base", true);
    if (!b || b.Value() == NoIrValue || exponent.Value() == NoIrValue) {
        return b ? Result<void>::Success() : Result<void>::Failure(b.Error());
    }

    const IrType type = context.TypeOf(b.Value());
    if (context.TypeOf(exponent.Value()).scalar == IrScalar::Bool) {
        return context.Error(MaterialDiagnosticCode::TypeMismatch, "Exponent",
                             "Exponent must be numeric");
    }

    if (context.Builder().Value(exponent.Value()).op == IrOp::Constant) {
        // 상수 지수의 지름길 전에 UE처럼 두 타입이 산술로 맞는지 본다(GetArithmeticResultType).
        const IrValueId operands[] = {b.Value(), exponent.Value()};
        auto common = context.CommonType(Span<const IrValueId>(operands, 2), NumberRule::FloatOnly);
        if (!common || !common.Value()) {
            return common ? Result<void>::Success() : Result<void>::Failure(common.Error());
        }

        if (IsConstant(context, exponent.Value(), 1)) {
            return context.Output("Result", b.Value());
        }

        if (IsConstant(context, exponent.Value(), 0)) {
            return Emit(context, "Result", context.One(type));
        }

        if (IsConstant(context, b.Value(), 0)) {
            return Emit(context, "Result", context.Zero(type));
        }
    }

    auto scalarExponent = context.CoerceTo(exponent.Value(), IrFloat(1), "Exponent");
    if (!scalarExponent || scalarExponent.Value() == NoIrValue) {
        return scalarExponent ? Result<void>::Success()
                              : Result<void>::Failure(scalarExponent.Error());
    }

    auto e = context.Splat(scalarExponent.Value(), type.components);
    auto floor = context.FloatScalar(PositiveClampedPowFloor);
    auto zero = context.Zero(type);
    if (!e || !floor || !zero) {
        return Result<void>::Failure(!e ? e.Error() : !floor ? floor.Error() : zero.Error());
    }

    auto small =
        context.Binary(IrOp::LessThanOrEquals, b.Value(), floor.Value(), NumberRule::FloatOnly);
    auto power = context.Binary(IrOp::Pow, b.Value(), e.Value(), NumberRule::FloatOnly);
    if (!small || !power) {
        return Result<void>::Failure(!small ? small.Error() : power.Error());
    }

    return Emit(context, "Result", context.Select(small.Value(), zero.Value(), power.Value()));
}

Result<void> LowerLinearInterpolate(LoweringContext& context) noexcept {
    // UE FHLSLMaterialTranslator::Lerp(HLSLMaterialTranslator.cpp:10367): A와 B가 같은 값이면 A,
    // 상수 alpha가 0이면 A, 1이면 B를 그대로 낸다(lerp 식의 반올림 없이 정확히).
    auto a = context.Input("A");
    auto b = context.Input("B");
    auto alpha = context.Input("Alpha");
    if (!a || !b || !alpha) {
        return Result<void>::Failure(!a ? a.Error() : !b ? b.Error() : alpha.Error());
    }

    if (a.Value() == NoIrValue || b.Value() == NoIrValue || alpha.Value() == NoIrValue) {
        return Result<void>::Success();
    }

    if (a.Value() == b.Value()) {
        return context.Output("Result", a.Value());
    }

    if (context.TypeOf(alpha.Value()).scalar == IrScalar::Float) {
        if (IsConstant(context, alpha.Value(), 0)) {
            return context.Output("Result", a.Value());
        }

        if (IsConstant(context, alpha.Value(), 1)) {
            return context.Output("Result", b.Value());
        }
    }

    return Emit(
        context, "Result",
        context.Ternary(IrOp::Lerp, a.Value(), b.Value(), alpha.Value(), NumberRule::FloatOnly));
}

Result<void> LowerSmoothStep(LoweringContext& context) noexcept {
    auto low = context.Input("Min");
    auto high = context.Input("Max");
    auto value = context.Input("Value");
    if (!low || !high || !value) {
        return Result<void>::Failure(!low ? low.Error() : !high ? high.Error() : value.Error());
    }

    return Emit(context, "Result",
                context.Ternary(IrOp::Smoothstep, low.Value(), high.Value(), value.Value(),
                                NumberRule::FloatOnly));
}

Result<void> LowerInverseLinearInterpolate(LoweringContext& context) noexcept {
    // UE FHLSLMaterialTranslator::InvLerp(HLSLMaterialTranslator.cpp:10264).
    auto a = context.Input("A");
    auto b = context.Input("B");
    auto value = context.Input("Value");
    if (!a || !b || !value) {
        return Result<void>::Failure(!a ? a.Error() : !b ? b.Error() : value.Error());
    }

    if (a.Value() == NoIrValue || b.Value() == NoIrValue || value.Value() == NoIrValue) {
        return Result<void>::Success();
    }

    bool clamp = false;
    auto read = context.ReadBool("clampResult", false, clamp);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    // 같은 식(같은 IR 값)이면 UE처럼 바로 1·0이다. 결과 폭은 Value의 폭이다.
    const IrType resultType = IrFloat(context.TypeOf(value.Value()).components);
    IrValueId result = NoIrValue;
    if (value.Value() == b.Value() || value.Value() == a.Value()) {
        // UE는 Y == A(1)를 먼저, X == A(0)를 나중에 보아 둘 다면 0이다.
        auto constant =
            value.Value() == a.Value() ? context.Zero(resultType) : context.One(resultType);
        if (!constant) {
            return Result<void>::Failure(constant.Error());
        }

        result = constant.Value();
    } else if (a.Value() == b.Value()) {
        return context.Error(MaterialDiagnosticCode::DivideByZero, StringView(),
                             "Div by Zero: InvLerp A == B");
    } else {
        auto numerator =
            context.Binary(IrOp::Subtract, value.Value(), a.Value(), NumberRule::Arithmetic);
        auto denominator =
            context.Binary(IrOp::Subtract, b.Value(), a.Value(), NumberRule::Arithmetic);
        if (!numerator || !denominator) {
            return Result<void>::Failure(!numerator ? numerator.Error() : denominator.Error());
        }

        auto divided = context.Binary(IrOp::Divide, numerator.Value(), denominator.Value(),
                                      NumberRule::Arithmetic);
        if (!divided) {
            return Result<void>::Failure(divided.Error());
        }

        result = divided.Value();
    }

    if (clamp && result != NoIrValue) {
        auto low = context.FloatScalar(UnitMin);
        auto high = context.FloatScalar(UnitMax);
        if (!low || !high) {
            return Result<void>::Failure(!low ? low.Error() : high.Error());
        }

        auto clamped =
            context.Ternary(IrOp::Clamp, result, low.Value(), high.Value(), NumberRule::NoDouble);
        if (!clamped) {
            return Result<void>::Failure(clamped.Error());
        }

        result = clamped.Value();
    }

    return context.Output("Result", result);
}

Result<void> LowerDotProduct(LoweringContext& context) noexcept {
    // UE FHLSLMaterialTranslator::Dot: 스칼라는 넓은 쪽으로 복제한다. 폭이 다른 두 벡터는
    // CoerceParameter가 자르지 않아 오류다("Cannot cast from larger type").
    auto a = context.Input("A");
    auto b = context.Input("B");
    if (!a || !b) {
        return Result<void>::Failure(!a ? a.Error() : b.Error());
    }

    if (a.Value() == NoIrValue || b.Value() == NoIrValue) {
        return Result<void>::Success();
    }

    const uint8 leftWidth = context.TypeOf(a.Value()).components;
    const uint8 rightWidth = context.TypeOf(b.Value()).components;
    if (leftWidth > 1 && rightWidth > 1 && leftWidth != rightWidth) {
        return context.Error(MaterialDiagnosticCode::TypeMismatch, StringView(),
                             "Cannot cast between vectors of different widths");
    }

    return Emit(context, "Result",
                context.Binary(IrOp::Dot, a.Value(), b.Value(), NumberRule::FloatOnly));
}

Result<void> LowerCrossProduct(LoweringContext& context) noexcept {
    auto a = context.Input("A");
    auto b = context.Input("B");
    if (!a || !b) {
        return Result<void>::Failure(!a ? a.Error() : b.Error());
    }

    auto left = context.Coerce(a.Value(), MaterialPinType::Float3, "A");
    auto right = context.Coerce(b.Value(), MaterialPinType::Float3, "B");
    if (!left || !right) {
        return Result<void>::Failure(!left ? left.Error() : right.Error());
    }

    static_assert(CrossComponents == 3, "cross product is defined for three components");
    return Emit(context, "Result",
                context.Binary(IrOp::Cross, left.Value(), right.Value(), NumberRule::FloatOnly));
}

// 길이다: 스칼라는 abs(UE 기본 번역기 최적화), 벡터는 length.
Result<IrValueId> LengthOf(LoweringContext& context, IrValueId value) noexcept {
    if (value == NoIrValue) {
        return Result<IrValueId>::Success(NoIrValue);
    }

    const bool scalar = context.TypeOf(value).components == 1;
    return context.Unary(scalar ? IrOp::Abs : IrOp::Length, value, NumberRule::FloatOnly);
}

Result<void> LowerLength(LoweringContext& context) noexcept {
    auto input = context.Input("Input");
    if (!input) {
        return Result<void>::Failure(input.Error());
    }

    return Emit(context, "Result", LengthOf(context, input.Value()));
}

Result<void> LowerDistance(LoweringContext& context) noexcept {
    auto a = context.Input("A");
    auto b = context.Input("B");
    if (!a || !b) {
        return Result<void>::Failure(!a ? a.Error() : b.Error());
    }

    auto difference = context.Binary(IrOp::Subtract, a.Value(), b.Value(), NumberRule::Arithmetic);
    if (!difference) {
        return Result<void>::Failure(difference.Error());
    }

    return Emit(context, "Result", LengthOf(context, difference.Value()));
}

Result<void> LowerNormalize(LoweringContext& context) noexcept {
    auto input = context.Input("VectorInput");
    if (!input) {
        return Result<void>::Failure(input.Error());
    }

    return Emit(context, "Result",
                context.Unary(IrOp::Normalize, input.Value(), NumberRule::FloatOnly));
}

Result<void> LowerAppend(LoweringContext& context) noexcept {
    // UE Build AppendVector: 성분 합 ≤ 4, 한쪽이 Double이면 Double, 아니면 Float.
    auto a = context.Input("A");
    auto b = context.Input("B");
    if (!a || !b) {
        return Result<void>::Failure(!a ? a.Error() : b.Error());
    }

    if (a.Value() == NoIrValue || b.Value() == NoIrValue) {
        return Result<void>::Success();
    }

    const IrType left = context.TypeOf(a.Value());
    const IrType right = context.TypeOf(b.Value());
    if (left.IsMatrix() || right.IsMatrix() || left.scalar == IrScalar::Bool ||
        right.scalar == IrScalar::Bool) {
        return context.Error(MaterialDiagnosticCode::TypeMismatch, StringView(),
                             "Append needs numeric scalars or vectors");
    }

    if (left.components + right.components > MaxIrComponents) {
        return context.Error(MaterialDiagnosticCode::TypeMismatch, StringView(),
                             "The appended vector would have more than 4 components");
    }

    const IrScalar scalar = left.scalar == IrScalar::Double || right.scalar == IrScalar::Double
                                ? IrScalar::Double
                                : IrScalar::Float;
    auto l = context.Convert(a.Value(), scalar);
    auto r = context.Convert(b.Value(), scalar);
    if (!l || !r) {
        return Result<void>::Failure(!l ? l.Error() : r.Error());
    }

    return Emit(context, "Result", context.Append(l.Value(), r.Value()));
}

Result<void> LowerComponentMask(LoweringContext& context) noexcept {
    auto input = context.Input("Input");
    if (!input || input.Value() == NoIrValue) {
        return input ? Result<void>::Success() : Result<void>::Failure(input.Error());
    }

    static constexpr StringView Fields[MaxIrComponents] = {"r", "g", "b", "a"};
    uint8 selected[MaxIrComponents] = {0, 0, 0, 0};
    uint8 count = 0;
    for (uint8 index = 0; index < MaxIrComponents; ++index) {
        bool enabled = false;
        auto read = context.ReadBool(Fields[index], false, enabled);
        if (!read || !read.Value()) {
            return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
        }

        if (enabled) {
            selected[count++] = index;
        }
    }

    const IrType type = context.TypeOf(input.Value());
    if (count == 0) {
        return context.Error(MaterialDiagnosticCode::TypeMismatch, StringView(),
                             "Component mask selects no components");
    }

    if (type.IsMatrix() || (type.components > 1 && selected[count - 1] >= type.components)) {
        return context.Error(MaterialDiagnosticCode::TypeMismatch, "Input",
                             "Not enough components in the input for the component mask");
    }

    // UE는 폭이 정해지지 않은 스칼라(MCT_Float)에 g·b·a 마스크를 .r로
    // 복제한다(HLSLMaterialTranslator.cpp: 10776–10817).
    if (type.components == 1) {
        for (uint8 index = 0; index < count; ++index) {
            selected[index] = 0;
        }
    }

    return Emit(context, "Result",
                context.Swizzle(input.Value(), Span<const uint8>(selected, count)));
}

Result<void> LowerFloatToUInt(LoweringContext& context) noexcept {
    auto input = context.Input("Input");
    if (!input) {
        return Result<void>::Failure(input.Error());
    }

    int64 mode = 0;
    auto read = context.ReadInteger("mode", static_cast<int64>(FloatToIntMode::Truncate), mode);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    IrOp op = IrOp::Truncate;
    switch (static_cast<FloatToIntMode>(mode)) {
    case FloatToIntMode::Truncate:
        op = IrOp::Truncate;
        break;
    case FloatToIntMode::Floor:
        op = IrOp::Floor;
        break;
    case FloatToIntMode::Round:
        op = IrOp::Round;
        break;
    case FloatToIntMode::Ceil:
        op = IrOp::Ceil;
        break;
    default:
        return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                             "Unknown float to uint mode");
    }

    auto rounded = context.Unary(op, input.Value(), NumberRule::Arithmetic);
    if (!rounded) {
        return Result<void>::Failure(rounded.Error());
    }

    return Emit(context, "Result", context.Convert(rounded.Value(), IrScalar::UInt));
}

Result<void> LowerUIntToFloat(LoweringContext& context) noexcept {
    auto input = context.Input("Input");
    if (!input) {
        return Result<void>::Failure(input.Error());
    }

    return Emit(context, "Result", context.Convert(input.Value(), IrScalar::Float));
}

Result<void> LowerConstantBiasScale(LoweringContext& context) noexcept {
    // UE Compile: Mul(Add(Constant(Bias), Input), Constant(Scale)).
    auto input = context.Input("Input");
    if (!input) {
        return Result<void>::Failure(input.Error());
    }

    float bias = 1.0f;
    float scale = 0.5f;
    auto readBias = context.ReadFloat("bias", 1.0f, bias);
    auto readScale = context.ReadFloat("scale", 0.5f, scale);
    if (!readBias || !readScale) {
        return Result<void>::Failure(!readBias ? readBias.Error() : readScale.Error());
    }

    if (!readBias.Value() || !readScale.Value()) {
        return Result<void>::Success();
    }

    auto biasValue = context.FloatConstant(Span<const float>(&bias, 1), StringView());
    auto scaleValue = context.FloatConstant(Span<const float>(&scale, 1), StringView());
    if (!biasValue || !scaleValue) {
        return Result<void>::Failure(!biasValue ? biasValue.Error() : scaleValue.Error());
    }

    auto biased =
        context.Binary(IrOp::Add, biasValue.Value(), input.Value(), NumberRule::Arithmetic);
    if (!biased) {
        return Result<void>::Failure(biased.Error());
    }

    return Emit(
        context, "Result",
        context.Binary(IrOp::Multiply, biased.Value(), scaleValue.Value(), NumberRule::Arithmetic));
}

constexpr LoweringEntry MathEntries[] = {
    {"Material.Abs", &LowerUnaryInput<IrOp::Abs, NumberRule::Arithmetic>},
    {"Material.Floor", &LowerUnaryInput<IrOp::Floor, NumberRule::Arithmetic>},
    {"Material.Ceil", &LowerUnaryInput<IrOp::Ceil, NumberRule::Arithmetic>},
    {"Material.Round", &LowerUnaryInput<IrOp::Round, NumberRule::Arithmetic>},
    {"Material.Truncate", &LowerUnaryInput<IrOp::Truncate, NumberRule::Arithmetic>},
    {"Material.Frac", &LowerUnaryInput<IrOp::Frac, NumberRule::FloatOnly>},
    {"Material.Saturate", &LowerUnaryInput<IrOp::Saturate, NumberRule::FloatOnly>},
    {"Material.Sign", &LowerUnaryInput<IrOp::Sign, NumberRule::FloatOnly>},
    {"Material.SquareRoot", &LowerUnaryInput<IrOp::Sqrt, NumberRule::FloatOnly>},
    {"Material.Exponential", &LowerUnaryInput<IrOp::Exponential, NumberRule::FloatOnly>},
    {"Material.Exponential2", &LowerUnaryInput<IrOp::Exponential2, NumberRule::FloatOnly>},
    {"Material.Logarithm", &LowerUnaryInput<IrOp::Logarithm, NumberRule::FloatOnly>},
    {"Material.Logarithm2", &LowerUnaryX<IrOp::Logarithm2>},
    {"Material.Logarithm10", &LowerUnaryX<IrOp::Logarithm10>},
    {"Material.Arcsine", &LowerUnaryInput<IrOp::ASin, NumberRule::FloatOnly>},
    {"Material.Arccosine", &LowerUnaryInput<IrOp::ACos, NumberRule::FloatOnly>},
    {"Material.Arctangent", &LowerUnaryInput<IrOp::ATan, NumberRule::FloatOnly>},
    {"Material.ArcsineFast", &LowerUnaryInput<IrOp::ASinFast, NumberRule::FloatOnly>},
    {"Material.ArccosineFast", &LowerUnaryInput<IrOp::ACosFast, NumberRule::FloatOnly>},
    {"Material.ArctangentFast", &LowerUnaryInput<IrOp::ATanFast, NumberRule::FloatOnly>},
    {"Material.Sine", &LowerPeriodic<IrOp::Sin>},
    {"Material.Cosine", &LowerPeriodic<IrOp::Cos>},
    {"Material.Tangent", &LowerPeriodic<IrOp::Tan>},
    {"Material.Add", &LowerBinaryAB<IrOp::Add, NumberRule::Arithmetic>},
    {"Material.Subtract", &LowerBinaryAB<IrOp::Subtract, NumberRule::Arithmetic>},
    {"Material.Multiply", &LowerBinaryAB<IrOp::Multiply, NumberRule::Arithmetic>},
    {"Material.Divide", &LowerBinaryAB<IrOp::Divide, NumberRule::Arithmetic>},
    {"Material.Min", &LowerBinaryAB<IrOp::Min, NumberRule::Arithmetic>},
    {"Material.Max", &LowerBinaryAB<IrOp::Max, NumberRule::Arithmetic>},
    {"Material.Fmod", &LowerFirstTyped<IrOp::Fmod, NumberRule::FloatOnly>},
    {"Material.Modulo", &LowerFirstTyped<IrOp::Modulo, NumberRule::NoDouble>},
    {"Material.Step", &LowerBinaryYX<IrOp::Step>},
    {"Material.Arctangent2", &LowerBinaryYX<IrOp::ATan2>},
    {"Material.Arctangent2Fast", &LowerBinaryYX<IrOp::ATan2Fast>},
    {"Material.Clamp", &LowerClamp},
    {"Material.OneMinus", &LowerOneMinus},
    {"Material.Power", &LowerPower},
    {"Material.LinearInterpolate", &LowerLinearInterpolate},
    {"Material.SmoothStep", &LowerSmoothStep},
    {"Material.InverseLinearInterpolate", &LowerInverseLinearInterpolate},
    {"Material.DotProduct", &LowerDotProduct},
    {"Material.CrossProduct", &LowerCrossProduct},
    {"Material.Length", &LowerLength},
    {"Material.Distance", &LowerDistance},
    {"Material.Normalize", &LowerNormalize},
    {"Material.Append", &LowerAppend},
    {"Material.ComponentMask", &LowerComponentMask},
    {"Material.FloatToUInt", &LowerFloatToUInt},
    {"Material.UIntToFloat", &LowerUIntToFloat},
    {"Material.ConstantBiasScale", &LowerConstantBiasScale},
};

} // namespace

Span<const LoweringEntry> MathLowerings() noexcept {
    return MathEntries;
}

} // namespace rocks::material::detail
