# 레벨 스크립트(LevelScript) 설계

기준일: 2026-11-20, 개정 1(결정 LD1–LD20 확정 — 5절, 리뷰 정정 반영 — 10절). 초안 0.1(2026-11-19, 결정 대기)을 리뷰어 1명이 문서 원문·UE 소스와 대조한 정정과 사용자 결정(모든 항목 권장안)을 반영한 첫 정본이다. 구현하지 않는다. 착수는 고정 구현 순서(리플렉션 B9 → Curve → A0-G → A1–A6 → 화면 마일스톤)와 Sparks 도입 단계(L2 이후, [Sparks](SparksLanguage.ko.md) 18절)를 따르며 이 문서는 그 순서를 바꾸지 않는다.

근거: [Sparks 언어](SparksLanguage.ko.md)(이하 SL) 초안 1.4, [엔진 구현 계획](EngineImplementationPlan.ko.md)(이하 EIP) 20·22·23절, [엔진 런타임](EngineRuntime/README.ko.md)(이하 ER) 4.6·7절, [Prefab](Prefab.ko.md)(이하 PF) 개정 5, [UI 툴킷](UiToolkit.ko.md)(이하 UT) 7.3절 (e), [에셋 시스템](AssetSystem.ko.md)(이하 AS) 4.6절, [리플렉션 B9](ReflectionExtensions.ko.md)(이하 RX) 5.2절(클래스 참조). UE 대조는 UE 5.8(5.8.4) 원본을 파일:행으로 확인했다(2절).

사용 흐름: 컴포넌트·액터·레벨 스크립트에 동작을 넣는 작업 순서를 Sparks와 C++로 나눠 [게임플레이 작업 안내](../Guides/GameplayWorkflowGuide.ko.md)에 둔다.

원칙: Blueprint 제외 결정(EIP 20.4·23.1절)을 유지한다. 실행 체계는 C++와 Sparks 하나뿐이다. 구조는 UE 5.8 레벨 블루프린트를 따르고 이름은 Rocks식으로 짓는다. UE 이름은 9절 대응표와 근거 인용에만 둔다. UE보다 엄격하게 정한 곳은 그 자리에 적는다.

약어:

- UE: `ENG/` = Engine/Source/Runtime/Engine/, `ED/` = Engine/Source/Editor/.
- Rocks: `WD` = Engine/Source/Runtime/World/Public/World.

## 1. 목적·범위·비범위

### 1.1 목적

질문은 "UE에서는 씬 배치와 플레이 시작 이벤트를 모두 Blueprint(BP)로 처리하니, Rocks도 BP를 먼저 지원하고 Sparks가 자리 잡으면 빼는 게 낫지 않은가"였다. 결론은 다음과 같다.

- **BP 제외는 유지한다.** UE에서 BP가 맡는 흐름은 Rocks의 다른 수단에 하나씩 대응되며(2.1절), 레벨 블루프린트에 해당하는 것만 빠져 있었다. 이 빈 곳을 Sparks 기반 타입 **LevelScript**(레벨마다 숨은 Actor 하나)로 채운다.
- BP는 기능 하나가 아니라 두 번째 실행 체계(그래프 편집기·컴파일러·VM·디버거·핫 리로드·복제·쿡·AOT)다. 콘텐츠가 쌓인 뒤에 빼면 사용자 에셋을 옮겨야 한다. UI 툴킷과 에디터가 먼저 있어야 돌아가므로 Sparks L0보다 늦게 나와 임시 수단 역할도 하지 못한다. UE 자신도 게임플레이 모델을 Verse로 옮기고 BP를 장기적으로 deprecate한다고 밝혔다(EIP S95, SL 16절).

### 1.2 범위

| 항목 | 이 문서가 정하는 것 |
|---|---|
| 연결 수단 정리 | 스크립트를 Actor에 연결하는 다섯 가지 연결점과 편집기 흐름, 기본값·hook 시점, Actor 사이 주고받기(3절) |
| LevelScript 타입 | C++ `LevelScript`(Actor 파생, 배치 불가), `Level.levelScript`, Sparks 기반 타입(4.1절) |
| 저장·로드 | Level 저장 형식과 검증, cooked 로드 검증(4.2절) |
| 수명 | 레벨 편입·Start·언로드 순서, 파괴 규칙(4.3·4.4절) |
| 참조 | `@edit` 참조의 대상·형태, prefab 배치가 펼친 노드 참조(4.5절) |
| 물리 | 다른 Actor의 트리거·충돌을 받는 엔진 Actor 이벤트, LevelScript의 물리 금지(4.6절) |
| 주변 | 스트리밍·네트워크·핫 리로드·편집 World·Tryout(4.7절), 편집기 명령·생성 경로(4.8절), 진단(4.9절) |

### 1.3 비범위

- 노드 기반 시각 편집기 — 보류 선택지로만 기록한다(LD2).
- Level Instance 안의 LevelScript — 1차 금지, Level Instance 도입 때 재검토(LD11, PF D1 (c)).
- 배치 Actor의 네트워크 정체성 규칙 — 네트워크 단계가 정하며 이 문서는 그 규칙을 따른다(6절 선행 조건, LD12).
- World Partition 셀 사이 참조 — 영속 Entity 참조가 생길 때까지 저장 거부라는 일반 규칙(SL 4.3절, PF D12)을 따른다(LD6).
- 편집기 화면 구성(메뉴 위치·아이콘) — 편집기 단계가 정한다. 이 문서는 명령과 결과 규칙만 정한다.

## 2. UE 5.8 조사

### 2.1 UE에서 블루프린트가 하는 일

UE의 BP 클래스는 코드(이벤트 그래프), 컴포넌트 구성(SCS), 기본값 데이터를 에셋 하나에 묶는다. 그래서 배치도 이벤트 처리도 BP를 거친다. Rocks는 이 셋을 나눠서 코드는 C++·Sparks 클래스가, 구성과 데이터는 prefab 에셋이 맡는다.

| UE에서 BP로 하는 일 | Rocks 대응 | 근거 |
|---|---|---|
| BP Actor 클래스를 레벨에 끌어다 배치 | prefab 에셋 배치(`PrefabPlacement`) 또는 C++·Sparks Actor 클래스 배치(클래스 원형 = default subobject) | PF D1·D3 |
| Actor BP의 BeginPlay·Overlap 등 이벤트 그래프 | Sparks `ScriptableComponent` 또는 Actor 파생 클래스의 `on Start()`·`on TriggerEnter(...)` | SL 5절 |
| BP 변수의 Details 편집, 다른 Actor 지정 | `@edit` 필드, `@edit var Target: Door?` | SL 4.3절 |
| GameMode·GameInstance BP의 게임 전체 처리 | `WorldService`(manifest `services`), 엔진 GameDirector·GameState 등 | SL 3.4절, ER 2절 |
| Widget Blueprint 그래프 | WidgetAsset + C++·Sparks `WidgetController` | UT |
| 레벨 블루프린트(레벨마다 하나인 스크립트) | **LevelScript**(이 문서) | — |

### 2.2 레벨 블루프린트의 구조

| 사실 | 근거 |
|---|---|
| Level이 BP 에셋(편집기 전용)과 그 BP로 만든 Actor 하나를 따로 가진다 | `ENG/Classes/Engine/Level.h:470-473` LevelScriptBlueprint(WITH_EDITORONLY_DATA), `:488-490` LevelScriptActor |
| `ALevelScriptActor`는 Actor 파생이며 `notplaceable`이다. 충돌·겹침·클릭 이벤트 재정의를 숨기고(`KismetHideOverrides`) Collision·Rendering·Transformation 범주를 숨긴다 | `ENG/Classes/Engine/LevelScriptActor.h:24-25` |
| 편집기에서 이름표·편집이 막혀 있다(`bActorLabelEditable = false`, `bEditable = false`) | `ENG/Private/LevelScriptActor.cpp:24-25` |
| 복제되며 항상 관련이다(`bReplicates = true`, `bAlwaysRelevant = true`) | `ENG/Private/LevelScriptActor.cpp:32-33` |
| 같은 Outer에 둘째 LSA를 만들면 생성자가 `ensure`로 알린다 | `ENG/Private/LevelScriptActor.cpp:35-46` |
| 로드 뒤 `ULevel::RepairLevelScript`가 둘 이상을 감지해 오류 로그를 남기고, 복구(여분 삭제) 알림은 `WorldType == Editor`일 때만 띄운다. 저장을 거부하지는 않는다 | `ENG/Private/Level.cpp:5052-`(감지·로그 5064-5071, 복구 버튼 5078) |
| 편집기는 레벨 BP 그래프에 Actor를 끌어 놓을 때 같은 레벨의 Actor만 받는다(`DroppedActor->GetLevel() == BlueprintLevel`) | `ED/Kismet/Private/BlueprintEditor.cpp:4081-4092` `FBlueprintEditor::OnGraphEditorDropActor` |
| "액터 이벤트" 노드는 편집기 원본 LSA에서만 다시 묶는다. LSA 패키지가 BP 패키지와 다르면(PIE 복제본) 묶지 않아 복제본이 편집기 객체에 교차 결합하는 것을 막는다 | `ED/UnrealEd/Private/Kismet2/BlueprintEditorUtils.cpp:8491-8545` `FixLevelScriptActorBindings` |
| 다른 레벨 호출 `RemoteEvent(FName)`은 보이는 모든 레벨의 LSA에서 인자 없는 함수를 이름 문자열로 찾아 호출하고 bool만 돌려준다 | `ENG/Private/LevelScriptActor.cpp:64-89` |
| 레벨 BP의 부모 클래스는 엔진 설정 `LevelScriptActorClass`로 고른다 | `ENG/Classes/Engine/Engine.h:875` |
| World Partition으로 변환할 때 메인이 아닌 레벨의 LSA는 지운다 | `ENG/Private/WorldPartition/WorldPartitionConverter.cpp:191-199` |
| Level Instance는 LSA를 가질 수 있고, 레벨 스크립트가 있는 Level Instance는 완전히 풀(break) 수 없다 | `ENG/Private/LevelInstance/LevelInstanceSubsystem.cpp:1704-1706`, 판정 함수 `:1875` |

### 2.3 BeginPlay 순서

UE는 LSA를 순서에서 특별 취급하지 않는다.

- `AWorldSettings::NotifyBeginPlay`가 `FActorIterator`로 Actor를 돌며 BeginPlay를 보낸다(`ENG/Private/WorldSettings.cpp:363-379`). 순회 순서는 편집기 빌드에서 Level `Actors` 배열 순서, 비편집기 빌드에서 `GetObjectsOfClass` 순서다(`ENG/Public/EngineUtils.h:176-260`).
- `ULevel::SortActorList`는 WorldSettings → 비복제 → 복제 순으로만 나눈다(`ENG/Private/Level.cpp:1262-1313`).
- 스트리밍 레벨은 `RouteActorInitialize`가 `Actors` 배열 순서로 BeginPlay를 보낸다(`ENG/Private/Level.cpp:3896-3909`).

그래서 UE에서 LSA의 BeginPlay가 다른 Actor보다 앞인지 뒤인지는 배열 위치에 달려 있다. Rocks의 순서 규칙(LD8)은 UE를 따른 것이 아니라 결정적 순서를 위한 Rocks 고유 규칙이다.

### 2.4 Rocks가 가져갈 구조

| UE 구조 | Rocks 결론 |
|---|---|
| Level이 Actor 하나를 따로 가리킨다(2.2) | `Level.levelScript`가 `actors` 안의 Actor 하나를 가리킨다(LD4) |
| BP 에셋을 Level 안에 편집기 전용으로 둔다 | 스크립트는 일반 `.spark` 파일이다. 파일 경로 = import 경로 규칙을 지킨다(LD4 (다) 기각) |
| `notplaceable`, 편집·이름표 막음 | UT `NotPlaceable` 재사용 + 편집기 규칙으로 Outliner의 "레벨 스크립트" 항목으로만 노출(LD3) |
| 둘 이상은 감지·로그·편집기 복구 | 저장 거부와 cooked 로드 거부(LD5 — UE보다 엄격) |
| 같은 레벨 Actor만 묶음, 복제본 교차 결합 방지 | 같은 Level 대상만 저장(LD6). 연결은 실행 중 session이 만들므로 복제본 교차 결합이 구조적으로 없다(4.7절) |
| `RemoteEvent` 이름 문자열 호출 | 두지 않는다. World 이벤트(LD10) |
| `KismetHideOverrides`·Collision 숨김 | 물리 hook·Collider 금지(LD18) |
| `bAlwaysRelevant`·복제 | 모든 process에 존재, 항상 관련(LD12) |
| `LevelScriptActorClass` 설정 | 프로젝트 설정 `DefaultLevelScriptClass`(LD20) |
| WP 변환 때 메인 레벨에만 남김, Level Instance는 허용하되 break만 막음 | WP 셀·Level Instance·prefab에 두지 않는다(LD11 — Level Instance는 UE보다 엄격) |

## 3. 스크립트를 Actor에 연결하는 방법

이 절은 현재 스펙(SL 초안 1.4, PF 개정 5)의 연결 수단을 한곳에 모은다. LevelScript는 그중 하나다.

### 3.1 연결점 다섯 가지

| 기반 타입 | 어디에 붙나 | 몇 개 | 쓰는 곳 | UE 대응 |
|---|---|---|---|---|
| `ScriptableComponent` | Actor 하나 — 직접 배치한 Actor, prefab, Sparks Actor 클래스의 `let` 필드 | Actor마다 여러 개 | 재사용하는 동작: 문 여닫기, 체력, 줍기 | ActorComponent BP |
| Actor 파생(`class Door : Actor`) | 클래스 자체가 Actor. 콘텐츠에서 끌어다 배치, `World.Spawn` | 배치·생성한 만큼 | 그 자체로 의미 있는 물체: 캐릭터, 문, 엘리베이터 | Actor BP 클래스 |
| `WorldService` | World. 패키지 manifest `services`에 등록 | World마다 1 | 게임 전체 상태: 점수, 퀘스트 진행 | GameMode·GameInstance BP 일부 |
| `LevelScript` | Level. 레벨마다 숨은 Actor 하나 | Level마다 0 또는 1 | 그 레벨에만 있는 연출: 시작 컷씬, 특정 문과 트리거 연결 | Level Blueprint |
| `WidgetController` | WidgetAsset의 컨트롤러 클래스 | widget마다 1 | UI 동작 | Widget Blueprint 그래프 |

### 3.2 편집기에서 연결하는 순서

1. **스크립트 만들기.** 콘텐츠 브라우저의 "새 Sparks 스크립트"에서 기반 타입(`ScriptableComponent`, Actor 등)을 고르면 패키지 안에 `.spark` 파일이 생긴다. 파일 경로가 곧 import 경로다. 편집은 VS Code와 Sparks 언어 서버(자동 완성·진단)로 하고, 저장하면 증분 컴파일된다(SL 12·13절). LevelScript는 "레벨 스크립트 열기" 명령이 만든다(4.8절).
2. **Actor에 붙이기 — 세 가지 경로.**
   - (A) 레벨에 직접 배치한 Actor: Actor를 고르고 Details의 "컴포넌트 추가"에서 Sparks 클래스를 고른다. 그 Actor의 `components`에 들어가 레벨과 함께 저장된다(LD15).
   - (B) prefab: prefab 편집기에서 추가하면 그 prefab의 모든 배치가 받는다. 배치 하나에만 추가하면 instance override의 "노드 추가" 항목이 된다(PF 3.5절, PF4 선행, LD15).
   - (C) Sparks Actor 클래스: `let Health = Health(MaxHealth: 200);`처럼 소유 component로 선언한다. 클래스 원형의 default subobject가 되며 선행 조건은 PF1이다.
   - (A)·(B)는 편집기에서 레벨·배치 데이터를 고치는 일이라 core 프로필에서도 된다. 실행 중 `AddComponent`로 붙이는 것은 full 전용이다(SL 1.3·4.5절).
3. **값 지정.** `@edit` 필드가 Details에 나타난다. `@edit(Min: 0, Max: 10)` 범위는 편집기 입력 제한이면서 실행 중 계약이다(SL 15절).
4. **다른 Actor 지정.** `@edit var Target: Door?` 필드에 Outliner의 Actor를 지정한다. 같은 저장 단위 안이면 약한 참조(`WeakObjectRef`)로 저장된다. 다른 World Partition 셀의 Actor는 영속 Entity 참조가 생긴 뒤 `SoftEntityId`로 저장되고 그 전에는 저장을 거부한다(SL 4.3·7.6절, PF D12). prefab 배치가 펼친 Actor는 (배치 정체 + 노드 경로)로 저장한다(LD17, PF D22).
5. **실행과 수정.** Tryout은 편집 World를 복제한 World에서 hook을 실행한다. 편집 World에서는 게임 hook이 돌지 않는다(PF D18). 실행 중 수정은 핫 리로드 R1(본문)–R3(재생성)으로 반영된다(SL 12.4절).

### 3.3 기본값이 정해지는 순서

C++ 생성자 → Sparks 필드 초기값 → subobject 기본값 → prefab·variant → 배치 instance override → spawn 인자. 뒤쪽이 이긴다(SL 4.5절, PF 3.1절). 코드의 기본값을 바꾸면 override하지 않은 배치에 반영되고, override한 배치는 편집기에 표시된다. LevelScript는 레벨에 직접 배치한 Actor와 같아서 "C++ 생성자 → Sparks 필드 초기값 → subobject 기본값 → 레벨 Record 값"이다.

### 3.4 hook이 불리는 시점

| 상황 | 순서 |
|---|---|
| 레벨 로드(직접 배치 Actor) | 편입 단계(ER 4.6절 1–5항) → Actor마다 `FinishSpawning`(OnRegister → Actor OnAwake → Component OnAwake → OnEnable, ER 7.3절) → **LevelScript의 `FinishSpawning`은 그 Level의 모든 배치 Actor 뒤**(LD8) → 다음 프레임 시작 처리에서 `IsReadyToStart`가 참인 Actor부터 Start(시작 큐 순서) → Start가 끝나면 Tick 계열 시작 |
| 레벨 로드(prefab 배치) | 배치 기록을 펼친다(PF5). 펼친 묶음은 spawn 묶음 규칙(묶음 전체 Awake 성공 뒤 graph 순서로 Enable — SL 5.1절)을 따르고, LevelScript는 이것도 기다린다(LD8) |
| `World.Spawn(prefab, at)` | spawn 묶음 Awake가 모두 성공해야 Enable. 하나라도 실패하면 spawn 실패로 rollback(SL SD43) |
| 파괴·셀 unload·서브 레벨 unload·travel·Tryout 종료 | Disable(활성이었다면) → Destroy → Detach. 실행 중이던 `await`는 취소되고 유지 자원은 해제된다(SL SD20). Level을 내릴 때는 LevelScript가 먼저다(LD8) |
| fault로 인한 격리 | Disable 없이 실행 정지. 파괴될 때 Destroy만 1회 실행(SL 8.3절) |

### 3.5 Actor 사이에 주고받는 방법

| 하고 싶은 일 | 방법 | 예 |
|---|---|---|
| 같은 Actor의 다른 component 찾기 | component에서는 `Owner.Find<T>()`, Actor 파생 클래스(LevelScript 포함)에서는 `Find<T>()`. 결과는 `T?` | `let bag = Owner.Find<Inventory>() ?? return;` |
| 지정한 Actor의 함수 호출 | `@edit` 참조 + `if (let)` | `if (let door = Target) { door.Open(player); }` |
| 지정한 Actor의 이벤트 받기 | `on 필드.이벤트()`. 필드 값이 바뀌면 자동으로 다시 연결 | `on Target.Opened() { ... }` |
| 지정한 Actor의 트리거·충돌 받기 | 엔진 Actor 이벤트 `TriggerEntered`·`TriggerExited`·`CollisionEntered`·`CollisionExited`(LD9) | `on GateTrigger.TriggerEntered(other: Actor) { ... }` |
| 여러 Actor의 같은 이벤트 | 배열 필드. 첫 인자가 발행한 원소 | `on Guards.Died(guard: Guard) { ... }` |
| 아무 Actor에나 붙는 이벤트 | Actor 확장 이벤트 `event Actor.X` | `emit target.Interacted(player);` |
| 대상을 모르는 알림(레벨 사이, UI) | World 이벤트 `event World.X`(LD10) | `emit World.GateOpened();` |
| 게임 전체 공유 상태 | `World.Service<T>()` | `World.Service<Score>().Add(10);` |

### 3.6 예제: 트리거에 들어가면 문이 열린다

같은 일을 component 방식과 레벨 스크립트 방식으로 쓴다. 문 클래스는 둘이 같이 쓴다.

~~~text
// Props/Door.spark: 직접 배치하는 Actor 클래스
class Door : Actor {
    @edit var OpenAngle: float = 110.0;      // 도
    @save var IsOpen: bool;
    event Opened(by: Actor?);

    fn Open(by: Actor?) {
        if (IsOpen) { return; }
        IsOpen = true;
        emit Opened(by);
    }
}
~~~

(가) 트리거 Actor에 붙인 component — 재사용하는 동작에 맞다. 트리거 Actor를 고르고 "컴포넌트 추가"로 붙인 뒤 `Target`에 문을 지정한다. 물리 hook은 `ScriptableComponent`에도 전달된다(SL 2.4·5.1절).

~~~text
// Props/OpenDoorOnEnter.spark
import Props.Door;
import Characters.PlayerCharacter;

class OpenDoorOnEnter : ScriptableComponent {
    @edit var Target: Door?;   // Details에서 지정

    on TriggerEnter(other: Actor) {             // 인자는 앞에서부터 필요한 만큼(SL 5.2절)
        if (let player = other as? PlayerCharacter) {
            if (let door = Target) {
                door.Open(player);
            }
        }
    }
}
~~~

(나) 레벨 스크립트 — 이 레벨에만 있는 연결에 맞다. 트리거에 스크립트를 붙이지 않고 연결을 레벨 스크립트 한 파일에 모은다.

~~~text
// Levels/Stage1.spark
import Props.Door;
import Characters.PlayerCharacter;
import Events.StageEvents;

class Stage1 : LevelScript {
    @edit var GateTrigger: Actor?;              // LevelScript의 객체 필드는 T?만(LD7)
    @edit var GateDoor: Door?;

    on Start() {
        Log.Info("Stage1 started");
    }

    on GateTrigger.TriggerEntered(other: Actor) {   // 엔진 Actor 물리 이벤트(LD9)
        if (let player = other as? PlayerCharacter) {
            if (let door = GateDoor) {
                door.Open(player);
            }
        }
    }

    on GateDoor.Opened() {
        emit World.GateOpened();                // 레벨 사이 알림(LD10)
    }
}

// Events/StageEvents.spark
event World.GateOpened();
~~~

- (나)의 `on GateTrigger.TriggerEntered`는 다른 Actor의 트리거 통지를 이벤트로 받는다. 물리 hook `TriggerEnter`는 자기 Actor 안에서만 받는 hook이고 hook 이름은 멤버 이름으로 쓸 수 없으므로(SL 3.7절 부록 A) 엔진 Actor 이벤트는 과거형 이름을 쓴다(LD9).
- LevelScript는 Actor 파생이라 `Owner`가 없고 자기 Actor의 component는 `Find<T>()`로 찾는다(SL 3.4절).
- 네트워크 게임에서 서버만 할 일은 `if (HasAuthority) { … }`로 감싸거나 `@authorityOnly` 이벤트로 받는다(LD12, SL 14절).

## 4. LevelScript 설계

### 4.1 모양

~~~cpp
// World 모듈(WD). 필드 이름·지정자는 ER 4.6절 표기를 따른다.
class Level : public Object {          // ROCKS_CLASS(WorldOnly)
    ROCKS_PROPERTY(ReadOnly, Required) ObjectRefArray<Actor> actors;   // LevelScript Actor도 여기 있다(LD4)
    ROCKS_PROPERTY(ReadOnly) ObjectRef<LevelScript> levelScript;       // actors 안의 그 Actor. 없으면 null
};

class LevelScript : public Actor {     // ROCKS_CLASS(WorldOnly, NotPlaceable)
    // 물리 hook 가상 함수(OnTriggerEnter·OnTriggerExit·OnCollisionEnter·OnCollisionExit)는 final로 막는다(LD18).
    // C++ 파생을 허용한다 — 프로젝트 공통 기반(LD20).
};

// 편집 World 전용. 기존 LevelScript가 있으면 간선을 끊고 파괴한 뒤 새 클래스로 만든다. null이면 제거만 한다.
Result<ObjectHandle> World::SetLevelScript(ObjectHandle level, ClassRef<LevelScript> scriptClass) noexcept;
~~~

- **`levelScript`는 ReadOnly PROPERTY다**(ER 4.6절 — World 필드는 모두 ReadOnly, 로드의 staged 쓰기만 허용). 값은 World API `World::SetLevelScript`로만 바꾼다. 이름은 ER 4.6절의 `SetRootComponent`·`SetRelativeTransform`과 같은 꼴이다. UE는 `ULevel::RegenerateLevelScriptActor`가 `SpawnActor`로 새 LSA를 만들고 옛 것을 지운다(`ENG/Private/Level.cpp:5095-5141`). Rocks도 "클래스를 주면 World가 만들고 가리킨다"는 같은 모양이다. 게임 World에서 부르면 `Unsupported`다(LevelScript는 레벨 데이터다).
- `World::SpawnActor`·`SpawnPrefab`은 LevelScript 파생 클래스를 `InvalidArgument`(진단 `LevelScriptSpawnRejected`)로 거부한다. `NotPlaceable` 자체는 편집기 배치 목록 표지일 뿐 실행 중 spawn을 막지 않으므로(UE와 같다) LevelScript의 "레벨당 0 또는 1"(LD5)을 지키려고 World가 따로 막는다.
- **Sparks에서는** `class Stage1 : LevelScript`로 쓴다. Actor 파생이므로 문맥 이름은 `World`·`Session`·`Input`·`Time`이고 자기 자신은 `this`다. `Owner`는 없다(SL 3.4절). hook·`@edit`·`@save`·`@net`·`let` component를 일반 Actor와 똑같이 쓴다. 로직이 커지면 `let`으로 `ScriptableComponent` 여러 개에 나눈다(LD5).
- **core 프로필에 포함한다**(LD13). `LevelScript`는 SL 3.7절의 핵심 엔진 타입(언어 버전 1에 고정)이다.
- **한 클래스를 여러 레벨이 쓸 수 있다**(LD19). 레벨마다 다른 값은 `@edit` 필드로 준다.
- **Outliner·Details**: LevelScript 타입은 Outliner의 일반 Actor 줄에 보이지 않고 Level마다 "레벨 스크립트" 항목 하나로만 노출된다. 그 항목을 고르면 Details에서 `@edit` 값을 편집한다. 이름표 편집·복사·붙여넣기·복제·삭제·이동 명령은 받지 않는다(제거는 "레벨 스크립트 제거" = `SetLevelScript(level, null)`). 새 CLASS 지정자를 만들지 않고 편집기 규칙으로 둔다(UE는 `bEditable = false`·`bActorLabelEditable = false` — 2.2절). 콘텐츠 배치 목록에서 빠지는 것은 UT 7.3절 (e) `NotPlaceable`이 맡는다(LD3).
- LevelScript는 root component가 없어도 된다(Transform이 의미 없다). 뷰포트에 기즈모를 표시하지 않는다.

### 4.2 저장과 로드

- LevelScript Actor는 레벨에 직접 배치한 Actor처럼 `Level.actors`에 저장된다. `@edit` 값과 참조도 같은 Actor Record 경로를 탄다. prefab에서 펼친 Actor용 Transient 목록 `spawnedActors`(PF D15)에는 들어가지 않는다.
- **선행 조건**: Sparks 클래스 Actor를 레벨에 저장하려면 스크립트 필드의 동적 schema 등록([Sparks 엔진 연동 요구 목록](SparksEngineIntegration.ko.md) SE-O3)이 먼저다. 지금 Level 그래프 저장은 생성 타입만 받는다(ER 4.6절 "Level 그래프 저장은 구조 시험용").
- **저장 검증**(레벨 패키지 단계에서 레벨 저장·쿡이 한다, 진단은 4.9절):
  1. `levelScript`가 null이 아니면 `actors`의 원소여야 한다(`LevelScriptNotInLevel`).
  2. `actors` 안의 LevelScript 파생 Actor는 0개 또는 1개이고, 1개면 그것이 `levelScript`여야 한다(`DuplicateLevelScript`).
  3. 레벨에 저장하는 참조 규칙(4.5절 — `CrossLevelReference`, PF D22).
  4. LevelScript는 Collider component를 갖지 않는다(`LevelScriptCollider`, LD18).
- **cooked 로드 검증**: 1·2·4를 cooked 레벨 로드도 다시 검사하며 어긋나면 `CorruptPackage`로 레벨 로드를 실패한다. 출시 빌드도 끄지 않는다(SL SD7). UE는 감지·오류 로그·편집기 복구만 하고 저장을 막지 않으므로(2.2절) Rocks가 더 엄격하다.
- 편입 단계(ER 4.6절 레벨 편입 계약)는 `levelScript`를 연결한 뒤 4.3절 순서로 수명 단계를 진행한다.

### 4.3 수명 순서(LD8)

| 시점 | 규칙 |
|---|---|
| 편입(Awake·Enable) | 그 Level의 모든 배치 Actor(직접 배치와 prefab 배치에서 펼친 Actor)가 `FinishSpawning`(OnRegister → Actor OnAwake → Component OnAwake → OnEnable)을 마친 뒤 LevelScript의 `FinishSpawning`을 한다. 그래서 LevelScript의 `on Enable`에서 참조 대상이 이미 활성이라는 것이 결정적으로 보장된다. 저장 때 `enabled`가 거짓인 Actor는 이 "이미 활성" 보장에서 빠진다(Awake만 끝난 상태) |
| Start | 엔진 시작 큐 순서(ER 7.2절)를 따른다. LevelScript가 마지막에 큐에 들어가므로, 같은 프레임에 Start할 준비가 된(`IsReadyToStart`) 활성 배치 Actor들의 Start 뒤에 LevelScript의 Start를 시작한다. 준비가 안 된 Actor나 꺼진 채 기다리는 Actor(`startDeferred`)의 Start는 기다리지 않는다. Start는 시작 순서만 뒤이고 다른 Actor의 Start가 끝나기(준비 대기 — SL SD12)를 기다리지 않는다. 다른 Actor가 준비됐다는 사실이 필요하면 그 Actor의 이벤트로 받는다 |
| 언로드(`DestroyLevel`·World 종료·travel) | LevelScript의 Disable·Destroy가 먼저, 그다음 배치 Actor의 Disable·Destroy다. 레벨 스크립트가 정리 중에 대상 Actor를 읽어도 대상은 아직 살아 있다 |

- 이 순서는 UE를 따른 것이 아니다. UE는 LSA를 특별 취급하지 않아 배열 위치가 순서를 정한다(2.3절).
- Level의 `actors_`는 순서를 보장하지 않는다(ER 7.2절 — 파괴가 끝 원소와 바꿔 떼기). 그래서 "일반 Actor와 같은 배열 순서"(LD8 (나))는 결정적이지 않다.
- 같은 Level 안에서 LevelScript보다 늦게 활성화되는 Actor(실행 중 spawn, 나중에 켠 Actor)는 이 보장 밖이다. 그런 대상은 `T?` 참조와 그 Actor의 이벤트로 다룬다.

### 4.4 파괴와 `levelScript` 간선(LD16)

- LevelScript는 `World::DestroyActor`로 파괴할 수 없다. `InvalidArgument`(진단 `LevelScriptDestroyRejected`)다. Sparks의 `RequestDestroy()`도 같은 규칙이다: 스크립트 단계 끝의 적용(SL 4.3절)에서 거부되며, 그 요청은 무시하고 위치별로 1회 보고한다(fault 아님 — 권한 없는 `@net` 쓰기와 같은 처리, SL 14절).
- LevelScript를 파괴하는 길은 `DestroyLevel`(World 종료·travel 포함)과 편집 World의 `SetLevelScript`뿐이다. 둘 다 파괴 대기에 넣기 전에 `levelScript` 간선을 먼저 끊는다. 그래서 파괴 요청된 Actor를 강한 간선이 붙잡아 회수되지 않는 문제(ER 7.4절 — "DestroyRequested여도 강한 간선이 붙잡으면 회수되지 않는다")가 생기지 않는다.
- 엔진 내부 예외 셋은 같은 규칙(간선을 먼저 끊는다)으로 처리한다: `OnAwake` 실패(ER 7.2절 — 그 Level은 이번 실행 동안 LevelScript 없이 돈다), Registry 우회 파괴의 복구(ER 7.4절 `RepairBypassedObjects` — 계약 위반이므로 `LevelScriptDestroyRejected`를 Error로 기록하고 간선을 끊은 뒤 정리한다), 핫 리로드 R3 재생성(새 instance로 간선을 바꿔 단다).
- fault 격리(SL 8.3절)는 파괴가 아니므로 간선을 유지한다. 격리된 LevelScript는 그 Level이 내려갈 때 Destroy를 1회 받는다.

### 4.5 참조 규칙(LD6·LD7·LD17)

- **필드 형태**: LevelScript의 `@edit` 객체 필드는 `T?`만 허용한다(LD7). LevelScript는 SL 3.2절 "`?` 없는 객체 필드" 허용 목록 (b) "prefab 안의 대상을 가리키는 `@edit` 필드"에 해당하지 않는다(prefab에 들어갈 수 없고, 대상은 레벨 Actor다). 위반은 Sparks 컴파일 오류 S0413이다. 레벨 Actor는 게임 중에 흔히 파괴되므로(적 처치, 부서지는 문) 필수 참조로 두면 참조 하나의 파괴가 LevelScript 전체를 격리시킨다.
- **대상**: 같은 Level에 배치된 Actor와 그 component다. 다른 Level의 객체를 지정하면 저장을 거부한다(`CrossLevelReference`). 레벨 사이 연락은 World 이벤트로 한다(LD10). 편집기의 "참조 추가"도 같은 Level의 Actor에만 켜진다(UE `OnGraphEditorDropActor`와 같다 — 2.2절).
- **World Partition 셀**: 셀 Actor를 가리키는 참조는 일반 규칙을 그대로 따른다 — 영속 Entity 참조(`SoftEntityId`)가 생기기 전에는 셀 사이 참조 저장을 거부하고(SL 4.3절, PF D12), 생긴 뒤에는 그 값으로 저장해 셀이 올라오면 해석된다(SL 7.6절). LevelScript만의 예외를 두지 않는다(6절 선행 조건).
- **prefab 배치가 펼친 Actor**(LD17): 펼친 Actor는 Transient `spawnedActors`(PF D15)에 있어 레벨 저장에 들어가지 않으므로 그 handle을 직접 저장할 수 없다. 레벨에 저장하는 weak 참조가 펼친 노드를 가리키면 Record에는 null로 쓰고 Level의 배치 노드 참조 표에 (배치 `instanceGuid` + `PrefabNodeAddress`)로 둔다. 로드에서 배치를 펼친 뒤 PF D6의 파생 ID 규칙으로 해석해 staged 경로 쓰기로 채운다. LevelScript만의 규칙이 아니라 레벨 저장 참조 일반 규칙이며 정본은 PF D22다. 영속 Entity Guid가 생기면 그 표현으로 바꾼다.

### 4.6 물리(LD9·LD18)

- **엔진 Actor 물리 이벤트**(LD9): 엔진 `Actor`가 이벤트 `TriggerEntered(other: Actor, trigger: Collider)`·`TriggerExited`·`CollisionEntered(other: Actor, hit: Hit)`·`CollisionExited`를 노출한다. 인자는 같은 이름 hook(`TriggerEnter` 등 — SL 5.1절)과 같다. 다른 객체(LevelScript, 다른 Actor의 component)가 `on X.TriggerEntered(other)`로 받는다.
  - 발행 시점: 같은 PostPhysics 물리 이벤트 batch(SL 5.4절 5단계)에서 그 Actor의 물리 hook을 실행한 다음 발행한다. 발행은 일반 `emit`처럼 큐에 등록되어 같은 스크립트 단계 안에서 전달된다.
  - 비용: Actor마다 delegate 필드를 두지 않는다. SL 5.5절의 지연 할당 이벤트 slot 표에 연결을 두므로 구독이 없는 Actor는 저장소도 발행 비용도 없다(1M Actor 기준 — 물리 bridge는 slot 표가 없으면 바로 건너뛴다).
  - 이름은 과거형이다. hook 이름·`On`+hook·Unity hook 이름은 수명 클래스와 기반 타입의 멤버 이름으로 쓸 수 없기 때문이다(SL 3.1·3.7절).
- **LevelScript의 물리 금지**(LD18): LevelScript는 물리 hook(`on TriggerEnter` 등)을 선언할 수 없고 Collider component를 가질 수 없다. 트리거가 필요하면 트리거 Actor를 배치하고 그 이벤트를 받는다(3.6절 (나)).
  - Sparks: 물리 hook 선언은 S0801, `let` Collider component 필드는 S0802 컴파일 오류다. 실행 중 추가(full)는 `AddComponent`가 거부한다.
  - C++ 파생: 물리 hook 가상 함수는 `LevelScript`에서 `final`이라 재정의가 C++ 컴파일 오류다. Collider subobject 선언은 타입 등록이 거부하고, `World::AddComponent`의 Collider 추가는 `InvalidArgument`, 레벨 저장 검증은 `LevelScriptCollider`다.
  - 근거: UE LSA도 충돌·겹침 이벤트 재정의를 숨기고 Collision 범주를 숨긴다(`LevelScriptActor.h:24` `KismetHideOverrides`·`HideCategories` — 2.2절). LevelScript에 Transform 의미가 없으므로 위치를 갖는 충돌체는 오해만 만든다.

### 4.7 스트리밍·네트워크·핫 리로드·편집 World

| 주제 | 규칙 |
|---|---|
| 영구 레벨·스트리밍 서브 레벨 | 각자 0 또는 1개의 LevelScript를 가진다(LD11). 서브 레벨이 내려가면(`DestroyLevel`) 셀 unload와 같은 정상 종료(Disable → Destroy → Detach)를 타고, 4.3절대로 LevelScript가 먼저다. SL SD20 종료 경로 표와 EIP 20.2절 종료 경로 목록에 서브 레벨 unload를 더한다 |
| World Partition 셀 | 셀은 LevelScript를 갖지 않는다. 메인 레벨의 LevelScript가 셀 Actor를 `T?`로 참조한다(UE도 WP 변환 때 메인 레벨에만 남긴다 — 2.2절). 셀 참조 저장은 4.5절 일반 규칙 |
| Level Instance·prefab | 1차에서는 둘 수 없다(LD11). prefab 본문의 LevelScript 파생 노드는 PF `PrefabUnsaveableNode`로 거부한다. Level Instance(PF D1 (c))를 도입할 때 다시 본다. UE는 Level Instance에 LSA를 허용하고 break만 막으므로 Rocks가 더 엄격하다 |
| 네트워크 | 레벨과 함께 모든 process에 생성되고 항상 관련이다(LD12, UE `bAlwaysRelevant`). 배치 Actor의 네트워크 정체성(서로 대응시키는 ID)은 네트워크 단계가 정하는 배치 Actor 규칙을 따른다 — 지금 EIP 17.1절 NetObjectId는 실행 중 발급뿐이고 영속 Entity Guid가 없다(6절 선행 조건). `@net`·`@rpc`는 Actor 규칙 그대로다. 서버만 할 일은 `HasAuthority` 분기·`@authorityOnly` 이벤트로 한다(SL 14절). 전용 서버에서도 같은 경로로 돈다 |
| 핫 리로드 | 본문 수정은 R1, 필드 추가·변경은 R2, 기반 타입·`let` 구성 변경은 R3(재생성, 필드 값 보존)이다. 일반 Actor와 같고, R3은 `levelScript` 간선을 새 instance로 바꿔 단다(4.4절) |
| 편집 World | 게임 hook은 실행하지 않고 등록과 `Validate`(full)만 실행한다(PF D18) |
| Tryout | 편집 World 복제가 `levelScript` 간선과 `@edit` 참조를 복제본 안으로 다시 연결한다. 이벤트 연결은 실행 중 session이 instance의 Enable에서 만들므로(SL 5.5절) 복제본이 편집기 객체에 결합하는 일이 구조적으로 없다(UE `FixLevelScriptActorBindings`의 패키지 가드가 막는 문제 — 2.2절) |

### 4.8 편집기 진입(LD14·LD15·LD20)

**명령**(EIP 22.1절 편집기 backend 명령, transaction 하나):

- **레벨 스크립트 열기**: Level에 LevelScript가 없으면 아래 규칙의 경로에 템플릿 파일을 만들고(이미 그 경로에 LevelScript 파생 클래스가 있으면 그 파일을 쓴다 — LD19), 컴파일이 끝났다는 통지를 받은 뒤 `World::SetLevelScript`로 instance를 만들고 VS Code(또는 Rocks Studio Script Editor)로 연다. 있으면 그 클래스 파일을 연다.
- **레벨 스크립트에 참조 추가**: Outliner에서 Actor를 우클릭해 고르면 언어 서버가 `@edit var <이름>: <타입>?;` 줄을 클래스에 넣고, 편집기가 새 revision 컴파일 완료 통지를 받은 뒤 그 필드에 해당 Actor를 지정한다. `<이름>`은 Actor 표시 이름을 아래 이름 규칙으로 바꾼 것이며 기존 멤버와 겹치면 아래 생성 경로의 충돌 규칙대로 번호 접미(`Door2`)를 붙인다. `<타입>`은 Actor 클래스이고 Sparks에 노출되지 않은 클래스면 노출된 가장 가까운 기반이다. 같은 Level의 Actor에만 켜진다(LD6). UE의 "레벨 BP에 액터 참조 추가"에 해당한다.
- **이벤트 추가**: 같은 메뉴에서 그 Actor 필드 타입의 이벤트(엔진 Actor 물리 이벤트 포함)를 고르면 `on <필드>.<이벤트>(<인자>) { }` 틀을 넣는다. 필드가 없으면 참조 추가를 먼저 한다.
- **레벨 스크립트 제거**: `World::SetLevelScript(level, null)`. 파일은 지우지 않는다.
- **배치 Actor에 컴포넌트 추가**(LD15): 직접 배치 Actor는 `components`에 추가해 레벨에 저장하고, prefab 배치는 override "노드 추가"로 저장한다(PF4 선행). core에서도 된다(실행 중 추가가 full 전용인 것과 다른 일이다 — SL 4.5절).

**언어 서버 채널**(SL 13절): 편집기가 `sparks-lsp`에 LSP `workspace/executeCommand`(서버 명령 `sparks.levelScript.addReference`·`sparks.levelScript.addEvent`)로 요청하면, 서버가 소스 위치를 정해 `workspace/applyEdit`로 VS Code·Script Editor에 편집을 보낸다. 서버가 파일을 직접 쓰지 않으므로 열린 편집기의 미저장 내용과 충돌하지 않는다. 편집이 저장되면 일반 증분 컴파일 경로를 탄다. 편집기는 시간 대기 없이 컴파일 완료 통지(revision 번호)로 다음 단계를 진행한다.

**생성 경로**: `<패키지>/Levels/<레벨 경로 이름>.spark`.

- `<패키지>`는 그 레벨의 콘텐츠 폴더를 소유한 Sparks 패키지이고, 없으면 프로젝트의 기본 스크립트 패키지(Studio "새 스크립트"가 쓰는 패키지 — SL 1.3절)다.
- `<레벨 경로 이름>`은 레벨 에셋의 콘텐츠 루트 기준 경로(폴더 + 이름, 확장자 제외)를 마디마다 Sparks 경로 이름 규칙(SL 3.1절 — 대문자로 시작하는 ASCII 영문자·숫자, `_` 없음, SL 3.4절)으로 바꾼 것이다. 바꾸는 규칙:
  1. 마디를 ASCII 영문자·숫자가 아닌 문자(`_`·`-`·공백·`.` 등)에서 나눈다.
  2. 조각마다 첫 글자를 대문자로 바꾸고 이어 붙인다(`stage_01` → `Stage01`, `forest-night` → `ForestNight`).
  3. 결과가 숫자로 시작하면 `Level`을 앞에 붙인다(`01_Intro` → `Level01Intro`).
  4. ASCII가 아닌 글자(한글 등)는 버린다. 버린 글자가 있거나 결과가 비면 명령이 이름 입력 창을 띄우고 변환 결과를 초안으로 채운다(사용자가 고친 이름도 같은 규칙으로 검사한다).
- **충돌**: 경로 비교는 대소문자를 무시한다(SL 3.4절 — 대소문자만 다른 두 경로 금지, `X.spark`와 폴더 `X/` 공존 금지). 같은 경로에 LevelScript 파생이 아닌 파일이나 폴더가 있으면 마지막 마디에 2부터 가장 작은 빈 번호를 접미로 붙인다. 이름이 숫자로 끝나면 사이에 `V`를 넣어 숫자가 섞이지 않게 한다(`Forest` → `Forest2`, `Stage01` → `Stage01V2`). 참조 추가의 필드 이름 접미도 같은 규칙이다. 클래스 이름은 마지막 마디와 같다. 같은 패키지의 다른 최상위 선언과 이름이 겹치면 같은 접미 규칙을 클래스 이름에도 적용한다.

**템플릿**(EIP 22.3절 기본 카탈로그, Copy 방식): 기반 클래스는 프로젝트 설정 `DefaultLevelScriptClass`(`ClassRef<LevelScript>`, 기본값 `LevelScript`)로 고른다(LD20, UE `LevelScriptActorClass` 대응 — 2.2절). core 패키지에서는 Sparks 클래스를 기반으로 쓸 수 없으므로(SL SD27) 프로젝트 공통 기반은 C++ LevelScript 파생으로 둔다. 설정이 Sparks 클래스를 가리키는데 대상 패키지가 core면 명령이 그 이유와 대안(C++ 기반 또는 full 패키지)을 보이고 멈춘다. Sparks 클래스끼리 상속은 full의 `open class`로 한다.

~~~text
// 템플릿 결과 예: Levels/Stage01.spark
class Stage01 : LevelScript {
    on Start() {
    }
}
~~~

### 4.9 진단

로그·진단 문구는 영어다. `ErrorCode`는 기존 것을 쓰고 원인은 진단 이름으로 구분한다(AS 4.6절 형식, PF 3.11절과 같은 꼴).

| 진단 | ErrorCode | 때 | 등록처 |
|---|---|---|---|
| `DuplicateLevelScript` | 저장·쿡: InvalidArgument, cooked 로드: CorruptPackage | `actors`에 LevelScript 파생 Actor가 `levelScript` 말고 또 있다(둘 이상, 또는 `levelScript`가 null인데 하나) | AS 4.6절 |
| `LevelScriptNotInLevel` | 같음 | `levelScript`가 `actors`의 원소가 아니다 | AS 4.6절 |
| `CrossLevelReference` | 저장·쿡: InvalidArgument | 레벨에 저장하는 참조가 다른 Level의 객체, 또는 (영속 Entity 참조 전) 다른 셀의 객체를 가리킨다 | AS 4.6절 |
| `NotPlaceableClass` | InvalidArgument | 편집기 배치 명령(콘텐츠 끌어 놓기·배치 목록)이 `NotPlaceable` 클래스를 받았다 | AS 4.6절 |
| `LevelScriptCollider` | 등록·저장·`AddComponent`: InvalidArgument, cooked 로드: CorruptPackage | LevelScript가 Collider component를 가진다 | AS 4.6절 |
| `LevelScriptSpawnRejected` | InvalidArgument | `World::SpawnActor`·`SpawnPrefab`에 LevelScript 파생 클래스 | World 로그 |
| `LevelScriptDestroyRejected` | InvalidArgument(World API)·Sparks는 무시 후 보고·우회 파괴 복구는 Error 기록 | LevelScript 파괴 요청(4.4절) | World 로그 |
| `PrefabUnsaveableNode`(재사용) | Unsupported | prefab 본문에 LevelScript 파생 노드 | PF 3.11절 |
| `PrefabPlacementReferenceOrphaned`(새) | 편집기: 진단만(null), cooked: InvalidArgument | 배치 노드 참조의 대상 노드가 없다(PF D22) | PF 3.11절 |
| S0413 | Sparks 컴파일 오류 | LevelScript의 `@edit` 객체 필드에 `?`가 없다 | SL 8.1절 |
| S0801 | Sparks 컴파일 오류 | LevelScript가 물리 hook을 선언했다 | SL 8.1절 |
| S0802 | Sparks 컴파일 오류 | LevelScript가 `let` Collider component 필드를 선언했다 | SL 8.1절 |

S 코드 메시지 예:

~~~text
Levels/Stage1.spark:5:21: error S0413: Object fields of a LevelScript must be optional.
    @edit var GateDoor: Door;
                        ^^^^
  Level actors can be destroyed during play, so a LevelScript reads them through 'T?'.
  fix: @edit var GateDoor: Door?;

Levels/Stage1.spark:12:5: error S0801: A LevelScript cannot declare physics hooks.
    on TriggerEnter(other: Actor) {
    ^^^^^^^^^^^^^^^
  A LevelScript has no colliders. Place a trigger actor and handle its event.
  fix: on GateTrigger.TriggerEntered(other: Actor) { … }
~~~

## 5. 결정 항목

2026-11-20 사용자 확정: 모든 항목을 권장안으로 정한다. 표마다 굵은 줄이 확정안이다. 부수 효과는 그 선택지를 고르면 다른 설계·문서·작업이 어떻게 달라지는지를 적었다.

**LD1. Blueprint를 어떻게 할까**

| 선택지 | 부수 효과 |
|---|---|
| **(가) 제외 유지 + LevelScript 추가 — 확정** | 실행 체계(언어·VM·디버거·핫 리로드·복제·AOT)가 하나로 유지된다. 비프로그래머가 들어올 수 있느냐는 Sparks 품질(언어 서버·진단·템플릿·4.8절 편집기 진입)에 달린다. UE 사용자는 노드 대신 코드로 옮겨 가는 비용을 치른다. EIP 20.4·23절에 Level Blueprint 대응을 기록한다 |
| (나) BP를 먼저 지원하고 나중에 제거 | 그래프 편집기·컴파일러·VM·디버거·핫 리로드·복제·쿡·AOT를 BP용으로 한 벌 더 만든다. 엔진 API 바인딩과 리플렉션 메타데이터가 두 언어를 섬겨야 한다. BP로 만든 사용자 에셋은 제거할 때 Sparks로 옮겨야 한다. UI 툴킷·에디터가 먼저 있어야 해서 Sparks L0보다 늦게 나오므로 임시 수단 역할도 못 한다. EIP·RCM·UT·PF의 "BP 제외" 문장 수십 곳을 고쳐야 한다 |

확정 이유: UE 자신이 BP에서 Verse로 옮겨 가는 중이고, 넣었다 빼는 비용이 처음부터 안 넣는 비용보다 크다.

**LD2. 노드 기반 시각 편집이 필요해지면**

| 선택지 | 부수 효과 |
|---|---|
| **(가) Sparks 시각 편집기를 보류 선택지로만 기록 — 확정** | 지금 할 일은 없다. Sparks L1–L2와 UI 툴킷 Designer 뒤에 필요한지 판단한다. 만들게 되면 그래프를 Sparks 코드로 내보내고 실행은 Sparks가 맡으므로 런타임은 하나로 유지된다. 나중에 접어도 생성된 코드가 남는다 |
| (나) 지금 스펙 착수 | 언어가 확정되기 전에 그래프와 코드의 왕복 규칙이 문법을 묶는다. 계획 순서에 없는 작업이다 |
| (다) 영구 제외 | 비프로그래머용 다른 경로가 영원히 없다. 판단 근거(실사용 경험)가 생기기 전에 문을 닫는다 |

**LD3. 레벨 스크립트를 무엇으로 띄울까**

| 선택지 | 부수 효과 |
|---|---|
| **(가) 레벨마다 숨은 Actor 하나 — 확정** | tick·`@net`·RPC·`let` component·hook·저장·핫 리로드 R3를 Actor 경로 그대로 쓴다. 배치 불가는 UT 7.3절 (e) CLASS 지정자 `NotPlaceable`을 재사용하고(UD15 중 이 지정자만 이 결정으로 확정), Outliner 숨김은 새 지정자 없이 편집기 규칙으로 둔다(4.1절). 레벨마다 Actor가 하나 늘지만 무시할 수준이다 |
| (나) Level이 소유하는 Actor 아닌 Object(WorldService식) | tick 등록·복제·저장·hook 호스트를 새로 만들어야 한다. Transform이 없는 점은 깔끔하지만 component를 붙일 수 없어 로직을 나눌 방법이 줄어든다 |
| (다) 새 타입 없이 관례만(빈 Actor + ScriptableComponent, Unity식) | 엔진·언어 변경이 없다. 대신 레벨 로직이 어디 있는지 정해진 곳이 없고, 여러 개로 흩어지며, 다른 Actor보다 늦게 활성화된다는 순서 보장도 없다 |

**LD4. Level과 어떻게 이어 저장할까**

| 선택지 | 부수 효과 |
|---|---|
| **(가) `actors`에 일반 Actor로 저장하고 `Level.levelScript`가 가리킴 — 확정** | `@edit` 값·참조·override·undo가 Actor Record 경로를 그대로 쓴다. ER 4.6절 Level 필드 표에 `levelScript`(ReadOnly PROPERTY, World API `SetLevelScript`로만 설정)를 더하고 레벨 저장 검증에 "`actors` 원소인지, 하나뿐인지"를 넣는다(UE `Level.h:488-490`과 같은 모양) |
| (나) Level에 클래스 참조와 값 표만 두고 로드 때 생성 | 값을 저장하는 형식·override·undo 경로를 따로 만들어야 한다. Actor Record와 두 갈래가 된다 |
| (다) 레벨 에셋 안에 스크립트 소스를 내장(UE LevelScriptBlueprint식) | "파일 경로 = import 경로" 규칙과 패키지 경계가 깨진다. 언어 서버가 다루기 어렵고 VCS diff가 레벨 바이너리에 섞인다. 쿡에서 소스를 빼는 처리도 필요하다 |

**LD5. 레벨당 몇 개까지**

| 선택지 | 부수 효과 |
|---|---|
| **(가) 0 또는 1 — 확정** | 찾기 쉽고 순서가 결정적이다. 로직이 커지면 LevelScript의 `let` ScriptableComponent로 나눈다. 저장과 cooked 로드 모두 검증한다(SL SD7, 4.2절). UE는 감지·오류 로그·편집기 복구뿐이고 저장을 막지 않으므로(2.2절) Rocks가 더 엄격하다. 실행 중 spawn으로 둘째가 생기지 않게 World가 LevelScript 파생 클래스의 spawn을 거부한다 |
| (나) 여러 개 목록 | 스크립트끼리의 시작 순서와 서로 참조하는 규칙을 따로 정해야 한다. 협업 때 파일이 나뉘는 장점은 (가)의 component 분할로도 얻는다 |

**LD6. 무엇을 참조할 수 있나**

| 선택지 | 부수 효과 |
|---|---|
| **(가) 같은 Level의 배치 Actor와 그 component만, 다른 Level은 저장 거부 — 확정** | UE 편집기 규칙(같은 레벨 Actor만 — `BlueprintEditor.cpp:4081-4092`)과 같다. 레벨 사이 연락은 World 이벤트(LD10)로 한다. 저장 검증에 "대상 Level 확인"(`CrossLevelReference`)이 들어간다. WP 셀 Actor 참조는 영속 Entity 참조가 생기기 전까지 셀 사이 참조 저장 거부라는 일반 규칙(SL 4.3절, PF D12)을 그대로 따른다 — 6절 선행 조건 |
| (나) 다른 Level의 Actor도 `SoftEntityId`로 허용 | 대상 레벨이 안 올라와 있으면 늘 null이라 "왜 비어 있지" 같은 혼란이 생긴다. 동작이 레벨 로드 순서에 의존하게 된다 |

**LD7. 필수 참조(`?` 없는 `@edit`)를 허용할까**

| 선택지 | 부수 효과 |
|---|---|
| **(가) `T?`만 허용 — 확정** | `if (let)` 검사 코드가 늘어난다. 대신 대상이 파괴돼도 레벨 로직은 계속 돈다. SL 3.2절 "`?` 없는 객체 필드" 허용 목록은 바꾸지 않고, "그 밖" 목록에 LevelScript를 명시한다(허용 목록 (b)에 해당하지 않음). 위반은 S0413 |
| (나) 같은 Level 대상에 한해 허용, 대상이 없어지면 격리 | 코드는 짧아진다. 하지만 참조한 Actor 하나만 파괴돼도 LevelScript 전체가 격리되어(SL 8.3절) 진행 불가 버그가 된다. prefab 안 참조는 같은 파괴 묶음이라 안전하지만 레벨 Actor는 그렇지 않다 |

**LD8. 다른 Actor와의 수명 순서**

| 선택지 | 부수 효과 |
|---|---|
| **(가) 그 Level의 배치 Actor 뒤(4.3절) — 확정** | 편입에서 LevelScript의 `FinishSpawning`은 모든 배치 Actor의 `FinishSpawning` 뒤라서 Enable에서 참조 대상이 활성이라는 것이 결정적으로 보장된다(저장 때 꺼진 Actor 제외). Start는 같은 프레임에 준비된 활성 배치 Actor 뒤에 시작하고 `startDeferred`·준비 안 된 Actor는 기다리지 않는다. 언로드는 LevelScript의 Disable·Destroy가 먼저다. ER 4.6절 편입 6항·7.2절과 SL 5.1절에 순서 규정을 더한다. UE는 순서를 보장하지 않으므로(2.3절) Rocks 고유 규칙이다 |
| (나) 일반 Actor와 같은 배열 순서 | Level `actors_` 순서 자체가 보장되지 않는다(ER 7.2절 — 파괴가 끝 원소와 바꿔 떼기). 저장·파괴 이력에 따라 동작이 바뀌고, 레벨을 다시 저장했을 뿐인데 버그가 생길 수 있다 |
| (다) 맨 먼저 | 대상들이 아직 Enable 전이라 Enable에서 상태를 반영할 수 없다. 대상 쪽 설정을 덮어쓸 위험도 있다 |

**LD9. 다른 Actor의 트리거·충돌을 어떻게 받나**

| 선택지 | 부수 효과 |
|---|---|
| **(가) 엔진 Actor가 `TriggerEntered(other: Actor, trigger: Collider)`·`TriggerExited`·`CollisionEntered(other: Actor, hit: Hit)`·`CollisionExited` 이벤트를 노출 — 확정** | LevelScript와 다른 component가 `on X.TriggerEntered(other)`로 바로 받는다(UE `OnActorBeginOverlap` 대응). 같은 물리 batch에서 hook 다음에 발행한다. Actor마다 delegate 필드를 두지 않고 SL 5.5절 지연 이벤트 slot 표를 써서 구독이 있을 때만 비용이 든다(1M Actor 기준). SL 5.2절 경로 표와 EIP 20.2절("자기 Entity의 모든 collider") 문구를 고친다. hook과 이름이 겹치지 않게 과거형으로 쓴다 |
| (나) 이벤트 없이 트리거에 중계 component를 붙임 | 엔진 변경이 없다. 대신 트리거마다 스크립트를 하나씩 붙여야 해서 연결을 레벨 스크립트 한 곳에 모은다는 의미가 절반으로 줄어든다 |

**LD10. 다른 레벨과 어떻게 연락하나**

| 선택지 | 부수 효과 |
|---|---|
| **(가) 기존 World 이벤트 `event World.X` — 확정** | 새 기능이 필요 없다. 이름과 인자를 컴파일 때 검사하고, 받는 레벨이 없으면 아무도 받지 않을 뿐이다(정상 동작). 이름 문자열 호출(UE `RemoteEvent`)은 두지 않는다 |
| (나) UE `RemoteEvent`처럼 이름 문자열로 호출 | 오타가 조용히 실패한다(UE는 bool만 돌려줌 — `LevelScriptActor.cpp:64-89`). Sparks의 방어적 원칙(검증 리터럴, 경고 없는 오류)과 맞지 않는다 |

**LD11. 서브 레벨·World Partition·Level Instance**

| 선택지 | 부수 효과 |
|---|---|
| **(가) 영구 레벨과 스트리밍 서브 레벨은 각자 0 또는 1, WP 셀·Level Instance·prefab은 불가 — 확정** | 서브 레벨 단위 연출이 가능하다. 서브 레벨 unload는 셀 unload와 같은 정상 종료 경로이며 SL SD20 종료 경로 표와 EIP 20.2절 목록에 행을 더한다. WP는 UE도 메인 레벨에만 남긴다. Level Instance는 UE가 허용하고 break만 막으므로(2.2절) Rocks가 더 엄격하며, Level Instance 도입 때(PF D1 (c)) 재검토한다 |
| (나) 영구 레벨만 | 규칙이 더 단순하다. 서브 레벨 단위 로직은 다시 Unity식 관례(LD3 (다))로 돌아간다 |

**LD12. 네트워크에서 어디에 있나**

| 선택지 | 부수 효과 |
|---|---|
| **(가) 모든 process에 존재, 항상 관련 — 확정** | 컷씬·UI 같은 로컬 연출도 레벨 스크립트에서 할 수 있다. UE LSA도 복제되고 항상 관련이다(`LevelScriptActor.cpp:32-33`). 배치 Actor의 네트워크 정체성은 네트워크 단계에서 정하는 배치 Actor 규칙을 따른다(선행 조건 — EIP 17.1절 NetObjectId는 실행 중 발급뿐, 영속 Entity Guid 없음). 서버만 할 일은 `HasAuthority` 분기·`@authorityOnly`로 한다. 전용 서버에서도 같은 경로로 돈다 |
| (나) 서버 전용 | 권한 실수가 없어 단순하다. 대신 클라이언트 연출이 다른 곳으로 흩어지고, 단독 실행과 접속 실행의 동작이 달라진다 |

**LD13. core 프로필에 넣을까**

| 선택지 | 부수 효과 |
|---|---|
| **(가) core에 포함 — 확정** | 비프로그래머가 레벨 연출을 바로 쓴다. 언어 버전 1에 고정된 핵심 엔진 타입 목록(SL 3.7절 부록 A)에 `LevelScript`를 더하므로 L0 착수 전에 문서에 넣는다(이번 개정) |
| (나) full 전용 | core 고정 목록을 건드리지 않는다. 대신 레벨 연출이 가장 필요한 사용자층이 쓸 수 없다 |

**LD14. 편집기 진입을 얼마나 도와줄까**

| 선택지 | 부수 효과 |
|---|---|
| **(가) "레벨 스크립트 열기"·"참조 추가"·"이벤트 추가" 명령(4.8절) — 확정** | UE의 레벨 BP 사용 흐름에 가장 가깝다. 생성 경로 `<패키지>/Levels/<레벨 경로 이름>.spark`와 이름 변환·충돌 규칙을 4.8절에 정했다. 편집기와 언어 서버 사이의 workspace edit 명령 채널을 SL 13절에 더하고, 템플릿을 EIP 22.3절 기본 카탈로그에 등록한다 |
| (나) 수동 작성·지정만 | 추가 작업이 없다. 대신 비프로그래머에게 파일 만들기·클래스 이름·필드 선언이 진입 장벽이 된다 |

**LD15. 배치된 Actor에 편집기에서 ScriptableComponent를 바로 붙일 수 있나**

| 선택지 | 부수 효과 |
|---|---|
| **(가) 직접 배치 Actor와 prefab 배치 모두 허용 — 확정** | UE "인스턴스에 컴포넌트 추가"와 같은 흐름이다. 직접 배치 Actor는 `components`에 추가해 레벨에 저장하고, prefab 배치는 override "노드 추가"(PF4 선행)로 저장한다. 지금 스펙에 암묵적인 부분을 SL 4.5절과 EIP 20.2절에 명시하고, 실행 중 추가가 full 전용인 점과 구분해 적는다 |
| (나) prefab·클래스 정의에서만 | 규칙이 하나로 줄어든다. 대신 한 번만 쓰는 로직도 prefab을 만들어야 해서 레벨 작업이 번거로워진다 |

**LD16. `levelScript` 간선과 파괴**

| 선택지 | 부수 효과 |
|---|---|
| **(가) `RequestDestroy`·`DestroyActor`로 파괴할 수 없음(오류), `DestroyLevel`과 편집 World `SetLevelScript`만 파괴하며 그 전에 간선을 끊음 — 확정** | 강한 간선 `levelScript` 때문에 파괴 요청된 Actor가 회수되지 않는 문제(ER 7.4절)가 생기지 않는다. 레벨 로직이 실수로 사라지지 않는다. fault 격리는 파괴가 아니므로 간선을 유지한다. 엔진 내부 예외(OnAwake 실패·우회 파괴 복구·R3 재생성)도 간선을 먼저 끊는다(4.4절) |
| (나) 일반 Actor처럼 파괴 허용, 파괴 정리가 간선을 끊음 | 파괴 정리에 "이 Actor를 가리키는 Level 필드" 처리를 더해야 하고, 파괴 뒤 그 레벨은 연결 로직을 잃는다. 레벨 스크립트가 있다는 가정으로 만든 콘텐츠가 실행 중에 조용히 멈출 수 있다 |

**LD17. prefab으로 배치한 Actor(펼친 노드)를 참조할 때**

| 선택지 | 부수 효과 |
|---|---|
| **(가) (배치 정체 + 노드 경로)로 저장하고 펼친 뒤 PF D6 파생 ID 규칙으로 해석 — 확정** | 펼친 Actor가 Transient `spawnedActors`(PF D15)에 있어 직접 저장할 수 없는 문제를 푼다. LevelScript만의 규칙이 아니라 레벨 저장 참조 일반 규칙이므로 PF D22와 ER 4.6절 저장 검증에 둔다. 배치 override로 노드가 지워지거나 원형이 바뀌어 대상이 없으면 null과 진단(`PrefabPlacementReferenceOrphaned`)이다. 영속 Entity Guid가 생기면 그 표현으로 바꾼다 |
| (나) 펼친 노드 참조 금지(prefab 배치는 Unpack 뒤에만 참조) | 규칙이 단순하다. 대신 문·트리거처럼 prefab으로 만든 물체를 레벨 스크립트에서 쓰려면 매번 Unpack해야 해서 prefab 전파가 끊긴다 |

**LD18. LevelScript의 물리**

| 선택지 | 부수 효과 |
|---|---|
| **(가) 물리 hook 선언과 Collider component 금지 — 확정** | 트리거는 배치한 트리거 Actor의 이벤트로 받는다(LD9). Sparks는 S0801·S0802 컴파일 오류, C++ 파생은 `final` hook·타입 등록·`AddComponent`·저장 검증(`LevelScriptCollider`)으로 막는다. UE LSA의 `KismetHideOverrides`·Collision 범주 숨김과 같은 방향이다(`LevelScriptActor.h:24`) |
| (나) 일반 Actor처럼 허용 | Transform 의미가 없는 Actor에 위치 있는 충돌체가 붙어 "어디서 겹치는지" 혼란이 생긴다. 레벨 연결을 트리거 Actor에 두는 쪽과 LevelScript에 두는 쪽이 섞인다 |

**LD19. 한 LevelScript 클래스를 여러 레벨이 쓸 수 있나**

| 선택지 | 부수 효과 |
|---|---|
| **(가) 허용, 레벨마다 값은 `@edit` — 확정** | 같은 규칙의 여러 스테이지(도전 맵 등)가 클래스 하나를 공유한다. "레벨 스크립트 열기"는 이미 있는 클래스를 고를 수 있다. 레벨 사이 공유 상태는 여전히 WorldService다 |
| (나) 레벨과 클래스 1:1 강제 | 같은 로직을 파일마다 복사하게 된다. 강제를 위한 검사(클래스 → 레벨 역색인)가 따로 필요하다 |

**LD20. 프로젝트 공통 기반 클래스**

| 선택지 | 부수 효과 |
|---|---|
| **(가) C++ LevelScript 파생을 공통 기반으로 둘 수 있고(core에서 Sparks 상속은 막혀 있으므로 — SL SD27), Sparks끼리 상속은 full, 템플릿 기반은 프로젝트 설정 `DefaultLevelScriptClass`(`ClassRef<LevelScript>`, 기본값 `LevelScript`) — 확정** | UE `LevelScriptActorClass`(`Engine.h:875`)와 같은 자리다. 프로젝트 공통 기능(컷씬 시작 도우미 등)을 C++ 기반에 둘 수 있다. 설정이 Sparks 클래스이고 대상 패키지가 core면 명령이 멈추고 이유를 보인다(4.8절) |
| (나) 기반은 늘 엔진 `LevelScript` | 설정이 없어 단순하다. 대신 공통 기능을 레벨마다 복사하거나 WorldService로 돌려야 한다 |

## 6. 선행 조건

결정이 아니라 이 설계가 기대는 다른 작업이다.

| 선행 | 내용 | 상태(2026-11-20) |
|---|---|---|
| Sparks 동적 schema 등록 | Sparks 클래스 Actor를 레벨에 저장하려면 스크립트 필드의 동적 schema 등록이 먼저다([Sparks 엔진 연동 요구 목록](SparksEngineIntegration.ko.md) SE-O3). 지금 Level 그래프 저장은 생성 타입만 받는다(ER 4.6절) | 후속(L2 전) |
| 레벨 패키지 단계 | Level 저장·쿡·cooked 로드와 레벨 편입 계약(ER 4.6절), 저장 검증(4.2절) | 후속 |
| Prefab PF4·PF5 | prefab 배치에 component 추가(override 노드 추가 — LD15), 배치 펼치기와 배치 노드 참조 해석(LD17, PF D22) | 후속 |
| 영속 Entity 참조 | 셀 사이 참조(LD6), 펼친 노드 참조의 최종 표현(LD17) | 후속(World 단계) |
| 네트워크 단계 | 배치 Actor의 네트워크 정체성 규칙(LD12) — EIP 17.1절 NetObjectId는 실행 중 발급뿐 | 후속 |
| 물리 단계 | Actor 물리 hook과 엔진 Actor 물리 이벤트(LD9), Collider 타입(SL 자리표시 이름) | 후속 |
| UT CU1 | CLASS 지정자 `NotPlaceable`(UT UD15) | 후속(CU1) |
| 편집기 단계 | Outliner "레벨 스크립트" 항목, 편집기 명령, 언어 서버 명령 채널(4.8절) | 후속(M3 이후) |

## 7. 다른 문서 개정

이번 개정에서 함께 고쳤다.

| 문서 | 고친 곳 |
|---|---|
| [Sparks](SparksLanguage.ko.md) 초안 1.4 | 머리말(판·기준 문서·식별자), 3.2절 기반 타입·"`?` 없는 객체 필드" 그 밖 목록, 3.4절 문맥 표, 3.7절 핵심 엔진 타입, 4.5절 편집기 component 추가, 5.1절 순서·물리 hook·SD20 종료 경로 표, 5.2절 경로 표(Actor 물리 이벤트), 5.5절 slot 표, 8.1절 S0413·S0801·S0802, 13절 편집기 명령, 17.2절 후속 표, 19절 SD46–SD50, 20절 서두 |
| [엔진 구현 계획](EngineImplementationPlan.ko.md) 개정 1.16 | 20.2절(Actor 물리 이벤트·서브 레벨 unload·편집기 component 추가), 20.4·20.5·23.1·24.1·33.2·33.7절(BP 제외 유지 + Level Blueprint 대응은 LevelScript, 노드 시각 편집은 보류 선택지), 22.1·22.3절(편집기 명령·템플릿), 33.19절 기록 |
| [엔진 런타임](EngineRuntime/README.ko.md) | 4.6절 Level 행 `levelScript`, 레벨 편입 6항(LD8), 레벨 저장 검증(LD5·LD6·LD17), 7.2절 `DestroyLevel`·World 종료 순서와 파괴 규칙(LD16) |
| [Prefab](Prefab.ko.md) 개정 5 | D22(LD17), 1.3절 비범위, 3.6절 배치 노드 참조, 3.11절 진단, 7절 Level 행 |
| [UI 툴킷](UiToolkit.ko.md) 초안 2.3 | 7.3절 (e) `NotPlaceable`을 LevelScript가 쓴다, UD15 중 `NotPlaceable`만 확정 |
| [에셋 시스템](AssetSystem.ko.md) 개정 11 | 4.6절 진단 행 |

[CompilerDiagnostics](CompilerDiagnostics.ko.md)는 C++ 경고 정책 문서라 고치지 않았다. 레벨 저장·쿡·로드 진단은 AS 4.6절, Sparks 컴파일 오류는 SL 8.1절 S 대역에 둔다.

## 8. 시험 목록

| 영역 | 시험 |
|---|---|
| 저장·로드 | `LevelScript_SavedInActorsAndPointedByLevel`, `LevelScript_DuplicateRejectedOnSaveAndCookedLoad`, `LevelScript_NotInActorsRejected`, `LevelScript_SpawnActorRejected`, `LevelScript_ColliderRejectedOnRegisterAddSave` |
| 수명 | `LevelScript_FinishSpawningAfterAllPlacedActors`(prefab 배치 포함), `LevelScript_DisabledPlacedActorNotGuaranteedActive`, `LevelScript_StartsAfterReadyActorsSameFrame`, `LevelScript_DoesNotWaitDeferredOrUnreadyActors`, `LevelScript_DestroyedFirstOnLevelUnload`, `LevelScript_SubLevelUnloadRunsDisableDestroy` |
| 파괴 | `LevelScript_DestroyActorRejected`, `LevelScript_ScriptRequestDestroyIgnoredAndReported`, `LevelScript_DestroyLevelCutsEdgeBeforeDestroy`(회수 확인), `LevelScript_BypassDestroyRepairedWithError`, `LevelScript_FaultKeepsEdge`, `LevelScript_R3ReloadRebindsEdge` |
| 참조 | `LevelScript_RequiredObjectFieldIsS0413`, `LevelScript_CrossLevelReferenceRejected`, `LevelScript_CrossCellReferenceRejectedBeforeEntityRef`, `LevelReference_ExpandedPrefabNodeRoundTrip`, `LevelReference_RemovedPrefabNodeIsOrphaned` |
| 물리 | `ActorEvent_TriggerEnteredAfterHookSameBatch`, `ActorEvent_NoSubscriberNoSlotTable`, `LevelScript_PhysicsHookIsS0801`, `LevelScript_ColliderFieldIsS0802` |
| 편집기 | `LevelScriptPath_ConvertsLevelNameSegments`, `LevelScriptPath_CaseInsensitiveCollisionGetsSuffix`, `LevelScriptPath_NonAsciiAsksForName`, `LevelScriptCommand_AddReferenceSameLevelOnly`, `LevelScriptCommand_WaitsForCompileRevision`(시간 대기 없음), `LevelScriptTemplate_UsesDefaultLevelScriptClass`, `LevelScriptTemplate_CoreRejectsSparksBase` |
| Tryout | `LevelScript_TryoutCopyRebindsInsideCopy`, `LevelScript_EditWorldRunsNoGameHooks` |

## 9. UE 대응표

| UE 5.8 | Rocks | 비고 |
|---|---|---|
| Level Blueprint(`ULevelScriptBlueprint`) | Sparks 파일 `<패키지>/Levels/<레벨 경로 이름>.spark` | Level에 내장하지 않는다(LD4) |
| `ALevelScriptActor` | `LevelScript` | Actor 파생, 배치 불가 |
| `ULevel::LevelScriptActor` | `Level.levelScript` | ReadOnly PROPERTY |
| `ULevel::RegenerateLevelScriptActor` | `World::SetLevelScript` | 편집 World 전용 |
| `UCLASS(notplaceable)` | CLASS 지정자 `NotPlaceable` | UT 7.3절 (e) |
| `bEditable`·`bActorLabelEditable = false` | Outliner "레벨 스크립트" 항목(편집기 규칙) | |
| `ULevel::RepairLevelScript`·`RemoveExtraLevelScriptActors` | 저장 거부·cooked 로드 거부(`DuplicateLevelScript`) | Rocks가 더 엄격 |
| `FixLevelScriptActorBindings`(액터 이벤트 노드 결합) | `on 필드.이벤트` 연결(SL 5.2·5.5절) | 실행 중 session이 연결 |
| "레벨 BP에 액터 참조 추가"·`OnGraphEditorDropActor` | "레벨 스크립트에 참조 추가" 명령 | 같은 Level만 |
| `RemoteEvent(FName)` | World 이벤트 `event World.X` | 이름 문자열 호출 없음 |
| `OnActorBeginOverlap`·`OnActorEndOverlap`·`OnActorHit` | Actor 이벤트 `TriggerEntered`·`TriggerExited`·`CollisionEntered`·`CollisionExited` | 지연 slot 표 |
| `KismetHideOverrides`(충돌 이벤트)·`HideCategories=(Collision)` | 물리 hook·Collider 금지(S0801·S0802·`LevelScriptCollider`) | |
| `bReplicates`·`bAlwaysRelevant` | 모든 process에 존재, 항상 관련 | |
| `UEngine::LevelScriptActorClass` | 프로젝트 설정 `DefaultLevelScriptClass` | |
| `TSubclassOf<ALevelScriptActor>` | `ClassRef<LevelScript>`(C++)·`Class<LevelScript>`(Sparks full) | RX 5.2절 |
| `HasAuthority()` | `HasAuthority`(Actor 파생)·`@authorityOnly` | SL 14절 |

## 10. 개정 기록

- 초안 0.1(2026-11-19): 설계 제안(결정 LD1–LD15 대기). 대상 문서는 Sparks·EIP 20·23절·ER 4.6절·Prefab.
- 개정 1(2026-11-20): 리뷰어 1명의 원문·UE 소스 대조와 사용자 결정(모든 항목 권장안)을 반영해 정본으로 만들었다.
  - 사실 정정: Prefab 기준 판은 개정 4(이번에 5). 진단 등록처는 AS 4.6절(이름 코드)과 SL 8.1절 S 대역이며 CompilerDiagnostics는 고치지 않는다. 배치 불가 표지는 RCM이 아니라 UT 7.3절 (e) `NotPlaceable` 재사용. 2.1절 표의 "엔진 MatchRules" → GameDirector(ER 2절). 레벨 로드 순서는 spawn 묶음 규칙이 아니라 Actor마다 `FinishSpawning`(ER 7.3절·4.6절 편입 6항)으로 3.4·4.3절을 고쳤다. 같은 Actor component 찾기는 `Find<T>()`(Actor 파생에는 `Owner`가 없다).
  - UE 인용 정정(2.2·2.3절): 같은 레벨 제한의 근거는 `OnGraphEditorDropActor`(BlueprintEditor.cpp:4081-4092), `FixLevelScriptActorBindings`는 8491-8545행이며 "편집기 원본 LSA에서만 다시 묶는다", Level.cpp:5064-5071은 `RepairLevelScript`의 감지·오류 로그이고 복구는 5078행·편집기 World에서만, 생성자 감지(LevelScriptActor.cpp:35-46), UE는 저장을 거부하지 않는다, `RemoteEvent`는 64-89행, Level Instance는 LSA를 허용하고 break만 막는다, `bAlwaysRelevant`(:33), `LevelScriptActorClass`(Engine.h:875). UE BeginPlay 순서는 LSA를 특별 취급하지 않는다(2.3절).
  - Sparks 이름 확인: `Class<T>`(full)·C++ `ClassRef<Base>`, `HasAuthority`·`Owner.HasAuthority`·`@authorityOnly`, 물리 hook 인자, ScriptableComponent도 물리 hook을 받는다, hook 이름은 멤버 이름 금지라 이벤트는 과거형.
  - 새 결정: LD16(파괴와 간선), LD17(펼친 노드 참조 — PF D22), LD18(물리 금지), LD19(클래스 공유), LD20(공통 기반·설정). LD8은 `FinishSpawning`·시작 큐 기준으로 다시 썼다.
  - 이 문서가 정한 세부: World API 이름 `World::SetLevelScript`(ER 4.6절 `Set…` 꼴, UE `RegenerateLevelScriptActor`처럼 클래스를 받아 World가 만든다), LevelScript 파생 클래스의 spawn 거부, 생성 경로의 이름 변환·충돌 규칙(4.8절), 언어 서버 명령 이름, S 코드 S0413·S0801·S0802, 진단 이름 `LevelScriptCollider`·`LevelScriptSpawnRejected`·`LevelScriptDestroyRejected`·`PrefabPlacementReferenceOrphaned`, 프로젝트 설정 이름 `DefaultLevelScriptClass`.
