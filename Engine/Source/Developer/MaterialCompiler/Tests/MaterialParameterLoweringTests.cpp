#include <Core/Text/StringBuilder.h>
#include <Platform/Memory.h>
#include <MaterialCompiler/MaterialConstants.h>
#include <MaterialCompiler/MaterialSourceWriter.h>
#include <MaterialSchema/MaterialContextValue.h>
#include <MaterialSchema/Nodes/Math/ComponentMaskProperties.generated.h>
#include <MaterialSchema/Nodes/Math/ComponentMaskProperties.h>
#include <MaterialSchema/Nodes/Parameters/ChannelMaskParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/ChannelMaskParameterProperties.h>
#include <MaterialSchema/Nodes/Parameters/DoubleVectorParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/DoubleVectorParameterProperties.h>
#include <MaterialSchema/Nodes/Parameters/ParameterCollectionTransformProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/ParameterCollectionTransformProperties.h>
#include <MaterialSchema/Nodes/Parameters/ParameterCollectionValueProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/ParameterCollectionValueProperties.h>
#include <MaterialSchema/Nodes/Parameters/ScalarParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/ScalarParameterProperties.h>
#include <MaterialSchema/Nodes/Parameters/StaticBoolParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/StaticBoolParameterProperties.h>
#include <MaterialSchema/Nodes/Parameters/StaticComponentMaskParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/StaticComponentMaskParameterProperties.h>
#include <MaterialSchema/Nodes/Parameters/StaticSwitchParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/StaticSwitchParameterProperties.h>
#include <MaterialSchema/Nodes/Parameters/VectorParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/VectorParameterProperties.h>

#include "MaterialGraphFixture.h"

// MS2c2 시험이다(Docs/Design/MaterialSystem 6.10절 파라미터·상수 식): 수 파라미터의 IR 값·모듈 표,
// 상수 식 프로그램의 출력 선택(접는 연산·접지 않는 연산·Constant 빈도)과 layout(크기 순·float4
// 경계· Double 두 칸), 생성 원문의 상수 블록·컬렉션 cbuffer, custom primitive data, static 값(입력
// 우선· 기본값·고른 가지만 번역), 컬렉션 검사 순서·문구·상한(V9), 컬렉션 변환 식을 본다.
using namespace rocks;
using namespace rocks::material;
namespace material_parameter_lowering_tests_file {

using namespace material_graph_fixture;

namespace {

constexpr Guid RoughnessId{0x50415241ull, 1};
constexpr Guid TintId{0x50415241ull, 2};
constexpr Guid OriginId{0x50415241ull, 3};
constexpr Guid SwitchId{0x50415241ull, 4};
constexpr Guid MaskId{0x50415241ull, 5};
constexpr Guid CollectionA{0x434f4c4cull, 1};
constexpr Guid CollectionB{0x434f4c4cull, 2};
constexpr Guid CollectionC{0x434f4c4cull, 3};
constexpr Guid FogId{0x434f4c50ull, 1};
constexpr Guid WindId{0x434f4c50ull, 2};
constexpr Guid MatrixId{0x434f4c50ull, 3};

// 시험 컬렉션 layout: scalar Fog(칸 0.y), vector Wind(칸 1), vector Matrix(칸 2–6).
constexpr MaterialCollectionEntry TestEntries[] = {{FogId, 0, 1},
                                                   {WindId, 1, MaterialCollectionWholeVector},
                                                   {MatrixId, 2, MaterialCollectionWholeVector}};

struct TranslateOptions {
    Span<const MaterialStaticParameterValue> statics{};
    Span<const MaterialCollectionLayout> collections{};
};

bool TranslateWith(GraphFixture& fixture, const MaterialTraitsDesc& traits,
                   MaterialTranslation& out, const TranslateOptions& options) noexcept {
    MaterialTranslationInput input{fixture.document, fixture.env.catalog, fixture.env.names, traits,
                                   TestAssetId};
    input.staticParameters = options.statics;
    input.collections = options.collections;
    return TranslateMaterial(fixture.env.allocator, input, out).IsOk();
}

bool HasText(StringView text, StringView part) noexcept {
    for (usize at = 0; at + part.ByteCount() <= text.ByteCount(); ++at) {
        if (StringView(text.Data() + at, part.ByteCount()) == part) {
            return true;
        }
    }

    return false;
}

bool HasMessage(const MaterialTranslation& out, StringView message) noexcept {
    for (const nodegraph::GraphDiagnostic& diagnostic : out.diagnostics.items) {
        if (diagnostic.message.View() == message) {
            return true;
        }
    }

    return false;
}

bool Generate(GraphFixture& fixture, MaterialTranslation& out, MaterialGeneratedSource& source,
              const TranslateOptions& options = {},
              const MaterialTraitsDesc& traits = Traits()) noexcept {
    return TranslateWith(fixture, traits, out, options) && out.succeeded &&
           WriteMaterialSource(fixture.env.allocator, out.module, traits, TestAssetId, MainGraphId,
                               source)
               .IsOk() &&
           source.succeeded;
}

Guid AddScalar(GraphFixture& fixture, Guid id, float value,
               StringView name = "Roughness") noexcept {
    ScalarParameterProperties properties;
    properties.parameterName = fixture.env.Intern(name);
    properties.parameterId = id;
    properties.defaultValue = value;
    return fixture.Add("Material.ScalarParameter", fixture.env.Save(properties));
}

Guid AddVector(GraphFixture& fixture, Guid id, LinearColor value) noexcept {
    VectorParameterProperties properties(fixture.env.allocator);
    properties.parameterName = fixture.env.Intern("Tint");
    properties.parameterId = id;
    properties.defaultValue = value;
    return fixture.Add("Material.VectorParameter", fixture.env.Save(properties));
}

Guid AddMask(GraphFixture& fixture, bool r, bool g = false) noexcept {
    ComponentMaskProperties properties;
    properties.r = r;
    properties.g = g;
    return fixture.Add("Material.ComponentMask", fixture.env.Save(properties));
}

// node.pin을 마스크 R로 AO에 잇는다.
void ToAo(GraphFixture& fixture, Guid node, StringView pin) noexcept {
    const Guid mask = AddMask(fixture, true);
    fixture.Connect(node, pin, mask, "Input");
    fixture.Connect(mask, "Result", OutputNodeId, "AmbientOcclusion");
}

const IrValue* FindOp(const MaterialTranslation& out, IrOp op) noexcept {
    for (const IrValue& value : out.module.Values()) {
        if (value.op == op) {
            return &value;
        }
    }

    return nullptr;
}

usize CountOp(const MaterialTranslation& out, IrOp op) noexcept {
    usize count = 0;
    for (const IrValue& value : out.module.Values()) {
        count += value.op == op ? 1 : 0;
    }

    return count;
}

uint64 ContextIndex(StringView name) noexcept {
    const Span<const MaterialContextValue> table = MaterialContextValues();
    for (usize index = 0; index < table.Size(); ++index) {
        if (table[index].name == name) {
            return index;
        }
    }

    return table.Size();
}

Guid AddCollectionValue(GraphFixture& fixture, Guid collection, Guid parameter,
                        StringView name = "Fog") noexcept {
    ParameterCollectionValueProperties properties;
    properties.collection.assetId = AssetId{collection};
    properties.parameterName = fixture.env.Intern(name);
    properties.parameterId = parameter;
    return fixture.Add("Material.ParameterCollectionValue", fixture.env.Save(properties));
}

Guid AddCollectionTransform(GraphFixture& fixture, Guid collection, Guid parameter,
                            CollectionTransformType type, StringView name = "Matrix") noexcept {
    ParameterCollectionTransformProperties properties;
    properties.collection.assetId = AssetId{collection};
    properties.parameterName = fixture.env.Intern(name);
    properties.parameterId = parameter;
    properties.transformType = type;
    return fixture.Add("Material.ParameterCollectionTransform", fixture.env.Save(properties));
}

} // namespace

// ScalarParameter: Uniform 빈도 Parameter(id·기본값), 모듈 표 한 항목, 원문은 상수 블록 읽기다.
TEST(MaterialParameter_ScalarParameter) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid node = AddScalar(fixture, RoughnessId, 0.25f);
    fixture.Connect(node, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(fixture, out, source));
    const IrValue* parameter = FindOp(out, IrOp::Parameter);
    REQUIRE(parameter != nullptr);
    CHECK(parameter->frequency == IrFrequency::Uniform);
    CHECK(parameter->type == IrFloat(1));
    CHECK((Guid{parameter->immediate[0], parameter->immediate[1]} == RoughnessId));
    CHECK(parameter->immediate[2] == static_cast<uint64>(IrParameterKind::Scalar));
    CHECK(parameter->immediate[3] == 0);
    REQUIRE(out.module.Parameters().Size() == 1);
    CHECK(out.module.Parameters()[0].id == RoughnessId);
    CHECK(out.module.Parameters()[0].defaults[0] == 0.25);
    CHECK(HasText(source.text.View(),
                  "cbuffer MaterialConstants\n{\n    float4 materialFloats[1];\n};\n"));
    CHECK(HasText(source.text.View(), "#define MATERIAL_NUM_CONSTANT_VECTORS 1\n"));
    CHECK(HasText(source.text.View(), " = materialFloats[0].x;\n"));
}

// 같은 ParameterId의 두 노드는 한 값·한 표 항목이다. 모듈 표는 ParameterId 순이고 즉시값 [3]이
// 그 번호다.
TEST(MaterialParameter_SharedIdAndTableOrder) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid tint = AddVector(fixture, TintId, LinearColor(0.1f, 0.2f, 0.3f, 0.4f));
    const Guid first = AddScalar(fixture, RoughnessId, 0.5f);
    const Guid second = AddScalar(fixture, RoughnessId, 0.5f);
    const Guid sum = fixture.Add("Material.Add");
    fixture.Connect(first, "Result", sum, "A");
    fixture.Connect(second, "Result", sum, "B");
    const Guid product = fixture.Add("Material.Multiply");
    fixture.Connect(sum, "Result", product, "A");
    fixture.Connect(tint, "R", product, "B");
    fixture.Connect(product, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateWith(fixture, Traits(), out, {}));
    REQUIRE(out.succeeded);
    CHECK(CountOp(out, IrOp::Parameter) == 2);
    REQUIRE(out.module.Parameters().Size() == 2);
    CHECK(out.module.Parameters()[0].id == RoughnessId);
    CHECK(out.module.Parameters()[1].id == TintId);
    CHECK(out.module.Parameters()[1].kind == IrParameterKind::Vector);
    CHECK(out.module.Parameters()[1].defaults[3] == static_cast<double>(0.4f));
    for (const IrValue& value : out.module.Values()) {
        if (value.op == IrOp::Parameter) {
            const Guid id{value.immediate[0], value.immediate[1]};
            CHECK(value.immediate[3] == (id == RoughnessId ? 0u : 1u));
        }
    }

    // 사칙은 프로그램이 접는다: 결과(곱) 하나만 상수 블록 출력이다.
    MaterialConstants constants(env.allocator);
    REQUIRE(BuildMaterialConstants(out.module, constants));
    REQUIRE(constants.members.Size() == 1);
    CHECK(constants.floatVectors == 1);
    CHECK(out.module.Values()[constants.members[0].value].op == IrOp::Multiply);
    // 명령은 모듈 번호 순이고 피연산자는 앞 명령 번호다.
    for (usize step = 0; step < constants.steps.Size(); ++step) {
        for (uint8 operand = 0; operand < constants.steps[step].operandCount; ++operand) {
            CHECK(constants.steps[step].operands[operand] < step);
        }
    }

    CHECK(constants.members[0].step + 1 == constants.steps.Size());

    // 출처(12.8절 MS3d1): 주 그래프 파라미터는 소유 null·Global·빈 경로, 이름은 노드 parameterName.
    REQUIRE(out.module.ParameterOrigins().Size() == 2);
    const IrParameterOrigin* origin = out.module.FindParameterOrigin(RoughnessId);
    REQUIRE(origin != nullptr);
    CHECK(origin->owner == Guid{});
    CHECK(origin->parameterId == RoughnessId);
    CHECK(origin->scope == ParameterScopeKind::Global);
    CHECK(origin->callPath.IsEmpty());
    CHECK(origin->name.View() == StringView("Roughness"));
}

// 같은 ParameterId를 다른 기본값으로 두 번 쓰면 ParameterConflict다(UE
// r.Material.PedanticErrorChecksEnabled 검사 — 쿠킹 파라미터 표가 어느 노드를 골라도 같게).
TEST(MaterialParameter_ConflictingDefaults) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid first = AddScalar(fixture, RoughnessId, 0.5f);
    const Guid second = AddScalar(fixture, RoughnessId, 0.75f);
    const Guid sum = fixture.Add("Material.Add");
    fixture.Connect(first, "Result", sum, "A");
    fixture.Connect(second, "Result", sum, "B");
    fixture.Connect(sum, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateWith(fixture, Traits(), out, {}));
    CHECK(!out.succeeded);
    CHECK(CountDiagnostics(out, MaterialDiagnosticCode::ParameterConflict,
                           nodegraph::GraphDiagnosticSeverity::Error) == 1);
}

// 접지 않는 연산(Lerp)과 빈도가 높은 값(TexCoord)에 닿는 파라미터는 그 자체가 출력이다. Constant
// 빈도 식(sin(상수))은 원문에 남는다.
TEST(MaterialParameter_ProgramBoundary) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid a = AddScalar(fixture, RoughnessId, 0.5f);
    const Guid b = AddScalar(fixture, TintId, 0.75f, "Metal");
    const Guid alpha = AddVarying(fixture, 0.3f);
    const Guid lerp = fixture.Add("Material.LinearInterpolate");
    fixture.Connect(a, "Result", lerp, "A");
    fixture.Connect(b, "Result", lerp, "B");
    fixture.Connect(alpha, "Result", lerp, "Alpha");
    const Guid uv = fixture.Add("Material.TextureCoordinate");
    const Guid u = AddMask(fixture, true);
    fixture.Connect(uv, "Result", u, "Input");
    const Guid product = fixture.Add("Material.Multiply");
    fixture.Connect(lerp, "Result", product, "A");
    fixture.Connect(u, "Result", product, "B");
    fixture.Connect(product, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(fixture, out, source));
    MaterialConstants constants(env.allocator);
    REQUIRE(BuildMaterialConstants(out.module, constants));
    REQUIRE(constants.members.Size() == 2);
    for (const MaterialConstantMember& member : constants.members) {
        CHECK(out.module.Values()[member.value].op == IrOp::Parameter);
        CHECK(member.vector == 0);
        CHECK(member.components == 1);
    }

    CHECK(constants.members[0].firstComponent == 0);
    CHECK(constants.members[1].firstComponent == 1);
    CHECK(HasText(source.text.View(), "materialFloats[0].x"));
    CHECK(HasText(source.text.View(), "materialFloats[0].y"));
    CHECK(HasText(source.text.View(), "sin("));
    CHECK(HasText(source.text.View(), "lerp("));
}

// layout: 큰 것부터 채우고 float4 경계를 넘지 않는다(float3 다음 float2는 새 칸, float1은 남은 칸).
// Double 출력은 tile·offset 두 칸이다.
TEST(MaterialParameter_ConstantLayout) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid tint = AddVector(fixture, TintId, LinearColor(1.0f, 1.0f, 1.0f, 1.0f));
    const Guid scalar = AddScalar(fixture, RoughnessId, 0.5f);
    DoubleVectorParameterProperties origin;
    origin.parameterName = env.Intern("Origin");
    origin.parameterId = OriginId;
    origin.defaultValue = Vec4d(1.0e7, 2.0, 3.0, 4.0);
    const Guid wide = fixture.Add("Material.DoubleVectorParameter", env.Save(origin));
    const Guid uv = fixture.Add("Material.TextureCoordinate");
    const Guid rg = AddMask(fixture, true, true);
    fixture.Connect(tint, "RGBA", rg, "Input");
    // float3(RGB) × uv.x, float2(RG) × uv, float1 × uv.x, double3 − world 위치 → 모두 출력이다.
    const Guid u = AddMask(fixture, true);
    fixture.Connect(uv, "Result", u, "Input");
    const Guid rgb = fixture.Add("Material.Multiply");
    fixture.Connect(tint, "RGB", rgb, "A");
    fixture.Connect(u, "Result", rgb, "B");
    const Guid two = fixture.Add("Material.Multiply");
    fixture.Connect(rg, "Result", two, "A");
    fixture.Connect(uv, "Result", two, "B");
    const Guid one = fixture.Add("Material.Multiply");
    fixture.Connect(scalar, "Result", one, "A");
    fixture.Connect(u, "Result", one, "B");
    const Guid world = fixture.Add("Material.WorldPosition");
    const Guid relative = fixture.Add("Material.Subtract");
    fixture.Connect(world, "XYZ", relative, "A");
    fixture.Connect(wide, "XYZ", relative, "B");
    const Guid dot = fixture.Add("Material.DotProduct");
    fixture.Connect(rgb, "Result", dot, "A");
    fixture.Connect(relative, "Result", dot, "B");
    const Guid dot2 = fixture.Add("Material.DotProduct");
    fixture.Connect(two, "Result", dot2, "A");
    fixture.Connect(uv, "Result", dot2, "B");
    const Guid sum = fixture.Add("Material.Add");
    fixture.Connect(dot, "Result", sum, "A");
    fixture.Connect(dot2, "Result", sum, "B");
    const Guid total = fixture.Add("Material.Add");
    fixture.Connect(sum, "Result", total, "A");
    fixture.Connect(one, "Result", total, "B");
    fixture.Connect(total, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(fixture, out, source));
    MaterialConstants constants(env.allocator);
    REQUIRE(BuildMaterialConstants(out.module, constants));
    // float3 둘(RGB, Double3 XYZ의 tile·offset — 값 번호 순으로 칸 0–2), float2 RG(칸 3 xy),
    // float1(칸 0 w).
    REQUIRE(constants.members.Size() == 5);
    CHECK(constants.floatVectors == 4);
    uint32 doubles = 0;
    uint32 tileVector = 0;
    for (const MaterialConstantMember& member : constants.members) {
        tileVector =
            member.kind == MaterialConstantSlotKind::DoubleTile ? member.vector : tileVector;
        CHECK(member.firstComponent + member.components <= 4);
        if (member.kind != MaterialConstantSlotKind::Value) {
            ++doubles;
            CHECK(member.components == 3);
        }

        if (member.components == 1) {
            CHECK(member.vector == 0);
            CHECK(member.firstComponent == 3);
        }

        if (member.components == 2) {
            CHECK(member.vector == 3);
        }
    }

    CHECK(doubles == 2);
    SystemAllocator scratch;
    StringBuilder<> expected(scratch);
    expected << "MakeMaterialDouble3(materialFloats[" << tileVector << "].xyz, materialFloats["
             << tileVector + 1 << "].xyz)";
    REQUIRE(expected.Status());
    CHECK(HasText(source.text.View(), expected.View()));
    CHECK(HasText(source.text.View(), "materialFloats[0].w"));
    CHECK(HasText(source.text.View(), "materialFloats[3].xy"));
}

// 파라미터가 없으면 상수 블록도 없다.
TEST(MaterialParameter_NoConstantBlockWithoutParameters) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid value = AddVarying(fixture, 0.5f);
    fixture.Connect(value, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(fixture, out, source));
    CHECK(!HasText(source.text.View(), "cbuffer"));
    CHECK(HasText(source.text.View(), "#define MATERIAL_NUM_CONSTANT_VECTORS 0\n"));
    CHECK(HasText(source.text.View(), "#define MATERIAL_NUM_COLLECTIONS 0\n"));
}

// VectorParameter 출력 마스크: RGB Float3, R…A Float1, RGBA Float4.
TEST(MaterialParameter_VectorOutputs) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid tint = AddVector(fixture, TintId, LinearColor(0.1f, 0.2f, 0.3f, 0.4f));
    const Guid dot = fixture.Add("Material.DotProduct");
    fixture.Connect(tint, "RGB", dot, "A");
    fixture.Connect(tint, "RGB", dot, "B");
    const Guid sum = fixture.Add("Material.Add");
    fixture.Connect(tint, "B", sum, "A");
    fixture.Connect(dot, "Result", sum, "B");
    fixture.Connect(sum, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateWith(fixture, Traits(), out, {}));
    REQUIRE(out.succeeded);
    const IrResult* ao = FindResult(out, "AmbientOcclusion");
    REQUIRE(ao != nullptr);
    const IrValue& total = ValueOf(out, ao->value);
    REQUIRE(total.op == IrOp::Add);
    const IrValue& blue = ValueOf(out, total.operands[0]);
    CHECK(blue.op == IrOp::Swizzle);
    CHECK(blue.immediate[0] == 2);
    const IrValue& product = ValueOf(out, total.operands[1]);
    REQUIRE(product.op == IrOp::Dot);
    CHECK(ValueOf(out, product.operands[0]).type == IrFloat(3));
}

// DoubleVectorParameter는 Double4 Parameter이고 XYZ는 Double3다.
TEST(MaterialParameter_DoubleVector) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    DoubleVectorParameterProperties origin;
    origin.parameterName = env.Intern("Origin");
    origin.parameterId = OriginId;
    origin.defaultValue = Vec4d(1.0e9, 2.0, 3.0, 4.0);
    const Guid node = fixture.Add("Material.DoubleVectorParameter", env.Save(origin));
    const Guid world = fixture.Add("Material.WorldPosition");
    const Guid relative = fixture.Add("Material.Subtract");
    fixture.Connect(world, "XYZ", relative, "A");
    fixture.Connect(node, "XYZ", relative, "B");
    ToAo(fixture, relative, "Result");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateWith(fixture, Traits(), out, {}));
    REQUIRE(out.succeeded);
    const IrValue* parameter = FindOp(out, IrOp::Parameter);
    REQUIRE(parameter != nullptr);
    CHECK(parameter->type == IrDouble(4));
    REQUIRE(out.module.Parameters().Size() == 1);
    CHECK(out.module.Parameters()[0].kind == IrParameterKind::DoubleVector);
    CHECK(out.module.Parameters()[0].defaults[0] == 1.0e9);
}

// custom primitive data: 번호 인자의 CustomPrimitiveData1·4 문맥 값, UI 영역은 V6(UE 문구), 범위
// 밖 번호는 InvalidProperty다.
TEST(MaterialParameter_CustomPrimitiveData) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    ScalarParameterProperties scalar;
    scalar.parameterName = env.Intern("Data");
    scalar.useCustomPrimitiveData = true;
    scalar.primitiveDataIndex = 5;
    const Guid node = fixture.Add("Material.ScalarParameter", env.Save(scalar));
    VectorParameterProperties vector(env.allocator);
    vector.parameterName = env.Intern("Colors");
    vector.useCustomPrimitiveData = true;
    vector.primitiveDataIndex = 34;
    const Guid colors = fixture.Add("Material.VectorParameter", env.Save(vector));
    const Guid sum = fixture.Add("Material.Add");
    fixture.Connect(node, "Result", sum, "A");
    fixture.Connect(colors, "R", sum, "B");
    fixture.Connect(sum, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(fixture, out, source));
    CHECK(out.module.Parameters().Size() == 0);
    bool single = false;
    bool four = false;
    for (const IrValue& value : out.module.Values()) {
        // 인자 없는 문맥 값(표면 기본 정점 기저 등)은 건너뛴다.
        if (value.op != IrOp::Extern || value.operandCount == 0) {
            continue;
        }

        const IrValue& index = ValueOf(out, value.operands[0]);
        if (value.immediate[0] == ContextIndex("CustomPrimitiveData1")) {
            single = index.immediate[0] == 5;
        } else if (value.immediate[0] == ContextIndex("CustomPrimitiveData4")) {
            four = index.immediate[0] == 34 && value.type == IrFloat(4);
        }
    }

    CHECK(single);
    CHECK(four);
    CHECK(!HasText(source.text.View(), "cbuffer MaterialConstants"));

    GraphFixture ui(env);
    const Guid uiNode = ui.Add("Material.ScalarParameter", env.Save(scalar));
    ui.Connect(uiNode, "Result", OutputNodeId, "Opacity");
    REQUIRE(ui.ready);
    MaterialTranslation rejected(env.allocator);
    REQUIRE(TranslateWith(ui, Traits(BlendMode::AlphaComposite, MaterialDomain::UserInterface),
                          rejected, {}));
    CHECK(HasOnlyErrorAt(rejected, MaterialDiagnosticCode::UnsupportedCapability, uiNode));
    CHECK(HasMessage(rejected, "Custom Primitive Data can't be used with the UI material domain."));

    GraphFixture range(env);
    scalar.primitiveDataIndex = 36;
    const Guid far = range.Add("Material.ScalarParameter", env.Save(scalar));
    range.Connect(far, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(range.ready);
    MaterialTranslation outside(env.allocator);
    REQUIRE(TranslateWith(range, Traits(), outside, {}));
    CHECK(HasOnlyErrorAt(outside, MaterialDiagnosticCode::InvalidProperty, far));
}

// ChannelMaskParameter: 입력이 없으면 UE 문구, 스칼라 입력은 Float4로 복제한 뒤 Vector 파라미터와
// Dot이다.
TEST(MaterialParameter_ChannelMask) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture missing(env);
    ChannelMaskParameterProperties properties(env.allocator);
    properties.parameterName = env.Intern("Mask");
    properties.parameterId = MaskId;
    const Guid lonely = missing.Add("Material.ChannelMaskParameter", env.Save(properties));
    missing.Connect(lonely, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(missing.ready);
    MaterialTranslation rejected(env.allocator);
    REQUIRE(TranslateWith(missing, Traits(), rejected, {}));
    CHECK(HasOnlyErrorAt(rejected, MaterialDiagnosticCode::MissingInput, lonely));
    CHECK(HasMessage(rejected, "Missing mask input"));

    GraphFixture fixture(env);
    const Guid node = fixture.Add("Material.ChannelMaskParameter", env.Save(properties));
    const Guid input = AddVarying(fixture, 0.5f);
    fixture.Connect(input, "Result", node, "Input");
    fixture.Connect(node, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateWith(fixture, Traits(), out, {}));
    REQUIRE(out.succeeded);
    const IrResult* ao = FindResult(out, "AmbientOcclusion");
    REQUIRE(ao != nullptr);
    const IrValue& dot = ValueOf(out, ao->value);
    REQUIRE(dot.op == IrOp::Dot);
    CHECK(ValueOf(out, dot.operands[0]).op == IrOp::Splat);
    CHECK(ValueOf(out, dot.operands[1]).op == IrOp::Parameter);
    REQUIRE(out.module.Parameters().Size() == 1);
    CHECK(out.module.Parameters()[0].defaults[0] == 1.0);
    CHECK(out.module.Parameters()[0].defaults[1] == 0.0);
}

// StaticBoolParameter: 입력 static 값이 우선이고 없으면 defaultValue다(id·종류가 맞아야 한다).
TEST(MaterialParameter_StaticBool) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    const auto run = [&](Span<const MaterialStaticParameterValue> statics, bool& value) noexcept {
        GraphFixture fixture(env);
        StaticBoolParameterProperties properties;
        properties.parameterName = env.Intern("UseDetail");
        properties.parameterId = SwitchId;
        properties.defaultValue = true;
        const Guid node = fixture.Add("Material.StaticBoolParameter", env.Save(properties));
        const Guid choose = fixture.Add("Material.StaticSwitch");
        fixture.Connect(node, "Result", choose, "Value");
        fixture.Connect(AddConstant(fixture, 1.0f), "Result", choose, "A");
        fixture.Connect(AddConstant(fixture, 0.0f), "Result", choose, "B");
        fixture.Connect(choose, "Result", OutputNodeId, "AmbientOcclusion");
        MaterialTranslation out(env.allocator);
        TranslateOptions options;
        options.statics = statics;
        if (!fixture.ready || !TranslateWith(fixture, Traits(), out, options) || !out.succeeded) {
            return false;
        }

        const float one[] = {1.0f};
        value = IsFloatResult(out, "AmbientOcclusion", one);
        return true;
    };

    bool value = false;
    REQUIRE(run({}, value));
    CHECK(value);
    const MaterialStaticParameterValue off[] = {
        {SwitchId, MaterialStaticParameterKind::Bool, false}};
    REQUIRE(run(off, value));
    CHECK(!value);
    const MaterialStaticParameterValue wrongKind[] = {
        {SwitchId, MaterialStaticParameterKind::ComponentMask, false}};
    REQUIRE(run(wrongKind, value));
    CHECK(value);
}

// StaticSwitchParameter: A·B 둘 다 필요(UE 문구 둘)하고, 고른 가지만 번역한다(고르지 않은 가지의
// 오류 노드는 진단하지 않는다).
TEST(MaterialParameter_StaticSwitch) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    StaticSwitchParameterProperties properties;
    properties.parameterName = env.Intern("Detail");
    properties.parameterId = SwitchId;
    properties.defaultValue = false;

    GraphFixture missing(env);
    const Guid lonely = missing.Add("Material.StaticSwitchParameter", env.Save(properties));
    missing.Connect(lonely, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(missing.ready);
    MaterialTranslation rejected(env.allocator);
    REQUIRE(TranslateWith(missing, Traits(), rejected, {}));
    CHECK(CountErrors(rejected) == 2);
    CHECK(HasMessage(rejected, "Missing A input"));
    CHECK(HasMessage(rejected, "Missing B input"));

    GraphFixture fixture(env);
    const Guid node = fixture.Add("Material.StaticSwitchParameter", env.Save(properties));
    const Guid broken = fixture.Add("Material.FontSample");
    fixture.Connect(broken, "R", node, "A");
    fixture.Connect(AddConstant(fixture, 0.25f), "Result", node, "B");
    fixture.Connect(node, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateWith(fixture, Traits(), out, {}));
    CHECK(out.succeeded);
    const float quarter[] = {0.25f};
    CHECK(IsFloatResult(out, "AmbientOcclusion", quarter));

    const MaterialStaticParameterValue on[] = {{SwitchId, MaterialStaticParameterKind::Bool, true}};
    MaterialTranslation chosen(env.allocator);
    TranslateOptions options;
    options.statics = on;
    REQUIRE(TranslateWith(fixture, Traits(), chosen, options));
    CHECK(!chosen.succeeded);
    CHECK(HasOnlyErrorAt(chosen, MaterialDiagnosticCode::PrerequisiteMissing, broken));
}

// StaticComponentMaskParameter: 기본 마스크와 입력 static mask, 입력 없음 문구.
TEST(MaterialParameter_StaticComponentMask) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    StaticComponentMaskParameterProperties properties;
    properties.parameterName = env.Intern("Channels");
    properties.parameterId = MaskId;
    properties.defaultG = true;

    GraphFixture missing(env);
    const Guid lonely = missing.Add("Material.StaticComponentMaskParameter", env.Save(properties));
    missing.Connect(lonely, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(missing.ready);
    MaterialTranslation rejected(env.allocator);
    REQUIRE(TranslateWith(missing, Traits(), rejected, {}));
    CHECK(HasOnlyErrorAt(rejected, MaterialDiagnosticCode::MissingInput, lonely));
    CHECK(HasMessage(rejected, "Missing ComponentMaskParameter input"));

    GraphFixture fixture(env);
    const Guid node = fixture.Add("Material.StaticComponentMaskParameter", env.Save(properties));
    const Guid uv = fixture.Add("Material.TextureCoordinate");
    fixture.Connect(uv, "Result", node, "Input");
    fixture.Connect(node, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateWith(fixture, Traits(), out, {}));
    REQUIRE(out.succeeded);
    const IrResult* ao = FindResult(out, "AmbientOcclusion");
    REQUIRE(ao != nullptr);
    CHECK(ValueOf(out, ao->value).op == IrOp::Swizzle);
    CHECK(ValueOf(out, ao->value).immediate[0] == 1);

    MaterialStaticParameterValue mask{MaskId, MaterialStaticParameterKind::ComponentMask, false};
    mask.mask[0] = true;
    const MaterialStaticParameterValue statics[] = {mask};
    TranslateOptions options;
    options.statics = statics;
    MaterialTranslation overridden(env.allocator);
    REQUIRE(TranslateWith(fixture, Traits(), overridden, options));
    REQUIRE(overridden.succeeded);
    const IrResult* red = FindResult(overridden, "AmbientOcclusion");
    REQUIRE(red != nullptr);
    CHECK(ValueOf(overridden, red->value).immediate[0] == 0);

    // 입력 폭을 넘는 마스크(B)는 ComponentMask와 같은 오류다.
    mask.mask[0] = false;
    mask.mask[2] = true;
    const MaterialStaticParameterValue wide[] = {mask};
    options.statics = wide;
    MaterialTranslation tooWide(env.allocator);
    REQUIRE(TranslateWith(fixture, Traits(), tooWide, options));
    CHECK(HasOnlyErrorAt(tooWide, MaterialDiagnosticCode::TypeMismatch, node));
}

// ParameterCollectionValue: 컬렉션 없음·파라미터 없음(UE 문구), scalar는 칸 성분·vector는 칸,
// 원문은 컬렉션 cbuffer다.
TEST(MaterialParameter_CollectionValue) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    const MaterialCollectionLayout layouts[] = {{CollectionA, TestEntries, 7}};
    TranslateOptions options;
    options.collections = layouts;

    GraphFixture invalid(env);
    const Guid unknown = AddCollectionValue(invalid, CollectionB, FogId);
    invalid.Connect(unknown, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(invalid.ready);
    MaterialTranslation noCollection(env.allocator);
    REQUIRE(TranslateWith(invalid, Traits(), noCollection, options));
    CHECK(HasOnlyErrorAt(noCollection, MaterialDiagnosticCode::InvalidProperty, unknown));
    CHECK(HasMessage(noCollection, "CollectionParameter has invalid Collection!"));

    GraphFixture missing(env);
    const Guid stray = AddCollectionValue(missing, CollectionA, OriginId, "Ghost");
    missing.Connect(stray, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(missing.ready);
    MaterialTranslation noParameter(env.allocator);
    REQUIRE(TranslateWith(missing, Traits(), noParameter, options));
    CHECK(HasOnlyErrorAt(noParameter, MaterialDiagnosticCode::InvalidProperty, stray));
    CHECK(HasMessage(noParameter, "CollectionParameter has invalid parameter Ghost"));

    GraphFixture fixture(env);
    const Guid fog = AddCollectionValue(fixture, CollectionA, FogId);
    const Guid wind = AddCollectionValue(fixture, CollectionA, WindId, "Wind");
    const Guid product = fixture.Add("Material.Multiply");
    fixture.Connect(fog, "Result", product, "A");
    fixture.Connect(wind, "Result", product, "B");
    ToAo(fixture, product, "Result");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(fixture, out, source, options));
    REQUIRE(out.module.Collections().Size() == 1);
    CHECK(out.module.Collections()[0].asset == CollectionA);
    CHECK(out.module.Collections()[0].vectors == 7);
    const IrValue* value = FindOp(out, IrOp::CollectionValue);
    REQUIRE(value != nullptr);
    CHECK(value->frequency == IrFrequency::Instance);
    CHECK(
        HasText(source.text.View(),
                "cbuffer MaterialCollection0\n{\n    float4 materialCollection0Vectors[7];\n};\n"));
    CHECK(HasText(source.text.View(), " = materialCollection0Vectors[0];\n"));
    CHECK(HasText(source.text.View(), " = materialCollection0Vectors[1];\n"));
    CHECK(HasText(source.text.View(), "#define MATERIAL_NUM_COLLECTIONS 1\n"));
    // 컬렉션 값은 상수 식 프로그램에 들어가지 않는다.
    MaterialConstants constants(env.allocator);
    REQUIRE(BuildMaterialConstants(out.module, constants));
    CHECK(constants.members.Size() == 0);
}

// 세 번째 컬렉션은 V9(UE 문구)다. 같은 컬렉션을 다시 쓰는 것은 세지 않는다.
TEST(MaterialParameter_CollectionLimit) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    const MaterialCollectionLayout layouts[] = {{CollectionA, TestEntries, 7},
                                                {CollectionB, TestEntries, 7},
                                                {CollectionC, TestEntries, 7}};
    TranslateOptions options;
    options.collections = layouts;
    GraphFixture fixture(env);
    const Guid a = AddCollectionValue(fixture, CollectionA, FogId);
    const Guid again = AddCollectionValue(fixture, CollectionA, WindId, "Wind");
    const Guid b = AddCollectionValue(fixture, CollectionB, FogId);
    const Guid c = AddCollectionValue(fixture, CollectionC, FogId);
    const Guid first = fixture.Add("Material.Multiply");
    fixture.Connect(a, "Result", first, "A");
    fixture.Connect(again, "Result", first, "B");
    const Guid second = fixture.Add("Material.Add");
    fixture.Connect(first, "Result", second, "A");
    fixture.Connect(b, "Result", second, "B");
    const Guid third = fixture.Add("Material.Add");
    fixture.Connect(second, "Result", third, "A");
    fixture.Connect(c, "Result", third, "B");
    ToAo(fixture, third, "Result");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateWith(fixture, Traits(), out, options));
    CHECK(!out.succeeded);
    CHECK(CountErrors(out) == 1);
    CHECK(HasMessage(out, "Material references too many MaterialParameterCollections!  A material "
                          "may only reference 2 different collections."));
    bool atThird = false;
    for (const nodegraph::GraphDiagnostic& diagnostic : out.diagnostics.items) {
        atThird = atThird || (diagnostic.node == c || diagnostic.node == b);
    }

    CHECK(atThird);
}

// ParameterCollectionTransform: UE 검사 순서·문구와 변환 종류별 결과 타입.
TEST(MaterialParameter_CollectionTransform) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    const MaterialCollectionLayout layouts[] = {{CollectionA, TestEntries, 7}};
    const MaterialCollectionLayout shortLayouts[] = {{CollectionA, TestEntries, 5}};
    TranslateOptions options;
    options.collections = layouts;
    const auto run = [&](CollectionTransformType type, Guid parameter, StringView name,
                         bool connect, StringView message, const TranslateOptions& use,
                         IrType& result) noexcept {
        GraphFixture fixture(env);
        const Guid node = AddCollectionTransform(fixture, CollectionA, parameter, type, name);
        if (connect) {
            const Guid uv = fixture.Add("Material.TextureCoordinate");
            const Guid append = fixture.Add("Material.Append");
            fixture.Connect(uv, "Result", append, "A");
            fixture.Connect(AddConstant(fixture, 1.0f), "Result", append, "B");
            fixture.Connect(append, "Result", node, "Input");
        }

        const Guid dot = fixture.Add("Material.DotProduct");
        fixture.Connect(node, "Result", dot, "A");
        fixture.Connect(node, "Result", dot, "B");
        fixture.Connect(dot, "Result", OutputNodeId, "AmbientOcclusion");
        MaterialTranslation out(env.allocator);
        if (!fixture.ready || !TranslateWith(fixture, Traits(), out, use)) {
            return false;
        }

        if (!message.IsEmpty()) {
            return HasOnlyErrorAt(out,
                                  message == StringView("CollectionTransform missing input")
                                      ? MaterialDiagnosticCode::MissingInput
                                      : MaterialDiagnosticCode::InvalidProperty,
                                  node) &&
                   HasMessage(out, message);
        }

        if (!out.succeeded) {
            return false;
        }

        const IrResult* ao = FindResult(out, "AmbientOcclusion");
        // Dot이 Double을 낮췄으면(Convert) 그 아래가 변환 결과다.
        const IrValue* operand = &ValueOf(out, ValueOf(out, ao->value).operands[0]);
        if (operand->op == IrOp::Convert) {
            operand = &ValueOf(out, operand->operands[0]);
        }

        result = operand->type;
        return out.module.Collections().Size() == 1;
    };

    IrType type;
    CHECK(run(CollectionTransformType::Position, MatrixId, "Matrix", false,
              "CollectionTransform missing input", options, type));
    CHECK(run(CollectionTransformType::Position, OriginId, "Ghost", true,
              "CollectionTransform has invalid parameter Ghost", options, type));
    CHECK(run(CollectionTransformType::Position, FogId, "Fog", true,
              "CollectionTransform parameter Fog is scalar, vectors are required", options, type));
    TranslateOptions shortOptions;
    shortOptions.collections = shortLayouts;
    CHECK(run(CollectionTransformType::LocalToWorld, MatrixId, "Matrix", true,
              "CollectionTransform parameter Matrix requires 5 vectors for LWC Matrix",
              shortOptions, type));
    CHECK(run(CollectionTransformType::Position, MatrixId, "Matrix", true, StringView(), options,
              type));
    CHECK(type == IrFloat(3));
    CHECK(run(CollectionTransformType::Vector, MatrixId, "Matrix", true, StringView(), options,
              type));
    CHECK(type == IrFloat(3));
    CHECK(run(CollectionTransformType::Projection, MatrixId, "Matrix", true, StringView(), options,
              type));
    CHECK(type == IrFloat(4));
    CHECK(run(CollectionTransformType::LocalToWorld, MatrixId, "Matrix", true, StringView(),
              options, type));
    CHECK(type == IrDouble(3));
    CHECK(run(CollectionTransformType::WorldToLocal, MatrixId, "Matrix", true, StringView(),
              options, type));
    CHECK(type == IrFloat(3));

    // 컬렉션이 입력에 없으면 "invalid Collection"이 파라미터 검사보다 먼저다.
    CHECK(run(CollectionTransformType::Position, OriginId, "Ghost", true,
              "CollectionTransform has invalid Collection!", TranslateOptions{}, type));
}

// 같은 ParameterId를 다른 종류(Scalar·Vector)가 쓰면 V8이다(엉뚱한 V1이 아니다).
TEST(MaterialParameter_SameIdOtherKind) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid scalar = AddScalar(fixture, RoughnessId, 0.5f, "Rough");
    VectorParameterProperties vector(env.allocator);
    vector.parameterName = env.Intern("Tint");
    vector.parameterId = RoughnessId;
    const Guid tint = fixture.Add("Material.VectorParameter", env.Save(vector));
    const Guid sum = fixture.Add("Material.Add");
    fixture.Connect(scalar, "Result", sum, "A");
    fixture.Connect(tint, "R", sum, "B");
    fixture.Connect(sum, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateWith(fixture, Traits(), out, {}));
    CHECK(!out.succeeded);
    CHECK(CountErrors(out) == 1);
    CHECK(CountDiagnostics(out, MaterialDiagnosticCode::ParameterConflict,
                           nodegraph::GraphDiagnosticSeverity::Error) == 1);
    CHECK(HasMessage(out, "Parameter is used with another parameter type"));
}

// 상수 식 프로그램: 공유 부분식은 명령 하나·출력 하나이고, Double1 출력은 같은 float4의 두 칸이다.
TEST(MaterialParameter_ProgramSharingAndDoubleScalar) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid scalar = AddScalar(fixture, RoughnessId, 0.5f);
    const Guid plus = fixture.Add("Material.Add");
    fixture.Connect(scalar, "Result", plus, "A");
    fixture.Connect(AddConstant(fixture, 1.0f), "Result", plus, "B");
    const Guid uv = fixture.Add("Material.TextureCoordinate");
    const Guid u = AddMask(fixture, true);
    fixture.Connect(uv, "Result", u, "Input");
    const Guid first = fixture.Add("Material.Multiply");
    fixture.Connect(plus, "Result", first, "A");
    fixture.Connect(u, "Result", first, "B");
    const Guid second = fixture.Add("Material.Subtract");
    fixture.Connect(plus, "Result", second, "A");
    fixture.Connect(u, "Result", second, "B");
    DoubleVectorParameterProperties origin;
    origin.parameterName = env.Intern("Origin");
    origin.parameterId = OriginId;
    origin.defaultValue = Vec4d(3.0e7, 0.0, 0.0, 0.0);
    const Guid wide = fixture.Add("Material.DoubleVectorParameter", env.Save(origin));
    const Guid world = fixture.Add("Material.WorldPosition");
    const Guid x = AddMask(fixture, true);
    fixture.Connect(world, "XYZ", x, "Input");
    const Guid offset = fixture.Add("Material.Subtract");
    fixture.Connect(x, "Result", offset, "A");
    fixture.Connect(wide, "X", offset, "B");
    const Guid sum = fixture.Add("Material.Add");
    fixture.Connect(first, "Result", sum, "A");
    fixture.Connect(second, "Result", sum, "B");
    const Guid total = fixture.Add("Material.Add");
    fixture.Connect(sum, "Result", total, "A");
    fixture.Connect(offset, "Result", total, "B");
    fixture.Connect(total, "Result", OutputNodeId, "AmbientOcclusion");
    REQUIRE(fixture.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateWith(fixture, Traits(), out, {}));
    REQUIRE(out.succeeded);
    MaterialConstants constants(env.allocator);
    REQUIRE(BuildMaterialConstants(out.module, constants));
    usize adds = 0;
    for (const IrValue& step : constants.steps) {
        adds += step.op == IrOp::Add ? 1 : 0;
    }

    CHECK(adds == 1);
    // float1 출력(p + 1)과 Double1 X의 tile·offset 셋이 한 float4에 들어간다.
    REQUIRE(constants.members.Size() == 3);
    CHECK(constants.floatVectors == 1);
    usize doubles = 0;
    for (const MaterialConstantMember& member : constants.members) {
        CHECK(member.vector == 0);
        CHECK(member.components == 1);
        doubles += member.kind != MaterialConstantSlotKind::Value ? 1 : 0;
    }

    CHECK(doubles == 2);
}

// CollectionTransform 식 구조: LocalToWorld는 tile × 2097152(Double 상수)를 더하고, Projection은
// 입력 .xyzz를 쓴다.
TEST(MaterialParameter_CollectionTransformStructure) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    const MaterialCollectionLayout layouts[] = {{CollectionA, TestEntries, 7}};
    TranslateOptions options;
    options.collections = layouts;
    const auto build = [&](CollectionTransformType type, MaterialTranslation& out) noexcept {
        GraphFixture fixture(env);
        const Guid node = AddCollectionTransform(fixture, CollectionA, MatrixId, type);
        const Guid uv = fixture.Add("Material.TextureCoordinate");
        const Guid append = fixture.Add("Material.Append");
        fixture.Connect(uv, "Result", append, "A");
        fixture.Connect(AddConstant(fixture, 1.0f), "Result", append, "B");
        fixture.Connect(append, "Result", node, "Input");
        const Guid dot = fixture.Add("Material.DotProduct");
        fixture.Connect(node, "Result", dot, "A");
        fixture.Connect(node, "Result", dot, "B");
        fixture.Connect(dot, "Result", OutputNodeId, "AmbientOcclusion");
        return fixture.ready && TranslateWith(fixture, Traits(), out, options) && out.succeeded;
    };

    MaterialTranslation world(env.allocator);
    REQUIRE(build(CollectionTransformType::LocalToWorld, world));
    bool tile = false;
    for (const IrValue& value : world.module.Values()) {
        if (value.op != IrOp::Constant || value.type.scalar != IrScalar::Double) {
            continue;
        }

        for (uint8 component = 0; component < value.type.components; ++component) {
            double number = 0.0;
            const uint64 bits = value.immediate[component];
            platform::MemoryCopy(&number, &bits, sizeof(number));
            tile = tile || number == IrDoubleTileSize;
        }
    }

    CHECK(tile);
    MaterialTranslation projection(env.allocator);
    REQUIRE(build(CollectionTransformType::Projection, projection));
    bool spread = false;
    for (const IrValue& value : projection.module.Values()) {
        spread = spread || (value.op == IrOp::Swizzle && value.type == IrFloat(4) &&
                            value.immediate[0] == (0u | (1u << 2) | (2u << 4) | (2u << 6)));
    }

    CHECK(spread);
}

} // namespace material_parameter_lowering_tests_file
