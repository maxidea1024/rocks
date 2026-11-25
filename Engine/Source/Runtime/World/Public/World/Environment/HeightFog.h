#pragma once

#include <Meta/Annotations.h>
#include <World/Actor.h>

namespace rocks {

// 높이 안개 Actor다(UE AExponentialHeightFog — Docs/Design/EngineRuntime 27.1절). root는
// HeightFogComponent다(root 규칙은 SkyAtmosphere와 같다). 환경 Actor 가운데 이것만 켜고 끄기가
// 있다(UE AExponentialHeightFog의 replicated bEnabled — 복제는 네트워크 단계).
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_WORLD_API HeightFog : public Actor {
    ROCKS_GENERATED_BODY()

public:
    explicit HeightFog(Allocator& allocator) noexcept : Actor(allocator) {
    }

    // 안개를 켜고 끈다: root 컴포넌트의 visible이다(UE OnRep_bEnabled). 오류: root가 없으면
    // InvalidArgument.
    Result<void> SetFogEnabled(bool enabled) noexcept;

    // root visible이다(root가 없으면 거짓).
    bool IsFogEnabled() const noexcept;

protected:
    Result<void> OnFrameworkPreAwake() noexcept final;
};

} // namespace rocks
