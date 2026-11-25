#pragma once

#include <Meta/Annotations.h>
#include <World/Actor.h>

namespace rocks {

// 광원 Actor의 기반이다(UE ALight — Docs/Design/EngineRuntime 24.2.5절). root가 광원 컴포넌트다.
// 파생은 프레임워크 PreAwake에서 root를 만든다(Spawn Transform이 root에 붙는다 — 17.3절). 레벨
// 로드로 그 타입의 root가 이미 있으면 그대로 쓰고, 다른 타입 root가 있으면 Spawn이
// InvalidArgument로 실패한다. 만든 컴포넌트는 Stationary다(UE 광원 Actor 생성자). 데이터만 있다:
// 렌더 연결 전에는 아무것도 비추지 않는다(24.1절).
//
// 사용 예:
//   auto lamp = world.SpawnActor<PointLight>(SpawnParams::At(transform));
//   world.With<PointLight>(lamp.Value(), [&](PointLight& light) noexcept {
//       return world.With<PointLightComponent>(light.LightComponentHandle(),
//           [](PointLightComponent& value) noexcept { return value.SetIntensity(8000.0f); });
//   });
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS(Abstract)
class ROCKS_WORLD_API Light : public Actor {
    ROCKS_GENERATED_BODY()

public:
    explicit Light(Allocator& allocator) noexcept : Actor(allocator) {
    }

    // root 광원 컴포넌트다(PreAwake 전에는 null).
    ObjectHandle LightComponentHandle() const noexcept {
        return RootComponent();
    }

    // 광원을 켜고 끈다: root 컴포넌트의 visible이다(UE ALight::SetEnabled — 복제는 네트워크
    // 단계). 오류: root가 없으면 InvalidArgument.
    Result<void> SetLightEnabled(bool enabled) noexcept;

    // root 컴포넌트의 visible이다(root가 없으면 거짓).
    bool IsLightEnabled() const noexcept;
};

} // namespace rocks
