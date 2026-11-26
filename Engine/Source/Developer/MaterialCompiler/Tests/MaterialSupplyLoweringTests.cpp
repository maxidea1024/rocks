#include <MaterialCompiler/MaterialContextSource.h>
#include <MaterialSchema/MaterialContextValue.h>
#include <MaterialSchema/Nodes/Constants/Constant3Properties.generated.h>
#include <MaterialSchema/Nodes/Constants/Constant3Properties.h>
#include <MaterialSchema/Nodes/DistanceFields/DistanceFieldApproxAoProperties.generated.h>
#include <MaterialSchema/Nodes/DistanceFields/DistanceFieldApproxAoProperties.h>
#include <MaterialSchema/Nodes/Math/ComponentMaskProperties.generated.h>
#include <MaterialSchema/Nodes/Math/ComponentMaskProperties.h>
#include <MaterialSchema/Nodes/Particles/DynamicParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Particles/DynamicParameterProperties.h>
#include <MaterialSchema/Nodes/PhysicsField/SamplePhysicsVectorFieldProperties.generated.h>
#include <MaterialSchema/Nodes/PhysicsField/SamplePhysicsVectorFieldProperties.h>
#include <MaterialSchema/Nodes/Sky/SkyAtmosphereLightDiskLuminanceProperties.generated.h>
#include <MaterialSchema/Nodes/Sky/SkyAtmosphereLightDiskLuminanceProperties.h>
#include <MaterialSchema/Nodes/SpeedTree/SpeedTreeProperties.generated.h>
#include <MaterialSchema/Nodes/SpeedTree/SpeedTreeProperties.h>
#include <MaterialSchema/Nodes/Surface/HairAttributesProperties.generated.h>
#include <MaterialSchema/Nodes/Surface/HairAttributesProperties.h>

#include "MaterialGraphFixture.h"

// MS2b4b 시험이다(Docs/Design/MaterialSystem 6.9절 공급 노드): UE 기본 번역기의 인자 기본값(광원
// 번호 자름·원판 각 −1·카메라 반대 방향·PixelDepth·VertexNormal)·검사(scene depth 영역·타입)·식
// (ApproxAO 단계 계산·LWC 위치 변환)·묶음 인자(4 피연산자)·공급하지 않을 때 인자 기본값
// (DynamicParameter)을 본다.
using namespace rocks;
using namespace rocks::material;
namespace material_supply_lowering_tests_file {

using namespace material_graph_fixture;

namespace {

uint64 ContextIndex(StringView name) noexcept {
    const Span<const MaterialContextValue> table = MaterialContextValues();
    for (usize index = 0; index < table.Size(); ++index) {
        if (table[index].name == name) {
            return index;
        }
    }

    return table.Size();
}

// 모듈에서 name 문맥 값을 찾는다(첫 것).
const IrValue* FindExtern(const MaterialTranslation& out, StringView name) noexcept {
    for (const IrValue& value : out.module.Values()) {
        if (value.op == IrOp::Extern && value.immediate[0] == ContextIndex(name)) {
            return &value;
        }
    }

    return nullptr;
}

Guid AddMask(GraphFixture& fixture, bool r) noexcept {
    ComponentMaskProperties properties;
    properties.r = r;
    return fixture.Add("Material.ComponentMask", fixture.env.Save(properties));
}

// node의 pin을 마스크 R로 AO에 잇고 번역한다.
bool TranslateToAo(GraphFixture& fixture, Guid node, StringView pin, MaterialTranslation& out,
                   const MaterialTraitsDesc& traits = Traits()) noexcept {
    const Guid mask = AddMask(fixture, true);
    fixture.Connect(node, pin, mask, "Input");
    fixture.Connect(mask, "Result", OutputNodeId, "AmbientOcclusion");
    return Translate(fixture, traits, out).IsOk();
}

bool HasText(StringView text, StringView part) noexcept {
    for (usize at = 0; at + part.ByteCount() <= text.ByteCount(); ++at) {
        if (StringView(text.Data() + at, part.ByteCount()) == part) {
            return true;
        }
    }

    return false;
}

} // namespace

// DynamicParameter: (기본값, 자른 번호) 인자, Param 출력은 성분. 공급하지 않으면 기본값 인자다.
TEST(MaterialSupply_DynamicParameter) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    DynamicParameterProperties properties(env.allocator);
    properties.parameterIndex = 7;
    const Guid node = fixture.Add("Material.DynamicParameter", env.Save(properties));
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateToAo(fixture, node, "Param2", out));
    REQUIRE(out.succeeded);
    const IrValue* value = FindExtern(out, "DynamicParameter");
    REQUIRE(value != nullptr);
    REQUIRE(value->operandCount == 2);
    CHECK(ValueOf(out, value->operands[0]).type == IrFloat(4));
    CHECK(ValueOf(out, value->operands[1]).immediate[0] == 3);

    String text(env.allocator);
    REQUIRE(WriteMaterialContextSource(env.allocator, text));
    CHECK(HasText(text.View(), "#define MaterialContextDynamicParameter(c, a0, a1) (a0)\n"));
}

// SphericalParticleOpacity는 scene depth를 읽어 불투명 Surface에서 V6, 반투명에서 통과한다.
TEST(MaterialSupply_SphericalParticleOpacityDepth) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture opaque(env);
    const Guid node = opaque.Add("Material.SphericalParticleOpacity");
    opaque.Connect(node, "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation rejected(env.allocator);
    REQUIRE(Translate(opaque, Traits(), rejected));
    CHECK(HasOnlyErrorAt(rejected, MaterialDiagnosticCode::UnsupportedCapability, node));

    GraphFixture translucent(env);
    const Guid particle = translucent.Add("Material.SphericalParticleOpacity");
    translucent.Connect(particle, "Result", OutputNodeId, "Opacity");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(translucent, Traits(BlendMode::AlphaComposite), out));
    CHECK(out.succeeded);
}

// 하늘: 광원 번호는 0–1로 자르고, 원판 각이 없으면 −1이다.
TEST(MaterialSupply_SkyDefaults) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    SkyAtmosphereLightDiskLuminanceProperties properties;
    properties.lightIndex = 5;
    const Guid node = fixture.Add("Material.SkyAtmosphereLightDiskLuminance", env.Save(properties));
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateToAo(fixture, node, "Result", out));
    REQUIRE(out.succeeded);
    const IrValue* value = FindExtern(out, "SkyAtmosphereLightDiskLuminance");
    REQUIRE(value != nullptr);
    CHECK(ValueOf(out, value->operands[0]).immediate[0] == 1);
    CHECK(FloatOf(ValueOf(out, value->operands[1]), 0) == -1.0f);

    // ViewLuminance 기본 방향은 −CameraVector(픽셀 전용)다.
    GraphFixture view(env);
    const Guid luminance = view.Add("Material.SkyAtmosphereViewLuminance");
    MaterialTranslation viewOut(env.allocator);
    REQUIRE(TranslateToAo(view, luminance, "Result", viewOut));
    REQUIRE(viewOut.succeeded);
    CHECK(FindExtern(viewOut, "CameraVector") != nullptr);
}

// EyeAdaptationInverse: LightValue(기본 (1, 1, 1)) × 역수, Alpha가 Float1이 아니면 V1.
TEST(MaterialSupply_EyeAdaptationInverse) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid node = fixture.Add("Material.EyeAdaptationInverse");
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateToAo(fixture, node, "EyeAdaptationInverse", out));
    REQUIRE(out.succeeded);
    CHECK(FindExtern(out, "EyeAdaptationInverse") != nullptr);

    GraphFixture wrong(env);
    const Guid inverse = wrong.Add("Material.EyeAdaptationInverse");
    const Guid uv = wrong.Add("Material.ScreenPosition");
    wrong.Connect(uv, "ViewportUV", inverse, "AlphaInput");
    MaterialTranslation rejected(env.allocator);
    REQUIRE(TranslateToAo(wrong, inverse, "EyeAdaptationInverse", rejected));
    CHECK(HasOnlyErrorAt(rejected, MaterialDiagnosticCode::TypeMismatch, inverse));
}

// DistanceFieldApproxAo: 단계 수를 1–4로 자르고 (위치, 노멀, (step, S, bias, max), N) 네 인자다.
TEST(MaterialSupply_DistanceFieldApproxAo) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    {
        GraphFixture fixture(env);
        DistanceFieldApproxAoProperties properties;
        properties.numSteps = 0;
        const Guid node = fixture.Add("Material.DistanceFieldApproxAo", env.Save(properties));
        fixture.Connect(node, "Result", OutputNodeId, "AmbientOcclusion");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        REQUIRE(out.succeeded);
        const IrValue* value = FindExtern(out, "DistanceFieldApproxAo");
        REQUIRE(value != nullptr);
        REQUIRE(value->operandCount == 4);
        // N = 1: step 0, S 3, bias = max = Base 15.
        const IrValue& packed = ValueOf(out, value->operands[2]);
        REQUIRE(packed.op == IrOp::Constant);
        CHECK(FloatOf(packed, 0) == 0.0f);
        CHECK(FloatOf(packed, 1) == 3.0f);
        CHECK(FloatOf(packed, 2) == 15.0f);
        CHECK(FloatOf(packed, 3) == 15.0f);
        CHECK(ValueOf(out, value->operands[3]).immediate[0] == 1);
        CHECK(FindExtern(out, "VertexNormal") != nullptr);
    }

    {
        // N = 3, S = 2: step = (150 − 15) / (2² − 1) = 45, bias = 15 − 45 = −30, max = 150.
        GraphFixture fixture(env);
        DistanceFieldApproxAoProperties properties;
        properties.numSteps = 3;
        properties.stepScaleDefault = 2.0f;
        const Guid node = fixture.Add("Material.DistanceFieldApproxAo", env.Save(properties));
        fixture.Connect(node, "Result", OutputNodeId, "AmbientOcclusion");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        REQUIRE(out.succeeded);
        const IrValue* value = FindExtern(out, "DistanceFieldApproxAo");
        REQUIRE(value != nullptr);
        const IrValue& packed = ValueOf(out, value->operands[2]);
        REQUIRE(packed.op == IrOp::Constant);
        CHECK(FloatOf(packed, 0) == 45.0f);
        CHECK(FloatOf(packed, 2) == -30.0f);
        CHECK(FloatOf(packed, 3) == 150.0f);
    }
}

// HairAttributes: U는 HairUv.x, Tangent는 useTangentSpace Bool 인자. 픽셀 전용이라 WPO는 V4.
TEST(MaterialSupply_HairAttributes) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid hair = fixture.Add("Material.HairAttributes", env.Save(HairAttributesProperties{}));
    fixture.Connect(hair, "U", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    CHECK(FindExtern(out, "HairUv") != nullptr);

    fixture.Connect(hair, "Tangent", OutputNodeId, "WorldPositionOffset");
    MaterialTranslation vertex(env.allocator);
    REQUIRE(Translate(fixture, Traits(), vertex));
    CHECK(HasOnlyErrorAt(vertex, MaterialDiagnosticCode::StageViolation, hair));
}

// 물리장: CameraRelative 위치는 TranslatedWorld − PreViewTranslation(Double) 뒤 Float3, fieldTarget
// 인자.
TEST(MaterialSupply_PhysicsField) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    SamplePhysicsVectorFieldProperties properties;
    properties.worldPositionOriginType = PositionOrigin::CameraRelative;
    const Guid node = fixture.Add("Material.SamplePhysicsVectorField", env.Save(properties));
    fixture.Connect(node, "Result", OutputNodeId, "WorldPositionOffset");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    const IrValue* value = FindExtern(out, "SamplePhysicsVectorField");
    REQUIRE(value != nullptr);
    const IrValue& position = ValueOf(out, value->operands[0]);
    CHECK(position.type == IrFloat(3));
    REQUIRE(position.op == IrOp::Convert);
    CHECK(ValueOf(out, position.operands[0]).op == IrOp::Subtract);
    CHECK(FindExtern(out, "TranslatedWorldPosition") != nullptr);
}

// SpeedTree: 정점 전용(표면은 V4), 이전 정점 문맥에서 accurateWindVelocities면 이전 프레임 표지 1.
TEST(MaterialSupply_SpeedTree) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    SpeedTreeProperties properties;
    properties.accurateWindVelocities = true;
    const Guid tree = fixture.Add("Material.SpeedTree", env.Save(properties));
    fixture.Connect(tree, "Result", OutputNodeId, "WorldPositionOffset");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    bool sawPrevious = false;
    bool sawCurrent = false;
    for (const IrValue& value : out.module.Values()) {
        if (value.op != IrOp::Extern ||
            value.immediate[0] != ContextIndex("SpeedTreeVertexOffset")) {
            continue;
        }

        const IrValue& flags = ValueOf(out, value.operands[1]);
        sawPrevious = sawPrevious || FloatOf(flags, 1) == 1.0f;
        sawCurrent = sawCurrent || FloatOf(flags, 1) == 0.0f;
    }

    CHECK(sawPrevious);
    CHECK(sawCurrent);

    GraphFixture pixel(env);
    const Guid wrong = pixel.Add("Material.SpeedTree", env.Save(SpeedTreeProperties{}));
    MaterialTranslation rejected(env.allocator);
    REQUIRE(TranslateToAo(pixel, wrong, "Result", rejected));
    CHECK(HasOnlyErrorAt(rejected, MaterialDiagnosticCode::StageViolation, wrong));
}

// 그 밖: RVT 사용자 데이터는 상수 0, DecalMipmapLevel TextureSize는 정확히 Float2(아니면 V1).
TEST(MaterialSupply_Misc) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid custom = fixture.Add("Material.RuntimeVirtualTextureCustomData");
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateToAo(fixture, custom, "Result", out));
    const float zero[] = {0.0f};
    CHECK(IsFloatResult(out, "AmbientOcclusion", zero));

    GraphFixture decal(env);
    const Guid level = decal.Add("Material.DecalMipmapLevel");
    Constant3Properties size;
    size.constant = LinearColor(1.0f, 1.0f, 1.0f, 1.0f);
    const Guid three = decal.Add("Material.Constant3", env.Save(size));
    decal.Connect(three, "Result", level, "TextureSize");
    decal.Connect(level, "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation rejected(env.allocator);
    REQUIRE(
        Translate(decal, Traits(BlendMode::Translucent, MaterialDomain::DeferredDecal), rejected));
    CHECK(HasOnlyErrorAt(rejected, MaterialDiagnosticCode::TypeMismatch, level));
}

} // namespace material_supply_lowering_tests_file
