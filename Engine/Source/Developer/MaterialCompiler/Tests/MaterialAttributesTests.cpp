#include <MaterialCompiler/MaterialSourceWriter.h>
#include <MaterialSchema/MaterialAttributeTable.h>
#include <MaterialSchema/MaterialContextValue.h>
#include <MaterialSchema/Nodes/MaterialAttributes/AggregateProperties.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/AggregateProperties.h>
#include <MaterialSchema/Nodes/MaterialAttributes/BlendAttributesProperties.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/BlendAttributesProperties.h>
#include <MaterialSchema/Nodes/MaterialAttributes/GetAttributesProperties.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/GetAttributesProperties.h>
#include <MaterialSchema/Nodes/MaterialAttributes/SetAttributesProperties.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/SetAttributesProperties.h>
#include <MaterialSchema/Nodes/MaterialAttributes/ShadingModelProperties.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/ShadingModelProperties.h>
#include <MaterialSchema/Nodes/Surface/SurfaceFromAttributesProperties.generated.h>
#include <MaterialSchema/Nodes/Surface/SurfaceFromAttributesProperties.h>

#include "MaterialGraphFixture.h"

// MS2e1 시험이다(Docs/Design/MaterialSystem 6.12절 MS2e1): Make·Break의 칸과 기본값, Set의 덮기,
// Get·Set 검사 문구, BlendAttributes(lerp·ShadingModel 문턱·Alpha 없음), Blend(수·속성·표면 거부·
// 입력 없음), Aggregate, 실행 중 선택의 칸별 선택, 출력 노드 useMaterialAttributes(진입점 칸·표면
// 변환· V5·CustomizedUV), SurfaceFromAttributes, custom 속성 → custom 출력(정점 포함)·MaterialCache
// 늦은 진단을 본다.
using namespace rocks;
using namespace rocks::material;
namespace material_attributes_tests_file {

using namespace material_graph_fixture;

namespace {

uint32 AttributesBit(rocks::ShadingModel model) noexcept {
    return 1u << static_cast<uint32>(model);
}

const IrResult* AttributesEntry(const MaterialTranslation& out, IrEntry entry,
                                StringView slot) noexcept {
    for (const IrResult& result : out.module.Results()) {
        if (result.entry == entry && result.slot == slot) {
            return &result;
        }
    }

    return nullptr;
}

float AttributesConstant(const MaterialTranslation& out, IrEntry entry, StringView slot) noexcept {
    const IrResult* result = AttributesEntry(out, entry, slot);
    if (result == nullptr || ValueOf(out, result->value).op != IrOp::Constant) {
        return -1.0f;
    }

    return FloatOf(ValueOf(out, result->value), 0);
}

bool AttributesNear(float value, float expected) noexcept {
    const float difference = value - expected;
    return difference < 1e-5f && difference > -1e-5f;
}

bool AttributesMessage(const MaterialTranslation& out, StringView message) noexcept {
    for (const nodegraph::GraphDiagnostic& diagnostic : out.diagnostics.items) {
        if (diagnostic.message.View() == message) {
            return true;
        }
    }

    return false;
}

const MaterialAttributeDefinition& AttributeNamed(StringView name) noexcept {
    for (const MaterialAttributeDefinition& definition : BuiltinMaterialAttributes()) {
        if (definition.name == name) {
            return definition;
        }
    }

    return BuiltinMaterialAttributes()[0];
}

// Get·Set 동적 핀 키(A_<id 32 hex>)다.
struct AttributeKey {
    char text[2 + 32] = {};

    StringView View() const noexcept {
        return StringView(text, sizeof(text));
    }
};

AttributeKey KeyOf(Guid id) noexcept {
    constexpr char hex[] = "0123456789abcdef";
    AttributeKey key;
    key.text[0] = 'A';
    key.text[1] = '_';
    usize at = 2;
    const uint64 halves[] = {id.high, id.low};
    for (uint64 half : halves) {
        for (int digit = 15; digit >= 0; --digit) {
            key.text[at++] = hex[(half >> (digit * 4)) & 0xF];
        }
    }

    return key;
}

MaterialTraitsDesc AttributesTraits() noexcept {
    MaterialTraitsDesc traits = Traits();
    traits.useMaterialAttributes = true;
    return traits;
}

// Unlit 표면의 emissive로 값 하나를 내보낸다(결과 Surface Emissive).
void EmitThroughUnlit(GraphFixture& fixture, Guid node, StringView pin) noexcept {
    const Guid unlit = fixture.Add("Material.SurfaceUnlit");
    fixture.Connect(node, pin, unlit, "EmissiveColor");
    fixture.Connect(unlit, "Result", OutputNodeId, "Surface");
}

} // namespace

// Make·Break: 연결된 핀만 칸이고, Break는 칸 값 또는 기본값이다.
TEST(MaterialAttributes_MakeBreak) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture set(env);
    const Guid make = set.Add("Material.MakeAttributes");
    set.Connect(AddConstant(set, 0.3f), "Result", make, "BaseColor");
    const Guid breaker = set.Add("Material.BreakAttributes");
    set.Connect(make, "Result", breaker, "MaterialAttributes");
    EmitThroughUnlit(set, breaker, "BaseColor");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(set, Traits(), out));
    REQUIRE(out.succeeded);
    CHECK(AttributesNear(AttributesConstant(out, IrEntry::Surface, "Emissive"), 0.3f));

    GraphFixture unset(env);
    const Guid empty = unset.Add("Material.MakeAttributes");
    const Guid broken = unset.Add("Material.BreakAttributes");
    unset.Connect(empty, "Result", broken, "MaterialAttributes");
    EmitThroughUnlit(unset, broken, "Specular");
    MaterialTranslation unsetOut(env.allocator);
    REQUIRE(Translate(unset, Traits(), unsetOut));
    REQUIRE(unsetOut.succeeded);
    // Specular 기본값 0.5다.
    CHECK(AttributesNear(AttributesConstant(unsetOut, IrEntry::Surface, "Emissive"), 0.5f));

    GraphFixture missing(env);
    const Guid alone = missing.Add("Material.BreakAttributes");
    EmitThroughUnlit(missing, alone, "BaseColor");
    MaterialTranslation missingOut(env.allocator);
    REQUIRE(Translate(missing, Traits(), missingOut));
    CHECK(HasErrorAt(missingOut, MaterialDiagnosticCode::MissingInput, alone));
}

// Set은 입력 기록을 연결된 속성 입력으로 덮고, Get은 그 칸을 읽는다.
TEST(MaterialAttributes_SetGet) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    const MaterialAttributeDefinition& baseColor = AttributeNamed("BaseColor");
    GraphFixture fixture(env);
    const Guid make = fixture.Add("Material.MakeAttributes");
    fixture.Connect(AddConstant(fixture, 0.2f), "Result", make, "BaseColor");
    SetAttributesProperties setProperties(env.allocator);
    REQUIRE(setProperties.attributeSetTypes.TryPushBack(baseColor.id));
    const Guid setter = fixture.Add("Material.SetAttributes", env.Save(setProperties));
    fixture.Connect(make, "Result", setter, "MaterialAttributes");
    const AttributeKey key = KeyOf(baseColor.id);
    fixture.Connect(AddConstant(fixture, 0.9f), "Result", setter, key.View());
    GetAttributesProperties getProperties(env.allocator);
    REQUIRE(getProperties.attributeGetTypes.TryPushBack(baseColor.id));
    const Guid getter = fixture.Add("Material.GetAttributes", env.Save(getProperties));
    fixture.Connect(setter, "Result", getter, "MaterialAttributes");
    EmitThroughUnlit(fixture, getter, key.View());
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    CHECK(AttributesNear(AttributesConstant(out, IrEntry::Surface, "Emissive"), 0.9f));

    GraphFixture duplicate(env);
    GetAttributesProperties twice(env.allocator);
    REQUIRE(twice.attributeGetTypes.TryPushBack(baseColor.id));
    REQUIRE(twice.attributeGetTypes.TryPushBack(baseColor.id));
    const Guid bad = duplicate.Add("Material.GetAttributes", env.Save(twice));
    duplicate.Connect(duplicate.Add("Material.MakeAttributes"), "Result", bad,
                      "MaterialAttributes");
    EmitThroughUnlit(duplicate, bad, key.View());
    MaterialTranslation duplicateOut(env.allocator);
    REQUIRE(Translate(duplicate, Traits(), duplicateOut));
    CHECK(HasErrorAt(duplicateOut, MaterialDiagnosticCode::InvalidProperty, bad));
    CHECK(AttributesMessage(duplicateOut, "Duplicate attribute types."));

    GraphFixture unknown(env);
    GetAttributesProperties stranger(env.allocator);
    REQUIRE(stranger.attributeGetTypes.TryPushBack(Guid{0x1234ull, 0x5678ull}));
    const Guid odd = unknown.Add("Material.GetAttributes", env.Save(stranger));
    unknown.Connect(unknown.Add("Material.MakeAttributes"), "Result", odd, "MaterialAttributes");
    EmitThroughUnlit(unknown, odd, "MaterialAttributesOut");
    MaterialTranslation unknownOut(env.allocator);
    REQUIRE(Translate(unknown, Traits(), unknownOut));
    CHECK(AttributesMessage(unknownOut, "Property type doesn't exist, needs re-mapping?"));
}

// BlendAttributes: 수 칸은 lerp, ShadingModel은 Alpha ≥ 0.5면 B, Alpha가 없으면 칸
// 없음(기본값)이다.
TEST(MaterialAttributes_BlendAttributes) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid a = fixture.Add("Material.MakeAttributes");
    const Guid b = fixture.Add("Material.MakeAttributes");
    fixture.Connect(AddConstant(fixture, 0.2f), "Result", a, "Metallic");
    fixture.Connect(AddConstant(fixture, 0.6f), "Result", b, "Metallic");
    ShadingModelProperties unlit;
    unlit.shadingModel = ShadingModelPreset::Unlit;
    ShadingModelProperties cloth;
    cloth.shadingModel = ShadingModelPreset::Cloth;
    fixture.Connect(fixture.Add("Material.ShadingModel", env.Save(unlit)), "Result", a,
                    "ShadingModel");
    fixture.Connect(fixture.Add("Material.ShadingModel", env.Save(cloth)), "Result", b,
                    "ShadingModel");
    BlendAttributesProperties properties;
    const Guid blend = fixture.Add("Material.BlendAttributes", env.Save(properties));
    fixture.Connect(a, "Result", blend, "A");
    fixture.Connect(b, "Result", blend, "B");
    fixture.Connect(AddConstant(fixture, 0.75f), "Result", blend, "Alpha");
    fixture.Connect(blend, "Result", OutputNodeId, "Attributes");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, AttributesTraits(), out));
    REQUIRE(out.succeeded);
    // ShadingModel 칸: 0.75 ≥ 0.5 → Cloth.
    CHECK(out.shadingModels == AttributesBit(rocks::ShadingModel::Cloth));
    const IrResult* metallic = AttributesEntry(out, IrEntry::Surface, "Metallic");
    REQUIRE(metallic != nullptr);
    CHECK(ValueOf(out, ValueOf(out, metallic->value).operands[0]).op == IrOp::Lerp);

    GraphFixture noAlpha(env);
    const Guid first = noAlpha.Add("Material.MakeAttributes");
    noAlpha.Connect(AddConstant(noAlpha, 0.2f), "Result", first, "Metallic");
    const Guid mixer = noAlpha.Add("Material.BlendAttributes", env.Save(properties));
    noAlpha.Connect(first, "Result", mixer, "A");
    noAlpha.Connect(noAlpha.Add("Material.MakeAttributes"), "Result", mixer, "B");
    noAlpha.Connect(mixer, "Result", OutputNodeId, "Attributes");
    MaterialTranslation noAlphaOut(env.allocator);
    REQUIRE(Translate(noAlpha, AttributesTraits(), noAlphaOut));
    REQUIRE(noAlphaOut.succeeded);
    // Alpha가 없으면 칸이 없어 고전 변환의 기본값(0)이다.
    CHECK(AttributesNear(AttributesConstant(noAlphaOut, IrEntry::Surface, "Metallic"), 0.0f));
}

// Blend: 수 값은 lerp, 둘 다 없으면 UE 문구, 표면은 막는다.
TEST(MaterialAttributes_Blend) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture numbers(env);
    const Guid blend = numbers.Add("Material.Blend");
    numbers.Connect(AddConstant(numbers, 0.2f), "Result", blend, "A");
    numbers.Connect(AddVarying(numbers, 0.6f), "Result", blend, "B");
    numbers.Connect(AddConstant(numbers, 0.5f), "Result", blend, "Alpha");
    EmitThroughUnlit(numbers, blend, "Result");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(numbers, Traits(), out));
    REQUIRE(out.succeeded);

    GraphFixture empty(env);
    const Guid nothing = empty.Add("Material.Blend");
    EmitThroughUnlit(empty, nothing, "Result");
    MaterialTranslation emptyOut(env.allocator);
    REQUIRE(Translate(empty, Traits(), emptyOut));
    CHECK(HasErrorAt(emptyOut, MaterialDiagnosticCode::MissingInput, nothing));
    CHECK(AttributesMessage(emptyOut, "No input value provided."));

    GraphFixture surfaces(env);
    const Guid mix = surfaces.Add("Material.Blend");
    surfaces.Connect(surfaces.Add("Material.SurfaceUnlit"), "Result", mix, "A");
    surfaces.Connect(surfaces.Add("Material.SurfaceUnlit"), "Result", mix, "B");
    surfaces.Connect(mix, "Result", OutputNodeId, "Surface");
    MaterialTranslation surfaceOut(env.allocator);
    REQUIRE(Translate(surfaces, Traits(), surfaceOut));
    CHECK(HasErrorAt(surfaceOut, MaterialDiagnosticCode::TypeMismatch, mix));
    CHECK(AttributesMessage(surfaceOut, "Cannot blend values of type 'Surface'."));
}

// Aggregate: 이름 입력이 칸이 되고 `<이름>Out`은 그 값이다.
TEST(MaterialAttributes_Aggregate) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    AggregateProperties properties(env.allocator);
    REQUIRE(properties.attributeNames.TryPushBack(env.Intern("Roughness")));
    const Guid aggregate = fixture.Add("Material.Aggregate", env.Save(properties));
    fixture.Connect(AddConstant(fixture, 0.35f), "Result", aggregate, "Roughness");
    EmitThroughUnlit(fixture, aggregate, "RoughnessOut");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    CHECK(AttributesNear(AttributesConstant(out, IrEntry::Surface, "Emissive"), 0.35f));
}

// 출력 노드 useMaterialAttributes: 표면은 칸의 고전 변환, 진입점 칸, 개별 핀은 V5다.
TEST(MaterialAttributes_OutputAttributes) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid make = fixture.Add("Material.MakeAttributes");
    fixture.Connect(AddConstant(fixture, 0.4f), "Result", make, "BaseColor");
    fixture.Connect(AddConstant(fixture, 0.7f), "Result", make, "Roughness");
    fixture.Connect(AddConstant(fixture, 0.25f), "Result", make, "AmbientOcclusion");
    fixture.Connect(make, "Result", OutputNodeId, "Attributes");
    fixture.Connect(AddConstant(fixture, 0.5f), "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, AttributesTraits(), out));
    REQUIRE(out.succeeded);
    CHECK(out.shadingModels == AttributesBit(rocks::ShadingModel::DefaultLit));
    CHECK(AttributesNear(AttributesConstant(out, IrEntry::Surface, "BaseColor"), 0.4f));
    CHECK(AttributesNear(AttributesConstant(out, IrEntry::Surface, "Roughness"), 0.7f));
    CHECK(AttributesNear(AttributesConstant(out, IrEntry::Surface, "AmbientOcclusion"), 0.25f));
    CHECK(CountDiagnostics(out, MaterialDiagnosticCode::InactiveOutput,
                           nodegraph::GraphDiagnosticSeverity::Warning) == 1);

    // useMaterialAttributes가 아니면 Attributes 핀이 V5다.
    MaterialTranslation plain(env.allocator);
    REQUIRE(Translate(fixture, Traits(), plain));
    CHECK(CountDiagnostics(plain, MaterialDiagnosticCode::InactiveOutput,
                           nodegraph::GraphDiagnosticSeverity::Warning) == 1);
}

// 실행 중 선택은 칸마다 고른다.
TEST(MaterialAttributes_RuntimeSelect) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid a = fixture.Add("Material.MakeAttributes");
    const Guid b = fixture.Add("Material.MakeAttributes");
    fixture.Connect(AddConstant(fixture, 0.2f), "Result", a, "BaseColor");
    fixture.Connect(AddConstant(fixture, 0.8f), "Result", b, "BaseColor");
    const Guid pass = fixture.Add("Material.ShadowPassSwitch");
    fixture.Connect(a, "Result", pass, "Default");
    fixture.Connect(b, "Result", pass, "Shadow");
    fixture.Connect(pass, "Result", OutputNodeId, "Attributes");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, AttributesTraits(), out));
    REQUIRE(out.succeeded);
    const IrResult* base = AttributesEntry(out, IrEntry::Surface, "BaseColor");
    REQUIRE(base != nullptr);
    CHECK(ValueOf(out, ValueOf(out, base->value).operands[0]).op == IrOp::Select);
}

// SurfaceFromAttributes: 칸이 고전 입력이고 ShadingModel 칸이 shading model이다.
TEST(MaterialAttributes_SurfaceFromAttributes) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid make = fixture.Add("Material.MakeAttributes");
    fixture.Connect(AddConstant(fixture, 0.45f), "Result", make, "BaseColor");
    ShadingModelProperties cloth;
    cloth.shadingModel = ShadingModelPreset::Cloth;
    fixture.Connect(fixture.Add("Material.ShadingModel", env.Save(cloth)), "Result", make,
                    "ShadingModel");
    SurfaceFromAttributesProperties properties;
    const Guid surface = fixture.Add("Material.SurfaceFromAttributes", env.Save(properties));
    fixture.Connect(make, "Result", surface, "MaterialAttributes");
    fixture.Connect(surface, "Result", OutputNodeId, "Surface");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    REQUIRE(out.succeeded);
    CHECK(out.shadingModels == AttributesBit(rocks::ShadingModel::Cloth));
    CHECK(AttributesNear(AttributesConstant(out, IrEntry::Surface, "BaseColor"), 0.45f));
}

// custom 속성 칸은 custom 출력이다(정점 빈도 FirstPersonInterpolationAlpha 포함). MaterialCache는
// 늦은 진단이다.
TEST(MaterialAttributes_CustomAttributes) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    const MaterialAttributeDefinition& bottom = AttributeNamed("ClearCoatBottomNormal");
    const MaterialAttributeDefinition& firstPerson =
        AttributeNamed("FirstPersonInterpolationAlpha");
    GraphFixture fixture(env);
    SetAttributesProperties properties(env.allocator);
    REQUIRE(properties.attributeSetTypes.TryPushBack(bottom.id));
    REQUIRE(properties.attributeSetTypes.TryPushBack(firstPerson.id));
    const Guid setter = fixture.Add("Material.SetAttributes", env.Save(properties));
    const AttributeKey bottomKey = KeyOf(bottom.id);
    const AttributeKey personKey = KeyOf(firstPerson.id);
    fixture.Connect(AddVarying(fixture, 0.5f), "Result", setter, bottomKey.View());
    fixture.Connect(AddConstant(fixture, 0.25f), "Result", setter, personKey.View());
    fixture.Connect(setter, "Result", OutputNodeId, "Attributes");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, AttributesTraits(), out));
    REQUIRE(out.succeeded);
    CHECK(AttributesEntry(out, IrEntry::CustomOutput, "ClearCoatBottomNormal0") != nullptr);
    CHECK(AttributesEntry(out, IrEntry::CustomOutputVertex, "FirstPersonOutput0") != nullptr);

    const MaterialAttributeDefinition& cache = AttributeNamed("MaterialCache");
    GraphFixture cached(env);
    SetAttributesProperties cacheProperties(env.allocator);
    REQUIRE(cacheProperties.attributeSetTypes.TryPushBack(cache.id));
    const Guid cacheSetter = cached.Add("Material.SetAttributes", env.Save(cacheProperties));
    const AttributeKey cacheKey = KeyOf(cache.id);
    cached.Connect(AddVarying(cached, 0.5f), "Result", cacheSetter, cacheKey.View());
    cached.Connect(cacheSetter, "Result", OutputNodeId, "Attributes");
    MaterialTranslation cachedOut(env.allocator);
    REQUIRE(Translate(cached, AttributesTraits(), cachedOut));
    CHECK(HasErrorAt(cachedOut, MaterialDiagnosticCode::PrerequisiteMissing, OutputNodeId));
}

} // namespace material_attributes_tests_file
