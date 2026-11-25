#pragma once

#include <Core/Math/Color.h>
#include <Core/Math/MathUtility.h>
#include <Core/Math/Shapes.h>
#include <Core/Math/Vector3.h>
#include <Meta/Annotations.h>
#include <World/Lights/LightingChannels.h>
#include <World/SceneComponent.h>
#include <World/Lights/LightComponent.h>

namespace rocks {

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
