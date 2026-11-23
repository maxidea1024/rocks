# Rocks 게임플레이 작업 안내

이 안내는 Rocks에서 **컴포넌트, 액터, 레벨에 동작을 넣는 방법**을 설명한다. 같은 일을 Sparks로 할 때와 C++로 할 때를 나란히 보여 준다. 블루프린트는 쓰지 않는다. 레벨마다 하나씩 두는 연출과 연결은 [레벨 스크립트](../Design/LevelScript.ko.md)(`LevelScript`)가 맡는다.

| 알고 싶은 것 | 읽을 절 |
|---|---|
| 무엇을 컴포넌트·액터·레벨 스크립트 중 어디에 쓸지 | 1 |
| 수정하고 확인하는 반복 주기 | 2 |
| 재사용하는 동작(체력, 줍기) | 3 |
| 그 자체가 하나의 물체(문, 캐릭터) | 4 |
| 이 레벨에서만 일어나는 일(성문과 트리거 연결) | 5 |
| hook마다 할 일과 하면 안 되는 일 | 6 |
| C++ 기반 클래스를 Sparks로 확장 | 7 |

**상태 표시.** 예제의 지정자와 API는 상태가 서로 다르다. 각 예제 아래에 상태를 적었다.

- **있음**: 지금 코드에 있다.
- **스펙**: 설계 문서에 확정했고 아직 구현하지 않았다.

"스펙"으로 표시한 이름은 구현할 때 바뀔 수 있다. 근거 문서는 다음과 같다.

- [Sparks 언어](../Design/SparksLanguage.ko.md) 초안 1.4
- [Prefab](../Design/Prefab.ko.md) 개정 5
- [레벨 스크립트](../Design/LevelScript.ko.md) 개정 1
- [엔진 구현 계획](../Design/EngineImplementationPlan.ko.md) 20·22·23절
- 현재 코드: `Engine/Source/Runtime/World`, `Core/Delegate.h`, `Templates/Game/Foundation`

## 1. 무엇을 어디에 쓰나

| 질문 | 쓰는 것 | 예 |
|---|---|---|
| 여러 물체에 같은 동작을 붙이나? | 컴포넌트 | 체력, 줍기, 회전, 상호작용 |
| 그 자체가 하나의 물체인가? | 액터 클래스 | 캐릭터, 문, 엘리베이터, 투사체 |
| 이 레벨에서만 일어나는 일인가? | 레벨 스크립트 | 시작 컷씬, 이 문과 저 트리거 연결 |
| 게임 전체에 걸친 상태인가? | WorldService | 점수, 퀘스트 진행, 경기 규칙 |

| 대상 | Sparks | C++ | 같은 클래스에서 값만 다른 변형 |
|---|---|---|---|
| 컴포넌트 | `class X : ScriptableComponent` (스펙) | `class X : public rocks::Component` (있음) | 배치·prefab의 override |
| 액터 | `class X : Actor` 또는 Pawn·Character 파생 (스펙) | `class X : public rocks::Actor` (있음) | prefab, prefab variant |
| 레벨 | `class X : LevelScript` (스펙) | `class X : public rocks::LevelScript` (스펙) | 해당 없음(레벨마다 하나, 값은 레벨마다 `@edit`) |
| 게임 전체 | `class X : WorldService` (스펙) | `WorldSubsystem` 파생 (있음) | 해당 없음 |

**Sparks와 C++ 가운데 고르기.** 게임 동작은 Sparks로 쓰는 것이 기본이다. Sparks로 쓰면 다음 이점이 있다.

- 저장하면 실행 중인 게임에 바로 반영된다.
- 이벤트를 연결하고 해제하는 일, 파괴된 대상을 처리하는 일, 자원을 정리하는 일을 언어가 대신 한다.

C++는 다음 경우에 쓴다.

- 매 프레임 많은 객체를 처리해야 해서 성능이 중요할 때. Sparks도 AOT 네이티브 변환을 고를 수 있으니 먼저 측정해 보고 결정한다.
- 엔진 기능을 새로 노출하거나, 다른 C++ 시스템과 깊게 맞물려야 할 때.
- Sparks가 상속해 쓸 기반 클래스를 만들 때(7절).

## 2. 작업 환경과 반복 주기

### 2.1 Sparks (스펙)

- **패키지**: 게임의 스크립트 루트에 `SparksPackage.json5`를 둔다. 여기에 이름, 언어 버전, core·full 프로필, 의존 패키지, services를 적는다. 파일 경로가 곧 import 경로다.
- **편집**: VS Code와 Sparks 언어 서버를 쓴다. 자동 완성, 오류 표시, 엔진 타입 hover를 제공한다.
- **컴파일**: 저장하면 증분 컴파일된다. 경고는 없고 오류만 있다.
- **반영**: 에디터나 Tryout이 실행 중이면 핫 리로드된다. 실패하면 이전 버전으로 계속 실행된다.
  - R1: 함수 본문 수정
  - R2: 필드 추가·변경
  - R3: 상속이나 `let` 구성 변경. 필드 값을 보존한 채 객체를 다시 만든다.

주기: 저장 → 컴파일 → 실행 중인 게임에 반영

### 2.2 C++ (있음)

- **모듈**: 게임 모듈의 `<모듈>.Build.cs`(템플릿에서 만든 프로젝트는 `<프로젝트>.Build.cs`)에 소스와 의존 모듈(`World` 등)을 적는다. 리플렉션 헤더는 적지 않는다 — 빌드가 `ROCKS_CLASS`·`ROCKS_STRUCT`·`ROCKS_ENUM` 표지가 있는 헤더를 찾는다.
- **식별자**: 타입·필드 식별자는 메타 컴파일러가 `<모듈>.rocksids.json`에 발급한다. 매크로에 직접 적지 않는다.
- **빌드**: `dotnet msbuild Rocks.proj -t:Game`. 메타 컴파일러가 `*.generated.h`와 모듈 등록 코드를 만든다.
- **반영**: 에디터를 다시 시작한다. 실행 중에 코드를 다시 불러오는 기능(UE Live Coding 대응)은 모듈 적재 단계 뒤에 별도 스펙으로 다룬다.

주기: 수정 → 빌드 → 에디터 재시작 → Tryout

### 2.3 공통

편집 World에서는 게임 hook(Awake·Start·Tick 등)이 돌지 않고 Validate만 실행된다. 동작은 Tryout에서 확인한다. Tryout은 편집 World를 복제해서 실행하므로, Tryout 중에 생긴 변화는 편집 원본에 남지 않는다.

## 3. 컴포넌트에 동작 넣기

예제는 피해를 받으면 체력이 줄고, 0이 되면 `Died` 이벤트를 내는 체력 컴포넌트다.

### 3.1 Sparks (스펙)

```sparks
// Combat/Health.spark
class Health : ScriptableComponent {
    @edit @save var MaxHealth: int = 100;
    @net var Current: int = 100;

    event Died(killer: Actor?);

    on Awake() {
        Current = MaxHealth;           // 배치마다 바꾼 MaxHealth 반영
    }

    fn ApplyDamage(amount: int, instigator: Actor?) {
        if (Current == 0) { return; }
        Current = Math.Max(0, Current - amount);
        if (Current == 0) { emit Died(instigator); }
    }
}
```

작업 순서:

1. 콘텐츠 브라우저에서 **새 Sparks 스크립트**를 고르고 기반 타입으로 `ScriptableComponent`를 골라 `Combat/Health.spark`를 만든다.
2. VS Code에서 위 코드를 쓰고 저장한다.
3. Actor를 고르고 Details의 **컴포넌트 추가**에서 `Health`를 고른다. 붙일 수 있는 곳은 3.3절에 있다.
4. Details에 `MaxHealth`가 나타나며, 배치마다 값을 바꿀 수 있다.
5. Tryout으로 확인한다. 고친 뒤에는 저장만 하면 반영된다.

### 3.2 C++

```cpp
// Source/Combat/Health.h
#pragma once

#include <Core/Delegate.h>
#include <World/Component.h>

#include <Health.generated.h>

namespace game {

ROCKS_CLASS(ScriptExtensible)
class Health : public rocks::Component {
    ROCKS_GENERATED_BODY()

public:
    explicit Health(rocks::Allocator& allocator) noexcept : Died(allocator) {
    }

    rocks::Result<void> OnAwake() noexcept override {
        current_ = maxHealth_;
        return rocks::Result<void>::Success();
    }

    ROCKS_FUNCTION(Lane = Game, Effects = WriteWorld)
    rocks::Result<void> ApplyDamage(rocks::int32 amount, rocks::WeakObjectRef<rocks::Actor> instigator) noexcept;

    ROCKS_PROPERTY(Params = "killer")
    rocks::MulticastDelegate<void(rocks::WeakObjectRef<rocks::Actor>)> Died;

private:
    ROCKS_PROPERTY(Edit, SaveGame)
    rocks::int32 maxHealth_ = 100;

    ROCKS_PROPERTY(ReadOnly, Replicated)
    rocks::int32 current_ = 100;
};

} // namespace game
```

```cpp
// Source/Combat/Health.cpp
rocks::Result<void> Health::ApplyDamage(rocks::int32 amount, rocks::WeakObjectRef<rocks::Actor> instigator) noexcept {
    if (current_ == 0) {
        return rocks::Result<void>::Success();
    }

    current_ = current_ > amount ? current_ - amount : 0;
    if (current_ == 0) {
        Died.Broadcast(instigator);
    }

    return rocks::Result<void>::Success();
}
```

상태:

- **있음**: `ROCKS_CLASS`, `ROCKS_PROPERTY`, `SaveGame`, `ReadOnly`, `MulticastDelegate`, `OnAwake`
- **스펙**: `Edit`, `Params`, `ROCKS_FUNCTION`, `ScriptExtensible`([UI 툴킷](../Design/UiToolkit.ko.md) CU1–CU3, Sparks 10.1절)
- **네트워크 단계**: `Replicated`

작업 순서:

1. 헤더(`Combat/Health.h` — `ROCKS_CLASS()` 표지)와 소스를 쓴다. 빌드가 표지로 헤더를 찾으므로 `<모듈>.Build.cs`는 고치지 않는다.
2. 빌드한 뒤 에디터를 다시 시작한다.
3. 이후는 Sparks와 같다. 컴포넌트 추가 목록에 `Health`가 나오고, `Edit` 필드가 Details에 나타난다.

`ScriptExtensible`과 `ROCKS_FUNCTION`이 붙어 있으므로 Sparks에서도 `Health`를 쓸 수 있다. 예를 들어 `on Health.Died()`로 이벤트를 받는다.

### 3.3 컴포넌트를 붙이는 곳 (Sparks·C++ 공통)

| 어디에 | 어떻게 | 저장되는 곳 | 상태 |
|---|---|---|---|
| 레벨에 직접 배치한 Actor 하나 | Actor 선택 → Details **컴포넌트 추가** | 그 Actor의 `components`(레벨) | 스펙(LevelScript LD15) |
| prefab 전체 | prefab 편집기에서 추가 | prefab 에셋. 모든 배치가 받는다 | 스펙(Prefab PF4) |
| prefab 배치 하나 | 배치를 고르고 **컴포넌트 추가** | 그 배치의 override "노드 추가" | 스펙(Prefab PF4) |
| Actor 클래스 정의 | Sparks `let` 필드, C++ `ROCKS_PROPERTY(Subobject)` | 클래스 원형. 그 클래스의 모든 Actor가 받는다 | 스펙(Prefab PF1) |
| 실행 중 코드 | C++ `World::AddComponent<T>`, Sparks는 full 프로필 | 저장되지 않는다(실행 중 생성) | C++ 있음 |

## 4. 액터에 동작 넣기

예제는 열리면 `Opened` 이벤트를 내고, 열린 상태를 세이브에 남기는 문이다. 경첩(SceneComponent)은 클래스 원형으로 가진다.

### 4.1 Sparks (스펙)

```sparks
// Props/Door.spark
class Door : Actor {
    let Hinge = SceneComponent();            // 클래스 원형 component

    @edit var OpenAngle: float = 110.0;      // 도
    @save var IsOpen: bool;

    event Opened(by: Actor?);

    on Enable() { ApplyAngle(); }            // 세이브에서 읽은 상태 반영
    on IsOpen.Changed() { ApplyAngle(); }

    fn Open(by: Actor?) {
        if (IsOpen) { return; }
        IsOpen = true;
        emit Opened(by);
    }

    fn ApplyAngle() {
        if (IsOpen) {
            Hinge.RelativeRotation.Yaw = OpenAngle;
        } else {
            Hinge.RelativeRotation.Yaw = 0.0;
        }
    }
}
```

`let` component의 초기값은 Prefab PF1(클래스 원형)이 구현된 뒤에 쓸 수 있다.

### 4.2 C++

```cpp
// Source/Props/Door.h
ROCKS_CLASS(ScriptExtensible)
class Door : public rocks::Actor {
    ROCKS_GENERATED_BODY()

public:
    explicit Door(rocks::Allocator& allocator) noexcept : Actor(allocator), Opened(allocator) {
    }

    void OnEnable() noexcept override {
        ApplyAngle();
    }

    ROCKS_FUNCTION(Lane = Game, Effects = WriteWorld)
    rocks::Result<void> Open(rocks::WeakObjectRef<rocks::Actor> by) noexcept;

    ROCKS_PROPERTY(Params = "by")
    rocks::MulticastDelegate<void(rocks::WeakObjectRef<rocks::Actor>)> Opened;

private:
    void ApplyAngle() noexcept;

    ROCKS_PROPERTY(Subobject, Parent = "root") // 지정자 이름은 PF1에서 확정
    rocks::ObjectRef<rocks::SceneComponent> hinge_;

    ROCKS_PROPERTY(Edit)
    double openAngle_ = 110.0; // 도

    ROCKS_PROPERTY(SaveGame)
    bool isOpen_ = false;
};
```

```cpp
// Source/Props/Door.cpp
rocks::Result<void> Door::Open(rocks::WeakObjectRef<rocks::Actor> by) noexcept {
    if (isOpen_) {
        return rocks::Result<void>::Success();
    }

    isOpen_ = true;
    ApplyAngle();
    Opened.Broadcast(by);
    return rocks::Result<void>::Success();
}

void Door::ApplyAngle() noexcept {
    const double yaw = isOpen_ ? openAngle_ : 0.0;
    auto rotated = GetWorld().With<rocks::SceneComponent>(hinge_.Handle(), [yaw](rocks::SceneComponent& hinge) noexcept {
        return hinge.SetRelativeRotation(rocks::Rotator<double>(0.0, yaw, 0.0).ToQuaternion());
    });
    if (!rotated) {
        // 경첩이 없으면 각도만 바꾸지 못한다. 오류 코드를 Log API로 남긴다(메시지는 영어).
    }
}
```

상태와 주의:

- **있음**: `World::With`, `SetRelativeRotation`, `Rotator::ToQuaternion`
- Rocks 생성자는 Registry나 World에 접근할 수 없다. 그래서 component를 생성자에서 만들지 않는다.
- 클래스 원형 지정자(PF1)가 생기기 전에는 `OnAwake`에서 `AddComponent`로 만든다. 이렇게 만든 component는 저장되지 않는다.

### 4.3 레벨에 놓고 변형 만들기 (Sparks·C++ 공통)

- **클래스를 그대로 배치**: 콘텐츠 브라우저의 클래스 목록에서 `Door`를 레벨로 끌어다 놓는다. Details에서 `OpenAngle`을 배치마다 바꾼다.
- **같은 문을 여러 번 쓸 때는 prefab**: 값과 구성을 정해 둔 문을 prefab 에셋으로 저장한다. 예를 들어 `IronDoor`는 각도를 90으로 두고 잠금 컴포넌트를 더한 문이다. UE에서 값만 다른 BP 자식 클래스를 만들던 자리다.
- **prefab의 변형은 variant**: `IronDoor`를 기준으로 색만 다른 `RustyIronDoor`를 만든다. 기준이 바뀌면, variant에서 덮어쓰지 않은 값은 따라 바뀐다.
- **실행 중 생성**:
  - Sparks: `let door = try World.Spawn(prefab, at);`
  - C++: `world.SpawnActor<Door>()`. 지금 코드에 있다.
  - prefab spawn은 Prefab PF 단계가 구현된 뒤에 쓸 수 있다.

**클래스와 prefab의 경계.**

- 동작(코드)과 구조(어떤 component가 반드시 있어야 하나)는 클래스에 둔다.
- 수치, 에셋, 선택적인 component 조합은 prefab에 둔다.
- 동작이 다르면 클래스를 나누고, 값만 다르면 prefab을 나눈다.

## 5. 레벨에 동작 넣기

예제는 플레이어가 성문 트리거에 들어가면 성문이 열리고, 열리면 다른 레벨과 UI에 알리는 흐름이다. 연결은 한곳에 모은다.

- Sparks: 레벨 스크립트 파일 하나
- C++: `LevelScript` 파생 클래스 하나

이 절의 내용은 모두 스펙이다([레벨 스크립트](../Design/LevelScript.ko.md) 개정 1).

### 5.1 Sparks

```sparks
// Levels/Stage1.spark
import Props.Door;
import Characters.PlayerCharacter;
import Events.StageEvents;

class Stage1 : LevelScript {
    @edit var GateTrigger: Actor?;
    @edit var GateDoor: Door?;

    on Start() {
        Log.Info("Stage1 started");
    }

    on GateTrigger.TriggerEntered(other: Actor) {   // 트리거 Actor의 이벤트(LD9)
        if (let player = other as? PlayerCharacter) {
            if (let door = GateDoor) {
                door.Open(player);
            }
        }
    }

    on GateDoor.Opened() {
        emit World.GateOpened();                      // 다른 레벨·UI에 알림(LD10)
    }
}
```

```sparks
// Events/StageEvents.spark
event World.GateOpened();
```

작업 순서:

1. 레벨을 열고 툴바에서 **레벨 스크립트 열기**를 누른다. 처음 누르면 다음 일이 차례로 일어난다.
   - 레벨 경로에서 이름을 지은 `Levels/Stage1.spark`가 템플릿으로 생긴다.
   - 그 스크립트가 이 레벨에 지정된다.
   - VS Code에서 파일이 열린다.
2. Outliner에서 트리거 Actor를 우클릭해 **레벨 스크립트에 참조 추가**를 고른다. 클래스에 `@edit var GateTrigger: Actor?;` 줄이 들어가고 값도 지정된다. 문도 같은 방법으로 추가한다.
3. 같은 메뉴의 **이벤트 추가**로 `on GateTrigger.TriggerEntered(...)` 틀을 넣고 내용을 쓴다.
4. Tryout으로 확인한다. 고친 뒤에는 저장하면 반영된다.

연결과 해제는 `on` 절이 자동으로 처리한다. 참조한 문이 파괴되면 `GateDoor`는 null이 되고 연결도 끊긴다.

### 5.2 C++

```cpp
// Source/Levels/Stage1Script.h
ROCKS_CLASS()
class Stage1Script : public rocks::LevelScript {
    ROCKS_GENERATED_BODY()

public:
    explicit Stage1Script(rocks::Allocator& allocator) noexcept : LevelScript(allocator) {
    }

    void OnEnable() noexcept override;
    void OnDisable() noexcept override;

private:
    void HandleGateEntered(rocks::WeakObjectRef<rocks::Actor> other, rocks::WeakObjectRef<rocks::Collider> trigger) noexcept;

    ROCKS_PROPERTY(Edit, EditScope = "Instance")
    rocks::WeakObjectRef<rocks::Actor> gateTrigger_;

    ROCKS_PROPERTY(Edit, EditScope = "Instance")
    rocks::WeakObjectRef<Door> gateDoor_;

    ROCKS_NATIVE() rocks::DelegateHandle enteredHandle_;
};
```

```cpp
// Source/Levels/Stage1Script.cpp
void Stage1Script::OnEnable() noexcept {
    // C++에서는 연결을 직접 한다. Enable에서 걸고 Disable에서 끊는다.
    // Actor 이벤트는 구독할 때 지연 slot 표에 자리가 생긴다(LD9). 구독 API 이름은 구현할 때 정한다.
    auto added = GetWorld().With<rocks::Actor>(gateTrigger_.Handle(), [this](rocks::Actor& trigger) noexcept {
        return trigger.TriggerEntered().TryAddRaw(this, &Stage1Script::HandleGateEntered);
    });
    if (added) {
        enteredHandle_ = added.Value();
    }
}

void Stage1Script::OnDisable() noexcept {
    // 트리거가 먼저 파괴됐으면 With가 실패하고, 구독도 이미 함께 사라졌다.
    auto removed = GetWorld().With<rocks::Actor>(gateTrigger_.Handle(), [this](rocks::Actor& trigger) noexcept {
        return trigger.TriggerEntered().Remove(enteredHandle_);
    });
    static_cast<void>(removed);
    enteredHandle_ = {};
}

void Stage1Script::HandleGateEntered(rocks::WeakObjectRef<rocks::Actor> other, rocks::WeakObjectRef<rocks::Collider>) noexcept {
    // PlayerCharacter가 아니면 With가 타입 오류로 끝난다.
    auto opened = GetWorld().With<PlayerCharacter>(other.Handle(), [&](PlayerCharacter&) noexcept {
        return GetWorld().With<Door>(gateDoor_.Handle(), [&](Door& door) noexcept { return door.Open(other); });
    });
    static_cast<void>(opened);
}
```

작업 순서:

1. 클래스를 `ROCKS_CLASS()` 표지와 함께 헤더에 쓰고, 빌드하고 에디터를 다시 시작한다.
2. 레벨 Details의 **레벨 스크립트** 항목에서 `Stage1Script`를 고른다(`World::SetLevelScript`). 이 목록에는 C++와 Sparks의 `LevelScript` 파생 클래스가 모두 나온다.
3. Details에 나타난 `gateTrigger_`, `gateDoor_`에 Outliner의 Actor를 지정한다.
4. Tryout으로 확인한다.

상태:

- **있음**: `TryAddRaw`, `Remove`, `DelegateHandle`, `World::With`
- **스펙**:
  - `LevelScript`(LD3·LD4)
  - Actor 이벤트 `TriggerEntered`(LD9). 구독 accessor의 모양은 아직 정하지 않았다.
- **물리 단계 타입**: `Collider`
- **미정**: C++에서 World 이벤트를 발행하는 API. Sparks의 World 이벤트 proxy와 함께 정한다.

### 5.3 레벨 스크립트의 규칙 (Sparks·C++ 공통)

**개수와 배치**

- 레벨마다 0개 또는 1개다. 로직이 커지면 레벨 스크립트 안에 컴포넌트를 여러 개 두어 나눈다.
- 콘텐츠에서 끌어다 배치할 수 없다(`NotPlaceable`). Outliner에서는 일반 Actor 줄에 나오지 않고 **레벨 스크립트** 항목으로만 보인다.
- 같은 레벨 스크립트 클래스를 여러 레벨에서 쓸 수 있다. 레벨마다 다른 값은 `@edit`로 준다.

**참조와 연락**

- 참조할 수 있는 것은 같은 레벨에 배치된 Actor와 그 component뿐이다. 참조는 언제든 비어 있을 수 있는 형태(`T?`, `WeakObjectRef`)로만 둔다.
- prefab으로 배치한 Actor를 가리키면, 엔진이 배치와 노드 경로로 저장했다가 다시 찾는다.
- 다른 레벨의 Actor를 지정하면 저장이 거부된다. 다른 레벨에 알릴 때는 World 이벤트를 쓴다. 이름 문자열로 다른 레벨의 함수를 부르는 방법은 없다.

**수명 순서**

- 레벨이 로드되면 그 레벨의 배치 Actor가 모두 생성(Awake·Enable)을 마친 뒤에 레벨 스크립트가 Awake·Enable한다. 그래서 Enable에서는 참조 대상이 이미 활성 상태다. 단, 꺼진 채 저장된 Actor는 예외다.
- Start도 같은 프레임에 시작하는 배치 Actor들 뒤에 시작한다. 다만 다른 Actor의 Start가 끝나기를 기다리지는 않는다. 다른 Actor가 준비됐다는 사실이 필요하면 그 Actor의 이벤트로 받는다.
- 레벨을 내릴 때는 레벨 스크립트가 먼저 Disable·Destroy한다.

**할 수 없는 것**

- 레벨 스크립트 자신은 트리거·충돌 hook을 가질 수 없고 Collider도 붙일 수 없다. 트리거는 트리거 Actor를 따로 배치하고 그 이벤트(`TriggerEntered`)로 받는다.
- 레벨 스크립트는 직접 파괴할 수 없다. 레벨과 함께만 사라진다.

**적용 범위와 네트워크**

- 스트리밍 서브 레벨은 각자 레벨 스크립트를 가질 수 있다. World Partition 셀, Level Instance, prefab에는 둘 수 없다.
- 레벨 스크립트는 서버와 클라이언트 모두에 있다. 서버에서만 할 일은 `HasAuthority`로 권한을 확인한 뒤 한다.

## 6. 수명 hook 공통 규칙

| Sparks | C++ | 언제 | 여기서 할 일 | 하면 안 되는 일 |
|---|---|---|---|---|
| `on Awake()` | `OnAwake()` | 생성 직후 1회 | 자기 필드 초기화. C++는 실행 중 component 추가 | 기다리기, 다른 Actor 생성, 자원 획득(Sparks). 실패를 돌려주면 spawn이 실패한다 |
| `on Enable()` | `OnEnable()` | 활성화될 때마다 | 저장·복제된 상태를 화면에 반영. C++는 이벤트 연결 | 한 번만 할 일(활성화마다 다시 불린다) |
| `on Start()` | `OnStart()` | 다음 프레임 시작 처리에서 1회 | 준비 대기(Sparks는 에셋 로드 `await` 가능), 게임 시작 처리 | Tick에서 할 일. Start가 끝나기 전에는 Tick이 시작되지 않는다 |
| `on Tick()` 등 | `OnTick(ctx)` 등 | 매 프레임·고정 step | 움직임, 입력 반영 | 기다리기, 생성, 자원 획득(Sparks). C++는 생성자에서 `PrimaryTick().tick = true`로 켠다 |
| `on Disable()` | `OnDisable()` | 비활성화·파괴 직전 | C++는 이벤트 연결 해제. Sparks는 자동 | `emit`(Sparks 컴파일 오류) |
| `on Destroy()` | `OnDestroy()` | 파괴될 때 1회 | 마지막 정리, 기록 | 새 객체 생성 |

Sparks hook과 C++ 가상 함수는 1:1로 대응한다(EIP 20.2절). 엔진 단계 이름으로는 Initialize·Activate·BeginPlay·Tick·Deactivate·EndPlay다. C++ 가상 함수는 지금 코드에 있다.

## 7. Sparks와 C++ 함께 쓰기

| 하고 싶은 일 | 방법 |
|---|---|
| C++ 클래스를 Sparks가 상속 | C++ 클래스에 `ROCKS_CLASS(ScriptExtensible)`를 붙이면 Sparks에서 `class BossDoor : Door`로 쓸 수 있다. 성능이 중요한 부분은 C++ 기반에, 레벨별 조정과 연출은 Sparks 파생에 둔다. UE에서 C++ 부모와 BP 자식으로 나누던 방식에 해당한다. 레벨 스크립트의 프로젝트 공통 기반도 C++ `LevelScript` 파생으로 만든다(core 프로필에서는 Sparks끼리 상속할 수 없다). |
| C++ 함수를 Sparks에서 호출 | `ROCKS_FUNCTION`을 붙인다. Sparks에서는 `door.Open(player)`로 보인다. |
| C++ 이벤트를 Sparks에서 받기 | `MulticastDelegate` 필드에 `ROCKS_PROPERTY(Params = ...)`를 붙인다. Sparks에서는 `on door.Opened()`로 받는다. C++ `Broadcast`가 스크립트 단계 안에서 불리면 발행 큐 항목이 되고, 밖에서 불리면 다음 입력 batch에서 Sparks handler에 전달된다(Sparks 5.4절). |
| Sparks 필드·이벤트를 C++에서 쓰기 | 권장하지 않는다. Sparks 필드는 C++ 필드가 아니라 스크립트 저장소에 있다. C++가 알아야 할 데이터와 이벤트는 C++ 기반 클래스에 선언하고, 값은 Sparks가 채운다. |
| 값만 Sparks나 데이터로 바꾸기 | C++ 필드에 `Edit`를 붙인다. 그러면 Details, prefab, Sparks 기본값 어디서든 바꿀 수 있다. |

### 7.1 역할별로 주로 만지는 것

| 역할 | 주로 만지는 것 |
|---|---|
| 레벨 디자이너 | 배치, prefab·variant, Details 값, 레벨 스크립트(Sparks core 프로필), 참조·이벤트 추가 명령 |
| 게임플레이 프로그래머 | Sparks 컴포넌트·액터, WorldService. 필요하면 C++ 기반 클래스 |
| 엔진·시스템 프로그래머 | C++ 기반 클래스, `ROCKS_FUNCTION` 노출, 성능 경로, 엔진 이벤트 |

## 8. 하지 않는 것

- 블루프린트나 노드 그래프로 게임 로직을 쓰지 않는다. 머티리얼·애니메이션·VFX 그래프와 컷씬 타임라인은 데이터 전용 편집기이며 게임 로직을 담지 않는다.
- 이름 문자열로 함수나 이벤트를 부르지 않는다. 레벨 사이 연락은 World 이벤트로, Actor 사이 연락은 참조와 이벤트로 한다.
- C++ 생성자에서 World, Registry, 다른 객체를 건드리지 않는다. component는 클래스 원형(PF1)이나 `OnAwake`에서 만든다.
- 실행 중에 추가한 component가 저장될 것이라고 기대하지 않는다. 저장할 구성은 클래스 원형, prefab, 배치에 둔다.
- 편집 World에서 게임 hook이 돌 것이라고 기대하지 않는다. 편집기에서는 Validate만 실행된다. 동작은 Tryout에서 확인한다.
- C++에서 이벤트를 연결해 놓고 해제하지 않은 채 두지 않는다. Enable에서 연결하고 Disable에서 해제한다. Sparks는 이 일을 자동으로 하므로, 대부분의 게임 로직은 Sparks로 쓰는 편이 안전하다.
- 레벨 스크립트에 트리거·충돌 처리를 직접 넣지 않는다. 트리거 Actor를 배치하고 그 이벤트를 받는다.
