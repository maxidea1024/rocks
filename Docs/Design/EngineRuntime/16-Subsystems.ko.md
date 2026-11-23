# 엔진 런타임 — 16. Subsystem 의존성(S8)

[목차](README.ko.md)

## 16. Subsystem 의존성(S8)

설계일 2026-11-23. UE 경로 약어: `E/` = `Engine/Source/Runtime/Engine/`.

### 16.1 지금 있는 것

`World/Subsystem.h`의 `SubsystemCollection<Base, Owner>`가 한 묶음 안에서 선언된 의존(`dependencies` 키 목록)으로 위상 정렬하고, 중복 키·목록에 없는 의존·순환을 아무것도 만들기 전에 `InvalidArgument`로 거부하며, 초기화 실패 때 이미 만든 것을 역순으로 정리한다. 쓰는 종류는 EngineSubsystem(6절)과 WorldSubsystem(`Initialize`·`OnWorldBeginPlay`·`Deinitialize`) 둘이다.

### 16.2 UE 5.8 계약

- 기본 `ShouldCreateSubsystem`은 true이고, WorldSubsystem은 `DoesSupportWorldType`(기본 Game·Editor·PIE)으로 거른다(`E/Public/Subsystems/Subsystem.h:56`, `E/Private/Subsystems/WorldSubsystem.cpp:18–32`).
- 생성 순서는 클래스 맵 순서라 형제 사이 순서를 보장하지 않고, 순서는 `InitializeDependency<T>()`로만 만든다. 대상이 생성 안 되면 nullptr와 로그, 순환은 감지하지 않고 초기화가 끝나지 않은 인스턴스를 돌려준다(`E/Private/Subsystems/SubsystemCollection.cpp:366–396, 483–543`).
- Deinitialize는 삽입 순서 **정방향**이다(`SubsystemCollection.cpp:349–358`). World는 `PreDeinitialize` 일괄 → `Deinitialize` 순서다(`E/Private/World.cpp:6474–6487`).
- 수명: Engine은 `UEngine::Init`·`PreExit`(`E/Private/UnrealEngine.cpp:2403, 2757`), GameInstance는 `UGameInstance::Init` **마지막**과 Shutdown에서 LocalPlayer를 다 지운 **뒤**(`E/Private/GameInstance.cpp:100, 129, 147–167`), LocalPlayer는 `PlayerAdded`·`PlayerRemoved`(`E/Private/LocalPlayer.cpp:262, 270, 280`).
- World 순서: `InitializeSubsystems` → `PostInitialize` → (컴포넌트 갱신 `OnWorldComponentsUpdated`) → Actor 초기화 → `OnWorldBeginPlay`(Actor BeginPlay **전**) → … → Actor EndPlay → `OnWorldEndPlay` → `PreDeinitialize` → `Deinitialize`(`World.cpp:2447–2610, 6154–6211`).
- Tickable WorldSubsystem은 PostPhysics 뒤 TimerManager 다음에 Tick한다(`E/Private/LevelTick.cpp:1778, 1816–1821`).
- 모듈 로드·언로드 때 살아 있는 묶음에 동적으로 더하고 빼며, 늦게 더한 WorldSubsystem에는 놓친 `PostInitialize`·`OnWorldBeginPlay`를 보충 호출한다(`World.cpp:752–772`).

### 16.3 Rocks 계약

- **엄격한 정적 의존 유지(ERD8).** 의존은 지금처럼 타입 정의에 정적으로 선언하고, 순환·누락을 생성 전에 거부한다. 정리는 초기화의 **역순**이다. UE와 다르다(UE는 순환 미감지·정방향 정리). 의존하는 쪽이 정리될 때까지 의존 대상이 살아 있어야 해서다. `InitializeDependency` 같은 실행 중 의존 요청은 두지 않는다.
- **조건부 생성(ERD9).** `SubsystemType`에 `shouldCreate(const void* owner)` 판정을 더한다(없으면 소유자 기본 판정 — `SubsystemCollection::Initialize`의 세 번째 인자, 없으면 생성). 생략한 것은 `Skipped()`(`SkippedSubsystem{name, reason, missing}`)에 남고 World는 `SkippedSubsystems()`로 보여 준다. WorldSubsystem 기본 판정은 World 종류가 Game·Editor·Tryout일 때 생성이다(UE `DoesSupportWorldType`, 16.4절 World 종류). 필수 의존이 생성되지 않으면 의존하는 쪽도 생성하지 않고 진단 `SubsystemSkippedMissingDependency`를 남긴다(오류는 아니다). UE와 다르다 — UE는 `InitializeDependency`가 nullptr를 돌려줘도 의존하는 쪽을 그대로 만든다. Rocks는 필수로 선언한 의존이 언제나 있다는 계약을 지키려고 의존하는 쪽을 뺀다(문서화된 차이). 있으면 순서만 맞추는 `optionalDependencies`를 따로 둔다.
- **바깥 계층 조회(ERD10).** 계층은 Engine ⊃ GameInstance ⊃ LocalPlayer, Engine ⊃ World다. World 모듈은 Engine·Gameplay를 모르므로(3절) World에는 **타입을 지운 조회 창**만 둔다: `WorldDesc::outerSubsystems`(`OuterSubsystemLookup` — `void* Find(SubsystemKey) const`)를 생성 때 넘기고 World는 해석하지 않는다. 타입 있는 도우미는 바깥 모듈의 자유 함수다: `GetEngineSubsystem<T>(World&)`(Engine 모듈), `GetGameInstanceSubsystem<T>(World&)`(Gameplay 모듈). 바깥이 안쪽을 조회하는 API는 두지 않는다.
  - 수명: GameInstance Subsystem은 게임 World의 `Deinitialize`보다 먼저 정리된다(18.2절 종료 순서, UE도 `GameEngine.cpp:1374–1381`에서 같다). 그래서 조회 창은 World `EndPlay`가 끝날 때(Director 없는 World는 `Shutdown` 시작 때) 닫히고, `PreDeinitialize`·`Deinitialize` 안의 조회는 nullptr다. 정리 단계에서 바깥 Subsystem이 필요하면 `OnWorldEndPlay`에서 끝낸다. 초기화 실패의 되돌림(이미 초기화한 것의 `Deinitialize`)도 창을 먼저 닫는다. 창이 돌려주는 `void*`는 그 계층 묶음의 Base 포인터이고, 창의 context는 World `Shutdown`이 끝날 때까지 살아 있어야 한다(World.h `OuterSubsystemLookup` 계약). Engine도 생략된 EngineSubsystem을 `SkippedSubsystems()`로 보여 주고 필수 의존 누락은 Warning을 남긴다.
- **새 종류(ERD11).** `GameInstanceSubsystem`(Gameplay 모듈, 18절)과 `LocalPlayerSubsystem`(18.4절)을 같은 `SubsystemCollection`으로 만든다. 초기화·정리 시점은 UE와 같다: GameInstance `OnInit` 뒤 마지막, LocalPlayer 추가 때. 정리는 LocalPlayer(제거 때) → GameInstance(LocalPlayer를 모두 지운 뒤).
- **동적 추가·제거.** 모듈 로더(ML)의 실행 중 모듈 적재가 생길 때 다룬다(후속). 지금은 묶음 생성 때 정해진 목록만 쓴다.

### 16.4 World 종류

`WorldDesc::kind`(`WorldKind`)를 더한다: `Game`(독립 실행·서버), `Editor`(편집 World), `Tryout`(편집 World 복제 실행 — 2절), `Preview`(에셋 미리보기·썸네일), `Inactive`(적재됐지만 실행하지 않는 World). UE `EWorldType`의 Game·Editor·PIE·EditorPreview·GamePreview·Inactive에 대응한다(GamePreview는 Preview로 합친다). Editor·Inactive World의 `BeginPlay`는 `InvalidArgument`이고 Engine은 Inactive World를 Tick하지 않는다. 종류는 Subsystem 생성 판정(ERD9)과 GameDirector 생성(19.3절)에 쓴다.

### 16.5 WorldSubsystem hook(ERD12)

| hook | 시점 | UE |
|---|---|---|
| `Initialize` | World `Initialize`, 의존 순서 | 같음 |
| `PostInitialize` | 모든 `Initialize` 뒤, 같은 순서 | `PostInitialize` |
| `OnWorldBeginPlay` | `World::BeginPlay`, Actor `OnStart` 전 | 같음 |
| `OnWorldEndPlay` | `World::EndPlay`(17.6절)에서 Actor 정리 **뒤**, 역순 | 시점 같음, 순서만 다름(UE 정방향 — `World.cpp:6203–6206`, ERD8) |
| `PreDeinitialize` | Shutdown, 모든 `Deinitialize` 전 일괄(역순) | 시점 같음, 순서만 다름(UE 정방향 — `World.cpp:6476–6479`) |
| `Deinitialize` | 역순 | 순서만 다름(ERD8) |

- **Tick하는 WorldSubsystem.** `WorldSubsystem`의 `IsTickable()`·`TickWhenPaused()`·`OnTick(World&, const TickContext&)`(구현 S8). LateUpdate 시작 엔진 단계 `TickableSubsystems`(15.4절)에서 World가 초기화 순서대로 먼저 부르고, 그 자리에 등록된 콜백(`SetEngineStage`)은 그 뒤에 부른다. World가 Initialized·Playing일 때만 실행하고 pause 중에는 `tickWhenPaused`가 참인 것만 unscaled delta로 실행한다(UE `ETickableTickType`·`IsTickableWhenPaused`).
- 늦은 추가가 없으므로 보충 호출은 필요 없다(동적 추가를 들일 때 UE처럼 보충한다).

### 16.6 시험(완료 조건)

- `shouldCreate` 거짓 → 생성 안 됨, 필수 의존 대상이 생성 안 되면 의존자도 생략(진단), 선택 의존은 순서만.
- World 종류별 기본 생성 판정.
- 바깥 계층 조회: WorldSubsystem이 Engine Subsystem을 `Initialize`에서 조회(S8), GameInstance Subsystem 조회는 S10 시험, `PreDeinitialize`·`Deinitialize`에서는 nullptr.
- hook 순서: Initialize → PostInitialize → OnWorldBeginPlay → (Actor OnStart) → … → Actor 정리 → OnWorldEndPlay → PreDeinitialize(역순) → Deinitialize(역순).
- Tickable: 실행 자리(LateUpdate 시작, Timers 뒤·CameraUpdate 전), pause 정책.
- GameInstance·LocalPlayer Subsystem의 초기화·정리 시점(18절 시나리오와 함께).
