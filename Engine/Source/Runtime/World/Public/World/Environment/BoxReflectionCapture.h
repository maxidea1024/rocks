#pragma once

#include <Meta/Annotations.h>
#include <World/Actor.h>
#include <World/Environment/ReflectionCapture.h>

namespace rocks {

// 상자 반사 캡처 Actor다(UE ABoxReflectionCapture). root 상대 scale (1000, 1000, 400)이 상자
// 크기다.
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_WORLD_API BoxReflectionCapture : public ReflectionCapture {
    ROCKS_GENERATED_BODY()

public:
    // 기본 상자 scale(UE ReflectionCaptureComponent.cpp:335)이다.
    static constexpr double DefaultScaleXY = 1000.0;
    static constexpr double DefaultScaleZ = 400.0;

    explicit BoxReflectionCapture(Allocator& allocator) noexcept : ReflectionCapture(allocator) {
    }

protected:
    Result<void> OnFrameworkPreAwake() noexcept final;
};

} // namespace rocks
