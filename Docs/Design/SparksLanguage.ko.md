# Sparks — Rocks 게임플레이 언어

정적 타입 · C 계열 문법 · 레지스터 VM · 비프로그래머 대상의 방어적 설계

**설계 기준일 2026.11.18 · 초안 1.3 · 상태: 설계 제안 / 미구현 · 성능 미측정**

이 문서는 [전체 계획](EngineImplementationPlan.ko.md) §23이 요구하는 Rocks 게임플레이 언어 Sparks를 정의한다. 프로그래머가 아닌 사용자도 쓸 수 있도록 Go처럼 단순하게, 실수할 여지가 적게, 메모리·리소스 누수가 생길 수 없는 구조로 설계한다. 기준은 엔진 구현 상태가 아니라 전체 계획 §23과 이 언어가 연결되는 전체 계획 §5·§6·§8·§9·§10·§16·§17·§20·§21·§24·§29의 계약이다. 이 문서의 문법·이름·API는 설계 제안이며 구현·측정 결과가 아니다. 예시의 엔진 API 이름(`AddMovement`, `World.Spawn` 등)은 바인딩 규칙을 보이기 위한 예시이며 실제 이름은 각 모듈의 C++ 선언에서 생성된다. 아직 엔진에 없는 게임 타입(`Character`, `Collider`·`SphereCollider`, `SoundAsset`, `Sequence`, `DataAsset`, `ScriptComponent`, `WidgetController`, `PlaySession`)은 자리표시 이름이며 해당 모듈이 들어올 때 확정한다(§17.2).

초안 1.3은 구현 착수 전 스펙 완비를 위한 개정이다. 엔진 런타임(Actor·World 코드)·Prefab 개정 3·리플렉션 B9·주변 시스템 스펙(로깅·CVar·입력·UI·콘텐츠 서명)과 이름·시점을 맞추고, 사용자 핵심 요구 3가지 — **핫 리로드 필수(§12.4), 오류 방어(§8.4), 선택형 native AOT(§12.5, Unity IL2CPP와 같은 backend 선택)** — 를 규격으로 확정하였다. 미정 값(전달 수 상한·예산·interface·CU9)을 정하고 표준 API(부록 B)를 추가하였다. 구현은 전체 계획의 순서(L0 = Foundation Gate 이후 M3)를 따른다.

| 항목 | 내용 |
|---|---|
| 기준 문서 | 전체 계획 §23 · [엔진 런타임](EngineRuntime.ko.md)(이하 ER) 2·6–8절과 World 코드 · [Prefab](Prefab.ko.md) 개정 3 · [런타임 클래스 메타데이터](RuntimeClassMetadata.ko.md)(이하 RCM) · [리플렉션 B9](ReflectionExtensions.ko.md)(이하 RX) · [입력 상세안](CoreObjectInputDetailedPlan.ko.md) · [UI 툴킷](UiToolkit.ko.md) CU1–CU9 · [로깅](LoggingSystem.ko.md) · [CVar](CVarSystem.ko.md) · [콘텐츠 서명](ContentSigning.ko.md) · [Clang 생성 Reflection](../Implementation/ReflectionMetadata.ko.md)(구현 상태) |
| Unity·Verse 비교 기준 | Unity 수명 hook 체계(종류·호출 순서·의미). Verse는 The Book of Verse[R19] 기준이며 언어 설계의 참고 대상이다. Rocks의 Unreal 기준은 UE 5.8(5.8.4, commit `f45729c0b6f0`) |
| Rocks 상태 | Sparks 언어·VM·컴파일러 미구현. Core C5(`NamePool::TryFind`)는 구현됨 |
| 식별자 | 도입 단계 L0–L4, Core 변경 C1–C5, 리플렉션 확장 CU9, Gate G1–G6, 결정 SD1–SD45, 진단 코드 S0100–S1199, 부록 A·B, 참고 자료 R1–R31 |

## 0. 핵심 결정

| 항목 | 결정 |
|---|---|
| 이름 | Sparks. 소스 `.spark`, 컴파일 모듈 `.sparkc`(SD1). 파일 경로가 import 경로다(`Weapons/Sword.spark` → `import Weapons.Sword;`). 이 경로가 엔진 공통 타입 도구 이름(asset 명령·클래스 참조 텍스트)이다([리플렉션 B9](ReflectionExtensions.ko.md) 5.2절). |
| 단순성 | 진단은 경고 없이 오류만 두고 모든 오류에 수정 예를 제시한다. 한 가지 일에는 한 가지 방법만 둔다. 실수가 잦은 구조는 언어 규칙으로 제거한다. 기능은 core 프로필과 full 프로필로 구분한다. |
| 문법 | C 계열이다. `var`/`let`, `fn Name(a: T): R`, 괄호로 감싼 조건식, `on` handler, `@attribute`, 범위 `0..<n`. 타입·멤버는 PascalCase로 C++ 이름과 같고, 내장 값 타입(`float`, `vec3`)과 지역 이름은 소문자로 시작한다. 한 위치에서 이름 하나는 대상 하나만 가리킨다. |
| 수명 hook | Unity의 hook 체계(종류·의미)를 따르고 이름은 Rocks 어휘를 사용한다: `Awake`, `Enable`, `Start`, `Tick`, `FixedTick`, `LateTick`, `Disable`, `Destroy`, 물리 `TriggerEnter` 등. 프레임 안 순서와 Start 시점은 엔진 World의 tick phase를 따른다(SD24·SD26). 종료 경로마다 실행하는 hook을 정한다(SD20). |
| 객체 참조 | "없음"은 하나다. 다른 Actor·Component는 `T?`로 받고 약한 handle로 저장하며 파괴되면 null로 읽힌다. 스크립트 단계(§5.4) 안의 파괴 요청은 그 단계가 끝날 때 World에 적용된다(SD3·SD25). 스크립트 상태는 GC root가 되지 않는다. 엔진 쪽 저장 표현은 같은 저장 단위 안 대상만 보존하는 `WeakObjectRef` 필드다(현재 코드 이름 `WeakRef` — 개명은 §17.2 코드 후속. 셀 사이 참조는 World의 영속 entity 참조 — RX 5.1절). |
| 이벤트 | `on 경로.이벤트(...)` 한 형태다. 명시 구독·closure가 없으며, 발행은 큐에 등록되어 현재 실행 조각(§5.4)이 끝난 뒤 같은 스크립트 단계 안에서 순서대로 전달된다. 기다리는 handler는 실행 중에 온 이벤트의 처리 방식을 표시한다(SD11). |
| 유지 자원 | 멈출 때까지 계속되는 자원(반복 사운드·입력 context·로딩 요청 등)은 필드나 지역 변수(`var`·`let`)에 담아야만 생성되고(로딩 요청은 지역 변수 전용), 블록이 끝나거나 다른 값을 넣거나 객체가 비활성화·종료되면 멈춘다. 복사하거나 다른 이름으로 옮길 수 없다. |
| 비동기 로딩 | `try await X.Load()` 하나로 로드하고, 상주는 결과 값을 보관하는 것으로 유지한다. 진행률은 지역 요청 handle로 조회한다. 이동·travel·준비 대기는 엔진이 prefetch와 로딩 화면을 관리한다. |
| 누수 | 소유 범위를 Session·Instance·Execution·ModuleHost로 고정하고, 범위 종료의 일괄 해제는 실패할 수 없는 3개 연산(EndExec·Detach·CloseSession)으로만 수행한다. 범위 종료마다 0을 검사하고 장시간 세션의 증가 추세를 Gate로 검사한다. |
| 실행 | 레지스터 VM(Scalar·Ref·Buf bank), bytecode 사전 컴파일(`.sparkc`), 스크립팅 backend 선택(VM 또는 native AOT = typed IR → C++, Unity의 Mono/IL2CPP 선택과 같은 맥락, target·빌드 구성별 옵션, SD31), 런타임 eval(target 정책, 기본 꺼짐), 컴파일러를 공유하는 LSP 언어 서버. |
| 핫 리로드 | 편집기·개발 빌드에서 저장하면 실행 중인 게임에 반영된다. 함수 본문(L1)부터 필드·`on`·component 구성·클래스 계층·`@net`·`@rpc`(L2–L3)까지 제자리 migration 또는 instance 재생성으로 처리하고, 계획·검증이 끝난 변경만 원자적으로 적용하며 실패하면 이전 revision을 유지한다. PIE 재시작은 엔진 C++ 변경과 패키지 의존 graph 변경에만 남긴다(§12.4, SD28–SD30). |
| 오류 방어 | 사용자 코드의 어떤 실수도 process·편집기·다른 instance를 망가뜨리지 않는다. 컴파일 오류는 마지막 성공 revision을 유지하고, 실행 오류는 실행 트리 → instance 순으로 범위를 좁혀 격리하며, 노출 API는 입력 오류로 Fatal하지 않는다(§8.4, SD32). |

## 1. 요구와 설계 원칙

### 1.1 요구와 대응

| 요구 | 설계 대응 | 절 |
|---|---|---|
| 정적 타입 | 모든 선언·식의 타입을 컴파일 시 확정한다. 함수 시그니처는 타입을 명시하고 지역 변수와 초기값이 있는 필드만 추론한다. null 가능성도 타입에 포함한다. | 4 |
| 평범한 문법, Unity 수준의 접근성 | C 계열 문법, Unity hook 체계, 선언 한 줄 노출, 저장 즉시 반영, Unity 대응표. | 2·3·5 |
| 비프로그래머 대상의 방어적·단순한 설계 | 경고 없는 오류, 한 가지 방법, 실수 구조의 언어 차원 제거, core·full 프로필 구분, 이름 하나에 대상 하나. | 1.2·3·4 |
| 메모리·리소스 누수 없음 | 고정 소유 범위와 해제 연산, 범위 종료 0 검사, soak Gate. | 7·9 |
| 코어·엔진·이벤트 접근 | 리플렉션에서 생성한 binding schema로 C++ 타입·필드·함수·이벤트를 사용한다. | 5·10 |
| 비동기 로딩 | 전체 계획 §9.2 LoadTicket·AssetLease 계약에 대응하는 `Load`·요청 handle·이동·travel. | 7 |
| VM, 사전 컴파일, AOT | 레지스터 VM, `.sparkc`, 선택적 C++ 변환. | 11·12 |
| 런타임 eval | 컴파일러를 런타임에 포함할 수 있는 구조, target 정책. | 12.6·12.7 |
| 핫 리로드 필수 | 등급별 reload(본문 → 제자리 migration → instance 재생성), 원자적 적용·롤백, 편집기 요약, L1부터 단계적 도입. | 12.4 |
| 오류에 방어적 | 범위를 좁히는 fault 처리, 권한 구역 분석, 폭주 상한, 편집기 문맥 transaction 취소, 노출 API의 Fatal 금지, 격리 복구. | 8 |
| 선택형 native AOT | target·빌드 구성마다 VM과 AOT 중 선택(IL2CPP 맥락), 두 backend의 비트 동일 결과, VM 항상 포함. | 12.5 |
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

### 1.3 core 프로필과 full 프로필

언어는 하나다. 패키지 manifest의 `profile: core | full`이 사용할 수 있는 문법을 정한다. 콘텐츠 패키지와 Studio의 "새 스크립트"는 core가 기본값이다.

| 프로필 | 포함 |
|---|---|
| core | `import`, `class`(Sparks 클래스끼리 상속 불가), `struct`, `enum`, `event`, `fn`/`async fn`, `const`, `var`/`let`, `if`/`for`/`while`/`switch`, `on`, `emit`, `await`, `try`, `override`(엔진 가상 함수 재정의); `@edit`·`@save`·`@net`·`@authorityOnly`·`@formerly`·`@restart`·`@skipWhileBusy`·`@tableRow`; 모든 수명 hook(`Validate` 제외); `All`·`Race`·`Delay`·`NextTick`·`NextFixedTick`; `Load`·`Assets.LoadAll`·`Owner.Teleport`·`Session.Travel`; 부록 B의 core 표준 API |
| full | `interface`, `open`(Sparks 클래스 상속·재정의 허용), `Class<T>`, `@rpc`, `@worker`·`@deterministic`·`@pure`, `@tick(Phase:, Interval:, Priority:)`, `@cheat`·`@editorButton`, `@cvar`, hook `Validate`, 실행 중 component 추가, bundle·우선순위 지정 로드, `World.Prefetch`·스트리밍 source·data layer 제어, 로딩 화면 직접 제어, `@test` |

사용자 정의 오류 타입은 두지 않는다(SD38). 오류는 엔진이 정한 닫힌 enum `Error`(부록 B) 하나다. core 패키지에서 full 문법을 작성하면 컴파일 오류이며 기능 이름과 core 대안을 제시한다. core 코드도 full 패키지가 제공하는 함수·이벤트는 호출·수신할 수 있다. 언어 서버는 core 패키지에서 full 전용 후보를 표시하지 않는다.

### 1.4 제외 범위

전체 계획 §23.1의 제외 항목(임의 포인터 연산·unchecked cast·native heap 접근·사용자 정의 allocator)을 유지한다. 추가로 동적 타입, 예외, closure와 함수 값, 명시 구독, 비구조적 coroutine, `defer`, 독립 스크립트 객체, 사용자 정의 generic, 사용자 함수·연산자 오버로딩, 기본 인자, 조건 연산자, 비트 연산자, 문자열 `+`, 강제 unwrap, 변경 가능한 전역 변수, 사용자 정의 오류 타입, `?? break`·`?? continue`를 제외한다.

## 2. 예시와 표기 반영

### 2.1 플레이어 캐릭터

`Characters/PlayerCharacter.spark`:

~~~text
import Weapons.Sword;
import Items.Pickup;
import Characters.Enemy;

class PlayerCharacter : Character {                  // Pawn 파생(Character는 자리표시): 자기 자신은 this
    @edit @net var MoveSpeed: float = 600.0;         // 초당 600 cm
    @edit let MaxHealth: float = 100.0;              // 생성 후 바뀌지 않는다

    let PickupRange = SphereCollider(Radius: 200.0); // 소유 component, 반지름 cm
    let MoveAction = Action("Gameplay.Move");        // 입력 action, 컴파일 시 검증
    let JumpAction = Action("Gameplay.Jump");        // 기반 멤버 Jump()와 이름이 겹치지 않게
    var CurrentTarget: Enemy?;                       // 다른 Actor: 파괴되면 null (기본값 null)
    var EquippedSword: Sword?;

    fn CalculateDamage(base: float, multiplier: float): float {
        return base * multiplier;
    }

    on Start() {
        PickupRange.RelativeLocation = vec3(0.0, 0.0, 80.0);   // cm
        Log.Info("Rocks Engine & Sparks Ready!");   // 카테고리 Script.Characters
    }

    on JumpAction.Triggered() {                      // 입력 phase 이름 그대로(SD35)
        AddImpulse(vec3(0.0, 0.0, 500.0));          // kg·cm/s(PhysicsJoltAdapter 3절 U1)
    }

    on Tick() {                                      // 인자 dt는 필요할 때만 선언
        let moveInput = MoveAction.Value;            // vec2
        if (moveInput.Length() > 0.0) {
            let dir = GetForwardVector() * moveInput.Y + GetRightVector() * moveInput.X;
            AddMovement(dir.Normalize() * MoveSpeed); // 초당 속도. dt는 엔진이 적용
        }
        if (let target = CurrentTarget) {
            LookAt(target.Location);
        }
    }

    on TriggerEnter(other: Actor, trigger: Collider) {   // 자기 Actor의 모든 collider에서 온다
        if (trigger != PickupRange) { return; }
        if (let pickup = other.Find<Pickup>()) {
            pickup.Collect(this);
        }
    }
}
~~~

### 2.2 전체 계획 §23.1 예시의 확장

상호작용은 서버에서 발행되는 Actor 확장 이벤트로 받는다. 모든 기기의 연출은 복제 필드의 현재 값(Tick)과 변경 이벤트(Changed)로 처리한다.

`Props/DoorLogic.spark`:

~~~text
import Gameplay.Interaction;     // @authorityOnly event Actor.Interacted(actor: Actor);
import Items.Inventory;

enum DoorState { Closed, Opening, Open }

class DoorLogic : ScriptComponent {
    @edit @save var OpenAngle: float = 90.0;           // Rotator 각도: 도
    @edit var Hinge: SceneComponent;               // ? 없음: 같은 prefab 안에서 반드시 지정
    @edit @save var RequiredKey = Tag("Item.Key.Iron");
    @edit(Min: 0.05) var SwingSeconds: float = 0.6;     // 0으로 나누기 방지
    @edit var OpenSound: SoundAsset = Asset("/Game/Audio/DoorOpen");
    @net var State = DoorState.Closed;
    var ShownState = DoorState.Closed;                 // 이 기기에 마지막으로 반영한 상태(복제하지 않음)
    var Swing = 0.0;                                   // 이 기기의 회전 진행률 0–1(복제하지 않음)

    event Opened(by: Actor?);

    on Enable() {                                       // 활성화될 때마다 현재 상태 반영
        ShownState = State;
        ApplyPose();
    }

    @skipWhileBusy                                      // 여는 동안 온 상호작용은 버린다
    on Owner.Interacted(actor: Actor) {                // 상호작용 시스템이 서버에서 발행
        if (State != DoorState.Closed || !HasKey(actor)) { return; }
        State = DoorState.Opening;
        await Delay(SwingSeconds, Clock.Game);
        State = DoorState.Open;
        emit Opened(actor);                             // await 이후 actor는 Actor?
    }

    on Disable() {                                      // 여는 도중 취소되면 권한 측이 되돌린다
        if (Owner.HasAuthority && State == DoorState.Opening) { State = DoorState.Closed; }
    }

    on State.Changed() {                                // 모든 기기에서 실행, await 불가
        if (ShownState == DoorState.Closed) {           // 닫힘에서 벗어나는 전이에만 소리
            World.Audio.PlayAt(OpenSound, Owner.Location);   // 일회성 효과: handle 없음
        }
        ShownState = State;
        ApplyPose();
    }

    on Tick(dt: float) {                                // 모든 기기에서 회전 연출
        if (State != DoorState.Opening) { return; }
        Swing = Math.Min(Swing + dt / SwingSeconds, 1.0);
        Hinge.RelativeRotation.Yaw = OpenAngle * Math.EaseOut(Swing);
    }

    fn ApplyPose() {
        if (State == DoorState.Closed) { Swing = 0.0; }
        if (State == DoorState.Open) { Swing = 1.0; }
        Hinge.RelativeRotation.Yaw = OpenAngle * Math.EaseOut(Swing);
    }

    fn HasKey(actor: Actor): bool {
        if (actor.Tags.Has(RequiredKey)) { return true; }
        let bag = actor.Find<Inventory>() ?? return false;
        return bag.Contains(RequiredKey);
    }
}
~~~

1. `on Owner.Interacted`는 Actor 확장 이벤트다. 전체 계획 예시의 `on Interact`를 interface·message 없이 같은 `on 경로.이벤트` 형태로 처리한다(§5.2).
2. `Interacted`는 `@authorityOnly` 이벤트이므로 그 handler 안의 `@net` 쓰기(`State = ...`)는 권한 구역으로 판정된다(§14, SD45). `await`가 있는 이벤트 handler는 실행 중에 같은 발행자(이 문 Actor)에서 온 이벤트의 처리 방식을 표시해야 하며, `@skipWhileBusy`는 버리고 `@restart`는 다시 시작한다(§6.1, SD11).
3. `actor`는 전달 시점에 유효하고 `await`를 지난 뒤 읽으면 `Actor?`다(§4.3). `Opened`의 인자를 `Actor?`로 선언하여 actor가 사라져도 발행이 누락되지 않는다.
4. 현재 상태는 활성화될 때마다 실행되는 `Enable`에서 반영한다. `Changed`는 활성화 전에 적용된 값(초기값·로드·late join)에는 발행되지 않고, 비활성 동안에는 연결이 꺼져 있기 때문이다. Changed는 값이 실제로 바뀐 경우 최종 값으로 한 번 전달되므로 중간 값(Opening)을 건너뛸 수 있다. 그래서 일회성 연출은 복제하지 않는 `ShownState`와 비교해 전이로 판정한다(§5.3).
5. 회전 연출은 모든 기기의 `Tick`이 복제된 `State`를 보고 진행하며 서버 handler는 상태만 변경한다.
6. 전용 서버에서 `World.Audio.PlayAt`은 효과가 없는 호출이다. 서버와 클라이언트가 같은 코드를 실행해도 결과가 달라지지 않는다(§7.5).
7. 여는 도중 비활성화되면 실행이 `await` 지점에서 취소된다. `await`는 실행이 끝날 수 있는 지점이므로(§6.1) `on Disable`에서 권한 측이 `State`를 되돌린다. `if (Owner.HasAuthority)` 블록은 권한 구역이므로 그 안의 `@net` 쓰기는 컴파일된다.
8. `Rotator`(`RelativeRotation.Yaw` 등)의 각도는 UE `FRotator`와 같은 도다. 삼각함수(`Math.Sin` 등)와 quaternion 축-각은 라디안이며 `Math.Radians`·`Math.Degrees`로 바꾼다(§4.2, SD18).

### 2.3 제시한 표기의 반영

| 제시 표기 | 반영 | 비고 |
|---|---|---|
| `var MoveSpeed: float = 600.0;`, `fn X(...): float`, PascalCase 멤버, 소문자 내장 타입, `Sword?`, `null`, `@edit @net`, `@edit let`, `if (...)`, 암묵적 `this`, `Log`, 파일 경로 import | 채택 | 수치의 단위는 엔진 규약(UE 5.8과 같은 cm·초)이므로 예시 값 `600.0`(cm/s)을 그대로 쓴다(§4.2, SD18 — 초안 1.1의 미터 규약에서 `6.0`으로 바꿨던 것을 되돌렸다) |
| `on Start()`, `on Tick(dt: float)` | 채택 | hook은 Unity 체계를 따르고 이름은 Rocks 어휘(§5.1). 인자는 필요할 때만 선언 |
| `Weak<Enemy>`, `Lock()` | 수정 | `Enemy?` 하나로 통합(§4.3). C++ 쪽은 `WeakObjectRef<T>` 필드(리플렉션 B9 — ReflectionExtensions 5.1절) |
| `var Trigger = SphereCollider(200.0);` | 수정 | 소유 component는 `let`, 생성 인자는 필드 이름, 필드 이름은 용도: `let PickupRange = SphereCollider(Radius: 200.0);`(cm) |
| `Trigger.SetRelativeLocation(...)` | 수정 | 쓰기 가능한 속성이 있으면 setter 함수를 노출하지 않는다: `PickupRange.RelativeLocation = ...` |
| `Input.GetAxis2D("MoveForward", "MoveRight")` | 수정 | action 필드 `let MoveAction = Action("Gameplay.Move")`와 `MoveAction.Value`. 버튼 이벤트는 입력 phase 이름(`Triggered`·`Started`·`Completed`·`Cancelled`, SD35) |
| `Log("...")` | 수정 | 레벨을 고르는 `Log.Info`·`Log.Warning`·`Log.Error`·`Log.Debug`(SD36) |
| `AddMovement(dir * MoveSpeed * dt)` | 수정 | 이동 API는 초당 속도를 받고 dt는 엔진이 적용(§10.3) |

### 2.4 Unity 사용자를 위한 대응표

| Unity (C#) | Sparks | 차이 |
|---|---|---|
| `class X : MonoBehaviour` | `class X : ScriptComponent` | — |
| `[SerializeField]`, public 필드 | `@edit`, `@save` | 편집기 노출과 저장을 구분 |
| `Awake` · `OnEnable` · `Start` | `on Awake()` · `on Enable()` · `on Start()` | Start는 준비 대기(로드 등)만 기다릴 수 있고 끝날 때까지 Tick은 시작되지 않는다(SD12). 현재 상태 반영은 활성화마다 실행되는 Enable에서 한다 |
| `Update` · `FixedUpdate` · `LateUpdate` | `on Tick()` · `on FixedTick()` · `on LateTick()` | Tick 계열에서는 기다리기·생성·자원 획득 불가 |
| 프레임 안 호출 순서(FixedUpdate → Update) | Tick → (FixedTick → 고정 물리) × n → 물리 이벤트 → LateTick | 순서는 엔진 World tick phase(UE tick group)를 따른다. FixedTick은 Tick 뒤이고 물리 이벤트는 프레임당 1 batch다(SD26) |
| `Start`(첫 Update 직전) | `on Start()` | 다음 프레임 시작 처리에서 시작한다(엔진 World 규칙, SD24) |
| `OnDisable` · `OnDestroy` | `on Disable()` · `on Destroy()` | 비활성화되면 실행 중인 실행 취소, 유지 자원 해제, 이벤트 수신 중지(SD10) |
| `OnTriggerEnter` · `OnCollisionEnter` | `on TriggerEnter(other, trigger)` · `on CollisionEnter(other, hit)` | 상대 Actor를 바로 받는다. 자기 Actor의 모든 collider에서 온다 |
| `OnValidate` | `on Validate()` | full 프로필, 저장되지 않는 미리보기 상태만 변경 |
| `GetComponent<T>()` | `Owner.Find<T>()` | `T?` 반환. 자기 Actor의 component만 찾는다 |
| `StartCoroutine` + `WaitForSeconds` | handler 안에서 `await Delay(2.0, Clock.Game)` | instance가 비활성화·파괴되면 자동 취소. 실행 중에 온 같은 발행자의 이벤트는 `@skipWhileBusy`면 버리고 `@restart`면 다시 시작(SD11) |
| `Instantiate` · `Destroy(obj)` | `try World.Spawn(prefab, transform)` · `obj.RequestDestroy()` | 파괴는 현재 스크립트 단계 끝에 World에 적용되며 그때까지 순회·`if (let)`에 남는다. 객체 원소 `Array`는 `Count` 대신 `SlotCount`(파괴된 자리 포함)와 `AliveCount()`를 쓴다(SD14·SD40) |
| `Debug.Log` | `Log.Info(...)` | 카테고리는 패키지에서 자동(`Script.<패키지>`), 보간 `${x}`는 이름 있는 로그 인자가 된다(SD36) |
| Addressables `LoadAssetAsync` | `try await X.Load()` | 해제 호출이 없다. 결과를 보관하는 동안 상주한다 |
| `ScriptableObject` | `class X : DataAsset` | 실행 중 읽기 전용 |
| `SendMessage`, `UnityEvent` | `emit` + `on 경로.이벤트` | 타입 검사, 자동 연결·해제. `Invoke`는 즉시 실행하지만 `emit`은 현재 실행 조각이 끝난 뒤 전달 |
| Netcode `OnValueChanged` | `on X.Changed()` | 값이 실제로 바뀐 경우 현재 실행 조각이 끝난 뒤 최종 값으로 한 번 전달. 활성화 전 적용(초기값·로드·late join)과 비활성 동안의 변경에는 오지 않으므로 Enable에서도 반영 |
| 파괴된 객체의 `== null` | `T?` 검사 | `?.`·`??`·`if (let)`·`== null`이 모두 같은 판정. 타입을 좁히는 형태는 `if (let)`과 `?? return`뿐 |
| 정수 `/`(버림) | `Math.Div(a, b)` | `int / int`는 오류(SD17) |
| `for (int i = 0; i < n; i++)` | `for (i in 0..<n)` | 끝은 포함하지 않는다(SD16) |
| `Vector3.up`, 각도(도) | `vec3(0.0, 0.0, 1.0)`, `Rotator` 각도는 도 | UE 5.8과 같은 왼손 좌표계, +X 전방·+Y 오른쪽·+Z 위, yaw 양수는 오른쪽 회전(위에서 볼 때 시계), cm·초. 삼각함수·quaternion 축-각은 라디안(SD18) |

## 3. 문법

### 3.1 어휘와 명명

소스는 UTF-8이다. 식별자는 ASCII 영문자와 숫자로 쓰고 영문자로 시작하며 `_`를 포함하지 않는다. 단독 `_`는 버림·생략을 뜻하는 별도 토큰이다. 주석은 `//`와 `/* */`(중첩 없음)이다. `///`는 바로 뒤가 `/`가 아닐 때만 문서 주석이며 선언·멤버·enum case 바로 앞에만 쓰고, 그 밖의 위치에서는 오류이며 `//`를 제시한다. 문장은 세미콜론으로 끝내고 블록은 중괄호로 감싼다. 서식은 설정 없는 단일 공식 formatter가 정한다.[R21]

| 대상 | 규칙(첫 글자로 판정) | 예 |
|---|---|---|
| 키워드·attribute 이름 | 소문자로 시작 | `var`, `on`, `@edit`, `@tableRow` |
| 내장 값 타입(닫힌 목록, 예약어) | 소문자 | `bool`, `int`, `float`, `string`, `vec2`–`vec4`, `quat`, `mat4` |
| 그 밖의 타입·멤버·이벤트·enum case·attribute 인자 이름·생성 인자 이름 | 대문자로 시작(PascalCase) | `DoorState.Open`, `MoveSpeed`, `@edit(Min: 0)`, `Radius:` |
| 지역 이름: 함수·handler 인자, `var`/`let` 지역 변수, `if (let)` binding, 반복 변수 | 소문자로 시작(lowerCamel) | `moveInput`, `dt` |

- 명명 규칙 위반은 오류다.
- **이름 하나에 대상 하나:** 한 위치에서 보이는 이름(지역 이름, 자기 멤버와 상속 멤버(엔진 C++ 포함), 문맥 이름(§3.4), 최상위 선언, import한 선언, prelude, 타입)이 둘 이상의 대상을 가리키면 나중에 도입된 사용자 선언의 위치에서 오류다. 그래서 이름을 찾는 우선순위가 없다. 값 이름(지역 이름·Sparks 멤버·문맥 이름) 사이에서는 대소문자만 달라도 오류다(`owner`와 `Owner`). 엔진 C++ 상속 멤버와는 철자가 정확히 같을 때만 충돌로 보므로, 엔진이 기반 타입에 멤버를 추가해도 사용자 지역 이름이 오류가 되지 않는다. 엔진 멤버 추가로 Sparks 멤버와 정확히 같은 이름이 생기면 언어 버전 변경처럼 그 선언 위치에서 오류와 이름 변경 빠른 수정을 제시한다. 지역 이름은 둘러싼 범위의 이름과만 비교하므로 끝난 형제 블록의 이름은 다시 쓸 수 있고, 타입 이름과는 대소문자만 달라도 된다(`let health = Target`).
- 멤버 이름은 그 위치에서 보이는 타입 이름과 같을 수 없다(`var Health: Health?`는 오류, `var HealthComp: Health?`). 문맥 이름·기반 타입 속성과 같은 이름의 엔진 타입(`World`, `Widget`)은 정적 멤버를 노출하지 않으므로 식에서는 항상 문맥 이름·속성이다.
- 한 타입의 필드·함수·이벤트는 이름 공간 하나를 공유하고 한 모듈의 최상위 선언도 이름 공간 하나다. 같은 이름은 오류다.
- 상속 멤버와 같은 이름의 선언은 오류다. 예외는 시그니처가 같은 재정의 가능 함수의 `override fn`이며, 재정의할 대상이 없는 `override`도 오류다.
- 키워드, 내장 타입, predeclared 이름(prelude·검증 리터럴·문맥 이름·RPC 접두·`Changed`)은 사용자 선언 이름으로 쓸 수 없다. hook 이름은 수명 클래스와 그 기반 타입의 멤버 이름으로만 쓸 수 없으며 struct 필드·enum case에는 쓸 수 있다. 전체 목록은 부록 A(§3.7)다.
- 오버로딩은 prelude 수학 함수(`Math.Min`·`Max`·`Abs`·`Clamp` 등, `int`와 `float` 공용)에만 있다. 모든 인자가 `int`면 int 판, 그 밖에 float 판에 맞으면 float 판을 고르며 어디에도 맞지 않으면 오류다.

### 3.2 선언과 기본값

| 선언 | 의미 | 예 |
|---|---|---|
| `class` | 엔진 수명 타입(`ScriptComponent`, Actor·Pawn 파생 `Character`, `WorldService`, `WidgetController`) 또는 `DataAsset`을 상속. 기반 타입이 수명과 문맥 이름을 정한다(§3.4). core에서 기반은 엔진 C++ 타입뿐이고 Sparks 클래스는 상속할 수 없다. full에서는 `open class`로 선언한 Sparks 클래스도 기반이 될 수 있다(SD27) | `class DoorLogic : ScriptComponent { }` |
| `struct` | 값 타입. 상속 없음. 스크립트 데이터는 struct로 표현한다 | `struct HitInfo { var Point: vec3; var Damage: int = 1; }` |
| `enum` | 열거. 플래그 조합은 `Set<E>` | `enum DamageKind { Physical, Fire, Poison }` |
| `event` | 클래스 안의 멤버 이벤트, 최상위의 Actor 확장 이벤트·World 이벤트 | `event Died(killer: Actor?);` · `event Actor.Interacted(actor: Actor);` · `event World.GameStarted();` |
| `fn`, `async fn` | 함수와 기다리는 함수 | `fn Lerp(a: float, b: float, t: float): float` |
| `const` | 컴파일 시 상수. 타입은 `bool`·`int`·`float`·`string`·enum·`Tag`·수학 타입. 값은 모듈 interface hash에 포함되므로 값이 바뀌면 쓰는 모듈이 다시 컴파일된다 | `const MaxStack: int = 99;` |
| `interface` (full) | 함수 시그니처만 가진 계약(§10.3 CU9) | `interface Damageable { fn ApplyDamage(amount: int); }` |

- **기본값(zero value):** 초기값이 없는 필드는 `0`, `0.0`, `false`, `""`, `null`, 빈 컨테이너, enum의 첫 case, 벡터 0, `quat`·`mat4`·`Transform`은 단위(UE `FQuat::Identity`·`FTransform::Identity`), `Rotator`는 (0, 0, 0), `Color`는 (0, 0, 0, 0), `Text`는 빈 Text로 시작한다. `Tag`·`InputAction`과 `?` 없는 객체·에셋·`Soft<T>`·`Row<T>` 필드에는 기본값이 없으므로(`Tag?`의 없음이 None Name이기 때문 — §15) `@edit`로 지정하거나 초기값(소유 component 생성식, `Asset(...)`·`Row(...)` 리터럴)을 두어야 하며 지정이 누락되면 쿡 오류다(SD19). 필드 초기값은 클래스 기본값 레코드에 들어가므로, non-null 참조 초기값(`Asset(...)`·`Soft<T>`·`Row(...)`)은 레코드의 참조 의존 수집(대상 정체성 비교, override하지 않은 값의 레코드 참조를 패키지 의존에 포함)이, 소유 component 생성식은 default subobject가 선행 조건이며 둘 다 해당 도입 단계(L2) 전에 갖춘다([RuntimeClassMetadata](RuntimeClassMetadata.ko.md) '클래스 기본값 결정'). 선택 값은 `T?`로 선언한다. 지역 변수는 선언할 때 초기화해야 한다.
- **`?` 없는 객체 필드:** (a) 소유 component를 생성하는 `let` 필드와 (b) prefab 안의 대상을 가리키는 `@edit` 필드(component·Actor 파생 클래스)에만 허용한다. 그 밖(WorldService·WidgetController·DataAsset의 객체 필드, `@edit` 없는 필드)은 컴파일 오류이며 `T?`를 제시한다.
- **struct 필드:** struct의 객체 필드는 `T?`만 허용한다. `?` 없는 에셋·`Soft<T>`·`Row<T>` 필드는 생성할 때 이름 인자로 반드시 지정한다.
- **필드 쓰기:** class의 Sparks 필드는 자기 instance의 코드(파생 클래스 포함)에서만 쓸 수 있고 다른 객체의 Sparks 필드는 읽기만 가능하다. 엔진 객체의 쓰기 가능한 native 속성(`ScriptWritable`)은 밖에서도 쓸 수 있다. struct 필드는 대입할 수 있는 경로(§4.4)를 통해 어디서나 쓴다.
- **`let`:** 재대입할 수 없다. 값 타입이면 내부 필드·원소 수정과 변경 메서드 호출도 금지된다. 객체 handle을 담은 `let`은 handle만 고정되며 대상 객체의 속성은 쓸 수 있다(`let PickupRange = ...` 뒤의 `PickupRange.RelativeLocation = ...`). 지역과 필드에 같은 규칙을 적용하며, `let` 필드의 값은 spawn parameter 적용(§4.5)까지 정해진다. `@save let`·`@net let`은 오류다.
- **함수:** public이다. 엔진 기반 타입의 재정의 가능한 함수는 `override fn`으로 재정의한다. full에서는 `open fn`이 Sparks 파생 클래스의 재정의를 허용하고, `override fn`은 다시 재정의할 수 있다. struct의 함수는 자기 필드를 쓸 수 없다. 반환 타입이 있는 함수는 모든 경로가 `return`해야 하며, `Result<void>` 함수는 끝에 도달하거나 `return;`하면 성공이다. `Result<T?>`는 오류다.
- **최상위 `fn`:** `this`·문맥 이름이 없는 순수 계산 함수다. 엔진 객체를 바꾸는 API·`emit`·`await`를 쓸 수 없다. struct의 `fn`도 문맥 이름이 없다.

### 3.3 문장과 식

문장과 식:

~~~text
var speed = 6.0;                              // float로 추론
var hits: Array<HitInfo> = [];
var scores: Map<string, int> = ["Red": 0, "Blue": 0];
for (item in Items) { … }                     // 시작 시점의 내용을 순회한다
for (i, item in Items) { … }                  // 번호와 원소
for (team, score in scores) { … }             // Map은 key와 값
for (_ in 0..<count) { }                      // 0 이상 count 미만, 변수가 필요 없으면 _
while (t < 1.0) { await NextTick(); }         // async 문맥 전용. 모든 경로에 await 또는 탈출

switch (State) {                              // enum은 모든 case를 처리해야 하며 default 없음
    case DoorState.Closed: { Open(); }
    case DoorState.Opening, DoorState.Open: { }
}                                             // enum이 아닌 switch는 default 필수

let label = player?.DisplayName ?? "Unknown";
let text = "HP ${Current}/${MaxHealth}";                 // string. 화면 표시는 Loc
let ratio = float(hits.Count) / float(total);            // int / int는 오류
if (let boss = target as? BossEnemy) { boss.Enrage(); }
if (let first = Enemies[0]) { first.Alert(); }          // 번호 읽기는 T?
let bag = Owner.Find<Inventory>() ?? return;            // null이면 함수 종료(지역 변수 선언에서만)
let mesh = try await MeshAsset.Load();                  // 실패하면 이 실행을 끝내고 기록
_ = bag.Add(item);                                      // 결과를 버릴 때는 _ 로 명시
~~~

- **숫자:** 암묵적 숫자 변환은 없다. 상수식의 0 나누기·overflow·NaN·Inf는 컴파일 오류다. 정수 리터럴(단항 `-`와 괄호 포함)은 타입이 명시된 선언·인자·반환·대입 우변과, 다른 피연산자가 `float`인 산술·비교에서 `float` 상수로 쓴다. const 이름과, 정수 리터럴 하나가 아닌 상수식(`2 * 3` 등)에는 이 승격을 적용하지 않는다. 명시 변환은 `float(i)`(2^53을 넘어 정확히 표현할 수 없으면 `IntegerOverflow` fault)와 `Math.FloorToInt`·`CeilToInt`·`RoundToInt`·`TruncToInt`(`RoundToInt`의 0.5는 0에서 먼 쪽, 범위 밖은 `IntegerOverflow`)이다. `Math.Floor` 등은 `float`를 반환한다(부록 B).
- **나눗셈:** `int / int`는 오류다(SD17). 몫은 `Math.Div(a, b)`(내림, `a == Math.Div(a, b) * b + a % b`), 비율은 `float(a) / float(b)`로 쓴다. 피연산자에 `float`가 있으면 `/`는 `float`를 반환한다. `%`는 정수 전용 내림 방식이며 `int` 변수의 `/=`는 오류다.[R30]
- **비교:** `==`·`!=`는 `bool`, `int`, `string`(UTF-8 바이트 비교), enum, `Tag`, `InputAction`, `Soft<T>`·`Row<T>`(ID 비교), 객체, 그리고 이들의 `T?`에만 쓴다. 객체 비교는 파괴되었으면 null로 읽은 뒤의 동일성이며 한쪽이 다른 쪽의 기반 타입일 때만 쓸 수 있다. `float`·수학 타입·struct·`Text`·컨테이너·`Result`·에셋 값의 `==`는 오류이며 `float`·vector는 `Math.NearlyEqual`을 쓴다. `?` 없는 `T`를 `null`과 비교하는 것도 오류다. Map 키·Set 원소·switch 대상은 `==`를 쓸 수 있는 타입만 허용하고, case에는 상수식만 쓰며 같은 case의 중복은 오류다. enum switch는 모든 case를 처리하고 `default`가 없으며(엔진 enum `Error`는 case가 늘 수 있으므로 `default` 필수), 그 밖의 switch는 `default`가 필수다.
- **문자열:** `+`는 없고 보간 `"${x}"`만 사용한다. 보간할 수 있는 타입은 `bool`·`int`·`float`·`string`·enum(case 이름)·`Tag`·수학 타입이다. `float`는 다시 읽었을 때 같은 값이 되는 가장 짧은 10진 표기로 쓰고 정수값이면 끝에 `.0`을 표기한다. `T?`·객체·struct·컨테이너·`Text`의 보간은 오류다.
- **조건과 평가:** 대입은 문장이다. 조건식은 `bool`만 허용한다. 평가 순서는 항상 왼쪽에서 오른쪽이며 `&&`·`||`·`??`는 왼쪽 값으로 결과가 정해지면 오른쪽을 평가하지 않는다. `break`는 반복문만 대상으로 한다.
- **사용하지 않는 이름과 버린 결과:** 사용하지 않는 지역 변수·import, 버린 결과는 오류다. "사용하지 않음"은 어느 경로에서도 읽지 않는 것이다. `void`가 아닌 모든 결과(`try`로 꺼낸 값과 `Result<void>` 포함)는 사용하거나 `_ =`로 버리며, 유지 자원은 `_ =`로도 버릴 수 없다. handler 인자는 이벤트 인자 목록의 앞에서부터 필요한 만큼 선언하고, 쓰지 않는 앞쪽 인자와 `override fn`의 쓰지 않는 인자는 이름을 `_`로 쓴다. 그 밖의 함수 인자를 쓰지 않으면 오류다. 반복 변수가 필요 없으면 `_`를 쓴다. 유지 자원 지역 변수는 보관 자체가 사용이다(§7.1).
- **`await`·`try`:** 문장 수준(문장 전체, 지역 변수 선언의 초기값, 단순 대입의 우변, `return`)에서만 쓰며 바로 뒤의 호출·이벤트·요청 하나에 적용된다(§3.6 `Value`). `await F() + 1`과 복합 대입(`+=` 등)의 우변은 오류이며 지역 변수로 먼저 받는다. `?? return`도 지역 변수 선언에서만 쓴다(`?? break`·`?? continue`는 없다 — SD39).
- **기대 타입:** 기대 타입이 없는 자리의 `Asset(...)`·`Row(...)`·`[]`·`null`은 오류이며 타입을 명시한다(`var mesh: Soft<Mesh> = Asset(...)`). 배열 리터럴의 원소는 같은 타입이어야 하고(정수 리터럴의 `float` 승격만 예외) 기반 타입으로 합치지 않는다. 기대 타입이 있는 `[]`는 빈 Array·Map·Set이며 Map 리터럴은 `["Red": 0]`이다.
- **반복:** `for`의 binder는 Array가 (번호, 원소), Map이 (key, 값)이고 Set과 범위는 binder 하나다. 범위 `a..<b`는 a 이상 b 미만이며 `for` 머리에서만 쓴다(SD16).
- **인자 형태:** 소문자 내장 수학 타입의 생성(`vec3(0.0, 0.0, 1.0)`, `quat(X, Y, Z, W)` 순)과 함수 호출은 위치 인자만 쓴다. struct·component·`Rotator`·`Color`·`Transform`·`WorldPosition`의 생성은 이름 지정 인자만 쓰며 순서는 자유이고 중복은 오류다. 검증 리터럴 `Loc`은 첫 인자(key) 뒤에 catalog 인자를 이름 지정으로 쓴다(`Loc("Hud.Hp", Current: hp, Max: MaxHealth)`). 한 호출에서 두 형태를 섞지 않는다. attribute 인자는 attribute마다 정한다.
- **enum 값:** 식 안의 enum 값은 항상 타입 이름으로 한정한다(`DoorState.Open`, `Clock.Game`). 타입 이름 없는 짧은 이름은 attribute 인자(`@net(OwnerOnly)`, `@rpc(Server, Reliable)`)에서 그 attribute가 정의한 이름에만 쓴다(SD15).
- **`this`:** 값으로만 쓴다(`pickup.Collect(this)`). 자기 멤버는 `this.` 없이 이름으로 쓰며 `this.MoveSpeed`는 오류다. 예외는 Actor 파생 클래스가 자기 Actor의 이벤트를 받거나 보내는 `on this.Interacted(...)`·`emit this.Interacted(...)` 경로다.
- **`as?`:** class·interface 타입의 하향 변환과 interface 조회에만 쓴다(§4.3).

### 3.4 파일·import·패키지·문맥 이름

- 파일 하나가 하나의 소스 모듈이며 패키지 루트 기준 파일 경로가 import 경로다. 경로의 폴더·파일 이름은 대문자로 시작하는 ASCII 식별자이고 대소문자를 구분해 비교한다. 대소문자만 다른 두 경로는 오류다(파일 시스템마다 결과가 달라지므로). 같은 폴더에 `X.spark`와 폴더 `X/`를 함께 둘 수 없다.
- **패키지 manifest:** 패키지 루트의 `SparksPackage.json5`(파일 이름 규칙은 JSON5 2단계 결정을 따른다)는 `name`(PascalCase), `sparks`(언어 버전, 정수), `profile`(`core`·`full`), `dependencies`(패키지 이름과 interface 호환 범위), `services`(World마다 생성할 `WorldService` 클래스 목록), `logCategory`(생략 시 패키지 이름)를 가진다. 알 수 없는 key는 오류다.
- import는 그 모듈의 모든 최상위 선언을 짧은 이름으로 도입한다. 도입한 이름이 다른 import·현재 파일의 선언·prelude와 같으면 그 import 줄에서 오류다. `import Weapons.Sword as Blades;`처럼 모듈 별칭을 붙이면 그 모듈의 이름은 `Blades.Sword`로만 쓴다. 그 밖의 한정 경로(`Weapons.Sword.Sword`)는 없다.
- 의존 패키지의 모듈은 `import 패키지.경로;`로, 현재 패키지의 모듈은 패키지 이름 없이 import한다. 패키지 이름은 현재 패키지의 최상위 폴더 이름과 같을 수 없다.
- 파일 사이의 순환 참조는 허용하고 순환하는 `const` 초기화만 오류다. 패키지는 빌드·로드·reload 단위이며 패키지 사이의 순환 의존은 금지한다.
- 엔진 API는 `Rocks` 패키지에 있고 핵심 타입과 prelude 이름(부록 A)은 자동으로 import한다.
- 변경 가능한 전역 변수는 없다. 공유 상태는 `class X : WorldService`로 선언하고 manifest `services`에 등록한다(전체 계획 §22.6 PI03). WorldService는 World마다 생성되는 Object 기반 스크립트 수명 타입이며, 엔진 쪽 호스트는 World의 `WorldSubsystem` 하나(ScriptServiceHost)가 맡는다(SD22). travel·World 종료 때 정상 종료된다. 접근은 `World.Service<T>(): T`이며 등록되지 않은 타입은 컴파일 오류다.

클래스 메서드에서 쓸 수 있는 **문맥 이름**은 기반 타입이 정하며 표에 없는 이름은 오류다. 문맥 이름은 `this`의 멤버가 아니다. component의 `Owner`(소유 Actor)와 WidgetController의 `Widget`(제어하는 widget instance)은 기반 타입의 읽기 전용 속성이므로 다른 상속 멤버처럼 이름으로 쓰고 다른 객체에서도 읽는다(`target.Owner`).

| 기반 타입 | 문맥 이름 |
|---|---|
| `ScriptComponent`, `WidgetController`, Actor 파생(`Pawn`·`Character` 등) | `World`·`Session`·`Input`·`Time` |
| `WorldService` | `World`·`Session`·`Time` |
| `DataAsset`, struct `fn`, 최상위 `fn` | 없음 |
| 서버 RPC 함수 본문(full) | 위에 더해 `Caller: Connection`(호출한 connection) |

- `Session`(타입 `PlaySession`)은 그 코드가 실행되는 process의 PlaySession(전체 계획 §22.6)에 대한 스크립트 접근점이며 travel·respawn 요청 같은 게임 session 기능(전체 계획 §20.4)을 노출한다. 오디오·시퀀서 등은 `World.Audio`처럼 문맥 이름을 거친다.
- Actor 파생 클래스에는 `Owner`가 없으며 자기 자신은 `this`다. 자기 Actor의 이벤트는 `on this.이벤트(...)`로 받는다(§5.2).

### 3.5 검증 리터럴

엔진 데이터를 가리키는 문자열은 종류 이름으로 감싼다. 보간이 없는 문자열 리터럴만 받으며 컴파일러가 사전과 대조해 안정 ID로 변환한다. attribute의 문자열 인자에도 보간을 쓸 수 없다.

| 리터럴 | 결과 | 런타임 문자열 조회 |
|---|---|---|
| `Tag("Character.Player")` | `Tag` | `Tag.Find(s): Tag?` |
| `Action("Gameplay.Jump")` | `InputAction` | `Input.FindAction(s): InputAction?` |
| `Asset("/Game/Props/Door")` | 기대 타입이 `T`면 하드 참조, `Soft<T>`면 소프트 참조. 기대 타입이 없으면 오류 | `Assets.Find<T>(s): Soft<T>?` |
| `Row("Chest.Common")` | `Row<T>`(기대 타입 필요) | `T.FindRow(s): Row<T>?` |
| `Loc("Hud.Hp", Current: hp, Max: MaxHealth)` | `Text`(catalog key와 이름 지정 인자, 쿡 시 TextId로 변환하고 catalog 인자 이름·형식과 대조) | `Text.Find(key): Text?` |
| `CVar("game.door.speed")` | `CVarRef<T>`(기대 타입 필요, CVar 사전과 대조, §10.4) | `CVars.Find<T>(s): CVarRef<T>?` |

런타임 문자열을 이름 타입으로 변환하는 조회는 엔진의 이름 pool에 새 항목을 만들지 않는다(§9.5).

### 3.6 문법 요약

대입 대상은 `Postfix`로 읽은 뒤 의미 규칙으로 제한한다. 함수 호출 결과·`?.`·`this.`가 들어간 경로는 대입 대상이 될 수 없다(S0731). `??` 다음이 `return`이면 `Exit`로 해석한다. 식 안의 `<`는 `Ident` 바로 뒤에서 `Type { "," Type }`과 `>`가 이어지고 그다음 토큰이 `(` 또는 `.`일 때만 타입 인자로 판정하며 그 밖에는 비교다. 쉼표로 나뉜 두 비교(`F(a < b, c > (d))`)는 괄호로 구분한다. 타입 인자를 닫는 위치의 `>=`는 `>`와 `=` 두 토큰으로 분리한다. 이름 지정 인자는 생성 호출과 attribute에서만 허용한다.

EBNF:

~~~text
File         = { Import } { Decl } ;
Import       = "import" Path [ "as" Ident ] ";" ;
Path         = Ident { "." Ident } ;

Decl         = { Attribute } ( ClassDecl | StructDecl | EnumDecl | ExtEventDecl | ConstDecl | FnDecl | InterfaceDecl ) ;
Attribute    = "@" Ident [ "(" AttrArg { "," AttrArg } ")" ] ;
AttrArg      = [ Ident ":" ] Expr ;
ClassDecl    = [ "open" ] "class" Ident ":" Path { "," Path } "{" { Member } "}" ;   (* "open"·두 번째 이후 Path(interface)는 full *)
InterfaceDecl = "interface" Ident "{" { [ "async" ] "fn" Ident "(" [ Params ] ")" [ ":" Type ] ";" } "}" ;   (* full *)
StructDecl   = "struct" Ident "{" { { Attribute } ( FieldDecl | FnDecl ) } "}" ;
EnumDecl     = "enum" Ident "{" Ident { "," Ident } [ "," ] "}" ;
ExtEventDecl = "event" Ident "." Ident "(" [ Params ] ")" ";" ;      (* 첫 이름은 Actor 또는 World *)
ConstDecl    = "const" Ident ":" Type "=" Expr ";" ;

Member       = { Attribute } ( FieldDecl | FnDecl | EventDecl | Handler ) ;
FieldDecl    = ( "var" | "let" ) Ident [ ":" Type ] [ "=" Expr ] ";" ;
FnDecl       = [ "open" | "override" ] [ "async" ] "fn" Ident "(" [ Params ] ")" [ ":" Type ] Block ;   (* "open"은 full *)
EventDecl    = "event" Ident "(" [ Params ] ")" ";" ;
Handler      = "on" EvPath "(" [ Params ] ")" Block ;
EvPath       = ( Ident | "this" ) { "." Ident } ;
Params       = Param { "," Param } ;
Param        = ( Ident | "_" ) ":" Type ;
Type         = Path [ TypeArgs ] [ "?" ] ;
TypeArgs     = "<" Type { "," Type } ">" ;

Block        = "{" { Stmt } "}" ;
Stmt         = LocalDecl | If | For | While | Switch | Return | Emit
             | "break" ";" | "continue" ";"
             | Postfix "=" Value ";" | Postfix CompoundOp Expr ";"
             | "_" "=" Value ";" | Value ";" ;
Value        = Expr | "try" [ "await" ] Postfix | "await" Postfix ;
LocalDecl    = ( "var" | "let" ) Ident [ ":" Type ] "=" Value [ "??" Exit ] ";" ;
Exit         = "return" [ Expr ] ;
Emit         = "emit" EvPath "(" [ Expr { "," Expr } ] ")" ";" ;
If           = "if" "(" ( Expr | "let" Ident "=" Expr ) ")" Block [ "else" ( If | Block ) ] ;
For          = "for" "(" Binder [ "," Binder ] "in" Expr [ "..<" Expr ] ")" Block ;
Binder       = Ident | "_" ;
While        = "while" "(" Expr ")" Block ;
Switch       = "switch" "(" Expr ")" "{" { "case" Expr { "," Expr } ":" Block } [ "default" ":" Block ] "}" ;
Return       = "return" [ Value ] ";" ;
CompoundOp   = "+=" | "-=" | "*=" | "/=" ;

Expr         = Or ;
Or           = And { "||" And } ;
And          = Eq { "&&" Eq } ;
Eq           = Cmp [ ( "==" | "!=" ) Cmp ] ;
Cmp          = Coal [ ( "<" | "<=" | ">" | ">=" ) Coal ] ;
Coal         = Add [ "??" Coal ] ;
Add          = Mul { ( "+" | "-" ) Mul } ;
Mul          = Unary { ( "*" | "/" | "%" ) Unary } ;
Unary        = { "!" | "-" } Postfix [ "as?" Path ] ;
Postfix      = Primary { "." Ident [ TypeArgs ] | "?." Ident [ TypeArgs ] [ Args ] | Args | "[" Expr "]" } ;
Args         = "(" [ Arg { "," Arg } ] ")" ;
Arg          = [ Ident ":" ] Expr ;
Primary      = Literal | Ident [ TypeArgs ] | "this" | "(" Expr ")" | "[" [ Elem { "," Elem } ] "]" ;
Elem         = Expr [ ":" Expr ] ;
Literal      = IntLit | FloatLit | StringLit | "true" | "false" | "null" ;
~~~

어휘:

~~~text
Ident        = Letter { Letter | Digit } ;                 (* ASCII, "_" 제외 *)
IntLit       = Digit { Digit } ;
FloatLit     = Digit { Digit } "." Digit { Digit } [ Exp ] | Digit { Digit } Exp ;
Exp          = ( "e" | "E" ) [ "+" | "-" ] Digit { Digit } ;
StringLit    = '"' { Char | Escape | "${" Expr "}" } '"' ;
Escape       = "\" ( '"' | "\" | "n" | "t" | "$" | "u{" Hex { Hex } "}" ) ;
~~~

- `Char`는 `"`·`\`·줄바꿈을 제외한 문자이며 단독 `$`는 `$` 문자다. `${`만 보간을 시작하고 문자 `${`는 `\${`로 쓴다. `${ }` 안에는 문자열 리터럴을 쓸 수 없다. `\u{}`는 16진 1–6자리다.
- 정수 리터럴은 10진만 쓰고 자릿수 구분자는 없다. 지수 표기는 `FloatLit`에만 있다. 범위를 넘는 리터럴은 컴파일 오류이며 int64 최솟값은 단항 `-`와 결합할 때만 쓴다.
- 숫자 뒤의 `.`는 다음 문자가 숫자일 때만 `FloatLit`에 포함되므로 `0..<n`은 범위로 읽힌다. `1.`·`.5`는 쓸 수 없다.

**interface·open(full):** interface에는 함수 시그니처만 두며 필드·이벤트·기본 구현이 없다. class만 구현하고 struct는 구현하지 않는다. 구현 함수는 같은 이름·시그니처의 `fn`이며 `override`를 쓰지 않는다. interface 타입은 참조로만 쓰고(`I?`, `as? I`, `Owner.Find<I>()`) `@save`·`@net`·`@edit` 필드에 둘 수 없다. 메서드 추가는 interface의 끝에만 허용한다(slot 호환, §10.3 CU9). `open class`만 Sparks 파생 클래스의 기반이 되고 `open fn`만 재정의된다. 우선순위(높은 것부터): `.` `?.` `()` `[]` → `as?` → 단항 `!` `-` → `*` `/` `%` → `+` `-` → `??`(오른쪽 결합) → 비교(결합 없음) → `==` `!=`(결합 없음) → `&&` → `||`. `??`가 비교보다 높으므로 `n?.Count ?? 0 > 5`는 `(n?.Count ?? 0) > 5`다. `?.`의 단락 범위는 바로 뒤의 이름과 그 이름에 붙은 타입 인자·호출 인자까지다(§4.3).

### 3.7 부록 A: 예약 이름

| 분류 | 이름 | 사용 제한 |
|---|---|---|
| 키워드(full 전용 포함, 항상 예약) | `import` `as` `class` `struct` `enum` `event` `fn` `async` `const` `var` `let` `if` `else` `for` `in` `while` `switch` `case` `default` `break` `continue` `return` `on` `emit` `await` `try` `override` `this` `true` `false` `null` `interface` `open` | 식별자로 쓸 수 없다 |
| 내장 타입 | `bool` `int` `float` `string` `vec2` `vec3` `vec4` `quat` `mat4`, `void`(`Result<void>`의 타입 인자 전용) | 식별자로 쓸 수 없다. 타입과 생성·변환 호출로만 쓴다 |
| prelude | `Log` `Assert` `Math` `Random` `Format` `Parse` `Delay` `NextTick` `NextFixedTick` `All` `Race` `Assets` `CVars` `Settings` `Clock` `Error` | 사용자 선언 이름으로 쓸 수 없다 |
| prelude 타입 | `Array` `Map` `Set` `Result` `Soft` `Row` `Text` `TagSet` `InputAction` `LoadRequest` `Connection` `PlaySession` `Class` `CVarRef` `Rotator` `Transform` `WorldPosition` `Color` `Hit` | 사용자 선언 이름으로 쓸 수 없다 |
| 핵심 엔진 타입(언어 버전 1에 고정) | `Actor` `Pawn` `Component` `SceneComponent` `ScriptComponent` `WorldService` `WidgetController` `Widget` `DataAsset` `Collider` `World` | 사용자 선언 이름으로 쓸 수 없다. 그 밖의 `Rocks` 패키지 타입은 이름 하나에 대상 하나 규칙(§3.1)으로만 충돌을 판정한다 |
| 검증 리터럴 | `Tag` `Action` `Asset` `Row` `Loc` `CVar` | 사용자 선언 이름으로 쓸 수 없다 |
| 문맥 이름 | `World` `Session` `Input` `Time` `Caller` | 사용자 선언 이름으로 쓸 수 없다 |
| RPC 접두 | `Server` `AllClients` `OwnerClient` | 사용자 선언 이름으로 쓸 수 없다 |
| 예약 이벤트 이름 | `Changed` | 사용자 선언 이름으로 쓸 수 없다. `X.Changed`는 항상 필드 X의 변경이다 |
| hook 이름 | `Awake` `Enable` `Start` `Tick` `FixedTick` `LateTick` `Disable` `Destroy` `TriggerEnter` `TriggerExit` `CollisionEnter` `CollisionExit` `Validate`, 이들에 `On`을 붙인 이름, Unity hook 이름 `Update` `FixedUpdate` `LateUpdate` | 수명 클래스와 그 기반 타입의 멤버 이름으로 쓸 수 없다 |

목록에 이름을 추가하는 것은 언어 버전 변경이다(manifest `sparks` 값). 기존 코드와 충돌하면 컴파일러가 이름 변경 빠른 수정을 제시한다.

### 3.8 부록 B: 표준 API

언어가 소유하는 표준 API다. binding 생성물이 아니므로 이 표가 규격이다. 모든 함수는 위치 인자이며 fault 조건은 §8.3의 종류다. 이름 옆 (full) 표시가 없으면 core다.

| 대상 | API | 규칙 |
|---|---|---|
| `Math`(int·float 공용 판) | `Min` `Max` `Abs` `Clamp(x, lo, hi)` `Sign` | §3.1 오버로딩 규칙. `Clamp`의 `lo > hi`는 `AssertFailed`. `Abs`의 int64 최솟값은 `IntegerOverflow` |
| `Math`(int) | `Div(a, b)` | 내림 몫, `b == 0`은 `DivideByZero` |
| `Math`(float) | `Floor` `Ceil` `Round` `Trunc`(→ float), `FloorToInt` `CeilToInt` `RoundToInt` `TruncToInt`(→ int), `Sqrt` `Pow` `Exp` `Ln` `Sin` `Cos` `Tan` `Asin` `Acos` `Atan` `Atan2(y, x)`, `Radians` `Degrees`, `Lerp(a, b, t)` `InverseLerp(a, b, v)`, `EaseIn` `EaseOut` `EaseInOut`(t를 [0, 1]로 clamp), `NearlyEqual(a, b, tolerance)`, 상수 `Math.Pi` | 삼각함수는 라디안. 결과가 NaN·Inf면 `NonFinite`(`Sqrt(-1.0)` 등). **초월 함수는 VM·AOT가 같은 Core Math 함수를 호출한다**(§12.5). 플랫폼 간 비트 동일성은 Core Math의 결정성 수준(CurveData D17과 같은 UE 수준 — Windows x64에서 UE와 같은 비트)을 따른다. `InverseLerp`의 `a == b`는 `DivideByZero` |
| `vec2`–`vec4` | 필드 `X` `Y` `Z` `W`, `+` `-`(벡터끼리), `*` `/`(float와), 단항 `-`, `Dot` `Cross`(vec3) `Length` `LengthSquared` `Distance(b)` `Normalize()` | `Normalize()`는 길이가 1e-8 미만이면 0 벡터를 반환한다(UE `GetSafeNormal`). 성분별 곱은 `Math.Scale(a, b)` |
| `quat` | `quat(X, Y, Z, W)`, `*`(합성: `a * b`는 b를 먼저 적용 — UE `FQuat`과 같음), `Rotate(v)` `Inverse()` `FromAxisAngle(axis, radians)` `ToRotator()` | 기본값 단위 quat. 0 길이 axis는 단위 quat을 반환 |
| `Rotator` | 필드 `Pitch` `Yaw` `Roll`(도), 이름 지정 생성, `ToQuat()` `Forward()` `Right()` `Up()` | UE `FRotator` 규약 |
| `Transform` | 필드 `Location` `Rotation`(quat) `Scale`(vec3), `TransformPosition(v)` `InverseTransformPosition(v)` `TransformVector(v)` `ToMatrix()` | 기본값 단위. 0 scale의 역변환은 `NonFinite` |
| `mat4` | `*`(UE 행 벡터 규약), `TransformPosition(v)` | core에서는 원소 16개 직접 생성 불가. `Transform.ToMatrix()`로 얻는다 |
| `Color` | 필드 `R` `G` `B` `A`(선형 float), 이름 지정 생성 | 기본값 (0, 0, 0, 0) |
| `string` | `Length`(UTF-8 byte 수), `CharCount()`(code point 수), `Contains` `StartsWith` `EndsWith`, `Find(s): int?`(byte 위치), `Slice(start, end)`, `Split(sep): Array<string>`, `Trim()`, `ToUpperAscii()` `ToLowerAscii()` | `Slice`의 범위 밖 또는 code point 경계가 아닌 위치는 `IndexOutOfRange`. 화면 표시는 `Text`(§3.5) |
| `Format` | `Format.Fixed(x: float, decimals: int): string`, `Format.Padded(i: int, width: int): string` | 보간 `${}`의 최단 표기 외 서식. `decimals`는 0–9, 밖이면 `AssertFailed` |
| `Parse` | `Parse.Int(s): int?`, `Parse.Float(s): float?` | 10진·`FloatLit` 문법만, 실패는 `null` |
| `Array<T>` | `[i]` 읽기 `T?`, `[i] = v`, `Add` `Insert(i, v)` `RemoveAt(i)` `Clear()` `Reverse()`, `==` 가능한 원소: `Contains` `IndexOf: int?` `Remove(v): bool`, `<` 가능한 원소(int·float·string·enum): `Sort()`. 값 원소: `Count`. 객체 원소: `SlotCount`(파괴된 자리 포함) `AliveCount()` `Compact()` | 객체 원소 Array에는 `Count`가 없다(SD40). `Compact()`는 파괴된 자리를 제거한다. 순회는 시작 시점 내용의 사본이다 |
| `Map<K, V>` | `[k]` 읽기 `V?`, `[k] = v`, `Count` `ContainsKey` `Remove(k): bool` `Keys(): Array<K>` `Values(): Array<V>` `Clear()` | 삽입 순서 |
| `Set<T>` | `Count` `Add(v): bool` `Remove(v): bool` `Contains` `Clear()` | 삽입 순서 |
| `Result<T>` | `Value: T?`, `Error: Error?` | §8.2 |
| `Error`(엔진 닫힌 enum) | `NotFound` `InvalidArgument` `LoadFailed` `SpawnFailed` `NotAuthority` `AlreadyTraveling` `ModuleUnloading` `Unsupported` `Busy` | case 추가는 언어 버전과 무관한 엔진 변경이므로 `Error`의 switch는 `default` 필수(§3.3) |
| `Log` | `Log.Info` `Log.Warning` `Log.Error` `Log.Debug`(message) | message는 문자열 리터럴(보간 허용)만. §10.4 |
| `Assert` | `Assert(cond: bool, message)` | 조건식은 Pure 문맥, message는 리터럴. 거짓이면 `AssertFailed`. 모든 빌드에서 검사 |
| `Random` | `Random.Int(min, maxExclusive)` `Random.Float(min, max)` `Random.Chance(p)` | §6.3 결정적 stream. `min >= maxExclusive`는 `AssertFailed` |
| `Time`(문맥 이름) | `Delta` `FixedDelta` `UiDelta` `GameSeconds` `Frame` | `Delta`는 SD21, `GameSeconds`는 World 시작 후 GameClock 누적, `Frame`은 프레임 번호 |
| 대기 | `Delay(seconds, clock)` `NextTick()` `NextFixedTick()` `All(...)` `Race(...)` | §6.1 |
| 에셋 | `X.Load()` `Assets.LoadAll(list)` `Assets.Find<T>(s)` | §7.3 |
| `Tag`·`TagSet` | `Tag.Find(s)`, `TagSet`: `Has` `HasAny(set)` `HasAll(set)` `Add` `Remove` | |
| 설정 | `CVars.Find<T>(s)`, `CVarRef<T>.Value`, `Settings.Get<T>()` | §10.4 |

## 4. 타입·참조·값

### 4.1 값과 참조

스크립트가 직접 다루는 데이터는 값이다. 참조는 엔진이 수명을 관리하는 객체(Actor·Component·Service·WidgetController)와 에셋뿐이며, 스크립트는 GC로만 회수되는 객체를 생성하지 않는다.

그림 1. 값 영역과 엔진 객체

- 값 영역(VM 소유·참조계수·COW): handler 실행 frame이 Scalar(`s0 int`, `s1 float`)·Ref(`r0 this`, `r1 Door`)·Buf(`b0 Array<Enemy>`) bank를 가진다. Ref는 handle+generation이며 GC root를 만들지 않는다.
- 참조계수 소유: Buf `b0`와 엔진 객체 Door의 `Targets: Array<Enemy>` 필드가 같은 COW 버퍼(rc=2)를 공유한다. 값은 참조계수 버퍼이며 closure가 없으므로 순환이 형성되지 않고 참조계수 0에서 회수된다.
- 약한 참조: Ref `r1`과 버퍼 원소 `[0] Enemy A`·`[1] Enemy B`는 엔진 객체를 약한 handle로 가리킨다. 엔진 객체 참조는 모두 약한 handle이며, 엔진 객체의 수명은 World·Actor의 소유 관계가 정한다.
- 에셋: 에셋 참조는 lease를 공유하는 값이며 보관하는 동안 상주를 유지한다(§7).

본문 규칙은 다음과 같다.

- `string`·`Array`·`Map`·`Set`은 참조계수 버퍼이며 공유 중인 버퍼를 변경할 때 먼저 복사한다.[R13] closure가 없으므로 값 사이 순환이 없고 참조계수만으로 회수된다. VM은 스크립트 상태에 대해 GC root를 제공하지 않는다(전체 계획 §23.2 개정).
- 소유 component 필드(`let PickupRange = SphereCollider(Radius: 200.0)`)도 handle이다. 엔진 쪽 표현은 Prefab 3.2절의 소유 subobject 필드(`ObjectRef<…>`, SubobjectGuid)이며 수명은 소유 Actor가 관리한다. 스크립트 필드는 두 번째 수명 관리자가 아니고 VM에는 약한 handle로 보인다.
- 참조계수는 Game lane 전용이다. 다른 lane으로 값을 전달할 때는 버퍼를 고정(freeze)하고 참조계수를 1 증가시켜 넘기며, 해제는 Game lane으로 되돌려 처리한다(§9.4).

### 4.2 기본 타입

| 분류 | 타입 | 규칙 |
|---|---|---|
| 논리·숫자 | `bool`, `int`(64비트), `float`(64비트) | overflow·NaN·Inf는 fault. 엔진의 32비트 값과 주고받을 때(native 필드 쓰기·인자·반환) 범위를 검사하며 정수 범위 밖은 `IntegerOverflow`, float32 범위 밖은 `NonFinite`이고 상수는 컴파일 오류다. `float`를 float32로 바꿀 때는 최근접 짝수로 반올림한다 |
| 수학 | `vec2`–`vec4`, `quat`, `mat4`, `Rotator`, `WorldPosition`, `Transform`, `Color` | Core 수학 타입의 바인딩(연산과 메서드는 부록 B). 스크립트 안의 성분은 `float`(64비트)이고 엔진에 전달할 때 위 규칙으로 변환한다. 단위·축은 엔진 규약(UE 5.8과 같은 왼손 좌표계, +X 전방·+Y 오른쪽·+Z 위, yaw 양수는 오른쪽 회전, 길이 cm·질량 kg·시간 초, `Rotator` 도, 삼각함수·quaternion 축-각·각속도 라디안)을 따른다(전체 계획 §2.2, Math 구현 문서의 Rotator 규약). 각도 단위가 둘이므로 각도를 받는 native 함수·필드는 UE처럼 이름이나 타입으로 단위를 드러내고(`Rotator` 필드는 도, `…InRadians`·`…InDegrees`), 언어 서버가 hover·자동완성에 단위를 표시한다(SD18) |
| 문자 | `string`, `Text` | 화면 표시 API는 `Text`만 받는다. `Text`는 `Loc(...)`로 생성 |
| 엔진 데이터 | `Tag`, `TagSet`, `InputAction`, `Soft<T>`, `Row<T>` | §3.5 리터럴. `Soft<T>`·`Row<T>`에는 빈 값이 없으며 선택 값은 `Soft<T>?`다(SD19) |
| 컨테이너 | `Array<T>`, `Map<K,V>`, `Set<T>` | 삽입 순서로 순회. 번호·키 읽기는 `T?`. 범위 밖 쓰기는 fault. Map의 `m[k] = v`는 삽입 또는 교체이고, 복합 대입과 경로 쓰기는 키가 없으면 fault. 무공변(`Array<Derived>`는 `Array<Base>`가 아니다). 타입 인자에는 `?`를 쓸 수 없다. C++ 쪽은 Core `OrderedMap`·`OrderedSet`(삽입 순서 보존)이고 키는 `==` 가능한 단일 값(정수·bool·enum·문자열·Name·Guid·Soft·클래스 참조 — 실수·객체 키는 저장할 수 없다)이다(RX 7절). 따라서 객체 키 Map·객체 원소 Set은 VM 전용(비영속·비복제)이며 `@edit`·`@save`·`@net` 필드에 두면 컴파일 오류다 |
| native 값 struct | binding된 C++ 값 struct(커브 `TangentCurve`·`PlainCurve`·계단 커브, `Hit` 등) | 값 의미(COW), `==` 불가, 저장 가능 여부는 구성 필드로 판정. 메서드는 binding이 노출한다(예: `Falloff.Eval(t): float` — CurveData 14절, float32 결과는 float64로 정확히 올라가고 인자는 위 변환 규칙) |
| 합성 | `T?`, `Result<T>` | 조회 실패는 `T?`, 작업 실패는 `Result`. `r.Value: T?`, `r.Error: Error?`이며 `Result<void>`에는 `Value`가 없다. `?`는 중첩되지 않는다 |
| 클래스 참조 (full) | `Class<T>` | TypeId와 기반 타입 제한. `Create(...)`. C++ 쪽은 `ClassRef<Base>`(리플렉션 B9 — ReflectionExtensions 5.2절) |

### 4.3 객체 참조와 "없음"

| 위치 | 타입 | 유효 범위 |
|---|---|---|
| `this` | `T` | 실행 중 항상 유효. 재개마다 generation을 확인하고 사라졌으면 실행을 취소 |
| `Owner`(component의 속성) | `Actor` | 실행 중 항상 유효. Destroy hook에서는 읽기 전용. 엔진 `Component::GetOwner()`의 binding |
| `Widget`(WidgetController의 속성) | widget 타입 | 실행 중 항상 유효 |
| 소유 component 필드 | `T` | 소유 Actor와 같은 수명 |
| `?` 없는 `@edit` 객체 필드 | `T` | 같은 prefab 안의 참조만 허용. non-null 검사는 Sparks 쿡·spawn 검증이 하며 리플렉션 Required(약한 참조에는 RMC2202로 금지)를 쓰지 않는다. spawn 시 해석에 실패하면 spawn 실패. 같은 파괴 묶음 밖에서 대상이 제거되면 참조하는 component를 격리(§8.3) |
| 그 밖의 객체 필드 | `T?` | 파괴되면 null. 다른 셀의 Actor는 셀이 상주할 때 자동 해석(엔진 `WeakObjectRef` 필드는 같은 저장 단위 안만 보존하므로 셀 사이 참조는 World의 영속 entity 참조가 맡는다 — RX 5.1절, Prefab D12. 영속 참조가 들어오기 전에는 셀 사이 참조 저장을 거부한다) |
| 컨테이너 원소 | 순회 변수 `T`, 번호·키 읽기 `T?` | `Array`는 파괴된 원소의 자리를 유지하고 null로 읽으며 순회는 살아 있는 원소만 제공한다. 객체 원소 Array에는 `Count`가 없고 `SlotCount`·`AliveCount()`로 뜻을 드러낸다(SD40). `Set` 원소와 `Map` 키는 파괴가 적용된 뒤 처음 접근할 때 제거한다(SD14) |
| 지역 이름(인자·지역 변수·`if (let)` binding·반복 변수) | `T` 또는 `T?` | `await`를 지난 뒤 읽으면 `T?`(아래) |

- **await 이후:** 어느 경로로든 `await`를 지난 뒤 읽는 지역 이름의 객체 타입 `T`는 그 읽기 지점에서 `T?`다. 반복문의 back-edge와 분기 합류도 포함하며, 그 변수에 다시 대입하면 `T`로 돌아간다. 이 규칙은 엔진 객체(Actor·Component·Service·WidgetController) 타입의 지역 이름에만 적용하며 에셋 값·유지 자원·로드 요청 handle을 담은 지역 이름은 해당하지 않는다. `this`, 문맥 이름, `Owner`·`Widget`, 소유 component 필드, `?` 없는 `@edit` 필드는 지역 이름이 아니므로 바뀌지 않는다.
- **null 검사는 하나다.** `if (let ...)`, `?.`, `??`, `== null`은 모두 "null이거나 파괴됨"을 같은 판정으로 처리한다. 타입을 좁히는 형태는 `if (let)`과 `?? Exit`뿐이며 `== null` 비교는 타입을 바꾸지 않는다. `?.`·`??`·`if (let)`의 대상이 `?` 타입이 아니면 오류다. `if (let x = e)`의 `e`가 `bool?`이면 오류이며, 참인지 볼 때는 `e ?? false`, 있는지 볼 때는 `e != null`을 쓴다.
- **`?.`와 `??`:** 수신자가 null이면 `?.` 바로 뒤의 이름과 그 호출 인자를 평가하지 않으며 결과는 `R?`(호출 결과가 `void`면 `void`)다. 단락은 `?.` 하나에만 적용되고 이어지는 `.`는 `R?` 수신자 규칙을 따르므로 `a?.B?.C`로 쓴다. `??`의 좌변은 `T?`, 우변은 `T`(결과 `T`)·`T?`(결과 `T?`)·`Exit`이며 기반 타입으로 합치지 않는다.
- **파괴는 스크립트 단계 끝에 World에 적용된다(두 시점, SD25).** 스크립트 단계가 열려 있는 동안 Game lane에서 요청된 파괴·활성 전환(스크립트, 스크립트가 호출한 native API, 전달 순회(§5.4)가 호출한 C++ handler)은 스크립트 bridge의 deferred 큐에 들어가고, 그 단계의 이벤트 전달이 끝난 뒤 요청 순서대로 World API(`World::DestroyActor`·`DestroyComponent`·`SetEnabled`)를 호출한다. 이것이 **적용 시점**이다. 적용된 객체는 엔진의 PendingDestroy 상태가 되어 Tick에서 빠지고, 실제 Disable·Destroy hook과 Detach는 World의 **프레임 끝 정리**에서 실행된다(ER 7.4). 같은 단계 안에서 확인한 객체는 단계 끝까지 유효하며 순회·`if (let)`에도 그대로 남는다(SD3). 적용 시점 이후 스크립트는 그 객체를 null로 읽는다. 객체 원소 Array의 `SlotCount`는 파괴된 자리를 포함하고 `AliveCount()`는 살아 있는 원소만 센다(SD40). 생성(`World.Spawn`)은 즉시 수행된다(§5.1).
- **파괴 묶음:** 한 번의 적용에서 같은 Actor graph(Actor와 그 component·부착 자식)의 파괴는 한 묶음으로 처리한다. 프레임 끝 정리는 묶음 단위로 Disable 전부(graph 역순) → Destroy 전부(graph 역순) → Detach 전부 순서이며 엔진 정리 순서(Component OnDisable → Actor OnDisable → Component OnDestroy → Actor OnDestroy → OnUnregister)와 같다. 같은 묶음 안의 `?` 없는 `@edit` 참조 대상 제거는 격리 사유가 아니다.
- **컨테이너 정리는 값 변경이 아니다.** 파괴로 인한 null 읽기와 Set·Map 항목 제거는 Changed·복제·저장을 일으키지 않으며 `let`에도 적용된다. Array의 죽은 자리는 대입·제거로만 없어지고, 증가는 장시간 soak Gate(§9.3 G2)가 검출한다.
- downcast는 `as?`만 제공한다. class 타입의 하향 변환과 interface 조회(full)에만 쓰고 입력이 null이면 결과도 null이다. 강제 unwrap은 없다.[R24]

### 4.4 대입 규칙

대입:

~~~text
Hinge.RelativeLocation.Z += 0.1;         // 쓰기 가능한 native 속성: 읽기 → 수정 → 쓰기
Points[i].X = 0.0;                        // 배열 원소에 기록
GetTransform().Location.Z = 1.0;          // 오류: 함수 결과는 대입 대상이 아니다
var p = Hinge.RelativeLocation;
p.Z = 5.0;                                // 오류: 바꾼 사본을 어느 경로에서도 읽지 않는다
for (h in Hits) { h.Damage = 0; }         // 오류: 반복 변수는 바꿀 수 없다 → Hits[i].Damage
other.Current = 0;                        // 오류(other: Health): 다른 객체의 Sparks 필드는 읽기만 한다
~~~

- 대입은 지역 변수, 자기 instance의 필드, 원소 경로, 쓰기 가능한 native 속성 경로에만 할 수 있다. 경로 중간의 값 타입은 원본에 기록된다. 반복 변수와 함수 인자는 바꿀 수 없다.
- 사본 수정 오류는 미사용 규칙(§3.3)의 한 경우다. 바꾼 `p`를 이후에 읽거나 `Hinge.RelativeLocation = p;`로 되돌려 쓰면 오류가 아니다.
- `let`의 규칙은 §3.2에 있다. C++ `ReadOnly` 필드는 엔진 쪽에서 바뀔 수 있는 읽기 전용 속성으로 표시한다.

### 4.5 생성과 기본값 순서

- struct와 component는 필드 이름으로 생성한다(`HitInfo(Point: p)`). 지정하지 않은 필드는 기본값을 사용하며 기본값이 없는 필드(§3.2)는 반드시 지정한다.
- component는 `let` 필드의 초기값으로만 생성한다(실행 중 추가는 full). 필드 초기값은 상수식과 component 생성만 허용하고 클래스 기본값 레코드에 반영한다 — 레코드는 가장 가까운 C++ base의 생성자 결과 위에 컴파일된 초기값을 덮어 합성한다(RCM '클래스 기본값 결정'). component 생성 초기값은 클래스 원형의 default subobject(Prefab PF1)가 선행 조건이며 그때까지 미지원이다. 상수식은 리터럴, 검증 리터럴, enum 값, const 이름, 이들의 산술·비교, 수학 타입·struct 생성, 배열·Map 리터럴, `Math.Radians`·`Math.Degrees`이며 그 밖의 함수 호출과 필드 참조는 포함하지 않는다. 컴파일 시 overflow는 오류다. C++ 생성자 안에서 스크립트 코드를 실행하지 않는다.
- 기본값 순서는 전체 계획 §5.3과 Prefab 3.1절을 따른다: native → Sparks 필드 초기값(이 둘의 결과가 클래스 기본값 레코드다) → subobject 기본값 레코드 → prefab·variant → instance override → spawn parameter. 코드의 기본값 변경은 override하지 않은 instance에 반영하고 override한 instance를 편집기에 표시한다.
- 소유 component 필드에도 안정 ID(SubobjectGuid)를 부여하므로 이름을 바꿔도 prefab override가 유지된다(전체 계획 §5.3).

## 5. 수명 hook과 이벤트

### 5.1 수명 hook

Unity의 hook 체계(종류·의미)를 따르고 이름은 Rocks 어휘를 사용한다. 프레임 안 순서와 Start 시점은 엔진 World(ER 7–8절, `World.h`의 tick phase)를 따르며 Unity와 다른 점은 §2.4에 적었다(SD24·SD26). Unity의 `On` 접두는 `on` 키워드가 대신한다. 모든 엔진 수명 타입(component·Actor 파생 클래스·service·widget controller)에 같은 hook을 적용한다.

| Sparks | Unity 대응 | 전체 계획 수명 단계 | C++ 가상 함수(SD9) | 규칙 |
|---|---|---|---|---|
| `on Awake()` | Awake | Initialize | `OnAwake` | spawn 묶음(`World.Spawn` 또는 prefab instantiate 1회가 생성한 객체 graph)의 Awake가 모두 성공하면 graph 순서(Actor 전위 순회, component는 subobject 표 순서)로 Enable한다. await·Actor 생성·자원 획득 불가. fault와 `try` 실패는 spawn 실패(SD43). prefab instancing이 아닌 활성화(레벨 로드 배치 객체)에서는 엔진 규칙대로 그 component만 제거하고 보고한다 |
| `on Enable()` | OnEnable | Activate | `OnEnable` | 이벤트 연결 활성화. 유지 자원 획득 가능. 처음과 다시 활성화될 때 모두 실행되므로 현재 상태를 반영하는 자리다. spawn에서는 `World.Spawn`이 반환하기 전에 실행하며, spawn 중의 fault·`try` 실패는 Awake와 같이 spawn 실패다(SD43). spawn 뒤의 재활성화에서 난 fault는 격리 |
| `on Start()` | Start | BeginPlay | `OnStart` | 엔진 World의 프레임 시작 처리에서 시작한다(ER 7.2: 처음 활성화된 다음 프레임 시작, BeginPlay 전 spawn이면 BeginPlay 안, C++ `IsReadyToStart()`가 참일 때). 엔진이 `OnStart`를 부르면 스크립트 bridge가 `on Start` 실행을 시작한다. 준비 대기만 기다릴 수 있고(SD12) 그동안 bridge가 그 instance의 Tick을 보류(`SetTickEnabled(false)`)하며, 완료한 뒤의 첫 해당 tick phase부터 Tick이 돈다. 그동안에도 이벤트·Changed는 전달된다. 완료 전에 비활성화되면 취소되고 다시 활성화될 때 처음부터 실행되며 이전 실행이 바꾼 필드는 되돌리지 않는다(SD24) |
| `on Tick(dt)` · `on FixedTick(dt)` · `on LateTick(dt)` | Update · FixedUpdate · LateUpdate | 가변·고정·LateUpdate tick group | `OnTick` 등 | Tick 문맥: await·생성·자원 획득·로드 불가(§6.2). Start가 완료된 뒤의 첫 해당 tick phase부터 실행. 기본 phase는 Tick = PrePhysics, LateTick = LateUpdate이며 full의 `@tick(Phase:, Interval:, Priority:)`가 엔진 `TickSettings`(phase·`intervalSeconds`·`priority`)에 대응한다. 일시 정지 중에는 실행하지 않는다(엔진 `tickWhenPaused`는 노출하지 않음) |
| `on Disable()` | OnDisable | Deactivate | `OnDisable` | 활성에서 비활성으로 바뀔 때만 1회. hook 실행 후 엔진이 실행 중인 실행을 취소하고 유지 자원을 해제하며(필드는 `null`) 이벤트 연결을 비활성으로 둔다(항목 유지, SD10) |
| `on Destroy()` | OnDestroy | EndPlay | `OnDestroy` | Teardown 문맥(§6.2). hook이 반환한 직후 Detach |
| `on TriggerEnter(other, trigger)` · `TriggerExit` · `on CollisionEnter(other, hit)` · `CollisionExit` | `OnTrigger*` · `OnCollision*` | PostPhysics 물리 이벤트 batch | `OnTriggerEnter` 등 | 자기 Actor의 모든 collider에서 오며 어느 collider인지는 `trigger` 인자로 구분한다. 재진입 판정의 발행자는 `other`다. 값으로 복사한 이벤트를 generation 재검증 후 전달(전체 계획 §16.2) |
| `on Validate()` (full) | OnValidate | EditorConstruct | `OnValidate` | 편집기 전용, 저장되지 않는 미리보기 상태만 변경 |

- 수명·물리 hook은 누적된다. 기반 클래스의 hook이 먼저 실행되고 파생 클래스의 것이 이어서 실행된다. `on 경로.이벤트` handler는 누적되지 않으며 클래스 계층 전체에서 같은 경로를 한 번만 선언한다. 파생 클래스가 동작을 바꾸려면 기반의 handler가 호출하는 `fn`을 `override`한다.
- 이름 하나짜리 `on X()`는 hook 전용이다.
- `fn Update()`·`fn Start()`·`fn OnStart()`처럼 hook·`On`+hook·Unity hook 이름의 함수는 오류이며 해당 `on` 형태를 제시한다.
- C++ 가상 함수에 `On` 접두를 쓰면 오디오·타임라인의 일반 `Start()` 메서드와 충돌하지 않는다(SD9). C++ 수명 가상 함수(`On*`·`OnRegister`·`OnUnregister`)는 binding에 노출하지 않는다.
- **활성 전환:** 스크립트 단계 안의 활성·비활성 전환 요청은 파괴와 같은 deferred 큐로 처리하여 이벤트 전달이 끝난 뒤, 파괴 적용 전에 요청 순서대로 World `SetEnabled`를 호출한다(SD25). 엔진의 `SetEnabled`는 hook을 동기 실행하므로 Enable·Disable hook은 이 적용 안에서 실행되고, 그때 등록된 emit은 같은 큐로 전달된다. 비활성 instance의 유지 자원 획득 결과는 `null`이다.
- **엔진이 연 진입:** 스크립트 단계 밖에서 엔진이 hook을 부르는 지점(레벨 로드·셀 스트리밍·복제 spawn의 Awake·Enable, 프레임 시작 처리의 Start, 프레임 끝 정리의 Disable·Destroy, 물리 이벤트 batch)은 VM에 들어갈 때 스크립트 단계 하나를 열고, hook 실행 → 발행 큐 전달 → 적용을 마친 뒤 닫는다. 따라서 SD3의 지연 규칙은 모든 스크립트 실행에 성립한다. 적용 루프(활성 전환·파괴가 다시 낳은 전환·파괴)는 한 단계에서 64회까지 돌고, 넘으면 남은 요청을 다음 단계로 넘기고 보고한다.
- 이 절과 §2.4·§5.2의 표는 handler 인자 타입을 생략하였다. 선언할 때는 `on Tick(dt: float)`, `on TriggerEnter(other: Actor, trigger: Collider)`처럼 타입을 명시한다.

종료 경로별 hook(SD20):

| 종료 경로 | Disable | Destroy | Detach |
|---|---|---|---|
| `RequestDestroy`, 부모 파괴, 셀 unload | 활성이면 실행 | 실행 | Destroy 반환 직후 |
| travel | 활성이면 실행 | 실행 | Destroy 반환 직후. 이전 World만 종료하고 Session은 유지 |
| PIE Stop, process 종료 | 활성이면 실행 | 실행 | Destroy 반환 직후. 그 뒤 CloseSession(§9.1) |
| 격리(§8.3: fault, 필수 `@edit` 참조 대상 제거) | 실행하지 않음 | 그 instance가 파괴될 때 실행 | 격리 시점에 실행·유지 자원·연결 해제(값 필드 보존), Destroy 반환 직후 Detach |
| GC가 도달 불가로 판정 | 실행하지 않음 | 실행하지 않음 | 회수 전 cleanup hook에서 정확히 1회(현재 `Object`에 이 hook이 없으므로 ObjectFoundations·GC 문서의 선행 조건 — §17.2). 스크립트 instance는 World 장부에 편입된 객체에만 붙으므로 이 경로는 안전망이다 |

정상 종료 경로의 hook은 Teardown 예산(§9.3) 안에서 실행하며 예산을 넘거나 fault가 나면 그 hook만 끊고 종료를 계속한다. 파괴 경로의 Disable과 Destroy는 safepoint를 열지 않는다(전체 계획 §5.2).

### 5.2 이벤트: on 경로.이벤트

이벤트(클래스 일부):

~~~text
class Health : ScriptComponent {
    @edit @save var MaxHealth: int = 100;
    @net var Current: int = 100;
    event Died(killer: Actor?);

    fn ApplyDamage(amount: int, instigator: Actor?) {
        if (Current == 0) { return; }
        Current = Math.Max(0, Current - amount);
        if (Current == 0) { emit Died(instigator); }
    }
}

class HealthBar : WidgetController {
    @edit var Target: Health?;
    @edit var Guards: Array<Guard>;

    on Enable() { Refresh(); }
    on Target.Current.Changed() { Refresh(); }                   // Target이 바뀌어도 1회 전달
    on Target.Died() { Hide(); }                                  // 인자는 필요한 만큼만
    on Guards.Died(guard: Guard) { Mark(guard); }                // 배열: 첫 인자는 발행한 원소
    on Widget.RetryButton.Clicked() { Session.RequestRespawn(); }
}
~~~

| 경로 | 예 | 의미 | 재진입 판정의 발행자 |
|---|---|---|---|
| 자기 필드 | `on State.Changed()` | 필드 값이 바뀐 뒤 | 해당 없음(await 불가) |
| 객체 필드 | `on Target.Died()` | 필드가 가리키는 객체의 이벤트. 필드 값이 바뀌면 다시 연결 | 가리키는 객체 |
| 배열 필드 | `on Guards.Died(guard)` | 배열의 서로 다른 객체마다 한 번 연결. 첫 인자는 발행한 원소이고 이벤트 인자가 뒤따른다 | 원소 객체 |
| `Owner` | `on Owner.Interacted(actor)` | 소유 Actor의 이벤트와 Actor 확장 이벤트 | 소유 Actor |
| 자기 Actor(Actor 파생 클래스) | `on this.Interacted(actor)` | 자기 Actor의 이벤트와 Actor 확장 이벤트 | 자기 Actor |
| 입력·UI | `on JumpAction.Triggered()`, `on Widget.Ok.Clicked()` | action 필드와 widget 경로. 사전·WidgetAsset schema와 컴파일 시 대조 | action instance, widget |
| `World` | `on World.GameStarted()` | World 이벤트(session의 World 이벤트 proxy를 통해 연결) | World |
| 물리 hook | `on TriggerEnter(other, trigger)` | 자기 Actor collider의 통지(§5.1) | 상대 `other` |

- handler 인자는 이벤트 인자 목록의 앞에서부터 필요한 만큼 타입과 함께 선언하며 타입은 정확히 일치해야 한다. 쓰지 않는 앞쪽 인자는 `_`로 쓴다(`on CollisionEnter(_: Actor, hit: Hit)`).
- 멤버 이벤트는 선언한 클래스 안에서만 `emit`한다. Actor 확장 이벤트(`event Actor.X`)와 World 이벤트(`event World.X`)는 최상위에 선언하며, 대상 타입의 이벤트(native 포함)와 패키지·의존 패키지 전체에서 이름이 유일해야 한다. `on`·`emit`으로 쓰려면 선언 모듈을 직접 import해야 하고(전이되지 않음) 그 사용은 import 사용으로 센다. 확장 이벤트는 (선언 모듈, 이름)의 안정 ID로 식별한다. 발행 형태는 `emit target.Interacted(player)`이며 Actor 파생 클래스는 `emit this.Interacted(player)`로 자기 Actor에 발행한다.
- 식 안에서 이벤트는 괄호 없이 `X.Event`로 쓴다(`await Skip.Triggered`). `emit` 밖의 `X.Event(...)`는 오류다.
- `Changed`는 예약 이벤트 이름이므로 `X.Changed`는 항상 필드 X의 변경이다.
- widget 경로는 평면 `Widget.이름` 한 형태다. 스크립트에 노출하는 자식 widget은 WidgetAsset 안에서 유일한 PascalCase ASCII 이름을 가지며 widget 자신의 멤버 이름과 같으면 저장·쿡 오류다.
- 해석할 수 없는 경로·이벤트, 시그니처 불일치는 오류다(유사 이름 제시).

### 5.3 Changed

- Sparks 필드와 변경 통지(CU5)를 지원하는 native 필드에만 있다. 그 밖의 필드에 연결하면 오류다.
- **값 기준:** 실행 조각(§5.4) 안에서 필드를 처음 쓸 때 발행 큐에 Changed 자리를 확보한다. 조각이 끝날 때 조각 시작 값과 끝 값이 다르면 그 자리에서 한 번 전달하고 같으면 자리를 취소한다. 중간 값은 전달되지 않고 이전 값은 제공하지 않는다. 비교는 스칼라·enum이 비트 동일(`0.0`과 `-0.0`은 다른 값), struct·컨테이너가 원소별, 객체가 동일성이다. 경로 쓰기(`Info.Point.X = 1.0`)와 변경 메서드(`Targets.Add(e)`)는 그 필드의 쓰기다. 스크립트 밖의 쓰기(복제 수신·native)는 엔진이 적용하는 batch 하나를 한 조각으로 본다.
- **활성화 전 적용과 비활성 동안의 변경은 발행하지 않는다.** 초기값, spawn 때의 저장 로드, 초기 복제·late join 상태, Awake와 Enable hook 안의 쓰기는 Changed를 발행하지 않는다. 비활성 instance는 연결이 꺼져 있으므로 그동안의 변경도 받지 않는다. 그래서 현재 상태는 활성화될 때마다 실행되는 `on Enable`에서 반영한다. Enable이 반환된 뒤의 쓰기는 Disable·Destroy 문맥을 제외하고 누가 썼는지와 무관하게 같은 규칙으로 발행한다. 실행 중인 instance에 저장을 적용하는 경로는 instance를 다시 생성한다(전체 계획 §23.2).
- **경로 원천:** `on A.B.Changed`는 A가 가리키는 대상이 바뀌면(파괴 반영으로 null이 된 경우 포함) 다시 연결한 뒤 한 번 전달한다.
- `Changed` handler에서는 `await`할 수 없다. handler가 같은 필드를 다시 쓰면 새 조각의 쓰기로 처리되어 값이 바뀐 경우에만 다시 전달되며 연쇄 깊이 상한(§5.4)이 적용된다. Disable·Destroy 문맥의 필드 쓰기는 Changed를 발행하지 않는다.
- 일회성 연출은 Changed가 중간 값을 건너뛸 수 있음을 전제로, 복제하지 않는 필드에 직전 표시 상태를 두고 비교하여 전이를 판정한다(§2.2).

### 5.4 전달 시점: 스크립트 단계

Sparks 코드는 엔진의 정해진 지점에서 실행되며 이 실행 묶음을 **스크립트 단계**라고 한다. **실행 조각**은 hook·handler·재개를 반환 또는 실제로 정지하는 `await`(§6.1)까지 실행하는 단위다. 한 프레임의 스크립트 단계는 엔진 World의 tick phase 순서(`World.h`, ER 6·8절)를 따른다.

| 순서 | 엔진 World 지점 | 스크립트 단계 |
|---|---|---|
| 1 | 시계 갱신 뒤, 프레임 시작 처리 앞 | **입력 batch**: 엔진 `PumpEvents`가 모은 입력 action phase 이벤트와 스크립트 단계 밖에서 생긴 이벤트(복제 수신·lane 간 전달·native 쓰기)를 FIFO로 전달 |
| 2 | 프레임 시작 처리(`OnStart`) | **Start**: 이번 프레임에 시작하는 `on Start` |
| 3 | PrePhysics | **Tick** |
| 4 | Physics × n(고정 step마다) | **고정 재개(NextFixedTick) → FixedTick → 고정 물리** |
| 5 | PostPhysics | **물리 이벤트 batch** → **재개**(NextTick·Delay·로드 완료·worker 완료) |
| 6 | LateUpdate | **LateTick** |
| 7 | PreRender 뒤 프레임 끝 정리 | 적용된 파괴의 Disable·Destroy·Detach(§4.3), reload 경계(§12.4) |

입력 batch·재개 단계를 실행할 World의 phase 경계 hook은 ER 8절에 계약으로 추가한다(§17.2). "가변 tick"은 프레임마다 1회, "고정 tick"은 고정 step마다 1회의 실행 시점이며, 재개 순서 key의 tick은 프레임 번호다.

실행 조각 → 발행 큐(FIFO · emit·Changed) → 다음 실행 조각(연쇄 깊이 상한 16) → 활성 전환 적용(World `SetEnabled`, Enable·Disable hook 실행, 이때 등록된 emit은 같은 큐로 전달) → 파괴 적용(World `DestroyActor`·`DestroyComponent` 호출, 실제 hook은 프레임 끝 정리) → 다음 스크립트 단계

- `emit`은 큐에 등록되고, 지금 실행 중인 조각이 반환하거나 `await`에 도달한 뒤 같은 스크립트 단계 안에서 발행 순서대로 전달된다. `World.Spawn`이 실행하는 Awake·Enable만 호출자의 실행 조각 안에서 중첩 실행되며, 그 밖에는 handler가 다른 handler 도중에 끼어드는 재진입이 없다.
- **전달 순회:** 큐 처리기는 항목 하나를 꺼내 그 시점에 연결되어 있고 활성인 handler(C++·Sparks)를 연결 순서대로 모두 실행한 뒤 다음 항목으로 넘어간다. 발행 시점의 연결 목록은 따로 보관하지 않는다. 스크립트 단계 안의 native `Broadcast`도 큐 항목으로 기록한다(binding schema에 선언). 연결 순서는 항목이 delegate에 추가된 순서이며 비활성화·재활성화로 바뀌지 않는다. 배열 원천은 번호 순으로 추가한다.
- **연쇄 상한:** 스크립트 단계를 시작한 실행 조각의 발행은 깊이 1이고 깊이 d인 handler의 발행은 깊이 d+1이다. Start 항목은 새 루트(깊이 1)다. 깊이 17이 될 항목을 등록하려는 조각은 `CascadeLimit` fault가 되고, 같은 루트에서 파생된 미전달 항목만 폐기한다.
- **전달 수와 큐 상한(SD33):** 스크립트 단계당 전달 **항목** 수 상한 N = 65,536이다(handler 호출 수가 아님). 넘는 항목은 다음 스크립트 단계로 이월한다. 연속 60 단계 동안 같은 루트 생성 위치에서 이월이 생기면 그 위치를 `CascadeLimit` fault로 처리하고 PIE를 일시 정지한다. session 발행 큐 상한은 262,144 항목이다. 스크립트의 `emit`·Changed가 큐를 넘기면 발행한 조각의 `BudgetExceeded` fault이고(조용히 버리지 않는다), 엔진발 항목은 같은 종류를 합치고 넘친 수를 집계한다(§9.4). 값은 프로젝트 설정이며 쿡 시 고정되고 실행 중 바꿀 수 없다.
- **이월 항목의 재검증:** 이월 항목과 스크립트 단계 밖에서 생긴 이벤트는 전달 직전에 인자 객체의 generation을 재검증한다. `?` 없는 객체 인자가 파괴되었으면 그 항목을 폐기하고 위치별로 보고하며(fault 아님), `T?` 인자는 null로 전달한다.
- 파괴는 전달이 끝난 뒤 적용하므로 같은 단계 안에서 이벤트 인자로 전달된 객체는 전달 시점에 유효하다. Disable·Destroy 문맥의 `emit`은 컴파일 오류이며, 적용 중 추가로 생긴 비활성화·파괴는 같은 적용 루프 끝에 요청 순서로 처리한다(64회 상한).
- **일시 정지:** 게임이 일시 정지된 동안에는 입력 batch와 재개 단계(UI 시계 대기만)만 실행하고 Start·Tick 계열·고정 단계는 실행하지 않는다. UI 연출은 `Delay(0.0, Clock.UI)` 반복으로 한다.

### 5.5 연결의 저장과 해제

- **보관 위치:** 발행자 쪽은 Core `MulticastDelegate`다. 이벤트 필드가 없는 확장 이벤트·native 필드 `Changed`·World 이벤트는 발행자 객체에 지연 할당하는 이벤트 slot 표에 저장하며 그 객체와 함께 해제된다.
- **항목:** **연결 표**는 session이 소유하는 연결 기록 배열이고 **연결 slot**은 그 원소 번호다. Sparks 연결 항목은 (연결 slot, generation) key와 handler ID만 가지며, 구독 instance의 Detach가 slot의 generation을 올려 key를 무효화한다. delegate thunk는 큐 항목만 기록하며 스크립트 코드는 전달 순회에서만 실행된다(§5.4). revision lease·raw pointer를 보관하지 않는다.
- **비활성화:** Disable은 항목을 삭제하지 않고 비활성으로 표시하며 Enable이 다시 활성화한다. 항목은 Detach, 격리, 원천 필드 변경(해당 발행자분), 선언이 추가·삭제·변경되는 reload, CloseSession에서 삭제된다.
- **구독자가 먼저 끝날 때:** 구독자 Detach가 자기 key를 폐기하면 항목은 즉시 호출 대상에서 제외되고, 발행자의 다음 compact에서 제거된다.
- **발행자가 먼저 끝날 때:** 발행자의 Detach 또는 회수 전 cleanup hook(§5.1, 선행 조건 §17.2)이 delegate를 비우고, 각 항목의 소멸이 연결 기록을 해제한다. GC 경로로 회수되는 경우에도 cleanup hook이 정확히 1회 실행되므로 누락이 없다.
- **필드·배열 원천:** 필드가 바뀌면(스크립트·로드·복제 모두) 실행 조각이 끝날 때 서로 다른 발행자 집합을 비교해 추가·제거한다. 같은 객체가 배열에 여러 번 있어도 연결은 하나이고 handler는 이벤트마다 1회 호출된다.
- **reload:** `on` 선언이 추가·삭제·변경되면 살아 있는 instance의 연결을 reload 경계에서 다시 구성한다.
- **session 범위:** 스크립트는 자기 session이 소유한 발행자에만 연결한다. 입력 action은 session의 InputConsumerScope 안의 action instance에, 엔진 전역 이벤트는 session World 이벤트 proxy에 연결한다. 공유 에셋은 이벤트를 노출하지 않는다.
- **Core 변경:** 생존 판정을 외부에서 제공하는 회수 가능 대상(C1 — 현재 약한 바인딩은 `SharedPtr`뿐), `TryAdd`의 상한 compact(죽은 항목이 직전 compact 후 살아 있던 수 + 8을 넘지 않음, C2 — 현재 compact 없음), 단일 pass compact(C3 — 현재 `Compact`는 항목을 지울 때마다 처음부터 다시 훑어 최악 O(n²)), `CompactDead`·`Stats`(C4). compact는 할당하지 않는다. C5(`NamePool::TryFind`)는 구현되어 있다.

## 6. 시간과 async

### 6.1 handler 안에서 기다리기

handler 안에서 기다리기(싱글플레이 예):

~~~text
class CutsceneTrigger : ScriptComponent {
    @edit var Cutscene: Soft<Sequence>;
    let Skip = Action("UI.Skip");

    @skipWhileBusy                                          // 재생 중 같은 Actor의 재진입은 버린다
    on TriggerEnter(other: Actor) {
        if (!other.Tags.Has(Tag("Character.Player"))) { return; }
        let ctx = Input.StartContext(Asset("/Game/Input/Cinematic")); // 이 블록이 끝나면 해제
        let seq = try await Cutscene.Load();
        await World.Sequencer.Prepare(seq);                 // texture mip·오디오를 미리 상주
        await Race(World.Sequencer.Play(seq), Skip.Triggered);    // 먼저 끝난 쪽만 남고 나머지는 취소
    }
}
~~~

- `on` handler는 바로 `await`할 수 있다(Awake·Enable·Disable·Tick 계열·Changed·Destroy·Validate 제외, Start는 준비 대기만). 다른 함수에서 기다리려면 `async fn`으로 선언하고 호출할 때 `await`해야 한다.
- **정지 지점:** 기본 대기(`Delay`·`NextTick`·`NextFixedTick`·이벤트·로드 요청·`All`·`Race`)의 `await`는 이미 완료된 경우(상주한 에셋의 로드 등)에도 **항상 정지**하고 정해진 재개 단계(§5.4)에서 재개한다. 그래서 `await` 뒤의 실행은 언제나 새 실행 조각이며 "await 이후 `T?`" 규칙(§4.3)과 emit 전달 시점이 입력과 무관하게 같다. async fn 호출의 `await` 자체는 정지 지점이 아니며 그 함수 안의 기본 대기가 정지한다.
- **재진입(SD11):** `await`가 있는 `on 경로.이벤트` handler와 물리 hook은 재진입 정책을 표시해야 한다. `@skipWhileBusy`는 같은 발행자(§5.2 표)의 이벤트로 시작된 실행이 `await`로 정지해 있을 때 새 이벤트를 버리고 디버거에 횟수를 표시한다. `@restart`는 실행 중인 것을 취소하고 다시 시작한다. 발행자가 다르면 따로 실행된다. 표시가 없으면 오류이며 두 형태를 수정 예로 제시한다. 기다리지 않는 handler와 수명 hook에 표시하면 오류다. 이벤트마다 반드시 처리해야 하는 계산(피해 적용 등)은 기다리지 않는 handler에 둔다.
- **구조적 동시성:** `All`·`Race`의 인자는 결과가 `void`인 기다릴 수 있는 식(`void` async 호출, `Delay`, `NextTick`, 이벤트)과 지역 변수에 담은 로드 요청 handle만 쓴다(SD41). 값을 돌려주는 async 호출과 `X.Load()`를 직접 인자로 쓰면 결과가 버려지므로 오류이며, 지역 변수에 담는 형태를 수정 예로 제시한다. 인자는 `await` 없이 쓰고 구문상 목록이므로 개수는 소스에 고정된다. 모든 인자를 왼쪽부터 시작한 뒤 판정한다. `Race`는 완료를 처리하는 시점에 즉시 결정하고 나머지를 취소하며, 같은 처리 시점에 여럿이 완료되면 인자 순서로 정한다. 둘 다 결과를 반환하지 않는다. 하위 작업의 fault는 부모 실행의 fault가 되어 형제를 취소한다.[R19]
- **취소:** 취소는 `await` 지점에서만 일어나고, 실행이 얻은 유지 자원은 엔진이 해제한다. instance 비활성화·파괴, World 종료, PIE Stop, 호환되지 않는 reload, `ReloadExecutions: Cancel`(§12.4)이 취소 원인이다. 실행 수 예산을 넘어 새 실행이 거부되면 fault가 아니라 건너뛰고 디버거에 횟수를 표시한다. `await`는 실행이 끝날 수 있는 지점이므로 `await` 앞뒤로 나누어 쓴 필드 상태는 취소되면 중간 값에 남는다. 되돌림이 필요하면 `on Disable`에서 권한 측이 정리한다.
- **시간(SD21):** `Delay(seconds, Clock.Game)`은 pause·배율을 반영하고 `Delay(seconds, Clock.UI)`는 반영하지 않는다(전체 계획 §8.3의 GameClock·UIClock — 현재 엔진은 `GameClock`만 있으며 UI 시계는 ER 후속). 기한은 등록한 프레임의 해당 시계 값 + `seconds`이고, 시계 값이 기한 이상이 된 첫 재개 단계에서 재개한다. `seconds`가 0 이하이면 다음 프레임의 재개 스크립트 단계에 재개하며, `Clock.Game`이면 일시 정지 중에는 재개하지 않는다. `NextTick()`은 다음 프레임의 재개 스크립트 단계까지, `NextFixedTick()`은 다음 고정 tick의 고정 재개까지 기다리며 둘 다 게임이 일시 정지된 동안 재개하지 않는다. 일시 정지 중 SingleStep(전체 계획 §8.3)은 고정 tick 1회를 실행하여 FixedTick과 `NextFixedTick` 대기를 1회 진행하고, 가변 debug delta를 지정하면 Tick과 `NextTick` 대기도 1회 진행한다. `Time.Delta`는 Tick 계열 hook에서는 그 tick의 dt, 그 밖에서는 직전 가변 tick의 GameClock delta다. 일시 정지 중 UI의 다음 프레임 대기는 `Delay(0.0, Clock.UI)`다.
- **이벤트 대기(SD13):** `await X.Event`는 대기를 등록한 뒤 처음 전달되는 `X.Event`에서 재개하며 값을 반환하지 않는다. 이벤트 인자가 필요하면 `on` handler를 쓴다. 발행자의 파괴가 반영되거나 발행자가 회수되면 대기는 완료되지 않은 채 끝난다. 단독 `await`면 그 실행이 끝나고(취소), `Race`에서는 그 인자를 완료되지 않은 것으로 보아 다른 인자를 기다리며, `All`에서는 전체가 끝난다. 같은 스크립트 단계에서 전달과 파괴가 함께 일어나면 전달이 먼저다.
- `while`은 async 문맥에서만 쓰며 반복 본문의 모든 경로에 `await` 또는 탈출이 있어야 한다.
- 재개는 Game lane의 재개 스크립트 단계에서 (대기를 등록한 tick, instance 안정 key, 등록 순번) 순서로 수행한다(전체 계획 §8.2·§8.4). 이벤트 대기는 전달 순회 안에서 다른 연결과 같이 연결 순서(대기 등록 시점)로 재개한다.

### 6.2 실행 문맥

함수와 native API에 실행 문맥을 표시하고 컴파일러가 호출 그래프 전체에 전이적으로 검사한다. 재정의 함수는 기반 함수의 문맥이 금지하는 동작을 사용할 수 없다. 표의 "Actor 생성"은 `World.Spawn`이며 struct·값의 생성은 제한하지 않는다.

| 문맥 | 진입 | 금지 |
|---|---|---|
| Game | 기본 | — |
| Init | `on Awake` | await, Actor 생성, 자원 획득, 로드 |
| Toggle | `on Enable`·`on Disable` | await. `on Disable`에서는 자원 획득과 `emit`도 금지(hook 직후 엔진이 해제하므로). 파괴 경로의 Disable은 Teardown처럼 safepoint를 열지 않음 |
| Start | `on Start` | 준비 대기(`Load`·`LoadAll`·`Sequencer.Prepare`·`DataLayerReady`) 외의 await |
| Tick | `on Tick`·`FixedTick`·`LateTick` | await, `World.Spawn`, 자원 획득, 로드. Tick에서 `emit`한 이벤트의 handler는 자기 문맥으로 실행되므로, 간접 생성 증가는 생성 위치별 ledger와 증가 추세 검사로 검출한다(§9.3) |
| Reaction | `on X.Changed` | await |
| Teardown | `on Destroy` | await, Actor 생성, 자원 획득, `emit`(엔진 C++ OnDestroy는 생성을 허용하지만 Sparks는 더 엄격하다). 접근은 `this`·자기 필드, 소유 component와 `Owner`의 읽기 전용 속성, binding이 일회성 효과로 선언한 World 서비스 호출로 한정. 할당은 허용하되 GC safepoint를 열지 않고 별도 실행 상한 적용(전체 계획 §5.2) |
| EditorConstruct (full) | `on Validate` | await, spawn, IO, RPC, `@edit`·`@save`·직렬화 필드 쓰기 |
| Pure (full) | `@pure fn` | 모든 쓰기. StateTree 조건·UI binding |
| Worker (full) | `@worker fn` | 엔진 객체 참조, 자원 handle. 값은 고정 후 전달 |
| Deterministic (full) | `@deterministic fn` | wall clock, IO, 결정적 stream 밖의 난수, `NextFixedTick` 외의 await(입력 상세안 §23.2) |
| Editor (full) | `@editorButton fn` | 편집기 Undo transaction 안에서만 실행. fault·`try` 실패는 transaction 전체 취소(§8.4) |

사용자 `fn`에는 문맥 표기가 없다. 컴파일러는 본문에서 효과 집합(await·생성·자원 획득·로드·emit·쓰기)을 추론하고 호출 그래프의 전이 폐포(재귀는 고정점)로 계산한다. 효과 집합은 모듈 interface hash에 포함되므로 본문 변경으로 효과가 바뀌면 호출하는 쪽이 다시 검사된다(§12.3).

### 6.3 결정성과 난수

- `Random`은 instance별 결정적 stream이다. seed는 (World seed, instance 안정 key, 클래스 TypeId)에서 만들고 World seed는 AppInfo의 고정 seed(`--deterministic` 실행)나 World 생성 시 값이다. 호출 순서가 같으면 플랫폼·backend(VM·AOT)와 무관하게 같은 값이 나온다.
- `Map`·`Set`은 삽입 순서로 순회하므로 순회 결과가 결정적이다. 초월 함수는 두 backend가 같은 Core Math 함수를 부르고(부록 B), 부동소수점은 FMA 축약·재결합 없이 계산한다(§11, §12.5). 따라서 backend 사이 결과는 같고, 플랫폼 사이 결과는 Core Math의 결정성 수준을 따른다.
- `@deterministic fn`(full)은 rollback 재시뮬레이션에서 다시 실행될 수 있으므로 emit·Changed·일회성 효과를 side-effect ledger로 기록하고 재실행 중에는 억제한다(입력 상세안 §23.2 VM bridge). 이 문맥의 결정성 수준은 CurveData D17과 같은 "UE 수준"(같은 빌드·같은 플랫폼 계열에서 같은 결과)이다.

## 7. 유지 자원과 비동기 로딩

### 7.1 유지 자원

멈출 때까지 계속되는 것(반복 사운드·지속 VFX·입력 context·로딩 요청·표시한 widget·카메라 점유)은 **유지 자원**이다. 사용자가 알아야 할 규칙은 3개다. 로딩 요청 handle(`LoadRequest<T>`)도 유지 자원이며 규칙 2·3을 따르지만, 생성이 실패하지 않으므로 `T?`가 아니고 `Start*` 이름 규칙에서 제외한다(§7.3).

1. **담아야 만들 수 있다.** 유지 자원은 지역 변수(`var`·`let`)의 선언 초기값(`?? return` 포함) 또는 필드·`var` 지역 변수 대입의 우변에서만 생성할 수 있다. 필드 초기값에서는 만들 수 없으므로 유지 자원 필드는 `var` `T?`뿐이다. 로딩 요청은 지역 변수에만 담는다(§7.3). `await`의 피연산자나 `All`·`Race`의 인자로 바로 쓰면 대기가 끝날 때까지 그 실행이 보관한다. 결과를 버리면(`_ =` 포함) "멈출 방법이 없다"는 오류다. 일회성 효과(`World.Audio.PlayAt`)는 handle 없이 호출한다.
2. **변수가 끝나면 멈춘다.** 필드나 `var` 지역 변수에 새 값·`null`을 넣을 때, 지역 변수의 블록을 떠날 때(`return`·`break`·`?? Exit`·fault·취소 포함, 반복문은 회차마다), 객체가 비활성화·종료될 때 멈춘다. 같은 블록의 지역 자원은 선언 역순으로 멈춘다.
3. **옮길 수 없다.** 복사, 다른 이름으로 옮기기(`if (let)`, 담긴 변수를 좌변으로 한 `??`, 대입), 배열·Map·struct·이벤트 인자·함수 인자(native 포함, `All`·`Race`의 인자로 기다리는 경우 제외)·반환값·다른 객체·`@save`/`@edit`/`@net` 필드 보관은 오류다. 쓸 때는 담긴 변수의 멤버를 호출한다(`Hum?.Pause()`).

유지 자원(클래스 본문 일부):

~~~text
var Hum: Sound?;
on Enable() { Hum = World.Audio.StartLoop(EngineCue, Owner); }   // 새 자원을 얻은 뒤 이전 값을 해제
@skipWhileBusy on Owner.Alarm() {
    let siren = World.Audio.StartLoop(SirenCue, Owner);           // 이 블록이 끝나면 멈춤
    await Delay(5.0, Clock.Game);
}
fn Quiet() { Hum?.Pause(); }                                      // 담긴 자리에서 호출
~~~

- 대입 순서는 새 자원 획득 → 이전 자원 해제 → 저장이다. 유지 자원 획득 API는 `T?`를 반환하고 유지 자원 필드는 `T?`로 선언한다. 획득이 할당량·예산·비활성 상태로 실패하면 결과는 `null`이며 이전 자원은 해제되고 디버거에 보고한다.
- 지역 유지 자원을 얻은 지점부터 블록 끝까지 어느 경로에도 `await`가 없으면 오류다. 그 자원은 같은 스크립트 단계 안에서 멈추기 때문이다. 수정 예는 계속 유지하려면 필드에 담기, 정해진 시간만 유지하려면 블록 안에서 `await Delay(...)`다.
- Disable·Detach가 해제한 필드는 `null`이 된다. 다시 획득하는 것은 사용자 코드이며 `on Enable`에서 한다.
- 필드 초기값에서 획득할 수 없고(상수식과 component 생성만 허용), 필드가 reload로 삭제되면 그 자원을 해제한다. 타이머 handle은 없다. 기다림은 `await Delay`, 반복은 handler 안의 반복문이다.
- binding schema가 자원 종류(유지·일회성)를 정하므로 사용자는 반환 타입으로 구분한다(§10.2). 유지 자원을 반환하는 API는 `Start*`, handle을 반환하지 않는 재생(일회성 효과와 `await`하는 재생)은 `Play*`로 이름을 짓고, 반복 설정된 에셋을 일회성 API에 넘기면 컴파일·쿡 오류다(§10.3).

### 7.2 에셋 참조

- **하드 참조** `@edit var OpenSound: SoundAsset = Asset("...")`: 이 클래스를 사용하는 prefab·셀의 의존성으로 함께 로드된다(스크립트 모듈의 의존성이 아니다). 엔진 표현은 `SoftObjectRef<T>` 필드와 instance가 보유하는 `AssetLease`이며, "하드"는 그 World 데이터를 담는 레벨·prefab 패키지의 Dependencies(Hard)로 나타낸다(AssetSystem 6.3절 — World 객체는 에셋을 `ObjectRef`로 가리킬 수 없다). 함수 본문의 하드 `Asset(...)` 리터럴도 같은 방식으로 클래스의 의존성이 된다. instance override가 값을 바꾸면 바뀐 에셋이 의존성이 된다. null이 아니다. 필드 초기값의 참조는 클래스 기본값 레코드에 들어가며, override하지 않은 값의 레코드 참조도 패키지 Dependencies에 넣고 로드 대조도 기준 적용 결과로 한다 — 이 의존 수집이 L2의 선행 조건이며 그 전에는 레코드의 참조 기본값(Soft 포함)이 null만 허용된다([RuntimeClassMetadata](RuntimeClassMetadata.ko.md) '클래스 기본값 결정').
- **소프트 참조** `Soft<T>`: AssetId만 가진다. 빈 값이 없으므로 선택 값은 `Soft<T>?`로 쓴다(SD19). `T`가 필요한 자리에 쓰면 오류이며 빠른 수정이 `try await X.Load()`를 삽입한다.
- prefab·PrimaryAsset별 하드 의존 크기 예산을 두고 초과하면 쿡 오류다. 셀 경계를 넘는 하드 edge도 쿡이 진단한다(전체 계획 §10.2).
- 로드한 결과 `T`는 lease를 공유하는 복사 가능한 값이며, 보관하는 동안 상주를 유지한다. `@net`·RPC·`@save`에는 `T`를 둘 수 없고 `Soft<T>`를 쓴다(빠른 수정 제공).

### 7.3 로드

로드(클래스 멤버):

~~~text
@skipWhileBusy on Owner.Hit() {
    let effect = try await HitFx.Load();           // 기본 형태: 실패하면 이 실행을 끝내고 기록
    World.Effects.PlayAt(effect, Owner.Location);
}
@skipWhileBusy on Owner.Equipped() {
    Fx = try await HitFx.Load();                   // 필드에 보관: instance가 끝날 때까지 상주
}

async fn LoadPreviews() {                          // 진행률이 필요할 때(WidgetController)
    let req = Assets.LoadAll(Previews);            // 지역 전용 요청 handle (유지 자원)
    while (!req.IsDone) {
        Widget.Progress.Value = req.Progress;
        await NextTick();
    }
    Loaded = try await req;                        // Array<Texture>, 이후 Loaded가 상주를 유지
}
~~~

- `X.Load()`와 `Assets.LoadAll(list)`는 요청 handle `LoadRequest<T>`를 반환하며 `await`의 결과는 `Result<T>`다. 요청 생성은 실패하지 않으며 상한·예산을 넘으면 admission(새 요청의 수락)이 지연된다. 요청 handle은 지역 변수에만 둘 수 있고 한 번만 `await`한다. 상주를 유지하는 방법은 결과 값을 보관하는 것 하나다.
- `LoadAll`은 전부 성공하거나 하나의 오류를 반환하고 전체 진행률을 제공한다.
- 같은 package 요청은 엔진이 하나의 로드로 합친다. 한 실행의 취소는 다른 실행의 요청을 취소하지 않는다(전체 계획 §9.2).
- 결과는 현재 target이 요구하는 준비 mask(CPU 객체·render·physics 등)를 충족한 뒤 반환된다. 임시 fallback 표시 상태를 완료로 보지 않는다. 현재 엔진의 `ReadyMask`는 `Cpu`만 구현되어 있고 Render는 `UnsupportedCapability`이므로 L2의 외형 에셋 로드는 렌더 단계의 준비 mask 구현이 선행 조건이다.
- 해제된 package는 상한이 있는 cache(전체 계획 §29.6 allowlist)에 잠시 남아 같은 에셋을 반복 로드할 때의 비용을 줄인다. 같은 호출 위치의 반복 로드는 ledger에 표시한다.
- 시간 제한은 `await Race(req, Delay(5.0, Clock.UI));` 다음 `req.IsDone`을 확인한다. `All`·`Race`의 인자로 쓰는 것은 요청 handle의 1회 `await`로 세지 않는다. `Race`에서 진 요청은 취소되지 않고 변수의 범위가 끝날 때까지 계속 진행한다(규칙 2). 그래서 "5초 뒤 안내를 띄우고 계속 기다리기"는 `Race` 다음 `try await req`로 쓴다. 결과를 꺼내는 `try await req`는 1회만 허용하고 두 번째는 `ContractViolation` fault다.

### 7.4 실패·취소와 전체 계획 §9.2 계약

- 결과는 `Result<T>`다: `NotFound`(삭제된 에셋, 예: 오래된 저장 데이터), `LoadFailed`(IO·검증·등록 실패). 등록 자체가 실패하면 이미 종료된 요청을 반환하고 `await`에서 `LoadFailed`를 받는다.
- 실행이 취소되면 요청 handle이 해제되어 이 실행의 관심만 반환한다. 결과를 받을 실행이 없는 완료는 binding의 고립 완료 경로가 lease를 반환한다(NL03 fixture).
- 한 LoadTicket에는 OnReady callback이 하나뿐이므로 같은 요청을 기다리는 여러 대기자는 binding이 분배한다.
- 쿡은 스크립트 상수·`@edit` 필드·DataTable 행에서 도달 가능한 모든 소프트 대상을 chunk에 배정하고, 배정되지 않으면 쿡 오류로 처리한다.
- instance별 진행 중 로드 수에 상한을 두어 취소된 로드가 누적되지 않게 한다.
- 로드한 값을 범위가 다른 곳(예: 셀과 함께 unload되는 Actor의 실행 → `WorldService` 필드)에 저장하면 엔진이 새 범위로 lease를 다시 발급한다. scope revoke 순서는 실행 취소 → 소유 범위의 정상 해제 → revoke이며, revoke 시점에 남은 항목은 ledger 실패다.
- 편집기에서 에셋이 다시 import되면 스크립트가 보유한 lease를 스크립트 단계 경계에서 새 revision으로 교체한다(편집기 전용).

### 7.5 서버와 준비 mask

- 준비 mask는 타입이 요구하는 mask와 target capability의 교집합이다. 전용 서버에서 외형 전용 타입(texture·사운드 등)의 로드는 즉시 효과 없는 stub으로 완료되고, 그것을 받는 API는 효과가 없다. 서버 쿡에서 제거된 하드 참조도 같은 stub이다. 따라서 같은 코드가 서버에서 실패해 보상 지급 등이 누락되는 일이 없다.
- PIE의 서버 session은 서버 capability와 서버 쿡 filter를 적용해 이 경로를 출시 전에 시험한다.
- 복제 spawn된 소프트 prefab은 클라이언트가 fixup 전에 로드한다. `@net` `Soft<T>` 필드의 `Changed`에서는 명시적으로 로드한다.

### 7.6 생성·이동·travel·스트리밍

- **생성:** `World.Spawn(prefab, at)`은 상주한 prefab만 받으며 Prefab의 instancing 경로(ConstructionContext, 미공개 graph rollback)로 생성한다 — 엔진 `World::SpawnActor`에는 아직 prefab·transform 인자가 없으므로 Prefab PF 단계가 선행 조건이다. 한 실행 조각에서 1,024개, 한 스크립트 단계에서 8,192개를 넘으면 `BudgetExceeded`다(§8.4). 소프트 prefab은 `let p = try await DoorRef.Load(); let door = try World.Spawn(p, at);`로 쓴다. 생성된 Actor는 prefab 의존 묶음의 lease를 스스로 보유하므로 `p`가 사라져도 prefab이 해제되지 않는다.
- **이동:** `try await Owner.Teleport(position)`은 목적지 prefetch·commit gate를 내부에서 처리하고 관심을 streaming source로 이관한다(전체 계획 §10.2). 직접 prefetch(`World.Prefetch`)는 full이다.
- **travel:** `Session.Travel(map): Result<void>`은 서버·단독 실행 전용이며 기다리지 않고 요청만 접수한다. 클라이언트 호출은 `NotAuthority`, 중복 호출은 `AlreadyTraveling`이다. 이전 World에 속한 호출 실행은 World 종료 취소로 끝나며 fault로 보고하지 않는다. 로딩 화면은 엔진이 프로젝트 설정으로 관리하여 PrepareDestination에 표시하고 새 World의 초기 준비 mask가 충족된 뒤 숨긴다(전체 계획 §10.3). 직접 제어하는 로딩 화면·seamless travel은 full이다.
- **다른 셀의 Actor:** `@edit var Target: Door?`가 다른 셀의 Actor를 가리키면 SoftEntityId로 저장되고, 셀이 상주할 때 자동 해석되며 unload되면 null이다. `on Target.Opened()` 연결도 이를 따른다. 셀 unload는 그 셀 객체들의 정상 Disable·Destroy(§5.1 종료 경로 표)이며 scope revoke가 아니다.
- **data layer(full):** `World.SetDataLayer(layer, DataLayerState.Active)`는 서버 전용이며 복제·저장되는 World 상태다. 준비는 `await World.DataLayerReady(layer)`로 기다린다. late join 클라이언트는 현재 상태를 받는다.
- **컷씬 준비:** `await World.Sequencer.Prepare(seq)`는 shot 예측으로 texture mip·오디오 chunk를 미리 상주시킨다. 일반 로드의 준비 mask에는 이것이 포함되지 않는다(전체 계획 §9.5).

## 8. 오류 처리

### 8.1 컴파일 진단

진단 예:

~~~text
UI/HealthBar.spark:14:14: error S0412: 'Target' may be null.
    let hp = Target.Current;
             ^^^^^^
  'Target' has type 'Health?'. Another actor reads as null once it is destroyed.
  fix: if (let health = Target) { let hp = health.Current; … }

Characters/PlayerCharacter.spark:42:5: error S0731: Cannot assign to a function result.
    GetTransform().Location.Z = 1.0;
    ^^^^^^^^^^^^^^
  GetTransform() returns a copy, so assigning to it never changes the original.
  fix: RelativeLocation.Z = 1.0;
~~~

- **형식(SD37):** `경로:줄:열: error S코드: 메시지` 다음에 원문 줄·표시·설명·`fix:` 수정 예를 둔다. 위치가 앞에 있어 VS Code problem matcher와 clang 형식 도구가 그대로 읽는다. 경고는 없으므로 종류는 항상 `error`다. CLI 종료 코드는 성공 0, 컴파일 오류 1, 사용법 오류 2다(AppInfoCommandLine 14.4절).
- **언어:** 컴파일러·VM·엔진이 내는 진단과 fault 보고는 영어다(실행 중 메시지 영어 규칙). LSP는 locale 옵션으로 지역화된 설명을 덧붙일 수 있으며 코드·위치·수정 예는 바뀌지 않는다. 사용자가 쓰는 `Log` 메시지와 `Assert` 문구의 언어는 사용자 콘텐츠이므로 프로젝트 정책이다.
- **코드 대역:** S01xx 어휘, S02xx 구문, S03xx 이름 해석, S04xx null·타입, S05xx 숫자·상수식, S06xx 실행 문맥·효과, S07xx 대입·`let`, S08xx 이벤트·수명 hook·재진입, S09xx 유지 자원·로드, S10xx 저장·ID·호환성, S11xx 패키지·reload·backend. 코드는 한 번 발급하면 의미를 바꾸지 않는다. 엔진 공통 진단 코드 등록부(RMC#### 메타 컴파일러, UI validate 코드와 함께)는 CompilerDiagnostics 문서에 둔다(§17.2).
- 진단은 원인 하나를 작성한 코드 그대로 보이고 수정 예를 포함한다. 한 원인에서 파생된 연쇄 오류는 표시하지 않는다.[R23] 수정 예는 이름 규칙(§3.1)을 지키도록 생성한다. 지역 이름은 멤버 이름이 아니라 타입 이름에서 도출한다(`Target: Health?` → `health`). 유사 이름 제시는 Core `EditDistance`와 이름 붙은 문턱 상수를 쓴다.
- 사전 대조 리터럴(`Tag`·`Action`·`CVar`)이 사전에서 이름이 바뀐 경우 사전의 redirect(`@formerly`와 같은 역할)를 따라 자동 변환하고 빠른 수정으로 소스를 고친다. redirect도 없으면 오류다.

### 8.2 Result와 try

- 조회 실패는 `T?`, 작업 실패는 `Result<T>`다. 결과를 사용하지 않으면 오류다. 직접 처리할 때는 `r.Value`·`r.Error`를 확인한다.
- `Result<T>`를 반환하는 함수는 성공을 `return 값;`, 실패를 `return Error.NotFound;`처럼 쓴다. `Error`는 엔진이 정한 닫힌 enum이며(case 목록은 부록 B) 사용자 정의 오류 타입은 없다(SD38).
- `try`는 실패하면 실행을 더 진행하지 않는다. `Result`를 반환하는 함수에서는 실패를 호출자에게 전달한다. 그 밖의 함수와 handler에서는 현재 실행(Execution 트리 전체)을 EndExec로 끝내고 위치별로 1회 진단을 기록한다. `try` 실패는 fault가 아니므로 격리 집계와 PIE 정지에 포함하지 않는다. 예외로 hook이 절반만 실행된 채 남지 않도록 `Awake`·`Enable`·`Start` 안의 `try` 실패는 그 hook의 fault다(SD43).
- 스크립트에 노출하는 API 이름에는 `Try` 접두를 쓰지 않는다. `try` 키워드("실패하면 여기서 끝")와 반대 의미로 읽히기 때문이다. `Result`를 돌려주는 C++ `TryX`는 Sparks에서 자동으로 `X`로 노출한다(§10.3, SD44).

### 8.3 fault와 발생 위치별 처리

| fault | 원인 |
|---|---|
| `IndexOutOfRange` | 범위 밖 원소 쓰기, Map에 없는 키의 복합 대입·경로 쓰기(읽기는 `T?`) |
| `IntegerOverflow`, `DivideByZero` | 정수 overflow, `Math.Div`·`%`의 0, 범위 밖 숫자 변환, `float(i)`의 정밀도 손실 |
| `NonFinite` | NaN·Inf를 만든 연산, float32 범위 밖 값의 전달 |
| `BudgetExceeded`, `CascadeLimit` | 명령어·호출 깊이·native stack·메모리·Spawn 수 예산, 발행 큐 상한(스크립트 발행), 이벤트 연쇄 깊이 상한·연속 이월. 메모리 예산 초과는 할당 위치를 보고 |
| `AssertFailed` | 모든 빌드에서 검사하며 조건식은 Pure 문맥. `@edit(Min, Max)` 범위 밖 쓰기 포함 |
| `ContractViolation` | native API의 계약 위반(파괴 지연 미준수, PendingDestroy 대상의 쓰기 호출 등), 요청 결과를 두 번 꺼냄 |

| 발생 위치 | 처리 |
|---|---|
| 필드 초기값·Awake·spawn 중 Enable | spawn 실패(`Result`), 미공개 graph rollback(전체 계획 §5.3), 큐에 들어갔지만 전달되지 않은 emit 폐기, 획득한 자원은 Detach로 해제(SD43) |
| 재활성화의 Enable, Start | 해당 instance 격리 |
| Tick 계열 | 해당 instance의 Tick·FixedTick·LateTick을 그 클래스 모듈의 새 revision이 게시될 때까지 중지. Disable·Enable로는 해제되지 않음 |
| 그 밖의 handler·실행 | 그 실행 트리만 중단하고 해제. 같은 instance에서 직전 600 프레임(프레임 번호 기준, 일시 정지·디버거 정지 중 프레임 제외, 경계 포함) 안에 세 번째 fault가 기록되면 격리(SD42). 한 실행 트리의 fault는 1회로 센다 |
| `on Validate`·`@editorButton`·`@cheat` | §8.4 편집기 경계 |
| 원격 client가 유발(RPC 처리 중) | instance·session에 영향 없음, 해당 connection에 집계(임계 초과 시 끊기, §8.4) |

- **격리:** Disable hook을 실행하지 않고 모든 실행을 EndExec하며 유지 자원 해제와 연결 key 폐기를 수행하고 값 필드는 검사용으로 보존한다. admission은 그 instance가 파괴될 때의 Destroy hook 1회만 허용한다. 다시 활성화할 수 없고, 그 instance가 파괴될 때(component는 소유 Actor 파괴, WorldService는 World 종료, WidgetController는 widget 제거) Destroy hook을 실행한 뒤 Detach한다(§5.1 종료 경로 표). 필수 `@edit` 참조 대상이 제거된 component도 같은 방식으로 격리한다(§4.3).
- **예산:** 엔진이 실행 조각을 시작할 때마다(호출·재개) 명령어 수 예산을 새로 부여하고 그 조각과 조각이 동기 호출한 함수가 공유한다. 예산은 플랫폼 allocator가 아닌 논리 단위(명령어 수, 원소 수 × 원소 크기)로 계산하므로 모든 플랫폼·빌드·재시뮬레이션에서 결과가 같다.
- **보고:** fault는 (모듈 hash, 함수, 위치, 종류)별로 1회 상세 보고하고 이후 횟수만 집계한다. PIE는 새 fault 위치에서 일시 정지한다. 출시 빌드는 hash·위치를 기록해 오프라인에서 소스 위치로 변환한다.
- 격리는 Sparks 쪽 상태이며 World는 모른다. 스크립트 bridge는 격리된 instance에 대한 엔진의 `SetEnabled(true)`·OnEnable을 무시하고, Tick 중지는 `SetTickEnabled(false)`로 구현하므로 Disable·Enable로 풀리지 않는다. 이 bridge 계약을 ER 7절에 추가한다(§17.2).
- 개발·출시 빌드와 두 backend(VM·AOT)는 같은 검사를 수행한다. 게임 실행에서 이미 엔진에 요청한 외부 효과는 되돌리지 않는다(전체 계획 §23.4). 편집기 문맥은 §8.4를 따른다.

### 8.4 오류 방어 경계

사용자 코드의 어떤 실수도 process·편집기·다른 instance·저장 데이터를 망가뜨리지 않는 것이 목표다(SD32). 범위는 컴파일 → 로드 → 실행 → 편집기 → 출시 순으로 좁혀 막는다.

| 경계 | 규칙 |
|---|---|
| 컴파일 | 오류가 있으면 실행 중인 게임은 마지막 성공 revision을 유지한다. 오류가 있는 상태로 PIE를 시작하면 마지막 성공 revision으로 실행할지 묻고, 성공 revision이 없으면 그 패키지의 클래스를 데이터 보존 placeholder로 둔다. 쿡은 실패한다 |
| 로드 | `.sparkc`가 verifier에서 거부되거나 interface·binding hash가 맞지 않으면 그 모듈의 클래스를 쓰는 prefab은 원래 필드를 보존하는 placeholder component로 로드하고 게임은 계속 실행한다(§15의 placeholder 규칙 확장) |
| 실행 | fault는 실행 트리 → instance 순으로만 번진다(§8.3). VM 내부 불변식 위반(verifier를 통과한 bytecode의 VM 버그)만 엔진 버그로 크래시 보고하며 사용자 코드 원인은 모두 fault다 |
| native 경계 | 스크립트에 노출하는 C++ 함수는 호출자 입력(인자·대상 상태)으로 Fatal·`check`·assert하지 않고 `Result`나 `ContractViolation`으로 돌려준다. binding thunk는 schema가 선언한 전제조건(non-null·범위·lane·권한·PendingDestroy 여부)을 호출 전에 모두 검사한다. 노출 함수의 인자 검증에 Fatal을 쓰면 policy 검사로 막는다(binding schema `NoFatalOnInput`, §10.2). Borrow는 thunk 안에서만 얻고 fault로 빠져나갈 때도 RAII로 반환한다 |
| 권한 | `@net` 필드 쓰기는 권한 구역(§14)에서만 컴파일된다. 실행 시 권한이 없으면 쓰기를 무시하고 위치별 1회 보고한다(fault 아님 — 클라이언트 연출이 멈추지 않게) |
| 폭주 | 무한 반복은 명령어 예산, 깊은 재귀는 호출 깊이 256과 native stack 검사, 대량 생성은 조각당 1,024·단계당 8,192 Spawn 상한, 이벤트 폭은 전달 수 상한과 연속 이월 검사(§5.4), 로그 폭주는 호출 위치별 초당 10회 상한(초과분은 횟수만 집계)으로 막는다. PIE에서는 같은 생성 위치의 살아 있는 Actor가 100,000을 넘으면 일시 정지하고 보고한다 |
| 편집기 | `on Validate` fault는 그 instance의 미리보기를 새 revision까지 끄고 에셋을 바꾸지 않는다. `@editorButton`·`@cheat` 실행의 fault·`try` 실패는 Undo transaction 전체를 취소하므로 에셋이 반쯤 바뀐 채 저장되지 않는다. "외부 효과를 되돌리지 않는다"(§8.3)는 게임 실행에만 적용한다 |
| 저장 데이터 | 저장 로드는 RPC와 같은 길이·값 검사를 거치고 범위 밖 값은 고쳐 보고한다(§15). 변환할 수 없는 값은 기본값으로 두고 보고하며 로드를 실패시키지 않는다 |
| 복구 | 개발 빌드에서는 격리된 instance를 그 클래스 모듈의 새 revision으로 재생성하는 편집기 명령을 제공한다(§12.4 instance 재생성 경로). 출시 빌드는 엔진 이벤트 `World.ScriptFaulted(instance, kind)`를 발행하므로 C++·WorldService가 checkpoint 복귀·메뉴 복귀로 softlock을 피할 수 있다 |
| 원격 | 원격 client가 유발한 fault는 그 connection에만 집계하고, 60초 안에 100회를 넘으면 connection을 끊는다(값은 프로젝트 설정) |

## 9. 소유·해제와 누수 방지

전체 계획 §29.6은 메모리 누수 0을 출시 Gate로 두고 모든 allocation·resource에 owner scope와 최종 회수 조건을 명시하도록 규정한다. Sparks는 소유 범위를 고정하고 범위 종료의 해제 경로를 3개 연산으로 한정한다.

### 9.1 소유 범위와 해제 연산

최상위 소유 범위는 **Session**(PlaySession 또는 process host), **Instance**(스크립트 수명 객체), **Execution**(handler 실행 1회와 그 `All`/`Race` 하위), **ModuleHost**(코드)의 4개다. 스크립트는 소유 범위를 새로 만들지 않는다. 범위가 끝날 때의 일괄 해제는 다음 연산으로만 일어나며, 이 연산은 사용자 코드를 실행하지 않고 할당하지 않고 멈추지 않으므로 실패할 수 없다. 범위 안의 개별 해제(대입·블록 종료·참조계수 0)는 §9.2를 따르며 같은 조건을 지킨다. admission은 새 실행·전달·로드 요청의 수락이다. 격리(§8.3)는 Detach의 앞 단계(실행 EndExec·유지 자원 해제·연결 key 폐기)를 먼저 수행하고 값 필드 초기화를 Destroy hook 뒤로 미룬 것이며 같은 실패 불가 조건을 지킨다.

| 연산 | 수행 내용 |
|---|---|
| `EndExec(frame)` | 하위 실행 종료 → 대기 key 폐기 → 지역 유지 자원을 역순 해제 → 값 슬롯 해제 → revision lease 반환 |
| `Detach(instance)` | admission 차단 → 모든 실행 EndExec → 필드 유지 자원을 선언 역순 해제 → 연결 key 폐기 → 자기 이벤트 delegate 비우기 → 값 필드 초기화 |
| `CloseSession` | admission 차단 → 모든 instance Detach → 큐·Changed 자리 폐기 → World 파괴 → 연결 표 해제(이 시점에 ledger 0). 정상 종료 hook은 그 전에 World 정상 종료에서 실행한다(§5.1) |

| 종료 경로 | 적용 |
|---|---|
| 정상 종료·fault·취소 | EndExec(fault는 Execution 트리 전체, 취소는 취소된 실행과 그 하위) |
| 비활성화 | Disable hook 후 실행 취소·유지 자원 해제·연결 비활성 |
| 파괴 | 스크립트 단계 끝 적용(PendingDestroy) → 프레임 끝 정리에서 묶음 단위 Disable(활성이면) → Destroy hook → Detach(§4.3) |
| 격리 | hook 없이 실행·유지 자원·연결 해제, 값 필드 보존. 그 instance가 파괴될 때 Destroy hook 후 Detach |
| GC가 도달 불가로 판정 | 회수 전 cleanup hook에서 Detach(hook 없음, 정확히 1회 보장, 전체 계획 §5.2 — 선행 조건 §17.2) |
| 셀 일괄 unload | Disable·Destroy, 각 Destroy 반환 직후 Detach. 발행자별 compact는 셀 단위 1회 |
| travel | 이전 World 정상 종료(Disable·Destroy·Detach). Session은 유지 |
| PIE Stop | World 정상 종료(Disable·Destroy) 후 CloseSession |
| reload | 변경된 `on` 선언과 필드만 해제. 대기 중인 이전 revision 실행은 §12.4를 따른다 |
| process 종료 | 모든 World 정상 종료 → 모든 CloseSession → ModuleHost → TypeRegistry → NamePool Shutdown. 정해진 시간 안에 Busy가 해소되지 않으면 강제 해제하지 않고 NL06 실패로 보고 |

### 9.2 대상별 직접 보유자와 해제 시점

| 대상 | 직접 보유자 | 해제 시점 |
|---|---|---|
| 값 버퍼(string·Array·Map) | 보관한 슬롯(참조계수), 회계는 instance | 참조계수 0 |
| 이벤트 연결 | Instance의 연결 집합 | Detach, 격리, 원천 필드 변경, 선언이 추가·삭제·변경되는 reload, CloseSession |
| 이벤트·시간·로드 대기 | Execution | 발생 시 또는 EndExec. 발행자 소멸 시 대기가 완료되지 않은 채 끝남(§6.1) |
| 실행 | handler 실행 자리 또는 부모 실행 | 반환·fault·취소·Detach |
| 발행 큐 항목 | Session | 전달, 대상 소멸 시 폐기, CloseSession |
| 유지 자원 | 담긴 필드 또는 지역 변수 | 대입·`null`·블록 종료·비활성화(Disable hook 반환 후)·격리·EndExec·Detach. backend 완료가 늦으면 유지 자원 slot의 generation을 확인하여 폐기된 slot에 도착한 완료는 버린다 |
| 일회성 효과 | 엔진 subsystem | 최대 지속 시간 후 스스로 종료. CloseSession(PIE Stop 포함)이 시작되면 재생 중인 것을 즉시 중지하고 이후 호출은 효과 없이 반환 |
| 에셋 lease | 결과 값을 보관한 슬롯·생성된 Actor | 참조계수 0, Actor 파괴 |
| 생성된 Actor | World 또는 부착 부모 | 스크립트 단계 끝 파괴, 부모 파괴, CloseSession. 설계상 상한이 없는 유일한 증가이므로 검출 대상 |
| code revision | ModuleHost, instance·frame이 lease | 마지막 lease 반환. process 단위 ledger로 검사 |
| eval·외부 모듈 | EvalScope 또는 ModuleLease | 범위 종료, 비동기 unload(§12.7) |
| 진단 표 | session ring, 위치별 counter는 revision(hash key) | ring 덮어쓰기, revision 반환. 강한 참조를 보관하지 않음 |

### 9.3 예산과 Gate

- **예산(논리 단위, SD34):** 상한은 획득 시점에 거부하며(로드 요청은 거부하지 않고 admission을 지연, 실행은 건너뜀) 해제 경로에서는 적용하지 않는다. 기본값은 프로젝트 설정이며 쿡 시 고정된다. 서버·클라이언트·재시뮬레이션의 fault 결과가 같아야 하므로 실행 중에는 바꿀 수 없고, 엔진 쪽 표현은 `BootCVar` + `Replicated()`(`sparks.budget.*`, CVarSystem)다.

| 예산 | 기본값 | 단위 |
|---|---|---|
| 명령어 수: 실행 조각 | 1,000,000 | bytecode 명령어(AOT도 같은 계수, §12.5) |
| 명령어 수: Disable·Destroy(Teardown) | 100,000 | 같음 |
| 명령어 수: eval 1회 | 1,000,000 | 같음 |
| 호출 깊이 | 256 | frame |
| VM 메모리: instance / session | 16 MiB / 512 MiB | 논리 크기(원소 수 × 원소 크기, 공유 버퍼는 보유자마다) |
| 단일 버퍼 | 16,777,216 원소 또는 64 MiB | 먼저 닿는 쪽 |
| 유지 자원: 종류별 instance당 | 64 | 개 |
| 구독 연결: instance당 / session당 | 4,096 / 1,048,576 | 항목 |
| 동시 실행·대기: instance당 | 256 | 실행 |
| 진행 중 로드: instance당 | 32 | 요청 |
| Spawn: 실행 조각당 / 스크립트 단계당 | 1,024 / 8,192 | Actor |
| 전달 수 N: 스크립트 단계당 / 발행 큐: session당 | 65,536 / 262,144 | 항목(§5.4) |
| 적용 루프: 스크립트 단계당 | 64 | 회(§5.1) |
| eval scope | 16 | 개 |
| pause 중 큐: 종류별 | 4,096 | 항목(§9.4) |
| 종료 Busy deadline / TypeRegistry 재시도 | 5 s / 120 프레임 | (§9.1, §12.7) |

생성 위치별 생성 Actor 수와 모듈별 revision 수는 상한 없이 ledger에 표시한다(revision은 §12.4의 살아 있는 revision 상한 8이 별도로 적용된다).
- **매 스크립트 단계 디버그 불변식:** 연결 표의 살아 있는 key 수와 항목 수 일치, 발행자별 죽은 항목 ≤ 직전 compact 시점의 살아 있는 항목 + 8(C2), instance별 유지 자원 ≤ 필드 수 + 활성 frame의 지역 수.
- **G1 · PIE Stop:** CloseSession 직전 연결 표 참조계수 1, CloseSession 후 모든 스크립트 counter 0, revision lease와 이름 pool이 기준선 복귀(전체 계획 §29.6 NL09).
- **G2 · 2시간 soak:** 생성·파괴 반복, 배열 재작성, 5분마다 reload, 10초마다 eval. 30초 구간별 최솟값을 기록하고, 연속 10구간 중 직전 구간보다 큰 구간이 8개 이상이면서 그 10구간의 최소제곱 기울기가 예산의 분당 1%(예산이 없는 counter는 시작 값의 분당 1%)를 넘는 counter가 없어야 한다. 위반 시 생성 위치별 증가량 상위 항목을 보고한다.
- **G3 · 음성 대조:** key 폐기 생략, C2 비활성, 취소 시 지역 자원 누락, 이름 pool 삽입 등 의도한 누수가 Gate를 실패시키는지 확인한다.
- **G4 · 실패 행렬:** 모든 획득·`TryAdd`·재연결에서 N번째 실패, 모든 `await`에서 fault·취소를 주입하고 Detach 후 모든 counter 0을 확인한다.
- **G5 · reload 행렬:** §12.4의 변경 종류마다(본문·필드·`on`·component 구성·클래스 계층·`@net`·`@rpc`·const·사전) 대기 중인 실행·유지 자원·구독 연결이 있는 상태에서 reload하고, 롤백 경로(계획 실패·instance 재생성 실패)를 주입해 counter·필드 값·연결 수가 기대와 같은지 확인한다.
- **G6 · backend 차등:** conformance corpus를 VM과 AOT로 실행해 결과·fault 종류와 위치 ID·이벤트·Changed 순서·ledger counter·예산 초과 지점을 비교한다. AOT를 고를 수 있는 모든 target에서 돈다(§12.5).

### 9.4 lane과 일시 정지

- VM 버퍼는 Game lane에서만 변경한다. 비동기 저장·복제 직렬화·디버거 watch·telemetry는 고정된 버퍼를 읽고 해제를 Game lane으로 되돌리며, ledger 검사 전에 이를 모두 처리한다. 되돌림은 실패할 수 없어야 하므로 버퍼 머리의 노드를 쓰는 `IntrusiveMpscQueue`(Synchronization 12.2절, AssetSystem lease 해제와 같은 방식)로 하며 용량 제한 큐를 쓰지 않는다.
- Sparks handler는 Game lane에서 발행되는 이벤트에만 연결한다. 다른 lane의 이벤트는 엔진이 POD로 전달하고 Game lane에서 값으로 생성한다.
- breakpoint·pause 중 쌓이는 큐(lane 간 전달, UI clock 대기, Changed)는 상한이 있고 같은 종류를 합치며 넘친 수를 집계한다(전체 계획 §23.4).

### 9.5 이름 pool

Core `NamePool`은 항목을 Shutdown에서만 해제하며 기본 상한이 65,536개다. 런타임 문자열을 이름으로 바꾸는 경로는 검색 전용 `TryFind`(Core 변경 C5)를 사용해 새 항목을 만들지 않는다. 런타임 문자열이 native `Name` 인자로 흘러가는 바인딩은 binding schema 오류다. source eval은 이름을 새로 등록하지 않는다.

## 10. 엔진 바인딩

### 10.1 C++ 선언과 Sparks에서의 형태

기존 `ROCKS_CLASS`·`ROCKS_PROPERTY`에 함수 리플렉션(CU2)·enum·struct(CU8)를 더한 선언에서 바인딩을 생성한다. 이벤트는 Core `MulticastDelegate` 필드에 `ROCKS_PROPERTY`를 붙이고, 메타 컴파일러가 필드 타입으로 `FieldKind::Event`를 판별한다(CU3 — 현재 `FieldKind`는 `Value`·`ObjectReference`뿐). 이벤트 인자 이름은 `Params` 지정자로 주며 이벤트 필드의 이름도 §10.3의 필드 이름 변환을 따른다. 매크로 이름은 현재 코드 기준이다(매크로 단축 개명은 보류 중이며 착수 시 함께 바꾼다).

C++ · `Health.h`:

~~~cpp
ROCKS_CLASS(ScriptExtensible)             // 신설 지정자. World 파생이므로 WorldOnly는 상속된다
class Health : public rocks::Component {
    ROCKS_GENERATED_BODY()

public:
    explicit Health(rocks::Allocator& allocator) noexcept : Died(allocator) {}

    ROCKS_PROPERTY(Edit, SaveGame)        // Edit: CU1 편집 메타(신설), SaveGame: 현재 지정자
    rocks::int32 maxHealth_ = 100;

    ROCKS_PROPERTY(ReadOnly, Replicated)  // Replicated: 네트워크 단계에서 신설
    rocks::int32 current_ = 100;

    ROCKS_PROPERTY(Params = "killer")     // CU3: MulticastDelegate 필드 → FieldKind::Event
    rocks::MulticastDelegate<void(rocks::WeakObjectRef<rocks::Actor>)> Died;

    ROCKS_FUNCTION(Lane = Game, Effects = WriteWorld)   // CU2
    rocks::Result<void> ApplyDamage(rocks::int32 amount, rocks::WeakObjectRef<rocks::Actor> instigator);
};
~~~

Sparks에서는 `var MaxHealth: int`(편집·저장), 읽기 전용 속성 `Current: int`, `event Died(killer: Actor?)`, `fn ApplyDamage(amount: int, instigator: Actor?): Result<void>`로 보인다(Studio hover 표기, 소스 문법이 아님). `WeakObjectRef`는 현재 코드의 `WeakRef`다(개명은 §17.2 코드 후속).

| 예시의 이름 | 현재 상태 | 선행 |
|---|---|---|
| `ROCKS_CLASS`·`ROCKS_PROPERTY`·`ROCKS_GENERATED_BODY`, `SaveGame`·`ReadOnly`·`Name =` | 있음 | — |
| `ScriptExtensible`, `ScriptName`, `ScriptWritable` | 없음 | 신설(L1, 메타 컴파일러) |
| `Edit`(편집 메타), `Params` | 없음 | UiToolkit CU1·CU3 |
| `ROCKS_FUNCTION`과 `Lane`·`Effects` 등 | 없음 | CU2(CVar 문서의 `Cheat`·`Authority`·`Help`와 지정자 이름 공간을 함께 정한다) |
| `Replicated` | 없음 | 네트워크 단계(RX 0절 범위 밖) |
| `FieldKind::Event` | 없음 | CU3 |

**Sparks 필드와 엔진 필드 플래그.** Sparks 클래스의 필드는 C++ 필드가 아니라 스크립트 instance 저장소에 놓이며, 런타임에 등록하는 schema(동적 schema — §17.2 선행 조건)로 리플렉션·저장·override·Changed에 참여한다. attribute는 다음 플래그에 대응한다.

| Sparks | 엔진 필드 의미 |
|---|---|
| attribute 없는 `var` | Transient(패키지·세이브에 저장하지 않는 실행 상태) |
| `@edit` | 영속(패키지에 저장) + 편집 노출(CU1) + prefab override 대상 |
| `@save` | SaveGame(세이브 범위) |
| `@net` | Replicated |
| `let` | 스크립트 쪽 재대입 금지(엔진 `ReadOnly`와 별개) |
| `@edit(Min:, Max:)` | CU1 강제 범위(편집 입력 범위이자 실행 계약, §15) |
| `@edit(UiMin:, UiMax:, Units:, Category:, EditIf:)` | CU1 편집 메타(편집기 전용) |
| `///` 문서 주석 | 편집기 tooltip |

### 10.2 binding schema

| 대상 | 기록 항목 |
|---|---|
| 타입 | TypeId, 기반 타입, `ScriptExtensible`, 수명 종류(엔진 객체·에셋·값), 외형 전용 여부(준비 mask), 구현한 interface 표(CU9) |
| 필드 | FieldId, 값 형식, 단위, nullability, 편집·저장·복제·ReadOnly·`ScriptWritable`, 변경 통지 지원(CU5) |
| 계산 속성 | getter·setter 함수 쌍으로 만든 쓰기 가능한 속성(예: `RelativeLocation` ← `SceneComponent`의 `relativeTransform`과 `SetRelativeLocation`). 손실이 생길 수 있는 왕복(quat 저장값 ↔ `RelativeRotation` Rotator)은 binding이 정규화 규칙을 선언한다. CU5 setter 통지와 함께 정한다 |
| 함수 | FunctionId, 시그니처와 nullability, 초당 값 인자, lane, `ThreadSafe`, `Deterministic`, 부수 효과, 최대 작업량, 취소 가능 여부, `MaySafepoint`, 권한, 편집기·출시 가용성, 오류 타입, `NoFatalOnInput`(입력 오류로 Fatal하지 않음 — 선언이 없으면 노출 불가, §8.4), PendingDestroy 대상에서의 동작(읽기 허용·쓰기는 효과 없음 또는 `ContractViolation`) |
| 자원 정책 | 반환하는 것의 종류: 유지 자원(로드 요청 `LoadRequest<T>` 포함) / 일회성 효과 / 값. 기다리는 API는 고립 완료 해제 경로를 선언. 선언이 없으면 노출 불가 |
| 파괴 지연 | 엔진 객체를 파괴하는 함수는 스크립트 단계 끝 deferred command로 처리함을 선언(SD3) |
| 이벤트 | `FieldKind::Event`, FieldId, 인자 이름(`Params`)·형식, 발행 lane, 발행 측(서버·소유 클라이언트·모든 기기), 스크립트 단계 안 `Broadcast`의 큐 기록, VM 연결 thunk |

### 10.3 이름·노출과 API 규칙

- 타입·함수·이벤트 이름은 C++ 이름 그대로다. 리플렉션 필드(이벤트 필드 포함)는 도구 이름(`Name =` 지정값, 없으면 C++ 이름에서 끝의 `_`를 뗀 것)의 첫 글자를 대문자로 바꿔 노출하며(SD2), 그 밖의 `_`를 포함한 이름은 `ScriptName`으로 지정해야 한다. `ScriptName`은 도구 이름 위에 덮는 스크립트 전용 이름이다.
- `Result`를 돌려주는 C++ 함수 `TryX`는 Sparks에서 자동으로 `X`로 노출한다(SD44). 변환한 이름이 다른 멤버와 겹치면 메타 컴파일 오류이며 `ScriptName`을 요구한다. 엔진 수명 hook 이름과 겹치는 C++ 함수(`Actor::Destroy` 등)는 `ScriptName`(예: `RequestDestroy`)으로 노출한다.
- 메타 컴파일러는 타입별로 기반 체인을 포함한 최종 Sparks 이름을 계산한다. 이름이 중복되거나 예약 이름(부록 A)·hook 이름과 같으면 메타 컴파일 오류이며 `ScriptName`을 요구한다. 스크립트에 노출하는 함수의 C++ 오버로드도 서로 다른 `ScriptName`이 없으면 메타 컴파일 오류다.
- C++ 수명 가상 함수(`On*`·`OnRegister`·`OnUnregister`)는 노출하지 않는다. 쓰기 가능한 속성이 있으면 같은 값을 쓰는 `SetX` 함수는 노출하지 않는다.
- **스크립트용 API 규칙:**
  - 이동 API는 초당 값을 받고 dt는 엔진이 적용한다. binding이 초당 값 인자를 표시하며 그 인자 식에 `dt`·`Time.Delta`를 곱하면 오류다.
  - 화면 표시 문자열은 `Text`만 받는다. 등록된 이름(socket·애니메이션 slot·태그·에셋)을 받는 인자는 검증 타입을 사용한다.
  - 유지 자원을 반환하는 API는 `Start*`, handle을 반환하지 않는 재생(일회성 효과와 `await`하는 재생)은 `Play*`로 이름을 짓고 위치·부착은 인자로 받는다. 로드 요청(`Load`·`LoadAll`)은 이 규칙에서 제외한다. 이름에 `Try` 접두를 쓰지 않는다.
  - 외형 전용 API는 전용 서버에서 효과가 없도록 구현한다.
  - 이벤트는 발행 측을 선언하고 언어 서버가 hover에 표시한다. 클라이언트에서만 실행되는 handler의 `@net` 필드 쓰기는 컴파일 오류다.
- 편집기 버튼은 `@editorButton fn`(full), 치트 명령은 `@cheat fn`(full, CheatSet 대응 — 출시 빌드 제외, 권한 `Authority`, 이름 변환은 CVar 14.3절 규칙)으로 표시한다(SD44). EditorWorld 객체에는 편집기 문맥 함수만 Undo transaction 안에서 호출한다.
- VM 진입은 Game lane에서만 허용하며 모든 빌드에서 같은 검사를 수행한다.
- **CU9(규격 확정):** TypeInfo에 `Interfaces: Span<const InterfaceImpl>`를 두고 `InterfaceImpl{ TypeId Interface; Span<const FunctionId> Slots; }`로 구현한 interface와 slot 표를 기록한다. slot 순서는 interface 선언 순서이고 호환을 위해 메서드 추가는 끝에만 허용한다. 호출은 (interface TypeId → slot 번호) 조회 1회로 dispatch하며 스크립트·C++ 모두 이 표를 쓴다. C++ interface는 `ROCKS_INTERFACE` 선언과 클래스 지정자로 등록하고 메타 컴파일러가 누락 구현을 오류로 낸다. Sparks interface는 컴파일러가 같은 표를 `.sparkc`에 기록한다. UI 툴킷 CU1–CU8과 같은 리플렉션 확장 묶음이다.

### 10.4 엔진 서비스 연동

| 서비스 | Sparks 형태 | 엔진 쪽 계약 |
|---|---|---|
| 로그(SD36) | `Log.Info/Warning/Error/Debug("Opened by ${actor.Name}")`. `Fatal`·`Trace` 레벨은 스크립트에 주지 않는다 | 카테고리는 `Script.<manifest logCategory>`(LoggingSystem 6.2 문법). 컴파일러가 `${expr}`을 이름 있는 자리 `{Name}`(식의 마지막 멤버 이름, 중복이면 오류)으로 낮추므로 템플릿 해시(EventId)가 컴파일 시 정해지고 반복 집계·rate limit(LG13)이 동작한다. VM은 실행 조각마다 `LogContextScope`(모듈·함수·instance·Actor id)를 쌓는다. 호출 위치별 초당 10회 상한(§8.4) |
| CVar·설정 | 읽기: `let dist = CVar("game.door.speed")` → `CVarRef<float>`, `.Value`. 런타임 이름 조회는 `CVars.Find<T>(s)`. 설정 struct는 `Settings.Get<GameplaySettings>()` | 값은 World 범위 snapshot이며 Sync 지점에서만 바뀐다. 쓰기는 `@cheat`·`@editorButton` 문맥에서만 Code 층으로 권한 재검사 후 한다. 스크립트 정의 CVar는 `@cvar(Help: "...", Min:, Max:) const`(full)로 `RegisterDynamic` 등록하며 스크립트에는 읽기 전용이다. 변경 알림은 Sync 뒤 입력 batch 단계의 큐 항목(`on CVars.Changed()`) |
| 입력(SD35) | `let JumpAction = Action("Gameplay.Jump")`, `on JumpAction.Started/Triggered/Ongoing/Completed/Cancelled()`, `JumpAction.Value` | 입력 상세안 21.2절 phase 어휘를 그대로 쓴다. 별칭(`Pressed`)은 두지 않는다. focus 손실·context 전환은 `Cancelled`로 온다 |
| 물리 | `on CollisionEnter(other: Actor, hit: Hit)`, `Hit`의 `ImpactPoint`·`ImpactNormal`·`Normal`·`Distance`(cm), 질의 `World.Physics.Raycast(from, to, filter): Hit?`, `World.Physics.OverlapSphere(center, radius, filter, max): Array<Actor>` | `Hit`는 PhysicsJoltAdapter 5절(UE `FHitResult`와 같은 법선 의미). 접촉 이벤트의 법선 방향은 어댑터 J11 확정 전까지 미정. closure가 없으므로 질의는 결과 배열형이며 Tick 문맥에서 허용하고 최대 결과 수 `max`는 필수 인자다. `AddImpulse`의 속도 변화 판(UE `bVelChange`)은 `AddVelocityChange`로 따로 노출한다 |
| UI | `class X : WidgetController`, `Widget.이름` 경로, `on Widget.Ok.Clicked()` | UiToolkit 5.4절의 초기화·사전 구성·구성·해체·풀 반환이 각각 Awake·Validate·Enable·Disable·Destroy에 대응한다. WidgetPool의 비활성 항목은 Enable·Disable을 받지 않는다. Screen push/pop은 유지 자원(`World.Ui.StartScreen(...)`)이다. UI binding 변환 함수는 `@pure fn`(full)이다 |
| 커브 | `@edit var Falloff: TangentCurve;`, `Falloff.Eval(t)` | native 값 struct(§4.2), CurveData 14절. 회전 성분은 도(X = Roll) |
| 시계 | `Time.Delta` 등(부록 B), `Delay(s, Clock.Game\|Clock.UI)` | 엔진 `GameClock`(UI 시계는 ER 후속) |

## 11. VM

- **레지스터 기반**, 타입별 opcode, 32비트 고정 명령. Lua 5.0은 2003년 레지스터 VM으로 전환하였고[R1], Shi 등은 실행 명령 수 47.21% 감소, 코드 크기 25.05% 증가, switch dispatch 실행 시간 32.28% 감소를 보고하였다(Pentium 4).[R2][R3] Luau·GDScript와 달리 모든 타입이 확정되므로 타입 태그 검사가 없다.[R6][R7]
- **3개 bank:** Scalar(태그 없는 64비트), Ref(엔진 객체 handle+generation), Buf(참조계수 버퍼). 스크립트 상태는 GC root를 만들지 않으며 재개 시 `this`의 generation을 확인한다. 호출 깊이는 256 frame까지다(§9.3).
- **필드 쓰기 opcode**는 Changed 연결 여부를 확인하고 실행 조각 안의 첫 쓰기에서 변경 후보로 기록한다(조각 끝 값 비교, §5.3). 정수 overflow·부동소수점 유한성을 검사하고 FMA 축약을 쓰지 않는다.
- **safepoint:** back-edge, 호출 복귀, `await`, 할당. Teardown 문맥과 파괴 경로의 Disable에서는 safepoint를 열지 않는다(전체 계획 §5.2). Borrow는 native call thunk 안에서만 얻고 반환 전 해제한다(전체 계획 §6.3).
- **verifier:** 로드 시 선형 1회.[R4] 구조(범위·branch·bank 크기), 타입(bank·TypeId·초기화·non-null), 바인딩(FunctionId·시그니처 hash·lane·자원 정책), 문맥(금지 호출, `await` 위치, eval capability)을 검사한다.
- **dispatch:** switch 기준으로 시작하고 computed goto·tail-call은 측정 후 선택한다.[R10][R11][R12][R14]
- **측정 항목(미측정):** opcode당 시간, native call 왕복, 숫자 검사 비용, 이벤트 큐 전달 비용(스크립트 단계당 1만·6.5만), 구독 연결 1만 개 Detach(목표 0.5 ms 이내), 5만 항목 발행자 compact(단일 pass, 목표 1 ms 이내), 본문 reload ≤2 s(전체 계획 §6.3·§27.8), AOT backend의 코드 크기·빌드 시간·VM 대비 속도.
- **trace·profiler:** TraceSystem 채널 `script`에 handler·함수 CPU 구간(FunctionId → 이름 표, 표 상한 16,384 안에서 등록), await 사이를 이은 Execution span, fault·`CascadeLimit`·`@skipWhileBusy` 버림·reload를 mark·counter로 기록한다. `TraceContext`에 instance·Actor id를 넣어 로그(§10.4)와 상관시킨다. 채널이 꺼져 있으면 비용은 relaxed load 1회다. profiler UI는 L4다.
- **디버거:** pc→source 표, 함수별 breakpoint 플래그, native 경계 표시, 멀티플레이의 process·World 표시(전체 계획 §23.4).

bytecode · `t = Math.Min(t + Time.Delta / SwingSeconds, 1.0)`:

~~~text
GETF.F64   s3, r0, #SwingSeconds   ; r0 = this
DIV.F64    s4, s2, s3              ; s2 = Time.Delta, 결과가 유한하지 않으면 NonFinite
ADD.F64    s4, s1, s4              ; s1 = t
LOADK.F64  s5, k0                  ; k0 = 1.0
MIN.F64    s1, s4, s5
~~~

## 12. 컴파일·핫 리로드·eval·backend

### 12.1 파이프라인과 .sparkc

`.spark` → 파서(오류 허용) → 타입 검사(binding schema·ids) → typed IR(문맥·대입·수명) → bytecode(bank 할당) → verifier → `.sparkc`

`.sparkc`는 header(format·compiler version·모듈 ID·content hash·interface hash·profile), imports(의존 interface hash, native API의 FunctionId·시그니처 hash, Tag·Action GUID), types(TypeId·FieldId·SubobjectGuid·초기값·이벤트·RpcId), functions(bank 크기·실행 문맥·await 지점별 레지스터 형태), constants, 분리 디버그 정보로 구성한다. 출력은 입력이 같으면 바이트 단위로 같다. 컴파일에 실패하면 실행 중인 게임은 마지막 성공 revision을 유지한다.

### 12.2 안정 ID

- TypeId·FieldId·SubobjectGuid·FunctionId·RpcId는 C++와 같은 `.rocksids.json` 형식을 쓴다(전체 계획 §7.1). 현재 메타 컴파일러는 빌드와 분리된 `ids add/rename/retire` 저작 명령으로만 발급하고 이름 키는 C++ 정규 이름이며 kind는 type·field·enum 계열뿐이다. Sparks 컴파일러는 자기 패키지 manifest의 **두 번째 발급자**로서 모듈 경로 기반 이름 키(`<패키지>/<경로>.<선언>`)와 subobject·function·rpc kind를 쓴다(StableIdManifest 확장, §17.2). 개발 빌드에서는 저장 후 컴파일이 새 선언의 ID를 발급하고, 입력 중인 미완성 선언에는 발급하지 않는다.
- 데이터가 연결된 ID(`@save`·`@net`·`@edit` 필드, 소유 component, enum case)의 소실은 매 컴파일의 오류이며 빠른 수정이 `@formerly("OldName")`을 삽입한다. `@formerly`는 manifest의 aliases 항목으로 기록된다. 의도한 삭제는 `rocks ids retire`(AppInfoCommandLine 14절 commandlet)로 tombstone을 남긴다. enum은 case 이름으로 저장하므로 case 이름 변경에도 `@formerly`를 쓴다.
- FunctionId는 이벤트 handler·RPC·`@cheat`·`@editorButton`·리플렉션 호출 대상·interface 구현 함수·엔진 가상 함수의 `override fn`에 발급한다.

### 12.3 증분 컴파일과 쿡

파일마다 interface hash와 함수별 body hash를 분리하고 본문 변경은 해당 함수만 다시 컴파일한다(전체 계획 §27.2·§27.3). interface hash에는 선언 시그니처뿐 아니라 `const` 값과 함수별 추론 효과 집합(§6.2)을 포함하므로, 본문 변경으로 효과가 바뀌면 호출하는 모듈이 다시 검사된다. Tag·Action·CVar 사전과 WidgetAsset schema도 그것을 쓰는 모듈의 입력이다. 쿡은 전체 컴파일·검증, 리터럴 재검증, WidgetAsset 경로 대조, 하드 의존 예산, 소프트 대상 chunk 배정, prefab·save·RPC 호환 검사, 디버그 정보 분리를 수행한다.

### 12.4 핫 리로드

핫 리로드는 필수 기능이다(사용자 요구). 편집기·개발 빌드에서 `.spark`를 저장하면 Rocks Build의 Sparks compile action(SD6)이 다시 컴파일하고, 실행 중인 모든 World(PIE의 모든 process 포함)에 새 revision을 적용한다. 출시 빌드에는 컴파일러가 없으므로 reload가 없다(eval 정책과 별개).

**등급.** 변경마다 가장 가벼운 등급으로 처리한다(SD28).

| 등급 | 대상 변경 | 처리 | 도입 |
|---|---|---|---|
| R1 본문 교체 | 함수·handler 본문 | 새 revision을 게시한다. 이후 시작하는 handler·호출은 새 revision을 쓴다. 대기 중인 실행은 `ReloadExecutions`(아래)를 따른다 | L1 |
| R2 제자리 migration | Sparks 필드 추가·삭제·형식 변경, `on` 선언 추가·삭제·변경, 필드 기본값, `const`, Tag·Action·CVar 사전, WidgetAsset schema | reload 경계에서 FieldId migration·구독 연결 재구성. 사전·schema 변경은 그것을 쓰는 모듈을 다시 컴파일한 뒤 적용한다 | L2 |
| R3 instance 재생성 | 클래스 계층(기반 타입) 변경, `let` component 구성 변경(subobject 추가·삭제), struct 배치가 바뀌어 제자리 변환이 불가능한 경우 | 현재 필드 값을 FieldId별로 보관 → 정상 종료 hook(Disable·Destroy)과 Detach → 새 클래스로 재생성(subobject는 Prefab 3.2절 표대로, 삭제된 SubobjectGuid를 가리키던 prefab override는 Prefab 3.7절 고아 보존) → 보관한 값을 spawn parameter 단계에 덮어쓰기 → Awake·Enable. 같은 Actor graph의 재생성은 한 묶음이다. 편집 World(PIE 밖)에 배치된 instance도 같은 경로로 재생성한다(UE reinstancing 대응) | L2 |
| R4 복제 schema 교체 | `@net` 필드 추가·삭제·형식 변경, `@rpc` 추가·삭제·시그니처 변경 | `@rpc` 추가는 RpcId manifest에 덧붙여 즉시 반영한다. 그 밖에는 클래스의 복제 schema hash를 올리고 모든 PIE process가 같은 프레임 경계에서 새 revision을 적용한다. 삭제·변경된 RpcId의 대기 메시지는 폐기·보고하고, 서버는 해당 클래스 instance의 전체 상태를 late join 경로(§5.3 활성화 전 적용 규칙)로 다시 보내며, 클라이언트는 hash가 맞을 때까지 그 instance의 복제 수신을 보류한다 | L3 |
| PIE 재시작 | 엔진 C++ 변경, 패키지 의존 graph 변경 | 재시작 안내(자동 재시작하지 않음) | — |

**원자적 적용과 롤백(SD29).**

1. **계획:** 컴파일·verifier가 끝나면 엔진 쪽 준비를 모두 계산한다 — 타입 revision 등록, 동적 schema, 클래스 기본값 레코드 재합성, 필드별 변환 가능 여부, subobject 변경 계획, 의존 패키지의 interface hash, 예산.
2. **판정:** 계획 단계에서 하나라도 실패하면(변환 불가가 아닌 오류, 기본값 레코드 재합성 실패, 결정성·참조 위반, 예산 초과) 아무것도 적용하지 않고 이전 revision을 유지하며 진단을 낸다. 적용 단위는 바뀐 패키지와 그 패키지에 의존하는 패키지 전체다(all-or-nothing).
3. **적용:** 실행 중인 실행 조각이 없는 reload 경계(프레임 끝 정리 뒤, §5.4 순서 7)에서 한 번에 적용한다. 디버거가 정지해 있으면 재개 후 첫 경계로 미룬다.
4. **instance 단위 실패:** 적용 중 R3 재생성의 Awake·Enable fault처럼 instance 하나에서 난 실패는 그 instance만 격리하고 나머지는 계속한다.

**보존 규칙.**

- 필드 값은 FieldId로 보존한다. 형식이 바뀌면 저장 로드와 같은 무손실 변환(§15)을 쓰고, 불가능하면 기본값을 두고 보고한다. enum은 case 이름으로 다시 대응시키며 삭제된 case는 첫 case로 두고 보고한다. struct·컨테이너 원소는 FieldId 단위로 재귀 migration한다.
- 객체 handle·로드한 에셋 값·유지 자원 필드는 형식이 같으면 그대로 두고, 다르면 해제한다. 삭제된 필드의 유지 자원·lease는 해제한다.
- 새 기본값은 spawn 이후 한 번도 쓰지 않은 필드(필드별 "쓰기 있음" bit)에만 반영하고, 쓴 필드는 편집기에 표시한다. prefab·instance override가 있는 필드는 override를 유지한다.
- reload가 적용한 값은 Changed를 발행하지 않는다. 구독 연결 순서는 유지되며 새 `on` 선언은 끝에 추가된다.
- Tick 설정(`@tick`)이 바뀌면 엔진 `TickSettings`를 다시 등록한다(엔진은 OnStart 때 한 번 등록하므로 bridge가 해제·재등록한다).
- Tick fault로 중지된 Tick은 그 클래스 모듈의 새 revision 게시 때 다시 시작한다. 격리된 instance는 개발 빌드에서 "이 revision으로 재생성" 명령으로 R3 경로를 밟을 수 있다(§8.4).

**대기 중인 실행(SD30).** 실행을 새 코드로 자동 이전하지 않는다(§20.3 #6). 프로젝트 설정 `ReloadExecutions: Keep | Cancel`이 정하며 PIE 기본값은 `Cancel`이다.

- `Keep`: 대기 중인 실행은 이전 revision으로 끝까지 실행하고 그 실행이 하는 호출도 이전 revision 안에서 해석한다(전체 계획 §23.4).
- `Cancel`: 본문이 바뀐 함수를 실행 스택에 가진 실행을 취소한다. 취소된 Start는 처음부터 다시 실행하고, 이벤트 handler는 다음 이벤트를 기다린다. `while` + `await NextTick()` 반복이 이전 코드로 계속 도는 일을 막는다.
- R2·R3에서 변경·삭제된 FieldId를 bytecode에서 참조하거나 삭제된 handler에 속한 실행은 설정과 무관하게 취소한다.
- 모듈별 살아 있는 revision은 8개까지다. 넘으면 가장 오래된 revision의 실행을 취소하고 보고한다.

**편집기 표시.** reload마다 요약을 보인다: 적용된 revision, 등급별 대상 수, 취소된 실행 수, 해제된 자원, 손실이 있었던 변환, 재생성·격리된 instance, 다시 시작한 Tick, 이전 revision으로 실행 중인 handler 수와 재시작 명령. 실패한 reload는 원인 진단과 "이전 revision 유지 중"을 표시한다.

**엔진 쪽 계약(선행 조건, §17.2).** 같은 TypeId 아래 여러 revision 레코드의 공존과 instance별 revision 포인터·경계에서의 교체·마지막 lease 반환 시 retire(RCM "모듈 reload"의 Busy 거부를 대체), 스크립트 필드의 동적 schema 등록과 graph encode/decode·override 경로·Changed/CU5 통지, live instance의 subobject 추가·삭제, World 프레임의 reload 경계 hook.

**목표와 시험.** 본문 reload는 저장부터 적용까지 2 s 이내(SD8, 전체 계획 I1). G2 soak의 "5분마다 reload"는 L2 Gate, G5 reload 행렬(§9.3)은 등급별 L1–L3 Gate다.

탈출하는 상수 문자열은 참조계수 버퍼로 복사하여 revision을 고정하지 않는다.

### 12.5 스크립팅 backend: VM과 native AOT

스크립팅 backend는 Unity의 Mono/IL2CPP 선택과 같은 맥락의 **옵션**이다(SD31). AOT 기능 자체는 반드시 제공하지만 쓸지 말지는 target·빌드 구성마다 프로젝트가 고르며 강제 기본값이 없다.

| 설정 | 값 | 기본 | Unity 대응 |
|---|---|---|---|
| `ScriptingBackend`(target × 빌드 구성) | `Vm` · `Aot` | `Vm` | Player Settings의 Scripting Backend(Mono/IL2CPP) |
| `AotOptimization` | `Debug` · `Release` · `Master` | `Release` | IL2CPP의 C++ Compiler Configuration |
| `AotFallbackModules` | module 목록 | 비어 있음 | — |

- 기본값이 모든 target에서 `Vm`인 이유: Rocks VM은 interpreter라 실행 가능한 메모리를 할당하지 않으므로(§12.6) iOS·콘솔에서도 강제할 이유가 없다. 두 backend는 관찰 가능한 결과가 같으므로 선택은 성능·실행 파일 크기·빌드 시간의 trade-off다.
- 편집기와 PIE는 항상 VM으로 실행한다(핫 리로드). 개발 빌드에서 `Aot`를 고르면 아래 혼합 규칙으로 reload를 유지한다.
- bytecode(`.sparkc`)는 backend와 무관하게 항상 생성하고, **VM interpreter는 AOT 구성에도 항상 링크한다** — 출시 후 콘텐츠 패치·DLC의 새 스크립트, eval·외부 모듈(항상 VM), 로드 시 hash 불일치 fallback에 필요하다.
- 파이프라인은 typed IR → C++ 생성 → 플랫폼 toolchain이다(전체 계획 §23.1의 "LLVM 기반 AOT"와 다르므로 §17에 기재).[R8][R9] JIT가 아니므로 iOS·콘솔의 JIT 제한과 무관하다.[R5]

**두 backend의 동등성 규칙.** AOT 생성기는 다음을 지켜 VM과 비트 단위로 같은 결과·fault·이벤트 순서를 낸다. 검증은 G6 차등 시험(§9.3)이다.

1. **예산 계수:** basic block마다 그 block에 대응하는 bytecode 명령어 수를 더하고 VM과 같은 지점(back-edge·호출 복귀)에서 검사한다. 호출 깊이·메모리 예산도 같은 논리 단위다.
2. **평가 순서:** 모든 부분식을 임시 변수로 순서화해 왼쪽에서 오른쪽 평가(§3.3)를 지키고, `&&`·`||`·`??`·`?.`는 명시적 분기로 생성한다.
3. **부동소수점:** strict FP, fast-math·재결합·FMA 축약 금지, `long double` 미사용, float32 변환은 최근접 짝수, `-0.0` 보존. 초월 함수는 두 backend가 같은 Core Math 함수(같은 실행 파일의 같은 심볼)를 호출하며 AOT가 컴파일러 내장 함수로 바꾸지 않게 한다. 컴파일러별 FP 설정은 build 규칙으로 강제한다.
4. **정수:** overflow 검사 intrinsic, `Math.Div`·`%`·int64 최솟값은 공통 runtime 함수.
5. **필드 쓰기·이벤트:** VM opcode와 같은 runtime 경로(Changed 후보 기록, emit 큐, 파괴·활성 전환 지연)를 호출한다. 필드 직접 대입으로의 최적화는 금지한다.
6. **safepoint·취소:** await·back-edge·호출 복귀·할당 위치를 VM과 1:1로 맞추고 generation 확인도 같은 지점에서 한다.
7. **위치 ID:** fault 보고 key의 위치는 bytecode 위치 ID로 통일하고 AOT는 검사 지점마다 이 ID를 상수로 넣는다. 보고·집계·격리 판정이 backend와 무관해진다.
8. **native stack:** AOT frame이 커서 호출 깊이 예산보다 먼저 native stack이 넘칠 수 있으므로 함수 진입마다 남은 stack을 검사하고 부족하면 `BudgetExceeded`로 처리한다(process를 끝내지 않음).

**AOT를 골랐을 때.**

| 기능 | VM | AOT | IL2CPP와 비교 |
|---|---|---|---|
| 런타임 eval·외부 모듈 | eval 정책을 따름 | VM 경로로 그대로 동작 | IL2CPP는 런타임 코드 로드 불가 |
| 핫 리로드(개발) | 지원 | 지원 — 고친 함수만 bytecode revision으로 교체 | IL2CPP는 불가 |
| 디버깅(개발) | DAP 전체 | breakpoint를 건 함수는 VM 진입으로 교체(deopt), 정지한 AOT 실행은 함수 단위 breakpoint만. native 디버거는 `#line`으로 `.spark` 줄을 표시 | development build 스크립트 디버깅 |
| 출시 진단 | 위치 보고 | 크래시·fault 위치 역변환 | 같음 |
| 리플렉션 호출 | 가능 | FunctionId 발급 대상(§12.2)은 strip하지 않는다. 사용자 generic이 없어 generic sharing 문제가 없다 | managed stripping |
| 빌드 시간·크기 | 짧음·작음 | 길음·큼(§11 측정 항목) | 같음 |

**VM과 AOT 혼합.**

- FunctionId마다 진입 간접 표(AOT 진입 또는 VM 진입)를 둔다. 개발 구성에서는 모든 Sparks→Sparks 호출이 이 표를 거치고 Sparks 함수끼리의 C++ inline을 금지한다. 출시 구성은 inline을 허용하되 위치 ID와 예산 계수 의미를 유지한다.
- 개발 구성 AOT는 필드 접근을 binding 표를 거쳐 하도록 생성한다. reload로 필드 배치가 바뀌면 그 클래스를 쓰는 AOT 함수를 VM 진입으로 바꾼다.
- 대기 중인 AOT 실행은 §12.4의 `ReloadExecutions`를 따른다. AOT 코드는 실행 파일에 정적으로 있으므로 revision lease가 반환되어도 unload할 것이 없다.
- 호출 규약: 경계에서는 VM frame의 bank 창(Scalar·Ref·Buf)으로 인자와 반환값을 주고받고 AOT 내부만 native 인자를 쓴다. Ref가 GC root가 아니므로 stack map이 필요 없다.
- async 함수·handler는 명시적 상태 기계 struct로 생성하고 ledger에 집계되는 실행 allocator에서 할당한다.

**빌드.**

- module마다 `.cpp` 1개를 생성하고(크면 분할) unity build·PCH를 쓸 수 있게 한다. 입력 key는 (module content hash, 의존 interface hash, binding hash, 생성기 버전, `AotOptimization`)이며 같으면 Rocks Build action 결과를 재사용한다.
- 시간·경로·순회 순서에 의존하지 않고 이름은 TypeId·FunctionId로 만들어 "입력이 같으면 바이트 동일"(§12.1)을 생성 C++에도 적용한다.
- 생성 C++에 `#line "<pkg>/X.spark" N`을 넣고, 분리된 symbol에 (native 주소 → module hash, 함수, 위치 ID) 표를 두어 VM과 같은 형식으로 오프라인 역변환한다(§8.3).
- 생성 코드는 예외·RTTI·STL 없이 엔진 runtime 헤더만 쓰고 같은 policy 검사를 받으며 주석은 영어다. 모든 플랫폼 toolchain의 공통 부분집합으로 쓴다.
- verifier는 생성 시점에 실행한다. 로드 시 SD7 검증은 AOT 코드에 대해 (module content hash, FunctionId·시그니처 hash, binding hash) 대조로 한다.

**실패와 fallback.**

- 런타임: `.sparkc`의 content hash가 실행 파일 안 AOT 코드의 hash와 다르면 그 module은 VM으로 실행하고 1회 보고한다(오류 아님).
- 빌드: AOT를 고른 구성에서 생성·C++ 컴파일이 실패하면 빌드 실패다. `AotFallbackModules`에 명시한 module만 VM fallback을 허용하고 빌드 결과에 표시한다. 차등 시험 불일치는 fallback으로 덮지 않는다(Gate 실패).
- 개발 중: AOT 생성이 실패하면 진단을 내고 VM으로 계속한다. 핫 리로드는 그대로다.

### 12.6 런타임 eval 정책

| target 정책 | 값 | 출시 기본(SD4) |
|---|---|---|
| `ScriptEval`(소스 eval) | `Off` · `DevOnly` · `On` | `Off` |
| `ExternalModules`(외부 모듈) | `Off` · `DevOnly` · `Signed` · `On` | `Off` |

두 정책은 빌드 상수다. `Off`이면 해당 기능을 링크하지 않고, `DevOnly`·`On` 안에서 실제로 켜는 것은 `BootCVar`(`sparks.eval.enabled`, `sparks.modules.enabled`)다. 스크립팅 backend 선택(§12.5)과는 독립이다 — AOT 구성에서도 eval·외부 모듈은 VM으로 실행한다.

`Off` target에는 컴파일러를 링크하지 않는다. JIT가 아니므로 실행 가능한 메모리를 할당하지 않으며, 마켓 판단 대상은 다운로드한 코드로 기능을 변경하는 것을 제한하는 규정이다(App Store 2.5.2).[R31] 콘솔 규정은 공개 자료로 확인하지 못하였다.

### 12.7 eval·외부 모듈의 수명

- **source eval**은 기존 타입에 대해 함수·handler를 실행하며 타입을 선언하지 않고, 변경 가능한 전역이 없고, 이름을 새로 등록하지 않는다. 같은 정적 타입 검사·검증·예산·ledger를 거치며 호출 가능한 API는 capability(기본 `ModSafe`: World 읽기·쓰기, IO·네트워크·저장 금지)로 제한한다.
- **외부 모듈**은 자체 ID manifest를 갖고 타입을 pool과 무관한 NameKey([Core 상세 사양](CoreDetailedSpec.ko.md) T02)로 등록한다. 의존 패키지의 interface hash가 다르면 로드를 거부한다. `@save`·`@net`·`@rpc`는 서명된 모듈에서만 허용한다.
- **서명:** 쿡 패키지 안의 `.sparkc`는 콘텐츠 서명(RSIG)이 이미 덮는다. 외부 모듈 서명은 RSIG 형식을 재사용하되 label `"rocks.sparks-module.v1\0"`, 모드 서명 전용 키 목록, `contentVersion` 하한과 `scope`를 갖는 별도 봉투다. Release가 매니페스트 밖 입력을 읽지 않는다는 ContentSigning 1절에 `ExternalModules=Signed` 경로를 예외로 등록하고, `ScriptEval=On`인 출시는 매니페스트 밖 입력을 받는다는 점을 target 결정 기록에 남긴다(§17.2).
- 외부 모듈이 생성한 Actor의 수명은 World가 관리하며 모듈 unload 전에 파괴한다. 저장 대상에서 제외(transient)한다.
- unload는 비동기다(Unloading → Unloaded). TypeRegistry가 Busy이면 120 프레임 동안 재시도한 뒤 그 모듈을 사용 중지하고 Gate 실패로 보고한다. Unloading 중 호출은 `ModuleUnloading` 오류를 반환하고, `Result`를 반환하지 않는 호출은 `ContractViolation` fault다.

### 12.8 신뢰 경계

| 입력 | 신뢰 | 검사 |
|---|---|---|
| 빌드에 포함된 `.sparkc`·AOT 코드 | 신뢰 | RSIG(쿡 패키지), 로드 시 verifier 또는 hash 대조(SD7) |
| 서명된 외부 모듈 | 조건부 | 모듈 서명 봉투, verifier, capability, 예산 |
| 서명 없는 외부 모듈·source eval | 비신뢰 | verifier(외부 bytecode fuzz Gate 대상), `ModSafe` capability(World 읽기·쓰기만, IO·네트워크·저장·`@cheat`·편집기 API 금지), 예산(DoS 방어), session 격리 |
| 저장 파일 | 비신뢰 | 길이·값·범위 검사, placeholder 로드(§15) |
| RPC 인자 | 비신뢰 | interest·소유 connection·길이·유한성·enum 검사, 빈도 제한(§14) |

로그에는 민감 정보(계정·토큰·결제 정보)를 쓰지 않는다(LoggingSystem 12절). verifier는 보안 경계이므로 외부 bytecode를 받는 target에서는 fuzz Gate를 통과해야 한다.

## 13. 언어 서버와 편집기

LSP 서버 `sparks-lsp`를 VS Code 확장과 Rocks Studio Script Editor가 공유한다.[R15] CLI·Rocks Build·언어 서버·런타임 eval이 같은 컴파일러 라이브러리를 사용하므로 같은 입력에서 같은 진단이 도출된다(전체 계획 §23.4). 서버는 엔진 없이 binding schema와 사전 파일만으로 동작하고 별도 process이며 문서 버전별 arena를 사용한다. 디버깅은 DAP adapter가 VM 디버거에 연결한다.[R16][R17]

자동완성 위치(`|` = 커서):

~~~text
class DoorLogic : ScriptComponent {
    @edit var RequiredKey = Tag("Item.|");        // 태그 사전
    on |                                           // Awake, Start, Tick, TriggerEnter, Owner.Interacted, State.Changed …
    on Target.|                                    // Target 타입의 이벤트와 필드 Changed
    fn Ring() {
        World.Audio.PlayAt(Asset("/Game/Audio/|"), Owner.Location);   // SoundAsset 에셋만
    }
}
~~~

- 리터럴 자동완성은 컴파일 검증과 같은 사전을 사용한다. Unity 이름(`Update`, `FixedUpdate`, `LateUpdate`, `OnEnable`, `GetComponent`)을 입력하면 Rocks 이름(`Tick`, `FixedTick`, `LateTick`, `Enable`, `Find<T>`)을 제시한다. core 패키지에서는 full 후보를 표시하지 않는다.
- hover는 타입·단위·문서 주석·native 선언 위치·하드 의존 크기·bundle·이벤트의 발행 측을 표시한다. 빠른 수정: `?` 추가, `?? return`, 누락 import, handler 골격(`@save`·`@net` 필드의 Changed 골격은 `on Enable`에 같은 반영 호출을 함께 생성), 재진입 정책 표시(`@skipWhileBusy`·`@restart`), `Soft`→`try await X.Load()`, `@formerly`, 이름 충돌 시 이름 변경.
- 도입 단계: L1 진단·의미 강조, L2 자동완성(멤버·hook·attribute·리터럴·경로)·hover·정의 이동·빠른 수정·서식·추론 타입 표시, L3 ID를 유지하는 rename·참조 찾기·VS Code 디버거.
- **디버거(DAP, L3):** `await`를 넘는 step(같은 실행의 다음 조각으로), 조건 breakpoint(Pure 문맥으로 평가), watch는 고정된 버퍼를 읽음(§9.4), breakpoint 중 저장은 재개 후 첫 reload 경계에 적용(§12.4), PIE의 여러 process 동시 연결, pause 중 큐 상한(§9.4), 출시 빌드의 오프라인 위치 변환(§8.3·§12.5).

### 13.1 시험 작성

- `@test fn`(full 또는 패키지 manifest의 test 패키지)은 headless World fixture에서 실행한다. 시험 시계는 `NextTick`·`Delay`를 결정적으로 진행하며 실제 시간에 기대지 않는다(sleep 금지). `Assert`가 실패 판정이다.
- CLI `sparks test <패키지>`가 실행하고 결과는 진단과 같은 형식으로 보고한다. 같은 corpus를 VM·AOT 차등 시험(G6)에도 쓴다.
- **conformance corpus:** `Conformance/<영역>/<사례>.spark`와 `.expect`. `.expect`에는 진단 코드·줄·열, 또는 실행 trace(전달 순서·fault 종류·위치 ID·ledger counter)를 적는다. 본문의 "오류다" 문장과 SD마다 음성 사례를 1개 이상 둔다. 영역은 진단 코드 대역(§8.1)과 같다.
- 제안 목표(미측정): 파일 1,000개 기준 자동완성 p95 100 ms, 편집 후 진단 p95 300 ms. 검증: 후보 golden test, prefix fuzz, CLI와의 진단 동등성, rename 후 저장 데이터 보존.

## 14. 네트워크

replication · RPC:

~~~text
class Weapon : ScriptComponent {
    @net var Ammo: int = 30;
    @net(OwnerOnly) var Reserve: int = 90;
    let ReloadKey = Action("Gameplay.Reload");

    on Enable() { RefreshAmmo(); }                            // 활성화 전 적용·late join 상태 반영
    on Ammo.Changed() { RefreshAmmo(); }                      // 모든 기기에서 실행

    @rpc(Server, Reliable) fn RequestReload() {               // full 프로필
        if (Reserve == 0) { return; }                         // 서버에서 다시 검증
        Reload(Caller);                                       // Caller: 호출한 connection
    }

    on ReloadKey.Triggered() { Server.RequestReload(); }        // 호출 위치에 원격 호출이 드러난다
}
~~~

- **복제 필드(core):** `@net` 필드는 **권한 구역** 안에서만 쓸 수 있다(SD45). 권한 구역은 `if (Owner.HasAuthority) { … }`(Actor 파생은 `HasAuthority`)의 참 분기, `@authorityOnly` 이벤트의 handler, 서버 RPC 본문, `on Disable`·`on Destroy` 안의 `HasAuthority` 분기다. 권한 구역 밖의 쓰기는 컴파일 오류이며 `if (Owner.HasAuthority)` 감싸기를 수정 예로 제시한다. 실행 시 권한이 없는데 쓰면(권한이 바뀐 경우) 쓰기를 무시하고 위치별 1회 보고한다(fault 아님, §8.4). `@authorityOnly event`는 권한 측에서만 `emit`할 수 있고 위반은 `ContractViolation` fault다. 허용 타입은 저장 가능 타입(§15)과 복제되는 객체 참조다. 활성화 전 적용(초기 복제·late join)은 `Changed`를 발행하지 않으므로 `on Enable`에서 반영한다.
- **RPC(full):** 자기 객체의 RPC만 `Server.`·`AllClients.`·`OwnerClient.` 접두로 호출하며 static manifest(RpcId)로 등록한다(전체 계획 §23.2). 서버 RPC 본문에는 문맥 이름 `Caller: Connection`(호출한 connection)이 주어진다. 객체 인자는 호출자의 interest set 안에 있어야 하며, 네트워크 소유 connection이 아닌 호출자의 RPC는 거부한다. 길이·유한성·enum 값을 검사하고 connection·RpcId별 빈도를 제한한다. 클라이언트가 보낸 `Soft<T>`는 자동 로드하지 않는다(전체 계획 §17.1·§17.3).
- 원격 client가 유발한 fault는 instance·session으로 확대하지 않는다. prediction·replay 대상은 `@deterministic`이다.

## 15. 저장·데이터·편집 연동

`Props/Chest.spark`:

~~~text
import Items.ItemData;

@tableRow(Asset("/Game/Data/LootTable"))
struct LootRow {
    var Item: Soft<ItemData>;
    var Weight: float;
}

class Chest : ScriptComponent {
    @edit var Lid: SceneComponent;                 // 같은 prefab 안의 필수 참조
    @edit var Loot: Row<LootRow> = Row("Chest.Common");
    @edit(Min: 0, Max: 10) var Uses: int = 3;
    @save @formerly("Opened") var IsOpened: bool;

    on Enable() { ApplyLid(); }                        // 로드된 상태 반영(활성화마다)
    on IsOpened.Changed() { ApplyLid(); }

    fn ApplyLid() {
        if (IsOpened) {
            Lid.RelativeRotation.Pitch = 110.0;            // 도
        } else {
            Lid.RelativeRotation.Pitch = 0.0;
        }
    }
}
~~~

- **저장 가능 타입:** `bool`·`int`·`float`·수학 타입·`string`·enum·`Tag`·`TagSet`·`Soft<T>`·`Row<T>`, 이들의 `T?`, 그리고 이들로만 구성된 struct·컨테이너. 엔진 객체(자기 component 포함)·로드한 에셋 값·`Text`·`Result`는 저장할 수 없다(전체 계획 §7.2). C++ 대응(리플렉션 B9 — ReflectionExtensions 19절): `Tag` → Name, `TagSet` → `OrderedSet<Name>`, `Row<T>` → Name, `T?`(단일 값·문자열·struct) → `Optional<T>`, `Tag?`·`Row<T>?`는 None Name(없음은 하나 — SD19).
- **호환성:** 매 컴파일 검사. 필드 추가는 기본값(또는 zero value), 삭제는 `retire`, 이름 변경은 `@formerly`, 무손실이 아닌 형식 변경은 migration 함수. enum은 case 이름으로 저장하므로 case 추가 위치는 자유이고 삭제는 `retire`다.
- **로드:** 무손실 변환이 가능하면 변환하고, 불가능하거나 알 수 없는 enum 값이면 기본값을 쓰고 보고한다. 클래스를 찾지 못하면 원래 필드를 보존하는 placeholder로 로드한다. 저장 파일에도 RPC와 같은 길이·값 검사를 적용한다.
- **범위 attribute:** `@edit(Min, Max)`는 편집기 입력 범위이자 실행 중 계약이다. 범위를 벗어난 값을 쓰면 `AssertFailed` fault이고, 저장 로드에서 범위를 벗어나면 가장 가까운 경계로 고치고 보고한다.
- **데이터 에셋:** `DataAsset`과 `@tableRow` struct는 실행 중 읽기 전용이다. `@edit` 배열 원소에는 ElementGuid가 없다(Prefab D4). 원소를 식별해 override해야 하는 데이터는 key 컨테이너(Map·Set) 또는 struct 안의 ID 필드로 표현한다.
- **enum 저장:** case 이름으로 저장하므로 순서 변경에 안전하다. case 삭제는 `retire`, 이름 변경은 `@formerly`다.

## 16. Verse 검토와 차용

Epic의 "The road to UE6"(2026-06-17)는 게임플레이 프로그래밍 모델의 Verse 전환을 밝히고, Actor·Blueprint는 초기 UE6에 남았다가 새 framework가 성숙하면 deprecate된다고 기재하였다. Blueprint 즉시 제거는 2차 보도다.[R18] Rocks의 Unreal 기준은 UE 5.8이며 Verse는 언어 설계의 참고 대상이다.

| Verse 기능 | 판정 | Sparks 반영 |
|---|---|---|
| failure context(인덱싱·map 조회) | 의미 차용 | 인덱스·키 읽기는 `T?`. `[]`/`()` 구분 문법은 미채택 |
| `OnBegin<suspends>` | 차용 | handler 안에서 바로 `await`. `Start`는 준비 대기만(SD12) |
| 구조적 동시성(`race` 패배 취소, `branch` 수명) | 차용 | `All`·`Race`, 비구조적 `spawn` 대응 없음 |
| effect specifier | 구조 차용 | 실행 문맥, 재정의는 문맥을 넓힐 수 없음 |
| persistable·publish 호환성 검사 | 차용 | 저장 가능 타입, 매 컴파일 호환성 검사 |
| `weak_map(player, t)` | 목적 차용 | 엔진 객체 원소·키의 자동 정리 |
| 읽기·쓰기 접근 분리 | 단순화 차용 | Sparks 필드는 밖에서 읽기만 |
| `message` 지역화 타입 | 차용 | 화면 표시 API는 `Text`만 |
| 평가 순서 명시, lambda 없음, option 타입 | 일치 | — |
| int / int → rational | 목적만 차용 | `int / int`는 오류, 몫(`Math.Div`)과 비율(`float(a) / float(b)`)을 구분해 쓴다(SD17) |
| 투기 실행·rollback | 보류 | 엔진 C++ 쪽 transactional 기반이 없고 전체 계획 §23.4가 외부 효과 자동 rollback을 하지 않음. UE6의 C++ transactionalize 진행 시 재검토 |
| `live` 변수, `[]`·`:=`·`set` 문법, 포착 불가 오류의 섬 전체 중지 | 미채택 | 미출시 기능, 평범한 문법 요구, 오류 영향 범위 축소 |

Verse의 기호가 많은 문법과 failure·동시성 개념에는 학습 비용이 있다.[R19][R20] Sparks는 C 계열 문법, `T?`, VS Code 디버거로 이 비용을 낮춘다.

## 17. 스펙 개정 대상

### 17.1 전체 계획·입력 상세안·UI 툴킷(초안 1.2까지 반영)

개정 대상은 전체 계획, 입력 상세안, UI 툴킷, Core 구현 코드다. 위치 열의 절 번호는 접두어가 없으면 전체 계획의 절이다. "현재 문구"는 전체 계획 개정 1.6 기준이며(개정 1.8 행은 1.7 기준), 문서 항목은 모두 전체 계획 개정 1.7·1.8, 입력 상세안, UI 툴킷에 반영하였다. 수명 단계 이름(BeginPlay 등)을 쓰는 전체 계획 §5.3과 입력 상세안 586행은 엔진 내부 단계 이름이므로 그대로 두고 전체 계획 §20.2의 대응표로 연결한다. Core 구현 코드(C1–C5)는 도입 단계 L1의 구현 대상이다.

| 위치 | 현재 문구 | 이 설계 |
|---|---|---|
| §23.1 이름·예시·범위 | 이름 미정, `ObjectRef`·`WeakObjectRef`, Result·generic·interface, 동적 eval 제외, LLVM 기반 AOT | Sparks, `T?` 단일 개념, 사용자 generic·closure 제외, interface full, eval target 정책, 스크립팅 backend 선택(VM 또는 typed IR → C++ AOT, SD31) |
| §23.2 VM·GC·coroutine | stack depth 검사, coroutine root 열거, 메모리 예산 | bank 검사, 스크립트 상태는 GC root 없음(약한 handle·lease), 논리 단위 메모리 예산, 구조적 동시성 |
| §23.1·§27.8 VM 방식·reload 목표 | VM 방식 미정, reload 시간 목표 | 레지스터 VM과 Scalar·Ref·Buf bank, reload 목표 I1 2 s(SD8) |
| §5.2·§20.2 파괴·수명 | RequestDestroy 즉시 전이, 수명은 component 기준, hook 이름 BeginPlay 등 | 스크립트 단계 중 파괴는 단계 끝 반영(SD3), 수명 hook을 service·widget controller에도 적용, Unity 체계 hook 이름과 C++ `On*` 가상 함수(SD9), BeginDestroy에서 Detach |
| §5.3, 입력 상세안 586행 | "optional integration Register / BeginPlay" | hook 대응표(§5.1)와 연결 |
| §23.2(§8.3 clock 대응) | Monotonic·Frame·Game·Fixed·UI | `Delay(s, Clock.Game\|Clock.UI)`, `NextTick`(가변 tick, 일시 정지 중 미재개), `NextFixedTick`(Fixed) |
| §9.2·§10.2·§10.3·§24.2 로딩 | LoadTicket·AssetLease·revoke·travel·bundle | 요청 handle·결과 값 보관, 대기자 분배, Spawn의 prefab lease, 외형 전용 stub, 필드 초기값 에셋은 prefab 의존, 엔진 관리 로딩 화면, Teleport 내부 prefetch |
| §23.4 native API 선언 | lane·최대 작업·취소 | + 스레드 안전성·결정성·부수 효과·MaySafepoint·권한·자원 정책·파괴 지연·고립 완료 경로 |
| §29.6 NL03·NL04·NL06 | PIE 1,000회 등 | NL09 신설(G1–G4 Sparks Gate), 무기한 재시도 대신 모듈 사용 중지·실패 보고 |
| UiToolkit CU2·CU3·CU5·CU8, CU9 신설 | UI 사용처 중심 | 언어 L1 선행. CU3은 `MulticastDelegate` 필드와 `FieldKind::Event`. CU9는 §10.3에서 정의 |
| 입력 상세안 §22.3 | subscription이 module/script lease 보유, VM reload 시 해제 | 연결 항목은 handler ID만 보유, Disable은 비활성 표시, 선언이 바뀌는 reload에서만 재구성 |
| Core `Delegate.h`·`NamePool.h`(구현) | 약한 바인딩은 `SharedPtr`만, TryAdd compact 없음 | C1–C4(C5 `TryFind`는 구현됨) |
| §23.3·§2.4 | Studio language service, 언어 경계 명명 보류 | LSP·DAP, 패키지 `Rocks`, 필드 첫 글자 대문자 변환 |
| §2.4 C++ 오버로드(개정 1.8) | 서로 다른 `ScriptName`이 있어야 노출 | 노출 함수의 오버로드에 서로 다른 `ScriptName`이 없으면 메타 컴파일 오류 |
| §5.2·§20.2·§23.2(개정 1.8) | 스크립트가 요청한 파괴만 지연, 연쇄 단계당 16회, Enable·Start fault는 instance 비활성화 | 스크립트 단계 동안의 모든 파괴 요청 지연, 인과 깊이 16, 격리, 종료 경로별 hook(SD20), 재진입 정책 표시(SD11) |
| §22.6 session 구성(개정 1.8) | VM globals·coroutine·root | 스크립트 실행·연결 표·대기(전역 변수와 GC root 없음) |
| §23.4 conformance(개정 1.8) | 순환 import를 오류 사례로 열거 | 패키지 간 순환 의존(파일 간 순환 참조는 허용) |

### 17.2 초안 1.3이 요구하는 후속(다른 문서·코드)

이번 개정은 문서 작업이다. 아래 항목은 이 규격이 기대는 다른 문서·코드의 변경이며, 표시한 시점에 해당 작업에서 반영한다. Sparks 구현은 전체 계획의 순서(L0 = Foundation Gate 이후 M3)를 따른다.

| 대상 | 필요한 변경 | 시점 |
|---|---|---|
| **코드베이스: `WeakRef<T>` → `WeakObjectRef<T>`** | `ObjectRuntime/Public/Object/ObjectRefs.h`의 타입 이름, 메타 컴파일러의 타입 인식·오류 문구, 시험, policy, 구현 문서(Docs/Implementation)의 `WeakRef` 표기, 그리고 다른 세션이 수정 중이던 CurveData 문서의 표기 1곳. 별칭 없이 바꾼다(SD23). 설계 문서 표기는 초안 1.3과 함께 바꾸었다 | 리플렉션 매크로 단축 개명과 묶어 사용자 착수 지시 때. **나중에 코드베이스에서도 고칠 것** |
| 전체 계획 §5.2·§9.2·§20.2–20.5·§23 | Entity → Actor, TransformComponent → SceneComponent, `OnAwake(ComponentContext&)` → 현재 시그니처, tick group 이름을 World tick phase로(ER 2절이 예고한 개정의 미반영분), §23의 도입 단계표(핫 리로드 L1–L3, AOT backend L3)·"LLVM 기반 AOT"를 스크립팅 backend 선택(SD31)으로, §29.6 NL09에 G5·G6 추가 | 다음 전체 계획 개정 |
| ER 7.2 | Component용 시작 준비 hook(`IsReadyToStart`), Sparks Start bridge 계약(SD24) | L2 전 |
| ER 7.4·7절 | 스크립트 단계 중 파괴·활성 요청의 bridge 큐 계약(SD25), 프레임 끝 정리 반복 상한, 격리 bridge 계약(§8.3) | L2 전 |
| ER 8절 | 입력 batch·재개 단계와 reload 경계를 실행할 phase 경계 hook(§5.4) | L1 전 |
| ObjectFoundations·GC 대규모 처리 | 회수 전 정확히 1회 실행되는 cleanup hook(GC 도달 불가 경로의 Detach, §5.1) | L1 전 |
| RCM·TypeRegistry | 같은 TypeId의 revision 공존·instance별 revision·retire(모듈 reload의 Busy 거부 대체), 스크립트 필드 동적 schema 등록(전체 계획 §23.4 "opaque script field storage" 구체화) | L1 전(본문 reload), L2 전(필드 migration) |
| Prefab | Sparks reload(R3)에 따른 subobject 표 변경 행, live instance의 subobject 추가·삭제 | L2 전 |
| 메타 컴파일러·StableIdManifest | Sparks 컴파일러의 두 번째 발급자 계약과 kind 확장(§12.2), `ScriptExtensible`·`ScriptName`·`ScriptWritable` 지정자, `TryX` 자동 이름(SD44), `NoFatalOnInput` 선언과 policy | L1 |
| UiToolkit CU1–CU9 | CU9 규격(§10.3) 반영, CU1 attribute 대응(§10.1), CU5 계산 속성 setter 통지(§10.2) | L1 |
| CompilerDiagnostics | 엔진 공통 진단 코드 등록부(Sparks S0100–S1199, 메타 컴파일러 RMC####, UI validate 코드) | L1 |
| ContentSigning 1절 | `ExternalModules=Signed` 예외와 모듈 서명 봉투(§12.7) | L3 |
| CVarSystem 14.5절 | Sparks 노출 문법(§10.4) 반영 | 다음 CVar 문서 개정 |
| Core `Delegate` | C1–C4 | L1 |
| 게임 타입 자리표시 | `Character`·`Collider`·`SphereCollider`·`SoundAsset`·`Sequence`·`DataAsset`·`ScriptComponent`·`WidgetController`·`PlaySession`의 Rocks 이름 확정(Rocks 고유 이름 원칙) | 해당 모듈 진입 시 |
| ID 개념 이름 | `SoftEntityId`·영속 Entity Guid·파생 Entity ID를 Actor 계열 이름으로 바꿀지 Prefab과 함께 결정 | Prefab PF 단계 |

## 18. 도입 단계

| 도입 단계 | 언어 기능 | 완료 기준(전체 계획 §23.1) |
|---|---|---|
| L0 · M3 | core·full 문법, 타입·참조·대입·숫자 규칙, hook·스크립트 단계·이벤트 전달 모델, 유지 자원, 소유·해제 연산, 예약 이름(부록 A), 표준 API(부록 B), 전달 수 상한·예산 값, CU9 규격, 진단 형식·코드 대역 — **초안 1.3에서 문서 규격 확정**. 남은 L0 작업은 conformance corpus 작성(§13.1) | Core/Object 계약에 맞는 언어 규칙 확정 |
| L1 · M3 | 컴파일러 라이브러리, verifier, interpreter, native call, 진단, Core C1–C4, LSP 기본(진단·의미 강조), **R1 본문 핫 리로드**(2 s 측정), CU9 구현 | 조건문·함수·객체 필드·배열·오류 처리와 바인딩 테스트, G5의 R1 행 |
| L2 · M3/M4 | 수명 hook, `on 경로.이벤트`·큐·`Changed`, async handler·`All`/`Race`, 유지 자원, `Load`·`LoadAll`·Teleport·Travel, ledger G1·G4, LSP 자동완성·빠른 수정, VS Code 확장, **R2·R3 핫 리로드**, 오류 방어 경계(§8.4), AOT 동기 함수와 차등 시험 틀(G6) | 상호작용·문·카메라·컷씬 trigger를 C++ 수정 없이 제작, G5의 R1–R3 행, G2의 reload 항목 |
| L3 · M5 | `@net`·RPC, 저장 호환성, 디버거(DAP), **R4 핫 리로드**, soak G2·G3, eval(`DevOnly`), **AOT backend 완성**(async 상태 기계, VM 혼합·reload·디버거, 크래시 역변환) | 취소·GC·replication·중단점·migration·누수 Gate 통합 검증, G5 전체, G6 전체 |
| L4 · M6 이후 | profiler UI, 최적화 IR, AOT profile 기반 최적화와 cold module VM 유지, target별 eval 정책 | hot function 비용과 마켓 규정 근거로 결정 |

## 19. 결정 사항

2026.11.12 검토에서 초안의 권고안 10건(SD1–SD10)을 채택하였고, 같은 날 4차 검증에서 도출된 11건(SD11–SD21)을 채택하였다. 2026.11.18 5차 검증(초안 1.3)에서 사용자 요구(핫 리로드 필수·오류 방어·선택형 AOT·`WeakObjectRef` 명칭)와 권장안으로 24건(SD22–SD45)을 확정하였다. SD4는 기본값만 확정하고 target별 값은 마켓 규정 검토 후 정한다. SD3·SD9·SD20은 엔진 수준 결정이며 전체 계획 개정 1.7·1.8의 §5.2·§20.2에 반영되어 있다.

- **SD1. 확장자:** 소스 `.spark`, 컴파일 모듈 `.sparkc`. `.sparkc`는 빌드 출력 폴더에만 생성한다. 초안의 `.sparks`는 언어 이름과 철자가 같아 혼동되므로 사용자 결정(2026-11-12)으로 `.sparkc`로 정정하였다.
- **SD2. C++ 리플렉션 필드 이름:** 첫 글자를 대문자로 바꾸고 끝의 `_`를 제거하여 노출하며, 그 밖의 `_`가 있으면 `ScriptName`이 필요하다(§10.3). 리플렉션 필드를 C++에서도 PascalCase로 선언하도록 규약을 개정하는 대안은 채택하지 않는다.
- **SD3. 파괴 요청의 스크립트 단계 끝 지연:** 스크립트 단계가 열려 있는 동안 Game lane에서 요청된 파괴(스크립트, 스크립트가 호출한 native API, 전달 순회가 호출한 C++ handler)를 그 단계가 끝날 때 반영한다(§4.3). 스크립트가 호출하는 native API가 이 규칙을 지키도록 binding schema에서 선언·검사한다(§10.2). 전체 계획 §5.2의 즉시 전이와 조정하는 엔진 수준 결정이다.
- **SD4. 런타임 eval의 target 정책:** 기본값만 정한다. 모든 target에서 출시 구성은 `Off`, 개발 구성은 `DevOnly`다. target별 실제 값은 마켓 규정 검토 후 정한다(§12.6, §18 L4).
- **SD5. 컴파일러·언어 서버 구현 언어:** C++ 라이브러리로 구현한다. 런타임 eval을 위해 출시 바이너리에 포함될 수 있어야 한다.
- **SD6. 저장 시 reload의 컴파일 경로:** Rocks Build의 Sparks compile action 단일 경로로 한다.
- **SD7. 출시 빌드의 로드 검증:** 예외 없이 항상 검증하며 끄는 설정을 두지 않는다.
- **SD8. VM 방식과 reload 시간 목표:** 레지스터 VM과 3개 bank(Scalar·Ref·Buf)로 하고, reload 시간 목표는 전체 계획 I1의 2 s로 한다(§17).
- **SD9. C++ 수명 hook 가상 함수 이름:** `OnAwake`·`OnStart`·`OnTick` 등 `On` 접두를 사용한다. 일반 `Start()` 메서드와의 충돌을 피하고 Sparks의 `on Start()`와 1:1로 대응한다. 전체 계획의 수명 단계 이름(Initialize·BeginPlay 등)은 내부 단계 이름으로 유지한다. 엔진 수준 결정이며 전체 계획 개정 1.7의 §5.2·§20.2에 반영되어 있다.
- **SD10. 비활성화 시 동작:** Disable이 실행 취소·유지 자원 해제·이벤트 수신 중지를 수행한다. Unity는 component 비활성화만으로 coroutine을 중지하지 않는다. "비활성 객체는 아무것도 하지 않는다"는 규칙이 누수 방지에 유리하여 Unity와 다르게 정한다.
- **SD11. 기다리는 handler의 재진입 정책:** `await`가 있는 이벤트 handler(Start 제외)는 `@skipWhileBusy`(실행 중 같은 발행자의 새 이벤트를 버림) 또는 `@restart`(취소 후 다시 시작)를 표시해야 하며 표시가 없으면 오류다. `try await` 한 줄을 추가하는 것만으로 이벤트가 조용히 버려지는 일을 방지한다(§6.1).
- **SD12. Start의 대기 범위:** Start는 준비 대기(`Load`·`LoadAll`·`Sequencer.Prepare`·`DataLayerReady`)만 기다린다. 시간·이벤트 대기는 Tick을 무기한 보류할 수 있으므로 오류다(§5.1).
- **SD13. 이벤트 대기의 결과:** `await X.Event`는 값을 반환하지 않는다. 발행자가 사라지면 대기는 완료되지 않은 채 끝나며, 단독 대기는 그 실행을 끝내고 `Race`에서는 그 인자를 완료되지 않은 것으로 본다(§6.1).
- **SD14. 객체 원소 Array의 파괴 처리:** `Array`는 파괴된 원소의 자리를 유지하고 null로 읽으며 순회는 살아 있는 원소만 제공한다. 자동 제거는 `Set` 원소와 `Map` 키에만 적용하여 번호가 조용히 바뀌는 일을 없앤다(§4.3).
- **SD15. enum 값 표기:** 식에서는 항상 타입 이름으로 한정한다(`Clock.Game`). 짧은 이름은 attribute가 정의한 인자에만 쓴다(§3.3).
- **SD16. 범위 표기:** `a..<b`(b 미포함)를 쓴다. 끝 포함 여부가 배경 언어마다 다르게 읽히는 `..`는 쓰지 않는다(§3.3).
- **SD17. 정수 나눗셈:** `int / int`는 오류다. 몫은 `Math.Div`, 비율은 `float(a) / float(b)`로 쓴다(§3.3). 2차 검증의 "`/`는 항상 `float`"를 대체한다.
- **SD18. 단위:** 스크립트의 값은 엔진 규약을 따르며 엔진 규약은 UE 5.8과 같다: 길이 cm, 질량 kg, 시간 초, `Rotator`(`Pitch`·`Yaw`·`Roll`) 도, 삼각함수·quaternion 축-각·각속도 라디안(각속도·토크 API는 `…InRadians`·`…InDegrees` 쌍). `Math.Radians(...)`·`Math.Degrees(...)`는 상수식에서 허용한다(§4.2·§4.5). 각도를 받는 native 함수·필드는 이름이나 타입으로 단위를 드러내고 언어 서버가 hover에 표시한다. **2026-11-17 번복:** 처음 결정은 미터·라디안이었고 "한 언어 안에 각도 단위 2개"를 피하려 했으나, 사용자 요구로 엔진 규약이 UE와 같은 cm·도(`Rotator`)·라디안(삼각함수)이 되어 이를 따른다. 두 각도 단위는 UE와 같이 API 이름·타입으로 구분한다.
- **SD19. 빈 Soft·Row 제거:** `Soft<T>`·`Row<T>`에는 빈 값이 없고 선택 값은 `Soft<T>?`·`Row<T>?`로 쓴다. "없음"은 `null` 하나다(§3.2).
- **SD20. 종료 경로별 hook:** `RequestDestroy`·부모 파괴·셀 unload·travel·PIE Stop·process 종료는 Disable(활성이면)·Destroy를 실행한 뒤 Detach한다. fault 격리와 GC 도달 불가 경로는 Disable을 실행하지 않는다(§5.1).
- **SD21. NextTick의 시계:** `NextTick()`은 게임 시계를 따라 일시 정지 중 재개하지 않으며 `Time.Delta`는 GameClock delta다. UI 시계의 다음 프레임 대기는 `Delay(0.0, Clock.UI)`다(§6.1).
- **SD22. 엔진 이름 정렬:** 게임 객체는 `Actor`(Entity 대체, ER 2절), 조종 대상은 `Pawn`, `Character`는 Pawn 파생 자리표시, 장면 component는 `SceneComponent`다. `WorldService`는 Object 기반 스크립트 공유 상태 타입이며 엔진 쪽 호스트는 `WorldSubsystem`이다. ID 개념(`SoftEntityId` 등)은 Prefab과 함께 결정할 때까지 유지한다.
- **SD23. 약한 객체 참조 이름:** `WeakRef<T>`를 `WeakObjectRef<T>`로 부른다(사용자 결정). `ObjectRef`·`WeakObjectRef`·`SoftObjectRef`로 가족을 맞추고 Core `WeakPtr`와 구분한다. 문서는 이번에 바꾸고 코드는 §17.2의 후속으로 바꾼다.
- **SD24. Start 시점:** C++ `OnStart`의 엔진 규칙(ER 7.2: 다음 프레임 시작 처리, `IsReadyToStart`)을 따르고, Sparks `on Start`는 그 시점에 시작하는 실행이다. 준비 대기 중에는 bridge가 Tick을 보류한다(§5.1).
- **SD25. 파괴·활성 전환의 두 시점:** 스크립트 단계 끝에 World API를 호출하는 적용 시점과, 실제 hook·Detach가 실행되는 프레임 끝 정리를 구분한다. 적용 이후 스크립트는 대상을 null로 읽는다(§4.3).
- **SD26. 프레임 순서:** Unity hook의 종류·의미를 따르되 프레임 안 순서는 엔진 World tick phase(UE tick group)를 따른다. FixedTick은 Tick 뒤이고 물리 이벤트는 프레임당 1 batch다(§5.4).
- **SD27. 상속:** core에서 Sparks 클래스는 상속할 수 없고 `override fn`은 엔진 가상 함수에만 쓴다. full의 `open class`·`open fn`만 Sparks 파생과 재정의를 허용한다(§3.2).
- **SD28. 핫 리로드 등급과 단계:** R1 본문 교체(L1), R2 제자리 migration·R3 instance 재생성(L2), R4 복제 schema 교체(L3). PIE 재시작은 엔진 C++ 변경과 패키지 의존 graph 변경에만 남긴다(§12.4).
- **SD29. reload 원자성:** 계획·검증이 모두 끝난 변경만 패키지와 의존 패키지 단위로 한 번에 적용하고, 하나라도 실패하면 이전 revision을 유지한다. instance 단위 실패는 그 instance만 격리한다(§12.4).
- **SD30. 대기 중인 실행의 reload:** 자동 이전은 하지 않고 `ReloadExecutions: Keep | Cancel`(PIE 기본 `Cancel`)과 모듈별 살아 있는 revision 상한 8을 둔다(§12.4).
- **SD31. 스크립팅 backend 선택:** Unity Mono/IL2CPP처럼 target·빌드 구성마다 `Vm`·`Aot`를 고르는 옵션이며 기본은 `Vm`이다(사용자 결정: AOT는 반드시 제공하되 쓸지는 선택). VM은 항상 포함하고 두 backend의 관찰 가능한 결과는 같다. 예산은 bytecode 명령어 기준으로 센다(§12.5).
- **SD32. 오류 방어 경계:** 컴파일·로드·실행·native 경계·권한·폭주·편집기·저장 데이터·복구·원격 경계를 정하고, 노출 API는 입력 오류로 Fatal하지 않는다(§8.4).
- **SD33. 전달 수·큐 상한:** 스크립트 단계당 전달 항목 65,536, session 발행 큐 262,144. 스크립트 발행의 초과는 조용히 버리지 않고 `BudgetExceeded`, 엔진발은 합치고 집계한다. 이월 항목은 전달 직전에 재검증한다(§5.4).
- **SD34. 예산 기본값:** §9.3 표의 값을 프로젝트 설정으로 두고 쿡 시 고정한다. 실행 중 변경은 막는다(fault 결과의 기기 간 일치).
- **SD35. 입력 이벤트 이름:** 입력 상세안의 phase 이름(`Started`·`Triggered`·`Ongoing`·`Completed`·`Cancelled`)을 그대로 쓰고 별칭을 두지 않는다(§10.4).
- **SD36. 로그:** `Log.Info/Warning/Error/Debug`, 카테고리는 패키지에서 자동, 보간은 이름 있는 로그 인자로 낮춘다. 스크립트 메시지의 언어는 사용자 콘텐츠이므로 프로젝트 정책이다(§10.4).
- **SD37. 진단 형식:** `경로:줄:열: error S코드: 메시지`, 영어, 코드 대역 S01xx–S11xx. 지역화 설명은 LSP locale로 덧붙인다(§8.1).
- **SD38. 사용자 정의 오류 타입 제거:** 오류는 엔진 닫힌 enum `Error` 하나다. 단순성 원칙에 따라 full에서도 두지 않는다(§8.2).
- **SD39. `?? break`·`?? continue` 제거:** 지역 선언의 `?? return`만 남긴다(§3.3).
- **SD40. 객체 원소 Array의 개수:** `Count` 대신 `SlotCount`·`AliveCount()`를 노출해 "모두 파괴되었는데 Count가 0이 아님" 함정을 없앤다(§4.3).
- **SD41. `All`·`Race` 인자:** 결과가 `void`인 대기와 지역 변수에 담은 요청 handle만 허용해 결과가 조용히 버려지지 않게 한다(§6.1).
- **SD42. 반복 fault 판정:** 시간(초) 대신 프레임 수(600 프레임)로 세어 모든 플랫폼·재시뮬레이션에서 같게 한다(§8.3).
- **SD43. spawn 중 fault와 hook 안 `try`:** spawn 중 Enable fault도 Awake처럼 spawn 실패이며, Awake·Enable·Start 안의 `try` 실패는 그 hook의 fault다(§5.1·§8.2).
- **SD44. 노출 이름과 명령 표시:** `Result`를 돌려주는 C++ `TryX`는 Sparks에서 `X`로 노출하고, `@command`를 `@cheat`(CheatSet)와 `@editorButton`으로 나눈다(§10.3).
- **SD45. 권한 구역:** `@net` 쓰기는 흐름 분석으로 판정한 권한 구역에서만 컴파일되고, 실행 시 권한이 없으면 무시하고 보고한다. 서버에서만 발행하는 Sparks 이벤트는 `@authorityOnly`로 표시한다(§14).

## 20. 교차 검증 기록

초안 0.1·0.4·0.5·1.0·1.2는 이 문서의 작업 초안이다.

### 20.1 5차 검증(초안 1.2, 스펙 완비)

2026-11-18 사용자 지시로 구현 착수 전 문서만으로 규격을 완비하였다. 리뷰어 3명이 독립적으로 (1) 문서 내부 정합성·규격 완결성과 사용자 핵심 요구(핫 리로드·오류 방어·선택형 AOT), (2) 엔진 설계 문서·현재 코드(World·Actor·메타 컴파일러·Core)와의 정합성, (3) 새 주변 시스템 스펙(로깅·CVar·입력·UI·커브·물리·공간 인덱스·동기화·콘텐츠 서명)과의 정합성과 빠진 영역을 검토하였다. 사용자 결정은 `WeakObjectRef` 명칭(SD23), AOT는 IL2CPP식 선택 옵션(SD31), 이번 개정은 문서만(코드 후속은 §17.2)이며, 나머지는 권장안으로 확정하였다.

| 관점 | 주요 지적 | 조치 | 위치 |
|---|---|---|---|
| 엔진 이름 | Entity·TransformComponent·`OnAwake(ComponentContext&)`, 없는 지정자(`Edit`·`Replicated`·`ScriptExtensible`), C5 구현 여부 | SD22·SD23, §10.1 예시 재작성과 선행 표, C5 완료 표기 | 0·2·10·17 |
| 수명·시점 | Start 시점, 파괴·활성 전환 즉시 전이, World tick phase, Component Awake 실패 범위, BeginDestroy hook 부재, 엔진이 연 진입 | SD24·SD25·SD26·SD43, 엔진이 연 진입도 스크립트 단계, cleanup hook 선행 조건 | 4.3·5.1·5.4·17.2 |
| 내부 모순 | Sparks 클래스 상속, 이월 항목의 파괴된 인자, 필드 초기값 유지 자원, `Race`에서 진 요청, 클라이언트 전용 판정 근거, FunctionId 대상, 증분 컴파일의 효과, 같은 묶음 파괴의 격리, 단위·상수식 잔재 | SD27·SD33 재검증·SD41·SD45, 규칙 정정 | 3·4·5·6·7·12 |
| 미확정 값 | 전달 수 N·큐 상한, 예산, interface 문법, CU9, conformance, zero value, `await` 정지, 일시 정지, 문맥, `Error` 목록, `Assert` | SD33·SD34, EBNF·CU9 규격, 부록 B, §6.1 정지 지점, §13.1 | 3·5·6·8·9·10·13 |
| 핫 리로드 | L3로 늦음, PIE 재시작 범위, 재생성 경로, 보존 규칙, 오래 기다리는 실행, 롤백, 편집기 표시, 엔진 쪽 계약(TypeRegistry revision·동적 schema·subobject) | SD28–SD30, §12.4 재작성, G5, §17.2 | 12.4·9.3·18 |
| 오류 방어 | 클라이언트 `@net` 쓰기, 폭주, 편집기 문맥 fault, 진단 누락, 격리 복구, spawn 중 Enable, native Fatal, 로드 실패·컴파일 오류 상태 PIE, softlock | SD32·SD43·SD45, §8.4 신설 | 8·14 |
| AOT | 선택 옵션 설정 모델, 동등성 생성 규칙, 혼합·reload·디버거, 빌드·역변환, 실패와 fallback | SD31, §12.5 재작성, G6 | 12.5·9.3·18 |
| 주변 시스템 | 입력 phase, Log 모델, 진단 형식·등록부, `Loc` 인자, CVar·설정, `@command`, `Try` 접두, 커브·native struct, CU1, UI 수명, 기본값 순서, ElementGuid, 물리 `Hit`·질의, lane 해제 큐, 결정성, trace, 신뢰 경계, 패키지 manifest, 시험 | SD35–SD37·SD44, §10.4 신설, §6.3·§12.8·§13.1 신설 | 3·4·6·9·10·11·12·13·15 |

채택하지 않은 권고는 다음과 같다.

- **Tick 인자 `dt` 제거(`Time.Delta`로 통일):** 사용자가 제시한 표기(`on Tick(dt: float)`, §2.3)이므로 유지한다.
- **Tick 문맥의 `World.Spawn` 허용:** 금지를 유지하고 간접 생성은 생성 위치별 ledger·Spawn 상한으로 감시한다.
- **shipping 기본 AOT:** 사용자 결정(SD31)에 따라 기본은 VM이고 AOT는 선택이다.
- **`@pure`를 core로 이동:** 기능을 늘리지 않는 원칙에 따라 UI 변환 함수는 full로 둔다.

### 20.2 4차 검증(초안 1.0, 언어 모호성)

초안 1.0을 대상으로, 같은 규격을 읽은 두 구현자 또는 구현자와 사용자가 다른 결론에 이를 수 있는 지점을 6개 관점(구문, 이름 해석, 타입·값 의미, 실행·이벤트·수명 의미, 용어·서술, 비프로그래머 오독)에서 독립 검토하였다. 원보고 147건을 주제별 119개 항목으로 병합하였고, 동작이 달라지는 11건은 SD11–SD21로 결정하였다. 단위 규약(전체 계획 §2.2), 전체 계획의 오버로드·session·await 문구, UI 툴킷의 CU9 언급, Core BackendLease의 의미는 원문과 대조하였다.

| 원인 | 주요 지적 | 조치 | 위치 |
|---|---|---|---|
| 시간 단위 용어 미정의 | "단계"의 5가지 의미, "전달 단위"·연쇄 16회의 계수·"tick" 미정의, 프레임 안 스크립트 단계 순서 부재, `NextTick` 시계가 절마다 다름 | 용어 분리(프로필·도입 단계·스크립트 단계·수명 단계), 실행 조각 기준 Changed, 인과 깊이 상한, 프레임 단계 순서, SD21 | 1.3·5.3·5.4·6.1 |
| 이벤트 전달 | C++·Sparks handler 순서, 호출 대상의 확정 시점, 재진입 판정의 "발행자" 미정의, 조용히 버려지는 이벤트 | 한 항목의 연결 전체를 연결 순서로 실행, 전달 시점 기준, 경로별 발행자 표, SD11 | 5.2·5.4·6.1 |
| Changed | 쓰기 기준과 값 기준의 충돌, 저장 로드 발행 여부의 절 간 충돌, 경로 원천 교체 | 조각 시작·끝 값 비교, Start 전 적용만 미발행(이후 Enable 반환 기준으로 변경), 재연결 시 1회 전달, `Changed` 예약 | 5.3 |
| 수명·종료 | spawn 묶음·Enable 시점, "비활성화"의 3가지 의미, Detach 시점, Teardown 접근 범위, PIE Stop의 hook 미실행 | Spawn 안 Awake·Enable과 큐의 Start, "격리" 단일 상태, Destroy 반환 직후 Detach, SD12·SD20 | 5.1·6.2·8.3·9.1 |
| 이름 해석 | 가림 금지가 지역 변수에만 적용, 암묵 멤버 미예약, import 별칭 의미, 확장 이벤트 충돌, SD2 변환 충돌, Character의 `Owner` | 이름 하나에 대상 하나, 부록 A, 모듈 별칭, 확장 이벤트 유일성과 안정 ID, 메타 컴파일 오류, 기반 타입별 문맥 이름 | 3.1·3.4·3.7·5.2·10.3 |
| 타입·값 | await 이후 `T?` 적용 대상, 중첩 optional, `?.` 단락 범위, `== null` 좁히기, `==`·보간 허용 타입, `let` 깊이, 버린 결과, `try` 실패 범위 | 흐름 기준 규칙과 제외 목록, `?` 중첩 금지, 한 단계 단락, 닫힌 허용 목록, 깊은 `let`, Execution 트리 종료, SD13·SD14·SD19 | 3.2·3.3·4.2·4.3·8.2 |
| 구문·어휘 | `try`·`await` 적용 범위, `<` 판정, FloatLit 미정의로 `0..count` 오토큰화, 문자열 escape, 예약어 목록 부재, 미사용 인자를 피할 수 없음 | `Value` 문법, 계층형 EBNF, 어휘 규칙, `_` 인자, SD16 | 3.3·3.6·3.7 |
| 사용자 오독 | 지역 유지 자원이 즉시 멈춤, `int / int`, 예시의 cm·도 수치와 엔진 미터·라디안의 불일치(2026-11-17 엔진 규약이 cm·도가 되어 해소 — SD18), 일시 상태에 묶은 소리, 모든 collider에서 오는 Trigger | await 없는 지역 자원 오류, SD17·SD18, 예시 수정, `trigger` 인자 판정 | 2·4.2·7.1 |
| 문서 간 정합 | 오버로드 처리(노출 생략과 오류), session의 VM globals·root, 순환 import, CU9 정의 부재, LSP 도입 단계, Gate 번호 | 전체 계획·입력 상세안 정정, CU9 정의 | 10.3·13·17 |

에이전트 간 권고가 일치하지 않은 6건은 다음과 같이 판정하였다.

- **물리 이벤트의 재진입 발행자:** 자기 Actor가 아니라 상대 `other`로 하였다. 자기 Actor로 보면 대기 중 다른 Actor의 Enter가 버려지고 Exit만 전달되어 짝이 깨진다.
- **`?.`의 단락:** 사슬 전체가 아니라 한 단계만 단락한다. 기존 S0412 규칙으로 판정되며 새 규칙이 필요 없다.
- **Disable 시 연결:** 항목을 삭제하지 않고 비활성으로 둔다. 활성 전환을 반복해도 handler 순서가 유지된다.
- **`try` 실패와 PIE 정지:** PIE를 정지하지 않는다. 복구 가능한 실패는 `Result`라는 원칙을 따른다. 단, Start 안의 `try` 실패는 Start fault다(§8.2).
- **같은 필드 2회 쓰기 진단:** 오류로 하자는 제안은 정상 코드(`Hp -= x; if (Hp < 0) { Hp = 0; }`)를 오류로 만들므로 채택하지 않았고 예시 수정으로 대체하였다.
- **fault 이후 상태:** "중지"와 "격리"로 나누지 않고 "격리" 하나로 하였다. Tick fault의 Tick 중지(§8.3)는 instance 상태가 아니다.

반영 뒤 서브에이전트 2개로 내부 정합성(30건)과 반영 누락·문서 간 정합(25건)을 다시 검증하여 수정하였다. 이 과정에서 현재 상태 반영 위치를 Start에서 Enable로 옮겼고(비활성 동안의 변경을 놓치지 않도록), 문 예시의 회전 연출을 모든 기기의 Tick으로 옮겼으며, 프레임 순서를 전체 계획 §8.2·§20.2에 맞추었다. 사용자 결정으로 컴파일 모듈 확장자를 `.sparkc`로 정정하였다(SD1).

### 20.3 3차 검증(초안 0.5)

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
| 10 | 생성된 Actor가 prefab lease보다 오래 남음 | 로딩 검토 | Spawn이 prefab 의존 묶음 lease를 Actor에 귀속 | 7.6 |
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

### 20.4 1·2차 검증 요약

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
