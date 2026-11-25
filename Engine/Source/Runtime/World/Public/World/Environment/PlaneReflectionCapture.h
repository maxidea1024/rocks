#pragma once

#include <Meta/Annotations.h>
#include <World/Actor.h>
#include <World/Environment/ReflectionCapture.h>

namespace rocks {

// 평면 반사 캡처 Actor다(UE APlaneReflectionCapture). root 상대 scale (1, 1000, 1000)이다.
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_WORLD_API PlaneReflectionCapture : public ReflectionCapture {
    ROCKS_GENERATED_BODY()

public:
    // 기본 평면 scale(UE ReflectionCaptureComponent.cpp:387)이다.
    static constexpr double DefaultScaleYZ = 1000.0;

    explicit PlaneReflectionCapture(Allocator& allocator) noexcept : ReflectionCapture(allocator) {
    }

protected:
    Result<void> OnFrameworkPreAwake() noexcept final;
};

} // namespace rocks
