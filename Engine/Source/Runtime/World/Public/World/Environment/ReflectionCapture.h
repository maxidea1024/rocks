#pragma once

#include <Meta/Annotations.h>
#include <World/Actor.h>

namespace rocks {

// 반사 캡처 Actor의 기반이다(UE AReflectionCapture — Docs/Design/EngineRuntime 27.6절). root가 반사
// 캡처 컴포넌트(Static)다. 파생: Sphere·Box·PlaneReflectionCapture.
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS(Abstract)
class ROCKS_WORLD_API ReflectionCapture : public Actor {
    ROCKS_GENERATED_BODY()

public:
    explicit ReflectionCapture(Allocator& allocator) noexcept : Actor(allocator) {
    }

    // root(반사 캡처 컴포넌트)다. PreAwake 전에는 비어 있다.
    ObjectHandle CaptureComponentHandle() const noexcept {
        return RootComponent();
    }
};

} // namespace rocks
