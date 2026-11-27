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

### 7.1.11 PH4b 구현 세부(2026-11-27)

overlap은 질의 기반 장부다(5.3절 — 시뮬레이션 접촉이 아니다). UE `BeginComponentOverlap`·`EndComponentOverlap`·`UpdateOverlapsImpl`·`ClearComponentOverlaps`(`PrimitiveComponent.cpp:4100–4720`)를 따른다.

**장부.** `PrimitiveComponent`의 NATIVE `overlappingComponents_`(겹친 컴포넌트 handle — UE `OverlappingComponents`, 저장하지 않는다)다. 겹침은 대칭이다(양쪽에 기록). 조회 `OverlappingComponents()`(Span)·`IsOverlappingComponent(handle)`와 Actor 단위 `IsOverlappingActor(world, a, b)`(a의 Primitive 컴포넌트 중 b의 컴포넌트와 겹친 것이 있는지 — UE `AActor::IsOverlappingActor`)를 둔다.

**시작(`BeginComponentOverlap` — `:4100–4172`).** 자기가 파괴 대기이거나 이미 겹쳐 있으면 하지 않는다. 겹칠 수 있어야 한다(UE `CanComponentsGenerateOverlap` — `:224–231`: 양쪽 `generateOverlapEvents`, 양쪽 실효 충돌이 `NoCollision`이 아님, 서로의 object type에 대한 응답의 최소가 Overlap). 새 Actor 쌍인지(같은 Actor가 아니고 두 Actor가 아직 겹치지 않음)를 먼저 본 뒤 양쪽 장부에 넣는다. 알림은 World가 Playing일 때만이다. 컴포넌트 이벤트는 PH4e다. 새 Actor 쌍이면 자기 Actor `OnBeginOverlap(상대 Actor)`, 그다음 상대 Actor `OnBeginOverlap(자기 Actor)`다(파괴 대기 Actor 제외 — UE `IsActorValidToNotify`).

**끝(`EndComponentOverlap` — `:4174–4231`).** 상대 장부에서 빼고 자기 장부에서 뺀다. 자기 장부에 있었고 World가 Playing이면, 같은 Actor가 아니고 두 Actor가 더는 겹치지 않을 때 자기 Actor `OnEndOverlap(상대)`, 그다음 상대 Actor `OnEndOverlap(자기)`다.

**갱신(`UpdateOverlaps(대기 겹침)` — `:4464–4650`).**
1. 소유 Actor가 Playing이 아니면 하지 않는다(자식도 — UE `HasActorBegunPlay`).
2. 자기 `generateOverlapEvents`가 참이고 질의 충돌이 켜져 있으면:
   - 대기 겹침을 먼저 시작한다.
   - 아직 파괴 대기가 아니고 `generateOverlapEvents`가 참이면 지금 자리에서 `ComponentOverlapAll`(`ignoreBlocks` — 막음은 overlap 이벤트가 아니다, 이동 무시 목록, 루트면 자기 Actor 전체를 뺀다 — 자식은 자기 차례에 갱신한다)로 새 목록을 얻는다. 자기 아님·상대 `generateOverlapEvents`·상대 Actor가 있고 Constructed가 아닌 것만 남긴다(UE `ShouldIgnoreOverlapResult` — `:3170–3200`).
   - 옛 목록(루트면 같은 Actor 항목은 빼고 본다)과 비교해 옛 목록에만 있는 것은 끝내고(사라진 컴포넌트 handle은 알림 없이 지운다), 새 목록에만 있는 것은 시작한다(끝이 먼저다).
3. 그렇지 않으면 기존 겹침을 모두 끝낸다(방금 꺼졌을 수 있다).
4. 자식을 복사본으로 돌며 갱신한다(발행 중 자식 목록이 바뀔 수 있다). Primitive가 아닌 SceneComponent는 자식만 갱신한다(UE `USceneComponent::UpdateOverlaps`).
- UE의 끝 위치 겹침 재사용(`bAllowCachedOverlaps`)과 회전만 한 이동의 장부 재사용은 최적화라 두지 않는다(늘 질의 — 결과는 같다).

**갱신하는 때.**
- `MoveComponent`(5.1절 7·8): sweep hit 가운데 막음이 아닌 것(겹침) 중 자기와 상대가 `generateOverlapEvents`이고 무시 대상이 아니며 막는 hit의 time 이하인 것을 중복 없이 대기 목록에 둔다(`:3399–3420`). 도착 위치 검사로 시작에 머물면 시작 겹침만 남긴다(`:3450–3454`). 움직였으면 옮긴 뒤 `UpdateOverlaps(대기 목록)`다. 옮기는 setter의 자동 갱신(아래)은 이 이동 동안 끈다.
- transform을 바꾸는 setter(위치·회전·scale·절대 표지·부착·떼기): SceneComponent의 루트 transform 통지가 끝날 때 `UpdateOverlaps()`다(UE는 이 setter가 모두 `MoveComponent(sweep 없음)`를 거쳐 갱신한다). 자식 전파 중에는 부르지 않는다(루트의 갱신이 자식으로 내려간다).
- 결과 반영(`ApplyPhysicsTransform`): 옮기는 동안 자동 갱신을 미뤄 두고, 결과 반영 표지(자기 body로 되돌려 보내지 않음)를 되돌린 뒤 갱신한다. hook이 이 컴포넌트를 옮기면 body도 따라가야 한다(UE `MOVECOMP_SkipPhysicsMove`는 인자라 hook에 새지 않는다). `MoveComponent`의 teleport 방식·`skipPhysicsMove`도 옮기기 범위에만 적용하고 overlap 갱신은 그 밖에서 한다.
- 형상 크기 변경(`SetBoxExtent`·`SetSphereRadius`·`SetCapsuleSize` → `UpdateBodyGeometry`): 갱신한다(UE 형상 setter의 `bUpdateOverlaps` 기본 참).
- Actor 시작: OnStart 뒤 Playing이 되면 이 Actor 안에서 맨 위인 SceneComponent(부모가 없거나 부모가 다른 Actor 것 — 손에 든 무기)마다 갱신하고 자식으로 내려간다(UE `AActor::BeginPlay` → `UpdateInitialOverlaps` — `Actor.cpp:4801`·`2386`). UE는 루트 하나에서 시작하지만 Rocks Actor는 루트를 자동으로 정하지 않아(`SetRootComponent`로만) 맨 위 컴포넌트가 여럿일 수 있다. 알림이 이 Actor를 파괴하면 남은 컴포넌트 갱신과 Tick 등록을 하지 않는다. 루트가 없는 Actor에서는 어느 질의도 자기 Actor를 빼지 않으므로 같은 Actor 겹침도 각 컴포넌트의 갱신이 직접 본다(Actor 알림은 여전히 없다).
- 충돌 설정 변경(UE `OnComponentCollisionSettingsChanged` — `PrimitiveComponentPhysics.cpp:1434–1447`): 충돌 setter·profile 이름 setter 뒤 등록돼 있으면 갱신한다.
- 파괴(UE `OnComponentDestroyed` — `PrimitiveComponent.cpp:2026–2044`): `OnDestroy`에서 `generateOverlapEvents`를 끄고 겹침을 모두 끝낸다(알림 있음).
- `SetGenerateOverlapEvents`는 바로 갱신하지 않는다(UE도 표지만 바꾼다 — 다음 갱신 때 반영).

**Actor 가상 hook.** `OnBeginOverlap(ObjectHandle otherActor)`·`OnEndOverlap(ObjectHandle otherActor)`(UE `NotifyActorBeginOverlap`·`NotifyActorEndOverlap`).

**시험**(`PhysicsOverlapEventTests.cpp`):
- sweep 이동이 겹침 상자를 지나 끝에서 겹치지 않으면 시작·끝이 한 번씩, 끝에서 겹치면 시작만 남는다. 양쪽 장부가 대칭이다.
- Actor 알림 순서(자기 → 상대), 같은 Actor 컴포넌트끼리는 Actor 알림이 없다, 두 컴포넌트가 같은 상대와 겹치면 Actor 시작·끝은 한 번씩이다.
- 응답이 Block이거나 한쪽 `generateOverlapEvents`가 거짓이면 겹침이 아니다.
- Actor 시작 때 이미 겹친 것을 시작한다. 위치 setter·결과 반영(떨어지는 Dynamic 구)도 갱신한다.
- 충돌을 Ignore로 바꾸면 끝, 파괴하면 끝 알림이 온다. `generateOverlapEvents`를 끄면 다음 갱신 때 끝난다.
- 발행 중 상대를 파괴해도 장부가 망가지지 않는다.

**PH4b 리뷰 반영(2026-11-27).** 치명 0, 중요 3, 경미 6.
- 결과 반영 중 overlap hook이 결과 반영 표지를 켠 채 돌아, hook에서 옮긴 Dynamic body가 따라가지 않았다(위 "결과 반영"). `MoveComponent`의 teleport 범위도 옮기기로 좁혔다.
- 다른 Actor 컴포넌트에 붙은 Actor의 시작 겹침과 형상 크기 변경 갱신을 더했다(위). 시작 알림이 자기 Actor를 파괴하면 이어지는 시작 처리를 멈춘다.
- 파괴로 고아가 된 자식 통지(`DetachForDestroy`)는 목록을 떼어 내 모두 고아로 만든 뒤 통지한다(통지의 overlap hook이 다른 부착을 바꿀 수 있다).
- 시험을 보강했다: 루트를 정한 Actor 경로, trigger를 지나 벽에 막히는 sweep(막는 hit 뒤 겹침 제외), 이동 무시 Actor, 형상 크기, 다른 Actor에 붙은 Actor 시작, hook 안 충돌 끄기·텔레포트·자기 파괴, EndPlay 중 알림 없음.

**한계.**
- 루트가 있는 Actor에서 루트와 부착되지 않은 형제 사이의 같은 Actor 겹침은 루트 갱신이 보지 않으므로 형제가 갱신될 때까지 남는다(UE도 구조는 같다 — Rocks는 루트가 자동이 아니라 더 흔하다).
- transform setter·결과 반영마다 자기와 모든 자손이 질의한다(UE `bCanSkipUpdateOverlaps` 하위 트리 생략 없음). `IsOverlappingActor`는 컴포넌트 수 × 장부 크기라 큰 trigger의 모두 끝내기는 O(N²)다. 측정이 필요해지면 Actor 쌍 계수 장부로 바꾼다.
- 상대를 더는 빌릴 수 없으면(우회 파괴) 그 Actor 쌍의 끝 알림은 없다(UE의 무효 weak pointer와 같다).
- PH4c 메모: 접촉 콜백은 step 중에 돈다. 거기서 충돌 setter → overlap 갱신(질의)을 부르면 step 중 질의 검사에 걸리므로 PH4c는 hit 발행을 step 뒤로 미룬다.

### 7.1.12 PH4c 구현 세부(2026-11-27)

시뮬레이션 접촉 hit와 probe다(5.4·6.4절). UE `FPhysScene_Chaos::HandleCollisionEvents`·`HandleEachCollisionEvent`(`E/Private/PhysicsEngine/Experimental/PhysScene_Chaos.cpp:1043–1176`), Chaos 충돌 이벤트 수집(`Experimental/Chaos/Private/EventDefaults.cpp`의 충돌 이벤트 등록 — 아래 인용은 이 파일), `AActor::DispatchPhysicsCollisionHit`(`E/Private/Actor.cpp:5690–5736`)를 따른다.

**UE 대조로 고친 것.**
- 5.4절의 "같은 쌍의 여러 스텝 접촉은 하나로 합친다"는 틀렸다. UE는 쌍과 solver 시간(`SolverTime`)을 key로 묶는다(`GetPendingCollisionForContactPair` — `PhysScene_Chaos.cpp:1022–1041`). 한 스텝 안의 같은 쌍 접촉만 합치고, 스텝이 다르면 따로 발행한다. Rocks도 스텝마다 따로다(한 프레임에 고정 스텝이 여럿이면 hit도 여럿).
- UE 5.8은 `MinDeltaVelocityForHitEvents`로 `bIsVelocityDeltaUnderThreshold`를 계산만 하고 어디서도 읽지 않는다(`PhysScene_Chaos.cpp:1087–1089` — 문턱이 효과가 없다). Rocks는 6.4절대로 거른다. 기본 0이면 거르지 않으므로 기본 동작은 UE와 같다.

**무엇이 hit인가**(UE `EventDefaults.cpp` 충돌 수집): `notifyRigidBodyCollision`이 참인 body가 낀 접촉 가운데, 이번 스텝에 solver 충격량이 0이 아니거나 probe 접촉(접촉점 있음)인 것이다. 쉬는 접촉(바닥에 놓인 구)도 충격량이 있어 스텝마다 hit가 난다(UE와 같다). 닿지 않은 speculative 접촉은 충격량이 0이라 빠진다. 잠든 body는 접촉을 보고하지 않는다(Jolt·Chaos 같다).

**수집(step 안 — `ContactListener`).** `JoltPhysicsScene`이 Jolt `ContactListener`를 단다. `OnContactAdded`·`OnContactPersisted`(Jolt는 둘 다 매번 접촉 설정을 새로 채운다)에서:
1. 어느 쪽이든 object 층의 probe 참여 표지가 있으면 `mIsSensor = true`(반작용 없음 — CCD도 지킨다).
2. 두 body의 칸(user data)에서 `notifyRigidBodyCollision` 표지를 읽는다. 칸 표는 step 동안 쓰지 않으므로(쓰기 접점은 `AssertNotStepping`) worker가 읽어도 된다. 둘 다 거짓이면 끝.
3. probe 접촉은 깊이가 음수(speculative 거리 안에서 떨어진 접촉)면 버린다. Jolt probe body는 sensor가 아니라 manifold가 speculative 거리(2 cm)로 만들어지지만, UE는 probe를 거리 0으로 다시 검출해 실제로 겹친 것만 낸다(Chaos `DetectProbeCollisions` — `PBDCollisionConstraints.cpp:582–600`).
4. `ContactNotice`를 복사한다: 두 body user data(Jolt 순서 — body 1 ID < body 2 ID), 하위 형상 ID 둘, 첫 접촉점(1·2 쪽 첫 점의 가운데, Jolt 좌표 — 발행 때 월드 cm로 바꾼다), 법선(`mWorldSpaceNormal` — body 1에서 body 2 쪽), 깊이, probe 여부, 추정 충격량(probe가 아니고 어느 쪽이 CCD(LinearCast) body일 때만 `EstimateCollisionResponse` — 접촉점 충격량 합 × 법선 = body 2가 받는 값, 반복 수·반발 최소 속도는 Jolt 장면 설정 값. 추정값은 CCD로만 닿은 쌍에만 쓰이므로 알림 접촉마다 속도 반복을 돌지 않는다).
5. 버퍼는 mutex 하나로 지키는 미리 잡은 배열이다(할당하지 않는다 — 6.4절의 worker별 버퍼 대신: 알림 body가 낀 쌍만 잠그므로 경합이 작고 순서는 뒤에서 정렬로 정한다). 용량은 step 전에 게임 thread가 max(`MinContactNoticeCapacity` 64, 2 × 지난 step의 (모은 수 + 놓친 수), 4 × 알림 body 수)로 늘린다. 넘치면 세고 step 뒤 Warning이다(다음 step은 늘어난 용량).
6. 게임 코드를 부르지 않는다.

**스텝 정리(step 직후, 게임 thread — `Step` 안).**
1. 칸 번호가 작은 쪽·큰 쪽, 하위 형상 ID 둘, 깊이·접촉점 좌표·법선·추정 충격량 순으로 정렬한다(worker 배정이 비결정적이다. LinearCast body는 같은 하위 형상 쌍에 discrete·CCD 콜백이 둘 올 수 있어 값으로 끝까지 가른다 — `ContactListener.h:63–65`).
2. 같은 body 쌍을 하나로 합친다: 첫 항목의 접촉점·법선·깊이, probe는 하나라도 있으면 참, 추정 충격량은 합. 사라진 칸(세대 불일치)은 버린다.
3. 충격량(body 2가 받는 값): probe면 0. 아니면 `GetAppliedContactImpulse(body 1, body 2)`가 참이고 접촉점이 있으면 그 값, 아니면(CCD로만 닿음 — Jolt CCD 캐시 항목은 접촉점이 0이다, `ContactConstraintManager.cpp:1467–1473`) 항목들의 추정 충격량 합이다.
4. 거르기: probe가 아니고 충격량이 정확히 0이면 버린다(UE `!AccumulatedImpulse.IsZero()`). 문턱 `minDeltaVelocityForHitEvents`(cm/s)가 0보다 크고 probe가 아니면(probe는 반작용이 없어 속도 변화가 늘 0이다), 두 body의 선속도 변화(step 끝 − step 시작, 성분마다)가 모두 문턱 이하일 때 버린다(UE `IsNearlyZero(threshold)`). step 시작 속도는 `Update` 전에 게임 thread가 활성 Dynamic body마다 칸에 찍는다(문턱이 있을 때만 — 중력 적분 전 값으로 UE `PreV`와 같다, `PBDRigidsEvolutionGBF.cpp:908`. 콜백의 body 속도는 Jolt가 중력을 적분한 뒤라 쉬는 접촉에서도 g·dt만큼 달라 쓰지 않는다). step 시작에 잠들어 있던 body는 0에서 시작한다. Dynamic이 아닌 body는 변화 0이다(UE도 rigid 입자만 계산한다).
5. 쌍의 각 쪽 가운데 알림 표지가 참인 쪽마다(칸 번호가 작은 쪽 먼저) `PhysicsContactHit`를 프레임 목록 끝에 더한다(UE는 등록한 컴포넌트마다 따로 항목 — `:1102–1106`). 할당 실패는 세고 step마다 Warning 한 번이다.

**hit 값**(UE `DispatchPhysicsCollisionHit`): `location`·`impactPoint` = 첫 접촉점, `normal`·`impactNormal` = 상대에서 나를 향하는 법선(나를 밀어내는 방향 — 내가 body 2면 Jolt 법선, body 1이면 반대. UE Chaos 법선도 상대에서 자기 쪽이고 swap 때 뒤집는다 — `:1094–1097`), `penetrationDepth` = max(0, 깊이 cm)(UE는 Chaos phi를 그대로 넣어 겹치면 음수다 — Rocks는 질의 hit와 같은 양수 깊이로 둔다), `blockingHit` 참, `component`·`actor` = 상대, 나머지(`time` 1 등)는 기본값. `normalImpulse`는 내가 받은 충격량(kg·cm/s, 축 A — 6.4절. UE는 swap 쪽에도 0번 입자 기준 값을 넘긴다 — Rocks는 받는 쪽으로 통일).

**발행(PhysicsResults — 3.1절).** `PhysicsWorldSubsystem::ApplyResults`가 결과 반영(3.4절) 뒤 `TakeContactHits()`로 프레임 목록을 꺼내(다음 꺼냄 전까지 유효한 Span — 발행 목록과 쌓는 목록의 버퍼를 바꿔 쥐어 용량을 다시 쓴다) 순서대로 `PrimitiveComponent::DispatchPhysicsHit(world, hit)`를 부른다. 자기 컴포넌트·Actor가 사라졌거나 파괴 대기면 건너뛴다(UE `Info0.Actor.Get()`·`IsActorValidToNotify`). `OnHit(myComponent, otherActor, otherComponent, selfMoved = 참, normalImpulse, hit)`다(UE는 물리 hit에 `bSelfMoved` 참을 넘긴다). 상대는 handle 그대로다(사라졌을 수 있다 — UE 약한 참조). 컴포넌트·Actor 이벤트(`OnComponentHit`·`OnActorHit`)는 PH4e가 같은 자리에 더한다.

**표지 바꾸기.** `PrimitiveComponent::SetNotifyRigidBodyCollision(bool)`(UE 같은 이름 — `BodyInstance` 표지 변경)이 `bodySettings.notifyRigidBodyCollision`을 바꾸고 body가 있으면 장면 `SetBodyNotifyContacts(handle, bool)`(새 접점 — step 밖)로 칸 표지를 바꾼다. 다음 step부터 적용된다.

**CCD.** `continuousCollision`이 참인 body는 Jolt `EMotionQuality::LinearCast`로 만든다(6.5절 표). 실행 중 바꾸는 setter(UE `SetUseCCD`)는 PH5다.

**설정.** `PhysicsSettings::minDeltaVelocityForHitEvents`(cm/s, 기본 0 — 6.8절 표). 음수·비유한이면 `Validate`가 `InvalidArgument`다.

**한계.**
- 같은 스텝에 discrete와 CCD가 섞인 쌍은 CCD 몫이 충격량에서 빠진다(6.4절 — 어댑터 경계 10.6절).
- Jolt는 하위 형상 쌍(manifold)마다 콜백하고 같은 평면 manifold를 합치므로(`ContactListener.h:31`) 접촉점 수는 Chaos와 다를 수 있다.
- 마찰 충격량(UE `TotalFrictionImpulse`)·물리 재질·뼈 이름은 hit에 없다(재질은 PH5·뼈는 skeletal 물리 때).
- listener 용량이 넘친 step은 접촉 일부를 잃고 어느 것을 잃는지는 worker 배정에 달린다(비결정적 — Warning이 남고 다음 step은 늘어난 용량이다). 하한(알림 body당 4개)보다 접촉이 갑자기 많아지는 첫 step에만 생긴다.
- Kinematic이 알림 body를 미는 경우의 충격량 방향은 따로 시험하지 않았다(같은 부호 규칙 — Dynamic 쌍과 같은 경로).

**PH4c 리뷰 반영(2026-11-27).** 치명 0, 중요 3, 경미 7.
- probe hit가 speculative 거리(2 cm) 안의 떨어진 접촉에서도 났다 → 깊이 음수면 버린다(수집 3).
- 문턱의 "step 전 속도"가 콜백 값(중력 적분 뒤)이라 쉬는 접촉의 변화가 g·dt였다 → `Update` 전 게임 thread에서 찍는다. 문턱이 probe hit를 모두 지우던 것을 고쳤다. listener 용량 하한에 알림 body 수를 더하고 넘침 한계를 적었다.
- 꺼낸 목록이 매 프레임 다시 할당되던 것(버퍼 바꿔 쥐기), 쓰지 않는 접촉점 수, 모든 알림 접촉의 추정 계산(CCD body만), 쌍마다 찍히던 할당 실패 Warning(step마다 한 번), 정렬 키 끝값(법선·추정 충격량), 6.4절 옛 문장, 시험 상수(`math::Pi`·가짜 handle 상수)를 고쳤다.
- 시험을 보강했다: probe hit 깊이 > 0, 문턱과 probe, 용량 넘침 뒤 다음 step, 여러 쌍의 칸 번호 순 발행, CCD 첫 hit 스텝의 hit 수 1.

**시험**(`PhysicsContactHitTests.cpp`):
- 표지 켠 구가 바닥에 떨어지면 닿는 스텝부터 hit가 나고 법선은 +Z(바닥에서 구 쪽), 충격량은 +Z이며 쉬는 동안 스텝마다 대략 m·g·dt다. 바닥 쪽 표지만 켜면 바닥 Actor가 받고 법선은 −Z다. 둘 다 켜면 둘 다 받는다(작은 칸 먼저).
- 표지가 없으면 hit가 없다. 실행 중 `SetNotifyRigidBodyCollision`으로 켜고 끈다.
- 한 프레임에 고정 스텝이 둘이면 hit도 둘이다.
- probe(`ProbeOnly`) 구는 바닥을 지나가며 충격량 0 hit를 받는다. probe 표지 없는 응답 Overlap 쌍은 접촉이 없다.
- 문턱이 크면 쉬는 접촉 hit가 사라지고 처음 부딪는 hit는 남는다.
- CCD: 얇은 벽으로 빠르게 가는 구는 `continuousCollision`이면 막히며 hit(추정 충격량, 0 아님)를 받고, 아니면 지나간다.
- hook에서 상대 Actor를 파괴해도 남은 hit 발행이 안전하다(파괴 대기 Actor는 받지 않는다).
- 같은 스텝의 여러 쌍은 칸 번호 순이다. 문턱이 있어도 probe hit는 남는다. listener 용량이 넘친 step 뒤 다음 step은 모두 받는다(`PhysicsContactSceneTests.cpp`).

### 7.1.13 PH4d 구현 세부(2026-11-27)

침투 해소 도우미다(5.2절). UE `UPrimitiveComponent::ComputePenetration`(`PrimitiveComponent.cpp:3984–3992` → `FBodyInstance::OverlapTest` `BodyInstance.cpp:4331`), `GetCollisionShape`(`PrimitiveComponent.cpp:3243`, `SphereComponent.cpp:201`·`BoxComponent.cpp:251`·`CapsuleComponent.cpp:240`), `UMovementComponent::SafeMoveUpdatedComponent`·`GetPenetrationAdjustment`·`ResolvePenetrationImpl`·`OverlapTest`(`MovementComponent.cpp:398–699`)를 따른다.

**자리.** UE처럼 `ComputePenetration`·`GetCollisionShape`는 `PrimitiveComponent`, `SafeMoveUpdatedComponent`·`GetPenetrationAdjustment`·`ResolvePenetration`은 `MovementComponent`(D4 — 엔진 런타임 26.3절)다. 5.2절의 "D4 이동 컴포넌트는 sweep 없이"는 그대로다: 이 단계는 도우미와 sweep 이동 overload만 더하고, 파생(투사체·경로 이동)을 sweep으로 바꾸는 일은 CM12다.

**`ComputePenetration(MtdResult& out, shape, position, rotation) → bool`.** 주어진 형상을 rotation으로 position에 놓았을 때 이 컴포넌트 body와 겹치면 참이고, `out`에 형상을 밀어낼 단위 방향(`direction`)과 거리(`distance`, cm)를 쓴다(UE `FMTDResult`). body·물리 장면이 없거나 겹치지 않으면 거짓이다. 장면 접점 `PhysicsScene::ComputePenetration(body, shape, position, rotation, MtdResult&)`(새 — step 밖)는 body의 Jolt `TransformedShape::CollideShape`(분리 거리 0 — 닿기만 해도 겹침, 깊이 0)로 가장 깊은 결과를 고른다. Jolt `mPenetrationAxis`는 body 쪽(shape 2)을 밀어낼 방향이라 질의 형상 방향은 그 반대다. 축 길이가 0이거나 비유한이면(중심이 겹침) 방향은 +Z, 거리는 깊이다(sweep 시작 겹침 hit의 막음 법선과 같다 — 7.1.8절). 형상 값이 비유한이거나 Line·Cylinder(CY2 전)·0 회전이면 거짓이다. `MtdResult`는 `World/Physics/MtdResult.h`다.

**`GetCollisionShape(inflation) → CollisionShape`.** 기본은 bounds 상자(`Bounds().boxExtent` — UE `Bounds.BoxExtent`)이고, 구·상자·캡슐이 자기 크기(scaled — `ScaledSphereRadius`·`ScaledBoxExtent`·`ScaledCapsuleRadius`·`ScaledCapsuleHalfHeight`)로 덮어쓴다. 부풀리기는 `CollisionShape::Inflate(inflation)`(새 — UE `FCollisionShape::Inflate`: 0이면 그대로, 상자 반폭·구 반경·캡슐 반경·반 높이에 더하고 0 아래로 내리지 않는다, Line·Cylinder는 그대로).

**`MovementComponent::MoveUpdatedComponent(delta, rotation, sweep, HitResult* outHit, TeleportMode) → Result<bool>`**(새 overload — UE `MoveUpdatedComponentImpl`): 평면 제약한 delta로 갱신 대상이 `PrimitiveComponent`면 `MoveComponent(…, sweep, outHit, MoveOptions{teleport})`, 아니면 sweep 없이 옮긴다(UE `USceneComponent::MoveComponentImpl` — outHit는 trace 끝점만 채운 빈 hit). 기존 두 인자 판(sweep 없음)은 그대로다. 내부 판은 `MoveOptions`를 받아 아래 표지를 넘긴다(UE는 컴포넌트 멤버 `MoveComponentFlags`를 범위로 바꾼다 — Rocks는 인자라 hook에 새지 않는다).

**`GetPenetrationAdjustment(hit) → Vec3d`**(가상): 시작 겹침이 아니면 0. 아니면 `ConstrainDirectionToPlane(hit.normal × ((깊이 > 0 ? 깊이 : DefaultPenetrationDepth 0.125) + |PenetrationPullbackDistance 0.125|))`다.

**`ResolvePenetration(adjustment, hit, rotation) → Result<bool>`**(가상 — UE `ResolvePenetrationImpl`): 평면 제약한 adjustment가 0이 아니고 갱신 대상이 Primitive이며 소유 Actor가 있을 때만 시도한다(그 밖은 거짓).
1. `hit.traceStart + adjustment`에 `GetCollisionShape(PenetrationOverlapCheckInflation 0.1)`을 rotation으로 놓고 막는 겹침을 본다(UE `OverlapTest` — 채널은 대상 object type, 응답은 대상 실효 응답 묶음, 소유 Actor와 이동 무시 목록 제외 — `InitCollisionParams` → `InitSweepCollisionParams`).
2. 비었으면 sweep 없이 `TeleportPhysics`로 adjustment만큼 옮기고 참.
3. 막혔으면 `neverIgnoreBlockingOverlaps` 없이(빠져나가는 시작 겹침은 무시 — 5.1절 5) adjustment를 sweep한다. 못 움직였고 그 hit가 시작 겹침이면 두 번째 조정량을 더한 방향(두 조정량이 다르고 합이 0이 아닐 때)으로 다시 sweep한다.
4. 아직이면 adjustment + 원래 이동(`hit.traceEnd − hit.traceStart`, 평면 제약)으로 sweep하고, 그것도 안 되고 원래 이동이 adjustment와 같은 쪽(내적 > 0)이면 원래 이동만 sweep한다. 반환 값은 마지막으로 움직였는지다.

**`SafeMoveUpdatedComponent(delta, rotation, sweep, HitResult& outHit, TeleportMode) → Result<bool>`**: `neverIgnoreBlockingOverlaps`·`disableBlockingOverlapDispatch`로 이동한다(UE CVar `p.MoveIgnoreFirstBlockingOverlap` 기본 0 — 먼저 밀어낸다). outHit가 시작 겹침이면 `ResolvePenetration(GetPenetrationAdjustment(outHit), outHit, rotation)`가 참일 때 원래 이동을 다시 한다(같은 표지가 아닌 기본 표지 — UE도 재시도는 범위 밖이다). 갱신 대상이 없으면 outHit를 `time` 1로 비우고 거짓이다.

**상수.** `PenetrationPullbackDistance` 0.125 cm·`PenetrationOverlapCheckInflation` 0.1 cm(UE CVar `p.PenetrationPullbackDistance`·`p.PenetrationOverlapCheckInflation` 기본값)·`DefaultPenetrationDepth` 0.125 cm(UE 리터럴)는 이름 있는 상수다. CVar 연결은 쓰는 곳(CM12 캐릭터 이동)이 생길 때 한다.

**한계.**
- MTD는 body의 모든 하위 형상을 본다(UE는 `bTraceComplex` 거짓이면 simple 형상만 — `PhysInterface_Chaos.cpp:1114–1170`). 지금 body는 상자·구·캡슐뿐이라 같고, 메시 body(PH6 complex-as-simple)가 들어올 때 하위 형상 표지로 거른다.
- Primitive가 아닌 갱신 대상의 sweep 없는 이동은 `TeleportMode`를 자식 body에 넘기지 않는다(teleport 범위가 PrimitiveComponent 안에만 있다 — UE `InternalSetWorldLocationAndRotation`은 넘긴다). `ResolvePenetration`은 이 경로를 쓰지 않는다.
- 음수 scale 상자의 `GetCollisionShape`는 부호 있는 반폭을 부풀려 0으로 잘린다(UE `GetScaledBoxExtent` + `Inflate`와 같다). 비균일 scale 캡슐의 질의 반경(min)과 body 반경(max)도 UE처럼 다르다.
- UE의 `SafeMoveUpdatedComponent`·`MoveUpdatedComponentImpl`·`OverlapTest`처럼 Rocks 판도 가상이다. `OverlapTest`는 무시 목록 버퍼를 재사용해 const가 아니다.
- `CollisionShapeKind`가 Collision과 Mesh(`SimpleCollision.h` — 진단 형상 종류)에 같은 이름으로 따로 있어 한 번역 단위에서 만나면 컴파일 오류였다. Mesh 쪽이 `SimpleCollisionShapeKind`로 개명해 해결했다(rocks-cf 1ee15bfb6). `PrimitiveComponent.h`의 `CollisionShape` 앞 선언은 헤더를 가볍게 두려고 그대로 둔다.
- `ResolvePenetration`의 "두 MTD 합산"·"조정량 + 원래 이동"·"원래 이동만" 분기는 시험 배치(쐐기 모서리)가 없어 시험하지 않았다(UE와 같은 순서 — 코드 대조만).

**PH4d 리뷰 반영(2026-11-27).** 치명 0, 중요 1, 경미 9. 밀어낸 자리 겹침 시험이 질의를 마치지 못하면(대상을 빌리지 못함·충돌 해석 실패) "비었다"로 보고 teleport하던 것을 막힘으로 고쳤다(sweep으로 간다). 축 없는 MTD 방향(+Z), 가상 함수, 문서 식 괄호, 낡은 주석, include 순서를 고치고 한계를 적었다. 시험을 보강했다: 회전한 벽·회전한 캡슐 질의의 MTD, 장면 접점 오류 경로(옛 handle·0 회전·비유한 위치·Cylinder), 막힌 해소 뒤 재시도의 반환 값·hit, 이동 무시 Actor면 밀어낸 자리 겹침에서 빠짐, 평면 제약으로 조정량이 0이면 해소 안 함, 갱신 대상 없는 SafeMove.

**시험**(`PhysicsPenetrationTests.cpp`):
- `ComputePenetration`: 상자 body와 겹친 구의 방향(+X)·거리(겹친 깊이), 겹치지 않으면·body 없으면 거짓, 거의 닿으면 거리 ≈ 0.
- `GetCollisionShape`: 구·상자·캡슐이 scale·부풀리기를 반영하고 0 아래로 줄지 않는다. `CollisionShape::Inflate` 경계.
- `GetPenetrationAdjustment`: 시작 겹침 아님 → 0, 깊이 0 → 기본 깊이, 평면 제약.
- `SafeMoveUpdatedComponent`: 벽에 5 cm 박힌 상자를 옆으로 옮기면 밀려나(teleport) 원래 이동을 마친다. 밀려날 자리가 다른 벽에 막히면 sweep으로 나온다. 박혀 있지 않으면 보통 sweep(벽에 막힘). 갱신 대상이 Primitive가 아니면 sweep 없이 옮긴다.

### 7.1.14 PH4e 구현 세부(2026-11-27)

hit·overlap의 컴포넌트·Actor 이벤트 필드다(5.5절). 리플렉션 이벤트(`Event<Args...>` — EventReflection ED1, EV2b)를 클래스 필드로 두고 PH4a–PH4c의 가상 hook 자리에서 같이 발행한다. UE `UPrimitiveComponent::OnComponentHit`·`OnComponentBeginOverlap`·`OnComponentEndOverlap`, `AActor::OnActorHit`·`OnActorBeginOverlap`·`OnActorEndOverlap`(`PrimitiveComponent.h`·`Actor.h` 동적 multicast delegate)를 따른다.

**이벤트 필드**(이름은 Rocks식 — UE 이름은 1.3절 대응표에 더한다):

| 클래스 | 필드 | 인자(`Params`) | UE |
|---|---|---|---|
| `PrimitiveComponent` | `componentHit` | `hitComponent, otherActor, otherComponent, normalImpulse, hit` — `WeakObjectRef<PrimitiveComponent>, WeakObjectRef<Actor>, WeakObjectRef<PrimitiveComponent>, Vec3d, const HitResult&` | `OnComponentHit` |
| | `componentOverlapBegan` | `overlappedComponent, otherActor, otherComponent, fromSweep, sweepResult` — 위 셋 + `bool, const HitResult&` | `OnComponentBeginOverlap`(`OtherBodyIndex`는 body가 하나라 뺀다) |
| | `componentOverlapEnded` | `overlappedComponent, otherActor, otherComponent` | `OnComponentEndOverlap` |
| `Actor` | `actorHit` | `selfActor, otherActor, normalImpulse, hit` — `WeakObjectRef<Actor>×2, Vec3d, const HitResult&` | `OnActorHit` |
| | `actorOverlapBegan`·`actorOverlapEnded` | `overlappedActor, otherActor` — `WeakObjectRef<Actor>×2` | `OnActorBeginOverlap`·`OnActorEndOverlap` |

- Actor·컴포넌트 인자는 `WeakObjectRef<T>`다: Sparks는 `ObjectHandle`을 노출하지 못한다(SparksBinding B14 — RMC2602). UE도 타입 있는 포인터를 넘긴다. 나중에 바꾸면 frame digest가 달라져 저장된 바인딩이 미해결(EventReflection ED24)이 되므로 지금 정한다.

- 저장한다(Transient 아님): 에디터·Sparks가 건 리플렉션 바인딩은 그래프에 남는다(EV3a1 — native 구독은 저장하지 않는다). UE도 블루프린트 바인딩은 클래스에 남는다.
- 생성자가 이벤트마다 allocator를 넘긴다(EV2b).

**HitResult 리플렉션.** 이벤트 인자는 리플렉션 값 형식이어야 한다(바인딩 frame — EventReflection ED5). `HitResult`를 `ROCKS_STRUCT`로 바꾸고 값 칸(`time`·`distance`·`location`·`impactPoint`·`normal`·`impactNormal`·`traceStart`·`traceEnd`·`penetrationDepth`·`faceIndex`·`blockingHit`·`startPenetrating`)을 `ROCKS_PROPERTY`로 둔다. `component`·`actor`는 `ObjectHandle`이라 리플렉션 struct 필드가 될 수 없어(함수·이벤트 인자 전용) `ROCKS_NATIVE`다. 스크립트는 상대를 이벤트 인자(`otherActor`·`otherComponent` — 약한 참조)로 받는다(UE delegate도 따로 넘긴다). 버린 대안: `WeakObjectRef<PrimitiveComponent>` 필드 — 질의·이동·시험 전체의 handle 비교가 바뀐다. 발행하는 파일은 `World/Physics/HitResult.generated.h`를 include한다.

**발행 순서**(가상 hook과 같은 자리 — UE 순서):
- 막는 sweep hit·시뮬레이션 hit(UE `InternalDispatchBlockingHit` `Actor.cpp:3525–3556`, `DispatchPhysicsCollisionHit` `:5690–5736`): 한쪽마다 Actor `OnHit` → `actorHit` → 그 쪽 컴포넌트 `componentHit`. 단계마다 다시 본다: Actor가 알림을 받을 수 있으면 hook, 그 뒤에도 받을 수 있으면 `actorHit`, 컴포넌트가 살아 있으면 `componentHit`다(hook이 자기를 파괴하면 이어지는 이벤트는 없다 — UE `IsActorValidToNotify` 재검사). sweep은 자기 쪽 → 상대 쪽(뒤집은 hit)이다. sweep hit의 `normalImpulse`는 0이다.
- overlap 시작(UE `BeginComponentOverlap` `PrimitiveComponent.cpp:4130–4166`): World가 Playing일 때 자기 `componentOverlapBegan` → 상대 `componentOverlapBegan`(sweep이면 뒤집은 hit) → 새 Actor 쌍이면 자기·상대 Actor `OnBeginOverlap` → 자기·상대 `actorOverlapBegan`.
- overlap 끝(UE `EndComponentOverlap` `:4174–4231`): World가 Playing이고 상대 Actor가 있으면 자기 `componentOverlapEnded` → 상대 `componentOverlapEnded` → 마지막 Actor 쌍 겹침이었으면 자기 Actor `OnEndOverlap`·`actorOverlapEnded` → 상대 Actor `OnEndOverlap`·`actorOverlapEnded`(UE는 끝만 Actor마다 hook과 이벤트를 붙여 부른다). PH4b 구현은 Actor 쌍이 아니면 일찍 돌아갔다 — 컴포넌트 이벤트를 그 앞에 둔다.
- 파괴 대기(`ComponentState::PendingDestroy`·`ActorState::PendingDestroy`)는 UE의 `MarkAsGarbage` 뒤에 해당해 알림을 받지 않는다(UE `IsPrimCompValidAndAlive`·`IsActorValidToNotify`는 `IsValid`). 예외: 자기 `OnDestroy`가 겹침을 모두 끝내는 동안은 자기와 자기 Actor도 끝 hook·이벤트를 받는다. UE는 컴포넌트 `OnComponentDestroyed`(`ActorComponent.cpp:2201`)와 `UWorld::DestroyActor`의 `ClearComponentOverlaps`(`LevelActor.cpp:981`)가 `MarkAsGarbage`(`:2204`·`:1055`)보다 먼저라 파괴되는 쪽도 끝 알림을 받는다. 이때는 Registry 파괴 요청 전이라 `Event`가 발행자를 붙잡는다. `Event`는 파괴 대기를 스스로 거르지 않으므로 상태는 발행하는 쪽이 본다. PH4b의 "파괴 대기 Actor에는 끝 알림 없음"도 이 규칙으로 바꿨다. 알림 시작 때 본 상태 기준이다(끝의 Actor hook과 이벤트 사이에는 UE처럼 다시 보지 않는다).

**sweep 결과(`fromSweep`·`sweepResult`).** UE `FOverlapInfo`처럼 대기 겹침이 sweep hit를 함께 쥔다: `MoveComponent`의 대기 목록을 (컴포넌트, hit) 쌍으로 바꾸고 `BeginComponentOverlap(other, fromSweep, sweepResult)`다. 끝 위치 갱신(`RefreshOverlaps`)이 찾은 겹침은 `fromSweep` 거짓, `sweepResult`는 상대 컴포넌트·Actor만 채운 hit다(UE `FOverlapInfo(Component)`). 상대 쪽에는 sweep이면 뒤집은 hit(법선 부호만 — UE `GetReversedHit`), 아니면 같은 hit다(UE가 `OtherOverlap.OverlapInfo`를 그대로 넘긴다 — 그 hit의 컴포넌트는 받는 쪽 자신이다).

**메타 컴파일러 수정.** 생성 소스는 클래스 base·이벤트 인자 대상의 생성 헤더만 include했다. 물려받은 이벤트의 연산표(`MakeEventOps<&Owner::field>`)는 선언한 클래스의 `NativeTypeKey`(`ResolveForWrite<Owner>`)를 쓰므로, 이벤트를 선언한 조상이 바로 위 base가 아니면(`PointLight` ← `Light` ← `Actor`) 컴파일이 실패했다(EV2b 시험은 같은 헤더의 파생만 본다). 이벤트 필드의 선언 클래스 헤더도 include한다(`CppEmitter` — 이벤트 리플렉션 담당에 알린다).

**미룬 것.** 깨움·잠듦(`OnComponentWake`·`OnComponentSleep` — `generateWakeEvents`)과 `OnComponentPhysicsStateChanged`는 body 깨움·잠듦 감지와 함께 PH5에서 한다(3.1절 PhysicsResults (3)).

**시험**(`PhysicsEventFieldTests.cpp`):
- sweep 막는 hit: 순서(자기 `OnHit` → `actorHit` → `componentHit` → 상대 셋), 인자, 상대 쪽 뒤집은 법선, 충격량 0.
- 시뮬레이션 hit: `componentHit`·`actorHit` 인자와 충격량.
- overlap 시작·끝: 컴포넌트 이벤트(같은 Actor 컴포넌트끼리도 난다 — Actor 이벤트는 없다), sweep이면 `fromSweep` 참·상대는 뒤집은 hit, 끝 위치 갱신은 거짓, Actor 이벤트는 쌍마다 한 번, 끝의 Actor hook·이벤트 순서.
- World가 Playing이 아니면(EndPlay) overlap 이벤트가 없다. 컴포넌트·Actor 파괴는 파괴되는 쪽과 상대가 모두 끝 이벤트를 받는다. OnHit hook이 자기 Actor를 파괴하면 이어지는 hit 이벤트가 없다. 같은 Actor의 두 컴포넌트가 한 trigger와 겹치면 Actor 이벤트는 한 번이다.
- `HitResult` 리플렉션: 생성 schema가 값 칸만 싣는다. 등록된 `componentHit` schema의 인자 종류(약한 참조 셋·struct).
- 메타 컴파일러 회귀: `ReflectionTestFixtures`의 세 헤더 사슬(`EventPublisherChild` ← `EventRelay` ← `EventRelayLeaf`)이 컴파일되고 상속 이벤트 연산을 싣는다(`EventFieldGeneratedTests`).

**한계.** 리플렉션 바인딩(`TryBind` — 리플렉션 함수 대상)으로 이 이벤트를 받는 시험은 없다(대상 함수를 가진 엔진 클래스가 없다 — Sparks 노출 EV4·EI 때 Sparks 함수로 본다). 바인딩 frame의 `HitResult` 복사는 일반 `ValueCopy`(plain copy)다.

**PH4e 리뷰 반영(2026-11-27).** 치명 0, 중요 4, 경미 6. hook이 자기를 파괴한 뒤 `actorHit`가 나던 것(UE 재검사), 파괴되는 쪽의 끝 알림을 UE와 반대로 막던 것(PH4b Actor hook 포함 — UE를 따른다), 이벤트 인자 `ObjectHandle` → `WeakObjectRef`(Sparks 노출), 시험 보강(위 목록, sweep 법선 0이 아닌지)을 고쳤다. 메타 컴파일러 회귀 fixture, 주석 예(`PointLight ← Light ← Actor`), `NotifyActorOverlap`의 bool 세 인자를 hook·이벤트 함수와 `OverlapEdge`로 나눔, 빈 줄을 고쳤다.
