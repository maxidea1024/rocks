#pragma once

#include <Meta/Annotations.h>
#include <World/Actor.h>

namespace rocks {

// decal Actor다(UE ADecalActor — Docs/Design/EngineRuntime 27.4절). root는 DecalComponent이고 상대
// 회전은 pitch −90°(아래)다. 이름은 컴포넌트와 구별하려고 Actor를 붙였다.
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_WORLD_API DecalActor : public Actor {
    ROCKS_GENERATED_BODY()

public:
    // 새 root의 기본 pitch(도, UE DecalActor.cpp:30)다.
    static constexpr double DefaultPitchDegrees = -90.0;

    explicit DecalActor(Allocator& allocator) noexcept : Actor(allocator) {
    }

protected:
    Result<void> OnFrameworkPreAwake() noexcept final;
};

} // namespace rocks
