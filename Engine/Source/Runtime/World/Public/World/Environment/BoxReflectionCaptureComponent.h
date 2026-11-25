#pragma once

#include <Meta/Annotations.h>
#include <World/Environment/ReflectionCaptureComponent.h>

namespace rocks {

// 상자 영향 반사 캡처다(UE UBoxReflectionCaptureComponent — Docs/Design/EngineRuntime 27.6절). 상자
// 크기는 컴포넌트 월드 scale이다.
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_WORLD_API BoxReflectionCaptureComponent : public ReflectionCaptureComponent {
    ROCKS_GENERATED_BODY()

public:
    // 기본 경계 전이 거리(cm, UE ReflectionCaptureComponent.cpp:1351)다.
    static constexpr float DefaultBoxTransitionDistance = 100.0f;

    explicit BoxReflectionCaptureComponent(Allocator& allocator) noexcept
        : ReflectionCaptureComponent(allocator) {
    }

    float BoxTransitionDistance() const noexcept {
        return boxTransitionDistance_;
    }

    // 오류: 비유한(UE에 UIMin뿐이다).
    Result<void> SetBoxTransitionDistance(float distance) noexcept;

    // |월드 scale|이다(UE :1364).
    double InfluenceBoundingRadius() const noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "boxTransitionDistance")
    float boxTransitionDistance_ = DefaultBoxTransitionDistance;
};

} // namespace rocks
