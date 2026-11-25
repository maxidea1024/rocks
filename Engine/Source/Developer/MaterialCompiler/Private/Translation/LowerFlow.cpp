#include <CoreTypeBindings/CoreValueTypes.generated.h>
#include <CoreTypeBindings/CoreValueTypes.h>
#include <MaterialSchema/Nodes/Math/ConvertInput.generated.h>
#include <MaterialSchema/Nodes/Math/ConvertMapping.generated.h>
#include <MaterialSchema/Nodes/Math/ConvertOutput.generated.h>
#include <MaterialSchema/Nodes/Math/MathNodeEnums.h>
#include <MaterialSchema/Nodes/Math/OperatorInput.generated.h>

#include "LoweringContext.h"
#include "LoweringTable.h"

// 흐름·선택·변환 노드 lowering이다(Docs/Design/MaterialSystem 6.8절 lowering 표 — DynamicBranch·If·
// Switch·Convert·Operator·NamedReroute).
namespace rocks::material::detail {

namespace {

// If의 같음 판정 기본 문턱이다(UE UMaterialExpressionIf::EqualsThreshold 기본값).
constexpr float DefaultIfEqualsThreshold = 0.00001f;
// Switch 식의 floor 보정값이다(UE FHLSLMaterialTranslator::Switch "floor(%s) + 0.5f").
constexpr float SwitchFloorBias = 0.5f;
// Operator 입력 기본값(UE FMaterialExpressionOperatorInput::ConstValue)이다.
constexpr float DefaultOperatorConstant = 1.0f;
// 이름 있는 재경유 선언 종류다.
constexpr StringView NamedRerouteDeclarationType = "Material.NamedRerouteDeclaration";

Result<void> FlowEmit(LoweringContext& context, StringView key, Result<IrValueId> value) noexcept {
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    return context.Output(key, value.Value());
}

Result<void> LowerDynamicBranch(LoweringContext& context) noexcept {
    // UE Build IfThenElse: 조건은 bool 스칼라(수는 bool로 바꾼다), 상수 조건이면 고른 입력만 쓴다.
    auto condition = context.Input("Condition");
    if (!condition || condition.Value() == NoIrValue) {
        return condition ? Result<void>::Success() : Result<void>::Failure(condition.Error());
    }

    const IrType type = context.TypeOf(condition.Value());
    if (type.IsMatrix() || type.components != 1) {
        return context.Error(MaterialDiagnosticCode::TypeMismatch, "Condition",
                             "Branch condition must be a scalar");
    }

    auto asBool = context.Convert(condition.Value(), IrScalar::Bool);
    if (!asBool || asBool.Value() == NoIrValue) {
        return asBool ? Result<void>::Success() : Result<void>::Failure(asBool.Error());
    }

    bool constant = false;
    if (context.ConstantBool(asBool.Value(), constant)) {
        auto chosen = context.Input(constant ? StringView("True") : StringView("False"));
        if (!chosen) {
            return Result<void>::Failure(chosen.Error());
        }

        return context.Output("Result", chosen.Value());
    }

    auto whenTrue = context.Input("True");
    auto whenFalse = context.Input("False");
    if (!whenTrue || !whenFalse) {
        return Result<void>::Failure(!whenTrue ? whenTrue.Error() : whenFalse.Error());
    }

    return FlowEmit(context, "Result",
                    context.Branch(asBool.Value(), whenTrue.Value(), whenFalse.Value()));
}

Result<void> LowerIf(LoweringContext& context) noexcept {
    // UE FHLSLMaterialTranslator::If(HLSLMaterialTranslator.cpp:6575).
    auto a = context.Input("A");
    auto b = context.Input("B");
    auto greater = context.Input("AGreaterThanB");
    auto less = context.Input("ALessThanB");
    auto hasEquals = context.IsConnected("AEqualsB");
    if (!a || !b || !greater || !less || !hasEquals) {
        return Result<void>::Failure(!a         ? a.Error()
                                     : !b       ? b.Error()
                                     : !greater ? greater.Error()
                                     : !less    ? less.Error()
                                                : hasEquals.Error());
    }

    IrValueId equals = NoIrValue;
    if (hasEquals.Value()) {
        auto value = context.Input("AEqualsB");
        if (!value) {
            return Result<void>::Failure(value.Error());
        }

        equals = value.Value();
        if (equals == NoIrValue) {
            return Result<void>::Success();
        }
    }

    if (a.Value() == NoIrValue || b.Value() == NoIrValue || greater.Value() == NoIrValue ||
        less.Value() == NoIrValue) {
        return Result<void>::Success();
    }

    // 결과가 모두 같은 값이면 그 값이다(UE AreEqualExpressions).
    if (greater.Value() == less.Value() && (!hasEquals.Value() || equals == greater.Value())) {
        return context.Output("Result", greater.Value());
    }

    float threshold = DefaultIfEqualsThreshold;
    auto read = context.ReadFloat("equalsThreshold", DefaultIfEqualsThreshold, threshold);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    IrValueId branches[3] = {greater.Value(), less.Value(), equals};
    const usize branchCount = hasEquals.Value() ? 3 : 2;
    auto common =
        context.CommonType(Span<const IrValueId>(branches, branchCount), NumberRule::Arithmetic);
    if (!common || !common.Value()) {
        return common ? Result<void>::Success() : Result<void>::Failure(common.Error());
    }

    for (usize index = 0; index < branchCount; ++index) {
        auto cast = context.Cast(branches[index], common.Value().Value());
        if (!cast || cast.Value() == NoIrValue) {
            return cast ? Result<void>::Success() : Result<void>::Failure(cast.Error());
        }

        branches[index] = cast.Value();
    }

    auto atLeast =
        context.Binary(IrOp::GreaterThanOrEquals, a.Value(), b.Value(), NumberRule::Arithmetic);
    if (!atLeast) {
        return Result<void>::Failure(atLeast.Error());
    }

    auto ordered = context.Select(atLeast.Value(), branches[0], branches[1]);
    if (!ordered || !hasEquals.Value()) {
        return FlowEmit(context, "Result", Move(ordered));
    }

    // select(abs(A - B) > threshold, select(A >= B, greater, less), equals)
    auto difference = context.Binary(IrOp::Subtract, a.Value(), b.Value(), NumberRule::Arithmetic);
    if (!difference) {
        return Result<void>::Failure(difference.Error());
    }

    auto distance = context.Unary(IrOp::Abs, difference.Value(), NumberRule::Arithmetic);
    auto limit = context.FloatConstant(Span<const float>(&threshold, 1), StringView());
    if (!distance || !limit) {
        return Result<void>::Failure(!distance ? distance.Error() : limit.Error());
    }

    auto apart =
        context.Binary(IrOp::GreaterThan, distance.Value(), limit.Value(), NumberRule::Arithmetic);
    if (!apart) {
        return Result<void>::Failure(apart.Error());
    }

    return FlowEmit(context, "Result", context.Select(apart.Value(), ordered.Value(), branches[2]));
}

Result<void> LowerSwitch(LoweringContext& context) noexcept {
    // UE FHLSLMaterialTranslator::Switch(HLSLMaterialTranslator.cpp:8437).
    auto selector = context.Input("SwitchValue");
    auto fallback = context.Input("Default");
    auto pins = context.DynamicPins();
    if (!selector || !fallback || !pins) {
        return Result<void>::Failure(!selector   ? selector.Error()
                                     : !fallback ? fallback.Error()
                                                 : pins.Error());
    }

    SystemAllocator scratch;
    Array<IrValueId> inputs(scratch);
    bool missing = false;
    for (const nodegraph::PinDescriptor& pin : pins.Value()) {
        if (pin.direction != nodegraph::PinDirection::Input) {
            continue;
        }

        auto value = context.DynamicInput(pin.key, Optional<float>(), true);
        if (!value) {
            return Result<void>::Failure(value.Error());
        }

        missing = missing || value.Value() == NoIrValue;
        if (auto added = inputs.TryPushBack(value.Value()); !added) {
            return added;
        }
    }

    if (inputs.IsEmpty()) {
        return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                             "Switch has no inputs");
    }

    if (missing || selector.Value() == NoIrValue || fallback.Value() == NoIrValue) {
        return Result<void>::Success();
    }

    // 상수 선택값이면 [i, i + 1)인 입력, 범위 밖이면 Default다.
    float chosen = 0.0f;
    if (context.ConstantFloat(selector.Value(), chosen)) {
        for (usize index = 0; index < inputs.Size(); ++index) {
            const float low = static_cast<float>(index);
            if (chosen >= low && chosen < low + 1.0f) {
                return context.Output("Result", inputs[index]);
            }
        }

        return context.Output("Result", fallback.Value());
    }

    auto common = context.CommonType(inputs.View(), NumberRule::Arithmetic);
    if (!common || !common.Value()) {
        return common ? Result<void>::Success() : Result<void>::Failure(common.Error());
    }

    const IrType type = common.Value().Value();
    auto scalarSelector = context.Prefix(selector.Value(), 1);
    if (!scalarSelector) {
        return Result<void>::Failure(scalarSelector.Error());
    }

    auto floored = context.Unary(IrOp::Floor, scalarSelector.Value(), NumberRule::FloatOnly);
    auto bias = context.FloatScalar(SwitchFloorBias);
    if (!floored || !bias) {
        return Result<void>::Failure(!floored ? floored.Error() : bias.Error());
    }

    auto position = context.Binary(IrOp::Add, floored.Value(), bias.Value(), NumberRule::FloatOnly);
    if (!position || position.Value() == NoIrValue) {
        return position ? Result<void>::Success() : Result<void>::Failure(position.Error());
    }

    // Σ step(i, f) * step(f, i + 1) * inputᵢ + (step(f, 0) + step(n, f)) * Default
    IrValueId sum = NoIrValue;
    for (usize index = 0; index < inputs.Size(); ++index) {
        auto low = context.FloatScalar(static_cast<float>(index));
        auto high = context.FloatScalar(static_cast<float>(index) + 1.0f);
        auto input = context.Cast(inputs[index], type);
        if (!low || !high || !input) {
            return Result<void>::Failure(!low ? low.Error() : !high ? high.Error() : input.Error());
        }

        auto above =
            context.Binary(IrOp::Step, low.Value(), position.Value(), NumberRule::FloatOnly);
        auto below =
            context.Binary(IrOp::Step, position.Value(), high.Value(), NumberRule::FloatOnly);
        if (!above || !below) {
            return Result<void>::Failure(!above ? above.Error() : below.Error());
        }

        auto inside =
            context.Binary(IrOp::Multiply, above.Value(), below.Value(), NumberRule::Arithmetic);
        if (!inside) {
            return Result<void>::Failure(inside.Error());
        }

        auto term =
            context.Binary(IrOp::Multiply, inside.Value(), input.Value(), NumberRule::Arithmetic);
        if (!term) {
            return Result<void>::Failure(term.Error());
        }

        if (sum == NoIrValue) {
            sum = term.Value();
        } else {
            auto added = context.Binary(IrOp::Add, sum, term.Value(), NumberRule::Arithmetic);
            if (!added) {
                return Result<void>::Failure(added.Error());
            }

            sum = added.Value();
        }
    }

    auto zero = context.FloatScalar(0.0f);
    auto count = context.FloatScalar(static_cast<float>(inputs.Size()));
    auto defaultValue = context.Cast(fallback.Value(), type);
    if (!zero || !count || !defaultValue) {
        return Result<void>::Failure(!zero    ? zero.Error()
                                     : !count ? count.Error()
                                              : defaultValue.Error());
    }

    auto before = context.Binary(IrOp::Step, position.Value(), zero.Value(), NumberRule::FloatOnly);
    auto after = context.Binary(IrOp::Step, count.Value(), position.Value(), NumberRule::FloatOnly);
    if (!before || !after) {
        return Result<void>::Failure(!before ? before.Error() : after.Error());
    }

    auto outside = context.Binary(IrOp::Add, before.Value(), after.Value(), NumberRule::FloatOnly);
    if (!outside) {
        return Result<void>::Failure(outside.Error());
    }

    auto defaultTerm = context.Binary(IrOp::Multiply, outside.Value(), defaultValue.Value(),
                                      NumberRule::Arithmetic);
    if (!defaultTerm) {
        return Result<void>::Failure(defaultTerm.Error());
    }

    return FlowEmit(context, "Result",
                    context.Binary(IrOp::Add, sum, defaultTerm.Value(), NumberRule::Arithmetic));
}

// Convert 값 종류의 성분 수다. 모르는 값이면 0이다.
uint8 ConvertComponents(int64 type) noexcept {
    switch (static_cast<ConvertValueType>(type)) {
    case ConvertValueType::Scalar:
        return 1;
    case ConvertValueType::Vector2:
        return 2;
    case ConvertValueType::Vector3:
        return 3;
    case ConvertValueType::Vector4:
        return 4;
    }

    return 0;
}

// 원소 reader의 LinearColor 칸이다.
Result<void> ReadElementColor(NodePropertyReader& element, StringView field,
                              float (&out)[MaxIrComponents]) noexcept {
    auto color = element.OpenStruct(field, GeneratedType<LinearColor>::Schema());
    if (!color) {
        return Result<void>::Failure(color.Error());
    }

    static constexpr StringView Channels[MaxIrComponents] = {"r", "g", "b", "a"};
    for (usize index = 0; index < MaxIrComponents; ++index) {
        auto channel = color.Value().ReadFloat(Channels[index], 0.0f);
        if (!channel) {
            return Result<void>::Failure(channel.Error());
        }

        out[index] = channel.Value();
    }

    return Result<void>::Success();
}

// Convert의 입출력 원소 하나다.
struct ConvertSide {
    uint8 components = 0;
    float defaults[MaxIrComponents] = {0.0f, 0.0f, 0.0f, 0.0f};
};

// Convert 속성 배열 field를 sides에 읽는다. 그래프 오류면 false다(진단은 부르는 쪽).
Result<bool> ReadConvertSides(NodePropertyReader& reader, StringView field,
                              const TypeSchema& schema, Array<ConvertSide>& sides) noexcept {
    auto list = reader.OpenStructArray(field, schema);
    if (!list) {
        return list.Error() == ErrorCode::OutOfMemory ? Result<bool>::Failure(list.Error())
                                                      : Result<bool>::Success(false);
    }

    for (usize index = 0; index < list.Value().Count(); ++index) {
        auto element = list.Value().Element(index);
        if (!element) {
            return element.Error() == ErrorCode::OutOfMemory
                       ? Result<bool>::Failure(element.Error())
                       : Result<bool>::Success(false);
        }

        auto type = element.Value().ReadInteger("type", 0);
        ConvertSide side;
        if (!type || (side.components = ConvertComponents(type.Value())) == 0) {
            return Result<bool>::Success(false);
        }

        if (auto read = ReadElementColor(element.Value(), "defaultValue", side.defaults); !read) {
            return read.Error() == ErrorCode::OutOfMemory ? Result<bool>::Failure(read.Error())
                                                          : Result<bool>::Success(false);
        }

        if (auto added = sides.TryPushBack(side); !added) {
            return Result<bool>::Failure(added.Error());
        }
    }

    return Result<bool>::Success(true);
}

// Convert 대응 하나다(UE FMaterialExpressionConvertMapping).
struct ConvertLink {
    int64 input = 0;
    int64 inputComponent = 0;
    int64 output = 0;
    int64 outputComponent = 0;
};

Result<bool> ReadConvertLinks(NodePropertyReader& reader, Array<ConvertLink>& links) noexcept {
    auto list = reader.OpenStructArray("convertMappings", GeneratedType<ConvertMapping>::Schema());
    if (!list) {
        return list.Error() == ErrorCode::OutOfMemory ? Result<bool>::Failure(list.Error())
                                                      : Result<bool>::Success(false);
    }

    for (usize index = 0; index < list.Value().Count(); ++index) {
        auto element = list.Value().Element(index);
        if (!element) {
            return element.Error() == ErrorCode::OutOfMemory
                       ? Result<bool>::Failure(element.Error())
                       : Result<bool>::Success(false);
        }

        auto input = element.Value().ReadInteger("inputIndex", 0);
        auto inputComponent = element.Value().ReadInteger("inputComponentIndex", 0);
        auto output = element.Value().ReadInteger("outputIndex", 0);
        auto outputComponent = element.Value().ReadInteger("outputComponentIndex", 0);
        if (!input || !inputComponent || !output || !outputComponent) {
            return Result<bool>::Success(false);
        }

        if (auto added = links.TryPushBack(ConvertLink{input.Value(), inputComponent.Value(),
                                                       output.Value(), outputComponent.Value()});
            !added) {
            return Result<bool>::Failure(added.Error());
        }
    }

    return Result<bool>::Success(true);
}

Result<void> LowerConvert(LoweringContext& context) noexcept {
    // UE UMaterialExpressionConvert::Compile(MaterialExpressions.cpp:22713).
    auto properties = context.Properties();
    auto pins = context.DynamicPins();
    if (!properties || !pins) {
        return Result<void>::Failure(!properties ? properties.Error() : pins.Error());
    }

    if (properties.Value() == nullptr) {
        return Result<void>::Success();
    }

    SystemAllocator scratch;
    Array<ConvertSide> inputs(scratch);
    Array<ConvertSide> outputs(scratch);
    Array<ConvertLink> links(scratch);
    auto readInputs = ReadConvertSides(*properties.Value(), "convertInputs",
                                       GeneratedType<ConvertInput>::Schema(), inputs);
    auto readOutputs = ReadConvertSides(*properties.Value(), "convertOutputs",
                                        GeneratedType<ConvertOutput>::Schema(), outputs);
    auto readLinks = ReadConvertLinks(*properties.Value(), links);
    if (!readInputs || !readOutputs || !readLinks) {
        return Result<void>::Failure(!readInputs    ? readInputs.Error()
                                     : !readOutputs ? readOutputs.Error()
                                                    : readLinks.Error());
    }

    if (!readInputs.Value() || !readOutputs.Value() || !readLinks.Value()) {
        return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                             "Convert properties cannot be read");
    }

    // 동적 핀 순서는 입력 Input0…, 출력 Output0…이다(19.13.3절).
    Array<Name> inputKeys(scratch);
    Array<Name> outputKeys(scratch);
    for (const nodegraph::PinDescriptor& pin : pins.Value()) {
        auto added = pin.direction == nodegraph::PinDirection::Input
                         ? inputKeys.TryPushBack(pin.key)
                         : outputKeys.TryPushBack(pin.key);
        if (!added) {
            return added;
        }
    }

    if (inputKeys.Size() != inputs.Size() || outputKeys.Size() != outputs.Size()) {
        return Result<void>::Success();
    }

    Array<IrValueId> compiled(scratch);
    if (auto sized = compiled.TryResize(inputs.Size()); !sized) {
        return sized;
    }

    for (IrValueId& value : compiled) {
        value = NoIrValue;
    }

    for (usize output = 0; output < outputs.Size(); ++output) {
        const ConvertSide& side = outputs[output];
        IrValueId components[MaxIrComponents] = {NoIrValue, NoIrValue, NoIrValue, NoIrValue};
        for (const ConvertLink& link : links) {
            if (link.output != static_cast<int64>(output)) {
                continue;
            }

            if (link.outputComponent < 0 || link.outputComponent >= side.components) {
                return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                                     "Invalid Mapping Output Component Index");
            }

            if (link.input < 0 || link.input >= static_cast<int64>(inputs.Size())) {
                return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                                     "Invalid Mapping Input Index");
            }

            const ConvertSide& source = inputs[static_cast<usize>(link.input)];
            if (link.inputComponent < 0 || link.inputComponent >= source.components) {
                return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                                     "Invalid Mapping Input Component Index");
            }

            IrValueId& value = compiled[static_cast<usize>(link.input)];
            if (value == NoIrValue) {
                auto connected = context.DynamicInput(inputKeys[static_cast<usize>(link.input)],
                                                      Optional<float>(), false);
                if (!connected) {
                    return Result<void>::Failure(connected.Error());
                }

                value = connected.Value();
                if (value == NoIrValue) {
                    // 연결이 없으면 입력 기본값(UE Constant4(DefaultValue))이다.
                    auto constant = context.FloatConstant(
                        Span<const float>(source.defaults, MaxIrComponents), StringView());
                    if (!constant) {
                        return Result<void>::Failure(constant.Error());
                    }

                    value = constant.Value();
                }
            }

            const uint8 component[] = {static_cast<uint8>(link.inputComponent)};
            auto picked = context.Swizzle(value, Span<const uint8>(component, 1));
            if (!picked) {
                return Result<void>::Failure(picked.Error());
            }

            components[link.outputComponent] = picked.Value();
        }

        IrValueId result = NoIrValue;
        for (uint8 index = 0; index < side.components; ++index) {
            IrValueId component = components[index];
            if (component == NoIrValue) {
                auto constant = context.FloatScalar(side.defaults[index]);
                if (!constant) {
                    return Result<void>::Failure(constant.Error());
                }

                component = constant.Value();
            }

            auto asFloat = context.Convert(component, IrScalar::Float);
            if (!asFloat) {
                return Result<void>::Failure(asFloat.Error());
            }

            if (result == NoIrValue) {
                result = asFloat.Value();
                continue;
            }

            auto appended = context.Append(result, asFloat.Value());
            if (!appended) {
                return Result<void>::Failure(appended.Error());
            }

            result = appended.Value();
        }

        if (auto written = context.OutputByName(outputKeys[output], result); !written) {
            return written;
        }
    }

    return Result<void>::Success();
}

// Operator 종류 하나의 lowering 규칙이다.
enum class OperatorShape : uint8 {
    // IrOp 인자 수만큼 앞 입력, 규칙대로 맞춘다.
    Plain = 0,
    // Add·Multiply: 입력 전부를 왼쪽부터 접는다(UE VariableInputsExpressionOperators).
    Variadic = 1,
    // 행렬 연산(Transpose·MatrixMultiply): 빌더 규칙 그대로(피연산자는 Float로).
    Matrix = 2,
    // Cross: 두 입력을 Float3로 맞춘다.
    Cross = 3,
    // Dot: 스칼라 복제, 폭이 다르면 짧은 쪽으로 자른다.
    Dot = 4,
    // Distance: Length(A − B).
    Distance = 5,
    // Select: 조건(Bool) 뒤 두 값.
    Select = 6,
    // LWC 타일 연산 — 표현이 MS2b다.
    Unsupported = 7
};

struct OperatorRule {
    IrOp op;
    NumberRule rule;
    OperatorShape shape;
};

// OperatorKind 값(0–72) 순서다(UE EMaterialExpressionOperatorKind — MathNodeEnums.h).
constexpr OperatorRule OperatorRules[] = {
    {IrOp::BitwiseNot, NumberRule::IntegerOnly, OperatorShape::Plain},
    {IrOp::Negate, NumberRule::Arithmetic, OperatorShape::Plain},
    {IrOp::Not, NumberRule::Logical, OperatorShape::Plain},
    {IrOp::Abs, NumberRule::Arithmetic, OperatorShape::Plain},
    {IrOp::ACos, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::ACosFast, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::ACosh, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::ASin, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::ASinFast, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::ASinh, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::ATan, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::ATanFast, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::ATanh, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::Ceil, NumberRule::Arithmetic, OperatorShape::Plain},
    {IrOp::Cos, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::Cosh, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::Exponential, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::Exponential2, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::Floor, NumberRule::Arithmetic, OperatorShape::Plain},
    {IrOp::Frac, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::IsFinite, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::IsInf, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::IsNan, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::Length, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::Logarithm, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::Logarithm10, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::Logarithm2, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::WorldPositionTile, NumberRule::FloatOnly, OperatorShape::Unsupported},
    {IrOp::Reciprocal, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::Round, NumberRule::Arithmetic, OperatorShape::Plain},
    {IrOp::Rsqrt, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::Saturate, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::Sign, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::Sin, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::Sinh, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::Sqrt, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::Tan, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::Tanh, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::Transpose, NumberRule::FloatOnly, OperatorShape::Matrix},
    {IrOp::Truncate, NumberRule::Arithmetic, OperatorShape::Plain},
    {IrOp::Equals, NumberRule::Equality, OperatorShape::Plain},
    {IrOp::GreaterThan, NumberRule::Arithmetic, OperatorShape::Plain},
    {IrOp::GreaterThanOrEquals, NumberRule::Arithmetic, OperatorShape::Plain},
    {IrOp::LessThan, NumberRule::Arithmetic, OperatorShape::Plain},
    {IrOp::LessThanOrEquals, NumberRule::Arithmetic, OperatorShape::Plain},
    {IrOp::NotEquals, NumberRule::Equality, OperatorShape::Plain},
    {IrOp::And, NumberRule::Logical, OperatorShape::Plain},
    {IrOp::Or, NumberRule::Logical, OperatorShape::Plain},
    {IrOp::Add, NumberRule::Arithmetic, OperatorShape::Variadic},
    {IrOp::Subtract, NumberRule::Arithmetic, OperatorShape::Plain},
    {IrOp::Multiply, NumberRule::Arithmetic, OperatorShape::Variadic},
    {IrOp::MatrixMultiply, NumberRule::FloatOnly, OperatorShape::Matrix},
    {IrOp::Divide, NumberRule::Arithmetic, OperatorShape::Plain},
    {IrOp::Modulo, NumberRule::NoDouble, OperatorShape::Plain},
    {IrOp::BitwiseAnd, NumberRule::IntegerOnly, OperatorShape::Plain},
    {IrOp::BitwiseOr, NumberRule::IntegerOnly, OperatorShape::Plain},
    {IrOp::BitwiseXor, NumberRule::IntegerOnly, OperatorShape::Plain},
    {IrOp::BitShiftLeft, NumberRule::IntegerOnly, OperatorShape::Plain},
    {IrOp::BitShiftRight, NumberRule::IntegerOnly, OperatorShape::Plain},
    {IrOp::ATan2, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::ATan2Fast, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::Cross, NumberRule::FloatOnly, OperatorShape::Cross},
    {IrOp::Distance, NumberRule::FloatOnly, OperatorShape::Distance},
    {IrOp::Dot, NumberRule::FloatOnly, OperatorShape::Dot},
    {IrOp::Fmod, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::Max, NumberRule::Arithmetic, OperatorShape::Plain},
    {IrOp::Min, NumberRule::Arithmetic, OperatorShape::Plain},
    {IrOp::Pow, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::Step, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::Clamp, NumberRule::NoDouble, OperatorShape::Plain},
    {IrOp::Lerp, NumberRule::FloatOnly, OperatorShape::Plain},
    {IrOp::Select, NumberRule::Arithmetic, OperatorShape::Select},
    {IrOp::Smoothstep, NumberRule::FloatOnly, OperatorShape::Plain},
};

static_assert(sizeof(OperatorRules) / sizeof(OperatorRules[0]) ==
                  static_cast<usize>(OperatorKind::Smoothstep) + 1,
              "operator rule table must cover every operator kind");

// Operator 인자 수다(Select는 IrOp 표가 3이다).
uint8 OperatorArity(const OperatorRule& rule) noexcept {
    return rule.shape == OperatorShape::Select ? uint8{3} : IrOpArity(rule.op);
}

Result<void> LowerOperator(LoweringContext& context) noexcept {
    // UE UMaterialExpressionOperator::Build(MaterialExpressionsToMIR.cpp:3474).
    auto properties = context.Properties();
    auto pins = context.DynamicPins();
    if (!properties || !pins) {
        return Result<void>::Failure(!properties ? properties.Error() : pins.Error());
    }

    if (properties.Value() == nullptr) {
        return Result<void>::Success();
    }

    int64 kind = 0;
    auto readKind =
        context.ReadInteger("operatorKind", static_cast<int64>(OperatorKind::Add), kind);
    if (!readKind || !readKind.Value()) {
        return readKind ? Result<void>::Success() : Result<void>::Failure(readKind.Error());
    }

    if (kind < 0 || kind >= static_cast<int64>(sizeof(OperatorRules) / sizeof(OperatorRules[0]))) {
        return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                             "Unknown operator kind");
    }

    const OperatorRule& rule = OperatorRules[kind];
    if (rule.shape == OperatorShape::Unsupported) {
        return context.Error(MaterialDiagnosticCode::NotYetLowered, StringView(),
                             "LWC tile operator is not translated in this stage yet");
    }

    auto list = properties.Value()->OpenStructArray("dynamicInputs",
                                                    GeneratedType<OperatorInput>::Schema());
    if (!list) {
        return list.Error() == ErrorCode::OutOfMemory
                   ? Result<void>::Failure(list.Error())
                   : context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                                   "Operator inputs cannot be read");
    }

    SystemAllocator scratch;
    Array<IrValueId> values(scratch);
    usize index = 0;
    for (const nodegraph::PinDescriptor& pin : pins.Value()) {
        if (pin.direction != nodegraph::PinDirection::Input) {
            continue;
        }

        float constant = DefaultOperatorConstant;
        if (index < list.Value().Count()) {
            auto element = list.Value().Element(index);
            if (!element) {
                return Result<void>::Failure(element.Error());
            }

            auto read = element.Value().ReadFloat("constValue", DefaultOperatorConstant);
            if (!read) {
                return Result<void>::Failure(read.Error());
            }

            constant = read.Value();
        }

        ++index;
        auto value = context.DynamicInput(pin.key, Optional<float>(constant), false);
        if (!value) {
            return Result<void>::Failure(value.Error());
        }

        if (auto added = values.TryPushBack(value.Value()); !added) {
            return added;
        }
    }

    const uint8 arity = OperatorArity(rule);
    if (values.Size() < arity || values.IsEmpty()) {
        return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                             "Operator has fewer inputs than its operation needs");
    }

    for (IrValueId value : values) {
        if (value == NoIrValue) {
            return Result<void>::Success();
        }
    }

    switch (rule.shape) {
    case OperatorShape::Variadic: {
        IrValueId folded = values[0];
        for (usize at = 1; at < values.Size(); ++at) {
            auto next = context.Binary(rule.op, folded, values[at], rule.rule);
            if (!next) {
                return Result<void>::Failure(next.Error());
            }

            folded = next.Value();
        }

        return context.Output("Result", folded);
    }
    case OperatorShape::Matrix: {
        auto first = context.Convert(values[0], IrScalar::Float);
        if (!first) {
            return Result<void>::Failure(first.Error());
        }

        if (arity == 1) {
            return FlowEmit(context, "Result",
                            context.Unary(rule.op, first.Value(), NumberRule::FloatOnly));
        }

        auto second = context.Convert(values[1], IrScalar::Float);
        if (!second || first.Value() == NoIrValue || second.Value() == NoIrValue) {
            return second ? Result<void>::Success() : Result<void>::Failure(second.Error());
        }

        IrBuilder& builder = context.Builder();
        auto product = builder.Binary(rule.op, first.Value(), second.Value());
        if (!product) {
            return product.Error() == ErrorCode::InvalidArgument
                       ? context.Error(MaterialDiagnosticCode::TypeMismatch, StringView(),
                                       "Matrix multiply needs a row vector or matrix and a "
                                       "matrix of matching size")
                       : Result<void>::Failure(product.Error());
        }

        return context.Output("Result", product.Value());
    }
    case OperatorShape::Cross: {
        auto left = context.Coerce(values[0], MaterialPinType::Float3, "A");
        auto right = context.Coerce(values[1], MaterialPinType::Float3, "B");
        if (!left || !right) {
            return Result<void>::Failure(!left ? left.Error() : right.Error());
        }

        return FlowEmit(context, "Result",
                        context.Binary(rule.op, left.Value(), right.Value(), rule.rule));
    }
    case OperatorShape::Dot: {
        IrValueId left = values[0];
        IrValueId right = values[1];
        const uint8 leftWidth = context.TypeOf(left).components;
        const uint8 rightWidth = context.TypeOf(right).components;
        if (leftWidth > 1 && rightWidth > 1 && leftWidth != rightWidth) {
            const uint8 width = leftWidth < rightWidth ? leftWidth : rightWidth;
            auto l = context.Prefix(left, width);
            auto r = context.Prefix(right, width);
            if (!l || !r) {
                return Result<void>::Failure(!l ? l.Error() : r.Error());
            }

            left = l.Value();
            right = r.Value();
        }

        return FlowEmit(context, "Result", context.Binary(rule.op, left, right, rule.rule));
    }
    case OperatorShape::Distance: {
        auto difference =
            context.Binary(IrOp::Subtract, values[0], values[1], NumberRule::Arithmetic);
        if (!difference) {
            return Result<void>::Failure(difference.Error());
        }

        return FlowEmit(context, "Result",
                        context.Unary(IrOp::Length, difference.Value(), NumberRule::FloatOnly));
    }
    case OperatorShape::Select:
        return FlowEmit(context, "Result", context.Select(values[0], values[1], values[2]));
    case OperatorShape::Plain:
        break;
    case OperatorShape::Unsupported:
        return Result<void>::Success();
    }

    if (arity == 1) {
        return FlowEmit(context, "Result", context.Unary(rule.op, values[0], rule.rule));
    }

    if (arity == 2) {
        return FlowEmit(context, "Result",
                        context.Binary(rule.op, values[0], values[1], rule.rule));
    }

    return FlowEmit(context, "Result",
                    context.Ternary(rule.op, values[0], values[1], values[2], rule.rule));
}

Result<void> LowerNamedRerouteDeclaration(LoweringContext& context) noexcept {
    // UE Compile: 입력을 그대로 넘긴다. 입력이 없으면 값이 없다.
    auto connected = context.IsConnected("Input");
    if (!connected) {
        return Result<void>::Failure(connected.Error());
    }

    if (!connected.Value()) {
        return context.Error(MaterialDiagnosticCode::MissingInput, "Input",
                             "Named reroute declaration has no input");
    }

    auto input = context.Input("Input");
    if (!input) {
        return Result<void>::Failure(input.Error());
    }

    return context.Output("Result", input.Value());
}

Result<void> LowerNamedRerouteUsage(LoweringContext& context) noexcept {
    Guid declaration;
    auto read = context.ReadGuid("declaration", declaration);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    bool found = false;
    auto value = context.NodeOutput(declaration, NamedRerouteDeclarationType, "Result", found);
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    if (!found) {
        return context.Error(MaterialDiagnosticCode::InvalidProperty, StringView(),
                             "Invalid named reroute variable");
    }

    return context.Output("Result", value.Value());
}

constexpr LoweringEntry FlowEntries[] = {
    {"Material.DynamicBranch", &LowerDynamicBranch},
    {"Material.If", &LowerIf},
    {"Material.Switch", &LowerSwitch},
    {"Material.Convert", &LowerConvert},
    {"Material.Operator", &LowerOperator},
    {"Material.NamedRerouteDeclaration", &LowerNamedRerouteDeclaration},
    {"Material.NamedRerouteUsage", &LowerNamedRerouteUsage},
};

} // namespace

Span<const LoweringEntry> FlowLowerings() noexcept {
    return FlowEntries;
}

} // namespace rocks::material::detail
