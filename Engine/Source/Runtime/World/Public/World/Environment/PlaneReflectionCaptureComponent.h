#pragma once

#include <Meta/Annotations.h>
#include <World/Environment/ReflectionCaptureComponent.h>

namespace rocks {

// 평면 반사 캡처다(UE UPlaneReflectionCaptureComponent — Docs/Design/EngineRuntime 27.6절).
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_WORLD_API PlaneReflectionCaptureComponent : public ReflectionCaptureComponent {
    ROCKS_GENERATED_BODY()

public:
    // 기본 영향 반지름 배율(UE ReflectionCaptureComponent.cpp:1385)이다.
    static constexpr float DefaultInfluenceRadiusScale = 2.0f;

    explicit PlaneReflectionCaptureComponent(Allocator& allocator) noexcept
        : ReflectionCaptureComponent(allocator) {
    }

    float InfluenceRadiusScale() const noexcept {
        return influenceRadiusScale_;
    }

    // 오류: 비유한(UE에 UIMin뿐이다).
    Result<void> SetInfluenceRadiusScale(float scale) noexcept;

    // |(scale.y, scale.z)| × influenceRadiusScale이다(UE :1398).
    double InfluenceBoundingRadius() const noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "influenceRadiusScale")
    float influenceRadiusScale_ = DefaultInfluenceRadiusScale;
};

} // namespace rocks
