#pragma once

#include <Meta/Annotations.h>
#include <World/Actor.h>
#include <World/Environment/ReflectionCapture.h>

namespace rocks {

// 구 반사 캡처 Actor다(UE ASphereReflectionCapture — ReflectionCaptureComponent.cpp:282).
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_WORLD_API SphereReflectionCapture : public ReflectionCapture {
    ROCKS_GENERATED_BODY()

public:
    explicit SphereReflectionCapture(Allocator& allocator) noexcept : ReflectionCapture(allocator) {
    }

protected:
    Result<void> OnFrameworkPreAwake() noexcept final;
};

} // namespace rocks
