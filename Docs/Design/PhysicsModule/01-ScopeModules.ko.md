# 물리 모듈 — 1. 범위·모듈·이름

[목차](README.ko.md)

## 1.1 범위

| 들인다(M4 물리 단계) | 들이지 않는다 / 나중 |
|---|---|
| World마다 물리 장면 하나, 컴포넌트 body 수명(생성·파괴·재생성) | 파괴(fracture)·vehicle·cloth·soft body·rope·buoyancy(16.3절 — 개별 모듈 단계) |
| Static·Kinematic·Dynamic body, 고정 스텝 시뮬레이션과 결과 반영 | Chaos 고유 기능(async 물리 보간, cluster union, field — CD14) |
| 단순 충돌 형상(Box·Sphere·Capsule·Cylinder), 메시 단순 충돌(`SimpleCollision`), complex-as-simple 정적 메시 | heightfield(지형 CM18), skeletal ragdoll·physics asset(CM7·CM14) |
| 장면 질의(line·shape trace, overlap), 질의 필터(채널·object type·profile) | 비동기 질의(UE `AsyncLineTrace` — 필요할 때 4.7절) |
| `MoveComponent` sweep 이동, 침투 해소 질의, overlap·hit 이벤트 | 캐릭터 이동(CM13 — 이 질의 API 위에 따로) |
| 힘·충격량·속도·질량·잠듦 API, DOF 잠금, CCD, weld | constraint·handle·spring·thruster·radial force(CM14) |
| 물리 재질의 기본값 경로(마찰·반발 기본값) | 물리 재질 에셋(에셋 단계 — 6.6절) |

## 1.2 모듈

| 모듈 | 위치 | 의존 | 책임 |
|---|---|---|---|
| `Collision`(있음, CD1) | `Engine/Source/Runtime/Collision` | ObjectRuntime, CoreTypeBindings, Config | 채널·응답·충돌 켬, profile 표, `BodySettings`, 질의 형상 값 `CollisionShape`(4.3절 — 이 문서가 더한다) |
| `World`(있음) | `Engine/Source/Runtime/World` | … Collision | 물리 장면 **접점**(추상 `PhysicsScene` — 2.2절), 질의 결과·인자 타입(컴포넌트·Actor 참조를 담으므로 World), `PrimitiveComponent`의 body 수명·`MoveComponent`·overlap 장부 |
| `Physics`(새) | `Engine/Source/Runtime/Physics` | 공개: World, Collision. 비공개: Jolt, Task | `PhysicsScene` 구현(Jolt), Jolt hook·job·할당·버전 확인, 좌표·단위 어댑터(어댑터 경계 7절 — private), body·형상 생성, step, 질의, 접촉 수집, `PhysicsWorldSubsystem` |
| 호스트(Sandbox·게임 타깃) | | … Physics | `GameInstanceConfig::worldSubsystems`에 `PhysicsWorldSubsystem::Type()`을 넣는다(`Gameplay/Private/GameInstance.cpp:595–605` — 설정 목록 다음에 `GameplayWorldSubsystem`이 붙는 자리). Gameplay는 Physics에 의존하지 않는다(PMD3 — Gameplay를 쓰는 도구·시험이 Jolt를 링크하지 않게) |

- Physics는 전용 서버에서도 쓴다(ClientOnly가 아니다 — 어댑터 경계 10.3절).
- Jolt 헤더는 Physics의 private 번역 단위만 include한다(STL 정책 좁은 예외 — 어댑터 경계 10.3절). Physics 공개 헤더는 Jolt 타입을 담지 않는다.
- World는 Physics에 의존하지 않는다. 컴포넌트는 World에 있는 추상 접점(2.2절)을 통해서만 물리를 부른다. 그래서 물리가 없는 World(헤드리스 시험·도구)에서 컴포넌트는 body 없이 돈다(UE에서 `GetPhysicsScene()`이 null인 World와 같다 — `E/Private/Components/ActorComponent.cpp:1138–1140`).

## 1.3 이름 대응

Rocks 고유 이름 원칙(UE 이름은 대응표에만, 업계 공용어는 예외)을 따른다. trace·sweep·overlap·hit·body·kinematic·weld·sleep은 업계 공용어라 그대로 쓴다.

| UE | Rocks | 비고 |
|---|---|---|
| `FPhysScene`(`FPhysScene_Chaos`) | `PhysicsScene`(World의 추상 접점) / `JoltPhysicsScene`(Physics private 구현) | |
| `FBodyInstance`(실행 상태 부분) | `PhysicsBodyHandle`(index + generation — 계획 16.1절) | 값 부분은 `BodySettings`(CD1) |
| `UWorld::LineTraceSingleByChannel` 등 27종 | `LineTrace`·`ShapeTrace`·`Overlap` × `Test`/`First`/`All` + `QueryFilter`(4.1절) | 필터 종류를 인자로 합쳤다(4.1절 PMD8) |
| `FCollisionQueryParams` | `QueryParams` | |
| `FCollisionResponseParams`·`FCollisionObjectQueryParams` | `QueryFilter`(채널 + 응답 묶음 / object type 집합 / profile 이름) | |
| `FCollisionShape` | `CollisionShape`(Collision 모듈) | |
| `FHitResult`·`FOverlapResult` | `HitResult`·`OverlapResult` | 공용어 |
| `ETeleportType` | `TeleportMode { None, TeleportPhysics, ResetPhysics }` | |
| `UPrimitiveComponent::MoveComponent` | `PrimitiveComponent::MoveComponent` | 공용 동작 이름 |
| `UpdateOverlaps` | `UpdateOverlaps` | |
| `FCollisionNotifyInfo` | `ContactNotice`(Physics 내부) | |
