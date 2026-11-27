#include <MaterialSchema/MaterialContextValue.h>
#include <MaterialSchema/Nodes/Coordinates/WorldPositionProperties.generated.h>
#include <MaterialSchema/Nodes/Coordinates/WorldPositionProperties.h>
#include <MaterialSchema/Nodes/Vectors/TransformPositionProperties.generated.h>
#include <MaterialSchema/Nodes/Vectors/TransformPositionProperties.h>
#include <MaterialSchema/Nodes/Vectors/TransformVectorProperties.generated.h>
#include <MaterialSchema/Nodes/Vectors/TransformVectorProperties.h>

#include "MaterialGraphFixture.h"

// MS2b2b 시험이다(Docs/Design/MaterialSystem 6.9절 공간 변환): UE TransformBase의 직접 쌍(문맥 함수
// 값·IR 연산)·경유 경로·입력 캐스트·결과 타입(World 위치만 Double3), 검증 오류, 이전 프레임 변형.
using namespace rocks;
using namespace rocks::material;
namespace material_transform_lowering_tests_file {

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

bool HasExtern(const MaterialTranslation& out, StringView name,
               IrExternVariant variant = IrExternVariant::Current) noexcept {
    for (const IrValue& value : out.module.Values()) {
        if (value.op == IrOp::Extern && value.immediate[0] == ContextIndex(name) &&
            value.immediate[1] == static_cast<uint64>(variant)) {
            return true;
        }
    }

    return false;
}

const IrValue* ResultValue(const MaterialTranslation& out, IrEntry entry,
                           StringView slot) noexcept {
    for (const IrResult& result : out.module.Results()) {
        if (result.entry == entry && result.slot == slot) {
            return &ValueOf(out, result.value);
        }
    }

    return nullptr;
}

Guid AddVectorTransform(GraphFixture& fixture, VectorTransformSource from,
                        VectorTransformDestination to) noexcept {
    TransformVectorProperties properties;
    properties.transformSourceType = from;
    properties.transformType = to;
    return fixture.Add("Material.TransformVector", fixture.env.Save(properties));
}

Guid AddPositionTransform(GraphFixture& fixture, PositionTransformSpace from,
                          PositionTransformSpace to, float tile = 32.0f) noexcept {
    TransformPositionProperties properties;
    properties.transformSourceType = from;
    properties.transformType = to;
    properties.constPeriodicWorldTileSize = tile;
    return fixture.Add("Material.TransformPosition", fixture.env.Save(properties));
}

Guid AddWorldPosition(GraphFixture& fixture, WorldPositionIncludedOffsets mode) noexcept {
    WorldPositionProperties properties;
    properties.worldPositionShaderOffset = mode;
    return fixture.Add("Material.WorldPosition", fixture.env.Save(properties));
}

// 접히지 않는 Float3 값이다.
Guid AddVarying3(GraphFixture& fixture) noexcept {
    const Guid normal = fixture.Add("Material.VertexNormal");
    return normal;
}

// transform 노드의 입력에 source를 잇고 결과를 WPO에 잇는다.
bool TranslateTransform(GraphFixture& fixture, Guid transform, Guid source, StringView pin,
                        MaterialTranslation& out,
                        const MaterialTraitsDesc& traits = Traits()) noexcept {
    fixture.Connect(source, pin, transform, "Input");
    fixture.Connect(transform, "Result", OutputNodeId, "WorldPositionOffset");
    return Translate(fixture, traits, out).IsOk();
}

} // namespace

// 벡터: Tangent → World는 직접 쌍, Local → View는 World·TranslatedWorld를 거친다(UE 경유).
TEST(MaterialTransform_VectorRoutes) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    {
        GraphFixture fixture(env);
        const Guid transform = AddVectorTransform(fixture, VectorTransformSource::Tangent,
                                                  VectorTransformDestination::World);
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateTransform(fixture, transform, AddVarying3(fixture), "Result", out));
        REQUIRE(out.succeeded);
        const IrValue* offset = ResultValue(out, IrEntry::Vertex, "WorldPositionOffset");
        REQUIRE(offset != nullptr);
        CHECK(offset->op == IrOp::Extern);
        CHECK(offset->immediate[0] == ContextIndex("TransformTangentVectorToWorld"));
        CHECK(offset->type == IrFloat(3));
    }

    {
        GraphFixture fixture(env);
        const Guid transform = AddVectorTransform(fixture, VectorTransformSource::Local,
                                                  VectorTransformDestination::View);
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateTransform(fixture, transform, AddVarying3(fixture), "Result", out));
        REQUIRE(out.succeeded);
        CHECK(HasExtern(out, "TransformLocalVectorToWorld"));
        CHECK(HasExtern(out, "TransformTranslatedWorldVectorToView"));
        // 이전 정점 문맥은 이전 변형이다.
        CHECK(HasExtern(out, "TransformLocalVectorToWorld", IrExternVariant::Previous));
    }

    {
        // 스칼라 Tangent 입력은 (0, 0, x)다.
        GraphFixture fixture(env);
        const Guid transform = AddVectorTransform(fixture, VectorTransformSource::Tangent,
                                                  VectorTransformDestination::World);
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateTransform(fixture, transform, AddVarying(fixture, 0.5f), "Result", out));
        REQUIRE(out.succeeded);
        const IrValue* offset = ResultValue(out, IrEntry::Vertex, "WorldPositionOffset");
        REQUIRE(offset != nullptr);
        CHECK(ValueOf(out, offset->operands[0]).op == IrOp::Append);
    }

    {
        // 같은 공간은 입력 그대로다.
        GraphFixture fixture(env);
        const Guid transform = AddVectorTransform(fixture, VectorTransformSource::World,
                                                  VectorTransformDestination::World);
        const Guid normal = AddVarying3(fixture);
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateTransform(fixture, transform, normal, "Result", out));
        REQUIRE(out.succeeded);
        const IrValue* offset = ResultValue(out, IrEntry::Vertex, "WorldPositionOffset");
        REQUIRE(offset != nullptr);
        CHECK(offset->immediate[0] == ContextIndex("VertexNormal"));
    }
}

// 위치: 대상 World만 Double3, TranslatedWorld ↔ World는 자르지 않는 IR Double 연산이다.
TEST(MaterialTransform_PositionTypes) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    {
        GraphFixture fixture(env);
        const Guid transform = AddPositionTransform(fixture, PositionTransformSpace::Local,
                                                    PositionTransformSpace::World);
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateTransform(fixture, transform, AddVarying3(fixture), "Result", out));
        REQUIRE(out.succeeded);
        CHECK(HasExtern(out, "TransformLocalPositionToWorld"));
        // 결과(Double3)를 WPO 핀(Float3)으로 낮춘다.
        const IrValue* offset = ResultValue(out, IrEntry::Vertex, "WorldPositionOffset");
        REQUIRE(offset != nullptr);
        CHECK(offset->op == IrOp::Convert);
        CHECK(ValueOf(out, offset->operands[0]).type == IrDouble(3));
    }

    {
        GraphFixture fixture(env);
        const Guid transform = AddPositionTransform(
            fixture, PositionTransformSpace::TranslatedWorld, PositionTransformSpace::World);
        const Guid world = AddWorldPosition(fixture, WorldPositionIncludedOffsets::CameraRelative);
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateTransform(fixture, transform, world, "XYZ", out));
        REQUIRE(out.succeeded);
        const IrValue* offset = ResultValue(out, IrEntry::Vertex, "WorldPositionOffset");
        REQUIRE(offset != nullptr);
        REQUIRE(offset->op == IrOp::Convert);
        const IrValue& difference = ValueOf(out, offset->operands[0]);
        CHECK(difference.op == IrOp::Subtract);
        CHECK(difference.type == IrDouble(3));
        CHECK(HasExtern(out, "PreViewTranslation"));
        CHECK(HasExtern(out, "PreViewTranslation", IrExternVariant::Previous));
    }

    {
        // World → View는 World → TranslatedWorld(더하기·낮춤) → View다.
        GraphFixture fixture(env);
        const Guid transform = AddPositionTransform(fixture, PositionTransformSpace::World,
                                                    PositionTransformSpace::View);
        const Guid world = AddWorldPosition(fixture, WorldPositionIncludedOffsets::Default);
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateTransform(fixture, transform, world, "XYZ", out));
        REQUIRE(out.succeeded);
        const IrValue* offset = ResultValue(out, IrEntry::Vertex, "WorldPositionOffset");
        REQUIRE(offset != nullptr);
        CHECK(offset->immediate[0] == ContextIndex("TransformTranslatedWorldPositionToView"));
        const IrValue& translated = ValueOf(out, offset->operands[0]);
        CHECK(translated.op == IrOp::Convert);
        CHECK(ValueOf(out, translated.operands[0]).op == IrOp::Add);
    }

    {
        // World → Local은 Surface·Volume만이다(V6).
        GraphFixture fixture(env);
        const Guid transform = AddPositionTransform(fixture, PositionTransformSpace::World,
                                                    PositionTransformSpace::Local);
        const Guid world = AddWorldPosition(fixture, WorldPositionIncludedOffsets::Default);
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateTransform(fixture, transform, world, "XYZ", out,
                                   Traits(BlendMode::Translucent, MaterialDomain::DeferredDecal)));
        CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::UnsupportedCapability, transform));
    }
}

// PeriodicWorld: 2의 거듭제곱 상수 tile은 PeriodicWorldOriginPow2, 아니면 PeriodicWorldOrigin.
// FirstPerson: TransformToFirstPerson(A, saturate(alpha)), 이전 정점 문맥은 이전 변형.
TEST(MaterialTransform_PeriodicAndFirstPerson) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    const float tiles[] = {32.0f, 30.0f};
    const StringView origins[] = {"PeriodicWorldOriginPow2", "PeriodicWorldOrigin"};
    for (usize index = 0; index < 2; ++index) {
        GraphFixture fixture(env);
        const Guid transform =
            AddPositionTransform(fixture, PositionTransformSpace::TranslatedWorld,
                                 PositionTransformSpace::PeriodicWorld, tiles[index]);
        const Guid world = AddWorldPosition(fixture, WorldPositionIncludedOffsets::CameraRelative);
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateTransform(fixture, transform, world, "XYZ", out));
        REQUIRE(out.succeeded);
        CHECK(HasExtern(out, origins[index]));
    }

    GraphFixture fixture(env);
    const Guid transform = AddPositionTransform(fixture, PositionTransformSpace::World,
                                                PositionTransformSpace::FirstPersonTranslatedWorld);
    const Guid world = AddWorldPosition(fixture, WorldPositionIncludedOffsets::Default);
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateTransform(fixture, transform, world, "XYZ", out));
    REQUIRE(out.succeeded);
    CHECK(HasExtern(out, "TransformToFirstPerson"));
    CHECK(HasExtern(out, "TransformToFirstPerson", IrExternVariant::Previous));
    bool saturated = false;
    for (const IrValue& value : out.module.Values()) {
        saturated = saturated || value.op == IrOp::Saturate;
    }

    // alpha 상수 1은 saturate가 접혀 상수 1이다.
    CHECK(!saturated);
}

// 검증: FirstPerson 원본, 3 성분이 아닌 입력(V1), 입력 없음(MissingInput).
TEST(MaterialTransform_Validation) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    {
        GraphFixture fixture(env);
        const Guid transform =
            AddPositionTransform(fixture, PositionTransformSpace::FirstPersonTranslatedWorld,
                                 PositionTransformSpace::World);
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateTransform(fixture, transform, AddVarying3(fixture), "Result", out));
        CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::InvalidProperty, transform));
    }

    {
        GraphFixture fixture(env);
        const Guid transform = AddVectorTransform(fixture, VectorTransformSource::World,
                                                  VectorTransformDestination::View);
        const Guid uv = fixture.Add("Material.ScreenPosition");
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateTransform(fixture, transform, uv, "ViewportUV", out));
        CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::TypeMismatch, transform));
    }

    {
        GraphFixture fixture(env);
        const Guid transform = AddPositionTransform(fixture, PositionTransformSpace::Local,
                                                    PositionTransformSpace::World);
        fixture.Connect(transform, "Result", OutputNodeId, "WorldPositionOffset");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::MissingInput, transform));
    }
}

} // namespace material_transform_lowering_tests_file
