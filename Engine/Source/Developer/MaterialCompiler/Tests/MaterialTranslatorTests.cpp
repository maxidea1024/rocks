#include "MaterialGraphFixture.h"

// 머티리얼 그래프 → IR 번역 시험이다(Docs/Design/MaterialSystem 6.8절, 단계 MS2a2). 노드 속성과 핀
// 사용자 값을 리플렉션 codec으로 Record에 써서(실제 wire) 그래프를 만들고, 진입점 결과의 IR 값과
// 진단 코드(V1·V2·V4·V5·V8·V11·V12·번역 코드)를 본다. MS2a 노드에는 값이 바뀌는 입력이 없어 상수가
// 접히므로, 접히지 않는 값은 초월 함수(sin)로 만든다(6.8절 상수 접기 범위).
using namespace rocks;
using namespace rocks::material;
namespace material_translator_tests_file {

using namespace material_graph_fixture;

// 출력 노드만 있는 Surface·Opaque: 활성 핀(WPO·이전 WPO·AO·PDO)이 기본값으로 진입점에 나간다(7.3절
// "쓰지 않는 함수는 기본값"). Opacity·OpacityMask·Refraction·SurfaceThickness는 비활성이다.
TEST(MaterialTranslator_OutputDefaults) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    CHECK(out.diagnostics.items.IsEmpty());
    // 표면이 없어도 표면 칸 셋(정점 노멀·tangent·DefaultLit)과 대표 노멀이 나간다(6.11절 MS2d1).
    REQUIRE(out.module.Results().Size() == 8);
    const float zero3[] = {0.0f, 0.0f, 0.0f};
    const float one[] = {1.0f};
    const float zero[] = {0.0f};
    CHECK(IsFloatResult(out, "WorldPositionOffset", zero3));
    CHECK(IsFloatResult(out, "AmbientOcclusion", one));
    CHECK(IsFloatResult(out, "PixelDepthOffset", zero));
    // WPO가 있으면 이전 정점 문맥도 번역한다(6.9절 — UE HLSLMaterialTranslator.cpp:1663–1667).
    CHECK(out.module.Results()[0].entry == IrEntry::Vertex);
    CHECK(out.module.Results()[1].entry == IrEntry::VertexPrevious);
    usize surfaceFields = 0;
    usize normals = 0;
    for (const IrResult& result : out.module.Results()) {
        surfaceFields += result.entry == IrEntry::Surface ? 1 : 0;
        normals += result.entry == IrEntry::Normal ? 1 : 0;
    }

    // AO와 표면 기본 칸 셋(Normal·Tangent·ShadingModel), 대표 노멀 하나다.
    CHECK(surfaceFields == 4);
    CHECK(normals == 1);

    // Masked면 OpacityMask(기본 1)가, AlphaComposite면 Opacity가 더해진다. 반투명은 PDO가 빠진다.
    MaterialTranslation masked(env.allocator);
    REQUIRE(Translate(fixture, Traits(BlendMode::Masked), masked));
    CHECK(IsFloatResult(masked, "OpacityMask", one));
    MaterialTranslation composite(env.allocator);
    REQUIRE(Translate(fixture, Traits(BlendMode::AlphaComposite), composite));
    CHECK(IsFloatResult(composite, "Opacity", one));
    CHECK(FindResult(composite, "PixelDepthOffset") == nullptr);
    MaterialTraitsDesc velocity = Traits(BlendMode::AlphaComposite);
    velocity.outputTranslucentVelocity = true;
    MaterialTranslation withVelocity(env.allocator);
    REQUIRE(Translate(fixture, velocity, withVelocity));
    CHECK(FindResult(withVelocity, "PixelDepthOffset") != nullptr);

    // 굴절은 반투명 ∧ 왜곡 ∧ IOR-from-F0 아님, 두께는 thinSurface일 때만이다.
    MaterialTraitsDesc refractive = Traits(BlendMode::Translucent);
    refractive.usesDistortion = true;
    refractive.refractionMethod = RefractionMethod::IndexOfRefraction;
    refractive.thinSurface = true;
    MaterialTranslation refraction(env.allocator);
    REQUIRE(Translate(fixture, refractive, refraction));
    const float refractionDefault[] = {1.0f, 0.0f, 0.0f};
    const float thickness[] = {0.01f};
    CHECK(IsFloatResult(refraction, "Refraction", refractionDefault));
    CHECK(IsFloatResult(refraction, "SurfaceThickness", thickness));
    refractive.refractionMethod = RefractionMethod::IndexOfRefractionFromF0;
    MaterialTranslation fromF0(env.allocator);
    REQUIRE(Translate(fixture, refractive, fromF0));
    CHECK(FindResult(fromF0, "Refraction") == nullptr);

    // PostProcess·LightFunction 영역은 MS2a 핀을 쓰지 않는다(Opacity는 AlphaComposite만). 표면 기본
    // 칸과 대표 노멀만 나간다(6.11절 MS2d1).
    MaterialTranslation light(env.allocator);
    REQUIRE(Translate(fixture, Traits(BlendMode::Opaque, MaterialDomain::LightFunction), light));
    CHECK(light.succeeded);
    for (const IrResult& result : light.module.Results()) {
        CHECK((result.entry == IrEntry::Surface || result.entry == IrEntry::Normal));
        CHECK(result.slot != StringView("AmbientOcclusion"));
    }
}

// 상수·산술은 빌더가 접는다(정확히 반올림되는 연산 — 6.8절).
TEST(MaterialTranslator_ConstantArithmeticFolds) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid two = AddConstant(fixture, 2.0f);
    const Guid three = AddConstant(fixture, 3.0f);
    const Guid add = fixture.Add("Material.Add");
    fixture.Connect(two, "Result", add, "A");
    fixture.Connect(three, "Result", add, "B");
    fixture.Connect(add, "Result", OutputNodeId, "OpacityMask");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(BlendMode::Masked), out));
    REQUIRE(out.succeeded);
    const float five[] = {5.0f};
    CHECK(IsFloatResult(out, "OpacityMask", five));

    // 연결이 없는 입력은 정의 기본값(Add B = 1)이다.
    GraphFixture defaults(env);
    const Guid lone = AddConstant(defaults, 2.0f);
    const Guid plusOne = defaults.Add("Material.Add");
    defaults.Connect(lone, "Result", plusOne, "A");
    defaults.Connect(plusOne, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(defaults.ready);
    MaterialTranslation defaulted(env.allocator);
    REQUIRE(Translate(defaults, Traits(), defaulted));
    const float threeValue[] = {3.0f};
    CHECK(IsFloatResult(defaulted, "AmbientOcclusion", threeValue));
}

// 핀 사용자 값: 연결 없는 입력은 사용자 값 → 정의 기본값 순이다. 폭·유한성 규칙을 어기면 진단이다.
TEST(MaterialTranslator_PinValues) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid add = fixture.Add("Material.Add");
    fixture.SetValue(add, "A", PinValue(4.0f, 1));
    fixture.SetValue(add, "B", PinValue(0.5f, 1));
    fixture.Connect(add, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    const float sum[] = {4.5f};
    CHECK(IsFloatResult(out, "AmbientOcclusion", sum));

    // 출력 노드 핀 값(WPO float3 = (2,2,2)).
    GraphFixture output(env);
    output.SetValue(OutputNodeId, "WorldPositionOffset", PinValue(2.0f, 3));
    REQUIRE(output.ready);
    MaterialTranslation offset(env.allocator);
    REQUIRE(Translate(output, Traits(), offset));
    const float twos[] = {2.0f, 2.0f, 2.0f};
    CHECK(IsFloatResult(offset, "WorldPositionOffset", twos));

    // 성분 수 0·5와 Float1 핀에 2성분은 InvalidPinValue, Inf는 V11이다.
    const MaterialPinValue invalid[] = {PinValue(1.0f, 0), PinValue(1.0f, 5), PinValue(1.0f, 2)};
    for (const MaterialPinValue& value : invalid) {
        GraphFixture bad(env);
        bad.SetValue(OutputNodeId, "AmbientOcclusion", value);
        REQUIRE(bad.ready);
        MaterialTranslation rejected(env.allocator);
        REQUIRE(Translate(bad, Traits(), rejected));
        CHECK(!rejected.succeeded);
        CHECK(HasOnlyErrorAt(rejected, MaterialDiagnosticCode::InvalidPinValue, OutputNodeId));
        CHECK(rejected.module.Results().IsEmpty());
    }

    GraphFixture infinite(env);
    MaterialPinValue huge = PinValue(1.0f, 1);
    huge.x = 1.0f / 0.0f;
    infinite.SetValue(OutputNodeId, "AmbientOcclusion", huge);
    REQUIRE(infinite.ready);
    MaterialTranslation nonFinite(env.allocator);
    REQUIRE(Translate(infinite, Traits(), nonFinite));
    CHECK(HasError(nonFinite, MaterialDiagnosticCode::NonFiniteConstant));
}

// 출력 노드 핀 변환(5.3절): 긴 벡터는 앞 성분(정보), 짧은 벡터 → 긴 핀은 V1, 스칼라는 복제다.
TEST(MaterialTranslator_OutputCoercion) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    Constant4Properties color;
    color.constant = LinearColor(0.25f, 0.5f, 0.75f, 1.0f);
    const Guid rgba = fixture.Add("Material.Constant4", env.Save(color));
    fixture.Connect(rgba, "RGBA", OutputNodeId, "AmbientOcclusion");
    fixture.Connect(rgba, "RGB", OutputNodeId, "WorldPositionOffset");
    const Guid scalar = AddConstant(fixture, 0.125f);
    fixture.Connect(scalar, "Result", OutputNodeId, "PixelDepthOffset");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    const float first[] = {0.25f};
    const float rgb[] = {0.25f, 0.5f, 0.75f};
    const float pdo[] = {0.125f};
    CHECK(IsFloatResult(out, "AmbientOcclusion", first));
    CHECK(IsFloatResult(out, "WorldPositionOffset", rgb));
    CHECK(IsFloatResult(out, "PixelDepthOffset", pdo));
    CHECK(CountDiagnostics(out, MaterialDiagnosticCode::NarrowedValue,
                           nodegraph::GraphDiagnosticSeverity::Info) == 1);

    // 스칼라는 float3 핀에 복제되고, float2는 float3 핀에 V1이다.
    GraphFixture widen(env);
    const Guid half = AddConstant(widen, 0.5f);
    widen.Connect(half, "Result", OutputNodeId, "WorldPositionOffset");
    REQUIRE(widen.ready);
    MaterialTranslation splat(env.allocator);
    REQUIRE(Translate(widen, Traits(), splat));
    const float halves[] = {0.5f, 0.5f, 0.5f};
    CHECK(IsFloatResult(splat, "WorldPositionOffset", halves));

    GraphFixture narrow(env);
    Constant2Properties pair;
    pair.r = 1.0f;
    pair.g = 2.0f;
    const Guid two = narrow.Add("Material.Constant2", env.Save(pair));
    narrow.Connect(two, "Result", OutputNodeId, "WorldPositionOffset");
    REQUIRE(narrow.ready);
    MaterialTranslation shorter(env.allocator);
    REQUIRE(Translate(narrow, Traits(), shorter));
    CHECK(!shorter.succeeded);
    CHECK(HasOnlyErrorAt(shorter, MaterialDiagnosticCode::TypeMismatch, OutputNodeId));
}

// 상수 노드의 성분 출력: Constant2·3·4의 R·G·B·A는 성분 하나, RGB는 앞 셋이다(UE 출력 마스크).
TEST(MaterialTranslator_ConstantComponents) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    Constant3Properties color;
    color.constant = LinearColor(0.1f, 0.2f, 0.3f, 1.0f);
    const Guid rgb = fixture.Add("Material.Constant3", env.Save(color));
    fixture.Connect(rgb, "B", OutputNodeId, "AmbientOcclusion");
    fixture.Connect(rgb, "Result", OutputNodeId, "WorldPositionOffset");
    Constant2Properties pair;
    pair.r = 7.0f;
    pair.g = 9.0f;
    const Guid two = fixture.Add("Material.Constant2", env.Save(pair));
    fixture.Connect(two, "G", OutputNodeId, "PixelDepthOffset");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    const float blue[] = {0.3f};
    const float all[] = {0.1f, 0.2f, 0.3f};
    const float green[] = {9.0f};
    CHECK(IsFloatResult(out, "AmbientOcclusion", blue));
    CHECK(IsFloatResult(out, "WorldPositionOffset", all));
    CHECK(IsFloatResult(out, "PixelDepthOffset", green));
    CHECK(out.diagnostics.items.IsEmpty());

    // ConstantDouble은 Double 상수이고, Float 출력 핀은 낮춘다(UE LWC demote — 6.8절).
    GraphFixture large(env);
    ConstantDoubleProperties precise;
    precise.value = 2.5;
    const Guid wide = large.Add("Material.ConstantDouble", env.Save(precise));
    const Guid sum = large.Add("Material.Add");
    large.Connect(wide, "Result", sum, "A");
    large.Connect(sum, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(large.ready);
    MaterialTranslation demoted(env.allocator);
    REQUIRE(Translate(large, Traits(), demoted));
    REQUIRE(demoted.succeeded);
    // Double 산술은 Double로 남고(LWC 승격) 출력 핀에서 Float로 바뀐다.
    const IrValue& converted = ValueOf(demoted, FindResult(demoted, "AmbientOcclusion")->value);
    CHECK(converted.type == IrFloat(1));
    CHECK(converted.op == IrOp::Convert);
    CHECK(ValueOf(demoted, converted.operands[0]).type == IrDouble(1));
}

// 필수 입력 없음(MissingInput)·순환(V2)·모르는 노드(V12)·아직 번역하지 않는 노드(NotYetLowered).
TEST(MaterialTranslator_GraphErrors) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    {
        GraphFixture fixture(env);
        const Guid sine = fixture.Add("Material.Sine");
        fixture.Connect(sine, "Result", OutputNodeId, "AmbientOcclusion");
        REQUIRE(fixture.ready);
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(!out.succeeded);
        CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::MissingInput, sine));
    }

    {
        GraphFixture fixture(env);
        const Guid first = fixture.Add("Material.Add");
        const Guid second = fixture.Add("Material.Add");
        fixture.Connect(first, "Result", second, "A");
        fixture.Connect(second, "Result", first, "A");
        fixture.Connect(second, "Result", OutputNodeId, "AmbientOcclusion");
        REQUIRE(fixture.ready);
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(!out.succeeded);
        CHECK(HasError(out, MaterialDiagnosticCode::Cycle));
        CHECK(out.module.Results().IsEmpty());
    }

    {
        GraphFixture fixture(env);
        const Guid unknown = fixture.Add("Material.DoesNotExist");
        // 선행 작업을 기다리는 노드(MPQ-CurveAsset)다. MS2 끝에는 구현 노드가 모두 번역된다.
        const Guid later = fixture.Add("Material.CurveAtlasRowParameter");
        fixture.Connect(unknown, "Result", OutputNodeId, "AmbientOcclusion");
        fixture.Connect(later, "R", OutputNodeId, "PixelDepthOffset");
        REQUIRE(fixture.ready);
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(!out.succeeded);
        CHECK(CountErrors(out) == 2);
        CHECK(HasErrorAt(out, MaterialDiagnosticCode::UnknownNode, unknown));
        CHECK(HasErrorAt(out, MaterialDiagnosticCode::PrerequisiteMissing, later));
    }

    {
        // 연결되지 않은 노드는 오류가 있어도 번역에 닿지 않는다(출력에서 거슬러 간다).
        GraphFixture fixture(env);
        (void)fixture.Add("Material.Sine");
        (void)fixture.Add("Material.DoesNotExist");
        REQUIRE(fixture.ready);
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(out.succeeded);
    }
}

// V5: 영역·블렌드 모드가 쓰지 않는 출력 핀 연결은 경고이고 IR에서 버린다. MS2a가 번역하지 않는 핀
// (Surface 등)의 연결은 NotYetLowered다.
TEST(MaterialTranslator_InactiveAndPendingOutputs) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid half = AddConstant(fixture, 0.5f);
    fixture.Connect(half, "Result", OutputNodeId, "Opacity");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    CHECK(out.succeeded);
    CHECK(FindResult(out, "Opacity") == nullptr);
    CHECK(CountDiagnostics(out, MaterialDiagnosticCode::InactiveOutput,
                           nodegraph::GraphDiagnosticSeverity::Warning) == 1);

    GraphFixture pending(env);
    const Guid value = AddConstant(pending, 0.5f);
    pending.Connect(value, "Result", OutputNodeId, "Displacement");
    REQUIRE(pending.ready);
    MaterialTranslation notYet(env.allocator);
    REQUIRE(Translate(pending, Traits(), notYet));
    CHECK(!notYet.succeeded);
    CHECK(HasOnlyErrorAt(notYet, MaterialDiagnosticCode::NotYetLowered, OutputNodeId));
}

// 주기 있는 삼각 함수: 입력 × (2π / period), period ≤ 0이면 입력 그대로(UE IR Build — 6.8절).
// 초월 함수는 접지 않는다.
TEST(MaterialTranslator_PeriodicTrigonometry) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid one = AddConstant(fixture, 1.0f);
    SineProperties periodic;
    periodic.period = 1.0f;
    const Guid sine = fixture.Add("Material.Sine", env.Save(periodic));
    fixture.Connect(one, "Result", sine, "Input");
    fixture.Connect(sine, "Result", OutputNodeId, "AmbientOcclusion");
    CosineProperties cosinePeriod;
    cosinePeriod.period = 1.0f;
    const Guid cosine = fixture.Add("Material.Cosine", env.Save(cosinePeriod));
    fixture.Connect(one, "Result", cosine, "Input");
    fixture.Connect(cosine, "Result", OutputNodeId, "PixelDepthOffset");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    const IrResult* ao = FindResult(out, "AmbientOcclusion");
    REQUIRE(ao != nullptr);
    const IrValue& sin = ValueOf(out, ao->value);
    CHECK(sin.op == IrOp::Sin);
    const IrValue& argument = ValueOf(out, sin.operands[0]);
    CHECK(argument.op == IrOp::Constant);
    CHECK(FloatOf(argument, 0) == TwoPiFloat);
    CHECK(ValueOf(out, FindResult(out, "PixelDepthOffset")->value).op == IrOp::Cos);

    // period 0이면 곱하지 않는다(Cosine도 — UE 기본 번역기의 코드 번호 0 곱 결함을 따르지 않는다).
    GraphFixture plain(env);
    const Guid input = AddConstant(plain, 0.25f);
    CosineProperties flat;
    flat.period = 0.0f;
    const Guid flatCosine = plain.Add("Material.Cosine", env.Save(flat));
    plain.Connect(input, "Result", flatCosine, "Input");
    plain.Connect(flatCosine, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(plain.ready);
    MaterialTranslation unscaled(env.allocator);
    REQUIRE(Translate(plain, Traits(), unscaled));
    const IrValue& cos = ValueOf(unscaled, FindResult(unscaled, "AmbientOcclusion")->value);
    CHECK(cos.op == IrOp::Cos);
    CHECK(FloatOf(ValueOf(unscaled, cos.operands[0]), 0) == 0.25f);
}

// Power: UE PositiveClampedPow — select(base <= 2.980233e-8, 0, pow(base, exponent)).
TEST(MaterialTranslator_PowerClampsBase) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid base = AddVarying(fixture, 0.5f);
    const Guid power = fixture.Add("Material.Power");
    fixture.Connect(base, "Result", power, "Base");
    fixture.Connect(power, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    const IrValue& select = ValueOf(out, FindResult(out, "AmbientOcclusion")->value);
    REQUIRE(select.op == IrOp::Select);
    const IrValue& condition = ValueOf(out, select.operands[0]);
    CHECK(condition.op == IrOp::LessThanOrEquals);
    CHECK(FloatOf(ValueOf(out, condition.operands[1]), 0) == 2.980233e-8f);
    CHECK(FloatOf(ValueOf(out, select.operands[1]), 0) == 0.0f);
    const IrValue& pow = ValueOf(out, select.operands[2]);
    CHECK(pow.op == IrOp::Pow);
    // Exponent 기본값 2다.
    CHECK(FloatOf(ValueOf(out, pow.operands[1]), 0) == 2.0f);
}

// Switch: 상수 선택값이면 [i, i + 1)인 입력, 범위 밖은 Default만 쓴다. 상수가 아니면 UE step
// 식이다.
TEST(MaterialTranslator_Switch) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    const auto build = [&](GraphFixture& fixture, Guid selector) noexcept {
        SwitchProperties properties(env.allocator);
        (void)properties.inputs.TryPushBack(SwitchInput{});
        (void)properties.inputs.TryPushBack(SwitchInput{});
        const Guid node = fixture.Add("Material.Switch", env.Save(properties));
        fixture.Connect(AddConstant(fixture, 10.0f), "Result", node, "Input0");
        fixture.Connect(AddConstant(fixture, 20.0f), "Result", node, "Input1");
        fixture.SetValue(node, "Default", PinValue(30.0f, 1));
        fixture.Connect(selector, "Result", node, "SwitchValue");
        fixture.Connect(node, "Result", OutputNodeId, "AmbientOcclusion");
        return node;
    };

    struct Case {
        float selector;
        float expected;
    };

    const Case cases[] = {{0.0f, 10.0f}, {0.99f, 10.0f}, {1.0f, 20.0f},
                          {1.5f, 20.0f}, {2.0f, 30.0f},  {-0.5f, 30.0f}};
    for (const Case& testCase : cases) {
        GraphFixture fixture(env);
        (void)build(fixture, AddConstant(fixture, testCase.selector));
        REQUIRE(fixture.ready);
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        REQUIRE(out.succeeded);
        const float expected[] = {testCase.expected};
        CHECK(IsFloatResult(out, "AmbientOcclusion", expected));
    }

    GraphFixture dynamic(env);
    (void)build(dynamic, AddVarying(dynamic, 1.0f));
    REQUIRE(dynamic.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(dynamic, Traits(), out));
    REQUIRE(out.succeeded);
    // 마지막 연산은 Σ항 + (step(f, 0) + step(n, f)) · Default다.
    const IrValue& sum = ValueOf(out, FindResult(out, "AmbientOcclusion")->value);
    CHECK(sum.op == IrOp::Add);
    const IrValue& defaultTerm = ValueOf(out, sum.operands[1]);
    CHECK(defaultTerm.op == IrOp::Multiply);
    CHECK(FloatOf(ValueOf(out, defaultTerm.operands[1]), 0) == 30.0f);

    // 입력이 비어 있으면(UE "missing input") 오류다.
    GraphFixture missing(env);
    SwitchProperties one(env.allocator);
    (void)one.inputs.TryPushBack(SwitchInput{});
    const Guid node = missing.Add("Material.Switch", env.Save(one));
    missing.Connect(node, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(missing.ready);
    MaterialTranslation absent(env.allocator);
    REQUIRE(Translate(missing, Traits(), absent));
    CHECK(HasOnlyErrorAt(absent, MaterialDiagnosticCode::MissingInput, node));
}

// Operator: Add·Multiply는 입력 전부를 접고, 연결 없는 입력은 constValue다. 비교는 Bool, 모르는
// 종류는 InvalidProperty, LWC 타일은 NotYetLowered다.
TEST(MaterialTranslator_Operator) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    OperatorProperties sum(env.allocator);
    sum.operatorKind = OperatorKind::Add;
    const float constants[] = {1.0f, 2.0f, 4.0f};
    for (float value : constants) {
        OperatorInput input;
        input.constValue = value;
        REQUIRE(sum.dynamicInputs.TryPushBack(input));
    }

    const Guid add = fixture.Add("Material.Operator", env.Save(sum));
    fixture.Connect(add, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    const float seven[] = {7.0f};
    CHECK(IsFloatResult(out, "AmbientOcclusion", seven));

    // 비교(상수 접힘 → Bool)는 Float 핀에 V1이다(Bool은 수로 바뀌지 않는다).
    GraphFixture compare(env);
    OperatorProperties greater(env.allocator);
    greater.operatorKind = OperatorKind::GreaterThan;
    REQUIRE(greater.dynamicInputs.TryPushBack(OperatorInput{}));
    REQUIRE(greater.dynamicInputs.TryPushBack(OperatorInput{}));
    const Guid node = compare.Add("Material.Operator", env.Save(greater));
    compare.Connect(node, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(compare.ready);
    MaterialTranslation boolean(env.allocator);
    REQUIRE(Translate(compare, Traits(), boolean));
    CHECK(HasOnlyErrorAt(boolean, MaterialDiagnosticCode::TypeMismatch, OutputNodeId));

    // 인자가 모자라면 InvalidProperty, LWC 타일은 NotYetLowered다.
    GraphFixture shortList(env);
    OperatorProperties lerp(env.allocator);
    lerp.operatorKind = OperatorKind::Lerp;
    REQUIRE(lerp.dynamicInputs.TryPushBack(OperatorInput{}));
    const Guid fewer = shortList.Add("Material.Operator", env.Save(lerp));
    shortList.Connect(fewer, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(shortList.ready);
    MaterialTranslation tooFew(env.allocator);
    REQUIRE(Translate(shortList, Traits(), tooFew));
    CHECK(HasOnlyErrorAt(tooFew, MaterialDiagnosticCode::InvalidProperty, fewer));

    GraphFixture tile(env);
    OperatorProperties lwc(env.allocator);
    lwc.operatorKind = OperatorKind::LWCTile;
    REQUIRE(lwc.dynamicInputs.TryPushBack(OperatorInput{}));
    const Guid tiled = tile.Add("Material.Operator", env.Save(lwc));
    tile.Connect(tiled, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(tile.ready);
    MaterialTranslation lwcOut(env.allocator);
    REQUIRE(Translate(tile, Traits(), lwcOut));
    CHECK(HasOnlyErrorAt(lwcOut, MaterialDiagnosticCode::NotYetLowered, tiled));
}

// If: AEqualsB가 있으면 select(abs(A − B) > t, select(A ≥ B, G, L), E), 없으면 select(A ≥ B, G, L).
// 상수 A·B면 접힌다.
TEST(MaterialTranslator_If) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    struct Case {
        float a;
        float b;
        bool equalsConnected;
        float expected;
    };

    const Case cases[] = {{2.0f, 1.0f, false, 10.0f},
                          {0.0f, 1.0f, false, 20.0f},
                          {1.0f, 1.0f, false, 10.0f},
                          {1.0f, 1.0f, true, 30.0f},
                          {2.0f, 1.0f, true, 10.0f}};
    for (const Case& testCase : cases) {
        GraphFixture fixture(env);
        const Guid node = fixture.Add("Material.If");
        fixture.Connect(AddConstant(fixture, testCase.a), "Result", node, "A");
        fixture.SetValue(node, "B", PinValue(testCase.b, 1));
        fixture.Connect(AddConstant(fixture, 10.0f), "Result", node, "AGreaterThanB");
        fixture.Connect(AddConstant(fixture, 20.0f), "Result", node, "ALessThanB");
        if (testCase.equalsConnected) {
            fixture.Connect(AddConstant(fixture, 30.0f), "Result", node, "AEqualsB");
        }

        fixture.Connect(node, "Result", OutputNodeId, "AmbientOcclusion");
        REQUIRE(fixture.ready);
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        REQUIRE(out.succeeded);
        const float expected[] = {testCase.expected};
        CHECK(IsFloatResult(out, "AmbientOcclusion", expected));
    }

    // 상수가 아닌 A면 바깥 Select의 조건이 GreaterThan(abs(A − B), threshold)다.
    GraphFixture dynamic(env);
    const Guid node = dynamic.Add("Material.If");
    dynamic.Connect(AddVarying(dynamic, 1.0f), "Result", node, "A");
    dynamic.Connect(AddConstant(dynamic, 10.0f), "Result", node, "AGreaterThanB");
    dynamic.Connect(AddConstant(dynamic, 20.0f), "Result", node, "ALessThanB");
    dynamic.Connect(AddConstant(dynamic, 30.0f), "Result", node, "AEqualsB");
    dynamic.Connect(node, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(dynamic.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(dynamic, Traits(), out));
    REQUIRE(out.succeeded);
    const IrValue& outer = ValueOf(out, FindResult(out, "AmbientOcclusion")->value);
    REQUIRE(outer.op == IrOp::Select);
    CHECK(ValueOf(out, outer.operands[0]).op == IrOp::GreaterThan);
    CHECK(ValueOf(out, outer.operands[1]).op == IrOp::Select);
    CHECK(FloatOf(ValueOf(out, outer.operands[2]), 0) == 30.0f);
}

// DynamicBranch: 상수 조건이면 고른 입력만 lowering하고(다른 쪽 오류는 닿지 않는다), 아니면
// Branch다.
TEST(MaterialTranslator_DynamicBranch) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    OperatorProperties greater(env.allocator);
    greater.operatorKind = OperatorKind::GreaterThan;
    OperatorInput two;
    two.constValue = 2.0f;
    REQUIRE(greater.dynamicInputs.TryPushBack(two));
    REQUIRE(greater.dynamicInputs.TryPushBack(OperatorInput{}));
    const Guid condition = fixture.Add("Material.Operator", env.Save(greater));
    const Guid branch = fixture.Add("Material.DynamicBranch");
    fixture.Connect(condition, "Result", branch, "Condition");
    fixture.Connect(AddConstant(fixture, 0.75f), "Result", branch, "True");
    fixture.Connect(AddConstant(fixture, 0.25f), "Result", branch, "False");
    fixture.Connect(branch, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    const float chosen[] = {0.75f};
    CHECK(IsFloatResult(out, "AmbientOcclusion", chosen));

    // 고르지 않은 쪽도 번역한다(UE 기본 번역기는 세 입력을 모두 컴파일한다): 그쪽 오류도 남는다.
    GraphFixture unchosen(env);
    const Guid same = unchosen.Add("Material.Operator", env.Save(greater));
    const Guid node = unchosen.Add("Material.DynamicBranch");
    const Guid broken = unchosen.Add("Material.Sine");
    unchosen.Connect(same, "Result", node, "Condition");
    unchosen.Connect(AddConstant(unchosen, 0.75f), "Result", node, "True");
    unchosen.Connect(broken, "Result", node, "False");
    unchosen.Connect(node, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(unchosen.ready);
    MaterialTranslation failed(env.allocator);
    REQUIRE(Translate(unchosen, Traits(), failed));
    CHECK(!failed.succeeded);
    CHECK(HasOnlyErrorAt(failed, MaterialDiagnosticCode::MissingInput, broken));

    GraphFixture dynamic(env);
    OperatorProperties less(env.allocator);
    less.operatorKind = OperatorKind::LessThan;
    REQUIRE(less.dynamicInputs.TryPushBack(OperatorInput{}));
    REQUIRE(less.dynamicInputs.TryPushBack(OperatorInput{}));
    const Guid test = dynamic.Add("Material.Operator", env.Save(less));
    dynamic.Connect(AddVarying(dynamic, 1.0f), "Result", test, "A");
    const Guid dynamicBranch = dynamic.Add("Material.DynamicBranch");
    dynamic.Connect(test, "Result", dynamicBranch, "Condition");
    dynamic.Connect(AddConstant(dynamic, 1.0f), "Result", dynamicBranch, "True");
    dynamic.Connect(AddConstant(dynamic, 2.0f), "Result", dynamicBranch, "False");
    dynamic.Connect(dynamicBranch, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(dynamic.ready);
    MaterialTranslation branched(env.allocator);
    REQUIRE(Translate(dynamic, Traits(), branched));
    REQUIRE(branched.succeeded);
    CHECK(ValueOf(branched, FindResult(branched, "AmbientOcclusion")->value).op == IrOp::Branch);
}

// 폭 규칙: 스칼라는 복제, 다른 폭의 벡터끼리는 V1(UE "Arithmetic between types … are undefined").
// Append 합 > 4·ComponentMask 범위 밖도 V1이다.
TEST(MaterialTranslator_WidthRules) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    Constant2Properties pair;
    pair.r = 1.0f;
    pair.g = 2.0f;
    Constant3Properties triple;
    triple.constant = LinearColor(1.0f, 2.0f, 3.0f, 0.0f);
    const Guid two = fixture.Add("Material.Constant2", env.Save(pair));
    const Guid three = fixture.Add("Material.Constant3", env.Save(triple));
    const Guid add = fixture.Add("Material.Add");
    fixture.Connect(two, "Result", add, "A");
    fixture.Connect(three, "Result", add, "B");
    fixture.Connect(add, "Result", OutputNodeId, "WorldPositionOffset");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    CHECK(!out.succeeded);
    CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::TypeMismatch, add));

    // Append float2 + float1 = float3, ComponentMask g·b.
    GraphFixture append(env);
    const Guid pairNode = append.Add("Material.Constant2", env.Save(pair));
    const Guid appended = append.Add("Material.Append");
    append.Connect(pairNode, "Result", appended, "A");
    append.Connect(AddConstant(append, 5.0f), "Result", appended, "B");
    append.Connect(appended, "Result", OutputNodeId, "WorldPositionOffset");
    ComponentMaskProperties mask;
    mask.g = true;
    mask.b = true;
    const Guid masked = append.Add("Material.ComponentMask", env.Save(mask));
    append.Connect(appended, "Result", masked, "Input");
    const Guid length = append.Add("Material.Length");
    append.Connect(masked, "Result", length, "Input");
    append.Connect(length, "Result", OutputNodeId, "PixelDepthOffset");
    REQUIRE(append.ready);
    MaterialTranslation joined(env.allocator);
    REQUIRE(Translate(append, Traits(), joined));
    REQUIRE(joined.succeeded);
    const float vector[] = {1.0f, 2.0f, 5.0f};
    CHECK(IsFloatResult(joined, "WorldPositionOffset", vector));
    const IrValue& lengthValue = ValueOf(joined, FindResult(joined, "PixelDepthOffset")->value);
    CHECK(lengthValue.op == IrOp::Length);
    CHECK(ValueOf(joined, lengthValue.operands[0]).type == IrFloat(2));

    GraphFixture overflow(env);
    const Guid tripleNode = overflow.Add("Material.Constant3", env.Save(triple));
    const Guid tooLong = overflow.Add("Material.Append");
    overflow.Connect(tripleNode, "Result", tooLong, "A");
    overflow.Connect(tripleNode, "Result", tooLong, "B");
    overflow.Connect(tooLong, "Result", OutputNodeId, "WorldPositionOffset");
    ComponentMaskProperties alpha;
    alpha.a = true;
    const Guid alphaMask = overflow.Add("Material.ComponentMask", env.Save(alpha));
    overflow.Connect(tripleNode, "Result", alphaMask, "Input");
    overflow.Connect(alphaMask, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(overflow.ready);
    MaterialTranslation rejected(env.allocator);
    REQUIRE(Translate(overflow, Traits(), rejected));
    CHECK(CountErrors(rejected) == 2);
    CHECK(HasErrorAt(rejected, MaterialDiagnosticCode::TypeMismatch, tooLong));
    CHECK(HasErrorAt(rejected, MaterialDiagnosticCode::TypeMismatch, alphaMask));
}

// InverseLinearInterpolate: 같은 식이면 1·0, A = B면 DivideByZero, 아니면 (V − A) / (B − A).
TEST(MaterialTranslator_InverseLinearInterpolate) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    struct Case {
        float a;
        float b;
        float value;
        bool clamp;
        float expected;
    };

    const Case cases[] = {{0.0f, 4.0f, 1.0f, false, 0.25f},
                          {0.0f, 4.0f, 4.0f, false, 1.0f},
                          {2.0f, 4.0f, 2.0f, false, 0.0f},
                          {0.0f, 4.0f, 8.0f, true, 1.0f},
                          {0.0f, 4.0f, 8.0f, false, 2.0f}};
    for (const Case& testCase : cases) {
        GraphFixture fixture(env);
        InverseLinearInterpolateProperties properties;
        properties.clampResult = testCase.clamp;
        const Guid node = fixture.Add("Material.InverseLinearInterpolate", env.Save(properties));
        fixture.SetValue(node, "A", PinValue(testCase.a, 1));
        fixture.SetValue(node, "B", PinValue(testCase.b, 1));
        fixture.SetValue(node, "Value", PinValue(testCase.value, 1));
        fixture.Connect(node, "Result", OutputNodeId, "AmbientOcclusion");
        REQUIRE(fixture.ready);
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        REQUIRE(out.succeeded);
        const float expected[] = {testCase.expected};
        CHECK(IsFloatResult(out, "AmbientOcclusion", expected));
    }

    GraphFixture same(env);
    const Guid node = same.Add("Material.InverseLinearInterpolate");
    same.SetValue(node, "A", PinValue(3.0f, 1));
    same.SetValue(node, "B", PinValue(3.0f, 1));
    same.SetValue(node, "Value", PinValue(1.0f, 1));
    same.Connect(node, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(same.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(same, Traits(), out));
    CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::DivideByZero, node));
}

// Convert: 대응이 있는 성분은 입력 성분, 없으면 출력 기본값이다. 잘못된 색인은 InvalidProperty다.
TEST(MaterialTranslator_Convert) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    ConvertProperties properties(env.allocator);
    ConvertInput input;
    input.type = ConvertValueType::Vector3;
    ConvertOutput output;
    output.type = ConvertValueType::Vector2;
    output.defaultValue = LinearColor(0.0f, 9.0f, 0.0f, 0.0f);
    ConvertMapping mapping;
    mapping.inputIndex = 0;
    mapping.inputComponentIndex = 2;
    mapping.outputIndex = 0;
    mapping.outputComponentIndex = 0;
    REQUIRE(properties.convertInputs.TryPushBack(input));
    REQUIRE(properties.convertOutputs.TryPushBack(output));
    REQUIRE(properties.convertMappings.TryPushBack(mapping));
    const Guid convert = fixture.Add("Material.Convert", env.Save(properties));
    Constant3Properties triple;
    triple.constant = LinearColor(1.0f, 2.0f, 3.0f, 0.0f);
    fixture.Connect(fixture.Add("Material.Constant3", env.Save(triple)), "Result", convert,
                    "Input0");
    const Guid mask = fixture.Add("Material.Append");
    fixture.Connect(convert, "Output0", mask, "A");
    fixture.Connect(AddConstant(fixture, 4.0f), "Result", mask, "B");
    fixture.Connect(mask, "Result", OutputNodeId, "WorldPositionOffset");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    const float expected[] = {3.0f, 9.0f, 4.0f};
    CHECK(IsFloatResult(out, "WorldPositionOffset", expected));

    GraphFixture bad(env);
    ConvertProperties wrong(env.allocator);
    ConvertMapping outside = mapping;
    outside.inputComponentIndex = 3;
    REQUIRE(wrong.convertInputs.TryPushBack(input));
    REQUIRE(wrong.convertOutputs.TryPushBack(output));
    REQUIRE(wrong.convertMappings.TryPushBack(outside));
    const Guid broken = bad.Add("Material.Convert", env.Save(wrong));
    bad.Connect(broken, "Output0", OutputNodeId, "WorldPositionOffset");
    REQUIRE(bad.ready);
    MaterialTranslation rejected(env.allocator);
    REQUIRE(Translate(bad, Traits(), rejected));
    CHECK(HasOnlyErrorAt(rejected, MaterialDiagnosticCode::InvalidProperty, broken));
}

// 재경유: 공통 Graph.Reroute와 이름 있는 재경유(선언·쓰임)는 값을 그대로 넘긴다. 없는 선언은
// InvalidProperty다. 비활성 노드(통과 쌍 없음)는 연결이 없는 것과 같다.
TEST(MaterialTranslator_Reroutes) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid value = AddConstant(fixture, 0.5f);
    const Guid reroute = fixture.Add(nodegraph::RerouteNodeTypeName);
    fixture.Connect(value, "Result", reroute, nodegraph::ReroutePinIn);
    const Guid declaration = fixture.Add("Material.NamedRerouteDeclaration");
    fixture.Connect(reroute, nodegraph::ReroutePinOut, declaration, "Input");
    NamedRerouteUsageProperties usage;
    usage.declaration = declaration;
    const Guid use = fixture.Add("Material.NamedRerouteUsage", env.Save(usage));
    fixture.Connect(use, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    const float half[] = {0.5f};
    CHECK(IsFloatResult(out, "AmbientOcclusion", half));

    GraphFixture invalid(env);
    NamedRerouteUsageProperties dangling;
    dangling.declaration = Guid{5, 5};
    const Guid lost = invalid.Add("Material.NamedRerouteUsage", invalid.env.Save(dangling));
    invalid.Connect(lost, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(invalid.ready);
    MaterialTranslation rejected(env.allocator);
    REQUIRE(Translate(invalid, Traits(), rejected));
    CHECK(HasOnlyErrorAt(rejected, MaterialDiagnosticCode::InvalidProperty, lost));

    GraphFixture disabled(env);
    const Guid off = AddConstant(disabled, 0.25f);
    disabled.Find(off)->enableState = nodegraph::NodeEnableState::Disabled;
    disabled.Connect(off, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(disabled.ready);
    MaterialTranslation skipped(env.allocator);
    REQUIRE(Translate(disabled, Traits(), skipped));
    REQUIRE(skipped.succeeded);
    const float one[] = {1.0f};
    CHECK(IsFloatResult(skipped, "AmbientOcclusion", one));
}

// V8: 같은 이름(대소문자 무시)·다른 종류의 파라미터 노드는 연결되지 않아도 오류다. 같은 종류는
// 된다.
TEST(MaterialTranslator_ParameterNameConflict) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    ScalarParameterProperties scalar;
    scalar.parameterName = env.Intern("Roughness");
    VectorParameterProperties vector(env.allocator);
    vector.parameterName = env.Intern("ROUGHNESS");
    (void)fixture.Add("Material.ScalarParameter", env.Save(scalar));
    const Guid clash = fixture.Add("Material.VectorParameter", env.Save(vector));
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    CHECK(!out.succeeded);
    CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::ParameterConflict, clash));

    GraphFixture shared(env);
    (void)shared.Add("Material.ScalarParameter", env.Save(scalar));
    (void)shared.Add("Material.ScalarParameter", env.Save(scalar));
    REQUIRE(shared.ready);
    MaterialTranslation same(env.allocator);
    REQUIRE(Translate(shared, Traits(), same));
    CHECK(same.succeeded);
}

// V11: NaN·Inf 상수 속성은 오류다.
TEST(MaterialTranslator_NonFiniteProperty) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const float zero = 0.0f;
    const Guid nan = AddConstant(fixture, zero / zero);
    fixture.Connect(nan, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    CHECK(!out.succeeded);
    CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::NonFiniteConstant, nan));
}

// 결정성(6.6절): 노드·간선 저장 순서만 다른 두 문서는 같은 IR hash다.
TEST(MaterialTranslator_DeterministicAcrossStorageOrder) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    const auto build = [&](GraphFixture& fixture, bool reversed) noexcept {
        const Guid a{NodeIdHigh, 100};
        const Guid b{NodeIdHigh, 101};
        const Guid sum{NodeIdHigh, 102};
        ConstantProperties first;
        first.r = 0.5f;
        ConstantProperties second;
        second.r = 1.5f;
        SineProperties sine;
        sine.period = 0.0f;
        const Guid wave{NodeIdHigh, 103};
        if (reversed) {
            (void)fixture.AddWithId("Material.Sine", wave, env.Save(sine));
            (void)fixture.AddWithId("Material.Add", sum, Array<uint8>(env.allocator));
            (void)fixture.AddWithId("Material.Constant", b, env.Save(second));
            (void)fixture.AddWithId("Material.Constant", a, env.Save(first));
            fixture.Connect(sum, "Result", OutputNodeId, "AmbientOcclusion");
            fixture.Connect(b, "Result", sum, "B");
            fixture.Connect(wave, "Result", sum, "A");
            fixture.Connect(a, "Result", wave, "Input");
        } else {
            (void)fixture.AddWithId("Material.Constant", a, env.Save(first));
            (void)fixture.AddWithId("Material.Constant", b, env.Save(second));
            (void)fixture.AddWithId("Material.Add", sum, Array<uint8>(env.allocator));
            (void)fixture.AddWithId("Material.Sine", wave, env.Save(sine));
            fixture.Connect(a, "Result", wave, "Input");
            fixture.Connect(wave, "Result", sum, "A");
            fixture.Connect(b, "Result", sum, "B");
            fixture.Connect(sum, "Result", OutputNodeId, "AmbientOcclusion");
        }
    };

    GraphFixture forward(env);
    build(forward, false);
    GraphFixture backward(env);
    build(backward, true);
    REQUIRE(forward.ready);
    REQUIRE(backward.ready);
    MaterialTranslation left(env.allocator);
    MaterialTranslation right(env.allocator);
    REQUIRE(Translate(forward, Traits(), left));
    REQUIRE(Translate(backward, Traits(), right));
    REQUIRE(left.succeeded);
    REQUIRE(right.succeeded);
    CHECK(left.module.Values().Size() == right.module.Values().Size());
    CHECK(left.module.Hash() == right.module.Hash());
}

// 입력 검사: 도메인이 Material이 아니거나 출력 노드가 없으면 InvalidArgument다.
TEST(MaterialTranslator_RejectsInvalidDocuments) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture wrongDomain(env);
    wrongDomain.document.domain = env.Intern("MaterialFunction");
    MaterialTranslation out(env.allocator);
    CHECK(Translate(wrongDomain, Traits(), out).Error() == ErrorCode::InvalidArgument);

    GraphFixture noOutput(env);
    noOutput.Main().nodes.Clear();
    CHECK(Translate(noOutput, Traits(), out).Error() == ErrorCode::InvalidArgument);
    CHECK(!out.succeeded);
}

// 깊은 사슬: 번역은 의존 후위 순서로 하므로 그래프 깊이가 C++ 스택을 쓰지 않는다(재귀 상한
// MaxLoweringDepth보다 훨씬 긴 사슬도 된다).
TEST(MaterialTranslator_DeepChainsDoNotRecurse) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    // 짝수 번의 1 − x라 결과는 처음 값이다.
    constexpr usize ChainLength = 4096;
    GraphFixture fixture(env);
    Guid previous = AddConstant(fixture, 0.5f);
    for (usize index = 0; index < ChainLength; ++index) {
        const Guid next = fixture.Add("Material.OneMinus");
        fixture.Connect(previous, "Result", next, "Input");
        previous = next;
    }

    fixture.Connect(previous, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    const float half[] = {0.5f};
    CHECK(IsFloatResult(out, "AmbientOcclusion", half));
}

} // namespace material_translator_tests_file
