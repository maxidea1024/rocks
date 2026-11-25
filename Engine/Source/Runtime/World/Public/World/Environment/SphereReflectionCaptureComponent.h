#pragma once

#include <Meta/Annotations.h>
#include <World/Environment/ReflectionCaptureComponent.h>

namespace rocks {

// 구 영향 반사 캡처다(UE USphereReflectionCaptureComponent — Docs/Design/EngineRuntime 27.6절).
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_WORLD_API SphereReflectionCaptureComponent : public ReflectionCaptureComponent {
    ROCKS_GENERATED_BODY()

public:
    // 기본 영향 반지름(cm, UE ReflectionCaptureComponent.cpp:1321)이다.
    static constexpr float DefaultInfluenceRadius = 3000.0f;

    explicit SphereReflectionCaptureComponent(Allocator& allocator) noexcept
        : ReflectionCaptureComponent(allocator) {
    }

    float InfluenceRadius() const noexcept {
        return influenceRadius_;
    }

    // 오류: 비유한(UE에 UIMin뿐이다).
    Result<void> SetInfluenceRadius(float radius) noexcept;

    double InfluenceBoundingRadius() const noexcept override {
        return static_cast<double>(influenceRadius_);
    }

private:
    ROCKS_PROPERTY(ReadOnly, Name = "influenceRadius")
    float influenceRadius_ = DefaultInfluenceRadius;
};

} // namespace rocks
