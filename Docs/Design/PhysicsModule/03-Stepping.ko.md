# 물리 모듈 — 3. step과 결과 반영

[목차](README.ko.md)

## 3.1 프레임 안의 자리

엔진 런타임 8절의 단계 순서 `PrePhysics → Physics(고정 스텝 수만큼) → PostPhysics → LateUpdate → PreRender`를 쓴다. 물리 일은 두 엔진 자리에서 한다.

| 자리 | 언제 | 하는 일 | UE |
|---|---|---|---|
| **PhysicsSimulate**(새 — 7.4절 요청) | Physics 단계의 고정 스텝마다, 사용자 Fixed hook 뒤 | (1) Kinematic 목표 적용과 목표 없는 Kinematic 정지(3.3절) (2) Jolt `PhysicsSystem::Update(fixedStep, collisionSteps 1)` (3) 이번 스텝 접촉의 solver 충격량을 읽고(6.4절) 활성 body·접촉·잠듦 기록을 스텝 번호와 함께 버퍼에 쌓음(3.4절) | `FStartPhysicsTickFunction` → `StartFrame` → `AdvanceAndDispatch`(`E/Private/PhysicsEngine/PhysLevel.cpp:239–245`, `PC/Private/ChaosScene.cpp:363–386`) |
| **PhysicsResults**(있음 — 엔진 런타임 15.4절 표 첫 칸) | Physics 단계 끝, 프레임에 한 번 | (1) 이번 프레임에 움직인 Dynamic body의 transform을 컴포넌트에 반영(3.4절) (2) 쌓인 hit 이벤트를 결정적 순서로 발행(5.4절) (3) 잠듦·깨움 이벤트 발행(step 밖 깨움 포함) | `FEndPhysicsTickFunction` → `EndFrame`·`OnSyncBodies`·충돌 통지(`PhysLevel.cpp:257–279`, `ChaosScene.cpp:500–576`, `E/Private/PhysicsEngine/Experimental/PhysScene_Chaos.cpp:2452–2727`) |

- 고정 스텝이 0번인 프레임(누적이 한 스텝에 못 미침)은 PhysicsSimulate가 없다. PhysicsResults는 그래도 돈다 — 게임 lane API(`WakeRigidBody` 등)가 step 밖에서 낸 깨움·잠듦 이벤트가 남아 있을 수 있다.
- 고정 스텝 길이와 프레임당 상한은 `TimeStepPolicy`(엔진 런타임 5절 — 계획 16.1절 제안 60 Hz·최대 4)다. 물리가 따로 dt를 정하지 않는다.
- pause 중 단일 스텝(`RequestSingleStep`)은 고정 스텝 한 번이므로 PhysicsSimulate도 한 번이다.
- **step을 도는 World는 Game·Tryout뿐이다.** Editor·Preview World는 장면(질의·picking용)만 있고 PhysicsSimulate를 돌리지 않는다(UE 편집 World는 물리 tick group을 돌리지 않는다). 그 World의 body 이동은 언제나 teleport다(3.3절).

## 3.2 쓰기 규칙(스레드·시점)

- 장면의 소유자는 게임 lane이다. 모든 접점 호출(2.2절)은 게임 lane에서 한다.
- step은 PhysicsSimulate 자리 안에서 끝난다. Jolt는 step 동안 Task worker를 쓰지만(job 연결 — 6.7절) 게임 lane은 step이 끝날 때까지 그 자리에서 기다린다(UE 기본 동기 모드와 같다 — `EndPhysics`가 완료를 기다린다). 그래서 step 밖의 쓰기·질의는 Jolt 상태를 바로 읽고 쓴다. 명령 큐를 두지 않는다.
  - 계획 16.1–16.3절의 command 구조(`Enqueue(PhysicsCommand)`, "callback 중 body 추가·삭제는 다음 command batch로", "쓰기는 Physics lane의 command 단계")는 비동기 step(게임 Tick과 겹치는 step)을 들일 때 쓴다. 지금은 동기 step이라 큐가 할 일이 없다(버린 대안: 처음부터 큐 — 쓰기 직후 같은 프레임 질의가 바뀐 값을 보지 못해 UE 동작과 달라진다). 계획 개정은 7.4절 요청이다.
- step 안(Jolt callback·job)에서는 접점을 부르지 않는다. contact·activation listener는 값만 복사한다(계획 16.2절). debug build는 step 중 접점 호출을 Fatal로 잡는다.
- 질의는 step 밖 어디서나 한다(QueryReadScope — 계획 16.3절). body를 더하거나 뺀 뒤에도 Jolt broadphase는 맞는 결과를 준다(최적화 전에는 느릴 뿐). 큰 일괄 추가(레벨 적재) 뒤에는 Physics가 한 번 `OptimizeBroadPhase`한다.
- **Fixed hook 안의 transform.** 결과 반영은 프레임에 한 번이라, 한 프레임에 고정 스텝이 둘 이상이면 두 번째 이후 스텝의 Fixed hook에서 Dynamic 컴포넌트의 transform은 첫 스텝 전 값이다. Fixed hook에서 시뮬레이션 상태가 필요하면 `ReadKinematics`(body의 현재 위치·속도 — 2.2절)로 읽는다. 시뮬레이션하는 컴포넌트는 부모에서 떨어져 있으므로(2.3.1절) 부착 전파가 Dynamic body를 옛 자세로 되돌리는 경로는 없다. 그래도 Dynamic body에 대한 `SetBodyTransform`은 명시 호출(teleport 포함)만 받고, 부착 전파에서 온 것은 무시한다(PMD15).

## 3.3 Kinematic·Static 이동

UE `SetBodyTransform`(`E/Private/PhysicsEngine/BodyInstance.cpp:2767–2839`)을 따른다.

| body | `TeleportMode::None` | `TeleportPhysics`·`ResetPhysics` |
|---|---|---|
| Kinematic | **즉시** 새 위치로 옮기고(질의가 바로 본다 — UE도 GT 위치·가속 구조를 바로 바꾼다, `PC/Private/ChaosEngineInterface.cpp:2603–2611`) step 시작 자세를 따로 기록한다. PhysicsSimulate에서 기록한 시작 자세로 되돌린 뒤 `MoveKinematic(target, fixedStep)`(속도 = 위치 차이 / 스텝) | 즉시 옮기고(`SetPositionAndRotation`) 속도 0, 시작 자세 기록을 지운다(UE `ChaosEngineInterface.cpp:2550–2556`) |
| Dynamic | 즉시 옮기고 속도는 그대로(UE `SetGlobalPose` — `:2544–2560`) | 같다(UE 단일 body는 `ResetPhysics`도 `TeleportPhysics`와 같은 경로) |
| Static | 즉시 옮긴다(broadphase 갱신) | 같다 |

- **목표 없는 스텝의 Kinematic 정지.** Jolt `MoveKinematic`은 위치가 아니라 **속도**를 정하고, 그 속도는 다음 스텝에도 남는다(`Physics/Body/Body.h:221`). 그래서 PhysicsSimulate는 이번 스텝에 목표가 없는 Kinematic body의 선·각속도를 0으로 둔다(`SetLinearAndAngularVelocity(0)` — 현재 자세로 `MoveKinematic`하는 것과 같은 결과). 같은 프레임의 고정 스텝 여러 번에서 목표는 첫 스텝이 쓰고(도달), 나머지 스텝은 정지다. 이동이 부드러워야 하는 Kinematic(움직이는 발판)은 Fixed hook에서 스텝마다 옮긴다. UE는 프레임당 물리 step이 기본 한 번이라 이 문제가 없다(3.5절). `MoveKinematic` 대신 teleport로만 옮기면 밀린 Dynamic body가 이동 속도를 받지 못한다(UnrealJolt가 그렇다 — 어댑터 경계 11.5절).
- **Editor·Preview World**는 Kinematic 이동도 언제나 teleport다(UE `BodyInstance.cpp:2796–2802` — `bEditorWorld`).
- 컴포넌트 이동 경로: `SceneComponent` transform이 바뀌면(부착 전파 포함) `PrimitiveComponent::OnTransformUpdated` 가상 hook이 body가 있으면 `SetBodyTransform`을 부른다(UE `OnUpdateTransform` → `SendPhysicsTransform` — `E/Private/Components/PrimitiveComponent.cpp:1061–1083`). 결과 반영(3.4절)이 부른 이동은 `skipPhysics` 표지로 body에 되돌려 보내지 않는다(UE `MOVECOMP_SkipPhysicsMove`).
- teleport 표지는 `SceneComponent`의 이동 API 인자(`TeleportMode`)로 전한다. D2·D4의 이동 API에 인자를 더하는 일은 7.4절 요청이다.

## 3.4 결과 반영

- **대상 모으기.** PhysicsSimulate가 스텝마다 Jolt 활성 body(`GetActiveBodies`)와 그 스텝에 잠든 body(`BodyActivationListener::OnBodyDeactivated` — 값만 복사)를 handle 집합에 더한다. 프레임 끝 활성 목록만 보면, 앞 스텝에서 움직이다 그 프레임 안에 잠든 body가 빠져 컴포넌트가 옛 위치에 남기 때문이다.
- **순서.** body handle 순으로 정렬해 돈다(Jolt 활성 목록 순서는 결정적이지 않다 — 어댑터 경계 J10). 시뮬레이션하는 컴포넌트는 부모에서 떨어져 있으므로(2.3.1절) 부모·자식 순서 문제가 없다. weld 자식은 부모 이동의 부착 전파로 따라간다(자기 body가 없다).
- **쓰기.** 각 body의 Jolt 위치·회전을 Rocks 월드 transform으로 바꾸고(어댑터 경계 2·3·6절), 컴포넌트에 `MoveComponent(delta, rotation, sweep = false, teleport = None, skipPhysics)`로 쓴다(UE `PhysScene_Chaos.cpp:2483–2491`·`2703`). 그래서 자식 부착 전파·overlap 갱신(5.3절)이 같은 경로로 돈다.
- **재진입.** 반영 중 overlap 이벤트가 다른 컴포넌트를 파괴할 수 있다. 정렬한 목록은 돌 때마다 handle generation을 다시 확인하고 만료된 것은 건너뛴다.
- 보간: 렌더 보간(`StepPlan.alpha` — 엔진 런타임 5절)은 렌더 단계에서 따로 정한다. 물리 결과는 마지막 스텝의 값 그대로다(UE 기본 동기 모드와 같다).

## 3.5 UE와 다른 점

| 항목 | UE | Rocks | 이유 |
|---|---|---|---|
| step dt | 기본 가변(프레임 dt, 상한 1/30 s), substepping 끔(`E/Private/PhysicsEngine/PhysicsSettings.cpp:21–27`, `ChaosScene.cpp:338–357`) | 고정 스텝(엔진 시간 정책), 프레임당 0–상한 회 | 계획 16.1절·엔진 런타임 5절의 결정성·서버 tick. UE async 모드(`bTickPhysicsAsync`, 고정 1/30)와 가깝다 |
| Kinematic 목표 | 프레임당 한 번(한 step) | 프레임의 첫 고정 스텝, 나머지 스텝 정지 | 위와 같다 |
| 명령 전달 | GT 쓰기를 다음 `AdvanceAndDispatch`가 넘긴다 | step 밖 즉시 쓰기(PMD6) | 동기 step(3.2절) |
| 결과 반영 대상 | dirty proxy | 스텝별 활성·잠든 body 합(정렬) | Jolt 활성 목록, 결정적 순서 |
