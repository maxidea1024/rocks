#include <MaterialSchema/MaterialContextValue.h>
#include <TestSupport/Test.h>

// 문맥 값 표 시험이다(Docs/Design/MaterialSystem 19.3.2·19.13.5·19.13.6절, 단계 MS1b3): 이름이
// 유일하고, UE 외부 코드 선언 101개가 모두 대응되며, 칸 값이 정의된 범위이고, UE 기본값 규칙(0,
// 파티클 색만 1)과 허용 영역·단계·기능 등급의 대표 사례가 UE와 같다.
using namespace rocks;
using namespace rocks::material;
namespace material_context_value_tests_file {

namespace {

// UE BaseMaterialExpressions.ini의 외부 코드 선언 수다(CFG/BaseMaterialExpressions.ini:1038–).
constexpr usize UnrealExternalCodeDeclarations = 101;

bool SameIgnoringCase(StringView left, StringView right) noexcept {
    if (left.ByteCount() != right.ByteCount()) {
        return false;
    }

    for (usize index = 0; index < left.ByteCount(); ++index) {
        char a = left.Data()[index];
        char b = right.Data()[index];
        a = a >= 'A' && a <= 'Z' ? static_cast<char>(a - 'A' + 'a') : a;
        b = b >= 'A' && b <= 'Z' ? static_cast<char>(b - 'A' + 'a') : b;
        if (a != b) {
            return false;
        }
    }

    return true;
}

} // namespace

// 표 형식: 이름 유일(대소문자 무시), UE 이름 101개가 유일하고 빠짐없음, 칸이 정의된 범위다.
TEST(MaterialContextValue_TableIsWellFormed) {
    const Span<const MaterialContextValue> values = MaterialContextValues();
    REQUIRE(!values.IsEmpty());
    usize unreal = 0;
    for (usize index = 0; index < values.Size(); ++index) {
        const MaterialContextValue& value = values[index];
        CHECK(!value.name.IsEmpty());
        CHECK(static_cast<uint32>(value.type) < MaterialPinTypeCount);
        CHECK(static_cast<uint32>(value.feature) < MaterialContextFeatureCount);
        CHECK(value.domains != 0 && (value.domains & ~AllMaterialDomains) == 0);
        CHECK(value.stages != 0 && (value.stages & ~AllMaterialStages) == 0);
        CHECK(value.form == MaterialContextValueForm::Function || value.argumentCount == 0);
        CHECK(value.minimumTier <= MaterialCapabilityTier::Sm6);
        CHECK(value.derivative <= MaterialDerivative::Valid);
        if (!value.unrealName.IsEmpty()) {
            ++unreal;
        }

        for (usize other = 0; other < index; ++other) {
            CHECK(!SameIgnoringCase(values[other].name, value.name));
            CHECK(value.unrealName.IsEmpty() || values[other].unrealName != value.unrealName);
        }

        CHECK(FindMaterialContextValue(value.name) == &value);
    }

    CHECK(unreal == UnrealExternalCodeDeclarations);
    CHECK(FindMaterialContextValue("NoSuchValue") == nullptr);
}

// UE와 같은 대표 칸: 공급하지 않았을 때 0이고 파티클 색만 1, 허용 영역·단계·기능 등급·미분·이전
// 프레임 변형(19.3.2·19.3.5절 예시).
TEST(MaterialContextValue_MatchesUnrealDeclarations) {
    const MaterialContextValue* particleColor = FindMaterialContextValue("ParticleColor");
    REQUIRE(particleColor != nullptr);
    CHECK(particleColor->feature == MaterialContextFeature::Particle);
    for (float component : particleColor->defaultValue.values) {
        CHECK(component == 1.0f);
    }

    const MaterialContextValue* vertexColor = FindMaterialContextValue("VertexColor");
    REQUIRE(vertexColor != nullptr);
    CHECK(vertexColor->type == MaterialPinType::Float4);
    CHECK(vertexColor->derivative == MaterialDerivative::Valid);
    for (float component : vertexColor->defaultValue.values) {
        CHECK(component == 0.0f);
    }

    // LightVector: LightFunction·DeferredDecal, 픽셀 단계(UE :1049).
    const MaterialContextValue* light = FindMaterialContextValue("LightVector");
    REQUIRE(light != nullptr);
    CHECK(light->domains ==
          (ToMask(MaterialDomain::LightFunction) | ToMask(MaterialDomain::DeferredDecal)));
    CHECK(light->stages == MaterialPixelStage);

    // IsFirstPerson: Surface·Volume(UE :1047). SkyLightEnvironmentSample: Surface만(UE :1107).
    const MaterialContextValue* firstPerson = FindMaterialContextValue("IsFirstPerson");
    REQUIRE(firstPerson != nullptr);
    CHECK(firstPerson->domains ==
          (ToMask(MaterialDomain::Surface) | ToMask(MaterialDomain::Volume)));
    CHECK(firstPerson->feature == MaterialContextFeature::FirstPerson);
    const MaterialContextValue* sky = FindMaterialContextValue("SkyLightEnvironmentSample");
    REQUIRE(sky != nullptr);
    CHECK(sky->unrealName == StringView("SkyLightEnvMapSample"));
    CHECK(sky->domains == ToMask(MaterialDomain::Surface));
    CHECK(sky->argumentCount == 2);

    // PreSkinned*: 정점 단계 전용. LightmapUvs·PrecomputedAoMask: 최소 SM5(UE :1050, 1073).
    const MaterialContextValue* preSkinned = FindMaterialContextValue("PreSkinnedPosition");
    REQUIRE(preSkinned != nullptr);
    CHECK(preSkinned->stages == MaterialVertexStage);
    CHECK(preSkinned->feature == MaterialContextFeature::Skin);
    const MaterialContextValue* lightmap = FindMaterialContextValue("LightmapUvs");
    REQUIRE(lightmap != nullptr);
    CHECK(lightmap->minimumTier == MaterialCapabilityTier::Sm5);
    const MaterialContextValue* aoMask = FindMaterialContextValue("PrecomputedAoMask");
    REQUIRE(aoMask != nullptr);
    CHECK(aoMask->minimumTier == MaterialCapabilityTier::Sm5);

    // 이전 프레임 변형(<PREV>)과 함수형 인자 수, 이름 규칙(Get·WS·DF·상표).
    const MaterialContextValue* particlePosition =
        FindMaterialContextValue("ParticlePositionAbsolute");
    REQUIRE(particlePosition != nullptr);
    CHECK(particlePosition->hasPrevious);
    CHECK(particlePosition->type == MaterialPinType::Double3);
    const MaterialContextValue* customData = FindMaterialContextValue("PerInstanceCustomData");
    REQUIRE(customData != nullptr);
    CHECK(customData->form == MaterialContextValueForm::Function);
    CHECK(customData->argumentCount == 2);
    CHECK(FindMaterialContextValue("InstanceToWorldDouble") != nullptr);
    CHECK(FindMaterialContextValue("VirtualizedGeometryPass") != nullptr);
    CHECK(FindMaterialContextValue("GetParticleToWorld") != nullptr);
    CHECK(FindMaterialContextValue("ParticleToWorld") != nullptr);
}

} // namespace material_context_value_tests_file
