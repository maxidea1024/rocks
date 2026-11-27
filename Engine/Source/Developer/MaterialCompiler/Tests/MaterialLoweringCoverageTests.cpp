#include <MaterialSchema/Nodes/Constants/Constant4Properties.generated.h>
#include <MaterialSchema/Nodes/Constants/Constant4Properties.h>
#include <MaterialSchema/Nodes/Math/ClampProperties.generated.h>
#include <MaterialSchema/Nodes/Math/ClampProperties.h>
#include <MaterialSchema/Nodes/Math/ComponentMaskProperties.generated.h>
#include <MaterialSchema/Nodes/Math/ComponentMaskProperties.h>
#include <MaterialSchema/Nodes/Math/OperatorProperties.generated.h>
#include <MaterialSchema/Nodes/Math/OperatorProperties.h>
#include <MaterialSchema/Nodes/Math/SineProperties.generated.h>
#include <MaterialSchema/Nodes/Math/SineProperties.h>
#include <MaterialSchema/Nodes/Utility/NamedRerouteUsageProperties.generated.h>
#include <MaterialSchema/Nodes/Utility/NamedRerouteUsageProperties.h>

#include "MaterialGraphFixture.h"

// MS2a lowering 표의 노드별 시험이다(Docs/Design/MaterialSystem 6.8절 — MS2a 리뷰 반영): 단항·이항
// 노드마다 접히지 않는 입력(sin)에서 결과 연산을 보고, UE 기본 번역기와 맞춘 규칙(첫 인자 타입·
// CoerceParameter·Power·Lerp·Clamp 지름길·ComponentMask 스칼라 복제), 오류가 아래로 번지지
// 않음(오류 정확히 하나), 비활성 노드 뒤쪽을 번역하지 않음, 이름 있는 재경유 순환을 본다.
using namespace rocks;
using namespace rocks::material;
namespace material_lowering_coverage_tests_file {

using namespace material_graph_fixture;

namespace {

// AO 결과의 IR 값이다(없으면 nullptr).
const IrValue* AoValue(const MaterialTranslation& out) noexcept {
    const IrResult* result = FindResult(out, "AmbientOcclusion");
    return result == nullptr ? nullptr : &ValueOf(out, result->value);
}

// 결과 칸 slot 값의 타입이다(없으면 Bool 4 — 어떤 시험 기대와도 다른 값).
IrType SlotType(const MaterialTranslation& out, StringView slot) noexcept {
    const IrResult* result = FindResult(out, slot);
    return result == nullptr ? IrBool(4) : ValueOf(out, result->value).type;
}

// 접히지 않는 float3 값(float3(sin(a), sin(b), sin(c)))이다.
Guid AddVaryingVector3(GraphFixture& fixture) noexcept {
    const Guid pair = fixture.Add("Material.Append");
    fixture.Connect(AddVarying(fixture, 0.1f), "Result", pair, "A");
    fixture.Connect(AddVarying(fixture, 0.2f), "Result", pair, "B");
    const Guid triple = fixture.Add("Material.Append");
    fixture.Connect(pair, "Result", triple, "A");
    fixture.Connect(AddVarying(fixture, 0.3f), "Result", triple, "B");
    return triple;
}

// 접히지 않는 float2 값이다.
Guid AddVaryingVector2(GraphFixture& fixture) noexcept {
    const Guid pair = fixture.Add("Material.Append");
    fixture.Connect(AddVarying(fixture, 0.4f), "Result", pair, "A");
    fixture.Connect(AddVarying(fixture, 0.5f), "Result", pair, "B");
    return pair;
}

OperatorProperties OperatorOf(Allocator& allocator, OperatorKind kind, usize inputs) noexcept {
    OperatorProperties properties(allocator);
    properties.operatorKind = kind;
    for (usize index = 0; index < inputs; ++index) {
        (void)properties.dynamicInputs.TryPushBack(OperatorInput{});
    }

    return properties;
}

} // namespace

// 단항 노드: 접히지 않는 입력에서 결과 연산이 lowering 표와 같다.
TEST(MaterialLowering_UnaryNodes) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    struct Case {
        StringView type;
        StringView pin;
        IrOp op;
    };

    const Case cases[] = {
        {"Material.Abs", "Input", IrOp::Abs},
        {"Material.Floor", "Input", IrOp::Floor},
        {"Material.Ceil", "Input", IrOp::Ceil},
        {"Material.Round", "Input", IrOp::Round},
        {"Material.Truncate", "Input", IrOp::Truncate},
        {"Material.Frac", "Input", IrOp::Frac},
        {"Material.Saturate", "Input", IrOp::Saturate},
        {"Material.Sign", "Input", IrOp::Sign},
        {"Material.SquareRoot", "Input", IrOp::Sqrt},
        {"Material.Exponential", "Input", IrOp::Exponential},
        {"Material.Exponential2", "Input", IrOp::Exponential2},
        {"Material.Logarithm", "Input", IrOp::Logarithm},
        {"Material.Logarithm2", "X", IrOp::Logarithm2},
        {"Material.Logarithm10", "X", IrOp::Logarithm10},
        {"Material.Arcsine", "Input", IrOp::ASin},
        {"Material.Arccosine", "Input", IrOp::ACos},
        {"Material.Arctangent", "Input", IrOp::ATan},
        {"Material.ArcsineFast", "Input", IrOp::ASinFast},
        {"Material.ArccosineFast", "Input", IrOp::ACosFast},
        {"Material.ArctangentFast", "Input", IrOp::ATanFast},
        {"Material.Normalize", "VectorInput", IrOp::Normalize},
        // 스칼라 Length는 abs다(UE 기본 번역기 최적화).
        {"Material.Length", "Input", IrOp::Abs},
    };
    for (const Case& testCase : cases) {
        GraphFixture fixture(env);
        const Guid node = fixture.Add(testCase.type);
        fixture.Connect(AddVarying(fixture, 0.5f), "Result", node, testCase.pin);
        fixture.Connect(node, "Result", OutputNodeId, "AmbientOcclusion");
        REQUIRE(fixture.ready);
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(out.succeeded);
        const IrValue* value = AoValue(out);
        CHECK(value != nullptr && value->op == testCase.op);
    }

    // Tangent는 주기 기본 1이라 곱한 뒤 tan이다.
    GraphFixture tangent(env);
    const Guid node = tangent.Add("Material.Tangent");
    tangent.Connect(AddVarying(tangent, 0.5f), "Result", node, "Input");
    tangent.Connect(node, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(tangent.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(tangent, Traits(), out));
    const IrValue* tan = AoValue(out);
    REQUIRE(tan != nullptr);
    CHECK(tan->op == IrOp::Tan);
    CHECK(ValueOf(out, tan->operands[0]).op == IrOp::Multiply);
}

// 이항·삼항 노드: 접히지 않는 입력에서 결과 연산이 표와 같다.
TEST(MaterialLowering_BinaryNodes) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    struct Case {
        StringView type;
        StringView first;
        StringView second;
        IrOp op;
    };

    const Case cases[] = {
        {"Material.Add", "A", "B", IrOp::Add},
        {"Material.Subtract", "A", "B", IrOp::Subtract},
        {"Material.Multiply", "A", "B", IrOp::Multiply},
        {"Material.Divide", "A", "B", IrOp::Divide},
        {"Material.Min", "A", "B", IrOp::Min},
        {"Material.Max", "A", "B", IrOp::Max},
        {"Material.Fmod", "A", "B", IrOp::Fmod},
        {"Material.Modulo", "A", "B", IrOp::Modulo},
        {"Material.Step", "Y", "X", IrOp::Step},
        {"Material.Arctangent2", "Y", "X", IrOp::ATan2},
        {"Material.Arctangent2Fast", "Y", "X", IrOp::ATan2Fast},
        {"Material.DotProduct", "A", "B", IrOp::Dot},
        {"Material.LinearInterpolate", "A", "B", IrOp::Lerp},
        {"Material.SmoothStep", "Min", "Max", IrOp::Smoothstep},
        // 스칼라 Distance는 abs(A − B)다.
        {"Material.Distance", "A", "B", IrOp::Abs},
    };
    for (const Case& testCase : cases) {
        GraphFixture fixture(env);
        const Guid node = fixture.Add(testCase.type);
        fixture.Connect(AddVarying(fixture, 0.5f), "Result", node, testCase.first);
        fixture.Connect(AddVarying(fixture, 0.25f), "Result", node, testCase.second);
        fixture.Connect(node, "Result", OutputNodeId, "AmbientOcclusion");
        REQUIRE(fixture.ready);
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(out.succeeded);
        const IrValue* value = AoValue(out);
        CHECK(value != nullptr && value->op == testCase.op);
    }

    // ConstantBiasScale은 (x + bias) × scale, Cross는 float3 두 개다.
    GraphFixture bias(env);
    const Guid scale = bias.Add("Material.ConstantBiasScale");
    bias.Connect(AddVarying(bias, 0.5f), "Result", scale, "Input");
    bias.Connect(scale, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(bias.ready);
    MaterialTranslation scaled(env.allocator);
    REQUIRE(Translate(bias, Traits(), scaled));
    const IrValue* multiply = AoValue(scaled);
    REQUIRE(multiply != nullptr);
    CHECK(multiply->op == IrOp::Multiply);
    CHECK(ValueOf(scaled, multiply->operands[0]).op == IrOp::Add);

    GraphFixture cross(env);
    const Guid product = cross.Add("Material.CrossProduct");
    cross.Connect(AddVaryingVector3(cross), "Result", product, "A");
    cross.Connect(AddVaryingVector3(cross), "Result", product, "B");
    cross.Connect(product, "Result", OutputNodeId, "WorldPositionOffset");
    REQUIRE(cross.ready);
    MaterialTranslation crossed(env.allocator);
    REQUIRE(Translate(cross, Traits(), crossed));
    REQUIRE(crossed.succeeded);
    CHECK(ValueOf(crossed, FindResult(crossed, "WorldPositionOffset")->value).op == IrOp::Cross);
}

// Constant4의 성분 출력 R·G·B·A다.
TEST(MaterialLowering_Constant4Components) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    const StringView keys[] = {"R", "G", "B", "A"};
    const float expected[] = {0.125f, 0.25f, 0.5f, 0.75f};
    for (usize index = 0; index < 4; ++index) {
        GraphFixture fixture(env);
        Constant4Properties color;
        color.constant = LinearColor(expected[0], expected[1], expected[2], expected[3]);
        const Guid node = fixture.Add("Material.Constant4", env.Save(color));
        fixture.Connect(node, keys[index], OutputNodeId, "AmbientOcclusion");
        REQUIRE(fixture.ready);
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        const float one[] = {expected[index]};
        CHECK(IsFloatResult(out, "AmbientOcclusion", one));
    }
}

// 오류가 아래로 번지지 않는다: 위쪽 노드 오류 하나면 진단도 하나다(Graph.Reroute·이름 있는 재경유
// 포함).
TEST(MaterialLowering_ErrorsDoNotCascade) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid broken = fixture.Add("Material.Sine");
    const Guid abs = fixture.Add("Material.Abs");
    fixture.Connect(broken, "Result", abs, "Input");
    const Guid reroute = fixture.Add(nodegraph::RerouteNodeTypeName);
    fixture.Connect(abs, "Result", reroute, nodegraph::ReroutePinIn);
    const Guid declaration = fixture.Add("Material.NamedRerouteDeclaration");
    fixture.Connect(reroute, nodegraph::ReroutePinOut, declaration, "Input");
    NamedRerouteUsageProperties usage;
    usage.declaration = declaration;
    const Guid use = fixture.Add("Material.NamedRerouteUsage", env.Save(usage));
    fixture.Connect(use, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    CHECK(!out.succeeded);
    CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::MissingInput, broken));

    // 이름 있는 재경유 순환(선언 ← 쓰임)은 V2 하나다.
    GraphFixture loop(env);
    const Guid decl = loop.Add("Material.NamedRerouteDeclaration");
    NamedRerouteUsageProperties back;
    back.declaration = decl;
    const Guid readBack = loop.Add("Material.NamedRerouteUsage", env.Save(back));
    const Guid add = loop.Add("Material.Add");
    loop.Connect(readBack, "Result", add, "A");
    loop.Connect(add, "Result", decl, "Input");
    loop.Connect(readBack, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(loop.ready);
    MaterialTranslation cycle(env.allocator);
    REQUIRE(Translate(loop, Traits(), cycle));
    CHECK(!cycle.succeeded);
    CHECK(CountErrors(cycle) == 1);
    CHECK(HasError(cycle, MaterialDiagnosticCode::Cycle));
}

// 비활성 노드는 연결이 없는 것과 같고 그 위쪽은 번역하지 않는다(통과 쌍이 없는 노드 —
// NodeGraph 6.5절). If의 AEqualsB가 비활성 노드에 닿으면 같음 가지가 없는 것이다.
TEST(MaterialLowering_DisabledNodes) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid unknown = fixture.Add("Material.DoesNotExist");
    const Guid off = fixture.Add("Material.Add");
    fixture.Connect(unknown, "Result", off, "A");
    fixture.Find(off)->enableState = nodegraph::NodeEnableState::Disabled;
    fixture.Connect(off, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    CHECK(out.succeeded);
    CHECK(out.diagnostics.items.IsEmpty());
    const float one[] = {1.0f};
    CHECK(IsFloatResult(out, "AmbientOcclusion", one));

    GraphFixture equals(env);
    const Guid choice = equals.Add("Material.If");
    equals.Connect(AddConstant(equals, 1.0f), "Result", choice, "A");
    equals.SetValue(choice, "B", PinValue(1.0f, 1));
    equals.Connect(AddConstant(equals, 10.0f), "Result", choice, "AGreaterThanB");
    equals.Connect(AddConstant(equals, 20.0f), "Result", choice, "ALessThanB");
    const Guid disabled = AddConstant(equals, 30.0f);
    equals.Find(disabled)->enableState = nodegraph::NodeEnableState::Disabled;
    equals.Connect(disabled, "Result", choice, "AEqualsB");
    equals.Connect(choice, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(equals.ready);
    MaterialTranslation noEquals(env.allocator);
    REQUIRE(Translate(equals, Traits(), noEquals));
    REQUIRE(noEquals.succeeded);
    const float greater[] = {10.0f};
    CHECK(IsFloatResult(noEquals, "AmbientOcclusion", greater));
}

// Power(UE HLSLMaterialTranslator.cpp:9819): 결과는 Base 타입, 지수는 첫 성분, 상수 지수 1·0과 상수
// 밑 0은 지름길이다.
TEST(MaterialLowering_PowerFollowsClassicTranslator) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid power = fixture.Add("Material.Power");
    fixture.Connect(AddVaryingVector3(fixture), "Result", power, "Base");
    fixture.Connect(AddVaryingVector2(fixture), "Result", power, "Exponent");
    fixture.Connect(power, "Result", OutputNodeId, "WorldPositionOffset");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    CHECK(SlotType(out, "WorldPositionOffset") == IrFloat(3));
    const IrValue& select = ValueOf(out, FindResult(out, "WorldPositionOffset")->value);
    REQUIRE(select.op == IrOp::Select);
    const IrValue& pow = ValueOf(out, select.operands[2]);
    REQUIRE(pow.op == IrOp::Pow);
    // 지수는 float2의 첫 성분을 복제한 float3다.
    const IrValue& exponent = ValueOf(out, pow.operands[1]);
    CHECK(exponent.op == IrOp::Splat);
    CHECK(ValueOf(out, exponent.operands[0]).op == IrOp::Swizzle);

    struct Case {
        float exponent;
        bool zeroBase;
        IrOp root;
    };

    // 지수 1 → Base 그대로(sin), 지수 0 → 상수 1, 밑 0 → 상수 0.
    const Case cases[] = {
        {1.0f, false, IrOp::Sin}, {0.0f, false, IrOp::Constant}, {3.0f, true, IrOp::Constant}};
    for (const Case& testCase : cases) {
        GraphFixture shortcut(env);
        const Guid node = shortcut.Add("Material.Power");
        if (testCase.zeroBase) {
            shortcut.Connect(AddConstant(shortcut, 0.0f), "Result", node, "Base");
        } else {
            shortcut.Connect(AddVarying(shortcut, 0.5f), "Result", node, "Base");
        }

        shortcut.SetValue(node, "Exponent", PinValue(testCase.exponent, 1));
        shortcut.Connect(node, "Result", OutputNodeId, "AmbientOcclusion");
        REQUIRE(shortcut.ready);
        MaterialTranslation shortOut(env.allocator);
        REQUIRE(Translate(shortcut, Traits(), shortOut));
        REQUIRE(shortOut.succeeded);
        const IrValue* value = AoValue(shortOut);
        REQUIRE(value != nullptr);
        CHECK(value->op == testCase.root);
        if (testCase.root == IrOp::Constant) {
            CHECK(FloatOf(*value, 0) == (testCase.zeroBase ? 0.0f : 1.0f));
        }
    }
}

// 첫 인자 타입(UE CoerceParameter): Clamp·Fmod는 결과가 첫 입력 타입이고 다른 입력을 맞춘다. 폭이
// 다른 두 벡터는 V1이다(Dot도). Clamp(0, 1)은 saturate다.
TEST(MaterialLowering_FirstOperandTypes) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    {
        GraphFixture fixture(env);
        const Guid clamp = fixture.Add("Material.Clamp");
        fixture.Connect(AddVaryingVector3(fixture), "Result", clamp, "Input");
        fixture.Connect(clamp, "Result", OutputNodeId, "WorldPositionOffset");
        REQUIRE(fixture.ready);
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        REQUIRE(out.succeeded);
        CHECK(ValueOf(out, FindResult(out, "WorldPositionOffset")->value).op == IrOp::Saturate);
    }

    struct ModeCase {
        ClampMode mode;
        IrOp op;
    };

    const ModeCase modes[] = {{ClampMode::Clamp, IrOp::Clamp},
                              {ClampMode::ClampMin, IrOp::Max},
                              {ClampMode::ClampMax, IrOp::Min}};
    for (const ModeCase& modeCase : modes) {
        GraphFixture fixture(env);
        ClampProperties properties;
        properties.clampMode = modeCase.mode;
        const Guid clamp = fixture.Add("Material.Clamp", env.Save(properties));
        fixture.Connect(AddVarying(fixture, 0.5f), "Result", clamp, "Input");
        fixture.SetValue(clamp, "Min", PinValue(0.25f, 1));
        fixture.SetValue(clamp, "Max", PinValue(0.75f, 1));
        fixture.Connect(clamp, "Result", OutputNodeId, "AmbientOcclusion");
        REQUIRE(fixture.ready);
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        REQUIRE(out.succeeded);
        const IrValue* value = AoValue(out);
        CHECK(value != nullptr && value->op == modeCase.op);
    }

    {
        // Fmod(float3, scalar) = float3, Fmod(scalar, float3) = 스칼라(B의 첫 성분).
        GraphFixture fixture(env);
        const Guid wide = fixture.Add("Material.Fmod");
        fixture.Connect(AddVaryingVector3(fixture), "Result", wide, "A");
        fixture.Connect(AddVarying(fixture, 0.7f), "Result", wide, "B");
        fixture.Connect(wide, "Result", OutputNodeId, "WorldPositionOffset");
        const Guid narrow = fixture.Add("Material.Fmod");
        fixture.Connect(AddVarying(fixture, 0.7f), "Result", narrow, "A");
        fixture.Connect(AddVaryingVector3(fixture), "Result", narrow, "B");
        fixture.Connect(narrow, "Result", OutputNodeId, "AmbientOcclusion");
        REQUIRE(fixture.ready);
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        REQUIRE(out.succeeded);
        CHECK(SlotType(out, "WorldPositionOffset") == IrFloat(3));
        CHECK(SlotType(out, "AmbientOcclusion") == IrFloat(1));
        CHECK(CountDiagnostics(out, MaterialDiagnosticCode::NarrowedValue,
                               nodegraph::GraphDiagnosticSeverity::Info) == 0);
    }

    {
        GraphFixture fixture(env);
        const Guid dot = fixture.Add("Material.DotProduct");
        fixture.Connect(AddVaryingVector3(fixture), "Result", dot, "A");
        fixture.Connect(AddVaryingVector2(fixture), "Result", dot, "B");
        fixture.Connect(dot, "Result", OutputNodeId, "AmbientOcclusion");
        REQUIRE(fixture.ready);
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::TypeMismatch, dot));
    }
}

// Lerp(UE :10367): A = B면 A, 상수 alpha 0·1이면 A·B 그대로다. InvLerp에서 A = B = Value면 0이다.
TEST(MaterialLowering_LerpShortcuts) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    struct Case {
        float alpha;
        IrOp root;
        float first;
    };

    const Case cases[] = {
        {0.0f, IrOp::Sin, 0.5f}, {1.0f, IrOp::Sin, 0.25f}, {0.5f, IrOp::Lerp, 0.0f}};
    for (const Case& testCase : cases) {
        GraphFixture fixture(env);
        const Guid lerp = fixture.Add("Material.LinearInterpolate");
        const Guid a = AddVarying(fixture, 0.5f);
        const Guid b = AddVarying(fixture, 0.25f);
        fixture.Connect(a, "Result", lerp, "A");
        fixture.Connect(b, "Result", lerp, "B");
        fixture.SetValue(lerp, "Alpha", PinValue(testCase.alpha, 1));
        fixture.Connect(lerp, "Result", OutputNodeId, "AmbientOcclusion");
        REQUIRE(fixture.ready);
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        const IrValue* value = AoValue(out);
        REQUIRE(value != nullptr);
        CHECK(value->op == testCase.root);
        if (testCase.root == IrOp::Sin) {
            CHECK(FloatOf(ValueOf(out, value->operands[0]), 0) == testCase.first);
        }
    }

    GraphFixture same(env);
    const Guid inverse = same.Add("Material.InverseLinearInterpolate");
    same.SetValue(inverse, "A", PinValue(2.0f, 1));
    same.SetValue(inverse, "B", PinValue(2.0f, 1));
    same.SetValue(inverse, "Value", PinValue(2.0f, 1));
    same.Connect(inverse, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(same.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(same, Traits(), out));
    REQUIRE(out.succeeded);
    const float zero[] = {0.0f};
    CHECK(IsFloatResult(out, "AmbientOcclusion", zero));
}

// ComponentMask 스칼라 입력은 g·b·a도 .r로 복제한다(UE :10776). DynamicBranch 상수 조건은 공통
// 타입이다.
TEST(MaterialLowering_ScalarMaskAndBranchType) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    ComponentMaskProperties mask;
    mask.g = true;
    mask.b = true;
    const Guid node = fixture.Add("Material.ComponentMask", env.Save(mask));
    fixture.Connect(AddVarying(fixture, 0.5f), "Result", node, "Input");
    const Guid append = fixture.Add("Material.Append");
    fixture.Connect(node, "Result", append, "A");
    fixture.Connect(AddConstant(fixture, 1.0f), "Result", append, "B");
    fixture.Connect(append, "Result", OutputNodeId, "WorldPositionOffset");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    CHECK(SlotType(out, "WorldPositionOffset") == IrFloat(3));

    // 상수 참 조건, True float, False float3 → 결과 float3(공통 타입으로 복제).
    GraphFixture branch(env);
    OperatorProperties greater(env.allocator);
    greater.operatorKind = OperatorKind::GreaterThan;
    OperatorInput two;
    two.constValue = 2.0f;
    REQUIRE(greater.dynamicInputs.TryPushBack(two));
    REQUIRE(greater.dynamicInputs.TryPushBack(OperatorInput{}));
    const Guid condition = branch.Add("Material.Operator", env.Save(greater));
    const Guid choose = branch.Add("Material.DynamicBranch");
    branch.Connect(condition, "Result", choose, "Condition");
    branch.Connect(AddConstant(branch, 0.5f), "Result", choose, "True");
    branch.Connect(AddVaryingVector3(branch), "Result", choose, "False");
    branch.Connect(choose, "Result", OutputNodeId, "WorldPositionOffset");
    REQUIRE(branch.ready);
    MaterialTranslation branched(env.allocator);
    REQUIRE(Translate(branch, Traits(), branched));
    REQUIRE(branched.succeeded);
    const float halves[] = {0.5f, 0.5f, 0.5f};
    CHECK(IsFloatResult(branched, "WorldPositionOffset", halves));
}

// Operator: Multiply 가변 접기, Select, Distance, Dot, 행렬 연산의 비행렬 입력(V1).
TEST(MaterialLowering_OperatorShapes) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    {
        GraphFixture fixture(env);
        OperatorProperties product(env.allocator);
        product.operatorKind = OperatorKind::Multiply;
        const float constants[] = {2.0f, 3.0f, 4.0f};
        for (float value : constants) {
            OperatorInput input;
            input.constValue = value;
            REQUIRE(product.dynamicInputs.TryPushBack(input));
        }

        const Guid node = fixture.Add("Material.Operator", env.Save(product));
        fixture.Connect(node, "Result", OutputNodeId, "AmbientOcclusion");
        REQUIRE(fixture.ready);
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        const float twentyFour[] = {24.0f};
        CHECK(IsFloatResult(out, "AmbientOcclusion", twentyFour));
    }

    struct Case {
        OperatorKind kind;
        usize inputs;
        IrOp root;
    };

    const Case cases[] = {{OperatorKind::Distance, 2, IrOp::Length},
                          {OperatorKind::Dot, 2, IrOp::Dot},
                          {OperatorKind::Sin, 1, IrOp::Sin},
                          {OperatorKind::Clamp, 3, IrOp::Clamp}};
    for (const Case& testCase : cases) {
        GraphFixture fixture(env);
        const Guid node =
            fixture.Add("Material.Operator",
                        env.Save(OperatorOf(env.allocator, testCase.kind, testCase.inputs)));
        fixture.Connect(AddVarying(fixture, 0.5f), "Result", node, "A");
        fixture.Connect(node, "Result", OutputNodeId, "AmbientOcclusion");
        REQUIRE(fixture.ready);
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(out.succeeded);
        const IrValue* value = AoValue(out);
        CHECK(value != nullptr && value->op == testCase.root);
    }

    {
        // Select(조건, 값, 값): 조건은 비교 결과다.
        GraphFixture fixture(env);
        const Guid compare = fixture.Add(
            "Material.Operator", env.Save(OperatorOf(env.allocator, OperatorKind::LessThan, 2)));
        fixture.Connect(AddVarying(fixture, 0.5f), "Result", compare, "A");
        const Guid select = fixture.Add(
            "Material.Operator", env.Save(OperatorOf(env.allocator, OperatorKind::Select, 3)));
        fixture.Connect(compare, "Result", select, "A");
        fixture.Connect(AddVarying(fixture, 0.25f), "Result", select, "B");
        fixture.Connect(AddConstant(fixture, 0.75f), "Result", select, "C");
        fixture.Connect(select, "Result", OutputNodeId, "AmbientOcclusion");
        REQUIRE(fixture.ready);
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        REQUIRE(out.succeeded);
        const IrValue* value = AoValue(out);
        REQUIRE(value != nullptr);
        CHECK(value->op == IrOp::Select);
        CHECK(ValueOf(out, value->operands[0]).op == IrOp::LessThan);
    }

    {
        // 행렬 연산(Transpose)에 행렬이 아닌 값은 V1 하나다.
        GraphFixture fixture(env);
        const Guid transpose = fixture.Add(
            "Material.Operator", env.Save(OperatorOf(env.allocator, OperatorKind::Transpose, 1)));
        fixture.Connect(AddVarying(fixture, 0.5f), "Result", transpose, "A");
        fixture.Connect(transpose, "Result", OutputNodeId, "AmbientOcclusion");
        REQUIRE(fixture.ready);
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::TypeMismatch, transpose));
    }
}

} // namespace material_lowering_coverage_tests_file
