#pragma once

#include <Meta/Annotations.h>
#include <World/LightComponent.h>

namespace rocks {

// 방향광(태양)이다(UE UDirectionalLightComponent — Docs/Design/EngineRuntime 24.2.2절). 세기는
// lux다(UE GetLightUnits는 Unitless를 돌려주고 주석이 lux다). 위치는 쓰지 않고 Direction만 쓴다.
// 그림자 편향은 [0, MaxShadowBias]다.
// 데이터만 있다: 렌더 장면 등록(RS1·RS3) 전에는 아무것도 비추지 않는다(24.1절).
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_WORLD_API DirectionalLightComponent : public LightComponent {
    ROCKS_GENERATED_BODY()

public:
    // UE 생성자 기본값이다(DirectionalLightComponent.cpp:1014–1083).
    static constexpr float DefaultDirectionalIntensity = 10.0f;
    static constexpr float DefaultLightSourceAngle = 0.5357f;
    static constexpr float DefaultDynamicShadowDistanceMovableLight = 40000.0f;
    static constexpr int32 DefaultDynamicShadowCascades = 4;
    static constexpr float DefaultCascadeDistributionExponent = 3.0f;
    static constexpr float DefaultCascadeFraction = 0.1f;
    static constexpr float DefaultFarShadowDistance = 300000.0f;
    // UE PostEditChange clamp 범위다(:1101–1105). transition 하한은 KINDA_SMALL_NUMBER다.
    static constexpr int32 MaxShadowCascades = 10;
    static constexpr float MinCascadeDistributionExponent = 0.1f;
    static constexpr float MaxCascadeDistributionExponent = 10.0f;
    static constexpr float MinCascadeTransitionFraction = 1.0e-4f;
    static constexpr float MaxCascadeTransitionFraction = 0.3f;
    // 대기 태양 번호 상한(UE AtmosphereSunLightIndex ClampMax 1 — 태양 둘)이다.
    static constexpr int32 MaxAtmosphereSunLightIndex = 1;

    explicit DirectionalLightComponent(Allocator& allocator) noexcept : LightComponent(allocator) {
        intensity_ = DefaultDirectionalIntensity;
        castTranslucentShadows_ = true;
        castVolumetricShadow_ = true;
    }

    // 태양 각지름(도)이다. 오류: 음수·비유한, Mobility. soft 각도 같다.
    float LightSourceAngle() const noexcept {
        return lightSourceAngle_;
    }

    Result<void> SetLightSourceAngle(float degrees) noexcept;

    float LightSourceSoftAngle() const noexcept {
        return lightSourceSoftAngle_;
    }

    Result<void> SetLightSourceSoftAngle(float degrees) noexcept;

    float ShadowAmount() const noexcept {
        return shadowAmount_;
    }

    // 오류: [0, 1] 밖·비유한, Mobility.
    Result<void> SetShadowAmount(float amount) noexcept;

    // cascade 그림자 거리(cm)다. Movable·Stationary일 때 각각 쓴다. 오류: 음수·비유한, Mobility.
    float DynamicShadowDistanceMovableLight() const noexcept {
        return dynamicShadowDistanceMovableLight_;
    }

    Result<void> SetDynamicShadowDistanceMovableLight(float distance) noexcept;

    float DynamicShadowDistanceStationaryLight() const noexcept {
        return dynamicShadowDistanceStationaryLight_;
    }

    Result<void> SetDynamicShadowDistanceStationaryLight(float distance) noexcept;

    int32 DynamicShadowCascades() const noexcept {
        return dynamicShadowCascades_;
    }

    // 오류: [0, MaxShadowCascades] 밖, Mobility. far cascade 수도 같다.
    Result<void> SetDynamicShadowCascades(int32 cascades) noexcept;

    float CascadeDistributionExponent() const noexcept {
        return cascadeDistributionExponent_;
    }

    // 오류: [0.1, 10] 밖·비유한, Mobility.
    Result<void> SetCascadeDistributionExponent(float exponent) noexcept;

    float CascadeTransitionFraction() const noexcept {
        return cascadeTransitionFraction_;
    }

    // 오류: [MinCascadeTransitionFraction, 0.3] 밖·비유한, Mobility.
    Result<void> SetCascadeTransitionFraction(float fraction) noexcept;

    float ShadowDistanceFadeoutFraction() const noexcept {
        return shadowDistanceFadeoutFraction_;
    }

    // 오류: [0, 1] 밖·비유한, Mobility.
    Result<void> SetShadowDistanceFadeoutFraction(float fraction) noexcept;

    int32 FarShadowCascadeCount() const noexcept {
        return farShadowCascadeCount_;
    }

    Result<void> SetFarShadowCascadeCount(int32 count) noexcept;

    float FarShadowDistance() const noexcept {
        return farShadowDistance_;
    }

    // 오류: 음수·비유한, Mobility.
    Result<void> SetFarShadowDistance(float distance) noexcept;

    // Forward 경로에서 주 방향광을 고르는 우선순위다(큰 값 우선).
    int32 ForwardShadingPriority() const noexcept {
        return forwardShadingPriority_;
    }

    // 오류: 음수, Mobility.
    Result<void> SetForwardShadingPriority(int32 priority) noexcept;

    bool IsAtmosphereSunLight() const noexcept {
        return atmosphereSunLight_;
    }

    Result<void> SetAtmosphereSunLight(bool sun) noexcept;

    int32 AtmosphereSunLightIndex() const noexcept {
        return atmosphereSunLightIndex_;
    }

    // 오류: [0, MaxAtmosphereSunLightIndex] 밖, Mobility.
    Result<void> SetAtmosphereSunLightIndex(int32 index) noexcept;

    const LinearColor& AtmosphereSunDiskColorScale() const noexcept {
        return atmosphereSunDiskColorScale_;
    }

    // 오류: 비유한 성분, Mobility.
    Result<void> SetAtmosphereSunDiskColorScale(const LinearColor& scale) noexcept;

protected:
    bool LimitsShadowBias() const noexcept override {
        return true;
    }

private:
    ROCKS_PROPERTY(ReadOnly, Name = "lightSourceAngle")
    float lightSourceAngle_ = DefaultLightSourceAngle;
    ROCKS_PROPERTY(ReadOnly, Name = "lightSourceSoftAngle") float lightSourceSoftAngle_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "shadowAmount") float shadowAmount_ = 1.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "dynamicShadowDistanceMovableLight")
    float dynamicShadowDistanceMovableLight_ = DefaultDynamicShadowDistanceMovableLight;
    ROCKS_PROPERTY(ReadOnly, Name = "dynamicShadowDistanceStationaryLight")
    float dynamicShadowDistanceStationaryLight_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "dynamicShadowCascades")
    int32 dynamicShadowCascades_ = DefaultDynamicShadowCascades;
    ROCKS_PROPERTY(ReadOnly, Name = "cascadeDistributionExponent")
    float cascadeDistributionExponent_ = DefaultCascadeDistributionExponent;
    ROCKS_PROPERTY(ReadOnly, Name = "cascadeTransitionFraction")
    float cascadeTransitionFraction_ = DefaultCascadeFraction;
    ROCKS_PROPERTY(ReadOnly, Name = "shadowDistanceFadeoutFraction")
    float shadowDistanceFadeoutFraction_ = DefaultCascadeFraction;
    ROCKS_PROPERTY(ReadOnly, Name = "farShadowCascadeCount") int32 farShadowCascadeCount_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "farShadowDistance")
    float farShadowDistance_ = DefaultFarShadowDistance;
    ROCKS_PROPERTY(ReadOnly, Name = "forwardShadingPriority") int32 forwardShadingPriority_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "atmosphereSunLight") bool atmosphereSunLight_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "atmosphereSunLightIndex") int32 atmosphereSunLightIndex_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "atmosphereSunDiskColorScale")
    LinearColor atmosphereSunDiskColorScale_ = LinearColor::White();
};

} // namespace rocks
