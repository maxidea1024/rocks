# 물리 모듈 — 7.1 단계별 구현 세부(PH4)

[목차](README.ko.md) · [7. 단계·시험·결정](07-StagesDecisions.ko.md) · [PH1–PH3](07a-StageDetailsPH1-PH3.ko.md)

7장 단계 표(7.1절)의 PH4 하위 단계 구현 세부다. 절 번호는 7장 그대로다.

### 7.1.10 PH4 하위 단계와 PH4a 구현 세부(2026-11-27)

PH4는 다섯으로 나눈다(크기 — "순서대로"). 각 하위 단계는 설계 갱신 → 구현 → 리뷰어 1명 → 커밋이다.

| 하위 단계 | 내용 | 선행 |
|---|---|---|
| PH4a | `MoveComponent` sweep 이동(5.1절 — 막는 hit 고르기·pullback·도착·이동 선택·teleport·이동 무시 목록)과 sweep hit 발행(Actor 가상 hook) | PH3 |
| PH4b | overlap 장부와 발행(5.3절 — 이동의 대기 겹침, `UpdateOverlaps`, 시작·충돌 변경·파괴 때 다시 계산, Actor 가상 hook) | PH4a |
| PH4c | 시뮬레이션 접촉 hit와 probe(5.4·6.4절 — `ContactListener`, `notifyRigidBodyCollision`, solver 충격량, CCD 추정, hit 문턱) | PH4a |
| PH4d | 침투 해소 도우미(5.2절 — `ComputePenetration`·`SafeMove`·`ResolvePenetration`) | PH3 |
| PH4e | 컴포넌트·Actor 이벤트 필드(5.5절 — `OnComponentHit`·`OnComponentBeginOverlap` 등) | EV2(리플렉션 이벤트 필드 — EventReflection 1절) |

이벤트 필드는 리플렉션 이벤트(`Event<Args...>` — EventReflection ED1)를 클래스 필드로 두는 EV2가 끝나야 달 수 있다. 그 전에는 5.4절대로 Actor 가상 hook만 부른다(PH4e에서 같은 자리에 이벤트 발행을 더한다).

**Actor 가상 hook 이름.** Rocks Actor hook 이름(`OnStart` — UE `BeginPlay`, `OnDestroy` — `EndPlay`)을 따라 `OnHit`(UE `NotifyHit`)·`OnBeginOverlap`(UE `NotifyActorBeginOverlap`)·`OnEndOverlap`(UE `NotifyActorEndOverlap`)이다. 5.5절 표의 UE 이름은 대응표다(1.3절에 더한다).

**PH4a 이동 선택**(World `World/Physics/MoveOptions.h` — UE `EMoveComponentFlags`·`ETeleportType`): Rocks에는 enum flag 도우미가 없어 bool 칸을 가진 값 struct로 둔다.
- `skipPhysicsMove`(UE `MOVECOMP_SkipPhysicsMove` — 자기 body로 되돌려 보내지 않는다, 3.4절 결과 반영의 표지와 같다).
- `neverIgnoreBlockingOverlaps`(`MOVECOMP_NeverIgnoreBlockingOverlaps`).
- `disableBlockingOverlapDispatch`(`MOVECOMP_DisableBlockingOverlapDispatch` — 시작 겹침 hit를 발행하지 않는다).
- `teleport`(`TeleportMode` — body 이동 방식, 3.3절).

**PH4a `MoveComponent`**(`PrimitiveComponent::MoveComponent(delta, rotation, sweep, HitResult* outHit, const MoveOptions&) → Result<bool>`(움직였는지 — UE `MoveComponentImpl` `PrimitiveComponent.cpp:3250–3585`):
1. Mobility가 Movable이 아니면(등록 뒤 게임 World — `CheckMovable`) 움직이지 않고 오류다(UE는 경고 뒤 `false` — Rocks 이동 setter와 같은 `InvalidArgument`·`StaticComponentMoved`).
2. 최소 이동: sweep이고 이동 제곱 거리가 (4·`KindaSmallNumber`)² 이하이며 회전이 같으면(성분 허용 오차 `MoveQuaternionTolerance` 1e-8 — UE `SCENECOMPONENT_QUAT_TOLERANCE`) 움직이지 않고 참이다(`outHit`는 trace 끝점만 채운 빈 hit — `:3281–3296`). 회전만 바뀌면 이동량을 0으로 둔다.
3. sweep 없음: 새 위치·회전으로 바로 옮긴다(`:3311–3317`).
4. sweep: 질의 충돌이 켜져 있고(실효 충돌 켬에 query가 있음) 이동량이 0보다 크면 `ComponentTraceAll`(7.1.9절 — 시작 회전 고정, 채널·응답은 자기 것, 자기 Actor 제외)을 부른다(`:3330–3354`).
   - 무시 목록 `moveIgnoreActors`·`moveIgnoreComponents`(UE `MoveIgnoreActors`·`MoveIgnoreComponents` — `InitSweepCollisionParams`)를 질의 무시 목록으로 넘긴다.
   - 자기 `generateOverlapEvents`가 거짓이면 겹침을 무시한다(`ignoreTouches` — `:3351`).
   - pullback(`PullBackHit` — `:3095–3099`): 모든 hit의 `time`에서 clamp(0.1, 0.1/D, 1/D) + 0.001을 빼고 [0, 1]로 자른다(D = 이동 거리 cm). 5.1절의 "약 0.1 cm"는 틀렸다: D가 10 cm를 넘으면 1/D + 0.001이라 거리로 1 cm + 0.001·D다(D = 500 cm면 1.5 cm). 이름 있는 상수 `PullbackTime`(0.1)·`PullbackTimeSlack`(0.001)로 둔다.
   - 막는 hit 고르기(`:3380–3398`, `ShouldIgnoreHitResult` `:3115–3165`): 막는 hit 가운데 시작 겹침(또는 거리 < `HitDistanceTolerance` 0)이고 `impactNormal · 이동 방향 > InitialOverlapTolerance`(0)인 것은 빠져나가는 중이라 무시한다(`neverIgnoreBlockingOverlaps`면 무시하지 않는다). 남은 시작 겹침 중 `impactNormal · delta`가 가장 작은 것, 없으면 첫 시작 겹침 아닌 막음이다. UE `MOVECOMP_IgnoreBases`는 쓰는 곳(캐릭터 이동 CM13)이 생길 때 더한다.
   - 도착: 막음이 없으면 끝, 있으면 시작 + time·delta다. 시작에서 도착까지 제곱 거리가 최소 이동 이하면 시작에 머물고 time 0이다(`:3434–3456`).
   - 질의 충돌이 꺼져 있으면 sweep 없이 delta만큼 옮긴다(`:3480–3485`).
   - 겹침 수집(`:3399–3420`)은 PH4b다.
5. 옮기기: 위치·회전이 바뀌지 않았으면(허용 오차 `KindaSmallNumber` — UE `InternalSetWorldLocationAndRotation`의 `Equals` 기본값) transform을 설정하지 않고 움직이지 않은 것이다(자손 body·Render·bounds를 건드리지 않는다). 바뀌었으면 부착 전파·body 이동(3.3절)이다. 이동 중의 body 이동 방식(teleport·skip)은 자손에도 같다(UE `UpdateComponentToWorld`가 teleport를 자식에 넘긴다). 그래서 이동 범위 값으로 둔다(게임 thread 범위 — `PrimitiveComponent.cpp` 안 thread_local, 공개 헤더에는 두지 않는다). `skipPhysicsMove`는 자기 body만이다(3.4절 결과 반영과 같다 — 자식 body는 따라간다).
6. hit 발행(`:3537–3550`, `DispatchBlockingHit` `:3588–3607`, `AActor::InternalDispatchBlockingHit` `Actor.cpp:3525–3556`): 막는 hit가 있고 (시작 겹침이 아니거나 `disableBlockingOverlapDispatch`가 거짓)이며 자기 컴포넌트가 파괴 대기가 아니면 발행한다.
   - 자기 Actor `OnHit(자기 컴포넌트, 상대 Actor, 상대 컴포넌트, selfMoved 참, normalImpulse 0, hit)`.
   - 그 뒤 상대 컴포넌트가 아직 살아 있으면 상대 Actor `OnHit(상대 컴포넌트, 자기 Actor, 자기 컴포넌트, selfMoved 거짓, 0, 뒤집은 hit)`. 뒤집은 hit는 `normal`·`impactNormal`의 부호를 바꾼 것이다(UE `FHitResult::GetReversedHit`).
   - 파괴 대기 Actor에는 발행하지 않는다(UE `IsActorValidToNotify`).
7. `outHit`에는 막는 hit를 쓰고, 없으면 trace 시작·끝만 채운 빈 hit를 쓴다.

**이동 무시 목록 API**(UE `IgnoreActorWhenMoving`·`IgnoreComponentWhenMoving`·`ClearMoveIgnoreActors`·`ClearMoveIgnoreComponents`·`GetMoveIgnoreActors`): `Result<void> IgnoreActorWhenMoving(actor, bool ignore)`·`IgnoreComponentWhenMoving`(추가는 할당 실패가 있다, 중복은 하나), `ClearMoveIgnoreActors()`·`ClearMoveIgnoreComponents()`, `MoveIgnoreActors()`·`MoveIgnoreComponents()`(Span). NATIVE 필드다(UE `Transient`).

**PH4a 리뷰 반영(2026-11-27).** 치명 0, 중요 2, 경미 5.
- 움직이지 않은 이동은 transform을 설정하지 않는다(위 5 — UE). 전에는 막혀 시작에 머무는 이동마다 자손 body·Render·bounds 갱신과 상대 값 재계산(반올림 누적)이 있었다.
- 시험을 보강했다: query 없는 PhysicsOnly 컴포넌트, `skipPhysicsMove`(자기 body는 그대로, 자식 body는 따라감), 회전만 바뀌는 sweep, pullback으로 time이 0이 되는 가까운 벽, Static 이동의 오류 코드.
- 한계: sweep 이동마다 hit 배열을 할당한다(UE `TArray`와 같다). 한 프레임에 여러 번 부르는 캐릭터 이동(CM13)에서 재사용 저장소로 바꾼다. 이동 범위의 teleport 방식은 범위 안의 다른 setter 이동에도 적용된다(코드 주석).

**시험**(`PhysicsMoveTests.cpp`):
- 벽으로 sweep하면 pullback만큼 앞에 멈추고 `outHit`가 막음이다(수치는 UE 식). sweep 없는 이동은 지나간다.
- 최소 이동은 움직이지 않고 참이다.
- 시작 겹침에서 빠져나가는 이동은 무시되어 끝까지 가고, 들어가는 이동은 시작에 머문다. `neverIgnoreBlockingOverlaps`면 빠져나가는 이동도 막힌다.
- hit 발행: 자기 Actor가 먼저(selfMoved 참, 법선은 자기 쪽), 그다음 상대 Actor(selfMoved 거짓, 뒤집은 법선)다. `disableBlockingOverlapDispatch`면 시작 겹침 hit를 발행하지 않는다. 발행 중 상대 컴포넌트를 파괴하면 상대 쪽 발행이 없다.
- 이동 무시 목록(Actor·컴포넌트)·NoCollision 컴포넌트는 지나간다.
- teleport: Kinematic body를 `TeleportPhysics`로 옮기면 다음 step 속도가 0이고, `None`이면 이동량/dt다. 자식 body도 같은 방식이다.
- Static 컴포넌트 이동은 오류다.
