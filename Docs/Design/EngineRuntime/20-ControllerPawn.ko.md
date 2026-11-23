# 엔진 런타임 — 20. Controller·Pawn(S12)

[목차](README.ko.md)

## 20. Controller·Pawn(S12)

설계일 2026-11-23. UE 경로 약어: `E/` = `Engine/Source/Runtime/Engine/`.

### 20.1 UE 5.8 계약

- `AController::Possess` → `OnPossess`: 기존 Pawn UnPossess → 대상 Pawn의 기존 Controller UnPossess → `Pawn::PossessedBy`(Owner·Controller·PlayerState 연결, `NotifyControllerChanged`) → `SetPawn`(Pawn·이동 컴포넌트 Tick이 Controller Tick 뒤) → `SetControlRotation` → `Pawn::DispatchRestart`(`E/Private/Controller.cpp:320–384, 495–540`, `E/Private/Pawn.cpp:671–711`).
- `UnPossess` → `OnUnPossess` → `Pawn::UnPossessed`(PlayerState·Owner·Controller 해제, Pawn 입력 컴포넌트 파괴, `NotifyControllerChanged`) → `SetPawn(null)`(`Controller.cpp:386–419`, `Pawn.cpp:713–751`).
- Pawn 파괴: `APawn::Destroyed`(EndPlay 전)가 `DetachFromControllerPendingDestroy` → `Controller::PawnPendingDestroy`(UnPossess, PlayerState 없는 Controller는 자신도 파괴)(`Pawn.cpp:550–562, 1112–1124`, `Controller.cpp:421–440`). Controller 파괴는 기본적으로 Pawn을 남긴다(`Controller.cpp:595–612`).
- PlayerController: Login 중 PostInit에서 PlayerState·`PlayerCameraManager` 생성(`E/Private/PlayerController.cpp:1069–1090`), `SetPlayer(LocalPlayer)`로 양방향 연결과 입력 시스템 초기화(5346–5400, 756–776). Tick은 `PlayerTick`(입력 처리) 뒤 `Tick`(5636–5685). 카메라는 모든 tick group과 타이머·tickable 뒤, PostUpdateWork 전에 `UpdateCameraManager`(`E/Private/LevelTick.cpp:1778–1883`, `PlayerController.cpp:6477–6493`).
- Pawn 입력 컴포넌트는 로컬 PlayerController가 possess할 때 `PawnClientRestart`에서 만들고(`Pawn.cpp:509–525`) UnPossessed에서 없앤다. 입력 스택은 push된 것 > Controller > LevelScript > Pawn(`PlayerController.cpp:2668–2740`).
- AI: `AutoPossessAI`가 Pawn PostInit에서 `SpawnDefaultController`(`Pawn.cpp:145–158, 384–406`).

### 20.2 프레임워크 내부 hook

이 절의 클래스는 17.8절의 `OnFrameworkPreAwake`·`OnFrameworkPostAwake`·`OnFrameworkEndPlay`(ERD19)로 엔진 처리를 한다. `OnFrameworkPostAwake`는 그 Actor의 모든 Component `OnAwake` 뒤라 UE `PostInitializeComponents`와 같은 자리다(자동 Possess가 Pawn·Component 초기화 뒤에 일어난다).

### 20.3 Controller

`Controller : Actor`(배치 불가). 가진 것: `pawn`, `playerState`, `controlRotation`(`Rotator`, 도 단위 — UE `FRotator`), `state`(`Inactive`·`Playing`·`Spectating`).

| API | 순서·규칙 |
|---|---|
| `Possess(pawn)` | 권한 확인(서버·standalone) → `OnPossess` → Pawn이 바뀌었으면 `OnPossessedPawnChanged(old, new)` 이벤트 |
| `OnPossess(pawn)`(virtual) | 기존 Pawn UnPossess → 대상의 기존 Controller UnPossess → `pawn.PossessedBy(this)` → `SetPawn` → `SetControlRotation(pawn 회전)` → `pawn.Restart()` |
| `UnPossess()`·`OnUnPossess()` | `pawn.UnPossessed()` → `SetPawn(null)` |
| `SetPawn` | Pawn Tick에 Controller Tick 선행을 건다(15.3절 자동 간선, UE `AddPawnTickDependency`). 바뀌면 이전 간선을 뗀다 |
| `PawnPendingDestroy(pawn)` | UnPossess → `state = Inactive` → PlayerState가 없으면 자신 파괴 |
| `OnFrameworkPostAwake` | PlayerState 생성(PlayerController는 언제나, AiController는 `wantsPlayerState`일 때) → `GameplayWorldSubsystem`에 등록 |
| `OnFrameworkEndPlay` | GameDirector가 있으면 사유와 무관하게 `Logout`(UE는 `Destroyed`에서 부르고, 전환·종료에서도 PlayerController를 `DestroyActor`로 지워 결국 부른다 — 21.3절) → PlayerState 파괴 → UnPossess(Pawn은 남긴다) → 등록 해제 |

### 20.4 Pawn

`Pawn : Actor`. 가진 것: `controller`, `playerState`, `autoPossessAi`(`Disabled`·`PlacedInWorld`·`Spawned`·`PlacedInWorldOrSpawned`), `aiControllerType`.

- `PossessedBy(controller)`: Owner = controller → controller·PlayerState 연결 → 사용자 hook `OnPossessed(Controller&)` → 바뀌었으면 `OnControllerChanged(old, new)`.
- `UnPossessed()`: PlayerState·Owner·controller 해제 → 입력 연결 해제 → `OnUnpossessed()` → `OnControllerChanged`.
- `Restart()`: 사용자 hook `OnRestart()`(UE `Restart` — 이동 정지·입력 소비는 이동 컴포넌트가 생기면 그쪽 일).
- `OnFrameworkPostAwake`: `autoPossessAi`가 맞으면(배치 여부는 21.2절 7단계 `populate` 중이면 배치) `aiControllerType`으로 AiController를 spawn해 Possess(UE `SpawnDefaultController`).
- `OnFrameworkEndPlay`: Controller가 있으면 `controller.PawnPendingDestroy(this)`(UE `DetachFromControllerPendingDestroy`).
- `GetControlRotation()`: Controller가 있으면 그 값(UE도 Controller에 위임).

### 20.5 PlayerController

`PlayerController : Controller`. 가진 것: `localPlayer`(handle — 엔진 Registry 객체), `playerInput`(그 프레임 입력 스냅숏), `cameraDirector`(handle).

- `OnFrameworkPostAwake`: Controller 부분(PlayerState) → CameraDirector spawn(Owner = 이 PlayerController) → `state = Spectating`.
- `SetPlayer(LocalPlayer&)`: 양방향 연결(LocalPlayer의 PlayerController handle) → `playerInput` 초기화 → Spectating이면 관전 시작 상태 유지.
- **PlayerTick.** PlayerController의 `OnTick`은 엔진이 닫고(`final`) ① `TickPlayerInput`(LocalPlayer의 `InputSource`에서 이번 프레임 키·마우스·게임패드 상태를 `playerInput`으로 복사) → ② `OnPlayerTick(ctx)`(UE `PlayerTick` — 입력 해석) → ③ `OnControllerTick(ctx)`(UE `Tick` 본문) 순으로 부른다. Pawn은 `Controller().PlayerInput()`으로 읽는다.
- **pause.** PlayerController Tick은 `tickWhenPaused`가 참이다(UE `PlayerController.cpp:204`). pause 중에는 ① `TickPlayerInput` → ③ `OnControllerTick`만 부르고 ② `OnPlayerTick`은 건너뛴다(UE pause 중 `TickPlayerInput`·`Tick`만 — `PlayerController.cpp:5504–5519`). pause 해제 입력은 `OnControllerTick`에서 `ctx.paused`를 보고 처리한다.
- **입력 스택**(UE `PushInputComponent`·`BuildInputStack`)과 Pawn 입력 컴포넌트(`SetupPlayerInputComponent`)는 `InputComponent`(입력 액션·매핑 — [컴포넌트 도입](../ComponentCatalog/04-Prerequisites.ko.md) CR21·CR26)와 함께 들인다. 이 단계는 9절 경계(PlayerController가 LocalPlayer `InputState`를 읽는다)까지다.
- `OnPossess`는 Controller 부분 뒤 `state = Playing`, CameraDirector view target = Pawn.
- `OnUnPossess`는 view target을 자신으로 되돌린다.
- `OnFrameworkEndPlay`: CameraDirector 파괴 → Controller 부분 → LocalPlayer 연결 해제. 로컬 플레이어가 없는(접속 끊김) PlayerController만 Pawn도 파괴한다(UE `PawnLeavingGame`).

### 20.6 CameraDirector(최소)

`CameraDirector : Actor`(배치 불가, UE `APlayerCameraManager`, 계획 20.5절 정본). 이 단계의 범위:

- view target(`ObjectHandle`, 기본은 소유 PlayerController)과 결과 `CameraView{location, rotation, fov}`. `fov` 기본 90도(UE `DefaultFOV`).
- `UpdateCamera(deltaSeconds)`: LateUpdate 시작 엔진 단계 `CameraUpdate`(15.4절)에서 PlayerController 등록 순서대로 부른다. pause 중에는 부르지 않는다(UE `LevelTick.cpp:1844–1851` — 15.4절). view target의 root SceneComponent 월드 Transform을 결과로 쓴다. view target이 Pawn이면 회전은 Controller `controlRotation`이다(UE 기본 `CalcCamera` — 카메라 컴포넌트가 없을 때 eye 위치·control rotation).
- 사용자 hook `OnUpdateView(CameraView&)`로 결과를 고칠 수 있다.
- blend·camera modifier·shake·카메라 컴포넌트 우선(UE `CalcCamera`의 `UCameraComponent`)은 CM0·CR29가 들일 때 넓힌다.

### 20.7 AiController(최소)

`AiController : Controller`. `wantsPlayerState`(기본 거짓), Possess·UnPossess hook만. brain·path following·perception은 CR39·CM16이다.

### 20.8 시험(완료 조건)

- Possess 순서(기존 해제 → PossessedBy → SetPawn 간선 → ControlRotation → Restart)와 이벤트, 다른 Controller의 Pawn 빼앗기.
- UnPossess, Pawn 파괴 → Controller Inactive(PlayerState 유지), AI는 자신도 파괴, Controller 파괴 → Pawn 남음·Logout이 EndPlay 전.
- SetPawn 간선: 같은 단계에서 Pawn Tick이 Controller Tick 뒤, Pawn 교체 시 간선 이동.
- PlayerTick: 입력 스냅숏 → OnPlayerTick → OnControllerTick 순서, 헤드리스 입력 주입, pause 중 OnPlayerTick 생략.
- CameraDirector: 갱신 자리(LateUpdate 시작, Timers·tickable 뒤), view target 기본·Possess 뒤 Pawn·UnPossess 뒤 복귀, pause 중 갱신 안 함·단일 스텝 프레임은 갱신.
- PlayerState 수명(S11에서 옮김): Controller가 만들고 GameState에 등록, Pawn 파괴에도 유지, Controller 파괴 때 제거.
- AutoPossessAI 네 값 × 배치·spawn.
