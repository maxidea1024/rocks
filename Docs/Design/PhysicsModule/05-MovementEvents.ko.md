# 물리 모듈 — 5. sweep 이동과 이벤트

[목차](README.ko.md)

이 장의 코드는 World 모듈(`PrimitiveComponent`·`SceneComponent`)에 들어간다. 물리는 4장 질의만 제공한다. UE에서도 이동·overlap 장부는 컴포넌트 쪽 코드다(`E/Private/Components/PrimitiveComponent.cpp`).

## 5.1 `MoveComponent`

`PrimitiveComponent::MoveComponent(delta, newRotation, sweep, HitResult* outHit, MoveFlags, TeleportMode)` — UE `MoveComponentImpl`(`PrimitiveComponent.cpp:3250`)과 같은 순서다.

1. **최소 이동.** sweep이고 이동 제곱 거리가 (4·`KindaSmallNumber`)² 이하이며 회전이 같으면 아무것도 하지 않는다(`:3283–3296`).
2. **sweep 없음.** transform을 바로 바꾸고 overlap을 갱신한다(`:3311–3317`).
3. **sweep.** `ComponentTraceAll`을 시작 회전으로 고정해 부른다(`:3354`).
   - 채널 = 자기 object type, 응답 = 자기 실효 응답 묶음(`WorldCollision.cpp:476`, `PrimitiveComponent.cpp:3202`).
   - 무시: `moveIgnoreActors`·`moveIgnoreComponents`, 자기 Actor(`WorldCollision.cpp:535–557`).
   - 자기 `generateOverlapEvents`가 거짓이면 겹침을 무시한다(`ignoreTouches` — `:3351`).
   - 형상: 컴포넌트의 단순(convex) 형상 각각(complex 제외).
4. **pullback.** 모든 hit의 `time`에서 clamp(0.1, 0.1/D, 1/D) + 0.001을 뺀다(D = 이동 거리 cm — `:3095–3099`·`:3361`). D가 10 cm를 넘으면 거리로 1 cm + 0.001·D다(개정 15 정정 — 전에는 "약 0.1 cm"라 적었다). 이름 있는 상수(`PullbackTime` 0.1, `PullbackTimeSlack` 0.001)로 둔다.
5. **막는 hit 고르기.** 시작 겹침 중 `impactNormal · delta`가 가장 작은 것, 없으면 첫 시작 겹침 아닌 막음(`:3380–3395`). 빠져나가는 방향(내적 > `InitialOverlapTolerance` 0)의 시작 겹침은 무시한다(`MoveFlags::NeverIgnoreBlockingOverlaps`면 무시하지 않음 — `:3135–3162`).
6. **도착.** 시작 + time·delta. 이동이 너무 작으면 시작점에 머문다(`:3434–3456`).
7. **겹침 수집.** 상대도 `generateOverlapEvents`이고 막는 hit의 time 이전인 겹침을 중복 없이 대기 목록에 둔다(`:3399–3420`).
8. **반영.** transform 적용(부착 전파·body 이동 — 3.3절) → `UpdateOverlaps(대기 목록)`(5.3절) → 막는 hit가 있으면 hit 발행(5.4절, `DispatchBlockingHit` — `:3588–3603`).

- `MoveOptions`(UE `EMoveComponentFlags` — bool 칸의 값 struct, 7.1.10절): `skipPhysicsMove`(3.4절 결과 반영), `neverIgnoreBlockingOverlaps`, `disableBlockingOverlapDispatch`(시작 겹침 hit 발행 안 함 — `:3539`), `teleport`. UE의 다른 표지는 쓰는 곳이 생길 때 더한다.
- 묶은 이동(UE `FScopedMovementUpdate` — 여러 이동의 overlap 갱신을 끝에 한 번)은 캐릭터 이동(CM13) 때 더한다.

## 5.2 침투 해소

이동 컴포넌트(D4·CM12)가 쓰는 도우미다(UE `UMovementComponent::SafeMoveUpdatedComponent`·`ResolvePenetration` — `E/Private/Components/MovementComponent.cpp:558–699`).

- `PrimitiveComponent::ComputePenetration(MtdResult&, shape, position, rotation)`: 이 컴포넌트와 주어진 형상의 최소 이동 방향·거리(UE `PrimitiveComponent.h:3177`). Jolt `CollideShape`의 `mPenetrationAxis`·`mPenetrationDepth`(J11)로 구한다.
- `SafeMove`(`MovementComponent::SafeMoveUpdatedComponent` — 7.1.13절): sweep 이동이 시작 겹침이면 `ResolvePenetration` 뒤 원래 이동을 다시 시도한다. 조정량 = `normal × ((depth 또는 0.125) + PenetrationPullbackDistance 0.125 cm)`. 0.1 cm 부풀린 겹침 시험이 비면 teleport, 막히면 sweep, 그다음 MTD 합산, 그다음 이동량을 더한 시도 순이다(UE와 같은 순서·같은 상수 — 이름 있는 상수).
- D4의 이동 컴포넌트는 지금 sweep 없이 동작한다(엔진 런타임 26장). CM12 때 이 도우미로 바꾼다.

## 5.3 overlap

UE `UpdateOverlapsImpl`(`PrimitiveComponent.cpp:4464–4653`)과 같다. overlap은 **질의 기반**이다(시뮬레이션 접촉이 아니다 — 시뮬레이션 body도 결과 반영 이동 때 같은 경로로 갱신된다, 3.4절).

- 장부: 컴포넌트마다 `overlappingComponents`(대칭으로 양쪽에 기록 — `:4126–4127`). NATIVE(영속 아님).
- 시작 조건: 양쪽 `generateOverlapEvents` && 양쪽 응답의 최소가 Overlap(`:224–231`). 이미 있으면 건너뛴다(`:4112`).
- 갱신(`UpdateOverlaps(대기 목록, 끝 transform)`):
  1. 소유 Actor가 BeginPlay 전이면 하지 않는다(Actor 단위 — `:4471–4475`).
  2. 대기 목록의 시작을 먼저 발행한다(`:4490–4497`).
  3. 끝 위치에서 `ComponentOverlapAll(ignoreBlocks)`로 새 목록을 얻는다. transform이 같으면 직전 결과를 다시 쓴다(`:4507–4553`).
  4. 옛 목록 − 새 목록은 끝, 새 목록 − 옛 목록은 시작(끝 먼저 — `:4559–4626`).
  5. 루트면 같은 Actor 자식은 빼고, 자식 컴포넌트는 재귀로 갱신한다(`:4486`·`:4643–4653`).
  6. 자기 `generateOverlapEvents`가 꺼져 있으면 기존 겹침을 전부 끝낸다(`:4632–4639`).
- 다시 계산하는 때: 이동(5.1절), BeginPlay(UE `UpdateInitialOverlaps` — `E/Private/Actor.cpp:4851–4887`), 충돌 설정 변경(`PrimitiveComponentPhysics.cpp:1434–1446`), Actor 충돌 켜고 끄기(`Actor.cpp:5322–5338`), 파괴(전부 끝 — `PrimitiveComponent.cpp:2026–2044`).
- 발행 순서(`:4135–4166`): 자기 `OnComponentBeginOverlap` → 상대 `OnComponentBeginOverlap`(sweep이면 뒤집은 hit) → 새 Actor 쌍이면 양쪽 `NotifyActorBeginOverlap` → 양쪽 `OnActorBeginOverlap`. 끝은 대칭 제거 뒤 컴포넌트 → 마지막 겹침이었으면 Actor(`:4174–4231`).
- 발행 중 재진입(이벤트 안에서 이동·파괴)은 UE처럼 허용하되, 장부 순회는 복사본으로 하고 파괴된 상대는 약한 참조로 건너뛴다.

## 5.4 hit

| 출처 | 동작 | UE |
|---|---|---|
| sweep 이동의 막는 hit | 자기 Actor(`selfMoved` 참)와 상대 Actor(뒤집은 hit)에 발행. 순서: `NotifyHit` → `OnActorHit` → `OnComponentHit`, `normalImpulse` 0 | `PrimitiveComponent.cpp:3588–3603`, `Actor.cpp:3525–3556` |
| 시뮬레이션 접촉 | `notifyRigidBodyCollision`이 참인 body의 접촉 쌍마다 `ContactNotice` 하나(첫 접촉점 + 접촉점들 + 누적 충격량). step 안에서는 값만 복사하고 PhysicsResults에서 스텝 순서대로 발행. 상대도 켰으면 상대 쪽 항목이 따로 생긴다 | `PhysScene_Chaos.cpp:1043–1130`·`1195–1218`, `Actor.cpp:5690–5736` |

- 시뮬레이션 hit의 `HitResult`는 첫 접촉으로 만들고 `normalImpulse`는 누적 충격량(N·s → kg·cm/s)이다. Jolt에서 solver 충격량을 읽는 방법은 6.4절.
- 한 스텝 안의 같은 쌍 접촉은 하나로 합친다(첫 접촉점, 충격량 합). 스텝이 다르면 따로 발행한다(UE key가 쌍과 solver 시간 — 7.1.12절).
- 발행은 Actor·컴포넌트 이벤트(구독형 통지 — 컴포넌트 도입 CR7)가 있어야 한다. CR7 전에는 가상 hook(`Actor::NotifyHit`·`NotifyActorBeginOverlap` 등)만 부른다(7.1절 단계 선행).

## 5.5 게임 코드 접점

| 이벤트 | 컴포넌트 | Actor |
|---|---|---|
| overlap 시작·끝 | `OnComponentBeginOverlap`·`OnComponentEndOverlap`(CR7) | `NotifyActorBeginOverlap`·`NotifyActorEndOverlap`(가상), `OnActorBeginOverlap`·`OnActorEndOverlap`(CR7) |
| hit | `OnComponentHit`(CR7) | `NotifyHit`(가상), `OnActorHit`(CR7) |
| 깨움·잠듦 | `OnComponentWake`·`OnComponentSleep`(CR7) | — |
| 물리 상태 바뀜 | `OnComponentPhysicsStateChanged`(CR7 — UE `PrimitiveComponent.cpp:971`) | — |

Sparks 바인딩(언어 엔진 통합)은 이 이벤트를 그대로 노출한다(LC 단계 — 별도 문서).
