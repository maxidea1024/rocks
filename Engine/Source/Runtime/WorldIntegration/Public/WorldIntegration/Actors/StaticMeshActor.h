#pragma once

#include <Meta/Annotations.h>
#include <World/Actor.h>

namespace rocks {

// 정적 메시를 배치하는 Actor다(UE AStaticMeshActor — Docs/Design/EngineRuntime 30.4절). 프레임워크
// PreAwake에서 StaticMeshComponent root를 준비한다(24.2.5절 규칙: 같은 타입 root는 재사용, 다른
// 타입 root는 Spawn이 InvalidArgument). 만든 컴포넌트는 profile "BlockAll", Mobility Static,
// overlap 이벤트 꺼짐, useDefaultCollision 켬이다(UE StaticMeshActor.cpp:27–38).
//
// 사용 예(root가 Static이므로 게임 World에서는 초기화 전 — Deferred Spawn 중 — 에 메시를 정한다):
//   auto placed = world.SpawnActorDeferred<StaticMeshActor>();
//   auto mesh = world.AddComponent<StaticMeshComponent>(placed.Value());
//   world.With<StaticMeshComponent>(mesh.Value(), [&](StaticMeshComponent& component) noexcept {
//       return component.SetStaticMesh(chair);
//   });
//   world.With<Actor>(placed.Value(), [&](Actor& actor) noexcept {
//       return actor.SetRootComponent(mesh.Value());   // PreAwake가 이 root를 재사용한다
//   });
//   world.FinishSpawning(placed.Value());
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_WORLDINTEGRATION_API StaticMeshActor : public Actor {
    ROCKS_GENERATED_BODY()

public:
    // UE StaticMeshActor.cpp:33의 profile 이름이다.
    static constexpr const char* StaticMeshActorCollisionProfileName = "BlockAll";

    explicit StaticMeshActor(Allocator& allocator) noexcept : Actor(allocator) {
    }

    // root StaticMeshComponent다(PreAwake 전에는 null — UE GetStaticMeshComponent). 뒤에 root를
    // 다른 컴포넌트로 바꾸면 그 handle이다(타입은 부르는 쪽이 With<StaticMeshComponent>로
    // 확인한다).
    ObjectHandle StaticMeshComponentHandle() const noexcept {
        return RootComponent();
    }

protected:
    Result<void> OnFrameworkPreAwake() noexcept override;
};

} // namespace rocks
