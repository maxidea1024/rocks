# Sparks — Rocks 게임플레이 언어

정적 타입 · C 계열 문법 · 레지스터 VM · 비프로그래머 대상의 방어적 설계

**설계 기준일 2026.11.12 · 초안 1.0 · 상태: 설계 제안 / 미구현 · 성능 미측정**

이 문서는 [전체 계획](EngineImplementationPlan.ko.md) §23이 요구하는 Rocks 게임플레이 언어 Sparks를 정의한다. 프로그래머가 아닌 사용자도 쓸 수 있도록 Go처럼 단순하게, 실수할 여지가 적게, 메모리·리소스 누수가 생길 수 없는 구조로 설계한다. 기준은 엔진 구현 상태가 아니라 전체 계획 §23과 이 언어가 연결되는 전체 계획 §5·§6·§8·§9·§10·§16·§17·§20·§21·§24·§29의 계약이다. 이 문서의 문법·이름·API는 설계 제안이며 구현·측정 결과가 아니다. 예시의 엔진 API 이름(`AddMovement`, `World.Spawn` 등)은 바인딩 규칙을 보이기 위한 예시이며 실제 이름은 각 모듈의 C++ 선언에서 생성된다.

| 항목 | 내용 |
|---|---|
| 기준 문서 | 전체 계획 개정 1.7 §23 · [입력 상세안](CoreObjectInputDetailedPlan.ko.md) · [UI 툴킷](UiToolkit.ko.md) CU2·CU3·CU5·CU8 · [Clang 생성 Reflection](../Implementation/ReflectionMetadata.ko.md)(구현 상태) |
| Unity·Verse 비교 기준 | Unity 수명 hook 체계(종류·호출 순서·의미). Verse는 The Book of Verse[R19] 기준이며 언어 설계의 참고 대상이다. Rocks의 Unreal 기준은 UE 5.6 |
| Rocks 상태 | Sparks 언어·VM·컴파일러 미구현 |
| 식별자 | 단계 L0–L4, Core 변경 C1–C5, Gate G1–G4, 결정 SD1–SD10, 참고 자료 R1–R31 |

## 0. 핵심 결정

| 항목 | 결정 |
|---|---|
| 이름 | Sparks. 소스 `.spark`, 컴파일 모듈 `.sparks`(SD1). 파일 경로가 import 경로다(`Weapons/Sword.spark` → `import Weapons.Sword;`). |
| 단순성 | 진단은 경고 없이 오류만 두고 모든 오류에 수정 예를 제시한다. 한 가지 일에는 한 가지 방법만 둔다. 실수가 잦은 구조는 언어 규칙으로 제거한다. 기능은 기본 단계(core)와 전체 단계(full)로 구분한다. |
| 문법 | C 계열이다. `var`/`let`, `fn Name(a: T): R`, 괄호로 감싼 조건식, `on` handler, `@attribute`. 타입·멤버는 PascalCase로 C++ 이름과 같고, 내장 값 타입(`float`, `vec3`)과 지역 변수는 소문자로 시작한다. |
| 수명 hook | Unity의 hook 체계(종류·호출 순서·의미)를 따르고 이름은 Rocks 어휘를 사용한다: `Awake`, `Enable`, `Start`, `Tick`, `FixedTick`, `LateTick`, `Disable`, `Destroy`, 물리 `TriggerEnter` 등. |
| 객체 참조 | "없음"은 하나다. 다른 Entity·Component는 `T?`로 받고 약한 handle로 저장하며 파괴되면 null로 읽힌다. 스크립트의 파괴 요청은 현재 단계가 끝날 때 반영된다(SD3). 스크립트 상태는 GC root가 되지 않는다. |
| 이벤트 | `on 경로.이벤트(...)` 한 형태다. 명시 구독·closure가 없으며, 발행은 큐에 등록되어 현재 실행 조각이 끝난 뒤 같은 단계 안에서 순서대로 전달된다. |
| 유지 자원 | 멈출 때까지 계속되는 자원(반복 사운드·입력 context·로딩 요청 등)은 필드나 `let`에 담아야만 생성되고, 블록이 끝나거나 다른 값을 넣거나 객체가 비활성화·종료되면 멈춘다. 복사·전달할 수 없다. |
| 비동기 로딩 | `try await X.Load()` 하나로 로드하고, 상주는 결과 값을 보관하는 것으로 유지한다. 진행률은 지역 요청 handle로 조회한다. 이동·travel·준비 대기는 엔진이 prefetch와 로딩 화면을 관리한다. |
| 누수 | 소유자를 Session·Instance·Execution·ModuleHost로 고정하고, 실패할 수 없는 3개 해제 연산(EndExec·Detach·CloseSession)으로만 해제한다. 범위 종료마다 0을 검사하고 장시간 세션의 증가 추세를 Gate로 검사한다. |
| 실행 | 레지스터 VM(Scalar·Ref·Buf bank), bytecode 사전 컴파일(`.sparks`), 선택적 native AOT(typed IR → C++), 런타임 eval(target 정책, 기본 꺼짐), 컴파일러를 공유하는 LSP 언어 서버. |

## 1. 요구와 설계 원칙

### 1.1 요구와 대응

| 요구 | 설계 대응 | 절 |
|---|---|---|
| 정적 타입 | 모든 선언·식의 타입을 컴파일 시 확정한다. 함수 시그니처는 타입을 명시하고 지역 변수만 추론한다. null 가능성도 타입에 포함한다. | 4 |
| 평범한 문법, Unity 수준의 접근성 | C 계열 문법, Unity hook 체계, 선언 한 줄 노출, 저장 즉시 반영, Unity 대응표. | 2·3·5 |
| 비프로그래머 대상의 방어적·단순한 설계 | 경고 없는 오류, 한 가지 방법, 실수 구조의 언어 차원 제거, 기본·전체 단계 구분. | 1.2·3·4 |
| 메모리·리소스 누수 없음 | 고정 소유자와 해제 연산, 범위 종료 0 검사, soak Gate. | 7·9 |
| 코어·엔진·이벤트 접근 | 리플렉션에서 생성한 binding schema로 C++ 타입·필드·함수·이벤트를 사용한다. | 5·10 |
| 비동기 로딩 | 전체 계획 §9.2 LoadTicket·AssetLease 계약에 대응하는 `Load`·요청 handle·이동·travel. | 7 |
| VM, 사전 컴파일, AOT | 레지스터 VM, `.sparks`, 선택적 C++ 변환. | 11·12 |
| 런타임 eval | 컴파일러를 런타임에 포함할 수 있는 구조, target 정책. | 12.6·12.7 |
| VS Code 자동완성 | 컴파일러를 공유하는 LSP 서버와 VS Code 확장. | 13 |

### 1.2 실수 방지 원칙

| 원칙 | 내용 |
|---|---|
| 경고 없는 오류 | 진단은 오류뿐이며 모든 오류에 원인과 수정 예를 함께 제시한다. 확신할 수 없는 것은 진단하지 않는다(오탐 0).[R21][R25] |
| 한 가지 방법 | 같은 일을 하는 문법을 두 개 두지 않는다. 이벤트 수신은 `on`, 자원 해제는 변수의 범위, 로드는 `Load` 하나다.[R21] |
| 언어 의미로 해결 | 실수가 잦은 구조는 lint 대신 의미를 변경한다.[R22] |
| 4대 버그 차단 | 범위 초과·null·정수 overflow·미초기화 변수를 각각 `T?` 인덱스 읽기, null 타입, overflow fault, zero value와 초기화 필수로 처리한다.[R28] |
| null과 파괴는 하나 | 모든 null 검사가 같은 판정(null 또는 파괴)을 사용한다.[R26] |
| 사본 수정 차단 | 함수 결과는 대입 대상이 될 수 없고 경로 대입은 원본에 기록한다.[R27] |
| 오류 종류 자체를 제거 | 오류 메시지 개선의 효과는 연구마다 결과가 일치하지 않는다.[R29] 메시지는 Elm 방식으로 작성하되[R23], 우선순위는 오류가 발생할 수 없는 구조에 둔다. |

### 1.3 기본 단계와 전체 단계

언어는 하나다. 패키지 manifest의 `profile: core | full`이 사용할 수 있는 문법을 정한다. 콘텐츠 패키지와 Studio의 "새 스크립트"는 core가 기본값이다.

| 단계 | 포함 |
|---|---|
| core | `import`, `class`, `struct`, `enum`, `event`, `fn`/`async fn`, `const`, `var`/`let`, `if`/`for`/`while`/`switch`, `on`, `emit`, `await`, `try`, `override`; `@edit`·`@save`·`@net`·`@formerly`·`@restart`·`@tableRow`; 모든 수명 hook(`Validate` 제외); `All`·`Race`·`Delay`·`NextTick`; `Load`·`Assets.LoadAll`·`Owner.Teleport`·`Session.Travel` |
| full | `interface`, `open`, `Class<T>`, `@rpc`, `@worker`·`@deterministic`·`@pure`, `@tick(Interval: ...)`, `@command`, hook `Validate`, 사용자 정의 오류 타입, 실행 중 component 추가, bundle·우선순위 지정 로드, `World.Prefetch`·스트리밍 source·data layer 제어, 로딩 화면 직접 제어 |

core 패키지에서 full 문법을 작성하면 컴파일 오류이며 기능 이름과 core 대안을 제시한다. core 코드도 full 패키지가 제공하는 함수·이벤트는 호출·수신할 수 있다. 언어 서버는 core 패키지에서 full 전용 후보를 표시하지 않는다.

### 1.4 제외 범위

전체 계획 §23.1의 제외 항목(임의 포인터 연산·unchecked cast·native heap 접근·사용자 정의 allocator)을 유지한다. 추가로 동적 타입, 예외, closure와 함수 값, 명시 구독, 비구조적 coroutine, `defer`, 독립 스크립트 객체, 사용자 정의 generic, 사용자 함수·연산자 오버로딩, 기본 인자, 조건 연산자, 비트 연산자, 문자열 `+`, 강제 unwrap, 변경 가능한 전역 변수를 제외한다.

## 2. 예시와 표기 반영

### 2.1 플레이어 캐릭터

`Characters/PlayerCharacter.spark`:

~~~text
import Weapons.Sword;
import Items.Pickup;

class PlayerCharacter : Character {
    @edit @net var MoveSpeed: float = 600.0;
    @edit let MaxHealth: float = 100.0;              // 생성 후 바뀌지 않는다

    let Trigger = SphereCollider(Radius: 200.0);     // 소유 component
    let Move = Action("Gameplay.Move");              // 입력 action, 컴파일 시 검증
    let Jump = Action("Gameplay.Jump");
    var CurrentTarget: Enemy?;                       // 다른 Entity: 파괴되면 null (기본값 null)
    var EquippedSword: Sword?;

    fn CalculateDamage(base: float, multiplier: float): float {
        return base * multiplier;
    }

    on Start() {
        Trigger.RelativeLocation = vec3(0.0, 0.0, 80.0);
        Log("Rocks Engine & Sparks Ready!");
    }

    on Jump.Pressed() {
        AddImpulse(vec3(0.0, 0.0, 500.0));
    }

    on Tick() {                                      // 인자 dt는 필요할 때만 선언
        let moveInput = Move.Value;                  // vec2
        if (moveInput.Length() > 0.0) {
            let dir = GetForwardVector() * moveInput.Y + GetRightVector() * moveInput.X;
            AddMovement(dir.Normalize() * MoveSpeed); // 초당 속도. dt는 엔진이 적용
        }
        if (let target = CurrentTarget) {
            LookAt(target.Location);
        }
    }

    on TriggerEnter(other: Entity) {                 // 자기 collider의 물리 통지
        if (let pickup = other.Find<Pickup>()) {
            pickup.Collect(this);
        }
    }
}
~~~

### 2.2 전체 계획 §23.1 예시의 확장

상호작용은 서버에서 발행되는 Entity 확장 이벤트로 받고, 모든 기기의 연출은 복제 필드의 변경 이벤트로 처리한다.

`Props/DoorLogic.spark`:

~~~text
import Gameplay.Interaction;     // event Entity.Interacted(actor: Entity);
import Items.Inventory;

enum DoorState { Closed, Opening, Open }

class DoorLogic : ScriptComponent {
    @edit @save var OpenAngle: float = 90.0;
    @edit var Hinge: TransformComponent;               // ? 없음: 같은 prefab 안에서 반드시 지정
    @edit @save var RequiredKey = Tag("Item.Key.Iron");
    @edit var SwingSeconds: float = 0.6;
    @edit var OpenSound: SoundCue = Asset("/Game/Audio/DoorOpen");
    @net var State = DoorState.Closed;

    event Opened(by: Entity);

    on Start() {
        ApplyPose();                                    // 초기값·로드 상태는 Start에서 반영
    }

    on Owner.Interacted(actor: Entity) {                // 상호작용 시스템이 서버에서 발행
        if (State != DoorState.Closed || !HasKey(actor)) { return; }
        State = DoorState.Opening;
        var t = 0.0;
        while (t < 1.0) {
            await NextTick();                           // 실행 중 새 상호작용은 무시된다
            t = Math.Min(t + Time.Delta / SwingSeconds, 1.0);
            Hinge.RelativeRotation.Yaw = OpenAngle * Math.EaseOut(t);
        }
        State = DoorState.Open;
        if (let a = actor) {                            // await 이후 actor는 Entity?
            emit Opened(a);
        }
    }

    on State.Changed() {                                // 모든 기기에서 실행, await 불가
        ApplyPose();
        if (State == DoorState.Opening) {
            World.Audio.PlayAt(OpenSound, Owner.Location);   // 일회성 효과: handle 없음
        }
    }

    fn ApplyPose() {
        if (State == DoorState.Open) { Hinge.RelativeRotation.Yaw = OpenAngle; }
    }

    fn HasKey(actor: Entity): bool {
        if (actor.Tags.Has(RequiredKey)) { return true; }
        let bag = actor.Find<Inventory>() ?? return false;
        return bag.Contains(RequiredKey);
    }
}
~~~

1. `on Owner.Interacted`는 Entity 확장 이벤트다. 전체 계획 예시의 `on Interact`를 interface·message 없이 같은 `on 경로.이벤트` 형태로 처리한다(§5.2).
2. handler 안에서 바로 `await`한다. 같은 발행자에서 온 이벤트로 이 handler가 실행 중이면 새 이벤트는 무시하고 디버거에 횟수를 표시한다(§6.1).
3. `actor`는 전달 시점에 유효하고 첫 `await` 이후 `Entity?`로 바뀐다(§4.3).
4. `Changed`는 초기값·로드·late join 적용 때는 발행되지 않는다. 그래서 소리가 다시 재생되지 않고, 현재 상태 반영은 `Start`에서 한다(§5.3).
5. 전용 서버에서 `World.Audio.PlayAt`은 효과가 없는 호출이다. 서버와 클라이언트가 같은 코드를 실행해도 결과가 달라지지 않는다(§7.5).

### 2.3 제시한 표기의 반영

| 제시 표기 | 반영 | 비고 |
|---|---|---|
| `var MoveSpeed: float = 600.0;`, `fn X(...): float`, PascalCase 멤버, 소문자 내장 타입, `Sword?`, `null`, `@edit @net`, `@edit let`, `if (...)`, 암묵적 `this`, `Log`, 파일 경로 import | 채택 | — |
| `on Start()`, `on Tick(dt: float)` | 채택 | hook은 Unity 체계를 따르고 이름은 Rocks 어휘(§5.1). 인자는 필요할 때만 선언 |
| `Weak<Enemy>`, `Lock()` | 수정 | `Enemy?` 하나로 통합(§4.3) |
| `var Trigger = SphereCollider(200.0);` | 수정 | 소유 component는 `let`, 생성 인자는 필드 이름: `let Trigger = SphereCollider(Radius: 200.0);` |
| `Trigger.SetRelativeLocation(...)` | 수정 | 쓰기 가능한 속성이 있으면 setter 함수를 노출하지 않는다: `Trigger.RelativeLocation = ...` |
| `Input.GetAxis2D("MoveForward", "MoveRight")` | 수정 | action 필드 `let Move = Action("Gameplay.Move")`와 `Move.Value` |
| `AddMovement(dir * MoveSpeed * dt)` | 수정 | 이동 API는 초당 속도를 받고 dt는 엔진이 적용(§10.3) |

### 2.4 Unity 사용자를 위한 대응표

| Unity (C#) | Sparks | 차이 |
|---|---|---|
| `class X : MonoBehaviour` | `class X : ScriptComponent` | — |
| `[SerializeField]`, public 필드 | `@edit`, `@save` | 편집기 노출과 저장을 구분 |
| `Awake` · `OnEnable` · `Start` | `on Awake()` · `on Enable()` · `on Start()` | Start는 기다릴 수 있다. Start가 끝날 때까지 Tick은 시작되지 않는다 |
| `Update` · `FixedUpdate` · `LateUpdate` | `on Tick()` · `on FixedTick()` · `on LateTick()` | Tick 계열에서는 기다리기·생성·자원 획득 불가 |
| `OnDisable` · `OnDestroy` | `on Disable()` · `on Destroy()` | 비활성화되면 실행 중 coroutine 취소, 유지 자원 해제, 이벤트 수신 중지(SD10) |
| `OnTriggerEnter` · `OnCollisionEnter` | `on TriggerEnter(other)` · `on CollisionEnter(other, hit)` | 상대 Entity를 바로 받는다 |
| `OnValidate` | `on Validate()` | full 단계, 저장되지 않는 미리보기 상태만 변경 |
| `GetComponent<T>()` | `Owner.Find<T>()` | `T?` 반환 |
| `StartCoroutine` + `WaitForSeconds` | handler 안에서 `await Delay(2.0, Game)` | owner 비활성화·파괴 시 자동 취소 |
| `Instantiate` · `Destroy(obj)` | `World.Spawn(prefab, transform)` · `obj.RequestDestroy()` | 파괴는 현재 단계 끝에 반영 |
| Addressables `LoadAssetAsync` | `try await X.Load()` | 해제 호출이 없다. 결과를 보관하는 동안 상주한다 |
| `ScriptableObject` | `class X : DataAsset` | 실행 중 읽기 전용 |
| `SendMessage`, `UnityEvent` | `emit` + `on 경로.이벤트` | 타입 검사, 자동 연결·해제 |
| 파괴된 객체의 `== null` | `T?` 검사 | `?.`·`??`·`if (let)`이 모두 같은 판정 |

## 3. 문법

### 3.1 어휘와 명명

소스는 UTF-8이고 식별자는 ASCII로 한정한다. 주석은 `//`, `/* */`, 문서 주석 `///`이다. 문장은 세미콜론으로 끝내고 블록은 중괄호로 감싼다. 서식은 설정 없는 단일 공식 formatter가 정한다.[R21]

| 대상 | 규칙 | 예 |
|---|---|---|
| 키워드·attribute 이름 | 소문자 | `var`, `on`, `@edit` |
| 내장 값 타입(닫힌 목록) | 소문자 | `bool`, `int`, `float`, `string`, `vec2`–`vec4`, `quat`, `mat4` |
| 그 밖의 타입·멤버·이벤트·enum case·attribute 인자 이름·생성 인자 이름 | PascalCase | `DoorState.Open`, `MoveSpeed`, `@edit(Min: 0)`, `Radius:` |
| 지역 변수·함수 인자 | lowerCamel | `moveInput`, `dt` |

- 명명 규칙 위반은 오류다. 이름 가림은 허용하지 않으며, 지역 변수가 멤버·hook 이름·암묵 멤버(`World`·`Owner`·`Session`·`Input`·`Time`, WidgetController의 `Widget`)·다른 지역 변수와 같거나 대소문자만 다르면 오류다.
- prelude 이름(`Log`, `Math`, `Delay`, `Assets` 등)과 hook 이름은 예약되어 멤버 이름으로 쓸 수 없다.
- 오버로딩은 prelude 수학 함수(`Math.Min`·`Max`·`Abs`·`Clamp` 등, `int`와 `float` 공용)에만 있다.

### 3.2 선언과 기본값

| 선언 | 의미 | 예 |
|---|---|---|
| `class` | 엔진 수명 타입(`ScriptComponent`, `Character`, `WorldService`, `WidgetController`) 또는 `DataAsset`을 상속. 기반 타입이 수명을 정한다 | `class DoorLogic : ScriptComponent { }` |
| `struct` | 값 타입. 상속 없음. 스크립트 데이터는 struct로 표현한다 | `struct HitInfo { var Point: vec3; var Damage: int = 1; }` |
| `enum` | 열거. 플래그 조합은 `Set<E>` | `enum DamageKind { Physical, Fire, Poison }` |
| `event` | 멤버 이벤트, Entity 확장 이벤트, World 이벤트 | `event Died(killer: Entity?);` · `event Entity.Interacted(actor: Entity);` |
| `fn`, `async fn` | 함수와 기다리는 함수 | `fn Lerp(a: float, b: float, t: float): float` |
| `const` | 컴파일 시 상수 | `const MaxStack: int = 99;` |

- **기본값(zero value):** 초기값이 없는 필드는 `0`, `0.0`, `false`, `""`, `null`, 빈 컨테이너, enum의 첫 case, 빈 `Soft<T>`·`Row<T>`(로드·조회 시 `NotFound`)로 시작한다. `?` 없는 객체·에셋 필드에는 기본값이 없으므로 `@edit`로 지정되거나 소유 component 생성식이어야 한다. 지역 변수는 선언 시 초기화해야 한다.
- **필드 쓰기:** Sparks 필드는 선언한 클래스 안에서만 쓸 수 있고 밖에서는 읽기만 가능하다. 엔진 객체의 쓰기 가능한 native 속성(`ScriptWritable`)은 밖에서도 쓸 수 있다.
- **함수:** public이다. 엔진 기반 타입의 재정의 가능한 함수는 `override fn`으로 재정의한다.

### 3.3 문장과 식

문장과 식:

~~~text
var speed = 6.0;                              // float로 추론
var hits: Array<HitInfo> = [];
for (item in Items) { }                       // 시작 시점의 내용을 순회한다
for (i, item in Items) { }                    // 번호와 원소
for (_ in 0..count) { }                       // 0 이상 count 미만, 변수가 필요 없으면 _
while (t < 1.0) { await NextTick(); }         // async 문맥 전용. 모든 경로에 await 또는 탈출

switch (State) {                              // enum은 모든 case를 처리해야 하며 default 없음
    case DoorState.Closed: { Open(); }
    case DoorState.Opening, DoorState.Open: { }
}

let label = player?.DisplayName ?? "Unknown";
let text = "HP ${Current}/${MaxHealth}";                 // string. 화면 표시는 Loc
if (let boss = target as? BossEnemy) { boss.Enrage(); }
if (let first = Enemies[0]) { first.Alert(); }          // 인덱스 읽기는 T?
let bag = Owner.Find<Inventory>() ?? return;            // null이면 함수 종료(let 문에서만)
let mesh = try await MeshAsset.Load();                  // 실패하면 이 실행을 끝내고 보고
_ = Inventory.TryAdd(item);                             // 결과를 버릴 때는 _ 로 명시
~~~

- 암묵적 숫자 변환은 없다. 정수 리터럴만 `float` 자리에 쓸 수 있다. 내장 수학 타입은 위치 인자로 생성한다(`vec3(0.0, 0.0, 1.0)`).
- `/`는 항상 `float`를 반환한다. 정수 나눗셈은 `Math.Div(a, b)`, `%`는 정수 전용 내림 방식이다.[R30]
- `float`·vector·struct의 `==`는 오류이며 `Math.NearlyEqual`을 사용한다. 객체 `==`는 동일성이다.
- 문자열 `+`는 없고 보간 `"${x}"`만 사용한다.
- 대입은 문장이다. 조건식은 `bool`만 허용한다.
- 사용하지 않는 지역 변수·import, 버린 결과는 오류다. handler 인자는 앞에서부터 필요한 만큼만 선언하고, 반복 변수가 필요 없으면 `_`를 쓴다. 유지 자원 지역 변수는 보관 자체가 사용이다(§7.1).
- `await`·`try`는 문장 수준(문장 전체, `let` 초기값, 단순 대입 우변, `return`)에서만 쓴다. 복합 대입(`+=` 등)의 우변에는 쓸 수 없다.
- 평가 순서는 항상 왼쪽에서 오른쪽이다. `break`는 반복문만 대상으로 한다.

### 3.4 파일·import·패키지

- 파일 하나가 하나의 모듈이며 패키지 루트 기준 파일 경로가 import 경로다. 이름이 충돌하면 `import Weapons.Sword as Blades;`로 구분한다.
- 파일 사이의 순환 참조는 허용하고 순환하는 `const` 초기화만 오류다. 패키지는 빌드·로드·reload 단위이며 패키지 사이의 순환 의존은 금지한다.
- 엔진 API는 `Rocks` 패키지에 있고 핵심 타입과 `Log`는 prelude로 자동 import한다. 클래스 메서드의 암묵 멤버는 `World`·`Owner`·`Session`·`Input`·`Time`으로 닫혀 있고, WidgetController에는 자신이 제어하는 widget 인스턴스 `Widget`이 추가된다. 오디오·시퀀서 등은 `World.Audio`처럼 이 멤버를 거친다.
- 변경 가능한 전역 변수는 없다. 공유 상태는 `class X : WorldService`로 선언한다(전체 계획 §22.6 PI03).

### 3.5 검증 리터럴

엔진 데이터를 가리키는 문자열은 종류 이름으로 감싼다. 문자열 리터럴만 받으며 컴파일러가 사전과 대조해 안정 ID로 변환한다.

| 리터럴 | 결과 | 런타임 문자열 조회 |
|---|---|---|
| `Tag("Character.Player")` | `Tag` | `Tag.Find(s): Tag?` |
| `Action("Gameplay.Jump")` | `InputAction` | `Input.FindAction(s): InputAction?` |
| `Asset("/Game/Props/Door")` | 기대 타입이 `T`면 하드 참조, `Soft<T>`면 소프트 참조 | `Assets.Find<T>(s): Soft<T>?` |
| `Row("Chest.Common")` | `Row<T>` | `T.FindRow(s)` |
| `Loc("Hud.Hp", Current, MaxHealth)` | `Text`(catalog key와 인자) | `Text.Find(key)` |

런타임 문자열을 이름 타입으로 변환하는 조회는 엔진의 이름 pool에 새 항목을 만들지 않는다(§9.5).

### 3.6 문법 요약

대입 대상(`Target`)에는 함수 호출과 `?.`가 포함되지 않는다. `??` 다음이 `return`·`break`·`continue`이면 `Exit`로 해석한다. 식 안의 `<...>`는 바로 뒤의 토큰이 `(` 또는 `.`일 때 타입 인자로 판정한다. 이름 지정 인자는 생성 호출과 attribute에서만 허용한다.

EBNF:

~~~text
File        = { Import } { Decl } ;
Import      = "import" Path [ "as" Ident ] ";" ;
Path        = Ident { "." Ident } ;

Decl        = { Attribute } ( ClassDecl | StructDecl | EnumDecl | EventDecl | ConstDecl | FnDecl ) ;
Attribute   = "@" Ident [ "(" AttrArg { "," AttrArg } ")" ] ;
AttrArg     = [ Ident ":" ] Expr ;
ClassDecl   = "class" Ident ":" Path "{" { Member } "}" ;
StructDecl  = "struct" Ident "{" { { Attribute } ( FieldDecl | FnDecl ) } "}" ;
EnumDecl    = "enum" Ident "{" Ident { "," Ident } [ "," ] "}" ;
EventDecl   = "event" [ Path "." ] Ident "(" [ Params ] ")" ";" ;
ConstDecl   = "const" Ident ":" Type "=" Expr ";" ;

Member      = { Attribute } ( FieldDecl | FnDecl | EventDecl | Handler ) ;
FieldDecl   = ( "var" | "let" ) Ident [ ":" Type ] [ "=" Expr ] ";" ;
FnDecl      = [ "override" ] [ "async" ] "fn" Ident "(" [ Params ] ")" [ ":" Type ] Block ;
Handler     = "on" Path "(" [ Params ] ")" Block ;
Params      = Param { "," Param } ;
Param       = Ident ":" Type ;
Type        = Path [ TypeArgs ] [ "?" ] ;
TypeArgs    = "<" Type { "," Type } ">" ;

Block       = "{" { Stmt } "}" ;
Stmt        = LocalDecl | If | For | While | Switch | Return | Emit
            | "break" ";" | "continue" ";"
            | Target "=" Value ";" | Target CompoundOp Expr ";"
            | "_" "=" Value ";" | Value ";" ;
Value       = [ "try" ] [ "await" ] Expr ;
LocalDecl   = ( "var" | "let" ) Ident [ ":" Type ] "=" Value [ "??" Exit ] ";" ;
Exit        = "return" [ Expr ] | "break" | "continue" ;
Emit        = "emit" [ Postfix "." ] Ident "(" [ Expr { "," Expr } ] ")" ";" ;
If          = "if" "(" ( Expr | "let" Ident "=" Expr ) ")" Block [ "else" ( If | Block ) ] ;
For         = "for" "(" Binder [ "," Binder ] "in" Expr ")" Block ;
Binder      = Ident | "_" ;
While       = "while" "(" Expr ")" Block ;
Switch      = "switch" "(" Expr ")" "{" { "case" Expr { "," Expr } ":" Block } [ "default" ":" Block ] "}" ;
Return      = "return" [ Value ] ";" ;
Target      = Ident { "." Ident | "[" Expr "]" } ;
CompoundOp  = "+=" | "-=" | "*=" | "/=" ;

Expr        = Unary { BinaryOp Unary } ;
BinaryOp    = "*" | "/" | "%" | "+" | "-" | "??" | ".." | "<" | "<=" | ">" | ">=" | "==" | "!=" | "&&" | "||" ;
Unary       = { "!" | "-" } Postfix [ "as?" Path ] ;
Postfix     = Primary { "." Ident [ TypeArgs ] | "?." Ident | Args | "[" Expr "]" } ;
Args        = "(" [ Arg { "," Arg } ] ")" ;
Arg         = [ Ident ":" ] Expr ;
Primary     = Literal | Ident [ TypeArgs ] | "this" | "(" Expr ")" | "[" [ Expr { "," Expr } ] "]" ;
Literal     = IntLit | FloatLit | StringLit | "true" | "false" | "null" ;
StringLit   = '"' { Char | "${" Expr "}" } '"' ;
~~~

full 단계의 `interface`·`open` 선언 문법은 L0 규격에서 추가한다. 우선순위(높은 것부터): `.` `?.` `()` `[]` → `as?` → 단항 `!` `-` → `*` `/` `%` → `+` `-` → `??` → `..`(`for` 머리에서만) → 비교(결합 없음) → `==` `!=`(결합 없음) → `&&` → `||`. `??`가 비교보다 높으므로 `n?.Count ?? 0 > 5`는 `(n?.Count ?? 0) > 5`다.

## 4. 타입·참조·값

### 4.1 값과 참조

스크립트가 직접 다루는 데이터는 값이다. 참조는 엔진이 수명을 관리하는 객체(Entity·Component·Service·WidgetController)와 에셋뿐이며, 스크립트는 GC로만 회수되는 객체를 생성하지 않는다.

그림 1. 값 영역과 엔진 객체

- 값 영역(VM 소유·참조계수·COW): handler 실행 frame이 Scalar(`s0 int`, `s1 float`)·Ref(`r0 this`, `r1 Door`)·Buf(`b0 Array<Enemy>`) bank를 가진다. Ref는 handle+generation이며 GC root를 만들지 않는다.
- 참조계수 소유: Buf `b0`와 엔진 객체 Door의 `Targets: Array<Enemy>` 필드가 같은 COW 버퍼(rc=2)를 공유한다. 값은 참조계수 버퍼이며 closure가 없으므로 순환이 형성되지 않고 참조계수 0에서 회수된다.
- 약한 참조: Ref `r1`과 버퍼 원소 `[0] Enemy A`·`[1] Enemy B`는 엔진 객체를 약한 handle로 가리킨다. 엔진 객체 참조는 모두 약한 handle이며, 엔진 객체의 수명은 World·Entity의 소유 관계가 정한다.
- 에셋: 에셋 참조는 lease를 공유하는 값이며 보관하는 동안 상주를 유지한다(§7).

본문 규칙은 다음과 같다.

- `string`·`Array`·`Map`·`Set`은 참조계수 버퍼이며 공유 중인 버퍼를 변경할 때 먼저 복사한다.[R13] closure가 없으므로 값 사이 순환이 없고 참조계수만으로 회수된다. VM은 스크립트 상태에 대해 GC root를 제공하지 않는다(전체 계획 §23.2 개정).
- 소유 component 필드(`let Trigger = SphereCollider(...)`)도 handle이다. 소유자는 전체 계획 §20.2대로 Entity이며 필드는 두 번째 소유자가 아니다.
- 참조계수는 Game lane 전용이다. 다른 lane으로 값을 전달할 때는 버퍼를 고정(freeze)하고 참조계수를 1 증가시켜 넘기며, 해제는 Game lane으로 되돌려 처리한다(§9.4).

### 4.2 기본 타입

| 분류 | 타입 | 규칙 |
|---|---|---|
| 논리·숫자 | `bool`, `int`(64비트), `float`(64비트) | 엔진의 32비트 값과 주고받을 때 범위 검사. overflow·NaN·Inf는 fault |
| 수학 | `vec2`–`vec4`, `quat`, `mat4`, `WorldPosition`, `Transform`, `Color` | Core 수학 타입의 바인딩. 성분은 `float`(전체 계획 §2.2) |
| 문자 | `string`, `Text` | 화면 표시 API는 `Text`만 받는다. `Text`는 `Loc(...)`로 생성 |
| 엔진 데이터 | `Tag`, `TagSet`, `InputAction`, `Soft<T>`, `Row<T>` | §3.5 리터럴 |
| 컨테이너 | `Array<T>`, `Map<K,V>`, `Set<T>` | 삽입 순서로 순회. 인덱스·키 읽기는 `T?`. 범위 밖 쓰기는 fault. 불변(invariant) |
| 합성 | `T?`, `Result<T>` | 조회 실패는 `T?`, 작업 실패는 `Result`. `r.Value: T?`, `r.Error: Error?` |
| 클래스 참조 (full) | `Class<T>` | TypeId와 기반 타입 제한. `Create(...)` |

### 4.3 객체 참조와 "없음"

| 위치 | 타입 | 유효 범위 |
|---|---|---|
| `this` | `T` | 실행 중 항상 유효. 재개마다 generation을 확인하고 사라졌으면 실행을 취소 |
| `Owner` | `Entity` | Destroy hook을 제외한 실행 중 유효 |
| 소유 component 필드 | `T` | owner와 같은 수명 |
| `?` 없는 `@edit` 객체 필드 | `T` | 같은 prefab 안의 참조만 허용. spawn 시 해석 실패하면 spawn 실패. 대상이 제거되면 참조하는 component를 비활성화 |
| 그 밖의 객체 필드·컨테이너 원소 | `T?` | 파괴되면 null. 다른 셀의 Entity는 셀이 상주할 때 자동 해석 |
| handler·함수 인자, 지역 변수 | `T` 또는 `T?` | 첫 `await` 이후 `this`가 아닌 객체는 `T?`로 바뀐다 |

- **null 검사는 하나다.** `if (let ...)`, `?.`, `??`, `== null`은 모두 "null이거나 파괴됨"을 같은 판정으로 처리한다.
- **파괴는 단계 끝에 반영된다.** 스크립트와 스크립트가 호출한 native API의 파괴 요청은 현재 단계의 이벤트 전달이 끝난 뒤 반영된다. 같은 단계 안에서 확인한 객체는 단계 끝까지 유효하다(SD3). 생성(`World.Spawn`)은 즉시 수행되며 `Awake`까지 실행한 뒤 반환한다.
- **컨테이너 정리:** 엔진 객체를 원소·키·값으로 가진 컨테이너는 파괴가 반영된 뒤 처음 접근할 때 파괴된 항목을 제거한다. 같은 단계 안에서는 파괴가 반영되지 않으므로 번호가 바뀌지 않는다. 반복 중인 공유 버퍼는 정리하지 않는다.
- downcast는 `as?`만 제공한다. 강제 unwrap은 없다.[R24]

### 4.4 대입 규칙

대입:

~~~text
Hinge.RelativeLocation.Z += 10.0;        // 쓰기 가능한 native 속성: 읽기 → 수정 → 쓰기
Points[i].X = 0.0;                        // 배열 원소에 기록
GetTransform().Location.Z = 1.0;          // 오류: 함수 결과는 대입 대상이 아니다
var p = Hinge.RelativeLocation;
p.Z = 5.0;                                // 오류: 바꾼 사본을 이후에 사용하지 않는다
for (h in Hits) { h.Damage = 0; }         // 오류: 반복 변수는 바꿀 수 없다 → Hits[i].Damage
Target.Current = 0;                       // 오류: Sparks 필드는 선언 클래스 안에서만 쓴다
~~~

- 대입은 지역 변수, 자기 필드, 원소 경로, 쓰기 가능한 native 속성 경로에만 할 수 있다. 경로 중간의 값 타입은 원본에 기록된다. 반복 변수와 함수 인자는 바꿀 수 없다.
- `let` 필드는 생성 후 바뀌지 않는다. C++ `ReadOnly` 필드는 엔진 쪽에서 바뀔 수 있는 읽기 전용 속성으로 표시한다.

### 4.5 생성과 기본값 순서

- struct와 component는 필드 이름으로 생성한다(`HitInfo(Point: p)`). 지정하지 않은 필드는 기본값을 사용한다.
- component는 `let` 필드의 초기값으로만 생성한다(실행 중 추가는 full). 필드 초기값은 상수식·리터럴·component 생성만 허용하고 ClassDefaults archetype에 반영한다. C++ 생성자 안에서 스크립트 코드를 실행하지 않는다.
- 기본값 순서는 전체 계획 §5.3을 따른다: native → Sparks 필드 초기값 → archetype → prefab·variant → instance override → spawn parameter. 코드의 기본값 변경은 override하지 않은 instance에 반영하고 override한 instance를 편집기에 표시한다.
- 소유 component 필드에도 안정 ID(SubobjectGuid)를 부여하므로 이름을 바꿔도 prefab override가 유지된다(전체 계획 §5.3).

## 5. 수명 hook과 이벤트

### 5.1 수명 hook

Unity의 hook 체계(종류·호출 순서·의미)를 따르고 이름은 Rocks 어휘를 사용한다. Unity의 `On` 접두는 `on` 키워드가 대신한다. 모든 엔진 수명 타입(component·service·widget controller)에 같은 hook을 적용한다.

| Sparks | Unity 대응 | 전체 계획 단계 | C++ 가상 함수(SD9) | 규칙 |
|---|---|---|---|---|
| `on Awake()` | Awake | Initialize | `OnAwake` | 같은 spawn 묶음의 모든 Awake가 끝난 뒤 Start가 시작된다. await·생성·자원 획득 불가. fault면 spawn 실패 |
| `on Enable()` | OnEnable | Activate | `OnEnable` | 이벤트 연결 활성화. 유지 자원 획득 가능 |
| `on Start()` | Start | BeginPlay | `OnStart` | 처음 활성화될 때 한 번. 기다릴 수 있으며 끝나기 전에는 Tick이 시작되지 않는다. 완료 전에 비활성화되면 취소되고 다시 활성화될 때 처음부터 실행된다 |
| `on Tick(dt)` · `on FixedTick(dt)` · `on LateTick(dt)` | Update · FixedUpdate · LateUpdate | 가변·고정·LateUpdate tick group | `OnTick` 등 | Tick 문맥: await·생성·자원 획득·로드 불가(§6.2) |
| `on Disable()` | OnDisable | Deactivate | `OnDisable` | hook 실행 후 엔진이 실행 중 coroutine을 취소하고 유지 자원을 해제하며 이벤트 연결을 비활성화한다(SD10) |
| `on Destroy()` | OnDestroy | EndPlay | `OnDestroy` | Teardown 문맥: `this`와 자기 필드만 접근, 일회성 효과 허용, await·생성·자원 획득 불가 |
| `on TriggerEnter(other, trigger)` · `TriggerExit` · `on CollisionEnter(other, hit)` · `CollisionExit` | `OnTrigger*` · `OnCollision*` | PostPhysics 물리 이벤트 batch | `OnTriggerEnter` 등 | 자기 Entity의 collider만. 값으로 복사한 이벤트를 generation 재검증 후 전달(전체 계획 §16.2) |
| `on Validate()` (full) | OnValidate | EditorConstruct | `OnValidate` | 편집기 전용, 저장되지 않는 미리보기 상태만 변경 |

- hook은 누적된다. 기반 클래스의 hook이 먼저 실행되고 파생 클래스의 것이 이어서 실행된다.
- `fn Update()`·`fn Start()`처럼 hook·Unity hook 이름의 함수는 오류이며 해당 `on` 형태를 제시한다.
- C++ 가상 함수에 `On` 접두를 쓰면 오디오·타임라인의 일반 `Start()` 메서드와 충돌하지 않는다(SD9).
- 이 절과 §5.2의 표는 handler 인자 타입을 생략하였다. 선언할 때는 `on Tick(dt: float)`, `on TriggerEnter(other: Entity, trigger: Collider)`처럼 타입을 명시한다.

### 5.2 이벤트: on 경로.이벤트

이벤트:

~~~text
class Health : ScriptComponent {
    @edit @save var MaxHealth: int = 100;
    @net var Current: int = 100;
    event Died(killer: Entity?);

    fn ApplyDamage(amount: int, instigator: Entity?) {
        if (Current == 0) { return; }
        Current = Math.Max(0, Current - amount);
        if (Current == 0) { emit Died(instigator); }
    }
}

class HealthBar : WidgetController {
    @edit var Target: Health?;
    @edit var Guards: Array<Guard>;

    on Start() { Refresh(); }
    on Target.Current.Changed() { Refresh(); }                   // 다른 객체 필드의 변경
    on Target.Died() { Hide(); }                                  // 인자는 필요한 만큼만
    on Guards.Died(guard: Guard) { Mark(guard); }                // 배열: 첫 인자는 발행한 객체
    on Widget.RetryButton.Clicked() { Session.RequestRespawn(); }
}
~~~

| 경로 | 예 | 의미 |
|---|---|---|
| 자기 필드 | `on State.Changed()` | 필드 값이 바뀐 뒤 |
| 객체 필드 | `on Target.Died()` | 필드가 가리키는 객체의 이벤트. 필드 값이 바뀌면 다시 연결 |
| 배열 필드 | `on Guards.Died(guard)` | 배열의 서로 다른 객체마다 한 번 연결. 첫 인자는 발행한 객체 |
| `Owner` | `on Owner.Interacted(actor)` | 소유 Entity의 이벤트와 Entity 확장 이벤트 |
| 입력·UI | `on Jump.Pressed()`, `on Widget.Ok.Clicked()` | action 필드와 widget 경로. 사전·WidgetAsset schema와 컴파일 시 대조 |
| `World` | `on World.MatchStarted()` | World 이벤트(session의 World service proxy를 통해 연결) |

- handler 인자는 이벤트 인자 목록의 앞에서부터 필요한 만큼만 타입과 함께 선언한다.
- 멤버 이벤트는 선언한 클래스 안에서만 `emit`한다. Entity 확장 이벤트와 World 이벤트는 import한 코드가 `emit target.Interacted(player)`로 발행한다.
- 해석할 수 없는 경로·이벤트, 시그니처 불일치는 오류다(유사 이름 제시).

### 5.3 Changed

- Sparks 필드와 변경 통지(CU5)를 지원하는 native 필드에만 있다. 그 밖의 필드에 연결하면 오류다.
- 한 전달 단위 안의 여러 변경은 1회로 합친다. 스크립트·저장 로드·복제 등 누가 썼는지와 무관하게 같은 규칙이다.
- **초기값 적용, 저장 로드, late join의 상태 적용에서는 발행하지 않는다.** 현재 상태의 반영은 `Start`에서 한다. 일회성 연출(소리)이 다시 재생되지 않는다(전체 계획 §17.3).
- `Changed` handler에서는 `await`할 수 없다. handler가 같은 필드를 다시 쓰면 다음 전달 단위에 새 `Changed`가 등록되며 연쇄 상한이 적용된다.

### 5.4 전달 시점: 단계

Sparks 코드는 엔진의 정해진 지점(입력 batch, 물리 이벤트 batch, tick group, coroutine 재개)에서 실행되며 이 실행 묶음을 **단계**라고 한다.

실행 조각(hook·handler·재개) → 발행 큐(FIFO · emit·Changed) → 다음 실행 조각(연쇄 상한 16회) → 파괴 반영(전체 계획 §20.2 deferred command) → 다음 단계

- `emit`은 큐에 등록되고, 지금 실행 중인 조각이 반환하거나 `await`에 도달한 뒤 같은 단계 안에서 발행 순서대로 전달된다. C++ handler도 이 전달 시점에 호출되므로 handler가 다른 handler 도중에 끼어드는 재진입이 없다.
- 한 이벤트의 handler는 연결 순서로 호출하므로 결과가 결정적이다.
- 한 단계의 연쇄 횟수(16회)와 전달 수에 상한이 있다. 초과하면 발행한 실행의 fault로 보고하고 그 연쇄의 남은 payload를 폐기한다.
- 파괴는 전달이 끝난 뒤 반영하므로 이벤트 인자로 전달된 객체는 전달 시점에 유효하다.

### 5.5 연결의 저장과 해제

- **저장:** 발행자 쪽은 Core `MulticastDelegate`다. 이벤트 필드가 없는 확장 이벤트·native 필드 `Changed`·World 이벤트는 발행자 객체에 지연 할당하는 이벤트 slot 표에 저장하며 그 객체와 함께 해제된다.
- **항목:** Sparks 연결 항목은 session 연결 표의 (slot, generation) key와 handler ID만 가진다. 호출되면 실행을 큐에 등록할 뿐 스크립트 코드를 직접 실행하지 않는다. revision lease·raw pointer를 보관하지 않는다.
- **구독자가 먼저 끝날 때:** 구독자 Detach가 자기 key를 폐기하면 항목은 즉시 호출 대상에서 제외되고, 발행자의 다음 compact에서 제거된다.
- **발행자가 먼저 끝날 때:** 발행자의 Detach 또는 BeginDestroy가 delegate를 비우고, 각 항목의 소멸이 연결 기록을 해제한다. GC 경로로 회수되는 경우에도 BeginDestroy가 정확히 1회 실행되므로 누락이 없다.
- **필드·배열 원천:** 필드가 바뀌면(스크립트·로드·복제 모두) 실행 조각이 끝날 때 서로 다른 발행자 집합을 비교해 추가·제거한다. 같은 객체가 배열에 여러 번 있어도 연결은 하나이고 handler는 이벤트마다 1회 호출된다.
- **reload:** `on` 선언이 추가·삭제되면 살아 있는 instance의 연결을 reload 경계에서 다시 구성한다.
- **session 범위:** 스크립트는 자기 session이 소유한 발행자에만 연결한다. 입력 action은 session의 InputConsumerScope 안의 action instance에, 엔진 전역 이벤트는 session World service proxy에 연결한다. 공유 에셋은 이벤트를 노출하지 않는다.
- **Core 변경:** 생존 판정을 외부에서 제공하는 회수 가능 대상(C1), `TryAdd`의 상한 compact(죽은 항목이 직전 compact 후 살아 있던 수 + 8을 넘지 않음, C2), 단일 pass compact(C3), `CompactDead`·`Stats`(C4). compact는 할당하지 않는다.

## 6. 시간과 async

### 6.1 handler 안에서 기다리기

coroutine(싱글플레이 예):

~~~text
class CutsceneTrigger : ScriptComponent {
    @edit var Cutscene: Soft<Sequence>;
    let Skip = Action("UI.Skip");

    on TriggerEnter(other: Entity) {
        if (!other.Tags.Has(Tag("Character.Player"))) { return; }
        let ctx = Input.PushContext(Asset("/Game/Input/Cinematic"));  // 이 블록이 끝나면 해제
        let seq = try await Cutscene.Load();
        await World.Sequencer.Prepare(seq);                 // texture mip·오디오를 미리 상주
        await Race(World.Sequencer.Play(seq), Skip.Pressed);    // 먼저 끝난 쪽만 남고 나머지는 취소
    }
}
~~~

- `on` handler는 바로 `await`할 수 있다(Awake·Enable·Disable·Tick 계열·Changed·Destroy 제외). 다른 함수에서 기다리려면 `async fn`으로 선언하고 호출할 때 `await`해야 한다.
- **재진입:** 기다리는 handler가 같은 발행자의 이벤트로 실행 중이면 새 이벤트는 무시하고 디버거에 횟수를 표시한다. 발행자가 다르면 따로 실행된다. `@restart`를 붙이면 실행 중인 것을 취소하고 다시 시작한다. 기다리지 않는 handler는 즉시 끝나므로 무시되는 경우가 없다.
- **구조적 동시성:** `All`·`Race`의 인자는 기다릴 수 있는 식(async 호출, `Delay`, 이벤트, 로드 요청)이며 `await` 없이 쓴다. `Race`는 진 쪽을 즉시 취소하고 같은 tick이면 인자 순서로 판정한다. 둘 다 결과를 반환하지 않는다. 하위 작업의 fault는 부모 실행의 fault가 되어 형제를 취소한다.[R19]
- **취소:** 취소는 `await` 지점에서만 일어나고, 실행이 얻은 유지 자원은 엔진이 해제한다. owner 비활성화·파괴, World 종료, PIE Stop, 호환되지 않는 reload가 취소 원인이다.
- **시간:** `Delay(seconds, Game)`은 pause·배율을 반영하고 `Delay(seconds, UI)`는 반영하지 않는다(전체 계획 §8.3의 GameClock·UIClock). `NextTick()`은 다음 가변 tick까지, `NextFixedTick()`은 다음 고정 tick까지 기다린다. 경과 시간은 `Time.Delta`다.
- **이벤트 대기:** `await X.Event`는 발행되면 `true`, 발행자가 파괴 요청되거나 회수되면 그중 먼저 일어난 시점에 `false`를 반환한다. 기다리기 전에 발행된 이벤트는 받지 않는다.
- `while`은 async 문맥에서만 쓰며 반복 본문의 모든 경로에 `await` 또는 탈출이 있어야 한다.
- 재개는 Game lane의 지정 단계에서 (단계, tick, owner 안정 key, 순번) 순서로 수행한다(전체 계획 §8.2·§8.4).

### 6.2 실행 문맥

함수와 native API에 실행 문맥을 표시하고 컴파일러가 호출 그래프 전체에 전이적으로 검사한다. 재정의 함수는 기반 함수보다 넓은 문맥을 요구할 수 없다.

| 문맥 | 진입 | 금지 |
|---|---|---|
| Game | 기본 | — |
| Init | `on Awake` | await, 생성, 자원 획득, 로드 |
| Toggle | `on Enable`·`on Disable` | await. `on Disable`에서는 자원 획득도 금지(hook 직후 엔진이 해제하므로) |
| Tick | `on Tick`·`FixedTick`·`LateTick` | await, `World.Spawn`, 자원 획득, 로드. Tick에서 `emit`한 이벤트의 handler는 자기 문맥으로 실행되므로, 간접 생성 증가는 생성 위치별 예산과 증가 추세 검사로 검출한다(§9.3) |
| Reaction | `on X.Changed` | await |
| Teardown | `on Destroy` | await, 생성, 자원 획득, 다른 객체 접근. 할당은 허용하되 GC safepoint를 열지 않고 별도 실행 상한 적용(전체 계획 §5.2) |
| EditorConstruct (full) | `on Validate` | spawn, IO, RPC, 저장 필드 쓰기 |
| Pure (full) | `@pure fn` | 모든 쓰기. StateTree 조건·UI binding |
| Worker (full) | `@worker fn` | 엔진 객체 참조, 자원 handle. 값은 고정 후 전달 |
| Deterministic (full) | `@deterministic fn` | wall clock, IO, 비고정 RNG, `NextFixedTick` 외의 await(입력 상세안 §23.2) |

## 7. 유지 자원과 비동기 로딩

### 7.1 유지 자원

멈출 때까지 계속되는 것(반복 사운드·지속 VFX·입력 context·로딩 요청·표시한 widget·카메라 점유)은 **유지 자원**이다. 사용자가 알아야 할 규칙은 3개다.

1. **담아야 만들 수 있다.** 유지 자원은 필드나 `let`/`var` 지역 변수에 대입할 때만 생성할 수 있다. 결과를 버리면 "멈출 방법이 없다"는 오류다. 일회성 효과(`World.Audio.PlayAt`)는 handle 없이 호출한다.
2. **변수가 끝나면 멈춘다.** 필드에 새 값이나 `null`을 넣을 때, 지역 변수의 블록이 끝날 때(fault·취소 포함), 객체가 비활성화·종료될 때 멈춘다.
3. **옮길 수 없다.** 복사, 배열·Map·struct·이벤트 인자·함수 인자·반환값·다른 객체·`@save`/`@edit`/`@net` 필드 보관은 오류다.

유지 자원:

~~~text
var Hum: Sound?;
on Enable() { Hum = World.Audio.PlayLoop(EngineCue, Owner); }   // 이전 값이 있으면 먼저 해제
on Owner.Alarm() {
    let siren = World.Audio.PlayLoop(SirenCue, Owner);           // 이 블록이 끝나면 멈춤
    await Delay(5.0, Game);
}
fn Quiet() { Hum?.Pause(); }                                      // 담긴 자리에서 호출
~~~

- 대입 순서는 새 자원 획득 → 이전 자원 해제 → 저장이다. 획득이 할당량·예산으로 실패하면 결과는 `null`이고 디버거에 보고한다.
- 필드 초기값에서 획득할 수 없고(상수식만 허용), 필드가 reload로 삭제되면 그 자원을 해제한다. 타이머 handle은 없다. 기다림은 `await Delay`, 반복은 handler 안의 반복문이다.
- binding schema가 자원 종류(유지·일회성)를 정하므로 사용자는 반환 타입으로만 구분한다(§10.2).

### 7.2 에셋 참조

- **하드 참조** `@edit var OpenSound: SoundCue = Asset("...")`: 이 클래스를 사용하는 prefab·셀의 의존성으로 함께 로드된다(스크립트 모듈의 의존성이 아니다). 함수 본문의 하드 `Asset(...)` 리터럴도 같은 방식으로 클래스의 의존성이 된다. instance override가 값을 바꾸면 바뀐 에셋이 의존성이 된다. null이 아니다.
- **소프트 참조** `Soft<T>`: AssetId만 가진다. `T`가 필요한 자리에 쓰면 오류이며 빠른 수정이 `try await X.Load()`를 삽입한다.
- prefab·PrimaryAsset별 하드 의존 크기 예산을 두고 초과하면 쿡 오류다. 셀 경계를 넘는 하드 edge도 쿡이 진단한다(전체 계획 §10.2).
- 로드한 결과 `T`는 lease를 공유하는 복사 가능한 값이며, 보관하는 동안 상주를 유지한다. `@net`·RPC·`@save`에는 `T`를 둘 수 없고 `Soft<T>`를 쓴다(빠른 수정 제공).

### 7.3 로드

로드:

~~~text
let fx = try await HitFx.Load();                  // 기본 형태: 실패하면 이 실행을 끝내고 보고
World.Effects.PlayAt(fx, Owner.Location);
Fx = try await HitFx.Load();                       // 필드에 보관: owner가 끝날 때까지 상주

async fn LoadPreviews() {                          // 진행률이 필요할 때
    let req = Assets.LoadAll(Previews);            // 지역 전용 요청 handle (유지 자원)
    while (!req.IsDone) {
        Widget.Progress.Value = req.Progress;
        await NextTick();
    }
    Loaded = try await req;                        // Array<Texture>, 이후 Loaded가 상주를 유지
}
~~~

- `X.Load()`와 `Assets.LoadAll(list)`는 요청 handle을 반환한다. 요청 handle은 지역 변수에만 둘 수 있고 한 번만 `await`한다. 상주를 유지하는 방법은 결과 값을 보관하는 것 하나다.
- `LoadAll`은 전부 성공하거나 하나의 오류를 반환하고 전체 진행률을 제공한다.
- 같은 package 요청은 엔진이 하나의 로드로 합친다. 한 caller의 취소는 다른 caller의 요청을 취소하지 않는다(전체 계획 §9.2).
- 결과는 현재 target이 요구하는 준비 mask(CPU 객체·render·physics 등)를 충족한 뒤 반환된다. 임시 fallback 표시 상태를 완료로 보지 않는다.
- 해제된 package는 상한이 있는 cache(전체 계획 §29.6 allowlist)에 잠시 남아 같은 에셋을 반복 로드할 때의 비용을 줄인다. 같은 호출 위치의 반복 로드는 ledger에 표시한다.
- 시간 제한은 `await Race(req, Delay(5.0, UI));` 다음 `req.IsDone`을 확인한다. `All`·`Race`의 인자로 쓰는 것은 요청 handle의 1회 `await`로 세지 않는다. 로드가 먼저 끝났으면 `try await req`로 결과를 즉시 받고, 시간이 먼저 끝났으면 요청은 취소되어 `IsDone`이 `false`로 남으며 다시 기다리면 오류다.

### 7.4 실패·취소와 전체 계획 §9.2 계약

- 결과는 `Result<T>`다: `NotFound`(삭제된 에셋, 예: 오래된 저장 데이터), `LoadFailed`(IO·검증·등록 실패). 등록 자체가 실패하면 이미 종료된 요청을 반환하고 `await`에서 `LoadFailed`를 받는다.
- 실행이 취소되면 요청 handle이 해제되어 이 caller의 관심만 반환한다. 결과를 받을 frame이 없는 완료는 binding의 고립 완료 경로가 lease를 반환한다(NL03 fixture).
- 한 LoadTicket에는 OnReady callback이 하나뿐이므로 같은 요청을 기다리는 여러 대기자는 binding이 분배한다.
- 쿡은 스크립트 상수·`@edit` 필드·DataTable 행에서 도달 가능한 모든 소프트 대상을 chunk에 배정하고, 배정되지 않으면 쿡 오류로 처리한다.
- owner별 진행 중 로드 수에 상한을 두어 취소된 로드가 누적되지 않게 한다.
- 로드한 값을 범위가 다른 owner(예: World 범위 실행 → Session 범위 service 필드)에 저장하면 엔진이 새 범위로 lease를 다시 발급한다. scope revoke 순서는 실행 취소 → owner 정상 해제 → revoke이며, revoke 시점에 남은 항목은 ledger 실패다.
- 편집기에서 에셋이 다시 import되면 스크립트가 보유한 lease를 단계 경계에서 새 revision으로 교체한다(편집기 전용).

### 7.5 서버와 준비 mask

- 준비 mask는 타입이 요구하는 mask와 target capability의 교집합이다. 전용 서버에서 외형 전용 타입(texture·사운드 등)의 로드는 즉시 효과 없는 stub으로 완료되고, 그것을 받는 API는 효과가 없다. 서버 쿡에서 제거된 하드 참조도 같은 stub이다. 따라서 같은 코드가 서버에서 실패해 보상 지급 등이 누락되는 일이 없다.
- PIE의 서버 session은 서버 capability와 서버 쿡 filter를 적용해 이 경로를 출시 전에 시험한다.
- 복제 spawn된 소프트 prefab은 클라이언트가 fixup 전에 로드한다. `@net` `Soft<T>` 필드의 `Changed`에서는 명시적으로 로드한다.

### 7.6 생성·이동·travel·스트리밍

- **생성:** `World.Spawn(prefab, at)`은 상주한 prefab만 받는다. 소프트 prefab은 `let p = try await DoorRef.Load(); let door = try World.Spawn(p, at);`로 쓴다. 생성된 Entity는 prefab 의존 묶음의 lease를 스스로 보유하므로 `p`가 사라져도 prefab이 해제되지 않는다.
- **이동:** `try await Owner.Teleport(position)`은 목적지 prefetch·commit gate를 내부에서 처리하고 관심을 streaming source로 이관한다(전체 계획 §10.2). 직접 prefetch(`World.Prefetch`)는 full이다.
- **travel:** `Session.Travel(map): Result<void>`은 서버·단독 실행 전용이며 기다리지 않고 요청만 접수한다. 이전 World에 속한 호출 실행은 travel 과정에서 정상 종료된다. 중복 호출은 `AlreadyTraveling`이다. 로딩 화면은 엔진이 프로젝트 설정으로 관리하여 PrepareDestination에 표시하고 새 World의 초기 준비 mask가 충족된 뒤 숨긴다(전체 계획 §10.3). 직접 제어하는 로딩 화면·seamless travel은 full이다.
- **다른 셀의 Entity:** `@edit var Target: Door?`가 다른 셀의 Entity를 가리키면 SoftEntityId로 저장되고, 셀이 상주할 때 자동 해석되며 unload되면 null이다. `on Target.Opened` 연결도 이를 따른다. 셀 unload는 그 셀 객체들의 정상 Disable·Destroy이며 scope revoke가 아니다.
- **data layer(full):** `World.SetDataLayer(layer, Active)`는 서버 전용이며 복제·저장되는 World 상태다. 준비는 `await World.DataLayerReady(layer)`로 기다린다. late join 클라이언트는 현재 상태를 받는다.
- **컷씬 준비:** `await World.Sequencer.Prepare(seq)`는 shot 예측으로 texture mip·오디오 chunk를 미리 상주시킨다. 일반 로드의 준비 mask에는 이것이 포함되지 않는다(전체 계획 §9.5).

## 8. 오류 처리

### 8.1 컴파일 진단

진단 예:

~~~text
error S0412: Target은 null일 수 있다              UI/HealthBar.spark:14:5
    Target.Refresh();
    ^^^^^^
  Target의 타입은 Health?이다. 다른 Entity는 파괴되면 null로 읽힌다.
  수정 예:  if (let target = Target) { target.Refresh(); }

error S0731: 함수 결과에는 대입할 수 없다          Characters/PlayerCharacter.spark:42:5
    GetTransform().Location.Z = 1.0;
    ^^^^^^^^^^^^^^
  GetTransform()은 값의 사본을 반환하므로 대입해도 원본이 바뀌지 않는다.
  수정 예:  RelativeLocation.Z = 1.0;
~~~

진단은 원인 하나를 작성한 코드 그대로 보이고 수정 예를 포함한다. 한 원인에서 파생된 연쇄 오류는 표시하지 않는다.[R23]

### 8.2 Result와 try

- 조회 실패는 `T?`, 작업 실패는 `Result<T>`다. 결과를 사용하지 않으면 오류다. 직접 처리할 때는 `r.Value`·`r.Error`를 확인한다.
- `try`는 실패 시 현재 함수를 끝낸다. `Result`를 반환하는 함수에서는 실패를 전달하고, handler에서는 그 실행을 끝내고 원인을 보고한다. `Start` 안의 `try` 실패는 Start fault다.

### 8.3 fault와 실행 단계별 처리

| fault | 원인 |
|---|---|
| `IndexOutOfRange` | 범위 밖 원소 쓰기(읽기는 `T?`) |
| `IntegerOverflow`, `DivideByZero` | 정수 overflow, `Math.Div`·`%`의 0 |
| `NonFinite` | NaN·Inf를 만든 연산 |
| `BudgetExceeded`, `CascadeLimit` | 명령어·재귀·메모리 예산, 이벤트 연쇄 상한 초과. 메모리 예산 초과는 할당 위치를 보고 |
| `AssertFailed` | 모든 빌드에서 검사하며 조건식은 Pure 문맥 |
| `ContractViolation` | native API의 계약 위반(파괴 지연 미준수 등) |

| fault 위치 | 처리 |
|---|---|
| 필드 초기값·Awake | spawn 실패(`Result`), 미공개 graph rollback(전체 계획 §5.3) |
| Enable·Start | 해당 instance 비활성화(Detach 수행). Destroy는 항상 실행 |
| Tick 계열 | 해당 instance의 Tick을 reload 전까지 중지 |
| 그 밖의 handler·coroutine | 그 실행만 중단하고 실행 범위를 해제. 10초 안에 3회 fault면 instance를 격리(Detach 후 검사용 상태 보존) |
| 원격 client가 유발(RPC 처리 중) | instance·session에 영향 없음, 해당 connection에 집계 |

- **예산:** 엔진이 Sparks를 호출하는 지점마다 명령어 수 예산 하나를 그 실행 전체가 공유한다. 예산은 플랫폼 allocator가 아닌 논리 단위(명령어 수, 원소 수 × 원소 크기)로 계산하므로 모든 플랫폼·빌드·재시뮬레이션에서 결과가 같다.
- **보고:** fault는 (모듈 hash, 함수, 위치, 종류)별로 1회 상세 보고하고 이후 횟수만 집계한다. PIE는 새 fault 위치에서 정지한다. 출시 빌드는 hash·위치를 기록해 오프라인에서 소스 위치로 변환한다.
- 개발·출시 빌드는 같은 검사를 수행한다. 이미 엔진에 요청한 외부 효과는 되돌리지 않는다(전체 계획 §23.4).

## 9. 소유·해제와 누수 방지

전체 계획 §29.6은 메모리 누수 0을 출시 Gate로 두고 모든 allocation·resource에 owner scope와 최종 회수 조건을 명시하도록 규정한다. Sparks는 소유자를 고정하고 해제 경로를 3개 연산으로 한정한다.

### 9.1 소유자와 해제 연산

소유자는 **Session**(PlaySession 또는 process host), **Instance**(스크립트 수명 객체), **Execution**(handler 실행 1회와 그 `All`/`Race` 하위), **ModuleHost**(코드) 넷이다. 스크립트는 소유자를 새로 만들지 않는다. 해제는 다음 연산으로만 일어나며, 이 연산은 사용자 코드를 실행하지 않고 할당하지 않고 멈추지 않으므로 실패할 수 없다.

| 연산 | 수행 내용 |
|---|---|
| `EndExec(frame)` | 하위 실행 종료 → 대기 key 폐기 → 지역 유지 자원을 역순 해제 → 값 슬롯 해제 → revision lease 반환 |
| `Detach(instance)` | admission 차단 → 모든 실행 EndExec → 필드 유지 자원을 선언 역순 해제 → 연결 key 폐기 → 자기 이벤트 delegate 비우기 → 값 필드 초기화 |
| `CloseSession` | admission 차단 → 모든 instance Detach → 큐·Changed snapshot 폐기 → World 파괴 → 연결 표 해제(이 시점에 ledger 0) |

| 종료 경로 | 적용 |
|---|---|
| 정상 종료·fault·취소 | EndExec(fault·취소는 하위 트리 전체) |
| 비활성화·파괴 | Disable 후 실행·유지 자원·연결 해제, Destroy hook 안에서 Detach |
| GC가 도달 불가로 판정 | BeginDestroy에서 Detach(정확히 1회 보장, 전체 계획 §5.2) |
| 셀 일괄 unload | 묶음 Detach. 발행자별 compact는 1회 |
| PIE Stop | CloseSession |
| reload | 변경된 `on` 선언과 필드만 해제. 실행 중 frame은 이전 revision 유지 |
| process 종료 | 모든 CloseSession → ModuleHost → TypeRegistry → NamePool Shutdown. 정해진 시간 안에 Busy가 해소되지 않으면 강제 해제하지 않고 NL06 실패로 보고 |

### 9.2 대상별 소유와 해제 시점

| 대상 | 소유자 | 해제 시점 |
|---|---|---|
| 값 버퍼(string·Array·Map) | 보관한 슬롯(참조계수), 회계는 owner | 참조계수 0 |
| 이벤트 연결 | Instance의 연결 집합 | Detach, 원천 필드 변경, reload의 선언 삭제 |
| 이벤트·시간·로드 대기 | Execution | 발생 시 또는 EndExec. 발행자 소멸 시 `false`로 재개 |
| 실행 | handler slot 또는 부모 실행 | 반환·fault·취소·Detach |
| 발행 큐 항목 | Session | 전달, 대상 소멸 시 폐기, CloseSession |
| 유지 자원 | 담긴 필드 또는 지역 변수 | 대입·`null`·블록 종료·EndExec·Detach. backend 완료가 늦으면 BackendLease로 보호하고 폐기된 slot에 도착한 완료는 버린다 |
| 일회성 효과 | 엔진 subsystem | 최대 지속 시간 후 스스로 종료. Stop 시작 후에는 효과 없음 |
| 에셋 lease | 결과 값을 보관한 슬롯·생성된 Entity | 참조계수 0, Entity 파괴 |
| 생성된 Entity | World 또는 부착 부모 | 단계 끝 파괴, 부모 파괴, CloseSession. 설계상 상한이 없는 유일한 증가이므로 검출 대상 |
| code revision | ModuleHost, instance·frame이 lease | 마지막 lease 반환. process 단위 ledger로 검사 |
| eval·외부 모듈 | EvalScope 또는 ModuleLease | 범위 종료, 비동기 unload(§12.7) |
| 진단 표 | session ring, 위치별 counter는 revision(hash key) | ring 덮어쓰기, revision 반환. 강한 참조를 보관하지 않음 |

### 9.3 예산과 Gate

- **예산(논리 단위):** instance별 VM 메모리(공유 버퍼는 보유자마다 계산), 종류별 유지 자원, 연결, 대기, 진행 중 로드; session별 합계와 단일 버퍼 크기; 단계당 실행 수·연쇄 16회·`All` 분기 1,024; 생성자별 생성 Entity 수(표시); 모듈별 revision 4개(표시); eval scope 수. 상한은 획득 시점에 거부하며 해제 경로에서는 적용하지 않는다. 예산 압박은 로드 admission을 지연시키고 실패시키지 않는다.
- **매 단계 디버그 불변식:** 연결 표의 살아 있는 key 수와 항목 수 일치, 발행자별 죽은 항목 ≤ 직전 compact 시점의 살아 있는 항목 + 8(C2), instance별 유지 자원 ≤ 필드 수 + 활성 frame의 지역 수.
- **G1 · PIE Stop:** 모든 스크립트 counter 0, 연결 표 참조계수 1, revision lease와 이름 pool이 기준선 복귀(전체 계획 §29.6 NL04).
- **G2 · 2시간 soak:** 생성·파괴 반복, 배열 재작성, 5분마다 reload, 10초마다 eval. 30초 구간별 최솟값이 10구간 중 8구간 이상 상승하고 기울기가 예산의 분당 1%를 넘는 counter가 없어야 한다. 위반 시 생성 위치별 증가량 상위 항목을 보고한다.
- **G3 · 음성 대조:** key 폐기 생략, C2 비활성, 취소 시 지역 자원 누락, 이름 pool 삽입 등 의도한 누수가 Gate를 실패시키는지 확인한다.
- **G4 · 실패 행렬:** 모든 획득·`TryAdd`·재연결에서 N번째 실패, 모든 `await`에서 fault·취소를 주입하고 Detach 후 모든 counter 0을 확인한다.

### 9.4 lane과 일시 정지

- VM 버퍼는 Game lane에서만 변경한다. 비동기 저장·복제 직렬화·디버거 watch·telemetry는 고정된 버퍼를 읽고 해제를 Game lane으로 되돌리며, ledger 검사 전에 이를 모두 처리한다.
- Sparks handler는 Game lane에서 발행되는 이벤트에만 연결한다. 다른 lane의 이벤트는 엔진이 POD로 전달하고 Game lane에서 값으로 생성한다.
- breakpoint·pause 중 쌓이는 큐(lane 간 전달, UI clock 대기, Changed)는 상한이 있고 같은 종류를 합치며 넘친 수를 집계한다(전체 계획 §23.3).

### 9.5 이름 pool

Core `NamePool`은 항목을 Shutdown에서만 해제하며 기본 상한이 65,536개다. 런타임 문자열을 이름으로 바꾸는 경로는 검색 전용 `TryFind`(Core 변경 C5)를 사용해 새 항목을 만들지 않는다. 런타임 문자열이 native `Name` 인자로 흘러가는 바인딩은 binding schema 오류다. source eval은 이름을 새로 등록하지 않는다.

## 10. 엔진 바인딩

### 10.1 C++ 선언과 Sparks에서의 형태

기존 `ROCKS_CLASS`·`ROCKS_PROPERTY`에 함수 리플렉션(CU2)·enum·struct(CU8)를 더한 선언에서 바인딩을 생성한다. 이벤트는 Core `MulticastDelegate` 필드에 `ROCKS_PROPERTY`를 붙이고, 메타 컴파일러가 필드 타입으로 `FieldKind::Event`를 판별한다. 이벤트 인자 이름은 `Params` 지정자로 준다.

C++ · `Health.h`:

~~~cpp
ROCKS_CLASS(ScriptExtensible)
class Health : public rocks::Component {
    ROCKS_GENERATED_BODY()

public:
    explicit Health(rocks::Allocator& allocator) noexcept : Died(allocator) {}

    ROCKS_PROPERTY(Edit, Save)
    rocks::int32 maxHealth = 100;

    ROCKS_PROPERTY(ReadOnly, Replicated)
    rocks::int32 current = 100;

    ROCKS_PROPERTY(Params = "killer")     // MulticastDelegate 필드 → 이벤트로 분류
    rocks::MulticastDelegate<void(rocks::WeakObjectRef<rocks::Entity>)> Died;

    ROCKS_FUNCTION(Lane = Game, Effects = WriteWorld)
    rocks::Result<void> ApplyDamage(rocks::int32 amount, rocks::WeakObjectRef<rocks::Entity> instigator);
};
~~~

Sparks에서는 `var MaxHealth: int`(편집·저장), 읽기 전용 속성 `Current: int`, `event Died(killer: Entity?)`, `fn ApplyDamage(amount: int, instigator: Entity?): Result<void>`로 보인다(Studio hover 표기, 소스 문법이 아님).

### 10.2 binding schema

| 대상 | 기록 항목 |
|---|---|
| 타입 | TypeId, 기반 타입, `ScriptExtensible`, 수명 종류(엔진 객체·에셋·값), 외형 전용 여부(준비 mask) |
| 필드 | FieldId, 값 형식, nullability, 편집·저장·복제·ReadOnly·`ScriptWritable`, 변경 통지 지원(CU5) |
| 함수 | FunctionId, 시그니처와 nullability, lane, `ThreadSafe`, `Deterministic`, 부수 효과, 최대 작업량, 취소 가능 여부, `MaySafepoint`, 권한, 편집기·출시 가용성, 오류 타입 |
| 자원 정책 | 반환하는 것의 종류: 유지 자원 / 일회성 효과 / 로드 요청 / 값. 기다리는 API는 고립 완료 해제 경로를 선언. 선언이 없으면 노출 불가 |
| 파괴 지연 | 엔진 객체를 파괴하는 함수는 단계 끝 deferred command로 처리함을 선언(SD3) |
| 이벤트 | `FieldKind::Event`, FieldId, 인자 이름(`Params`)·형식, 발행 lane, VM 연결 thunk |

### 10.3 이름·노출과 API 규칙

- 타입·함수·이벤트 이름은 C++ 이름 그대로다. 리플렉션 필드는 첫 글자를 대문자로, 끝의 `_`를 제거해 노출한다(SD2). C++ 오버로드는 서로 다른 `ScriptName`이 없으면 메타 컴파일 오류다.
- 쓰기 가능한 속성이 있으면 같은 값을 쓰는 `SetX` 함수는 노출하지 않는다.
- **스크립트용 API 규칙:** 이동 API는 초당 속도를 받고 dt는 엔진이 적용한다. 화면 표시 문자열은 `Text`만 받는다. 등록된 이름(socket·애니메이션 slot·태그·에셋)을 받는 인자는 검증 타입을 사용한다. 외형 전용 API는 전용 서버에서 효과가 없도록 구현한다.
- 편집기 버튼·콘솔 명령의 호출 대상은 `@command fn`(full)으로 표시한다. EditorWorld 객체에는 편집기 문맥 함수만 Undo transaction 안에서 호출한다.
- VM 진입은 Game lane에서만 허용하며 모든 빌드에서 같은 검사를 수행한다. `interface`(full)는 TypeInfo interface 표로 dispatch한다(CU9).

## 11. VM

- **레지스터 기반**, 타입별 opcode, 32비트 고정 명령. Lua 5.0은 2003년 레지스터 VM으로 전환하였고[R1], Shi 등은 실행 명령 수 47.21% 감소, 코드 크기 25.05% 증가, switch dispatch 실행 시간 32.28% 감소를 보고하였다(Pentium 4).[R2][R3] Luau·GDScript와 달리 모든 타입이 확정되므로 타입 태그 검사가 없다.[R6][R7]
- **3개 bank:** Scalar(태그 없는 64비트), Ref(엔진 객체 handle+generation), Buf(참조계수 버퍼). 스크립트 상태는 GC root를 만들지 않으며 재개 시 `this`의 generation을 확인한다.
- **필드 쓰기 opcode**는 Changed 연결 여부를 확인하고 발행을 등록한다. 정수 overflow·부동소수점 유한성을 검사하고 FMA 축약을 쓰지 않는다.
- **safepoint:** back-edge, 호출 복귀, `await`, 할당(Teardown 문맥 제외). Borrow는 native call thunk 안에서만 얻고 반환 전 해제한다(전체 계획 §6.3).
- **verifier:** 로드 시 선형 1회.[R4] 구조(범위·branch·bank 크기), 타입(bank·TypeId·초기화·non-null), 바인딩(FunctionId·시그니처 hash·lane·자원 정책), 문맥(금지 호출, `await` 위치, eval capability)을 검사한다.
- **dispatch:** switch 기준으로 시작하고 computed goto·tail-call은 측정 후 선택한다.[R10][R11][R12][R14]
- **측정 항목(미측정):** opcode당 시간, native call 왕복, 숫자 검사 비용, 이벤트 큐 전달 비용(단계당 1만), 연결 1만 개 Detach와 5만 항목 발행자 compact(목표 1 ms 이내 단일 pass), 본문 reload ≤2 s(전체 계획 §6.3·§27.8).
- **디버거:** pc→source 표, 함수별 breakpoint 플래그, native 경계 표시, 멀티플레이의 process·World 표시(전체 계획 §23.3).

bytecode · `t = Math.Min(t + Time.Delta / SwingSeconds, 1.0)`:

~~~text
GETF.F64   s3, r0, #SwingSeconds   ; r0 = this
DIV.F64    s4, s2, s3              ; s2 = Time.Delta, 결과가 유한하지 않으면 NonFinite
ADD.F64    s4, s1, s4              ; s1 = t
LOADK.F64  s5, k0                  ; k0 = 1.0
MIN.F64    s1, s4, s5
~~~

## 12. 컴파일·reload·eval·AOT

### 12.1 파이프라인과 .sparks

`.spark` → 파서(오류 허용) → 타입 검사(binding schema·ids) → typed IR(문맥·대입·수명) → bytecode(bank 할당) → verifier → `.sparks`

`.sparks`는 header(format·compiler version·모듈 ID·content hash·interface hash·profile), imports(의존 interface hash, native API의 FunctionId·시그니처 hash, Tag·Action GUID), types(TypeId·FieldId·SubobjectGuid·초기값·이벤트·RpcId), functions(bank 크기·실행 문맥·await 지점별 레지스터 형태), constants, 분리 디버그 정보로 구성한다. 출력은 입력이 같으면 바이트 단위로 같다. 컴파일에 실패하면 실행 중인 게임은 마지막 성공 revision을 유지한다.

### 12.2 안정 ID

- TypeId·FieldId·SubobjectGuid·FunctionId·RpcId는 C++와 같은 `.rocksids.json`을 사용한다(전체 계획 §7.1). 개발 빌드에서는 저장 후 컴파일이 새 선언의 ID를 발급하고, 입력 중인 미완성 선언에는 발급하지 않는다.
- 데이터가 연결된 ID(`@save`·`@net`·`@edit` 필드, 소유 component)의 소실은 매 컴파일의 오류이며 빠른 수정이 `@formerly("OldName")`을 삽입한다. 의도한 삭제는 `rocks ids retire`로 tombstone을 남긴다.
- FunctionId는 이벤트 handler·RPC·`@command`·리플렉션 호출 대상에만 발급한다.

### 12.3 증분 컴파일과 쿡

파일마다 interface hash와 함수별 body hash를 분리하고 본문 변경은 해당 함수만 다시 컴파일한다(전체 계획 §27.2·§27.3). 쿡은 전체 컴파일·검증, 리터럴 재검증, WidgetAsset 경로 대조, 하드 의존 예산, 소프트 대상 chunk 배정, prefab·save·RPC 호환 검사, 디버그 정보 분리를 수행한다.

### 12.4 reload

| 변경 | 처리 |
|---|---|
| 함수 본문 | 새 revision을 게시한다. 이후 호출·handler는 새 revision을 사용하고, 실행 중 frame은 이전 revision으로 끝까지 실행한다(전체 계획 §23.2). frame을 새 코드로 자동 이전하지 않는다. 편집기는 이전 코드로 실행 중인 handler 수와 대상을 표시하고 재시작 명령을 제공한다. |
| 필드 기본값 | override하지 않은 instance에 반영하고 override한 instance를 표시한다. |
| 필드·`on` 선언 추가·삭제 | Sparks frame이 0개인 경계에서 migration하고 연결을 다시 구성한다. 삭제된 필드의 유지 자원·lease를 해제한다. 바뀐 배치에 접근하는 이전 revision 실행은 취소한다. |
| `@net`·`@rpc` 배치, 클래스 계층 | PIE 재시작. |

탈출하는 상수 문자열은 참조계수 버퍼로 복사하여 revision을 고정하지 않는다.

### 12.5 native AOT

bytecode 사전 컴파일은 항상 수행하고, 기계어 AOT는 성능이 필요한 target에서 선택한다. typed IR → C++ 생성 → 플랫폼 toolchain 빌드를 권고한다(전체 계획 §23.1의 "LLVM 기반 AOT"와 다르므로 §17에 기재).[R8][R9]

- 실행 파일의 일부이므로 iOS·콘솔의 JIT 제한과 무관하다.[R5]
- VM과 같은 검사(overflow·유한성·인덱스·예산·safepoint·handle 확인·파괴 지연)를 생성하고 FMA 축약을 끈다. fault 탈출은 RAII와 반환 코드로 처리한다(예외 없음).
- async 함수·handler는 명시적 상태 기계 struct로 생성하고 ledger에 집계되는 실행 allocator에서 할당한다.
- AOT 함수와 bytecode 함수는 같은 호출 규약으로 서로 호출한다. eval·외부 모듈은 항상 VM이다. AOT 빌드에서 본문을 고친 함수는 bytecode revision으로 대체되어 reload가 유지된다.
- conformance corpus를 VM과 AOT로 실행해 결과·fault·이벤트 순서·ledger를 비교하는 차등 시험을 Gate로 둔다.

### 12.6 런타임 eval 정책

| target 정책 | 값 | 출시 기본(SD4) |
|---|---|---|
| `ScriptEval`(소스 eval) | `Off` · `DevOnly` · `On` | `Off` |
| `ExternalModules`(외부 모듈) | `Off` · `DevOnly` · `Signed` · `On` | `Off` |

`Off` target에는 컴파일러를 링크하지 않는다. JIT가 아니므로 실행 가능한 메모리를 할당하지 않으며, 마켓 판단 대상은 다운로드한 코드로 기능을 변경하는 것을 제한하는 규정이다(App Store 2.5.2).[R31] 콘솔 규정은 공개 자료로 확인하지 못하였다.

### 12.7 eval·외부 모듈의 수명

- **source eval**은 기존 타입에 대해 함수·handler를 실행하며 타입을 선언하지 않고, 변경 가능한 전역이 없고, 이름을 새로 등록하지 않는다. 같은 정적 타입 검사·검증·예산·ledger를 거치며 호출 가능한 API는 capability(기본 `ModSafe`: World 읽기·쓰기, IO·네트워크·저장 금지)로 제한한다.
- **외부 모듈**은 자체 ID manifest를 갖고 타입을 pool과 무관한 NameKey로 등록한다. 의존 패키지의 interface hash가 다르면 로드를 거부한다. `@save`·`@net`·`@rpc`는 서명된 모듈에서만 허용한다.
- 모듈이 생성한 Entity는 모듈 소유이며 저장 대상에서 제외(transient)한다.
- unload는 비동기다(Unloading → Unloaded). TypeRegistry가 Busy이면 정해진 횟수만 재시도한 뒤 격리하고 Gate 실패로 보고한다. Unloading 중 호출은 오류를 반환한다.

## 13. 언어 서버와 편집기

LSP 서버 `sparks-lsp`를 VS Code 확장과 Rocks Studio Script Editor가 공유한다.[R15] CLI·Rocks Build·언어 서버·런타임 eval이 같은 컴파일러 라이브러리를 사용하므로 같은 입력에서 같은 진단이 도출된다(전체 계획 §23.3). 서버는 엔진 없이 binding schema와 사전 파일만으로 동작하고 별도 process이며 문서 버전별 arena를 사용한다. 디버깅은 DAP adapter가 VM 디버거에 연결한다.[R16][R17]

자동완성 위치(`|` = 커서):

~~~text
class DoorLogic : ScriptComponent {
    @edit var RequiredKey = Tag("Item.|");        // 태그 사전
    on |                                           // Awake, Start, Tick, TriggerEnter, Owner.Interacted, State.Changed …
    on Target.|                                    // Target 타입의 이벤트와 필드 Changed
    fn Ring() {
        World.Audio.PlayAt(Asset("/Game/Audio/|"), Owner.Location);   // SoundCue 에셋만
    }
}
~~~

- 리터럴 자동완성은 컴파일 검증과 같은 사전을 사용한다. Unity 이름(`Update`, `FixedUpdate`, `LateUpdate`, `OnEnable`, `GetComponent`)을 입력하면 Rocks 이름(`Tick`, `FixedTick`, `LateTick`, `Enable`, `Find<T>`)을 제시한다. core 패키지에서는 full 후보를 표시하지 않는다.
- hover는 타입·문서 주석·native 선언 위치·하드 의존 크기·bundle을 표시한다. 빠른 수정: `?` 추가, `?? return`, 누락 import, handler 골격, `Soft`→`try await X.Load()`, `@formerly`.
- 단계: L1 진단·기본 자동완성·의미 강조, L2 리터럴·경로 자동완성·hover·정의 이동·빠른 수정·서식·추론 타입 표시, L3 ID를 유지하는 rename·참조 찾기·VS Code 디버거.
- 제안 목표(미측정): 자동완성 p95 100 ms(파일 1,000개), 편집 후 진단 300 ms. 검증: 후보 golden test, prefix fuzz, CLI와의 진단 동등성, rename 후 저장 데이터 보존.

## 14. 네트워크

replication · RPC:

~~~text
class Weapon : ScriptComponent {
    @net var Ammo: int = 30;
    @net(OwnerOnly) var Reserve: int = 90;
    let ReloadKey = Action("Gameplay.Reload");

    on Ammo.Changed() { RefreshAmmo(); }                      // 모든 기기에서 실행

    @rpc(Server, Reliable) fn RequestReload() {               // full 단계
        if (Reserve == 0) { return; }                         // 서버에서 다시 검증
        Reload(Caller);                                       // Caller: 호출한 connection
    }

    on ReloadKey.Pressed() { Server.RequestReload(); }        // 호출 위치에 원격 호출이 드러난다
}
~~~

- **복제 필드(core):** `@net` 필드는 권한 측에서만 쓸 수 있으며, 그렇지 않으면 모든 빌드에서 fault다. 허용 타입은 저장 가능 타입, 복제되는 객체 참조, `Soft<T>`다. 초기 적용·late join은 `Changed`를 발행하지 않는다.
- **RPC(full):** `Server.`·`AllClients.`·`OwnerClient.` 접두로만 호출하며 static manifest(RpcId)로 등록한다(전체 계획 §23.2). 서버 RPC에는 `Caller`가 주입되고, 객체 인자는 호출자의 interest set 안에 있어야 하며, 소유하지 않은 객체에 대한 RPC는 거부한다. 길이·유한성·enum 값을 검사하고 connection·RpcId별 빈도를 제한한다. 클라이언트가 보낸 `Soft<T>`는 자동 로드하지 않는다(전체 계획 §17.1·§17.3).
- 원격 client가 유발한 fault는 instance·session으로 확대하지 않는다. prediction·replay 대상은 `@deterministic`이다.

## 15. 저장·데이터·편집 연동

`Props/Chest.spark`:

~~~text
@tableRow(Asset("/Game/Data/LootTable"))
struct LootRow {
    var Item: Soft<ItemData>;
    var Weight: float;
}

class Chest : ScriptComponent {
    @edit var Lid: TransformComponent;                 // 같은 prefab 안의 필수 참조
    @edit var Loot: Row<LootRow> = Row("Chest.Common");
    @edit(Min: 0, Max: 10) var Uses: int = 3;
    @save @formerly("Opened") var IsOpened: bool;

    on Start() { ApplyLid(); }                         // 로드된 상태 반영
    on IsOpened.Changed() { ApplyLid(); }

    fn ApplyLid() {
        if (IsOpened) {
            Lid.RelativeRotation.Pitch = 110.0;
        } else {
            Lid.RelativeRotation.Pitch = 0.0;
        }
    }
}
~~~

- **저장 가능 타입:** 스칼라·수학·문자열·enum·`Tag`·`Soft<T>`와 그것으로 구성된 struct·컨테이너. 다른 Entity·로드한 에셋 값은 저장할 수 없다(전체 계획 §7.2).
- **호환성:** 매 컴파일 검사. 필드 추가는 기본값(또는 zero value), 삭제는 `retire`, 이름 변경은 `@formerly`, 무손실이 아닌 형식 변경은 migration 함수. enum case는 뒤에 추가만 허용.
- **로드:** 무손실 변환이 가능하면 변환하고, 불가능하거나 알 수 없는 enum 값이면 기본값을 쓰고 보고한다. 클래스를 찾지 못하면 원래 필드를 보존하는 placeholder로 로드한다. 저장 파일에도 RPC와 같은 길이·값 검사를 적용한다.
- **데이터 에셋:** `DataAsset`과 `@tableRow` struct는 실행 중 읽기 전용이다. `@edit` 배열 원소에는 ElementGuid를 자동 부여한다(전체 계획 §20.3·§24.2).

## 16. Verse 검토와 차용

Epic의 "The road to UE6"(2026-06-17)는 게임플레이 프로그래밍 모델의 Verse 전환을 밝히고, Actor·Blueprint는 초기 UE6에 남았다가 새 framework가 성숙하면 deprecate된다고 기재하였다. Blueprint 즉시 제거는 2차 보도다.[R18] Rocks의 Unreal 기준은 UE 5.6이며 Verse는 언어 설계의 참고 대상이다.

| Verse 기능 | 판정 | Sparks 반영 |
|---|---|---|
| failure context(인덱싱·map 조회) | 의미 차용 | 인덱스·키 읽기는 `T?`. `[]`/`()` 구분 문법은 미채택 |
| `OnBegin<suspends>` | 차용 | handler·`Start` 안에서 바로 `await` |
| 구조적 동시성(`race` 패배 취소, `branch` 수명) | 차용 | `All`·`Race`, 비구조적 `spawn` 대응 없음 |
| effect specifier | 구조 차용 | 실행 문맥, 재정의는 문맥을 넓힐 수 없음 |
| persistable·publish 호환성 검사 | 차용 | 저장 가능 타입, 매 컴파일 호환성 검사 |
| `weak_map(player, t)` | 목적 차용 | 엔진 객체 원소·키의 자동 정리 |
| 읽기·쓰기 접근 분리 | 단순화 차용 | Sparks 필드는 밖에서 읽기만 |
| `message` 지역화 타입 | 차용 | 화면 표시 API는 `Text`만 |
| 평가 순서 명시, lambda 없음, option 타입 | 일치 | — |
| int / int → rational | 목적만 차용 | `/`는 항상 `float` |
| 투기 실행·rollback | 보류 | 엔진 C++ 쪽 transactional 기반이 없고 전체 계획 §23.4가 외부 효과 자동 rollback을 하지 않음. UE6의 C++ transactionalize 진행 시 재검토 |
| `live` 변수, `[]`·`:=`·`set` 문법, 포착 불가 오류의 섬 전체 중지 | 미채택 | 미출시 기능, 평범한 문법 요구, 오류 영향 범위 축소 |

UEFN 커뮤니티에서는 기호가 많은 문법, failure·동시성의 학습 비용, 일반 디버거 부재가 지적되었다(2차 자료). Sparks는 C 계열 문법, `T?`, VS Code 디버거로 대응한다.[R19][R20]

## 17. 스펙 개정 대상

개정 대상은 전체 계획, 입력 상세안, UI 툴킷, Core 구현 코드다. 위치 열의 절 번호는 접두어가 없으면 전체 계획의 절이다. "현재 문구"는 전체 계획 개정 1.6 기준이며, 문서 항목은 모두 전체 계획 개정 1.7·입력 상세안·UI 툴킷에 반영하였다. 단계 이름(BeginPlay 등)을 쓰는 전체 계획 §5.3과 입력 상세안 586행은 엔진 내부 단계 이름이므로 그대로 두고 전체 계획 §20.2의 대응표로 연결한다. Core 구현 코드(C1–C5)는 L1 구현 대상이다.

| 위치 | 현재 문구 | 이 설계 |
|---|---|---|
| §23.1 이름·예시·범위 | 이름 미정, `ObjectRef`·`WeakRef`, Result·generic·interface, 동적 eval 제외, LLVM 기반 AOT | Sparks, `T?` 단일 개념, 사용자 generic·closure 제외, interface full, eval target 정책, typed IR → C++ AOT 권고 |
| §23.2 VM·GC·coroutine | stack depth 검사, coroutine root 열거, 메모리 예산 | bank 검사, 스크립트 상태는 GC root 없음(약한 handle·lease), 논리 단위 메모리 예산, 구조적 동시성 |
| §5.2·§20.2 파괴·수명 | RequestDestroy 즉시 전이, 수명은 component 기준, hook 이름 BeginPlay 등 | 스크립트 단계 중 파괴는 단계 끝 반영(SD3), 수명 hook을 service·widget controller에도 적용, Unity 체계 hook 이름과 C++ `On*` 가상 함수(SD9), BeginDestroy에서 Detach |
| §5.3, 입력 상세안 586행 | "optional integration Register / BeginPlay" | hook 대응표(§5.1)와 연결 |
| §8.3 clock | Monotonic·Frame·Game·Fixed·UI | `Delay(s, Game\|UI)`, `NextTick`(Frame), `NextFixedTick`(Fixed) |
| §9.2·§10.2·§10.3·§24.2 로딩 | LoadTicket·AssetLease·revoke·travel·bundle | 요청 handle·결과 값 보관, 대기자 분배, Spawn의 prefab lease, 외형 전용 stub, 필드 초기값 에셋은 prefab 의존, 엔진 관리 로딩 화면, Teleport 내부 prefetch |
| §23.4 native API 선언 | lane·최대 작업·취소 | + 스레드 안전성·결정성·부수 효과·MaySafepoint·권한·자원 정책·파괴 지연·고립 완료 경로 |
| §29.6 NL03·NL04·NL06 | PIE 1,000회 등 | G1–G4 Sparks Gate, 무기한 재시도 대신 격리·실패 보고 |
| UiToolkit CU2·CU3·CU5·CU8, CU9 신설 | UI 사용처 중심 | 언어 L1 선행. CU3은 `MulticastDelegate` 필드와 `FieldKind::Event` |
| 입력 상세안 §22.3 | subscription이 module/script lease 보유, VM reload 시 해제 | 연결 항목은 handler ID만 보유, 선언이 바뀌는 reload에서만 재구성 |
| Core `Delegate.h`·`NamePool.h`(구현) | 약한 바인딩은 `SharedPtr`만, TryAdd compact 없음, 검색 전용 조회 없음 | C1–C5 |
| §23.3·§2.4 | Studio language service, 언어 경계 명명 보류 | LSP·DAP, 패키지 `Rocks`, 필드 첫 글자 대문자 변환 |

## 18. 단계별 도입

| 단계 | 언어 기능 | 완료 기준(전체 계획 §23.1) |
|---|---|---|
| L0 · M3 | core·full 문법, 타입·참조·대입·숫자 규칙, hook·단계·이벤트 전달 모델, 유지 자원, 소유·해제 연산, conformance fixture | Core/Object 계약에 맞는 언어 규칙 확정 |
| L1 · M3 | 컴파일러 라이브러리, interpreter, native call, 오류만 있는 진단, Core C1–C5, LSP 기본 | 조건문·함수·객체 필드·배열·오류 처리와 바인딩 테스트 |
| L2 · M3/M4 | 수명 hook, `on 경로.이벤트`·큐·`Changed`, async handler·`All`/`Race`, 유지 자원, `Load`·`LoadAll`·Teleport·Travel, ledger G1·G4, LSP 자동완성·빠른 수정, VS Code 확장 | 상호작용·문·카메라·컷씬 trigger를 C++ 수정 없이 제작 |
| L3 · M5 | `@net`·RPC, 저장 호환성, 디버거(DAP), reload, soak G2·G3, eval(`DevOnly`) | 취소·GC·replication·중단점·migration·누수 Gate 통합 검증 |
| L4 · M6 이후 | profiler, 최적화 IR, 선택 AOT, target별 eval 정책 | hot function 비용과 마켓 규정 근거로 결정 |

## 19. 결정 사항

2026.11.12 검토에서 초안의 권고안 10건을 모두 채택하였다. SD4는 기본값만 확정하고 target별 값은 마켓 규정 검토 후 정한다. SD3·SD9는 엔진 수준 결정이며 전체 계획 개정 1.7의 §5.2·§20.2에 반영되어 있다.

- **SD1. 확장자:** 소스 `.spark`, 컴파일 모듈 `.sparks`. `.sparks`는 빌드 출력 폴더에만 생성한다. 대안 `.sparkc`는 채택하지 않는다.
- **SD2. C++ 리플렉션 필드 이름:** 첫 글자를 대문자로 변환하여 노출한다(§10.3). 리플렉션 필드를 C++에서도 PascalCase로 선언하도록 규약을 개정하는 대안은 채택하지 않는다.
- **SD3. 파괴 요청의 단계 끝 지연:** 스크립트와 스크립트가 호출한 native API의 파괴 요청을 현재 단계가 끝날 때 반영한다(§4.3). 스크립트가 호출하는 native API가 이 규칙을 지키도록 binding schema에서 선언·검사한다(§10.2). 전체 계획 §5.2의 즉시 전이와 조정하는 엔진 수준 결정이며 전체 계획 개정 1.7의 §5.2·§20.2에 반영되어 있다.
- **SD4. 런타임 eval의 target 정책:** 기본값만 정한다. 출시 빌드는 `Off`, 개발 빌드는 `DevOnly`다. target별 실제 값은 마켓 규정 검토 후 정한다(§12.6, §18 L4).
- **SD5. 컴파일러·언어 서버 구현 언어:** C++ 라이브러리로 구현한다. 런타임 eval을 위해 출시 바이너리에 포함될 수 있어야 한다.
- **SD6. 저장 시 reload의 컴파일 경로:** Rocks Build의 Sparks compile action 단일 경로로 한다.
- **SD7. 출시 빌드의 로드 검증:** 항상 검증한다. 외부 모듈을 허용하는 target에서는 필수다.
- **SD8. VM 방식과 reload 시간 목표:** 레지스터 VM과 3개 bank(Scalar·Ref·Buf)로 하고, reload 시간 목표는 전체 계획 I1의 2 s로 한다. 전체 계획 개정 대상이다(§17).
- **SD9. C++ 수명 hook 가상 함수 이름:** `OnAwake`·`OnStart`·`OnTick` 등 `On` 접두를 사용한다. 일반 `Start()` 메서드와의 충돌을 피하고 Sparks의 `on Start()`와 1:1로 대응한다. 전체 계획의 단계 이름(Initialize·BeginPlay 등)은 내부 단계 이름으로 유지한다. 엔진 수준 결정이며 전체 계획 개정 1.7의 §5.2·§20.2에 반영되어 있다.
- **SD10. 비활성화 시 동작:** Disable이 실행 취소·유지 자원 해제·이벤트 수신 중지를 수행한다. Unity는 component 비활성화만으로 coroutine을 중지하지 않는다. "비활성 객체는 아무것도 하지 않는다"는 규칙이 누수 방지에 유리하여 Unity와 다르게 정한다.

## 20. 교차 검증 기록

초안 0.1·0.4·0.5는 이 문서의 작업 초안이다.

### 20.1 3차 검증(초안 0.5)

초안 0.4를 대상으로 정합성 점검(48건), 누수 반례 검토(16건), 누수 수정안 반례 검토, 수정안을 보지 않은 수명 모델 독립 설계, 비동기 로딩 설계 반례 검토를 수행하였다. 누수 검토의 코드 근거 3건(`TryAdd`의 compact 부재, `NamePool`의 Shutdown 전용 해제·65,536 상한, TypeRegistry Busy)은 저장소 코드와 일치함을 확인하였다. 사용자 결정(Unity 체계 hook, Tick·FixedTick·LateTick, 비동기 로딩 반영)을 함께 반영하였다.

| # | 지적 | 출처 | 조치 | 위치 |
|---|---|---|---|---|
| 1 | 세션 종료 0 검사는 세션 중 증가를 검출하지 못하고 메모리 예산이 누락됨 | 누수 | 범위 종료마다 검사, 논리 단위 예산 복원, 2시간 soak 구간 최솟값 기울기 Gate | 9.3 |
| 2 | delegate 항목이 확실히 제거되지 않음(GC 경로, compact 비용, 중복 원소) | 누수, 수정안 검토, 독립 설계 | 회수 가능 대상(C1)·상한 compact(C2)·단일 pass(C3), BeginDestroy Detach, 원소별 연결 1개 | 5.5 |
| 3 | 확장 이벤트·World 이벤트·native Changed의 저장 위치 부재 | 누수 | 발행자 객체의 이벤트 slot 표, session World service proxy | 5.5 |
| 4 | 자원 handle 타입·이전 규칙 부재, 반복문 안 지역 자원 누적 | 누수, 수정안 검토 | 유지 자원 3규칙, 블록 종료 해제 | 7.1 |
| 5 | 정지 frame이 파괴된 객체 유지, `this` 비root 시 use-after-free | 누수, 수정안 검토 | 스크립트 상태 GC root 없음, 재개 시 generation 확인 | 4.1·11 |
| 6 | 본문 hash가 같으면 frame 이전하는 수정안은 상수표·inline 변경 시 조용히 오동작 | 수정안 검토 | 자동 이전 미채택, 편집기 재시작 안내, revision 수 표시 | 12.4 |
| 7 | 모듈별 NamePool은 Name 비교를 깨뜨림, eval 타입 이름이 엔진 pool 증가 | 수정안 검토, 독립 설계 | source eval은 타입·이름 등록 없음, 외부 모듈은 NameKey, `TryFind`(C5) | 9.5·12.7 |
| 8 | 무기한 재시도·종료 Busy | 수정안 검토 | 재시도 상한 후 격리·실패 보고, 종료 deadline | 9.1·12.7 |
| 9 | 로딩 대기 중 이벤트 폐기는 Enter/Exit 짝·서버 판정을 깨뜨림 | 로딩 검토 | Start 대기 중에는 Tick 계열만 보류, 이벤트는 정상 전달 | 5.1 |
| 10 | 생성된 Entity가 prefab lease보다 오래 남음 | 로딩 검토 | Spawn이 prefab 의존 묶음 lease를 Entity에 귀속 | 7.6 |
| 11 | 전용 서버에서 외형 에셋 로드 실패로 서버·클라이언트 결과 차이 | 로딩 검토 | 준비 mask 교집합과 효과 없는 stub, PIE 서버 session 시험 | 7.5 |
| 12 | 필드 초기값 하드 참조가 세션 내내 상주 | 로딩 검토 | instantiate하는 prefab·셀의 의존성으로 처리, 하드 의존 예산 쿡 오류 | 7.2 |
| 13 | 상주 유지 경로 2개(필드 요청·값), `Get`의 cache 의존 | 로딩 검토 | 요청은 지역 전용, 상주는 값 보관 하나, `Get`·`IsLoaded` 제거 | 7.3 |
| 14 | Teleport 관심 공백, travel 호출 실행 취소, 로딩 화면 미해제 | 로딩 검토 | `Owner.Teleport` 내부 prefetch, `Session.Travel` 접수형·엔진 관리 로딩 화면 | 7.6 |
| 15 | `emit` 전달 시점이 절마다 다름, C++ handler 즉시 호출로 재진입 | 정합성 | 실행 조각 종료 후 FIFO 하나로 통일, C++ handler도 전달 시점 | 5.4 |
| 16 | 필드 쓰기 권한 규칙과 native 속성 예시 충돌, setter·속성 중복 | 정합성 | `ScriptWritable` native 속성 예외, 속성이 있으면 setter 비노출 | 3.2·10.3 |
| 17 | 예시의 규칙 위반(대소문자만 다른 이름, 미사용 인자, 버린 결과, 서버 handler의 UI 호출, 암묵 전역) | 정합성 | 예시 재작성, handler 인자 앞부분 선언, `_` binder, `NextTick`·`Race` 무반환, 암묵 멤버 닫힌 목록 | 2·3·6 |
| 18 | EBNF 누락(generic 메서드, BinaryOp·Literal, 복합 대입의 await, `?? Exit`, emit 형태) | 정합성 | 문법 보완 | 3.6 |
| 19 | 초기 적용 시 Changed 발행 여부 미정, late join 일회성 재생 | 정합성 | 초기·로드·late join은 Changed 없음, Start에서 반영 | 5.3 |
| 20 | AOT 방식·clock 이름·Start 개명 범위·소유 component 이중 소유가 스펙 개정 목록에 없음 | 정합성 | §17에 추가, 소유 component는 handle·SubobjectGuid | 4.1·4.5·17 |
| 21 | 사용자 결정: hook은 Unity 체계, 이름은 Rocks 어휘(Tick·FixedTick·LateTick) | 사용자 | hook 표, 일반 `Start()` 메서드와의 충돌은 C++ `On*`로 해소, 종료 hook 후보 `Stop`과 PIE `Stop`의 충돌은 `Destroy` 채택으로 해소 | 5.1 |
| 22 | 사용자 요구: 비동기 로딩 | 사용자 | §7 신설 | 7 |

### 20.2 1·2차 검증 요약

1차(초안 0.1): 독립 설계·반례 검토·사실 확인 3건. 필드 컨테이너 barrier, Worker 경계의 값 고정, 결정성 문맥의 tick 기반 await, Scalar·Ref·Buf bank, native→Sparks 진입 문맥 검사, interface 표(CU9)가 유지되고 있다. 사실 확인 정정(VM Showdown 수치, 콘솔 JIT 미확인, computed goto 효과)은 §11·§12.5에 반영되어 있다. 사용자 검토로 Core `MulticastDelegate`를 재사용하고(§10.1) VS Code 언어 서버를 추가하였다(§13).

2차(초안 0.4): 단순성 감사, 비프로그래머 실수 시뮬레이션(31건), 언어 안티패턴, 런타임 방어성, 외부 근거, 초안을 보지 않은 최소 설계, Verse 조사 7건. "없음" 단일 개념, 함수 결과 대입 금지, 명시 구독·closure·`launch`·`defer` 제거, 큐 전달, 경고 없는 오류, 숫자 규칙, 단계별 fault 정책, 누수 방지 절, 런타임 eval, Verse 판정이 이때 결정되었다.

## 21. 참고 자료

P는 1차 자료, S는 2차 자료다.

| ID | 자료 | 구분 |
|---|---|---|
| R1 | Ierusalimschy, de Figueiredo, Celes. The Implementation of Lua 5.0. JUCS 11(7), 2005. [lua.org/doc/jucs05.pdf](https://www.lua.org/doc/jucs05.pdf) | P |
| R2 | Shi, Gregg, Beatty, Ertl. Virtual Machine Showdown: Stack Versus Registers. VEE 2005. [usenix.org/…/p153-yunhe.pdf](https://www.usenix.org/legacy/events/vee05/full_papers/p153-yunhe.pdf) | P |
| R3 | Shi, Casey, Ertl, Gregg. ACM TACO 4(4), 2008. [mural.maynoothuniversity.ie/…/KC-Virtual-2008.pdf](https://mural.maynoothuniversity.ie/10185/1/KC-Virtual-2008.pdf) | P |
| R4 | WebAssembly Core Specification, Validation Algorithm. [webassembly.github.io/…/algorithm.html](https://webassembly.github.io/spec/core/appendix/algorithm.html) | P |
| R5 | Apple Platform Security Guide: 동적 코드 서명. [support.apple.com/guide/security/sec15bfe098e/web](https://support.apple.com/guide/security/sec15bfe098e/web) | P |
| R6 | Luau: Type checking, Performance. [luau.org/types](https://luau.org/types) | P |
| R7 | Godot Docs: Static typing in GDScript. [docs.godotengine.org/…/static_typing.html](https://docs.godotengine.org/en/stable/tutorials/scripting/gdscript/static_typing.html) | P |
| R8 | Hazelight UnrealEngine-Angelscript, Precompiled Data. [angelscript.hazelight.se/…/precompiled-data/](https://angelscript.hazelight.se/cpp-bindings/precompiled-data/) | P |
| R9 | Unity Manual: IL2CPP Introduction. [docs.unity.com/…/il2cpp-introduction](https://docs.unity.com/en-us/engine/6000.7/manual/programming-environment/scripting-backends/il2cpp/il2cpp-introduction) | P |
| R10 | Ertl, Gregg. The Structure and Performance of Efficient Interpreters. JILP 5, 2003. [jilp.org/vol5/v5paper12.pdf](https://jilp.org/vol5/v5paper12.pdf) | P |
| R11 | CPython `ceval_macros.h`, What's New in Python 3.14. [docs.python.org/3.14/whatsnew/3.14.html](https://docs.python.org/3.14/whatsnew/3.14.html) | P |
| R12 | Clang Attribute Reference: `musttail`. [clang.llvm.org/…#musttail](https://clang.llvm.org/docs/AttributeReference.html#musttail) | P |
| R13 | Swift Array 문서(copy-on-write). [developer.apple.com/documentation/swift/array](https://developer.apple.com/documentation/swift/array) | P |
| R14 | Rohou, Swamy, Seznec. CGO 2015. 서지 정보와 2차 요약만 확인. | S |
| R15 | Language Server Protocol Specification. [microsoft.github.io/language-server-protocol/…](https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/) | P |
| R16 | Debug Adapter Protocol. [microsoft.github.io/debug-adapter-protocol/](https://microsoft.github.io/debug-adapter-protocol/) | P |
| R17 | VS Code API: Language Server Extension Guide. [code.visualstudio.com/…/language-server-extension-guide](https://code.visualstudio.com/api/language-extensions/language-server-extension-guide) | P |
| R18 | Epic Games. The road to UE6, 2026-06-17. [unrealengine.com/news/the-road-to-ue-6](https://www.unrealengine.com/news/the-road-to-ue-6). 원문 접근 거부로 재게재 인용 확인. Unreal Fest Tokyo 보도(S). | P/S |
| R19 | The Book of Verse. [verselang.github.io/book/](https://verselang.github.io/book/) | P |
| R20 | The Verse Calculus. ICFP 2023. [doi.org/10.1145/3607845](https://doi.org/10.1145/3607845) | P |
| R21 | Go FAQ, Effective Go. [go.dev/doc/faq](https://go.dev/doc/faq) | P |
| R22 | Go Blog: Fixing For Loops in Go 1.22. [go.dev/blog/loopvar-preview](https://go.dev/blog/loopvar-preview) | P |
| R23 | Elm: Compiler Errors for Humans. [elm-lang.org/news/compiler-errors-for-humans](https://elm-lang.org/news/compiler-errors-for-humans) | P |
| R24 | Kotlin: Null safety. [kotlinlang.org/docs/null-safety.html](https://kotlinlang.org/docs/null-safety.html) | P |
| R25 | Luau RFC: New non-strict mode. [rfcs.luau.org/new-nonstrict.html](https://rfcs.luau.org/new-nonstrict.html) | P |
| R26 | Unity Blog: Custom == operator, should we keep it? [unity.com/blog/…](https://unity.com/blog/engine-platform/custom-operator-should-we-keep-it) | P |
| R27 | Microsoft Learn: Compiler Error CS1612. [learn.microsoft.com/…/cs1612](https://learn.microsoft.com/en-us/dotnet/csharp/language-reference/compiler-messages/cs1612) | P |
| R28 | Sweeney. The Next Mainstream Programming Language. POPL 2006. [ics.uci.edu/…/Tim-POPL-2006.pdf](https://ics.uci.edu/~wscacchi/GameIndustry/Tim-POPL-2006.pdf) | P |
| R29 | 오류 메시지 연구: Becker 외 2016(S), Pettit 외 2017, Barik 외 2017, Santos·Becker 2024 [arxiv.org/abs/2409.18661](https://arxiv.org/abs/2409.18661). 연구 간 결과가 일치하지 않는다. | P/S |
| R30 | PEP 238: Changing the Division Operator. [peps.python.org/pep-0238/](https://peps.python.org/pep-0238/) | P |
| R31 | Apple App Store Review Guidelines 2.5.2. [developer.apple.com/app-store/review/guidelines/](https://developer.apple.com/app-store/review/guidelines/) | P |
