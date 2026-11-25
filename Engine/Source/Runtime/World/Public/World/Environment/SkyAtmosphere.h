#pragma once

#include <Meta/Annotations.h>
#include <World/Actor.h>

namespace rocks {

// 대기 Actor다(UE ASkyAtmosphere — Docs/Design/EngineRuntime 27.1절). root는
// SkyAtmosphereComponent다. 환경 Actor는 광원 Actor와 같은 규칙으로 PreAwake에서 root를 만든다(같은
// 타입 root는 재사용, 다른 타입은 Spawn이 InvalidArgument — 24.2.5절). 데이터만 있다: 렌더 연결
// 전에는 그리지 않는다.
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_WORLD_API SkyAtmosphere : public Actor {
    ROCKS_GENERATED_BODY()

public:
    explicit SkyAtmosphere(Allocator& allocator) noexcept : Actor(allocator) {
    }

protected:
    Result<void> OnFrameworkPreAwake() noexcept final;
};

} // namespace rocks
