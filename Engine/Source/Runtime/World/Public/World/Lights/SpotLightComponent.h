#pragma once

#include <Meta/Annotations.h>
#include <World/Lights/LightUnits.h>
#include <World/Lights/LightComponentBase.h>
#include <World/Lights/LightComponent.h>
#include <World/Lights/LocalLightComponent.h>
#include <World/Lights/PointLightComponent.h>

namespace rocks {

// 원뿔 광원이다(UE USpotLightComponent — 24.2.2절). 각은 반각(도)이고 setter는 값만 저장한다.
// 렌더에 쓰는 각은 ClampedConeAngles가 자른다(UE 실행 중 setter와 같다).
// 데이터만 있다: 렌더 장면 등록(RS1·RS3) 전에는 아무것도 비추지 않는다(24.1절).
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_WORLD_API SpotLightComponent : public PointLightComponent {
    ROCKS_GENERATED_BODY()

public:
    // 기본 바깥 반각(도, UE OuterConeAngle 44)이다.
    static constexpr float DefaultOuterConeAngle = 44.0f;
    // clamp 상한(도, UE GetClampedConeAngles 88.9)과 inner·outer 최소 간격(라디안 0.001)이다.
    static constexpr float MaxConeAngle = 88.9f;
    static constexpr float MinConeAngleGapRadians = 0.001f;

    // ClampedConeAngles 결과다(라디안).
    struct ConeAngles {
        float innerRadians = 0.0f;
        float outerRadians = 0.0f;
    };

    explicit SpotLightComponent(Allocator& allocator) noexcept : PointLightComponent(allocator) {
    }

    // Lumens는 원뿔 입체각으로 나눈다(UE SpotLightComponent.cpp:79).
    float ComputeLightBrightness() const noexcept override;

    float InnerConeAngle() const noexcept {
        return innerConeAngle_;
    }

    float OuterConeAngle() const noexcept {
        return outerConeAngle_;
    }

    // 오류: 비유한, Mobility(Stationary도 막는다 — UE SpotLightComponent.cpp:59). 아래도 같다.
    Result<void> SetInnerConeAngle(float degrees) noexcept;
    Result<void> SetOuterConeAngle(float degrees) noexcept;

    // inner = clamp(inner, 0, 88.9°), outer = clamp(outer, inner + 0.001, 88.9° + 0.001)(라디안 —
    // UE GetClampedConeAngles).
    ConeAngles ClampedConeAngles() const noexcept;

    // clamp한 바깥 반각(라디안)과 그 cos다(UE GetHalfConeAngle·GetCosHalfConeAngle).
    float HalfConeAngle() const noexcept {
        return ClampedConeAngles().outerRadians;
    }

    float CosHalfConeAngle() const noexcept;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "innerConeAngle") float innerConeAngle_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "outerConeAngle") float outerConeAngle_ = DefaultOuterConeAngle;
};

} // namespace rocks
