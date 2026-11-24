#pragma once

#include <Core/Math/Color.h>
#include <Core/Math/MathUtility.h>
#include <Core/Math/Shapes.h>
#include <Core/Math/Vector3.h>
#include <Meta/Annotations.h>
#include <World/LightingChannels.h>
#include <World/SceneComponent.h>

namespace rocks {

// 광원 세기의 단위다(UE ELightUnits — E/Classes/Engine/Scene.h:83). EV는 EV100이다.
ROCKS_ENUM()
enum class LightUnits : uint8 {
    Unitless,
    Candelas,
    Lumens,
    EV,
    Nits
};

// 모든 광원 컴포넌트의 기반이다(UE ULightComponentBase — Docs/Design/EngineRuntime 24.2절).
// 데이터만 가진다: 렌더 장면 등록(CR12·RS1·RS3)이 생기기 전에는 아무것도 비추지 않는다. 값이 바뀌면
// Render 시스템 상태가 있을 때 다시 보내기를 표시한다(지금은 상태가 없어 아무 일도 없다).
//
// 규약(24.2.3절):
// - setter는 비유한·범위 밖 값을 InvalidArgument로 거부하고 값을 바꾸지 않는다.
// - 게임 World에서 초기화 뒤에는 Static 광원의 데이터를 바꿀 수 없다(InvalidArgument + Warning
//   StaticComponentDataChanged — UE AreDynamicDataChangesAllowed). Stationary는 세기·색 등을 바꿀
//   수 있다. Spawn·OnAwake 안과 편집 World에서는 막지 않는다.
// - 세기 단위는 파생이 정한다(IntensityUnits). 세기는 단위가 EV일 때만 음수를 받는다.
//
// 사용 예(Actor OnAwake):
//   auto light = GetWorld().AddComponent<PointLightComponent>(Handle());
//   GetWorld().With<PointLightComponent>(light.Value(), [](PointLightComponent& value) noexcept {
//       (void)value.SetIntensityUnits(LightUnits::Candelas);
//       return value.SetIntensity(8.0f);    // 8 cd
//   });
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS(Abstract)
class ROCKS_WORLD_API LightComponentBase : public SceneComponent {
    ROCKS_GENERATED_BODY()

public:
    // 기본 세기다(UE ULightComponentBase 생성자 π — LightComponent.cpp:436).
    static constexpr float DefaultIntensity = math::Pi<float>;

    explicit LightComponentBase(Allocator& allocator) noexcept : SceneComponent(allocator) {
    }

    float Intensity() const noexcept {
        return intensity_;
    }

    // 오류: 비유한, 단위가 EV가 아닌데 음수(InvalidArgument), Mobility(24.2.3절).
    Result<void> SetIntensity(float intensity) noexcept;

    // 세기의 단위다(기본 Unitless — 국소 광원이 재정의한다).
    virtual LightUnits IntensityUnits() const noexcept {
        return LightUnits::Unitless;
    }

    // 필터 색(sRGB 인코딩 byte, UE FColor LightColor)이다.
    Color LightColor() const noexcept {
        return lightColor_;
    }

    // sRGB 해독한 선형 색이다(UE GetLightColor — alpha는 byte/255).
    LinearColor LinearLightColor() const noexcept;

    // sRGB byte를 그대로 둔다. 오류: Mobility.
    Result<void> SetLightColor(Color color) noexcept;

    // srgb면 선형 색을 sRGB로 인코딩해 둔다(ToSRGB). 거짓이면 인코딩 없이 byte로 양자화한다
    // (QuantizeRound — UE SetLightColor(FLinearColor, bSRGB), LightComponent.cpp:1130). 오류:
    // 비유한 성분(InvalidArgument), Mobility.
    Result<void> SetLightColor(const LinearColor& color, bool srgb = true) noexcept;

    bool AffectsWorld() const noexcept {
        return affectsWorld_;
    }

    // 거짓이면 광원 전체가 꺼진다. 오류: 게임 World 초기화 뒤(ComponentDataLockedInGame — UE도 실행
    // 중 바꿀 수 없다).
    Result<void> SetAffectsWorld(bool affects) noexcept;

    bool CastsShadows() const noexcept {
        return castShadows_;
    }

    Result<void> SetCastShadows(bool cast) noexcept;

    bool CastsStaticShadows() const noexcept {
        return castStaticShadows_;
    }

    Result<void> SetCastStaticShadows(bool cast) noexcept;

    bool CastsDynamicShadows() const noexcept {
        return castDynamicShadows_;
    }

    Result<void> SetCastDynamicShadows(bool cast) noexcept;

    bool AffectsTranslucentLighting() const noexcept {
        return affectTranslucentLighting_;
    }

    Result<void> SetAffectTranslucentLighting(bool affects) noexcept;

    bool HasTransmission() const noexcept {
        return transmission_;
    }

    Result<void> SetTransmission(bool transmission) noexcept;

    bool CastsVolumetricShadow() const noexcept {
        return castVolumetricShadow_;
    }

    Result<void> SetCastVolumetricShadow(bool cast) noexcept;

    float IndirectLightingIntensity() const noexcept {
        return indirectLightingIntensity_;
    }

    // 오류: 비유한, Mobility.
    Result<void> SetIndirectLightingIntensity(float intensity) noexcept;

    float VolumetricScatteringIntensity() const noexcept {
        return volumetricScatteringIntensity_;
    }

    // 오류: 비유한, Mobility.
    Result<void> SetVolumetricScatteringIntensity(float intensity) noexcept;

    // Static이고 소유 Actor가 있다(UE HasStaticLighting — 굽는 조명 대상).
    bool HasStaticLighting() const noexcept;

    // Movable이 아니고 소유 Actor가 있다(UE HasStaticShadowing).
    bool HasStaticShadowing() const noexcept;

protected:
    // Mobility 검사 → 대입 → 바뀌었으면 Render 다시 보내기 표시다. 값 검증은 호출 전에 한다.
    template <typename T>
    Result<void> ChangeLightData(T& field, const T& value, const char* property,
                                 bool blockStationary = false) noexcept {
        if (auto allowed = CheckDynamicDataChange(property, blockStationary); !allowed) {
            return allowed;
        }

        if (!(field == value)) {
            field = value;
            MarkRenderStateDirtyIfPresent();
        }

        return Result<void>::Success();
    }

    // 파생 생성자가 기본값을 바꾼다(UE 파생 생성자처럼).
    ROCKS_PROPERTY(ReadOnly, Name = "intensity") float intensity_ = DefaultIntensity;
    ROCKS_PROPERTY(ReadOnly, Name = "lightColor") Color lightColor_ = Color::White();
    ROCKS_PROPERTY(ReadOnly, Name = "affectsWorld") bool affectsWorld_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "castShadows") bool castShadows_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "castStaticShadows") bool castStaticShadows_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "castDynamicShadows") bool castDynamicShadows_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "affectTranslucentLighting")
    bool affectTranslucentLighting_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "transmission") bool transmission_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "castVolumetricShadow") bool castVolumetricShadow_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "indirectLightingIntensity")
    float indirectLightingIntensity_ = 1.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "volumetricScatteringIntensity")
    float volumetricScatteringIntensity_ = 1.0f;
};

// 방향을 가진 광원의 기반이다(UE ULightComponent — 24.2.2절). 방향은 월드 +X 축이다.
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS(Abstract)
class ROCKS_WORLD_API LightComponent : public LightComponentBase {
    ROCKS_GENERATED_BODY()

public:
    // 기본 색온도(K, D65 — UE Temperature 6500)다.
    static constexpr float DefaultTemperature = 6500.0f;
    // 기본 그림자 편향(UE ShadowBias·ShadowSlopeBias 0.5)과 방향광의 상한(UE
    // DirectionalLightComponent PostEditChange clamp 10)이다.
    static constexpr float DefaultShadowBias = 0.5f;
    static constexpr float MaxShadowBias = 10.0f;

    explicit LightComponent(Allocator& allocator) noexcept : LightComponentBase(allocator) {
    }

    // 렌더러에 줄 밝기다(UE ComputeLightBrightness). 기본은 intensity 그대로이고 국소 광원이 단위
    // 변환을 더한다(24.2.4절). IES 프로파일은 아직 없다.
    virtual float ComputeLightBrightness() const noexcept {
        return intensity_;
    }

    // 빛이 나가는 방향(월드 +X 축 단위 벡터 — UE GetDirection)이다. 월드 행렬 축이 0이면 +X다.
    Vec3d Direction() const noexcept;

    float Temperature() const noexcept {
        return temperature_;
    }

    // 오류: 비유한, Mobility.
    Result<void> SetTemperature(float kelvin) noexcept;

    bool UsesTemperature() const noexcept {
        return useTemperature_;
    }

    Result<void> SetUseTemperature(bool use) noexcept;

    // 그리기를 멈추는 거리(cm, 0 이하 = 제한 없음)다.
    float MaxDrawDistance() const noexcept {
        return maxDrawDistance_;
    }

    // 오류: 비유한, Mobility.
    Result<void> SetMaxDrawDistance(float distance) noexcept;

    float MaxDistanceFadeRange() const noexcept {
        return maxDistanceFadeRange_;
    }

    // 오류: 비유한, Mobility.
    Result<void> SetMaxDistanceFadeRange(float range) noexcept;

    float SpecularScale() const noexcept {
        return specularScale_;
    }

    // 오류: [0, 1] 밖·비유한, Mobility.
    Result<void> SetSpecularScale(float scale) noexcept;

    float DiffuseScale() const noexcept {
        return diffuseScale_;
    }

    // 오류: [0, 1] 밖·비유한, Mobility.
    Result<void> SetDiffuseScale(float scale) noexcept;

    float ShadowResolutionScale() const noexcept {
        return shadowResolutionScale_;
    }

    // 0이면 그림자 맵을 끈다. 오류: 음수·비유한, Mobility.
    Result<void> SetShadowResolutionScale(float scale) noexcept;

    float ShadowBias() const noexcept {
        return shadowBias_;
    }

    // 오류: 비유한, Mobility.
    Result<void> SetShadowBias(float bias) noexcept;

    float ShadowSlopeBias() const noexcept {
        return shadowSlopeBias_;
    }

    // 오류: 비유한, Mobility.
    Result<void> SetShadowSlopeBias(float bias) noexcept;

    float ShadowSharpen() const noexcept {
        return shadowSharpen_;
    }

    // 오류: 비유한, Mobility.
    Result<void> SetShadowSharpen(float sharpen) noexcept;

    // 접촉 그림자 길이다(0 = 끔). 기본은 화면 공간 비율이고 ContactShadowLengthInWorldSpace면 cm다.
    float ContactShadowLength() const noexcept {
        return contactShadowLength_;
    }

    // 오류: 음수·비유한, Mobility.
    Result<void> SetContactShadowLength(float length) noexcept;

    bool ContactShadowLengthInWorldSpace() const noexcept {
        return contactShadowLengthInWorldSpace_;
    }

    Result<void> SetContactShadowLengthInWorldSpace(bool worldSpace) noexcept;

    bool CastsTranslucentShadows() const noexcept {
        return castTranslucentShadows_;
    }

    Result<void> SetCastTranslucentShadows(bool cast) noexcept;

    const rocks::LightingChannels& LightingChannels() const noexcept {
        return lightingChannels_;
    }

    // UE처럼 Mobility를 보지 않는다(LightComponent.cpp:1366).
    Result<void> SetLightingChannels(const rocks::LightingChannels& channels) noexcept;

protected:
    // 그림자 편향을 [0, MaxShadowBias]로 제한하는지다(방향광만 — UE
    // DirectionalLightComponent.cpp:1107).
    virtual bool LimitsShadowBias() const noexcept {
        return false;
    }

    ROCKS_PROPERTY(ReadOnly, Name = "temperature") float temperature_ = DefaultTemperature;
    ROCKS_PROPERTY(ReadOnly, Name = "useTemperature") bool useTemperature_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "maxDrawDistance") float maxDrawDistance_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "maxDistanceFadeRange") float maxDistanceFadeRange_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "specularScale") float specularScale_ = 1.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "diffuseScale") float diffuseScale_ = 1.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "shadowResolutionScale") float shadowResolutionScale_ = 1.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "shadowBias") float shadowBias_ = DefaultShadowBias;
    ROCKS_PROPERTY(ReadOnly, Name = "shadowSlopeBias") float shadowSlopeBias_ = DefaultShadowBias;
    ROCKS_PROPERTY(ReadOnly, Name = "shadowSharpen") float shadowSharpen_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "contactShadowLength") float contactShadowLength_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "contactShadowLengthInWorldSpace")
    bool contactShadowLengthInWorldSpace_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "castTranslucentShadows") bool castTranslucentShadows_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "lightingChannels") rocks::LightingChannels lightingChannels_;
};

// 위치와 영향 반경을 가진 광원의 기반이다(UE ULocalLightComponent — 24.2.2절). 세기 기본은 5000
// Unitless다(UE 생성자 — 편집기 배치의 Candelas 변환은 컴포넌트 기본이 아니다).
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS(Abstract)
class ROCKS_WORLD_API LocalLightComponent : public LightComponent {
    ROCKS_GENERATED_BODY()

public:
    // 국소 광원 기본 세기(UE LocalLightComponent.cpp:13)와 영향 반경(cm, :16)이다.
    static constexpr float DefaultLocalIntensity = 5000.0f;
    static constexpr float DefaultAttenuationRadius = 1000.0f;

    explicit LocalLightComponent(Allocator& allocator) noexcept : LightComponent(allocator) {
        intensity_ = DefaultLocalIntensity;
    }

    LightUnits IntensityUnits() const noexcept override {
        return intensityUnits_;
    }

    // 단위만 바꾼다(세기 값은 그대로 — UE SetIntensityUnits). EV에서 다른 단위로 가면 음수 세기는
    // 0이 된다(UE PostEditChangeProperty). 오류: 범위 밖 enum, 이 광원이 받지
    // 않는 단위(1/r²가 꺼진 Point는 Unitless만 — InvalidArgument), Mobility.
    Result<void> SetIntensityUnits(LightUnits units) noexcept;

    float AttenuationRadius() const noexcept {
        return attenuationRadius_;
    }

    // 오류: 음수·비유한, Mobility(Stationary도 막는다 — UE AreDynamicDataChangesAllowed(false)).
    Result<void> SetAttenuationRadius(float radius) noexcept;

    float InverseExposureBlend() const noexcept {
        return inverseExposureBlend_;
    }

    // 오류: [0, 1] 밖·비유한, Mobility.
    Result<void> SetInverseExposureBlend(float blend) noexcept;

    // from 단위의 세기를 to 단위로 바꾸는 배율이다(UE GetUnitsConversionFactor —
    // LocalLightComponent.cpp:146). cosHalfConeAngle은 Lumens 계산의 원뿔 반각 cos이다(-1 = 구).
    // [-1, 1 - 1e-4]로 자른다. 같은 단위면 1이다.
    static float UnitsConversionFactor(LightUnits from, LightUnits to,
                                       float cosHalfConeAngle = -1.0f) noexcept;

    // 영향 경계 구다(월드 위치 중심, 반지름 attenuationRadius — UE GetBoundingSphere).
    virtual Sphere<double> BoundingSphere() const noexcept;

    // 경계가 영향 구와 닿는지다(중심 거리 <= 반지름 + bounds 구 반지름 — UE AffectsBounds).
    virtual bool AffectsBounds(const ComponentBounds& bounds) const noexcept;

protected:
    // 이 광원이 단위를 받는지다(기본 참 — Point는 1/r²가 꺼지면 Unitless만).
    virtual bool AcceptsIntensityUnits(LightUnits) const noexcept {
        return true;
    }

    // 단위가 EV가 아니면 음수 세기를 0으로 둔다(단위가 바뀐 뒤 — UE PostEditChangeProperty).
    void ClampIntensityForUnits() noexcept;

    ROCKS_PROPERTY(ReadOnly, Name = "intensityUnits")
    LightUnits intensityUnits_ = LightUnits::Unitless;
    ROCKS_PROPERTY(ReadOnly, Name = "attenuationRadius")
    float attenuationRadius_ = DefaultAttenuationRadius;
    ROCKS_PROPERTY(ReadOnly, Name = "inverseExposureBlend") float inverseExposureBlend_ = 0.0f;
};

} // namespace rocks
