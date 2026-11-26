#include <MaterialCompiler/MaterialSourceWriter.h>
#include <MaterialSchema/MaterialContextValue.h>
#include <MaterialSchema/Nodes/Constants/ConstantDoubleProperties.generated.h>
#include <MaterialSchema/Nodes/Constants/ConstantDoubleProperties.h>
#include <MaterialSchema/Nodes/Constants/StaticBoolProperties.generated.h>
#include <MaterialSchema/Nodes/Constants/StaticBoolProperties.h>
#include <MaterialSchema/Nodes/Constants/TimeProperties.generated.h>
#include <MaterialSchema/Nodes/Constants/TimeProperties.h>
#include <MaterialSchema/Nodes/Custom/PerInstanceCustomDataProperties.generated.h>
#include <MaterialSchema/Nodes/Custom/PerInstanceCustomDataProperties.h>
#include <MaterialSchema/Nodes/Custom/PerInstanceCustomData3Properties.generated.h>
#include <MaterialSchema/Nodes/Custom/PerInstanceCustomData3Properties.h>

#include "MaterialGraphFixture.h"

// MS2b1 시험이다(Docs/Design/MaterialSystem 6.9절): 평가 문맥별 번역(정점·이전 정점·픽셀 — 같은
// 노드의 진단은 한 번), 이전 프레임 변형(Time은 이전 값, DeltaTime은 현재 값 — UE
// HLSLMaterialTranslator.cpp:5280·5325), 문맥 값 단계(V4)·영역(V6) 음성 대조와 닿지 않는 노드의
// 무진단, 진입점별 사용 기능·필요 기능 등급(MS-T13 (a)), 문맥 값 노드의 출력 마스크·속성 규칙.
using namespace rocks;
using namespace rocks::material;
namespace material_context_lowering_tests_file {

using namespace material_graph_fixture;

namespace {

// 문맥 값 표에서 name의 번호다(없으면 표 크기).
uint64 ContextIndex(StringView name) noexcept {
    const Span<const MaterialContextValue> table = MaterialContextValues();
    for (usize index = 0; index < table.Size(); ++index) {
        if (table[index].name == name) {
            return index;
        }
    }

    return table.Size();
}

// entry 진입점의 slot 결과다.
const IrResult* FindEntryResult(const MaterialTranslation& out, IrEntry entry,
                                StringView slot) noexcept {
    for (const IrResult& result : out.module.Results()) {
        if (result.entry == entry && result.slot == slot) {
            return &result;
        }
    }

    return nullptr;
}

// root에서 닿는 값 가운데 name 문맥 값(Extern)이 있으면 그 값이다(모듈 번호는 피연산자가 작다).
const IrValue* FindExtern(const MaterialTranslation& out, IrValueId root,
                          StringView name) noexcept {
    const Span<const IrValue> values = out.module.Values();
    const uint64 index = ContextIndex(name);
    for (usize at = 0; at <= root && at < values.Size(); ++at) {
        if (values[at].op == IrOp::Extern && values[at].immediate[0] == index) {
            // root에서 닿는지는 순서만으로 알 수 없으므로 시험 그래프는 그 값만 둔다.
            return &values[at];
        }
    }

    return nullptr;
}

// entry의 slot 결과에서 name 문맥 값을 찾는다.
const IrValue* EntryExtern(const MaterialTranslation& out, IrEntry entry, StringView slot,
                           StringView name) noexcept {
    const IrResult* result = FindEntryResult(out, entry, slot);
    return result == nullptr ? nullptr : FindExtern(out, result->value, name);
}

Guid AddTime(GraphFixture& fixture, bool ignorePause, bool overridePeriod, float period) noexcept {
    TimeProperties properties;
    properties.ignorePause = ignorePause;
    properties.override_Period = overridePeriod;
    properties.period = period;
    return fixture.Add("Material.Time", fixture.env.Save(properties));
}

// 진단 하나가 node 위치의 code 오류이고 문구가 message인가.
bool HasMessageAt(const MaterialTranslation& out, MaterialDiagnosticCode code, Guid node,
                  StringView message) noexcept {
    for (const nodegraph::GraphDiagnostic& diagnostic : out.diagnostics.items) {
        if (diagnostic.code == ToDiagnosticCode(code) && diagnostic.node == node &&
            diagnostic.message.View() == message) {
            return true;
        }
    }

    return false;
}

// text에 part가 있는가.
bool HasText(StringView text, StringView part) noexcept {
    for (usize at = 0; at + part.ByteCount() <= text.ByteCount(); ++at) {
        if (StringView(text.Data() + at, part.ByteCount()) == part) {
            return true;
        }
    }

    return false;
}

} // namespace

// 이전 정점 문맥(6.9절): Time은 이전 프레임 변형, DeltaTime은 현재 값 그대로다(UE
// bCompilingPreviousFrame — DeltaTime은 바꾸지 않는다).
TEST(MaterialContext_PreviousFrameVariant) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid time = AddTime(fixture, false, false, 0.0f);
    fixture.Connect(time, "Result", OutputNodeId, "WorldPositionOffset");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    const IrValue* current = EntryExtern(out, IrEntry::Vertex, "WorldPositionOffset", "Time");
    const IrValue* previous =
        EntryExtern(out, IrEntry::VertexPrevious, "WorldPositionOffset", "Time");
    REQUIRE(current != nullptr);
    REQUIRE(previous != nullptr);
    CHECK(current->immediate[1] == static_cast<uint64>(IrExternVariant::Current));
    // 두 값은 같은 표 번호의 다른 변형이라 hash-consing으로 합쳐지지 않는다.
    bool hasPrevious = false;
    for (const IrValue& value : out.module.Values()) {
        hasPrevious = hasPrevious ||
                      (value.op == IrOp::Extern && value.immediate[0] == ContextIndex("Time") &&
                       value.immediate[1] == static_cast<uint64>(IrExternVariant::Previous));
    }

    CHECK(hasPrevious);
    CHECK(FindEntryResult(out, IrEntry::Vertex, "WorldPositionOffset")->value !=
          FindEntryResult(out, IrEntry::VertexPrevious, "WorldPositionOffset")->value);

    // DeltaTime은 이전 변형이 없어 두 진입점이 같은 값이다.
    GraphFixture delta(env);
    const Guid deltaTime = delta.Add("Material.DeltaTime");
    delta.Connect(deltaTime, "Result", OutputNodeId, "WorldPositionOffset");
    MaterialTranslation deltaOut(env.allocator);
    REQUIRE(Translate(delta, Traits(), deltaOut));
    REQUIRE(deltaOut.succeeded);
    CHECK(FindEntryResult(deltaOut, IrEntry::Vertex, "WorldPositionOffset")->value ==
          FindEntryResult(deltaOut, IrEntry::VertexPrevious, "WorldPositionOffset")->value);
}

// 이전 정점 문맥은 정점 문맥이 WPO 결과를 냈을 때만 번역한다(WPO 비활성 영역 — PostProcess).
TEST(MaterialContext_PreviousOnlyWithOffset) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(BlendMode::Opaque, MaterialDomain::PostProcess), out));
    REQUIRE(out.succeeded);
    CHECK(FindEntryResult(out, IrEntry::Vertex, "WorldPositionOffset") == nullptr);
    CHECK(FindEntryResult(out, IrEntry::VertexPrevious, "WorldPositionOffset") == nullptr);
}

// 여러 문맥에서 번역한 노드의 오류는 하나다(UE HLSLMaterialTranslator.cpp:4446–4455).
TEST(MaterialContext_SharedNodeReportsOnce) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid broken = fixture.Add("Material.Ddx");
    fixture.Connect(broken, "Result", OutputNodeId, "WorldPositionOffset");
    fixture.Connect(broken, "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    CHECK(!out.succeeded);
    CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::MissingInput, broken));
}

// V4(6.9절): 픽셀 전용 값이 정점 진입점에, 정점 전용 값이 픽셀 진입점에 닿으면 오류다. 맞는
// 단계는 통과한다.
TEST(MaterialContext_StageViolation) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    {
        GraphFixture fixture(env);
        const Guid normal = fixture.Add("Material.PixelNormal");
        fixture.Connect(normal, "Result", OutputNodeId, "WorldPositionOffset");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(!out.succeeded);
        CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::StageViolation, normal));
        CHECK(HasMessageAt(out, MaterialDiagnosticCode::StageViolation, normal,
                           "'PixelNormal' is not available in the vertex stage"));
    }

    {
        GraphFixture fixture(env);
        const Guid skinned = fixture.Add("Material.PreSkinnedPosition");
        fixture.Connect(skinned, "Result", OutputNodeId, "AmbientOcclusion");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(!out.succeeded);
        CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::StageViolation, skinned));
        CHECK(HasMessageAt(out, MaterialDiagnosticCode::StageViolation, skinned,
                           "'PreSkinnedPosition' is not available in the pixel stage"));
    }

    {
        // 같은 값이 맞는 단계(정점)에서는 통과한다. 픽셀 전용 CameraVector는 표면에서 통과한다.
        GraphFixture fixture(env);
        const Guid skinned = fixture.Add("Material.PreSkinnedPosition");
        const Guid camera = fixture.Add("Material.CameraVector");
        fixture.Connect(skinned, "Result", OutputNodeId, "WorldPositionOffset");
        fixture.Connect(camera, "Result", OutputNodeId, "AmbientOcclusion");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(out.succeeded);
        CHECK(CountErrors(out) == 0);
    }

    {
        // 미분은 정점 진입점에서 오류다(IR 연산 Ddx).
        GraphFixture fixture(env);
        const Guid derivative = fixture.Add("Material.Ddx");
        fixture.Connect(AddVarying(fixture, 0.5f), "Result", derivative, "Value");
        fixture.Connect(derivative, "Result", OutputNodeId, "WorldPositionOffset");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(!out.succeeded);
        CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::StageViolation, derivative));
    }
}

// V6(6.9절): 표 허용 영역 밖의 값은 오류다. 허용 영역이면 통과하고, 진입점에서 닿지 않는 노드는
// 진단이 없다(DCE 뒤 검사).
TEST(MaterialContext_DomainViolation) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    {
        GraphFixture fixture(env);
        const Guid decal = fixture.Add("Material.DecalColor");
        fixture.Connect(decal, "R", OutputNodeId, "AmbientOcclusion");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(!out.succeeded);
        CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::UnsupportedCapability, decal));
        CHECK(HasMessageAt(out, MaterialDiagnosticCode::UnsupportedCapability, decal,
                           "'DecalColor' is not available in the Surface material domain"));
    }

    {
        GraphFixture fixture(env);
        const Guid decal = fixture.Add("Material.DecalColor");
        fixture.Connect(decal, "R", OutputNodeId, "AmbientOcclusion");
        MaterialTranslation out(env.allocator);
        REQUIRE(
            Translate(fixture, Traits(BlendMode::Translucent, MaterialDomain::DeferredDecal), out));
        CHECK(out.succeeded);
    }

    {
        // 닿지 않는 노드와 비활성 핀(Opaque의 Opacity — V5 경고)의 위쪽은 V6가 없다.
        GraphFixture fixture(env);
        (void)fixture.Add("Material.LightVector");
        const Guid decal = fixture.Add("Material.DecalColor");
        fixture.Connect(decal, "R", OutputNodeId, "Opacity");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(out.succeeded);
        CHECK(CountErrors(out) == 0);
        CHECK(CountDiagnostics(out, MaterialDiagnosticCode::InactiveOutput,
                               nodegraph::GraphDiagnosticSeverity::Warning) == 1);
    }
}

// MS-T13 (a): 진입점별 사용 기능과 필요 기능 등급(6.9절 모듈 기록), 생성 원문의 사용 정의.
TEST(MaterialContext_UsageAndTier) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid lightmap = fixture.Add("Material.LightmapUvs");
    const Guid particle = fixture.Add("Material.ParticleColor");
    const Guid add = fixture.Add("Material.Add");
    fixture.Connect(lightmap, "Result", add, "A");
    fixture.Connect(particle, "RGB", add, "B");
    fixture.Connect(add, "Result", OutputNodeId, "AmbientOcclusion");
    fixture.Connect(particle, "RGB", OutputNodeId, "WorldPositionOffset");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    // LightmapUvs(Float2) + RGB(Float3)는 폭이 맞지 않아 V1이다 — A만 쓴다.
    CHECK(HasError(out, MaterialDiagnosticCode::TypeMismatch));

    GraphFixture valid(env);
    const Guid uvs = valid.Add("Material.LightmapUvs");
    const Guid color = valid.Add("Material.ParticleColor");
    const Guid sum = valid.Add("Material.Add");
    valid.Connect(uvs, "Result", sum, "A");
    valid.Connect(color, "R", sum, "B");
    valid.Connect(sum, "Result", OutputNodeId, "AmbientOcclusion");
    valid.Connect(color, "RGB", OutputNodeId, "WorldPositionOffset");
    MaterialTranslation result(env.allocator);
    REQUIRE(Translate(valid, Traits(), result));
    REQUIRE(result.succeeded);
    const MaterialContextFeatureMask surface = result.module.EntryFeatures(IrEntry::Surface);
    CHECK(surface ==
          (ToMask(MaterialContextFeature::Lightmap) | ToMask(MaterialContextFeature::Particle)));
    CHECK(result.module.EntryFeatures(IrEntry::Vertex) == ToMask(MaterialContextFeature::Particle));
    CHECK(result.module.EntryFeatures(IrEntry::VertexPrevious) ==
          ToMask(MaterialContextFeature::Particle));
    CHECK(result.module.EntryFeatures(IrEntry::Opacity) == NoMaterialContextFeatures);
    // LightmapUvs는 최소 SM5다(UE HLSLMaterialTranslator.cpp:11551).
    CHECK(result.module.RequiredTier() == MaterialCapabilityTier::Sm5);

    MaterialGeneratedSource source(env.allocator);
    REQUIRE(WriteMaterialSource(env.allocator, result.module, Traits(), TestAssetId, MainGraphId,
                                source));
    REQUIRE(source.succeeded);
    const StringView text = source.text.View();
    const StringView present[] = {"#define MATERIAL_VERTEX_USES_PARTICLE 1\n",
                                  "#define MATERIAL_VERTEX_PREVIOUS_USES_PARTICLE 1\n",
                                  "#define MATERIAL_SURFACE_USES_PARTICLE 1\n",
                                  "#define MATERIAL_SURFACE_USES_LIGHTMAP 1\n",
                                  "MaterialContextLightmapUvs(c)",
                                  "MaterialContextParticleColor(c)"};
    for (StringView part : present) {
        CHECK(HasText(text, part));
    }

    const StringView absent[] = {"MATERIAL_VERTEX_USES_LIGHTMAP", "MATERIAL_OPACITY_USES"};
    for (StringView part : absent) {
        CHECK(!HasText(text, part));
    }

    // 깨끗한 Surface는 Mobile이고 사용 기능이 없다.
    GraphFixture empty(env);
    MaterialTranslation plain(env.allocator);
    REQUIRE(Translate(empty, Traits(), plain));
    CHECK(plain.module.RequiredTier() == MaterialCapabilityTier::Mobile);
    CHECK(plain.module.EntryFeatures(IrEntry::Surface) == NoMaterialContextFeatures);
}

// Time 속성(UE HLSLMaterialTranslator.cpp:5276–5323): ignorePause면 RealTime, 주기 0이면 상수 0,
// 주기가 있으면 fmod(시간, 주기).
TEST(MaterialContext_TimeProperties) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    {
        GraphFixture fixture(env);
        fixture.Connect(AddTime(fixture, true, false, 0.0f), "Result", OutputNodeId,
                        "AmbientOcclusion");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        REQUIRE(out.succeeded);
        CHECK(EntryExtern(out, IrEntry::Surface, "AmbientOcclusion", "RealTime") != nullptr);
    }

    {
        GraphFixture fixture(env);
        fixture.Connect(AddTime(fixture, false, true, 0.0f), "Result", OutputNodeId,
                        "AmbientOcclusion");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        const float zero[] = {0.0f};
        CHECK(IsFloatResult(out, "AmbientOcclusion", zero));
    }

    {
        GraphFixture fixture(env);
        fixture.Connect(AddTime(fixture, false, true, 2.0f), "Result", OutputNodeId,
                        "AmbientOcclusion");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        REQUIRE(out.succeeded);
        const IrResult* result = FindEntryResult(out, IrEntry::Surface, "AmbientOcclusion");
        REQUIRE(result != nullptr);
        CHECK(ValueOf(out, result->value).op == IrOp::Fmod);
    }
}

// 출력 마스크: VertexColor Result는 RGB(Float3), 성분 핀은 Float 스칼라다. ParticleSpriteRotation
// Deg는 성분 1이다.
TEST(MaterialContext_OutputMasks) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid color = fixture.Add("Material.VertexColor");
    const Guid rotation = fixture.Add("Material.ParticleSpriteRotation");
    const Guid add = fixture.Add("Material.Add");
    fixture.Connect(color, "A", add, "A");
    fixture.Connect(rotation, "Deg", add, "B");
    fixture.Connect(add, "Result", OutputNodeId, "AmbientOcclusion");
    fixture.Connect(color, "Result", OutputNodeId, "WorldPositionOffset");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    const IrResult* offset = FindEntryResult(out, IrEntry::Vertex, "WorldPositionOffset");
    REQUIRE(offset != nullptr);
    const IrValue& rgb = ValueOf(out, offset->value);
    CHECK(rgb.type == IrFloat(3));
    CHECK(rgb.op == IrOp::Swizzle);
    const IrResult* ao = FindEntryResult(out, IrEntry::Surface, "AmbientOcclusion");
    REQUIRE(ao != nullptr);
    const IrValue& sum = ValueOf(out, ao->value);
    REQUIRE(sum.op == IrOp::Add);
    const IrValue& alpha = ValueOf(out, sum.operands[0]);
    const IrValue& degrees = ValueOf(out, sum.operands[1]);
    CHECK(alpha.op == IrOp::Swizzle);
    CHECK(alpha.immediate[0] == 3);
    CHECK(degrees.op == IrOp::Swizzle);
    CHECK(degrees.immediate[0] == 1);
}

// Ddx·Ddy: Double 입력은 Float로 낮춘다. StaticBool은 Bool 상수라 DynamicBranch 조건이 접힌다.
TEST(MaterialContext_DerivativeAndStaticBool) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    {
        GraphFixture fixture(env);
        ConstantDoubleProperties properties;
        properties.value = 3.0;
        const Guid value = fixture.Add("Material.ConstantDouble", env.Save(properties));
        const Guid derivative = fixture.Add("Material.Ddy");
        fixture.Connect(value, "Result", derivative, "Value");
        fixture.Connect(derivative, "Result", OutputNodeId, "AmbientOcclusion");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        REQUIRE(out.succeeded);
        const IrResult* result = FindEntryResult(out, IrEntry::Surface, "AmbientOcclusion");
        REQUIRE(result != nullptr);
        const IrValue& value0 = ValueOf(out, result->value);
        CHECK(value0.op == IrOp::Ddy);
        CHECK(value0.type == IrFloat(1));
        CHECK(value0.frequency == IrFrequency::Pixel);
    }

    {
        GraphFixture fixture(env);
        StaticBoolProperties properties;
        properties.value = true;
        const Guid condition = fixture.Add("Material.StaticBool", env.Save(properties));
        const Guid branch = fixture.Add("Material.DynamicBranch");
        fixture.Connect(condition, "Result", branch, "Condition");
        fixture.Connect(AddConstant(fixture, 0.25f), "Result", branch, "True");
        fixture.Connect(AddConstant(fixture, 0.75f), "Result", branch, "False");
        fixture.Connect(branch, "Result", OutputNodeId, "AmbientOcclusion");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        const float chosen[] = {0.25f};
        CHECK(IsFloatResult(out, "AmbientOcclusion", chosen));
    }
}

// PerInstanceCustomData: (dataIndex uint, 기본값 Float) 인자의 함수형 값이다. 3Vector의 기본값은
// Float3다.
TEST(MaterialContext_PerInstanceCustomData) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    PerInstanceCustomDataProperties properties;
    properties.dataIndex = 5;
    const Guid data = fixture.Add("Material.PerInstanceCustomData", env.Save(properties));
    PerInstanceCustomData3Properties vectorProperties;
    vectorProperties.dataIndex = 1;
    const Guid vector = fixture.Add("Material.PerInstanceCustomData3", env.Save(vectorProperties));
    fixture.Connect(data, "Result", OutputNodeId, "AmbientOcclusion");
    fixture.Connect(vector, "Result", OutputNodeId, "WorldPositionOffset");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    CHECK(CountDiagnostics(out, MaterialDiagnosticCode::NarrowedValue,
                           nodegraph::GraphDiagnosticSeverity::Info) == 0);
    const IrResult* ao = FindEntryResult(out, IrEntry::Surface, "AmbientOcclusion");
    REQUIRE(ao != nullptr);
    const IrValue& value = ValueOf(out, ao->value);
    REQUIRE(value.op == IrOp::Extern);
    REQUIRE(value.operandCount == 2);
    const IrValue& index = ValueOf(out, value.operands[0]);
    CHECK(index.type == IrUInt(1));
    CHECK(index.immediate[0] == 5);
    CHECK(ValueOf(out, value.operands[1]).type == IrFloat(1));
    const IrResult* offset = FindEntryResult(out, IrEntry::Vertex, "WorldPositionOffset");
    REQUIRE(offset != nullptr);
    const IrValue& vectorValue = ValueOf(out, offset->value);
    REQUIRE(vectorValue.op == IrOp::Extern);
    CHECK(ValueOf(out, vectorValue.operands[1]).type == IrFloat(3));
    CHECK(vectorValue.frequency == IrFrequency::Instance);
}

} // namespace material_context_lowering_tests_file
