#include <MaterialCompiler/MaterialSourceWriter.h>
#include <MaterialSchema/MaterialContextValue.h>
#include <MaterialSchema/Nodes/Constants/ConstantDoubleProperties.generated.h>
#include <MaterialSchema/Nodes/Constants/ConstantDoubleProperties.h>
#include <MaterialSchema/Nodes/Constants/ViewPropertyProperties.generated.h>
#include <MaterialSchema/Nodes/Constants/ViewPropertyProperties.h>
#include <MaterialSchema/Nodes/Coordinates/RotatorProperties.generated.h>
#include <MaterialSchema/Nodes/Coordinates/RotatorProperties.h>
#include <MaterialSchema/Nodes/Coordinates/PannerProperties.generated.h>
#include <MaterialSchema/Nodes/Coordinates/PannerProperties.h>
#include <MaterialSchema/Nodes/Math/ComponentMaskProperties.generated.h>
#include <MaterialSchema/Nodes/Math/ComponentMaskProperties.h>
#include <MaterialSchema/Nodes/Coordinates/TextureCoordinateProperties.generated.h>
#include <MaterialSchema/Nodes/Coordinates/TextureCoordinateProperties.h>
#include <MaterialSchema/Nodes/Coordinates/WorldPositionProperties.generated.h>
#include <MaterialSchema/Nodes/Coordinates/WorldPositionProperties.h>
#include <MaterialSchema/Nodes/Vectors/BoundsProperties.generated.h>
#include <MaterialSchema/Nodes/Vectors/BoundsProperties.h>
#include <MaterialSchema/Nodes/Vectors/ReflectionVectorProperties.generated.h>
#include <MaterialSchema/Nodes/Vectors/ReflectionVectorProperties.h>

#include "MaterialGraphFixture.h"

// MS2b2a 시험이다(Docs/Design/MaterialSystem 6.9절): LWC(Double) 생성(뺄셈 Float 자름·상수 tile +
// offset 표기·Double 연산 철자), 위치 노드의 단계별 값(_NoShaderOffsets는 픽셀에서만), TexCoord
// 타일링·UnMirror·채널 수, Panner·Rotator 식, ViewProperty 속성별 값·역수, 경계, 사용자 노멀
// 반사, TruncateWorldPosition.
using namespace rocks;
using namespace rocks::material;
namespace material_coordinate_lowering_tests_file {

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

// 모듈에 name 문맥 값(variant 변형)이 있는가.
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

const IrResult* EntryResult(const MaterialTranslation& out, IrEntry entry,
                            StringView slot) noexcept {
    for (const IrResult& result : out.module.Results()) {
        if (result.entry == entry && result.slot == slot) {
            return &result;
        }
    }

    return nullptr;
}

bool HasText(StringView text, StringView part) noexcept {
    for (usize at = 0; at + part.ByteCount() <= text.ByteCount(); ++at) {
        if (StringView(text.Data() + at, part.ByteCount()) == part) {
            return true;
        }
    }

    return false;
}

Guid AddWorldPosition(GraphFixture& fixture, WorldPositionIncludedOffsets mode) noexcept {
    WorldPositionProperties properties;
    properties.worldPositionShaderOffset = mode;
    return fixture.Add("Material.WorldPosition", fixture.env.Save(properties));
}

Guid AddMask(GraphFixture& fixture, bool r, bool g, bool b) noexcept {
    ComponentMaskProperties properties;
    properties.r = r;
    properties.g = g;
    properties.b = b;
    return fixture.Add("Material.ComponentMask", fixture.env.Save(properties));
}

// 번역·생성이 성공하면 생성 원문을 text에 둔다.
bool Generate(GraphFixture& fixture, MaterialTranslation& out, MaterialGeneratedSource& source,
              const MaterialTraitsDesc& traits) noexcept {
    return Translate(fixture, traits, out).IsOk() && out.succeeded &&
           WriteMaterialSource(fixture.env.allocator, out.module, traits, TestAssetId, MainGraphId,
                               source)
               .IsOk() &&
           source.succeeded;
}

} // namespace

// 같은 Double 타입 둘의 뺄셈은 Float로 자른다(UE Sub의 LWCTruncateMode 2). Double − Float는 Double.
TEST(MaterialCoordinates_DoubleSubtractTruncates) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid world = AddWorldPosition(fixture, WorldPositionIncludedOffsets::Default);
    const Guid camera = fixture.Add("Material.CameraPosition");
    const Guid subtract = fixture.Add("Material.Subtract");
    fixture.Connect(world, "XYZ", subtract, "A");
    fixture.Connect(camera, "Result", subtract, "B");
    fixture.Connect(subtract, "Result", OutputNodeId, "WorldPositionOffset");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    const IrResult* offset = EntryResult(out, IrEntry::Vertex, "WorldPositionOffset");
    REQUIRE(offset != nullptr);
    const IrValue& value = ValueOf(out, offset->value);
    CHECK(value.type == IrFloat(3));
    REQUIRE(value.op == IrOp::Convert);
    const IrValue& difference = ValueOf(out, value.operands[0]);
    CHECK(difference.op == IrOp::Subtract);
    CHECK(difference.type == IrDouble(3));

    // 이전 정점 문맥은 두 값 모두 이전 프레임 변형이다.
    CHECK(HasExtern(out, "WorldPosition", IrExternVariant::Previous));
    CHECK(HasExtern(out, "CameraPosition", IrExternVariant::Previous));

    GraphFixture mixed(env);
    const Guid position = AddWorldPosition(mixed, WorldPositionIncludedOffsets::Default);
    const Guid minus = mixed.Add("Material.Subtract");
    mixed.Connect(position, "XYZ", minus, "A");
    mixed.Connect(AddVarying(mixed, 0.5f), "Result", minus, "B");
    const Guid floor = mixed.Add("Material.Floor");
    mixed.Connect(minus, "Result", floor, "Input");
    mixed.Connect(floor, "Result", OutputNodeId, "WorldPositionOffset");
    MaterialTranslation mixedOut(env.allocator);
    REQUIRE(Translate(mixed, Traits(), mixedOut));
    REQUIRE(mixedOut.succeeded);
    const IrResult* mixedOffset = EntryResult(mixedOut, IrEntry::Vertex, "WorldPositionOffset");
    REQUIRE(mixedOffset != nullptr);
    // 결과 핀(Float3)으로 낮추기 전까지 Double이다.
    const IrValue& demoted = ValueOf(mixedOut, mixedOffset->value);
    REQUIRE(demoted.op == IrOp::Convert);
    CHECK(ValueOf(mixedOut, demoted.operands[0]).op == IrOp::Floor);
    CHECK(ValueOf(mixedOut, demoted.operands[0]).type == IrDouble(3));
}

// 생성 원문: Double 연산은 MaterialDouble*, 상수는 tile + offset, 낮추기는 MaterialDoubleDemote다.
TEST(MaterialCoordinates_DoubleSourceSpelling) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid world = AddWorldPosition(fixture, WorldPositionIncludedOffsets::Default);
    ConstantDoubleProperties far;
    far.value = 5000000.0;
    const Guid constant = fixture.Add("Material.ConstantDouble", env.Save(far));
    const Guid add = fixture.Add("Material.Add");
    fixture.Connect(world, "XYZ", add, "A");
    fixture.Connect(constant, "Result", add, "B");
    const Guid floor = fixture.Add("Material.Floor");
    fixture.Connect(add, "Result", floor, "Input");
    const Guid max = fixture.Add("Material.Max");
    fixture.Connect(floor, "Result", max, "A");
    fixture.Connect(world, "XYZ", max, "B");
    fixture.Connect(max, "Result", OutputNodeId, "WorldPositionOffset");
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(fixture, out, source, Traits()));
    const StringView text = source.text.View();
    // 5000000 = 2 × 2097152 + 805696.
    const StringView expected[] = {"MakeMaterialDouble1(2.0, 805696.0)",
                                   "MakeMaterialDouble3(",
                                   "MaterialDoubleAdd(",
                                   "MaterialDoubleFloor(",
                                   "MaterialDoubleMax(",
                                   "MaterialDoubleDemote(",
                                   "const MaterialDouble3 m"};
    for (StringView part : expected) {
        CHECK(HasText(text, part));
    }
}

// _NoShaderOffsets 값은 픽셀 문맥에서만 쓰고 정점 문맥은 보통 위치다(UE _NoMaterialOffsets).
TEST(MaterialCoordinates_ShaderOffsetsByStage) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid world =
        AddWorldPosition(fixture, WorldPositionIncludedOffsets::CameraRelativeNoOffsets);
    fixture.Connect(world, "XYZ", OutputNodeId, "WorldPositionOffset");
    fixture.Connect(world, "Z", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    CHECK(HasExtern(out, "TranslatedWorldPosition"));
    CHECK(HasExtern(out, "TranslatedWorldPosition", IrExternVariant::Previous));
    CHECK(HasExtern(out, "TranslatedWorldPositionNoShaderOffsets"));
    CHECK(out.module.EntryFeatures(IrEntry::Surface) == ToMask(MaterialContextFeature::Vertex));
}

// TextureCoordinate: 타일링(u ≠ v면 Float2 곱), 채널 수 정의, UnMirror는 픽셀 전용, 색인 상한.
TEST(MaterialCoordinates_TextureCoordinate) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    {
        GraphFixture fixture(env);
        TextureCoordinateProperties properties;
        properties.coordinateIndex = 2;
        properties.uTiling = 2.0f;
        properties.vTiling = 3.0f;
        const Guid uv = fixture.Add("Material.TextureCoordinate", env.Save(properties));
        const Guid mask = AddMask(fixture, true, false, false);
        fixture.Connect(uv, "Result", mask, "Input");
        fixture.Connect(mask, "Result", OutputNodeId, "AmbientOcclusion");
        MaterialTranslation out(env.allocator);
        MaterialGeneratedSource source(env.allocator);
        REQUIRE(Generate(fixture, out, source, Traits()));
        CHECK(HasText(source.text.View(), "#define MATERIAL_NUM_TEXCOORDS 3\n"));
        CHECK(HasText(source.text.View(), "MaterialContextTexCoord(c, 2u)"));
        CHECK(HasText(source.text.View(), "float2(2.0, 3.0)"));
    }

    {
        // 같은 u·v 타일링 1은 곱하지 않는다.
        GraphFixture fixture(env);
        const Guid uv =
            fixture.Add("Material.TextureCoordinate", env.Save(TextureCoordinateProperties{}));
        const Guid mask = AddMask(fixture, false, true, false);
        fixture.Connect(uv, "Result", mask, "Input");
        fixture.Connect(mask, "Result", OutputNodeId, "AmbientOcclusion");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        REQUIRE(out.succeeded);
        const IrResult* ao = EntryResult(out, IrEntry::Surface, "AmbientOcclusion");
        REQUIRE(ao != nullptr);
        const IrValue& picked = ValueOf(out, ao->value);
        REQUIRE(picked.op == IrOp::Swizzle);
        CHECK(ValueOf(out, picked.operands[0]).op == IrOp::Extern);
    }

    {
        GraphFixture fixture(env);
        TextureCoordinateProperties properties;
        properties.unMirrorU = true;
        const Guid uv = fixture.Add("Material.TextureCoordinate", env.Save(properties));
        const Guid mask = AddMask(fixture, true, false, false);
        fixture.Connect(uv, "Result", mask, "Input");
        fixture.Connect(mask, "Result", OutputNodeId, "WorldPositionOffset");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(!out.succeeded);
        CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::StageViolation, uv));
    }

    {
        GraphFixture fixture(env);
        TextureCoordinateProperties properties;
        properties.coordinateIndex = 8;
        const Guid uv = fixture.Add("Material.TextureCoordinate", env.Save(properties));
        const Guid mask = AddMask(fixture, true, false, false);
        fixture.Connect(uv, "Result", mask, "Input");
        fixture.Connect(mask, "Result", OutputNodeId, "AmbientOcclusion");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::InvalidProperty, uv));
    }
}

// Panner: Append(Time × Speed.x, Time × Speed.y) + TexCoord(0), fractionalPart면 frac.
// Rotator: Float2 좌표면 Float2, Float3 좌표면 z를 붙인다.
TEST(MaterialCoordinates_PannerAndRotator) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    {
        GraphFixture fixture(env);
        PannerProperties properties;
        properties.fractionalPart = true;
        const Guid panner = fixture.Add("Material.Panner", env.Save(properties));
        fixture.SetValue(panner, "Speed", PinValue(0.5f, 2));
        const Guid mask = AddMask(fixture, true, false, false);
        fixture.Connect(panner, "Result", mask, "Input");
        fixture.Connect(mask, "Result", OutputNodeId, "AmbientOcclusion");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        REQUIRE(out.succeeded);
        const IrResult* ao = EntryResult(out, IrEntry::Surface, "AmbientOcclusion");
        REQUIRE(ao != nullptr);
        const IrValue& sum = ValueOf(out, ValueOf(out, ao->value).operands[0]);
        REQUIRE(sum.op == IrOp::Add);
        const IrValue& offset = ValueOf(out, sum.operands[0]);
        REQUIRE(offset.op == IrOp::Append);
        CHECK(ValueOf(out, offset.operands[0]).op == IrOp::Frac);
        CHECK(ValueOf(out, sum.operands[1]).op == IrOp::Extern);
        CHECK(HasExtern(out, "Time"));
        CHECK(HasExtern(out, "TexCoord"));
    }

    {
        GraphFixture fixture(env);
        const Guid rotator = fixture.Add("Material.Rotator", env.Save(RotatorProperties{}));
        const Guid mask = AddMask(fixture, false, true, false);
        fixture.Connect(rotator, "Result", mask, "Input");
        fixture.Connect(mask, "Result", OutputNodeId, "AmbientOcclusion");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        REQUIRE(out.succeeded);
        const IrResult* ao = EntryResult(out, IrEntry::Surface, "AmbientOcclusion");
        REQUIRE(ao != nullptr);
        const IrValue& rotated = ValueOf(out, ValueOf(out, ao->value).operands[0]);
        CHECK(rotated.type == IrFloat(2));
        CHECK(rotated.op == IrOp::Add);
    }

    {
        GraphFixture fixture(env);
        const Guid rotator = fixture.Add("Material.Rotator", env.Save(RotatorProperties{}));
        const Guid world = AddWorldPosition(fixture, WorldPositionIncludedOffsets::CameraRelative);
        fixture.Connect(world, "XYZ", rotator, "Coordinate");
        fixture.Connect(rotator, "Result", OutputNodeId, "WorldPositionOffset");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        REQUIRE(out.succeeded);
        const IrResult* offset = EntryResult(out, IrEntry::Vertex, "WorldPositionOffset");
        REQUIRE(offset != nullptr);
        const IrValue& whole = ValueOf(out, offset->value);
        CHECK(whole.type == IrFloat(3));
        CHECK(whole.op == IrOp::Append);
    }
}

// ViewProperty: 전용 역수 값(TanHalf → CotanHalf), 없으면 1 / 값, ViewportOffset은 상수 0.
TEST(MaterialCoordinates_ViewProperty) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    const auto translate = [&](ExposedViewProperty property, StringView pin,
                               MaterialTranslation& out) noexcept {
        GraphFixture fixture(env);
        ViewPropertyProperties properties;
        properties.property = property;
        const Guid view = fixture.Add("Material.ViewProperty", env.Save(properties));
        const Guid mask = AddMask(fixture, true, false, false);
        fixture.Connect(view, pin, mask, "Input");
        fixture.Connect(mask, "Result", OutputNodeId, "AmbientOcclusion");
        return Translate(fixture, Traits(), out).IsOk() && out.succeeded;
    };

    MaterialTranslation tangent(env.allocator);
    REQUIRE(translate(ExposedViewProperty::TanHalfFieldOfView, "InvProperty", tangent));
    CHECK(HasExtern(tangent, "ViewCotanHalfFieldOfView"));

    MaterialTranslation fov(env.allocator);
    REQUIRE(translate(ExposedViewProperty::FieldOfView, "InvProperty", fov));
    const IrResult* ao = EntryResult(fov, IrEntry::Surface, "AmbientOcclusion");
    REQUIRE(ao != nullptr);
    CHECK(ValueOf(fov, ValueOf(fov, ao->value).operands[0]).op == IrOp::Divide);

    MaterialTranslation offset(env.allocator);
    REQUIRE(translate(ExposedViewProperty::ViewportOffset, "Property", offset));
    const float zero[] = {0.0f};
    CHECK(IsFloatResult(offset, "AmbientOcclusion", zero));

    MaterialTranslation buffer(env.allocator);
    REQUIRE(translate(ExposedViewProperty::BufferSize, "InvProperty", buffer));
    CHECK(HasExtern(buffer, "SceneTexelSize"));
}

// 경계: Bounds type이 고르는 값, 영역 밖(DeferredDecal)은 V6.
TEST(MaterialCoordinates_Bounds) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    BoundsProperties properties;
    properties.type = BoundsType::PreSkinnedLocal;
    const Guid bounds = fixture.Add("Material.Bounds", env.Save(properties));
    fixture.Connect(bounds, "Extents", OutputNodeId, "WorldPositionOffset");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    CHECK(HasExtern(out, "PreSkinnedLocalBoundsFull"));

    MaterialTranslation decal(env.allocator);
    REQUIRE(
        Translate(fixture, Traits(BlendMode::Translucent, MaterialDomain::DeferredDecal), decal));
    CHECK(HasOnlyErrorAt(decal, MaterialDiagnosticCode::UnsupportedCapability, bounds));
}

// 사용자 노멀 반사는 픽셀 전용 CameraVector를 읽는다(WPO는 V4, 표면은 통과).
TEST(MaterialCoordinates_ReflectionAboutCustomNormal) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    ReflectionVectorProperties properties;
    properties.normalizeCustomWorldNormal = true;
    const Guid reflection = fixture.Add("Material.ReflectionVector", env.Save(properties));
    const Guid normal = fixture.Add("Material.VertexNormal");
    fixture.Connect(normal, "Result", reflection, "CustomWorldNormal");
    const Guid mask = AddMask(fixture, true, false, false);
    fixture.Connect(reflection, "Result", mask, "Input");
    fixture.Connect(mask, "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    CHECK(HasExtern(out, "CameraVector"));
    CHECK(!HasExtern(out, "ReflectionVector"));
    bool normalized = false;
    for (const IrValue& value : out.module.Values()) {
        normalized = normalized || value.op == IrOp::Normalize;
    }

    CHECK(normalized);

    fixture.Connect(reflection, "Result", OutputNodeId, "WorldPositionOffset");
    MaterialTranslation vertex(env.allocator);
    REQUIRE(Translate(fixture, Traits(), vertex));
    CHECK(HasOnlyErrorAt(vertex, MaterialDiagnosticCode::StageViolation, reflection));
}

// TruncateWorldPosition: Double은 Float로, Float는 그대로, 연결이 없으면 MissingInput.
TEST(MaterialCoordinates_TruncateWorldPosition) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid world = AddWorldPosition(fixture, WorldPositionIncludedOffsets::Default);
    const Guid truncate = fixture.Add("Material.TruncateWorldPosition");
    fixture.Connect(world, "XYZ", truncate, "Input");
    fixture.Connect(truncate, "Result", OutputNodeId, "WorldPositionOffset");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    const IrResult* offset = EntryResult(out, IrEntry::Vertex, "WorldPositionOffset");
    REQUIRE(offset != nullptr);
    CHECK(ValueOf(out, offset->value).type == IrFloat(3));

    GraphFixture empty(env);
    const Guid lonely = empty.Add("Material.TruncateWorldPosition");
    empty.Connect(lonely, "Result", OutputNodeId, "WorldPositionOffset");
    MaterialTranslation missing(env.allocator);
    REQUIRE(Translate(empty, Traits(), missing));
    CHECK(HasOnlyErrorAt(missing, MaterialDiagnosticCode::MissingInput, lonely));
}

} // namespace material_coordinate_lowering_tests_file
