# Sparks — Rocks 게임플레이 언어

정적 타입 · C 계열 문법 · 레지스터 VM · 비프로그래머 대상의 방어적 설계

**설계 기준일 2026.11.20 · 초안 1.5 · 상태: 설계 제안 / 미구현 · 성능 미측정**

이 문서는 [전체 계획](EngineImplementationPlan.ko.md) §23이 요구하는 Rocks 게임플레이 언어 Sparks를 정의한다. 프로그래머가 아닌 사용자도 쓸 수 있도록 Go처럼 단순하게, 실수할 여지가 적게, 메모리·리소스 누수가 생길 수 없는 구조로 설계한다. 기준은 엔진 구현 상태가 아니라 전체 계획 §23과 이 언어가 연결되는 전체 계획 §5·§6·§8·§9·§10·§16·§17·§20·§21·§24·§29의 계약이다. 이 문서의 문법·이름·API는 설계 제안이며 구현·측정 결과가 아니다. 예시의 엔진 API 이름(`AddMovement`, `World.Spawn` 등)은 바인딩 규칙을 보이기 위한 예시이며 실제 이름은 각 모듈의 C++ 선언에서 생성된다. 아직 엔진에 없는 게임 타입(`Character`, `Collider`·`SphereCollider`, `SoundAsset`, `Sequence`, `DataAsset`, `ScriptableComponent`, `WidgetController`, `PlaySession`)은 자리표시 이름이며 해당 모듈이 들어올 때 확정한다(§17.2).

초안 1.3은 구현 착수 전 스펙 완비를 위한 개정이다. 엔진 런타임(Actor·World 코드)·Prefab 개정 3·리플렉션 B9·주변 시스템 스펙(로깅·CVar·입력·UI·콘텐츠 서명)과 이름·시점을 맞추고, 사용자 핵심 요구 3가지 — **핫 리로드 필수(§12.4), 오류 방어(§8.4), 선택형 native AOT(§12.5, Unity IL2CPP와 같은 backend 선택)** — 를 규격으로 확정하였다. 미정 값(전달 수 상한·예산·interface·CU9)을 정하고 표준 API(부록 B)를 추가하였다. 구현 순서는 초안 1.5의 §18을 따른다.

초안 1.5(2026.11.20)는 구현 순서를 바꾼 개정이다. 엔진 요소와 무관한 언어 코어(LC0–LC9)를 먼저 구현하고 엔진 연동(EI1–EI9)은 엔진이 준비되는 대로 더한다(§18, SD51–SD53). 엔진·도구 코드에 필요한 사항은 [Sparks 엔진 연동 요구 목록](SparksEngineIntegration.ko.md)으로 분리하였다.

초안 1.4(2026.11.20)는 레벨마다 하나인 연결 스크립트(UE Level Blueprint 대응)를 기반 타입 `LevelScript`로 더한 개정이다. 정본은 [레벨 스크립트](LevelScript.ko.md)(결정 LD1–LD20)이고, 이 문서에는 언어 쪽 요지(기반 타입·참조 형태·수명 순서·엔진 Actor 물리 이벤트·편집기 명령·진단 코드)만 두며 결정은 SD46–SD50으로 요약한다.

| 항목 | 내용 |
|---|---|
| 기준 문서 | 전체 계획 §23 · [엔진 런타임](EngineRuntime.ko.md)(이하 ER) 2·6–8절과 World 코드 · [Prefab](Prefab.ko.md) 개정 3 · [레벨 스크립트](LevelScript.ko.md) 개정 1 · [런타임 클래스 메타데이터](RuntimeClassMetadata.ko.md)(이하 RCM) · [리플렉션 B9](ReflectionExtensions.ko.md)(이하 RX) · [입력 상세안](CoreObjectInputDetailedPlan.ko.md) · [UI 툴킷](UiToolkit.ko.md) CU1–CU9 · [로깅](LoggingSystem.ko.md) · [CVar](CVarSystem.ko.md) · [콘텐츠 서명](ContentSigning.ko.md) · [Clang 생성 Reflection](../Implementation/ReflectionMetadata.ko.md)(구현 상태) |
| Unity·Verse 비교 기준 | Unity 수명 hook 체계(종류·호출 순서·의미). Verse는 The Book of Verse[R19] 기준이며 언어 설계의 참고 대상이다. Rocks의 Unreal 기준은 UE 5.8(5.8.4, commit `f45729c0b6f0`) |
| Rocks 상태 | Sparks 언어·VM·컴파일러 미구현. Core C5(`NamePool::TryFind`)는 구현됨 |
| 식별자 | 도입 단계 L0–L4, 언어 코어 마일스톤 LC0–LC9, 엔진 연동 단계 EI1–EI9(요구 항목 SE-*), Core 변경 C1–C5, 리플렉션 확장 CU9, Gate G1–G6, 결정 SD1–SD50, 진단 코드 S0100–S1199, 부록 A·B, 참고 자료 R1–R31 |

## 0. 핵심 결정

| 항목 | 결정 |
|---|---|
| 이름 | Sparks. 소스 `.spark`, 컴파일 모듈 `.sparkc`(SD1). 파일 경로가 import 경로다(`Weapons/Sword.spark` → `import Weapons.Sword;`). 이 경로가 엔진 공통 타입 도구 이름(asset 명령·클래스 참조 텍스트)이다([리플렉션 B9](ReflectionExtensions.ko.md) 5.2절). |
| 단순성 | 진단은 경고 없이 오류만 두고 모든 오류에 수정 예를 제시한다. 한 가지 일에는 한 가지 방법만 둔다. 실수가 잦은 구조는 언어 규칙으로 제거한다. 기능은 core 프로필과 full 프로필로 구분한다. |
| 문법 | C 계열이다. `var`/`let`, `fn Name(a: T): R`, 괄호로 감싼 조건식, `on` handler, `@attribute`, 범위 `0..<n`. 타입·멤버는 PascalCase로 C++ 이름과 같고, 내장 값 타입(`float`, `vec3`)과 지역 이름은 소문자로 시작한다. 한 위치에서 이름 하나는 대상 하나만 가리킨다. |
| 수명 hook | Unity의 hook 체계(종류·의미)를 따르고 이름은 Rocks 어휘를 사용한다: `Awake`, `Enable`, `Start`, `Tick`, `FixedTick`, `LateTick`, `Disable`, `Destroy`, 물리 `TriggerEnter` 등. 프레임 안 순서와 Start 시점은 엔진 World의 tick phase를 따른다(SD24·SD26). 종료 경로마다 실행하는 hook을 정한다(SD20). |
| 객체 참조 | "없음"은 하나다. 다른 Actor·Component는 `T?`로 받고 약한 handle로 저장하며 파괴되면 null로 읽힌다. 스크립트 단계(§5.4) 안의 파괴 요청은 그 단계가 끝날 때 World에 적용된다(SD3·SD25). 스크립트 상태는 GC root가 되지 않는다. 엔진 쪽 저장 표현은 같은 저장 단위 안 대상만 보존하는 `WeakObjectRef` 필드다(셀 사이 참조는 World의 영속 entity 참조 — RX 5.1절). |
| 이벤트 | `on 경로.이벤트(...)` 한 형태다. 명시 구독·closure가 없으며, 발행은 큐에 등록되어 현재 실행 조각(§5.4)이 끝난 뒤 같은 스크립트 단계 안에서 순서대로 전달된다. 기다리는 handler는 실행 중에 온 이벤트의 처리 방식을 표시한다(SD11). |
| 유지 자원 | 멈출 때까지 계속되는 자원(반복 사운드·입력 context·로딩 요청 등)은 필드나 지역 변수(`var`·`let`)에 담아야만 생성되고(로딩 요청은 지역 변수 전용), 블록이 끝나거나 다른 값을 넣거나 객체가 비활성화·종료되면 멈춘다. 복사하거나 다른 이름으로 옮길 수 없다. |
| 비동기 로딩 | `try await X.Load()` 하나로 로드하고, 상주는 결과 값을 보관하는 것으로 유지한다. 진행률은 지역 요청 handle로 조회한다. 이동·travel·준비 대기는 엔진이 prefetch와 로딩 화면을 관리한다. |
| 누수 | 소유 범위를 Session·Instance·Execution·ModuleHost로 고정하고, 범위 종료의 일괄 해제는 실패할 수 없는 3개 연산(EndExec·Detach·CloseSession)으로만 수행한다. 범위 종료마다 0을 검사하고 장시간 세션의 증가 추세를 Gate로 검사한다. |
| 실행 | 레지스터 VM(Scalar·Ref·Buf bank), bytecode 사전 컴파일(`.sparkc`), 스크립팅 backend 선택(VM 또는 native AOT = typed IR → C++, Unity의 Mono/IL2CPP 선택과 같은 맥락, target·빌드 구성별 옵션, SD31), 런타임 eval(target 정책, 기본 꺼짐), 컴파일러를 공유하는 LSP 언어 서버. |
| 핫 리로드 | 편집기·개발 빌드에서 저장하면 실행 중인 게임에 반영된다. 함수 본문과 필드·`on`(언어 코어 LC7)부터 component 구성·클래스 계층(연동 EI6), `@net`·`@rpc`(연동 EI7)까지 제자리 migration 또는 instance 재생성으로 처리하고, 계획·검증이 끝난 변경만 원자적으로 적용하며 실패하면 이전 revision을 유지한다. Tryout 재시작은 엔진 C++ 변경과 패키지 의존 graph 변경에만 남긴다(§12.4, SD28–SD30). |
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
| 핫 리로드 필수 | 등급별 reload(본문 → 제자리 migration → instance 재생성), 원자적 적용·롤백, 편집기 요약, 언어 코어 LC7부터 단계적 도입. | 12.4 |
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
        let moveInput = MoveAction.Value;            // vec3(2축 action은 X·Y)
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

class DoorLogic : ScriptableComponent {
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
| `class X : MonoBehaviour` | `class X : ScriptableComponent` | — |
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
| 내장 값 타입(닫힌 목록, 예약어) | 소문자 | `bool`, `int`, `float`, `int8`–`int64`, `uint8`–`uint64`, `float32`, `float64`, `string`, `vec2`–`vec4`, `quat`, `mat4` |
| 그 밖의 타입·멤버·이벤트·enum case·attribute 인자 이름·생성 인자 이름 | 대문자로 시작(PascalCase) | `DoorState.Open`, `MoveSpeed`, `@edit(Min: 0)`, `Radius:` |
| 지역 이름: 함수·handler 인자, `var`/`let` 지역 변수, `if (let)` binding, 반복 변수 | 소문자로 시작(lowerCamel) | `moveInput`, `dt` |

- 명명 규칙 위반은 오류다.
- **이름 하나에 대상 하나:** 한 위치에서 보이는 이름(지역 이름, 자기 멤버와 상속 멤버(엔진 C++ 포함), 문맥 이름(§3.4), 최상위 선언, import한 선언, prelude, 타입)이 둘 이상의 대상을 가리키면 나중에 도입된 사용자 선언의 위치에서 오류다. 그래서 이름을 찾는 우선순위가 없다. 값 이름(지역 이름·Sparks 멤버·문맥 이름) 사이에서는 대소문자만 달라도 오류다(`owner`와 `Owner`). 엔진 C++ 상속 멤버와는 철자가 정확히 같을 때만 충돌로 보므로, 엔진이 기반 타입에 멤버를 추가해도 사용자 지역 이름이 오류가 되지 않는다. 엔진 멤버 추가로 Sparks 멤버와 정확히 같은 이름이 생기면 언어 버전 변경처럼 그 선언 위치에서 오류와 이름 변경 빠른 수정을 제시한다. 지역 이름은 둘러싼 범위의 이름과만 비교하므로 끝난 형제 블록의 이름은 다시 쓸 수 있고, 타입 이름과는 대소문자만 달라도 된다(`let health = Target`).
- 멤버 이름은 그 위치에서 보이는 타입 이름과 같을 수 없다(`var Health: Health?`는 오류, `var HealthComp: Health?`). 문맥 이름·기반 타입 속성과 같은 이름의 엔진 타입(`World`, `Widget`)은 정적 멤버를 노출하지 않으므로 식에서는 항상 문맥 이름·속성이다.
- 한 타입의 필드·함수·이벤트는 이름 공간 하나를 공유하고 한 모듈의 최상위 선언도 이름 공간 하나다. 같은 이름은 오류다.
- 상속 멤버와 같은 이름의 선언은 오류다. 예외는 시그니처가 같은 재정의 가능 함수의 `override fn`이며, 재정의할 대상이 없는 `override`도 오류다.
- 키워드, 내장 타입, predeclared 이름(prelude·검증 리터럴·문맥 이름·RPC 접두·`Changed`)은 사용자 선언 이름으로 쓸 수 없다. hook 이름은 수명 클래스와 그 기반 타입의 멤버 이름으로만 쓸 수 없으며 struct 필드·enum case에는 쓸 수 있다. 전체 목록은 부록 A(§3.7)다.
- 오버로딩은 prelude 수학 함수(`Math.Min`·`Max`·`Abs`·`Clamp`·`Sign`·`Div`, 모든 숫자 타입 공용 — 부록 B)에만 있다. 인자는 모두 같은 숫자 타입이어야 하며 그 타입의 판을 고른다. 리터럴 인자는 다른 인자의 타입을 따르고, 모두 리터럴이면 기본 타입(`int`·`float`)이다. 맞는 판이 없으면 오류다.

### 3.2 선언과 기본값

| 선언 | 의미 | 예 |
|---|---|---|
| `class` | 엔진 수명 타입(`ScriptableComponent`, Actor·Pawn 파생 `Character`, Actor 파생 `LevelScript`, `WorldService`, `WidgetController`) 또는 `DataAsset`을 상속. 기반 타입이 수명과 문맥 이름을 정한다(§3.4). core에서 기반은 엔진 C++ 타입뿐이고 Sparks 클래스는 상속할 수 없다. full에서는 `open class`로 선언한 Sparks 클래스도 기반이 될 수 있다(SD27) | `class DoorLogic : ScriptableComponent { }` |
| `struct` | 값 타입. 상속 없음. 스크립트 데이터는 struct로 표현한다 | `struct HitInfo { var Point: vec3; var Damage: int = 1; }` |
| `enum` | 열거. 플래그 조합은 `Set<E>` | `enum DamageKind { Physical, Fire, Poison }` |
| `event` | 클래스 안의 멤버 이벤트, 최상위의 Actor 확장 이벤트·World 이벤트 | `event Died(killer: Actor?);` · `event Actor.Interacted(actor: Actor);` · `event World.GameStarted();` |
| `fn`, `async fn` | 함수와 기다리는 함수 | `fn Lerp(a: float, b: float, t: float): float` |
| `const` | 컴파일 시 상수. 타입은 `bool`·숫자 타입·`string`·enum·`Tag`·수학 타입. 값은 모듈 interface hash에 포함되므로 값이 바뀌면 쓰는 모듈이 다시 컴파일된다 | `const MaxStack: int = 99;` |
| `interface` (full) | 함수 시그니처만 가진 계약(§10.3 CU9) | `interface Damageable { fn ApplyDamage(amount: int); }` |

- **기본값(zero value):** 초기값이 없는 필드는 `0`, `0.0`, `false`, `""`, `null`, 빈 컨테이너, enum의 첫 case, 벡터 0, `quat`·`mat4`·`Transform`은 단위(UE `FQuat::Identity`·`FTransform::Identity`), `Rotator`는 (0, 0, 0), `Color`는 (0, 0, 0, 0), `Text`는 빈 Text로 시작한다. `Tag`·`InputAction`, `?` 없는 객체(엔진 `object`·Sparks class·interface)·에셋·`Soft<T>`·`Row<T>`·`Class<T>`·`CVarRef<T>`·`LoadRequest<T>`·`Result<T>`, 그리고 그런 필드를 초기값 없이 가진 struct(엔진 struct 포함)에는 기본값이 없으므로(`Tag?`의 없음이 None Name이기 때문 — §15) `@edit`로 지정하거나 초기값(소유 component 생성식, `Asset(...)`·`Row(...)` 리터럴)을 두어야 하며 지정이 누락되면 쿡 오류다(SD19). 필드 초기값은 클래스 기본값 레코드에 들어가므로, non-null 참조 초기값(`Asset(...)`·`Soft<T>`·`Row(...)`)은 레코드의 참조 의존 수집(대상 정체성 비교, override하지 않은 값의 레코드 참조를 패키지 의존에 포함)이, 소유 component 생성식은 default subobject가 선행 조건이며 둘 다 해당 도입 단계(L2) 전에 갖춘다([RuntimeClassMetadata](RuntimeClassMetadata.ko.md) '클래스 기본값 결정'). 선택 값은 `T?`로 선언한다. 지역 변수는 선언할 때 초기화해야 한다.
- **`?` 없는 객체 필드:** (a) 소유 component를 생성하는 `let` 필드와 (b) prefab 안의 대상을 가리키는 `@edit` 필드(component·Actor 파생 클래스)에만 허용한다. 그 밖(WorldService·WidgetController·DataAsset의 객체 필드, `@edit` 없는 필드, LevelScript의 `@edit` 객체 필드 — LevelScript는 prefab에 들어갈 수 없고 대상이 레벨 Actor라 (b)에 해당하지 않는다)은 컴파일 오류이며 `T?`를 제시한다(LevelScript의 경우는 S0413 — [LevelScript](LevelScript.ko.md) LD7).
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

- **숫자 타입(SD56, Go 수준):** 정수 `int8`·`int16`·`int32`·`int64`·`uint8`·`uint16`·`uint32`·`uint64`, 실수 `float32`·`float64`. 기본형 `int`는 `int64`, `float`는 `float64`의 다른 이름이다(같은 타입이며 변환이 필요 없다 — 비프로그래머는 대부분 `int`·`float`만 쓴다). 크기를 정한 타입은 엔진 데이터(C++ `int32`·`float` 필드 등)와 저장 크기를 맞출 때 쓴다.
- **변환은 명시적으로만(Go와 같다):** 서로 다른 숫자 타입은 섞어 계산·비교·대입할 수 없고 `int32(x)`·`float32(x)`·`uint8(x)`·`float(i)`처럼 타입 이름으로 바꾼다. 정수끼리의 변환과 정수 → 실수 변환은 값이 대상 타입에 정확히 들어가지 않으면 `IntegerOverflow` fault다(`uint8(300)`, 2^53을 넘는 `float(i)` 등 — Go처럼 잘라 내지 않는다, SD57). `float64` → `float32`는 최근접 짝수로 반올림하고 범위를 넘으면 `NonFinite`다. 실수 → 정수는 반올림 방식을 드러내는 `Math.FloorToInt`·`CeilToInt`·`RoundToInt`·`TruncToInt`(결과는 `int`, `RoundToInt`의 0.5는 0에서 먼 쪽, 범위 밖은 `IntegerOverflow`)로만 하고 다른 정수 타입은 그 결과를 다시 바꾼다. `Math.Floor` 등은 같은 실수 타입을 반환한다(부록 B).
- **숫자 리터럴은 타입이 없는 상수다(Go의 untyped constant):** 정수 리터럴과 실수 리터럴(단항 `-`·괄호·리터럴끼리의 상수식 포함)은 쓰는 자리의 타입을 따른다 — 타입이 명시된 선언·인자·반환·대입 우변, 다른 피연산자의 타입이 정해진 산술·비교. 정수 리터럴은 실수 자리에도 쓸 수 있지만 실수 리터럴은 정수 자리에 쓸 수 없다. 값이 그 타입에 들어가지 않으면 컴파일 오류다(S0109: `var b: uint8 = 300`, `var c: uint8 = -1`). 자리의 타입이 없으면(`var x = 1`) 정수 리터럴은 `int`, 실수 리터럴은 `float`이다. 이름 붙은 `const`는 선언한 타입을 가진다. 상수식의 0 나누기·overflow·NaN·Inf는 컴파일 오류다.
- **overflow(SD57):** 모든 정수 타입의 산술(`+`·`-`·`*`·단항 `-`·`%`·`Math.Div`)이 그 타입의 범위를 넘으면 `IntegerOverflow` fault다. 부호 없는 타입의 `0 - 1`도 fault다(조용히 돌아가지 않는다). 부호 없는 타입에 단항 `-`를 쓰면 컴파일 오류다. 모든 실수 타입의 NaN·Inf 결과는 `NonFinite` fault다.
- **나눗셈:** 정수 타입끼리의 `/`는 오류다(SD17 — 모든 정수 타입). 몫은 `Math.Div(a, b)`(내림, `a == Math.Div(a, b) * b + a % b`), 비율은 `float(a) / float(b)`로 쓴다. 실수 타입의 `/`는 같은 실수 타입을 반환한다. `%`는 정수 전용 내림 방식이며 정수 변수의 `/=`는 오류다.[R30]
- **비교:** `==`·`!=`는 `bool`, `int`, `string`(UTF-8 바이트 비교), enum, `Tag`, `InputAction`, `Soft<T>`·`Row<T>`(ID 비교), 객체, 그리고 이들의 `T?`에만 쓴다. 객체 비교는 파괴되었으면 null로 읽은 뒤의 동일성이며 한쪽이 다른 쪽의 기반 타입일 때만 쓸 수 있다. `float`·수학 타입·struct·`Text`·컨테이너·`Result`·에셋 값의 `==`는 오류이며 `float`·vector는 `Math.NearlyEqual`을 쓴다. `?` 없는 `T`를 `null`과 비교하는 것도 오류다. Map 키·Set 원소·switch 대상은 `==`를 쓸 수 있는 타입만 허용하고, case에는 상수식만 쓰며 같은 case의 중복은 오류다. enum switch는 모든 case를 처리하고 `default`가 없으며(엔진 enum `Error`는 case가 늘 수 있으므로 `default` 필수), 그 밖의 switch는 `default`가 필수다.
- **문자열:** `+`는 없고 보간 `"${x}"`만 사용한다. 보간이 없는 원문 그대로의 텍스트는 verbatim `@"..."`로 쓴다(§3.6). 보간할 수 있는 타입은 `bool`·`int`·`float`·`string`·enum(case 이름)·`Tag`·수학 타입이다. `float`는 다시 읽었을 때 같은 값이 되는 가장 짧은 10진 표기로 쓰고 정수값이면 끝에 `.0`을 표기한다. `T?`·객체·struct·컨테이너·`Text`의 보간은 오류다.
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
- **모듈 경로(LC2a 확정):** 패키지 루트 기준 경로를 `/`로 나눈 구간이다. 마지막 구간은 `.spark`로 끝나며 확장자를 뗀 것이 모듈 이름이다. 구간은 모두 영문 대문자로 시작하고 ASCII 영문자·숫자만 쓴다(`_`·공백·점 없음 — S1108). import 경로는 구간을 `.`로 이은 것이다(`Combat/Score.spark` → `Combat.Score`). 대소문자만 다른 두 경로는 S1109(폴더 구간끼리도 비교한다. ASCII 대소문자를 접어 같은 철자 무리에서 byte 순으로 처음인 철자는 그대로 두고, 그 밖의 철자를 쓰는 모듈 파일마다 하나씩 낸다), 같은 폴더의 `X.spark`와 폴더 `X/`는 S1110(위치는 `X.spark`)이며 셋 다 그 파일의 파일 전체 진단이다. S1109–S1111은 경로가 맞는(S1108이 아닌) 모듈만 본다.
- **패키지 manifest:** 패키지 루트의 `SparksPackage.json5`(파일 이름 규칙은 JSON5 2단계 결정을 따른다)는 `name`(PascalCase), `sparks`(언어 버전, 정수), `profile`(`core`·`full`), `dependencies`(의존 패키지 이름 목록, SD59), `services`(World마다 생성할 `WorldService` 클래스 목록), `logCategory`(생략 시 패키지 이름)를 가진다. 알 수 없는 key는 오류다. 형식(LC2a 확정)은 JSON5(Core `JsonSyntax::Json5` — 중복 key는 오류)의 객체 하나다.

  | key | 필수 | 값 |
  |---|---|---|
  | `name` | 예 | 패키지 이름. 영문 대문자로 시작하는 ASCII 영문자·숫자(`_` 없음, 56 byte 이하 — `logCategory`를 생략하면 이 이름이므로 그 상한과 같다). `Rocks`는 엔진 패키지 이름이라 쓸 수 없다 |
  | `sparks` | 예 | 언어 버전. JSON5 정수 값이어야 하고(10진·16진 — `0x1`도 된다. `1.0`·`1e0`처럼 소수·지수 표기는 정수가 아니어서 S1104) 지금은 `1`뿐이다(1이 아닌 정수는 S1105) |
  | `profile` | 예 | `"core"` 또는 `"full"`(§1.3) |
  | `dependencies` | 아니오 | 의존 패키지 이름의 배열. 중복·자기 자신·`Rocks`는 오류다(`Rocks`에는 언제나 의존한다). 호환 범위는 패키지 버전이 생기는 단계에서 더한다(SD59) |
  | `services` | 아니오 | `WorldService` 클래스 경로의 배열: 모듈 import 경로 뒤에 클래스 이름(`Combat.Score.ScoreKeeper`). 중복은 오류다. 그 클래스가 있고 `WorldService` 파생인지는 이름 해석(LC2b)이 검사한다 |
  | `logCategory` | 아니오 | 영문 대문자로 시작하는 ASCII 영문자·숫자 구간을 점으로 이은 이름, 7구간·56 byte 이하(엔진 카테고리 `Script.<logCategory>`가 LoggingSystem 6.2절 문법 — 8구간·63 byte — 에 들게). 생략하면 패키지 이름 |

  manifest 진단(S11xx)의 위치: JSON5 문법 오류는 그 오류 위치, 값 오류는 그 값의 시작, 알 수 없는 key와 key 이름 오류는 그 key 값의 시작(Core JSON은 key 위치를 따로 두지 않는다), 빠진 필수 key는 최상위 객체의 시작이다. 한 manifest의 오류는 모두 알린다(첫 오류에서 멈추지 않는다).

  **manifest 오류**는 S1101–S1105와, manifest 파일을 읽지 못하게 하는 원문 검사(S0101 잘못된 UTF-8·S0103 크기 초과 — 이때는 manifest를 읽지 않는다)다. S0102(외톨이 CR)는 그 자리를 알리고 그대로 읽으며 manifest 오류가 아니다. binding schema 파일도 같다(S0101·S0103이면 읽지 않고 그 파일은 무효다). 다른 key에 오류가 있어도 `name` 값 자체가 맞으면 그 패키지는 **이름이 있다**(S1112 비교와 의존 대상 찾기에 쓴다). 이름이 없는 패키지는 manifest를 읽지 못했거나 `name`이 없거나 틀린 것이다.

  manifest 오류가 있는 패키지와 이름이 겹친 뒤쪽 패키지(S1112)는 **무효**이며 모듈을 분석하지 않는다(모듈 경로 검사·구문 분석·import 해석 모두 — manifest가 정하는 `profile`·의존 없이는 검사 결과가 달라지므로). 무효인 의존 패키지와 없는 의존 패키지를 가리키는 import는 판정하지 않는다(이미 그 원인을 알렸다). `dependencies`에 없는 다른 패키지를 가리키는 import는 그 패키지가 무효여도 S0302다(고칠 것은 이쪽 manifest이므로).
- import는 그 모듈의 모든 최상위 선언을 짧은 이름으로 도입한다. 도입한 이름이 다른 import·현재 파일의 선언·prelude와 같으면 그 import 줄에서 오류다. `import Weapons.Sword as Blades;`처럼 모듈 별칭을 붙이면 그 모듈의 이름은 `Blades.Sword`로만 쓴다. 그 밖의 한정 경로(`Weapons.Sword.Sword`)는 없다.
- 의존 패키지의 모듈은 `import 패키지.경로;`로, 현재 패키지의 모듈은 패키지 이름 없이 import한다. 의존 패키지 이름은 현재 패키지의 최상위 폴더·최상위 모듈 이름과 같을 수 없다(아래 S1111).
- **import 해석(LC2a 확정):** import 경로의 첫 구간이 manifest `dependencies`에 있는 패키지 이름이면 나머지 구간이 그 패키지의 모듈 경로이고, 아니면 경로 전체가 현재 패키지의 모듈 경로다. 그래서 의존 패키지 이름(과 `Rocks`)은 현재 패키지의 최상위 폴더 이름·최상위 모듈 이름과 같을 수 없다(S1111 — 첫 구간의 뜻이 둘이 되지 않게. 위치는 manifest의 그 의존 항목이고, `Rocks`는 의존 항목이 없으므로 그 이름으로 시작하는 첫 모듈(import 경로 byte 순)의 파일 전체 진단이다). 해석 결과가 없는 경로는 S0301이다(첫 구간이 의존 패키지 이름뿐이면 "패키지는 모듈이 아니다"로, 현재 패키지 자신의 이름으로 시작하면 "같은 패키지의 모듈은 패키지 이름 없이 import한다"로 설명하고 이름을 뗀 경로가 모듈이면 그것을 제시한다). 첫 구간이 이 컴파일에 있는 다른 패키지 이름(이름이 있는 패키지 — 정확히 같은 철자)인데 `dependencies`에 없으면 S0302, 같은 모듈을 두 번 import하면(별칭 유무와 무관) 두 번째 줄부터 줄마다 S0303, 자기 모듈의 import는 줄마다 S0304, `import Rocks…`는 S0305다. import 진단의 위치는 import 경로의 시작이다. 경로에 구문 오류가 있거나 경로 토큰 안에서 시작하는 어휘 진단(S01xx — 예: `_`가 든 이름의 S0105)이 있는 import와 선언 뒤의 import(S0210 — 구문 트리에 남지 않는다)는 판정하지 않는다(이미 그 원인을 알렸다).
- **`Rocks` 패키지(LC2a 확정, SD60):** 엔진 binding schema가 정의하는 `Rocks` 패키지의 모든 타입은 import 없이 보인다(§2 예시의 `Character`·`SphereCollider`·`SoundAsset`). 그래서 `Rocks`는 import하지 않으며, 사용자 이름과의 충돌은 이름 하나에 대상 하나 규칙(§3.1)이 판정한다(부록 A의 핵심 엔진 타입은 예약 이름이다).
- **패키지 사이 의존:** 한 컴파일에 함께 넣은 소스 패키지만 의존 대상이다(binding schema 패키지는 `Rocks`뿐이고 `Rocks`는 의존 목록에 쓰지 않는다). 의존 이름은 정확히 같은 철자의 이름이 있는 패키지 중 먼저 더한 것을 가리킨다.
  - 없는 패키지는 S1106(위치는 그 의존 항목)이다. 단, 이름이 없는 패키지가 하나라도 있으면 그것이 그 의존일 수 있으므로 S1106을 내지 않는다(그 manifest의 진단이 이미 있다).
  - 순환은 S1107이다. 유효한 패키지 사이의 의존 간선으로 강하게 연결된 묶음(SCC)을 찾아 묶음 하나에 진단 하나를 낸다. 위치는 묶음에서 이름 byte 순으로 처음인 패키지의 manifest에서 묶음 안을 가리키는 첫 의존 항목이고, 메시지는 그 항목에서 시작해 처음 패키지로 돌아오는 가장 짧은 길(간선은 manifest 순서로 넓이 우선)이다. 자기 자신 의존은 S1104이므로 묶음은 둘 이상이다.
  - 같은 이름의 패키지 둘은 S1112다. 이름이 있는 패키지끼리 ASCII 대소문자를 무시하고 비교한다(이름이 폴더·파일 이름이 되고 파일 시스템마다 대소문자 구분이 다르므로). 다른 manifest 오류와 무관하게 비교하며, 같은 무리에서 먼저 더한 패키지가 이름을 갖고 뒤의 것마다 그 `name` 값에 S1112를 내고 무효로 한다(먼저 더한 것이 무효여도 그렇다 — 한 이름이 둘을 가리키지 않게).
- 파일 사이의 순환 참조는 허용하고 순환하는 `const` 초기화만 오류다. 패키지는 빌드·로드·reload 단위이며 패키지 사이의 순환 의존은 금지한다.
- 엔진 API는 `Rocks` 패키지에 있고 핵심 타입과 prelude 이름(부록 A)은 자동으로 import한다.
- 변경 가능한 전역 변수는 없다. 공유 상태는 `class X : WorldService`로 선언하고 manifest `services`에 등록한다(전체 계획 §22.6 PI03). WorldService는 World마다 생성되는 Object 기반 스크립트 수명 타입이며, 엔진 쪽 호스트는 World의 `WorldSubsystem` 하나(ScriptServiceHost)가 맡는다(SD22). travel·World 종료 때 정상 종료된다. 접근은 `World.Service<T>(): T`이며 등록되지 않은 타입은 컴파일 오류다.

클래스 메서드에서 쓸 수 있는 **문맥 이름**은 기반 타입이 정하며 표에 없는 이름은 오류다. 문맥 이름은 `this`의 멤버가 아니다. component의 `Owner`(소유 Actor)와 WidgetController의 `Widget`(제어하는 widget instance)은 기반 타입의 읽기 전용 속성이므로 다른 상속 멤버처럼 이름으로 쓰고 다른 객체에서도 읽는다(`target.Owner`).

| 기반 타입 | 문맥 이름 |
|---|---|
| `ScriptableComponent`, `WidgetController`, Actor 파생(`Pawn`·`Character`·`LevelScript` 등) | `World`·`Session`·`Input`·`Time` |
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

대입 대상은 `Postfix`로 읽은 뒤 의미 규칙으로 제한한다. 맨 바깥 모양이 이름·멤버(`.`·`?.`)·원소(`[]`)가 아니면(`a + b`, `F()`, `(a)`, `this`, 리터럴) 구문 분석이 S0211로 막고, 그 경로 안에 함수 호출 결과·`?.`·`this.`가 들어간 것은 뒤 단계가 막는다(S0731). `??` 다음이 `return`이면 `Exit`로 해석한다. 식 안의 `<`는 `Ident` 바로 뒤에서 `Type { "," Type }`과 `>`가 이어지고 그다음 토큰이 `(` 또는 `.`일 때만 타입 인자로 판정하며 그 밖에는 비교다(`<`가 128단계 넘게 중첩되면 판정을 멈추고 타입 인자로 본다 — 어느 쪽으로 읽어도 오류인 원문이며 그 분석은 깊이 상한 S0215에 걸린다). 쉼표로 나뉜 두 비교(`F(a < b, c > (d))`)는 괄호로 구분한다. 타입 인자를 닫는 위치의 `>=`는 `>`와 `=` 두 토큰으로 분리한다. 이름 지정 인자는 생성 호출과 attribute에서만 허용한다.

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
EnumDecl     = "enum" Ident "{" EnumCase { "," EnumCase } [ "," ] "}" ;   (* case는 하나 이상 *)
EnumCase     = { Attribute } Ident ;                                   (* `@formerly` 등 *)
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
Literal      = IntLit | FloatLit | StringLit | VerbatimLit | "true" | "false" | "null" ;
~~~

어휘:

~~~text
Ident        = Letter { Letter | Digit } ;                 (* ASCII, "_" 제외 *)
IntLit       = Decimal | ( "0x" | "0X" ) { "_" } HexDigit { [ "_" ] HexDigit }
             | ( "0b" | "0B" ) { "_" } BinDigit { [ "_" ] BinDigit } ;   (* 구분자는 여러 개 이어 써도 된다 *)
Decimal      = Digit { { "_" } Digit } ;
FloatLit     = Decimal "." Decimal [ Exp ] | "." Decimal [ Exp ] | Decimal Exp ;
Exp          = ( "e" | "E" ) [ "+" | "-" ] Decimal ;
StringLit    = '"' { Char | Escape | "${" Expr "}" } '"' ;
Escape       = "\" ( '"' | "\" | "n" | "t" | "$" | "u{" Hex { Hex } "}" ) ;
VerbatimLit  = '@"' { VerbatimChar | '""' } '"' ;                 (* escape·보간 없음, 여러 줄 가능 *)
~~~

- `Char`는 `"`·`\`·줄바꿈을 제외한 문자이며 단독 `$`는 `$` 문자다. `${`만 보간을 시작하고 문자 `${`는 `\${`로 쓴다. `${ }` 안에는 문자열 리터럴을 쓸 수 없다. `\u{}`는 16진 1–6자리다.
- **숫자 리터럴은 최신 C#의 형태를 따른다(SD54, 2026-11-20 사용자 결정).** 10진(`1_000_000`), 16진(`0xFF_FF`, `0x_FF`), 2진(`0b1010_0101`), 실수(`1.5`, `.5`, `1e3`, `1_000.5e-3`)를 쓴다. 자릿수 구분자 `_`는 숫자 사이에 몇 개든 둘 수 있고, 16진·2진은 접두사 바로 뒤에도 둘 수 있다. 숫자 끝, `.` 앞뒤, `e` 바로 뒤에는 둘 수 없다. 앞자리 0은 10진으로 읽는다(`007`은 7 — 8진 리터럴은 없다). 16진·2진은 정수 리터럴이다.
- **C#과 다른 점:** 접미사(`u`·`l`·`ul`·`f`·`d`·`m`)는 쓰지 않는다. 리터럴은 Go처럼 쓰는 자리의 타입을 따르므로(§3.3 "숫자 리터럴은 타입이 없는 상수다") 타입은 선언(`var x: uint8 = 1`)이나 변환(`float32(1.5)`)으로 정한다. 접미사를 쓰면 S0114로 이유와 고칠 형태를 보인다.
- 범위를 넘는 리터럴은 컴파일 오류이며 int64 최솟값은 단항 `-`와 결합할 때만 쓴다. 16진·2진도 int64 범위다(`0xFFFF_FFFF_FFFF_FFFF`를 -1로 읽지 않는다).
- 숫자 뒤의 `.`는 다음 문자가 숫자일 때만 `FloatLit`에 포함되므로 `0..<n`은 범위로 읽힌다. `1.`은 쓸 수 없다(C#과 같다).
- **verbatim 문자열(SD55, 2026-11-20 사용자 결정):** `@"..."`는 C#의 verbatim 문자열이다. `\`는 그대로 글자이고 escape가 없으며 `"`는 `""`로 쓴다. 보간 `${ }`도 하지 않는다(그대로 글자). 여러 줄에 걸칠 수 있고, 원문의 CRLF는 값에서 LF가 된다(저장소 줄 끝 설정과 무관하게 같은 값). 경로·정규식·여러 줄 텍스트에 쓴다. `@` 바로 뒤에 `"`가 올 때만 verbatim이며 그 밖의 `@`는 attribute다.

**interface·open(full):** interface에는 함수 시그니처만 두며 필드·이벤트·기본 구현이 없다. class만 구현하고 struct는 구현하지 않는다. 구현 함수는 같은 이름·시그니처의 `fn`이며 `override`를 쓰지 않는다. interface 타입은 참조로만 쓰고(`I?`, `as? I`, `Owner.Find<I>()`) `@save`·`@net`·`@edit` 필드에 둘 수 없다. 메서드 추가는 interface의 끝에만 허용한다(slot 호환, §10.3 CU9). `open class`만 Sparks 파생 클래스의 기반이 되고 `open fn`만 재정의된다. 우선순위(높은 것부터): `.` `?.` `()` `[]` → `as?` → 단항 `!` `-` → `*` `/` `%` → `+` `-` → `??`(오른쪽 결합) → 비교(결합 없음) → `==` `!=`(결합 없음) → `&&` → `||`. `??`가 비교보다 높으므로 `n?.Count ?? 0 > 5`는 `(n?.Count ?? 0) > 5`다. `?.`의 단락 범위는 바로 뒤의 이름과 그 이름에 붙은 타입 인자·호출 인자까지다(§4.3).

### 3.7 부록 A: 예약 이름

| 분류 | 이름 | 사용 제한 |
|---|---|---|
| 키워드(full 전용 포함, 항상 예약) | `import` `as` `class` `struct` `enum` `event` `fn` `async` `const` `var` `let` `if` `else` `for` `in` `while` `switch` `case` `default` `break` `continue` `return` `on` `emit` `await` `try` `override` `this` `true` `false` `null` `interface` `open` | 식별자로 쓸 수 없다 |
| 내장 타입 | `bool` `int` `float` `int8` `int16` `int32` `int64` `uint8` `uint16` `uint32` `uint64` `float32` `float64` `string` `vec2` `vec3` `vec4` `quat` `mat4`, `void`(`Result<void>`의 타입 인자 전용) | 식별자로 쓸 수 없다. 타입과 생성·변환 호출로만 쓴다 |
| prelude | `Log` `Assert` `Math` `Random` `Format` `Parse` `Delay` `NextTick` `NextFixedTick` `All` `Race` `Assets` `CVars` `Settings` `Clock` `Error` | 사용자 선언 이름으로 쓸 수 없다 |
| prelude 타입 | `Array` `Map` `Set` `Result` `Soft` `Row` `Text` `TagSet` `InputAction` `LoadRequest` `Connection` `PlaySession` `Class` `CVarRef` `Rotator` `Transform` `WorldPosition` `Color` `Hit` | 사용자 선언 이름으로 쓸 수 없다 |
| 핵심 엔진 타입(언어 버전 1에 고정) | `Actor` `Pawn` `Component` `SceneComponent` `ScriptableComponent` `LevelScript` `WorldService` `WidgetController` `Widget` `DataAsset` `Collider` `World` | 사용자 선언 이름으로 쓸 수 없다. 그 밖의 `Rocks` 패키지 타입은 이름 하나에 대상 하나 규칙(§3.1)으로만 충돌을 판정한다 |
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
| `Math`(모든 숫자 타입 공용 판) | `Min` `Max` `Abs` `Clamp(x, lo, hi)` `Sign` | §3.1 오버로딩 규칙(인자는 같은 타입). `Clamp`의 `lo > hi`는 `AssertFailed`. 부호 있는 타입의 최솟값에 대한 `Abs`는 `IntegerOverflow`. 부호 없는 타입에는 `Abs`·`Sign`이 없다 |
| `Math`(모든 정수 타입) | `Div(a, b)` | 내림 몫, `b == 0`은 `DivideByZero` |
| `Math`(`float`=`float64`; `float32`는 변환 후 사용 — Go의 math처럼) | `Floor` `Ceil` `Round` `Trunc`(→ float), `FloorToInt` `CeilToInt` `RoundToInt` `TruncToInt`(→ int), `Sqrt` `Pow` `Exp` `Ln` `Sin` `Cos` `Tan` `Asin` `Acos` `Atan` `Atan2(y, x)`, `Radians` `Degrees`, `Lerp(a, b, t)` `InverseLerp(a, b, v)`, `EaseIn` `EaseOut` `EaseInOut`(t를 [0, 1]로 clamp), `NearlyEqual(a, b, tolerance)`, 상수 `Math.Pi` | 삼각함수는 라디안. 결과가 NaN·Inf면 `NonFinite`(`Sqrt(-1.0)` 등). **초월 함수는 VM·AOT가 같은 Core Math 함수를 호출한다**(§12.5). 플랫폼 간 비트 동일성은 Core Math의 결정성 수준(CurveData D17과 같은 UE 수준 — Windows x64에서 UE와 같은 비트)을 따른다. `InverseLerp`의 `a == b`는 `DivideByZero` |
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
| 에셋 | `X.Load()` `Assets.LoadAll<T>(list)` `Assets.Find<T>(s)` | §7.3 |
| `Tag`·`TagSet` | `Tag.Find(s)`, `TagSet`: `Has` `HasAny(set)` `HasAll(set)` `Add` `Remove` | |
| 설정 | `CVars.Find<T>(s)`, `CVarRef<T>.Value`, `Settings.Get<T>()` | §10.4 |

#### 부록 B 시그니처(LC2c 확정, SD67)

위 표의 정확한 시그니처다. 컴파일러가 이 목록을 언어 소유 멤버 표로 그대로 가진다. 표기: `fn 이름(매개변수): 반환`(반환이 없으면 `void`), `let 이름: 타입`은 읽기 전용 속성, `var 이름: 타입`은 쓸 수 있는 필드, `static`은 타입 이름으로 부른다, `await`는 대기를 돌려준다(그 값의 타입은 `:` 뒤 — §3.10), `event`는 기다릴 수 있고 `on`으로 받는 이벤트다. `N`은 모든 숫자 타입(오버로딩, §3.1), `S`는 부호 있는 정수·실수 타입, `I`는 정수 타입, `V`는 `vec2`·`vec3`·`vec4`, `T`·`K`는 그 타입의 타입 인자다. `[==]`는 `==`를 쓸 수 있는 원소일 때만, `[<]`는 `<`를 쓸 수 있는 원소일 때만 있는 멤버다. `[객체]`는 원소가 객체 타입(엔진 `object`·Sparks class·interface — 파괴될 수 있는 것)일 때, `[값]`은 그 밖(값 타입·에셋)일 때만 있는 멤버다.

~~~text
namespace Math {
    fn Min(a: N, b: N): N            fn Max(a: N, b: N): N
    fn Abs(x: S): S                  fn Sign(x: S): S
    fn Clamp(x: N, lo: N, hi: N): N  fn Div(a: I, b: I): I
    fn Floor(x: float): float        fn Ceil(x: float): float
    fn Round(x: float): float        fn Trunc(x: float): float
    fn FloorToInt(x: float): int     fn CeilToInt(x: float): int
    fn RoundToInt(x: float): int     fn TruncToInt(x: float): int
    fn Sqrt(x: float): float         fn Pow(x: float, y: float): float
    fn Exp(x: float): float          fn Ln(x: float): float
    fn Sin(x: float): float          fn Cos(x: float): float          fn Tan(x: float): float
    fn Asin(x: float): float         fn Acos(x: float): float         fn Atan(x: float): float
    fn Atan2(y: float, x: float): float
    fn Radians(degrees: float): float   fn Degrees(radians: float): float
    fn Lerp(a: float, b: float, t: float): float
    fn InverseLerp(a: float, b: float, value: float): float
    fn EaseIn(t: float): float       fn EaseOut(t: float): float      fn EaseInOut(t: float): float
    fn NearlyEqual(a: float, b: float, tolerance: float): bool
    fn NearlyEqual(a: V, b: V, tolerance: float): bool
    fn Scale(a: V, b: V): V
    let Pi: float
}
vec2 { var X: float; var Y: float }     vec3 { …, var Z: float }     vec4 { …, var W: float }
V {                                     // vec2·vec3·vec4 공통, 생성 vec3(x, y, z)
    fn Dot(other: V): float             fn Length(): float           fn LengthSquared(): float
    fn Distance(other: V): float        fn Normalize(): V
}
vec3 { fn Cross(other: vec3): vec3 }
quat {                                  // 생성 quat(x, y, z, w)
    var X: float; var Y: float; var Z: float; var W: float
    fn Rotate(v: vec3): vec3            fn Inverse(): quat           fn ToRotator(): Rotator
    static fn FromAxisAngle(axis: vec3, radians: float): quat
}
Rotator {                               // 이름 지정 생성 Rotator(Pitch:, Yaw:, Roll:)
    var Pitch: float; var Yaw: float; var Roll: float
    fn ToQuat(): quat   fn Forward(): vec3   fn Right(): vec3   fn Up(): vec3
}
Transform {                             // 이름 지정 생성 Transform(Location:, Rotation:, Scale:)
    var Location: vec3; var Rotation: quat; var Scale: vec3
    fn TransformPosition(v: vec3): vec3      fn InverseTransformPosition(v: vec3): vec3
    fn TransformVector(v: vec3): vec3        fn ToMatrix(): mat4
}
mat4 { fn TransformPosition(v: vec3): vec3 }
Color { var R: float; var G: float; var B: float; var A: float }   // 이름 지정 생성
string {
    let Length: int                     fn CharCount(): int
    fn Contains(text: string): bool     fn StartsWith(text: string): bool   fn EndsWith(text: string): bool
    fn Find(text: string): int?         fn Slice(start: int, end: int): string
    fn Split(separator: string): Array<string>
    fn Trim(): string                   fn ToUpperAscii(): string            fn ToLowerAscii(): string
}
namespace Format {
    fn Fixed(value: float, decimals: int): string
    fn Padded(value: int, width: int): string
}
namespace Parse { fn Int(text: string): int?     fn Float(text: string): float? }
Array<T> {                              // [i] 읽기 T?, [i] = v
    fn Add(value: T)    fn Insert(index: int, value: T)    fn RemoveAt(index: int)
    fn Clear()          fn Reverse()
    [==] fn Contains(value: T): bool    [==] fn IndexOf(value: T): int?    [==] fn Remove(value: T): bool
    [<] fn Sort()
    [값] let Count: int
    [객체] let SlotCount: int    [객체] fn AliveCount(): int    [객체] fn Compact()
}
Map<K, V> {                             // [k] 읽기 V?, [k] = v
    let Count: int      fn ContainsKey(key: K): bool    fn Remove(key: K): bool
    fn Keys(): Array<K> fn Values(): Array<V>           fn Clear()
}
Set<T> {
    let Count: int      fn Add(value: T): bool    fn Remove(value: T): bool
    fn Contains(value: T): bool                   fn Clear()
}
Result<T> { let Value: T?  /* Result<void>에는 없다 */   let Error: Error? }
namespace Log {
    fn Info(message: string)  fn Warning(message: string)  fn Error(message: string)  fn Debug(message: string)
}
fn Assert(condition: bool, message: string)
namespace Random {
    fn Int(min: int, maxExclusive: int): int
    fn Float(min: float, max: float): float
    fn Chance(probability: float): bool
}
Time { let Delta: float; let FixedDelta: float; let UiDelta: float; let GameSeconds: float; let Frame: int }
enum Clock { Game, UI }
fn Delay(seconds: float, clock: Clock): await void
fn NextTick(): await void               fn NextFixedTick(): await void
fn All(waits...): await void            fn Race(waits...): await void    // 대기 1개 이상(§6.1)
namespace Assets {                      // T는 에셋 타입(아니면 S0411)
    fn LoadAll<T>(assets: Array<Soft<T>>): LoadRequest<Array<T>>
    fn Find<T>(path: string): Soft<T>?
}
Soft<T> { fn Load(): LoadRequest<T> }   // T는 에셋 타입
LoadRequest<T> { let IsDone: bool; let Progress: float }   // await 결과는 Result<T>
Tag { static fn Find(name: string): Tag? }
TagSet {                                // 생성 TagSet()(빈 집합)
    fn Has(tag: Tag): bool      fn HasAny(tags: TagSet): bool    fn HasAll(tags: TagSet): bool
    fn Add(tag: Tag)            fn Remove(tag: Tag)
}
InputAction {
    let Value: vec3             // UE FInputActionValue처럼 3축(1축·bool은 X, 2축은 X·Y)
    event Started   event Triggered   event Ongoing   event Completed   event Cancelled
}
Text { static fn Find(key: string): Text? }
namespace CVars { fn Find<T>(name: string): CVarRef<T>? }
CVarRef<T> { let Value: T }
namespace Settings { fn Get<T>(): T }
Class<T> { fn Create(): T }             // full, T는 class 타입
~~~

`Row<T>`의 조회(`T.FindRow`)와 `Input`·`World`·`Session`·`Caller`의 멤버는 엔진 binding schema가 정한다(문맥 이름의 타입은 §3.10). `WorldPosition`의 멤버는 그 규격을 정할 때 더한다.

### 3.9 이름 해석 세부(LC2b 확정)

이 절은 §3.1·§3.4의 이름 규칙을 컴파일러가 판정하는 방식으로 확정한다. 진단 코드는 §8.1 "이름 해석" 표다.

**범위.** LC2b는 다음을 해석·검사한다.

- 선언(모듈 최상위 class·struct·enum·interface·const·fn, 멤버 field·fn·event, enum case, interface 메서드, import 별칭, 매개변수, 지역 이름)의 명명·예약·중복·충돌·상속·`override`·기반 목록.
- 모든 타입 표기의 대상: 소스의 `Type`·기반 목록·`as?` 대상·식 안의 타입 인자(`Find<Pickup>`), binding schema의 타입 표기·`base`·`constraint`.
- 식의 단순 이름(`Identifier` 노드 — `.` 뒤가 아닌 이름)과 `on`·`emit` 경로의 첫 이름.
- manifest `services`.

뒤 단계로 넘기는 것: `.` 뒤 멤버와 호출의 이름 지정 인자 이름(수신자 타입이 필요하다 — LC2c), `?`·`void` 위치와 타입 인자 제약(LC2c), attribute 이름과 인자(attribute마다 정의한다 — `@net(OwnerOnly)`의 짧은 이름, LC3), 이름 하나짜리 `on X()`의 hook과 확장 이벤트(`event Actor.Interacted`) 선언(LC3), 프로필(LC2d), 사용하지 않는 이름(LC2d).

**이름의 출처(SD62).**

| 출처 | 이름 |
|---|---|
| 내장 타입(언어) | `bool` `int` `float` `int8`–`int64` `uint8`–`uint64` `float32` `float64` `string` `vec2` `vec3` `vec4` `quat` `mat4` `void` |
| prelude 타입(언어 소유) | 제네릭 `Array<T>` `Set<T>` `Map<K, V>` `Result<T>` `Soft<T>` `Row<T>` `Class<T>` `CVarRef<T>` `LoadRequest<T>`, 그 밖 `Text` `Tag` `TagSet` `InputAction` `Rotator` `Transform` `Color`, enum `Clock`(`Game`·`UI` — 타입이며 `Clock.Game`처럼 값으로도 쓴다). `WorldPosition`은 언어 소유 이름으로 예약만 한다(규격이 정해지면 이 표에 정의한다 — 연동 요구 SE-G13. 그때까지 쓰면 S0318) |
| prelude 값(언어 소유) | `Log` `Assert` `Math` `Random` `Format` `Parse` `Delay` `NextTick` `NextFixedTick` `All` `Race` `Assets` `CVars` `Settings`, 검증 리터럴 `Tag` `Action` `Asset` `Row` `Loc` `CVar`(`Tag`·`Row`는 타입이기도 하다) |
| 엔진 패키지 `Rocks`(binding schema) | schema의 모든 타입. 부록 A의 핵심 엔진 타입과 `Connection`·`PlaySession`·`Hit`·`Error`(닫힌 enum), 문맥 이름 `Input`의 타입 `InputSystem`(LC2c)은 schema가 제공한다 |
| 문맥 이름 | `World` `Session` `Input` `Time` `Caller` |
| RPC 접두 | `Server` `AllClients` `OwnerClient`(쓰임 규칙은 LC3) |

언어 소유 이름(내장 타입·prelude 타입·prelude 값)은 컴파일러의 표이며 그 멤버는 부록 B다(LC2c부터 쓴다). binding schema의 타입 이름은 언어 소유 이름일 수 없다(S1120). 부록 A의 "prelude"·"prelude 타입" 행은 사용자 선언을 막는 예약 목록이고 출처는 이 표가 정한다(`Clock`은 언어 enum 타입, `Error`·`Connection`·`PlaySession`·`Hit`는 schema 타입이다).

**보이는 이름.**

- 모든 위치: 내장 타입, prelude, 엔진 타입, 그 모듈의 최상위 선언, import한 선언. 별칭 없는 import는 대상 모듈의 최상위 선언 전부를 짧은 이름으로, 별칭 import는 별칭 이름 하나를 도입한다(대상 모듈의 선언은 `별칭.이름`). 확장 이벤트는 이름을 도입하지 않는다(대상 타입의 멤버다 — LC3). 같은 패키지의 다른 모듈도 import해야 보인다.
- class 본문(필드 초기값·함수·handler): 위에 더해 자기 멤버와 상속 멤버(엔진 기반 체인의 field·function·event와 Sparks 기반 클래스의 멤버).
- struct 본문(필드 초기값·함수): 위에 더해 자기 멤버.
- 함수·handler 본문: 위에 더해 매개변수와 둘러싼 블록의 지역 이름, 그리고 class 함수·handler에서는 쓸 수 있는 문맥 이름(아래). 지역 이름의 범위는 선언 문장 다음부터 그 블록 끝이며(초기값 안에서는 아직 보이지 않는다), `if (let x = e)`의 `x`는 then 블록, `for`의 binder는 몸통이다.
- enum case는 `E.Case`(LC2c)로만, interface 메서드는 멤버 접근으로만 보인다.

**문맥 이름의 쓸 수 있는 곳.** class 함수·handler 본문에서 기반 체인으로 정한다. `Actor`·`Component`·`WidgetController` 파생은 `World` `Session` `Input` `Time`, `WorldService` 파생은 `World` `Session` `Time`이다. 그 밖(`DataAsset` 파생 등)과 struct 함수·최상위 fn·필드 초기값·const 초기값에는 없다. `Caller`는 위의 문맥 이름이 있는 class의 함수 중 `@rpc` attribute의 첫 인자가 이름 `Server`인 함수 본문에서만 쓴다(구문 모양만 본다 — attribute 규칙은 LC3. struct·`DataAsset` 파생의 `@rpc(Server)` 함수에는 없다). 쓸 수 없는 곳의 문맥 이름은 S0321이다. 식의 `World`는 언제나 문맥 이름이며 엔진 타입 `World`가 아니다(§3.1).

**찾기.** 이름은 그 위치에서 보이는 이름 중 철자가 정확히 같은 대상을 가리킨다. 우선순위는 없다 — 대상이 둘이 되는 선언은 선언 검사(아래)가 막는다. 예외는 엔진끼리의 겹침뿐이다: 식에서는 상속 엔진 멤버와 문맥 이름이 같은 이름의 엔진 타입보다 앞서고(`Widget`·`World`), 타입 자리에서는 타입만 찾는다. 같은 철자의 대상이 둘 이상이면(그 충돌은 선언 검사가 이미 알렸다) 이름 진단을 받지 않은 쪽을 가리키고, 모두 받았으면 찾는 차례(지역 이름 — 나중에 선언한 것부터 → 자기 멤버 → 상속 멤버(가까운 기반부터) → 문맥 이름 → 모듈 최상위 선언 → import한 이름(먼저 들여온 것) → 엔진 타입 → 언어 이름)에서 앞의 것이다. 그래서 진단 하나 뒤에 같은 원인의 진단이 이어지지 않는다(예: 같은 블록에서 두 번 선언한 `x`의 쓰임은 첫 선언, 엔진 타입과 겹쳐 진단을 받은 최상위 선언의 이름은 엔진 타입).

**선언 검사.** 선언 이름 하나에 S03xx를 하나만 낸다. 아래 순서로 처음 걸리는 것 하나이고, 그 이름의 뒤 검사(`override`·interface 구현 검사 포함)는 하지 않는다(선언은 그대로 도입해 연쇄를 막는다). 충돌은 "나중에 도입된 사용자 선언"에 낸다(§3.1): 같은 공간 안은 원문 순서로 뒤의 것, 멤버·지역 이름은 둘러싼 이름보다 뒤, 최상위 선언은 엔진 타입보다 뒤다. 예외로 import가 낀 충돌은 고칠 곳이 그 import 줄이므로 언제나 import에 낸다(S0311).

1. **명명(S0306).** PascalCase(대문자로 시작): 타입·enum case·최상위 const·fn·멤버(field·fn·event)·interface 메서드·import 별칭. lowerCamel(소문자로 시작): 매개변수(함수·handler·이벤트·interface 메서드)·지역 이름·`if (let)` binder·`for` binder. 수정 예는 첫 글자의 대소문자만 바꾼 이름이며, 그것이 키워드나 예약 이름이면 바꿀 방향만 안내한다.
2. **예약(S0307).** 부록 A의 내장 타입·prelude·prelude 타입·핵심 엔진 타입·검증 리터럴·문맥 이름·RPC 접두·`Changed`와 철자가 정확히 같은 사용자 선언 이름(enum case·매개변수 포함 모든 선언).
3. **hook 이름(S0308).** 수명 클래스(기반 체인에 `Actor`·`Component`·`WorldService`·`WidgetController`가 있는 class)의 멤버 이름이 hook 이름(`Awake` `Enable` `Start` `Tick` `FixedTick` `LateTick` `Disable` `Destroy` `TriggerEnter` `TriggerExit` `CollisionEnter` `CollisionExit` `Validate`), 그 앞에 `On`을 붙인 이름, Unity hook 이름(`Update` `FixedUpdate` `LateUpdate`)과 정확히 같다. 수정 예는 `on` 형태다(`Update` → `on Tick()`).
4. **중복(S0309).** 같은 선언 공간(모듈 최상위, 한 타입의 멤버, 한 enum의 case, 한 interface의 메서드, 한 매개변수 목록, 한 블록의 지역 이름)에서 ASCII 대소문자를 무시하고 같은 이름이 다시 나오면 둘째부터 낸다(한 자리에서 쓴 철자끼리는 대소문자 차이도 실수로 본다).
5. **충돌(S0310·S0312·S0313).** 다른 출처의 보이는 이름과 비교한다. 값 이름(지역 이름·매개변수·Sparks 멤버·문맥 이름)끼리는 ASCII 대소문자를 무시하고, 그 밖(엔진 멤버·타입·최상위 선언·import한 선언·prelude)과는 철자가 정확히 같을 때만 충돌이다(§3.1). 쓸 수 없는 위치의 문맥 이름은 보이지 않으므로 충돌하지 않는다.
   - S0313: class 멤버가 상속 멤버와 같다(엔진 멤버는 정확히, Sparks 멤버는 대소문자 무시). 시그니처가 같은 재정의 가능 함수의 `override fn`은 예외다. 상속 대상이 재정의 가능하고 시그니처가 같으면 수정 예로 `override fn`을 제시한다.
   - S0312: class·struct 멤버 이름이 그 위치에서 보이는 타입 이름(모듈 최상위 타입·import한 타입·엔진 타입·prelude 타입)과 정확히 같다(`var Health: Health?`).
   - S0310: 그 밖의 충돌 — 최상위 선언과 엔진 타입, 멤버와 최상위 fn·const·import한 선언·별칭, 멤버·지역 이름과 문맥 이름(대소문자 무시), 지역 이름·매개변수와 둘러싼 지역 이름·매개변수·자기 멤버·Sparks 상속 멤버(대소문자 무시), class가 상속하는 멤버와 그 모듈의 최상위 선언(정확히 — 위치는 최상위 선언, class 안에서 그 이름이 둘을 가리키므로).
   - 범위: `if (let)` binder와 `for` binder는 몸통 블록을 둘러싼 범위에 있다(몸통 안의 같은 이름은 S0310). 함수·handler 매개변수 목록도 몸통을 둘러싼 범위다. 본문이 없는 매개변수 목록(이벤트·interface 메서드·확장 이벤트)은 이름이 본문에서 쓰이지 않으므로 1·2·4번만 검사한다(멤버·문맥 이름과 비교하지 않는다).
6. **import 충돌(S0311).** 별칭 없는 import가 도입한 이름이 앞선 import가 도입한 이름·현재 파일의 최상위 선언과 정확히 같거나, 별칭이 그것들·엔진 타입과 같거나, import한 이름·별칭이 그 모듈의 class가 상속하는 멤버와 같으면 그 import에 낸다(위치는 경로 시작, 별칭이면 별칭 이름). 겹치는 이름이 여럿이어도 import 줄당 하나이며 메시지는 원문 순서로 처음 겹친 이름이다. 대상 모듈 안에서 이미 오류인 이름(예약·엔진 타입 충돌)은 import에서 다시 알리지 않는다. 별칭의 명명·예약은 1·2번 규칙이다. S0303·S0304를 받은 import의 별칭은 검사하지 않고 그 모듈을 가리키게만 둔다(쓰임에 연쇄 진단이 없게).

**`override`·`open`.**

- `override fn`의 대상은 같은 이름(정확히)의 상속 함수다. 엔진 함수는 schema `overridable: true`인 것, Sparks 함수는 `open fn`·`override fn`이 재정의 가능하다. 대상이 없거나(struct·최상위 fn의 `override` 포함), 같은 이름의 상속 멤버가 함수가 아니거나 재정의할 수 없으면 S0314다.
- 시그니처는 매개변수 수, 각 매개변수 타입(이름은 무관 — 쓰지 않는 인자는 `_`), 반환 타입, `async` 여부가 모두 같아야 한다. 다르면 S0315다. 어느 쪽이든 타입을 해석하지 못한 매개변수·반환이 있으면 수·`async`를 포함해 아무것도 비교하지 않는다(그 타입의 진단이 이미 있다).
- `open fn`은 `open class`의 함수에만 쓴다. struct·최상위 fn·`open`이 아닌 class의 `open fn`은 S0316이다.

**기반 목록.** 첫 기반은 schema의 `object`·`asset` 타입 중 `scriptExtensible: true`인 것 또는 Sparks `open class`, 나머지 기반은 interface(schema `interface`·Sparks `interface`)이며 같은 interface를 두 번 쓸 수 없다. 어긋나면 그 기반 경로에 S0322다(기반 타입을 찾지 못하면 S0318, 타입이 아니면 S0319). Sparks 클래스 사이의 상속 순환은 S0323이며 순환 하나에 하나(순환에 든 class 중 패키지 이름 → 모듈 import 경로 → 원문 위치의 byte 순으로 처음인 것의 첫 기반)다. 기반이 될 수 없어 S0322를 받은 class(`open`이 아닌 class)는 기반이 없는 것으로 보므로 순환을 이루지 않는다. class는 기반 interface(와 그 schema `base` 체인 — 같은 interface는 한 번만 센다)의 모든 메서드를 같은 이름·시그니처의 함수(자기 함수 또는 상속 함수, `override` 없이)로 구현해야 하며, 없으면 class 이름에, 이름은 같지만 시그니처가 다르면 자기 함수면 그 함수 이름에, 상속 함수면 class 이름에 S0324다. 진단을 받은 interface 메서드와 이름 진단을 받은 구현 함수는 다시 알리지 않는다.

**타입 표기 해석.**

- 한 구간 이름은 타입 이름 공간(내장 타입·prelude 타입·엔진 타입·모듈 최상위 타입·import한 타입)에서 찾는다. 없으면 S0318이다. 같은 패키지와 의존 패키지에서 그 이름의 최상위 타입을 선언한 모듈이 하나뿐이면 수정 예는 그 import이고, 아니면 편집 거리 2 이하의 비슷한 타입 이름이다. 타입이 아닌 대상(prelude 값, 검증 리터럴 `Action`·`Asset`·`Loc`·`CVar`, 문맥 이름 `Session`·`Input`·`Time`·`Caller`, RPC 접두, 별칭, 최상위 const·fn, 멤버)이면 S0319다.
- 두 구간 경로는 첫 구간이 import 별칭일 때만 그 모듈의 최상위 타입이다. 첫 구간이 별칭이 아니면(`Weapons.Sword`) S0318이며 import한 선언은 짧은 이름으로 쓴다고 설명한다. 세 구간 이상도 S0318이다. 위치는 해석하지 못한 첫 구간이다.
- 제네릭 타입은 정한 수의 타입 인자(`Map` 2, 나머지 1)가 있어야 하고 제네릭이 아닌 타입에는 타입 인자가 없어야 한다. 어긋나면 그 `Type`의 시작에 S0320이다.
- 기반 목록과 `as?` 대상(`Path`)도 같은 규칙이다(타입 인자가 없다).
- binding schema 표기는 내장 타입·prelude 타입·`Rocks` schema 타입·그 함수의 타입 매개변수에서 찾는다. 못 찾거나 타입 인자 수가 틀리면 그 표기 값에 S1120이며, 그 멤버의 타입은 정해지지 않은 채 파일은 계속 쓴다(쓰는 쪽에서 연쇄 진단을 내지 않는다). `base`는 같은 kind 계열(`object` → `object`, `asset` → `asset`, `interface` → `interface`)의 schema 타입이어야 하고 순환이 없어야 하며, `constraint`는 `object`·`interface` schema 타입이어야 한다. 어긋나면 그 값에 S1120이다.

**단순 이름.** 식의 `Identifier`와 `on`·`emit` 경로의 첫 이름(`this` 제외)을 위 규칙으로 찾는다. 못 찾으면 S0317이다(수정 예는 타입과 같은 방식 — import 제안 또는 비슷한 이름, 별칭 import의 선언을 짧은 이름으로 쓰면 `별칭.이름`). 문맥 이름인데 그 위치에서 쓸 수 없으면 S0321이다. 이름 하나짜리 `on X()`의 `X`는 hook이며 찾지 않는다(LC3).

**연쇄 방지(SD64).** 이름 공간이 불완전하면 그 안의 S0317·S0318을 내지 않는다(그 이름이 빠진 선언에서 왔을 수 있기 때문이다). 그 밖의 진단은 낸다.

- 모듈: 그 모듈에 어휘·구문 진단(S01xx·S02xx — 선언을 잃지 않는 S0102·S0208도 단순함을 위해 포함한다)이 있다, 해석하지 못한 import(S0301·S0302·S0305이거나 판정하지 않은 import — S0303·S0304는 해석한 것으로 본다)가 있다, 직접 import한 모듈에 어휘·구문 진단이 있다(전이하지 않는다 — import한 모듈의 import 실패는 그 모듈의 선언을 없애지 않는다).
- class: 첫 기반을 해석하지 못했거나(S0318·S0319·S0322) 상속 순환(S0323)에 들었거나 Sparks 기반 클래스가 불완전하다. 이 class의 멤버 타입·본문에서는 S0317·S0318에 더해 기반에 기대는 검사를 하지 않는다: S0308(수명 클래스인지 모른다), S0313·S0314·S0315(상속 멤버를 모른다), S0324, S0321과 문맥 이름·상속 멤버와의 S0310.

**manifest `services`(S0325).** 항목의 마지막 구간을 뺀 경로가 같은 패키지 모듈의 import 경로이고, 마지막 구간이 그 모듈 최상위 class의 이름이며, 그 class의 기반 체인에 `WorldService`가 있어야 한다. 어긋나면 manifest의 그 항목에 S0325다. 그 모듈에 어휘·구문 진단이 있거나(선언이 빠졌을 수 있다) 그 class가 불완전하면(기반을 모른다) 내지 않는다. 해석하지 못한 import는 그 모듈의 선언을 없애지 않으므로 이유가 되지 않는다.

**이름 해석 덤프(SD58 확장, `AppendNameDump`).** 모듈 하나의 해석 결과를 결정적인 텍스트로 적는다(형식은 `SparksCompiler/Compilation.h`). corpus 사례 옆의 `<사례>.names`가 golden이다(§13.1). 선언 이름(`decl` 줄 — 최상위·멤버·case·별칭·매개변수·지역 이름), 해석한 자리(단순 이름·`on`·`emit` 경로 첫 이름), 타입 표기(`type` 줄)마다 한 줄이며 원문 위치 순서다. 대상은 선언 종류와 정규 이름(`패키지:모듈 경로.이름`, 엔진은 `Rocks.이름`)으로 적고, 지역 이름·매개변수는 선언 위치를 함께 적는다. 확장 이벤트는 매개변수 이름·타입만 LC2b가 검사하고(다른 매개변수 목록과 같다) 대상과 이벤트 이름은 LC3다.

### 3.10 타입 검사(LC2c 확정)

이 절은 §3.3·§4의 식·타입 규칙을 컴파일러가 판정하는 방식으로 확정한다. 진단 코드는 §8.1 "null·타입"(S04xx)·"숫자·상수식"(S05xx) 표와 S0326이다. LC2c는 식마다 타입을 정하고(기대 타입 전달 포함) 아래 규칙을 검사한다. 뒤로 넘기는 것: 흐름에 기대는 규칙(await 이후 `T?`, 미사용, 대입 가능 경로·`let` — LC2d), 실행 문맥·효과(await·emit·생성을 쓸 수 있는 곳 — LC3), 검증 리터럴의 사전 대조(태그·action·에셋·row·catalog·CVar 사전이 있어야 한다 — 연동 EI1·EI4), 프로필(LC2d).

**타입의 종류.** 값 타입(내장 타입, prelude 값 타입, struct, enum, 컨테이너·`Result`·`Soft`·`Row` 등 prelude 제네릭), 참조 타입(엔진 `object`·`asset`과 Sparks class·interface), 선택 타입 `T?`(T는 `?`가 아닌 타입), 그리고 식에만 있는 다섯 가지다.

| 식 전용 타입 | 뜻 | 쓰이는 곳 |
|---|---|---|
| 타입 없는 정수·실수 상수 | 숫자 리터럴과 그 상수식(Go의 untyped constant) | 기대 타입으로 바뀌고, 없으면 `int`·`float` |
| `null` | `null` 리터럴 | 기대 타입 `T?`로 바뀐다. 기대 타입이 없으면 S0417 |
| 대기 | `async fn` 호출, `Delay`·`NextTick`·`NextFixedTick`·`All`·`Race`, 이벤트(`X.Event`) | `await`의 피연산자와 `All`·`Race`의 인자로만 쓴다(그 밖은 S0422) |
| 타입 이름 | 식에 쓴 타입(`DoorState`, `Math`가 아닌 `vec3` 등) | 멤버 접근(enum case·static 함수)과 생성·변환 호출의 대상으로만 쓴다(그 밖은 S0405) |
| 이름 공간 | prelude 값(`Math`·`Log`·`Random`·`Format`·`Parse`·`Assets`·`CVars`·`Settings`)과 import 별칭 | 멤버 접근으로만 쓴다(S0405) |

`void`는 값이 아니다. `void` 결과를 값 자리에 쓰면 S0405이다. 로드 요청 `LoadRequest<T>`(`soft.Load()`·`Assets.LoadAll<T>(...)`)는 대기가 아니라 값이다: 지역 변수에 담아 진행률을 보고 `await`(결과 `Result<T>`)와 `All`·`Race`에 쓴다(§7.3).

**대입 가능(타입 A의 값을 타입 B 자리에).** 다음 중 하나일 때만이다. 그 밖은 S0401이다.

1. A와 B가 같은 타입이다(`int`와 `int64`는 같다).
2. A가 타입 없는 상수이고 아래 "숫자 상수" 규칙으로 B(또는 `B?`의 B)가 된다.
3. B가 `T?`이고 A가 `T`·`T?`로 대입 가능하거나 A가 `null`이다.
4. A·B가 참조 타입이고 A가 B의 파생(기반 체인·구현한 interface — schema `base`·Sparks 기반 목록)이다. 컨테이너·`Result`·`Soft` 등 제네릭의 타입 인자는 같아야 한다(무공변 — `Array<Derived>`는 `Array<Base>`가 아니다).
5. 검증 리터럴·컬렉션 리터럴은 아래 각 규칙대로 B에 맞춰진다.

숫자 사이에는 암묵 변환이 없다(SD56). `float32` 값을 `float` 자리에 쓰려면 `float(x)`다.

**기대 타입.** 다음 자리는 기대 타입을 아래로 전달한다: 타입을 쓴 지역·필드 선언의 초기값, const 초기값, 대입의 오른쪽(왼쪽의 타입), 호출 인자(매개변수 타입), 이름 지정 인자(그 필드 타입), `return`의 값(함수 반환 타입, `Result<T>`면 아래 반환 규칙), `emit` 인자(이벤트 매개변수 타입), 컬렉션 리터럴 원소(원소·key·값 타입), `??`의 오른쪽(왼쪽의 `T?`이면 `T`), 이항 산술·비교의 상대 피연산자(한쪽이 정해진 타입일 때), `switch` case 값(대상의 타입), 괄호 안. 기대 타입은 검사 순서를 바꾸지 않는다(왼쪽에서 오른쪽).

**숫자 상수(SD66).** 정수·실수 리터럴과 그 상수식(단항 `-`, 괄호, 상수끼리의 `+`·`-`·`*`·`%`, 실수 상수의 `/`)은 타입 없는 상수다.

- 타입 없는 정수 상수는 −2^63부터 2^64−1까지의 정확한 정수다. 계산 중 이 범위를 벗어나면 S0503이다. 타입 없는 실수 상수는 double로 계산하며 NaN·Inf가 되면 S0505다.
- 정해진 숫자 타입 자리에서 그 타입이 된다. 정수 상수는 정수·실수 타입 자리에 모두 쓸 수 있고 값이 그 타입에 정확히 들어가야 한다(정수는 범위, 실수는 float32·float64로 정확히 표현 — 그렇지 않으면 S0109). 실수 상수는 정수 자리에 쓸 수 없다(S0509). 실수 상수가 float32 자리에 오면 최근접 짝수로 반올림하며, 반올림 결과가 무한대면(절댓값이 2^128 − 2^103 이상) S0109다(`3.4028235e38`은 float32 최댓값으로 반올림되므로 들어간다).
- 자리의 타입이 없으면 정수 상수는 `int`, 실수 상수는 `float`가 된다(`var x = 1`).
- 정수 상수끼리의 `/`는 SD17대로 S0501이다. `%`의 오른쪽이나 `Math.Div`의 둘째 인자가 값 0인 정수 상수(타입 없는 상수식·`const` 이름)면 왼쪽이 상수가 아니어도 S0504다(위치는 그 0). 실수 `/`의 0은 오류가 아니다(IEEE 754).
- 이름 붙은 `const`는 선언 타입을 가진 정해진 상수다(타입 없는 상수가 아니다).
- 타입 없는 상수끼리의 비교는 둘 다 정수면 정수 비교, 하나라도 실수면 실수 비교다(`1 < 2.5`는 `bool`, `1 == 1.5`는 실수 `==`라 S0407).
- **정해진 상수 접기.** 피연산자가 모두 상수(타입 없는 상수·`const` 이름·enum case·`true`·`false`·보간 없는 문자열)인 정해진 타입의 식 — 단항 `-`·`!`, `+`·`-`·`*`·`%`, 실수 `/`, 비교, `&&`·`||`, 숫자 변환 `T(c)` — 은 컴파일 때 값이 정해진다. 정수 결과가 그 타입의 범위를 벗어나면 S0503, 실수 결과가 NaN·Inf면 S0505다. switch case 값과 0 나누기 판정은 이 값을 쓴다. const 초기값이 상수식이어야 한다는 규칙과 const 순환은 §3.11이다(S0510·S0511 — 값을 정하지 못한 const를 쓰는 자리는 더 진단하지 않는다).

**연산자.** 피연산자의 타입이 맞지 않으면 S0406이다(숫자 타입이 섞이면 S0507). 한쪽이 타입 없는 상수면 다른 쪽 타입으로 맞춘다. 다른 쪽이 숫자·벡터가 아닌 타입이면 맞추지 않고 상수를 `int`·`float`로 보아 연산자 규칙으로 판정한다(`"a" + 1`·`q * 2`·`mode < 1`은 S0406, `flag == 1`은 S0407). `T?` 값의 산술·순서 비교는 S0406이다(`??`·`if (let)`로 먼저 푼다).

| 연산 | 피연산자 | 결과 |
|---|---|---|
| `+` `-` `*` | 같은 숫자 타입 | 그 타입 |
| `/` | 같은 실수 타입(정수는 S0501) | 그 타입 |
| `%` | 같은 정수 타입 | 그 타입 |
| 단항 `-` | 부호 있는 정수·실수 타입(부호 없는 타입은 S0506), 벡터 | 그 타입 |
| `+` `-`(벡터) | 같은 `vec2`–`vec4` | 그 타입 |
| `*` `/`(벡터·실수) | `vecN * float`, `float * vecN`, `vecN / float` | `vecN` |
| `*`(합성) | `quat * quat`, `mat4 * mat4` | 같은 타입 |
| `<` `<=` `>` `>=` | 같은 숫자 타입, `string`(UTF-8 byte 순), 같은 enum(선언 순) | `bool` |
| `==` `!=` | §3.3 "비교"의 타입끼리(같은 타입, 또는 한쪽이 다른 쪽의 기반인 객체, `T?`와 `T`·`null`) — 그 밖은 S0407 | `bool` |
| `!` | `bool` | `bool` |
| `&&` `\|\|` | `bool` | `bool` |
| `??` | 왼쪽 `T?`(아니면 S0414), 오른쪽 `T`·`T?`·`null`·`Exit` | `T`, `T?`, `T?`, `T` |

문자열 `+`는 없다(S0406, 수정 예는 보간). 복합 대입 `x op= v`는 `x = x op v`의 연산 규칙을 따른다(`%=`는 없다 — §3.6 `CompoundOp`): 숫자 타입 `x`에는 같은 타입의 값(다른 숫자 타입은 S0507, 숫자가 아니면 S0406, 정수 `/=`는 S0501), 벡터는 `+=`·`-=`에 같은 벡터·`*=`·`/=`에 `float`, `quat`·`mat4`는 `*=`에 같은 타입이며 그 밖의 타입·연산은 S0406이다. `x`의 타입은 읽기 타입이므로 원소 대상 `a[i] op= v`는 `T?`라 S0406이다(`a[i] = (a[i] ?? 0) + v`로 쓴다).

**변환.** 숫자 타입 이름을 함수처럼 부른 `T(x)`(인자 하나, 위치 인자 — 인자 수가 다르면 S0403, 이름 지정 인자는 S0404)는 숫자 변환이다. x는 숫자 타입이나 타입 없는 상수여야 한다(그 밖은 S0508). 실수 → 정수 타입은 S0502이며 `Math.FloorToInt` 등을 제시한다. 상수(타입 없는 상수·정해진 상수)의 변환은 컴파일 때 판정한다(정수 값이 대상 범위 밖이면 S0109). 숫자가 아닌 내장 타입(`bool`·`string`)의 변환 호출은 없다(S0508).

**멤버 접근(`.`, S0326).** 수신자의 종류마다 찾는 곳이 다르며 없으면 S0326이다(비슷한 멤버 이름을 제시).

- 값(struct·enum 아닌 값 타입·prelude 값 타입): 그 타입의 필드·함수(부록 B, schema). 선택 타입 `T?`에 `.`을 쓰면 S0412이다(`?.`·`if (let)`·`??`를 제시).
- 참조: 그 타입과 기반 체인의 필드·함수·이벤트(Sparks 멤버 포함). 이벤트 `X.E`는 대기다.
- 타입 이름: enum case(`DoorState.Open`, 엔진 enum 포함)와 static 함수(schema `static`, 부록 B의 `Tag.Find` 등). 그 밖의 멤버는 S0425이다(인스턴스 멤버를 타입으로 부름). 거꾸로 인스턴스로 static 함수를 부르면 S0425이다.
- 이름 공간: prelude 이름 공간의 멤버(부록 B), 별칭은 그 모듈의 최상위 선언(타입이면 타입 이름, 함수·const면 그 값).
- 타입 없는 상수의 멤버는 그 기본 타입(`int`·`float`)의 멤버이고, `null`의 멤버는 S0417이다.
- `?.`: 수신자가 `T?` 값이어야 하고(값이 아니거나 — 이름 공간·타입 이름·별칭 — `?`가 없으면 S0414) 결과는 멤버 타입의 선택 타입이다(이미 `?`면 그대로, `void`면 `void`). `?.`로 부른 async 함수·`?.`로 얻은 이벤트는 수신자가 null이면 기다리지 않고 바로 끝나며 `await` 결과는 `R?`(`void`면 `void`)다.
- 문맥 이름의 타입: `World` → schema `World`, `Session` → schema `PlaySession`, `Input` → schema `InputSystem`, `Caller` → schema `Connection`, `Time` → 언어 타입 `Time`(부록 B).

**호출.**

- 함수 값(최상위 fn, 멤버 함수, static 함수, prelude 함수)의 호출은 위치 인자만 쓴다(이름 지정 인자는 S0404). 인자 수가 다르면 S0403, 인자 타입은 매개변수 타입에 대입 가능해야 한다(S0401). 함수가 아닌 것을 부르면 S0402이다.
- 제네릭 함수(schema `Find<T: Component>`, 부록 B의 `Settings.Get<T>`·`Assets.Find<T>` 등)는 타입 인자를 명시해야 하며(추론하지 않는다 — 빠지거나 수가 틀리면 S0411), 타입 인자는 제약을 지켜야 한다(schema 제약의 파생, `Assets.Find`·`Assets.LoadAll`의 T는 에셋 타입 — S0411). `CVars.Find<T>`·`Settings.Get<T>`·`Row<T>`의 T는 연동 단계에서 사전·설정 schema와 대조한다. 제네릭이 아닌 함수와 함수가 아닌 것(생성·변환의 타입 이름, 검증 리터럴, 값)에 타입 인자를 붙이면 S0320이다.
- `async fn`·대기를 돌려주는 호출의 결과는 대기다(`await` 없이 값으로 쓰면 S0422).
- 생성: struct·schema struct·component(`object` 중 `Component` 파생)·`Rotator`·`Color`·`Transform`의 타입 이름 호출은 이름 지정 인자만 쓴다(위치 인자는 S0404). 인자 이름은 그 타입의 필드여야 하고(S0326, 이름에) 중복은 S0404(뒤 이름에), 위치 인자는 S0404(호출 하나에 첫 위치 인자 한 곳), 읽기 전용 필드(Sparks `let`, 엔진 `access: read`)도 생성에서 정한다(생성은 처음 값이다). 인자에 진단이 없을 때만 기본값이 없는 필드(§3.2의 기본값 없는 타입, 초기값 없음 — component는 Sparks가 선언한 필드만)를 보며, 빠뜨린 필드를 선언 순서로 모아 호출에 S0404 하나를 낸다. `TagSet()`은 빈 집합을 만든다(필드 없음). 인자에 진단이 있으면 생성 결과는 해석하지 못한 것이다(연쇄 방지). schema struct는 `constructible: true`일 때만 생성한다(S0402). 그 밖의 참조 타입(Actor 등)은 생성 호출이 없다(S0402 — 생성은 `World.Spawn` 등 엔진 API). component 생성이 놓일 수 있는 자리(`let` 필드 초기값 — §4.5)는 LC3이 검사한다.
- `vec2`–`vec4`·`quat`의 생성은 위치 인자(성분 수만큼, 실수)다: 이름 지정 인자는 S0404(첫 것에), 그다음 성분 수가 다르면 S0403(호출에), 성분은 `float` 자리다(S0401). `mat4`의 직접 생성은 없다(S0402).
- `Math`의 자리표시 함수(`Min`·`Max`·`Abs`·`Clamp`·`Sign`·`Div`·`NearlyEqual`·`Scale` — 부록 B의 `N`·`S`·`I`·`V`)는 §3.1 오버로딩 규칙이다: 자리표시 인자를 모두 같은 타입으로 맞추고(상수는 다른 자리표시 인자의 타입), 모두 상수면 실수 상수가 있거나 자리표시가 `int`를 받지 않으면 `float`, 아니면 `int`다(`Math.NearlyEqual(1, 2, 0.1)`은 `float`). 맞는 판이 없으면 S0401이다. `Abs`·`Sign`은 부호 없는 타입에 없다(S0401).

**`this`.** class의 함수·handler·필드 초기값에서 그 class 타입의 값이다. struct·최상위 fn·const 초기값의 `this`는 S0423다. 자기 멤버를 `this.X`(괄호로 감싼 `(this).X`도)로 쓰면 S0423다(`on this.E`·`emit this.E`는 식이 아니다 — 쓸 수 있는 곳은 §3.3·LC3).

**선택 타입 연산(S0414).** `?.`·`??`의 왼쪽과 `if (let x = e)`의 `e`는 `T?`여야 한다. `if (let)`의 `e`가 `bool?`이면 S0414다(`e ?? false`·`e != null`을 제시, §4.3). `?` 없는 `T`와 `null`의 비교는 S0407이다.

**검증 리터럴(S0424).** `Tag`·`Action`·`Asset`·`Row`·`Loc`·`CVar`의 인자 수를 먼저 본다(`Loc`은 key 하나 이상, 나머지는 하나 — 아니면 S0403). 첫 인자는 보간 없는 문자열 리터럴이어야 한다. `Tag(...)` → `Tag`, `Action(...)` → `InputAction`, `Loc(key, 이름: 값...)` → `Text`(key 뒤는 이름 지정 인자 — 위치 인자는 첫 것에 S0404, 같은 이름은 뒤 것에 S0404, 값은 보간할 수 있는 타입 — S0419). `Asset(...)`은 기대 타입이 에셋 타입 `T`·`T?`이면 하드 참조, `Soft<T>`·`Soft<T>?`면 소프트 참조이고, `Row(...)`는 기대 타입 `Row<T>`·`Row<T>?`, `CVar(...)`는 `CVarRef<T>`가 있어야 한다(기대 타입이 없으면 S0417, 있지만 맞지 않는 종류면 S0401 — `let n: int = Asset("x")`). 사전과의 대조는 연동 단계다.

**컬렉션 리터럴(S0417·S0418).** 기대 타입 없는 배열 리터럴의 `null` 원소는 S0417이다(원소 타입은 `?`일 수 없다). `[...]`는 기대 타입이 `Array<T>`·`Set<T>`이면 원소 리터럴, `Map<K, V>`이면 `키: 값` 리터럴이며 원소마다 T(K·V)를 기대 타입으로 검사한다. 기대 타입이 없으면 원소가 하나 이상이고 모두 같은 타입일 때 `Array<그 타입>`이다(정수 상수와 실수 상수가 섞이면 `float` — §3.3, 그 밖에 타입이 다르면 S0418, 기반 타입으로 합치지 않는다). 빈 `[]`·Map 리터럴·`Set`은 기대 타입이 있어야 한다(S0417). 기대 타입이 컬렉션이 아니면 리터럴에 S0401이다(`let n: int = []`). 기대 타입 없이 정해진 타입의 원소가 있으면 타입 없는 상수 원소는 그 타입으로 맞추고(`[x, 1.5]`에서 `x: int`면 S0509) 숫자가 아닌 타입이면 S0418이다(`[s, 1]`). 원소 타입은 `?`일 수 없다(§4.2 — S0401).

**문자열 보간(S0419).** `${}` 안의 식은 `bool`·숫자 타입·`string`·enum·`Tag`·`vec2`–`vec4`·`quat`·`Rotator`·`Color`·`Transform` 타입이어야 한다. `T?`·객체·struct·컨테이너·`Text`는 S0419다.

**조건(S0408).** `if`·`while`의 조건, `!`·`&&`·`||`의 피연산자, `Assert`의 첫 인자는 `bool`이다(`bool?`도 아니다).

**`as?`(S0420).** 피연산자는 참조 타입(`T?` 포함), 대상은 class·interface 타입이며 결과는 대상의 `?`다. 대상이 피연산자 타입 자신이거나 그 기반이면(올림 변환) S0420이다. class끼리는 대상이 피연산자의 파생이어야 하고(관계없는 class는 S0420), interface가 끼면 관계없이 허용한다(실행 중 조회).

**`await`·`try`(S0421).** `await e`의 `e`는 대기여야 하고 결과는 그 값의 타입이다: `async fn`은 반환 타입, 로드 요청 `LoadRequest<T>`는 `Result<T>`, 그 밖의 대기는 `void`. `try e`의 `e`는 `Result<T>`(`try await`면 await 결과가 `Result<T>`)이며 결과는 `T`(`Result<void>`면 `void`)다. 둘을 쓸 수 있는 자리는 §3.3(구문, S0212)과 LC3(실행 문맥)이다.

**반환(S0409·S0410).**

- 반환 타입이 없거나 `void`인 함수·handler의 `return e;`는 S0410, 반환 타입이 있는 함수의 `return;`도 S0410이다. `Result<void>` 함수는 `return;`이 성공이다.
- `Result<T>` 함수의 `return e`는 `e`가 `T`에 대입 가능하면 성공, `Error` 값(엔진 enum `Error`)이면 실패다. 둘 다 아니면 S0401이다(`Result<void>` 함수의 `return 5;`도 S0401). `Result<T?>`는 S0401(§3.2)이다.
- 반환 타입이 있고 `Result<void>`가 아닌 함수는 본문이 "끝나는 문장"으로 끝나야 한다(S0409, 위치는 함수 본문의 닫는 `}`). 끝나는 문장: `return`, 마지막 문장이 끝나는 문장인 블록, `else`가 있고 두 갈래가 모두 끝나는 `if`, `default`가 있거나(enum이면 모든 case를 다룬) 모든 갈래가 끝나는 `switch`. `while`은 끝나는 문장이 아니다.

**인덱스(`a[i]`).** `Array<T>`는 `int` 위치, `Map<K, V>`는 `K` key를 받으며(S0401) 읽기 결과는 `T?`·`V?`, 쓰기 `a[i] = v`의 자리 타입은 `T`·`V`다. 수신자가 `T?`면 S0412, 인덱스가 없는 타입이면 S0406이다.

**emit.** `emit` 인자는 이벤트 매개변수 타입을 기대 타입으로 검사한다(인자 수가 다르면 S0403). 경로가 이벤트 이름 하나거나 `this.이벤트`(class 기반 체인의 이벤트 — Rules 단계는 §3.12의 확장 이벤트 규칙)일 때 이벤트를 정하고, 그 밖의 경로(확장 이벤트 대상 — LC3)는 인자의 이름만 검사한다(기대 타입을 모르는 자리 — 아래 연쇄 방지).

**반복(`for`, S0406·S0412).** `for (x in e)`의 `e`가 `Array<T>`·`Set<T>`이면 `x`는 `T`, `Map<K, V>`이면 key `K`다. 이름이 둘인 `for (a, b in e)`는 `Array<T>`면 `a`가 `int` 위치·`b`가 `T`, `Map<K, V>`면 `a`가 key `K`·`b`가 값 `V`이며 `Set`에 이름 둘은 S0406이다. `e`가 `T?`면 S0412, 그 밖의 타입은 S0406이다(`for`는 배열·Map·Set·범위만 돈다). 범위 `a..<b`는 정수 범위다: 시작을 먼저 검사하고 끝은 시작의 타입을 기대 타입으로 검사한다(둘 다 타입 없는 상수면 `int`, 시작만 상수면 끝의 타입). 정수 타입이 아니면 S0406, 끝이 시작의 타입에 대입 가능하지 않으면 S0401, 범위에 이름 둘은 S0406이다. 이름이 받은 타입은 본문에서 그 지역 이름의 타입이다.

**식 타입 덤프(SD58 확장, `AppendTypeDump`).** 모듈 하나의 식마다 정한 타입을 결정적인 텍스트로 적는다(형식은 `SparksCompiler/Compilation.h`). corpus 사례 옆의 `<사례>.types`가 golden이다(§13.1). 한 줄에 식 하나(노드 범위·노드 종류·정규 타입)다. 타입은 식 자신의 타입이다: 파생 값이 기반 자리에, `T`가 `T?` 자리에 놓여도 그대로이고, 자신의 타입이 없는 타입 없는 상수와 `null`만 그 자리의 타입을 받는다. 대입 대상은 쓰기 자리의 타입이다(원소 쓰기 `a[i] = v`는 `T`). 상수로 접힌 타입 없는 상수식은 바깥 식만 적는다. 값이 아닌 식(타입 이름·이름 공간·`void`·대기)과 해석하지 못한 식(자신이 진단을 받았거나 안쪽 진단 때문에 타입을 정하지 못한 식)은 적지 않는다. 자리에 맞지 않아 그 자리에서 진단(S0401 등)을 받은 값은 자신의 타입으로 적는다.

**switch(S0415·S0416).** 대상은 정수 타입·`bool`·`string`·enum(엔진 enum·`Clock` 포함) 타입이다(그 밖과 `T?`는 S0407 — `T?`는 `??`·`if (let)`로 먼저 푼다). case 값은 대상 타입의 상수다(enum case, 숫자·문자열·`bool` 상수, 정해진 상수 — 아니면 S0401). 같은 값의 case가 두 번 나오면 뒤 case 값에 S0416이다(문자열은 해석한 값으로 비교 — `"A"`와 `@"A"`·`"\u{41}"`은 같다). S0415의 위치는 빠진 case·`default` 없음이면 대상 식, 있으면 안 되는 `default`면 그 `default`다. enum 대상은 모든 case를 다뤄야 하며 `default`가 없다(S0415 — 빠진 case를 메시지에 적는다). 엔진 `Error`와 enum이 아닌 대상은 `default`가 있어야 한다(S0415).

**타입 인자 제약.** 컨테이너의 원소·key 타입과 `Result`·`Soft`·`Row`·`LoadRequest`·`CVarRef`·`Class`의 타입 인자는 `?`일 수 없다(S0401). `Map`의 key·`Set`의 원소는 `==`를 쓸 수 있는 타입이어야 한다(S0407). `void`는 반환 타입과 `Result<void>`의 타입 인자로만 쓴다(그 밖의 `void` 표기와 `void?`는 S0405). `Soft<T>`의 T는 에셋 타입, `Class<T>`의 T는 class 타입(Sparks class·엔진 `object`, `?` 없음)이어야 한다(S0411). 검사는 모든 타입 표기(매개변수·필드·지역·반환·이벤트 매개변수·식 안의 타입 인자·`as?` 대상)에 하며 위반은 그 타입 인자의 표기 자리에 하나다. `void` 반환 타입은 쓸 수 있다. `void`로 표기한 매개변수·필드·지역은 해석하지 못한 이름이다(연쇄 방지).

**값 struct의 순환(S0426).** struct는 필드를 값으로 담으므로 자신을 담을 수 없다. 필드 타입(`?`를 벗긴 것, 표기 없는 필드는 초기값에서 정한 타입)이 Sparks struct면 그 struct로 가는 간선이며, 간선이 순환을 이루면(자기 자신 포함, 다른 struct를 거쳐도) 순환 하나에 S0426 하나다. 위치는 순환에 든 struct 중 (패키지 이름, 모듈 import 경로, 원문 위치) byte 순으로 처음인 struct의, 순환 안의 struct를 가리키는 첫 필드의 타입 표기(표기가 없으면 필드 이름)다(S0323과 같은 순서). 컨테이너(`Array`·`Map`·`Set`)·그 밖의 제네릭과 참조 타입을 거치는 것은 순환이 아니다. 순환에 든 struct는 기본값 판정(§3.2)에서 기본값이 있는 것으로 본다(연쇄 방지). struct 기본값은 필드마다 초기값이 있거나 필드 타입에 기본값이 있을 때이며, 엔진 struct도 schema 필드로 같은 규칙을 쓴다(schema struct가 값으로 순환하면 — 생성된 schema에는 없는 손상 — 순환에 든 struct는 기본값이 있는 것으로 본다).

**연쇄 방지(LC2c).** 해석하지 못한 이름·타입(NoType)이 낀 식은 더 검사하지 않고 결과 타입도 해석하지 못한 것이다(S04xx·S05xx를 더 내지 않는다). 멤버 이름 공간이 불완전한 class(§3.9 — 기반 체인에 해석하지 못한 기반이 있다)의 멤버를 찾지 못한 멤버 접근·이름 지정 인자는 S0326 없이 해석하지 못한 것이다. 진단은 자리(피연산자·인자·원소·이름 지정 인자)마다 하나이며, 바깥 식은 안쪽 자리가 진단을 받았으면 그 결과를 해석하지 못한 것으로 보고 더 진단하지 않는다(`1 && 2`는 두 피연산자에 S0408 하나씩). 기대 타입이 앞선 오류로 정해지지 않은 자리(해석하지 못한 타입 표기·호출 대상·매개변수·필드·대입 대상·이벤트)는 기대 타입이 없는 자리와 다르다: `null`·`[]`·Map 리터럴·`Asset`·`Row`·`CVar(...)`에 S0417을 내지 않고 안의 이름만 검사하며, 타입 표기를 해석하지 못한 지역·필드는 초기값의 타입을 받지 않는다(해석하지 못한 이름). `void`(`void?` 포함)로 표기한 매개변수·필드·지역도 해석하지 못한 이름이다.

**초기값 깊이(S0427).** 필드·const의 타입·값은 초기값을 검사해 정하므로 초기값이 다른 필드·const를 따라간다. 이름(`X`, 별칭의 `Alias.X`)으로 가리키는 의존은 의존 순서대로 미리 정해 깊이를 쓰지 않는다. 그 밖(값의 멤버 등)으로 이어지는 연쇄가 64단계를 넘으면 넘은 초기값에 S0427 하나이며 그 필드·const는 해석하지 못한 것이다(악의적·생성된 원문이 컴파일러 stack을 넘치게 하지 못하게 — §8.1 S0215와 같은 이유).

### 3.11 흐름·대입·마무리(LC2d 확정)

이 절은 §1.3·§3.2·§3.3·§4.3–§4.5의 흐름·대입·미사용·상수식·필드·프로필 규칙을 컴파일러가 판정하는 방식으로 확정한다. 진단 코드는 §8.1의 S0428, S0510–S0512, S07xx, S1113이다. 뒤로 넘기는 것: attribute 이름·인자의 검사(이 절은 `@edit`가 있는지만 본다 — LC3), 실행 문맥·효과(await·emit을 쓸 수 있는 곳, 최상위 fn의 엔진 변경 API — LC3), 유지 자원을 `_ =`로 버리는 것의 금지(LC3), LevelScript의 `@edit` 객체 필드(S0413 — LC3), 권한 구역(LC3), component 생성이 놓이는 자리(LC3), `@edit` 지정 누락(쿡 단계 — SD19).

**대입 경로(S0701–S0706·S0731, SD73).** `x = v`·`x op= v`의 대상과, 값 타입 경로를 바꾸는 변경 메서드 호출(부록 B: Array의 `Add`·`Insert`·`RemoveAt`·`Clear`·`Reverse`·`Remove`·`Sort`·`Compact`, Map의 `Remove`·`Clear`, Set의 `Add`·`Remove`·`Clear`, TagSet의 `Add`·`Remove`)의 수신자를 경로로 본다. 경로는 뿌리(이름, `this`를 생략한 자기 필드) 뒤에 멤버(`.`)·원소(`[]`)가 이어진 것이며, 뿌리에서 끝으로 보아 처음 어긋난 곳의 진단 하나를 낸다. 위치는 대입 대상(변경 메서드 호출은 그 호출)의 시작이다.

- 경로에 호출 결과나 `?.`가 있으면 S0731이다(`GetTransform().Location.Z = 1`, `a?.B = 1`, `Make().Items.Add(1)`). 다만 호출 결과나 그 뒤의 중간 값(끝 구간의 값 제외)이 참조 타입이면 그 객체의 속성 쓰기이므로 아래 참조 규칙을 따른다(`GetOwner().Location = v`). `?.`가 든 경로는 언제나 S0731이다. 맨 바깥이 이름·멤버·원소가 아닌 대상(S0211)은 이 절의 검사를 하지 않는다. `this.X`는 S0423이다(§3.10).
- 뿌리: 지역 `var`는 쓸 수 있다. 지역 `let`과 `const`는 S0701, 함수·handler 인자와 반복 변수·`if (let)` binding은 S0702, 문맥 이름·타입 이름·이름 공간·함수·enum case 자체에 대입하면 S0706이다. 이름만 쓴 자기 필드는 `var`면 쓸 수 있고 `let`이면 S0701이며, struct 함수 안의 자기 필드는 S0705다(struct 함수는 자기 필드를 쓰지 않는다 — 새 값을 돌려준다).
- 경로가 참조 타입 값(엔진 `object`·Sparks class·interface)을 지나면 그 뒤는 그 객체의 속성 쓰기이며 앞 경로의 규칙과 무관하다(`let` handle의 속성은 쓸 수 있다 — §3.2). 객체의 멤버 중 Sparks class 필드는 뿌리가 이름만 쓴 자기 필드가 아니면 S0703(다른 객체의 Sparks 필드는 읽기만 한다 — 같은 class의 다른 instance여도), 엔진 필드는 schema `access: readWrite`가 아니면 S0704다.
- 값 타입 값(struct·수학 타입·컨테이너·`TagSet` 등)의 멤버·원소 쓰기와 변경 메서드는 그 값을 담은 경로를 바꾸므로 앞 경로가 쓸 수 있어야 한다(위 규칙을 그 경로에 적용 — `let p = vec3(...); p.X = 1;`은 S0701). 부록 B의 언어 멤버는 `var`만 쓸 수 있고 `let` 속성은 S0704다(`text.Length = 1`). 쓸 수 있는 엔진 필드의 값 멤버(`Hinge.RelativeRotation.Yaw = 30.0`)는 읽기 → 수정 → 쓰기다.

**버린 결과(S0713).** 식 문장의 값이 `void`가 아니면 S0713이다: 값을 돌려주는 호출(`Result<void>` 포함), `await`로 얻은 값(`await LoadCount();`), `try`로 꺼낸 값. `void` 호출, 결과가 `void`인 대기의 `await`, `try`로 꺼낸 `void`는 문장이 된다. 버리려면 `_ = ...`로 쓴다(대기는 S0422 — §3.10).

**사용하지 않는 이름과 값(S0710–S0712·S0714, SD74).** "읽기"는 이름을 값으로 쓰는 모든 자리다: 식, 멤버 접근·변경 메서드 호출의 수신자, `return`·`emit`의 인자, 대입 오른쪽. 대입의 왼쪽 이름(값 타입 경로의 뿌리 포함)은 읽기가 아니다. 참조 타입 값을 지나는 경로(`a.Health = 3`)의 뿌리는 읽기다. 변경 메서드 호출(`list.Add(x)`)은 수신자의 읽기이며 S0711의 쓰기가 아니다.

- S0710: 지역 변수(`var`·`let`·`?? return`이 선언한 이름)·`if (let)` binding·반복 변수 중 어디에서도 읽지 않는 이름이다. 위치는 그 이름의 선언이다. 쓰지 않는 반복 변수는 `_`로 쓴다.
- S0711: 지역 `var`에 쓴 값(대입·복합 대입·멤버·원소 쓰기 — 선언의 초기값은 제외)을 그 뒤 어느 경로에서도 읽지 않는다(§4.4의 사본 수정 포함). 경로는 분기와 반복의 되돌아가는 경로(back-edge)를 따른다. 위치는 그 대입 대상이다. 선언 초기값을 읽기 전에 모든 경로에서 덮어쓰는 것은 오류가 아니다. `=`로 이름 전체에 쓰면 앞에 쓴 값은 그 뒤 경로에서 사라지고, 복합 대입과 멤버·원소 쓰기는 앞 값을 바탕으로 하므로 앞 값의 읽기로 센다(`p.X = 1; p.Y = 2; Use(p);`는 오류가 아니다). 어디에서도 읽지 않는 이름은 S0710 하나만 낸다(그 이름의 대입에 S0711을 더하지 않는다).
- S0712: 본문이 있는 함수·handler의 인자 중 읽지 않는 인자다. 시그니처가 정해진 자리(`override fn`, 파생 class가 다시 정의할 `open fn`, 기반 interface의 메서드를 구현하는 함수, handler)의 쓰지 않는 인자는 이름을 `_`로 쓰고, 그 밖의 함수는 쓰지 않는 인자를 지운다(이름 `_` 인자도 S0712). 위치는 그 인자 이름이다.
- S0714: import한 모듈의 선언(타입·함수·const·별칭)을 그 모듈에서 하나도 쓰지 않는다. 위치는 import 경로다. 확장 이벤트의 사용(§5.2)은 이벤트 해석(LC3) 전까지 이름으로 본다: 이 모듈의 `on`·`emit` 경로 끝 이름이 대상 모듈이 선언한 확장 이벤트 이름과 같으면 그 import를 쓴 것이다.
- attribute 인자는 LC3 전까지 결합하지 않으므로 S0714에서는 이름으로 본다: 이 모듈의 attribute 인자 안의 식별자가 대상 모듈의 최상위 선언 이름이거나 그 import의 별칭이면 그 import를 쓴 것이다(`@edit(Min: MinSpeed)`).
- 연쇄 방지: 구문 오류(S01xx·S02xx)가 있는 모듈은 S0710–S0712·S0714를 판정하지 않는다(복구가 건너뛴 원문의 사용이 보이지 않는다). 이름을 해석하지 못한 식별자가 있는 함수·handler는 S0710–S0712를, 선언 이름에 진단이 있는 함수(S0313 등 — `override`를 빠뜨렸을 수 있다)는 S0712를, 이름·타입을 찾지 못한 진단(S0317·S0318)이 있는 모듈은 S0714를 판정하지 않는다(찾지 못한 이름이 그 지역 이름·import를 가리키려던 것일 수 있다).

**await 뒤의 객체 지역 이름(S0412, SD75).** §4.3 "await 이후"는 함수 본문의 문장을 실행 순서대로 따라가는 경로 분석이다(분기 합류와 반복의 되돌아가는 경로를 고정점까지 따른다). `await`가 든 문장을 지나면 그때 보이는 모든 지역 이름(인자·지역 변수·binding·반복 변수)이 "기다린 뒤" 상태가 되고, 그 이름에 값을 넣으면(선언·대입·`if (let)`·반복의 새 값) 풀린다. 어느 경로로든 "기다린 뒤" 상태로 도달하는 읽기에서 이름의 타입이 객체 타입(엔진 `object`·Sparks class·interface — 에셋 제외)이면 그 읽기의 타입은 `T?`다. 한 문장 안에서 `await` 앞에서 읽는 이름(`let r = await a.MoveTo(p);`의 `a`·`p`, `x = await F(x);`의 오른쪽 `x`)은 그 문장의 `await` 전이다. 그 읽기에 `.`을 쓰면 S0412이며 메시지는 기다리는 동안 객체가 사라질 수 있음을 알리고 `if (let)`로 다시 확인하라고 제시한다. 대입은 오른쪽 값을 먼저 계산하고 왼쪽 경로를 그 뒤에 읽는다(`a.Health = await F();`의 `a`는 `await` 뒤의 읽기다). 조건이 리터럴 `true`인 `while`(괄호 포함)은 조건이 거짓인 경로가 없어 `break`로만 끝난다. `emit` 대상 이름(`emit d.Interacted()`의 `d`)의 판정은 이벤트 해석(LC3)과 함께 한다. 이 분석은 타입 검사 전에 끝나야 하므로(읽기의 타입을 정한다) default 없는 switch는 모든 case를 다룬 enum switch라도 어느 case도 실행하지 않고 지나가는 경로를 둔다. S0711의 경로 분석도 같은 규칙을 쓴다.

**상수식(S0510–S0512, SD76).** 상수식은 리터럴(`null` 포함), 보간 없는 문자열, 검증 리터럴, enum case, const 이름, 이들의 단항·이항 연산(산술·비교·논리)과 괄호, 숫자 변환 `T(c)`, 수학 타입·struct 생성, 배열·Map 리터럴, `Math.Radians`·`Math.Degrees`다(인자·원소도 상수식). 그 밖의 호출·필드·지역·인자·문맥 이름·`this`는 상수식이 아니다.

- const의 타입은 `bool`·숫자 타입·`string`·enum(엔진 enum·`Clock` 포함)·`Tag`·`vec2`–`vec4`·`quat`·`Rotator`·`Color`·`Transform`이다. 그 밖은 S0512이며 위치는 타입 표기다.
- const 초기값은 상수식이어야 한다. 필드 초기값은 상수식이거나, 초기값 자체(괄호 안 포함)가 component 생성(인자는 상수식)이어야 한다 — 원소·인자 안의 component 생성은 상수식이 아니다. 어기면 S0510이며 위치는 처음으로 상수식이 아닌 하위 식이다(왼쪽부터).
- struct 필드 초기값이 생성하는 struct(`T(...)` — 배열·Map 리터럴 안 포함)를 따라 그 struct로 돌아오면 S0429다(`struct Tree { var Kids: Array<Tree> = [Tree()]; }` — 기본값이 끝없이 커진다). 같은 강한 연결 묶음 안의 생성 식마다 하나이며 위치는 그 생성 식이다. 값 순환(S0426)과 달리 컨테이너를 거쳐도 순환이다.
- const 초기값이 서로를 가리키면(자기 자신 포함) 순환 하나에 S0511 하나다. 위치는 순환에 든 const 중 (패키지 이름, 모듈 import 경로, 원문 위치) byte 순으로 처음인 것의 이름이다(S0323과 같은 순서). 순환에 든 const와 S0510을 받은 const의 값은 정해지지 않으며 그 const를 쓰는 자리(case 값 등)는 더 진단하지 않는다.

**객체 필드(S0428).** `?` 없는 객체 타입(엔진 `object`·Sparks class·interface) 필드는 class의 (a) component 생성을 초기값으로 둔 `let` 필드와 (b) Component·Actor 파생 class(LevelScript 파생 제외 — S0413, LC3)의 `@edit` 필드에만 쓴다. 그 밖(struct 필드, WorldService·WidgetController·DataAsset 파생 class의 필드, `@edit`도 component 생성도 없는 필드)은 S0428이며 `T?`를 제시한다. 위치는 필드의 타입 표기(표기가 없으면 필드 이름)다.

**프로필(S1113).** `profile: "core"` 패키지의 모듈에서 full 문법을 쓰면 S1113이며 기능 이름과 core 대안을 제시한다. 이 단계가 보는 full 문법은 `interface` 선언, `open` class·fn, Sparks class·interface를 기반으로 쓰는 것(core의 기반은 엔진 타입뿐 — full 의존 패키지의 것도), `Class<T>` 타입 표기, full 전용 attribute 이름(`@rpc`·`@worker`·`@deterministic`·`@pure`·`@tick`·`@cheat`·`@editorButton`·`@cvar`·`@test`), hook `on Validate()`다. 위치는 그 선언의 이름(`interface`·`open class`·`open fn`), Sparks 기반마다 그 기반 경로, 타입 표기, attribute(`@`부터), handler 이름이며 full 문법 하나에 하나다. 실행 중 component 추가·우선순위 지정 로드처럼 API 단위의 full 기능은 binding schema에 표시해 연동 단계에서 본다.
### 3.12 이벤트·hook 선언(LC3a 확정)

이 절은 §5.1–§5.3의 hook·`on` 경로·`emit`·확장 이벤트·재진입 표시 규칙을 컴파일러가 판정하는 방식으로 확정한다. 진단 코드는 §8.1의 S0413, S08xx다. 뒤로 넘기는 것: handler·hook 안의 효과(await·emit·생성의 문맥별 금지 — LC3b), widget 경로의 대조(WidgetAsset schema가 컴파일 입력에 들어올 때 — 연동 단계), 입력 action 사전 대조(검증 리터럴 사전 — 연동 단계), 실행 중 연결·전달(LC6).

**수명 클래스.** hook·`on` handler는 기반 체인에 엔진 수명 타입(Component·Actor·WorldService·WidgetController)이 있는 class에만 둔다. 그 밖의 class(DataAsset 파생 등)의 handler는 S0804다(struct 본문의 `on`은 구문 오류 S0204다). 기반을 해석하지 못한 class(3.9절 연쇄 방지)는 이 절을 판정하지 않고, 원천 이름을 해석하지 못한 handler는 중복 비교에서 뺀다.

**hook(`on X(...)`, 경로 구간 하나).** X는 `Awake`·`Enable`·`Start`·`Tick`·`FixedTick`·`LateTick`·`Disable`·`Destroy`·`TriggerEnter`·`TriggerExit`·`CollisionEnter`·`CollisionExit`·`Validate`(full) 중 하나다. 그 밖의 이름은 S0803이며 편집 거리 2 이하의 hook 이름이 있으면 그것을, 그 이름이 자기 class(기반 class 포함)의 이벤트면 이벤트는 `emit`한 class 안에서 받지 않는다는 설명과 함수를 직접 부르라는 수정 예를 제시한다.

- 물리 hook(`Trigger*`·`Collision*`)은 Component·Actor 파생 class에만 있고(WorldService·WidgetController에서는 S0804), LevelScript 파생 class에서는 S0801이다.
- 인자: Tick 계열은 없거나 `(dt: float)`, `TriggerEnter`·`TriggerExit`는 `(other: Actor, trigger: Collider)`, `CollisionEnter`·`CollisionExit`는 `(other: Actor, hit: Hit)`의 앞에서부터 필요한 만큼, 나머지 hook은 인자가 없다. 넘치는 인자와 타입이 정확히 같지 않은 인자는 S0805이며 위치는 그 인자다(타입이 같지 않으면 타입 표기, 넘치면 그 인자 이름). 인자 이름 `_`는 허용한다.
- 같은 class에 같은 hook이 둘이면 두 번째부터 S0806이다(기반과 파생의 같은 hook은 누적된다 — §5.1).

**`on` 경로(`on A.E(...)`, `on A.B.Changed()`).** 경로 끝 구간이 이벤트 이름이고 앞 구간이 원천이다. 원천은 다음 중 하나이며 그 밖은 S0807(위치는 해석하지 못한 첫 구간, 편집 거리 2 이하의 필드·이름 제시)이다.

| 원천 | 조건 | 이벤트 |
|---|---|---|
| `this` | Actor 파생 class(그 밖은 S0807, 경로가 `this` 하나면 S0807) | 그 class 기반 체인의 엔진 Actor 이벤트와 Actor 확장 이벤트(Sparks `event`는 받지 않는다 — S0808) |
| `World` | World 문맥 이름이 있는 class | 엔진 `World` 타입의 이벤트와 World 확장 이벤트 |
| `Widget` | WidgetController 파생 class, `Widget.이름.이벤트`(구간 셋) | 판정하지 않는다(위 "뒤로 넘기는 것") |
| 필드 이름 | 자기·상속 필드(Sparks 필드, 엔진 필드 `Owner` 등) | 아래 |

필드 원천의 이벤트: 경로가 `F.Changed`면 필드 F 자신의 Changed, `F.G.Changed`면 F의 타입(객체·`?` 허용)의 필드 G의 Changed다. 그 밖의 `F.E`는 F의 타입 T(객체, `T?`, 또는 원소가 객체인 `Array<T>`)의 이벤트 E다: Sparks class 체인의 `event`, 엔진 타입 체인의 이벤트(schema), 부록 B 언어 이벤트(`InputAction.Triggered` 등), T가 Actor 파생이면 Actor 확장 이벤트. 구간이 넷 이상이거나 `F.G.E`(E가 `Changed`가 아님)는 S0807이다. 이벤트를 찾지 못하면 S0808(위치는 이벤트 이름, 편집 거리 2 이하의 이벤트 제시)이다.

- **Changed:** Sparks `var` 필드와 schema가 `changed: true`로 표시한 엔진 필드에만 있다. `let` 필드·const(`on Limit.Changed()`)·언어 멤버(`on Jump.Value.Changed()`)·`changed`가 없는 엔진 필드의 `Changed`는 S0809다. `Changed`는 예약 이벤트 이름이므로 `event Changed`·확장 이벤트 `Changed`도 S0809다.
- **인자:** handler 인자는 이벤트 인자 목록(배열 원천은 앞에 원소 `T` 하나를 더한 목록, Changed는 빈 목록)의 앞에서부터 필요한 만큼이며 타입이 정확히 같아야 한다(`?` 유무 포함). 넘치거나 다르면 S0810이며 위치 규칙은 S0805와 같다.
- **중복:** class와 그 기반 class 전체에서 같은 경로(구간 글자가 같은 것)를 둘 선언하면 나중 것(파생 class, 같은 class면 뒤의 것)이 S0806이다.

**확장 이벤트(`event Actor.X(...)`·`event World.X(...)`).** 최상위에만 쓰며(구문) 대상은 `Actor`·`World` 두 이름이다. 그 밖의 대상은 S0813이다. 이름은 대상 타입의 이벤트(엔진 schema 포함)와도, 같은 대상의 다른 확장 이벤트(현재 패키지와 의존 패키지 전체 — 의존 패키지에서 온 것과의 충돌은 이 패키지 쪽에)와도 같을 수 없으며 S0814다(위치는 이벤트 이름, 메시지에 먼저 선언한 곳). 매개변수 타입은 함수 매개변수와 같은 규칙으로 해석한다. `on`·`emit`과 식의 `X.E`(`await Owner.Interacted`, `Race`의 인자 — §6.1)에서 확장 이벤트는 이 모듈이나 이 모듈이 직접 import한 모듈의 것만 찾는다(식에서는 단계와 무관하게 멤버처럼 찾는다). 그렇게 보이는 같은 이름의 확장 이벤트가 둘이면(서로 의존하지 않는 두 패키지) 그 사용 자리에 S0814다. 의존 패키지 전체는 전이 의존이다. 같은 이름의 확장 이벤트가 import하지 않은 모듈에 있으면 S0815이며 그 모듈의 import를 제시한다. 해석한 확장 이벤트는 그 import의 사용이다(S0714 — 3.11절의 이름 판정을 대신한다).

**`emit`.** 인자는 이벤트 매개변수 타입의 자리로 검사한다(수가 다르면 S0403 — §3.10). `emit this(...)`처럼 이벤트가 없는 경로는 S0807이다.

- `emit E(...)`: E는 이 class가 선언한 `event`여야 한다. 기반 class의 이벤트·엔진 이벤트는 S0811("선언한 class 안에서만 emit한다"), 찾지 못하면 S0808이다.
- `emit this.X(...)`: Actor 파생 class에서 Actor 확장 이벤트 X를 자기 Actor에 발행한다(Actor 파생이 아니면 S0807, 찾지 못하면 S0808).
- `emit t.X(...)`: t는 지역 이름·인자·필드(이름 하나)이고 그 타입이 Actor 파생(`?` 없음)이거나 World이며 X는 그 대상의 확장 이벤트다. t의 타입이 `T?`이면 S0412, await 뒤 상태의 객체 지역 이름이면 3.11절의 S0412다. t가 그 밖의 타입이면 S0811(멤버·엔진 이벤트는 발행할 수 없다).
- 식 안의 `X.E(...)`(이벤트를 호출 형태로 씀)는 S0812이며 `emit`이나 `await X.E`를 제시한다.

**재진입 표시(SD11).** `on` 경로 handler와 물리 hook 중 본문에 `await`(또는 `try await`)가 있는 것은 `@skipWhileBusy`나 `@restart` 중 정확히 하나를 붙인다. 없으면 S0816이며 두 형태를 수정 예로 제시한다(위치는 handler 이름). 둘 다 붙였거나, `await`가 없는 handler, 물리 hook이 아닌 hook, `Changed` handler, 함수에 붙이면 S0817이다(위치는 attribute). widget 경로 handler도 재진입을 판정한다. 그 밖의 선언(필드·이벤트·class)에 붙이면 attribute 자리 규칙의 S0621이다(§3.13). `Changed` handler의 `await`는 LC3b(Reaction 문맥)가 알리므로 S0816을 내지 않는다.

**LevelScript(LevelScript LD7·LD18).** LevelScript 파생 class의 `@edit` 객체 필드에 `?`가 없으면 S0413(3.11절의 S0428 대신), `let` 필드의 초기값(괄호 안 포함)이 Collider 파생 component 생성이면(필드 표기와 무관하게 생성식의 타입으로) S0802, 물리 hook은 S0801이다.
### 3.13 실행 문맥·효과·attribute(LC3b 확정)

이 절은 §6.1·§6.2의 실행 문맥, `await`·`while` 규칙과 attribute의 이름·자리·인자를 컴파일러가 판정하는 방식으로 확정한다. 진단 코드는 §8.1의 S06xx다. 뒤로 넘기는 것: 유지 자원의 담는 자리(LC3c), `@net`·`@rpc`·`@save`의 타입·권한(LC3d), attribute의 실행 의미(연동 단계), Teardown의 읽기 접근 제한(`Owner`의 읽기 전용 속성만 — 연동 단계에서 schema 표시와 함께).

**attribute.** 이름이 아래 표에 없으면 S0620(편집 거리 2 이하의 이름 제시), 표의 자리가 아닌 곳이면 S0621, 한 선언에 같은 attribute가 둘이면 두 번째가 S0622, 인자 모양이 틀리면 S0623이다(위치는 그 attribute, 인자 오류는 틀린 그 인자 — 남는 인자면 첫 남는 인자). schema 함수의 `pure`는 `oneShot`·`effects`와 함께 쓸 수 없다(S1120). 프로필(full 전용)은 S1113이 보고 재진입 표시는 §3.12가 본다. 실행 문맥 attribute(`@pure`·`@worker`·`@deterministic`·`@editorButton`·`@cheat`·`@test`)는 한 함수에 하나뿐이다(둘째부터 S0621).

| attribute | 자리 | 인자 |
|---|---|---|
| `@edit` | class·struct 필드 | 없음, 또는 이름 지정 `Min:`·`Max:`(숫자 리터럴(앞의 `-` 포함)이나 숫자 const 이름·별칭 멤버 — 필드 타입이 숫자일 때만. attribute 인자는 식을 계산하지 않는다) |
| `@save` | class의 `var` 필드 | 없음 |
| `@net` | class의 `var` 필드 | 없음, 또는 짧은 이름 `OwnerOnly` |
| `@authorityOnly` | `event` 선언(멤버·확장) | 없음 |
| `@formerly` | 필드·enum case·class·struct·enum | 보간 없는 문자열 리터럴 하나 |
| `@tableRow` | struct | 검증 리터럴 `Asset("...")` 하나 |
| `@skipWhileBusy`·`@restart` | handler(§3.12) | 없음 |
| `@rpc` (full) | class의 `async`가 아닌 함수 | 짧은 이름 `Server`·`AllClients`·`OwnerClient` 하나, 그 뒤 선택 `Reliable`·`Unreliable` 하나 |
| `@pure`·`@worker`·`@deterministic` (full) | 함수 | 없음 |
| `@editorButton`·`@cheat` (full) | class의 인자 없는 함수 | 없음 |
| `@test` (full) | 인자 없는 최상위 함수 | 없음 |
| `@tick` (full) | `on Tick`·`on FixedTick`·`on LateTick` | 이름 지정 `Phase:`(`PrePhysics`·`PostPhysics`·`LateUpdate`), `Interval:`(0 이상의 숫자 리터럴·const), `Priority:`(정수 리터럴·const) 중 하나 이상, 각각 한 번 |
| `@cvar` (full) | WorldService 파생 class의 `var` 필드(bool·숫자·`string`) | 보간 없는 문자열 리터럴 하나(CVar 이름) |

**효과.** 함수·hook·handler 본문마다 효과 집합을 추론한다. 효과와 근원은 다음이다.

| 효과 | 근원 |
|---|---|
| 대기 | `await`(아래 준비 대기 밖) |
| 준비 대기 | `await`의 피연산자가 `X.Load()`·`Assets.LoadAll(...)`, `LoadRequest<T>` 값, schema `effects`에 `readiness`가 있는 함수 호출, 인자가 모두 로드 요청인 `All`·`Race` |
| 생성 | schema `effects`에 `spawn`이 있는 함수(`World.Spawn`) |
| 자원 획득 | schema `effects`에 `acquire`가 있는 함수(유지 자원을 돌려주는 `Start*` — §7.1, LC3c가 자원 정책으로 넓힌다) |
| 로드 | `X.Load()`·`Assets.LoadAll(...)` 호출 |
| 발행 | `emit` |
| 쓰기 | 필드(자기 필드, 객체를 지난 경로)에 대입·변경 메서드 호출. 지역 이름만 쓰는 것은 쓰기가 아니다 |
| 객체 쓰기 | 쓰기 중 참조 타입 값을 지나는 경로(다른 객체의 속성) |
| 엔진 호출 | schema `pure`가 아닌 엔진 함수 호출 |
| 입출력 | schema `effects`에 `io`·`wallClock`이 있는 함수 |
| RPC 호출 | `Server.F(...)` 등 RPC 접두 호출(§3.15 — 네트워크 송신) |
| 객체 참조 | 엔진 객체·Sparks class·interface 타입의 값(인자·지역·필드·`this`·문맥 이름)을 쓰는 것 |

호출은 부른 함수의 효과를 더한다(호출 그래프의 전이 폐포, 재귀는 고정점). 수신자가 `this`가 아닌 다른 객체인 메서드 호출에서 부른 함수의 쓰기는 객체 쓰기다. `await`의 피연산자가 `async fn` 호출이면 그 `await` 자체는 대기 효과가 아니다. `open`·엔진 `overridable` 함수의 호출은 그 함수와 컴파일 안의 모든 `override`(여러 단계의 재정의 포함)의 효과를, interface 메서드(Sparks·엔진)의 호출은 그것을 구현한 모든 class 함수(상속한 함수로 구현한 것 포함)의 효과를 더한다(재정의·구현이 기반 문맥이 금지하는 동작을 숨길 수 없다). `override fn`은 자기 문맥 attribute가 없으면 재정의한 기반 함수 사슬의 문맥 attribute(`@pure`·`@worker`·`@deterministic`)를 따른다. 계산은 호출 그래프의 역방향 worklist로 하므로 사슬 길이와 무관하게 간선 수에 비례한다. S0601을 받은 `await`는 효과로 전하지 않는다(부르는 쪽의 연쇄 방지). 대기를 만드는 호출(`async fn`을 `All`·`Race`의 인자로 넘기는 것 포함)도 부른 것이다.

**문맥.** 다음 본문은 표의 효과를 쓸 수 없다. 직접 쓰면 그 자리(`await`·`emit`·대입·호출)에, 호출이 가져오면 그 호출에 S0602를 내며 메시지는 문맥과 효과, 호출이면 부른 함수 이름을 적는다(호출 하나에 진단 하나, 같은 위치에도 하나 — 걸린 효과 중 더 구체적인 것: 대기, 생성, 자원 획득, 로드, 발행, 직렬화 필드 쓰기, 객체 쓰기, 쓰기, 엔진 호출, 일회성 효과, RPC 호출, 입출력, 객체 참조 순). 같은 위치에 S0602와 S1003(§3.15)이 함께 걸리면 S0602 하나만 낸다.

| 본문 | 금지 효과 |
|---|---|
| `on Awake` | 대기·준비 대기·생성·자원 획득·로드 |
| `on Enable` | 대기·준비 대기 |
| `on Disable` | 대기·준비 대기·자원 획득·발행 |
| `on Start` | 대기(준비 대기는 된다) |
| `on Tick`·`FixedTick`·`LateTick` | 대기·준비 대기·생성·자원 획득·로드 |
| `on X.Changed` | 대기·준비 대기 |
| `on Destroy` | 대기·준비 대기·생성·자원 획득·발행·객체 쓰기·엔진 호출(schema `pure`·`oneShot` 함수는 된다) |
| `on Validate` (full) | 대기·준비 대기·생성·입출력·직렬화 필드(`@edit`·`@save`·`@net`) 쓰기·RPC 호출(§3.15) |
| 최상위 `fn` | 대기·준비 대기·발행·생성·자원 획득·로드·엔진 호출·일회성 효과·객체 쓰기(§3.1 "순수 계산 함수"). 최상위 `async fn`은 S0601이다. `@test` 함수는 시험 World의 진입점이라 이 제한이 없다(§13) |
| `@pure fn` (full) | 쓰기·발행·대기·준비 대기·생성·자원 획득·로드·엔진 호출·일회성 효과·RPC 호출 |
| `@worker fn` (full) | 객체 참조·자원 획득·로드·대기·준비 대기·발행·RPC 호출 |
| `@deterministic fn` (full) | 입출력·대기(`NextFixedTick()`의 대기만 된다)·준비 대기·RPC 호출(재생하면 다시 보낸다). `Random`은 instance별 결정적 stream이므로 된다(§6.3) |

**`await`를 쓸 수 있는 곳(S0601).** `await`는 `async fn`과 handler·hook 본문에만 쓴다. 그 밖(`async`가 아닌 `fn`)의 `await`는 S0601이며 `async fn`으로 선언하라고 제시한다. hook 문맥이 대기를 금지하는 것은 S0602다.

**`while`(S0603).** `while`은 대기할 수 있는 본문(`async fn`, 대기를 금지하지 않는 hook·handler)에서만 쓰고, 반복 본문의 시작에서 다음 회차(본문 끝·`continue`)로 가는 모든 경로에 `await`가 있어야 한다(`break`·`return`으로 나가는 경로는 된다). 어기면 S0603이며 위치는 `while`이다. 경로는 문장 구조로 판정한다: `if`는 두 갈래, `switch`는 case마다(default가 없고 모든 case를 다룬 enum switch가 아니면 지나가는 갈래 포함 — 타입 검사 뒤의 판정이다), `for`·안쪽 `while`의 본문은 0번 돌 수 있다. `async fn` 호출의 `await`는 그 자체로 대기가 아니다(§6.1 — 그 함수의 대기가 효과로 온다). 무한 반복은 `for`·조건 반복 대신 `while`로만 쓰고 프레임마다 기다리는 게임 반복의 모양만 허용하는 규칙이다.

**schema key(LC3b).** 함수 객체에 다음 key를 더한다(§10.2 표).

| key | 값 |
|---|---|
| `effects` | 문자열 배열: `spawn`·`acquire`·`readiness`·`io`·`wallClock`(중복 불가) |
| `pure` | bool(기본 false). 관찰 가능한 상태를 바꾸지 않는 함수(getter·계산) |
| `oneShot` | bool(기본 false). handle 없는 일회성 효과(§7.1, `World.Audio.PlayAt`) |
### 3.14 유지 자원·로드(LC3c 확정)

이 절은 §7.1·§7.3의 유지 자원 규칙 셋과 로드 요청 handle을 컴파일러가 판정하는 방식으로 확정한다. 진단 코드는 §8.1의 S09xx다. 뒤로 넘기는 것: 실행 중 해제 순서·generation(LC6), `try await req`의 두 번째 결과 꺼내기(실행 중 `ContractViolation` — LC6), 반복 설정 에셋을 일회성 API에 넘기는 쿡 오류(연동 단계).

**유지 자원 타입.** binding schema가 `held: true`로 표시한 엔진 객체 타입(반복 사운드·입력 context 등)과 언어 타입 `LoadRequest<T>`의 값이 유지 자원이다(`T?` 포함). schema `effects`에 `acquire`가 있는 함수는 유지 자원 타입(`T?`)을 돌려준다(아니면 S1120). 유지 자원을 돌려주는 호출이 획득이다(LC3b의 자원 획득 효과).

**담는 자리(규칙 1).** 획득 호출(`X.Load()`·`Assets.LoadAll(...)` 포함)은 다음 자리에만 쓴다. 그 밖은 S0902, 결과를 버리는 자리(식 문장, `_ =`)는 S0901("멈출 방법이 없다" — 식 문장의 S0713은 내지 않는다)이다.

- 지역 변수(`var`·`let`) 선언의 초기값(`?? return` 포함)과 `if (let s = ...)`의 값(binding이 담고 then 블록이 끝나면 멈춘다). 받는 이름의 타입도 유지 자원 타입이어야 한다(기반 타입 이름에 담으면 S0902)
- 대입의 오른쪽: 대상이 유지 자원 타입의 자기 필드(이름만 쓴 것) 또는 `var` 지역 변수일 때. 로드 요청은 지역 변수에만 담는다(필드면 S0902)
- `await`·`try await`의 피연산자, `All`·`Race`의 인자(대기가 끝날 때까지 그 실행이 보관한다)

**옮길 수 없음(규칙 3).** 유지 자원을 담은 이름(지역·필드)은 다음 자리에서만 읽는다: 멤버 접근·호출의 수신자(`.`·`?.` — `Hum?.Pause()`, `req.IsDone`), `await`·`try await`의 피연산자, `All`·`Race`의 인자, `null`과의 `==`·`!=`. 그 밖(다른 이름에 대입·초기값, `if (let)`, `??`, 함수·이벤트 인자, 반환, 컨테이너·struct 원소)은 S0903이다. 대입의 왼쪽(`Hum = ...`)은 읽기가 아니다.

**담는 선언(S0904).** 유지 자원 타입은 지역 이름과 다음 필드에만 쓴다: class의 `var` `T?` 필드(attribute `@save`·`@edit`·`@net` 없음 — 로드 요청은 필드에 둘 수 없다). struct 필드, `let`·`?` 없는·`@save`/`@edit`/`@net` 필드, 컨테이너·`Result`·`Soft`의 타입 인자, 함수·handler·이벤트 매개변수, 반환 타입에 쓰면 S0904이며 위치는 그 타입 표기다.

**기다리지 않는 지역 자원(S0905).** 획득 호출을 초기값으로 둔 유지 자원 타입의 지역 변수 선언 뒤(다른 이름에서 옮긴 것은 S0903만), 같은 블록의 끝까지(뒤 문장과 그 안 — 반복·분기 포함, `if (let)` binding은 then 블록) 정지하는 `await`(§3.13 — 대기하지 않는 `async fn`의 await는 세지 않는다)가 하나도 없으면 S0905다(그 자원은 같은 스크립트 단계에서 멈춘다). 수정 예는 계속 유지하려면 필드에 담기, 정해진 시간만 유지하려면 블록 안의 `await Delay(...)`다.

**schema key(LC3c).** 타입 객체에 `held`(bool, 기본 false, `object`만, `scriptExtensible`과 함께 쓸 수 없다)를 더한다(§10.2 표). 유지 자원 타입을 돌려주는 함수는 `effects`에 `acquire`가 있어야 하고(반대도 — S1120), 엔진 이벤트의 인자는 유지 자원 타입일 수 없다(S1120).
### 3.15 권한·RPC·저장 타입(LC3d 확정)

이 절은 §14의 복제 필드·권한 구역(SD45)·RPC와 §15의 저장 가능 타입을 컴파일러가 판정하는 방식으로 확정한다. 진단 코드는 §8.1의 S10xx다. 뒤로 넘기는 것: 저장 호환성(필드 삭제 `retire`·형식 변경 migration — 이전 빌드의 ID manifest가 필요하다, LC4 §12.2), 실행 중 권한 판정·`ContractViolation`(LC6·연동), interest set·빈도 제한(연동 EI7).

**저장 가능 타입(S1001).** `bool`·숫자 타입·수학 타입(`vec2`–`vec4`·`quat`·`mat4`·`Rotator`·`Color`·`Transform`)·`string`·enum(Sparks·엔진)·`Clock`·`Tag`·`TagSet`·`Soft<T>`·`Row<T>`·`Class<T>`(클래스 경로로 저장 — UE `TSubclassOf`), 그 `T?`, 이들로만 된 struct(Sparks·엔진)와 `Array`·`Map`·`Set`이다. 컨테이너를 거쳐 자기 자신을 담는 struct(`struct Tree { var Kids: Array<Tree>; }` — S0426은 컨테이너를 거친 것을 순환으로 보지 않는다)도 저장 가능하다: 판정은 타입마다 한 번 하고 계산 중인 타입은 가능으로 본다. `@save` 필드의 타입이 아니면 S1001이며 위치는 타입 표기(없으면 필드 이름)다. 엔진 객체·Sparks class·interface, 로드한 에셋 값(`T` — `Soft<T>`를 제시), `Text`, `Result`, 유지 자원은 저장할 수 없다. 유지 자원을 타입 인자로 담은 타입은 S0904(§3.14)만 내고 S1001·S1002를 더하지 않는다.

**복제 타입(S1002).** `@net` 필드와 RPC 함수의 매개변수는 저장 가능 타입에 객체 참조(엔진 객체·Sparks class·interface, 그 `T?`, 그것을 담은 컨테이너·struct)를 더한 타입만 쓴다. 로드한 에셋 값은 S1002이며 `Soft<T>`를 제시한다. RPC 함수의 반환 타입은 `void`뿐이다(`: void`는 쓸 수 있다 — §3.4, 다른 반환 타입은 S1002).

**권한 구역(S1003, SD45).** `@net` 필드(자기 필드 — 이름만 쓴 것이나 그 값 타입 경로·변경 메서드)의 쓰기는 권한 구역 안에서만 한다. 권한 구역은 다음이다.

- 조건이 권한 시험이거나 `&&`의 한 항이 권한 시험인 `if`의 참 분기(괄호 무시). 권한 시험은 `Owner.HasAuthority`(component)와 `HasAuthority`(Actor 파생)의 읽기이며, 이름이 아니라 바인더가 정한 엔진 심볼(Component의 `Owner`, Actor의 `HasAuthority`)로 판정한다. 구역 안의 중첩 `if`·`else if` 사슬의 갈래도 구역이다. `on Disable`·`on Destroy` 안에서도 같다.
- 조건이 부정 권한 시험(`!` 권한 시험, 또는 그것을 한 항으로 둔 `||`)인 `if`의 else 분기.
- 조기 반환 관용구: 블록 안에서 조건이 부정 권한 시험이고 참 분기가 반드시 `return`으로 끝나는(`return` 하나이거나 마지막 문장이 `return`인 블록) `if` 뒤의 같은 블록 문장(UE의 `if (!HasAuthority()) return;`).
- 서버 RPC(`@rpc(Server ...)`) 본문.
- `@authorityOnly` 이벤트(멤버·확장)와 schema `publisher: "server"` 엔진 이벤트의 handler 본문.

`||`의 한 항인 권한 시험과 `!` 권한 시험의 참 분기는 구역이 아니다. 함수의 `@net` 쓰기는 LC3b 효과처럼 호출로 전해지며, 권한 구역 안의 호출은 그 쓰기를 전하지 않는다. 다른 객체의 함수 호출(`other.SetOpen()`)이 가져오는 그 객체의 `@net` 쓰기도 같다: 권한은 서버·클라이언트 world 단위이므로 자기 객체의 권한 시험이 구역을 연다. 진입 본문(hook·handler·`@rpc` 함수, 엔진 `overridable` 함수의 재정의, `@cheat` 함수 — 엔진이 직접 부르는 본문)에 권한 구역 밖의 `@net` 쓰기가 남으면 그 자리(대입·호출)에 S1003이고 `if (Owner.HasAuthority)`(Actor 파생은 `if (HasAuthority)`) 감싸기를 제시한다. `@editorButton`은 편집기 world(항상 권한이 있다)에서만 돌므로 진입 본문이 아니다. 어느 진입 본문에서도 부르지 않는 함수는 판정하지 않는다.

**RPC 호출(S1004).** RPC는 자기 객체의 `@rpc` 함수를 그 대상 접두로만 부른다: `@rpc(Server)`는 `Server.F(...)`, `@rpc(AllClients)`는 `AllClients.F(...)`, `@rpc(OwnerClient)`는 `OwnerClient.F(...)`(접두를 괄호로 감싸도 같다). 접두 없이 부르기, 다른 대상 접두, `@rpc`가 아닌 함수에 접두, 접두 뒤의 멤버가 함수가 아닌 것(`Server.Open` 필드), 자기 객체가 없는 곳(최상위 `fn`, struct 메서드)의 접두는 S1004다. `@rpc` 함수를 재정의한 `override fn`은 `@rpc`를 다시 쓰지 않아도 기반의 RPC 대상을 잇는다(UE처럼 재정의는 RPC의 구현만 바꾼다 — 서버 RPC의 재정의 본문도 권한 구역이다). `async` RPC는 S0621이다(RPC는 보내기만 한다). RPC 호출은 보내기만 하므로 부른 쪽 본문에 그 함수의 효과를 더하지 않고 효과 "RPC 호출"이 된다(`on Validate`·`@pure`·`@worker`·`@deterministic`이 금지 — §3.13).

**`Caller`.** RPC 본문의 문맥 이름 `Caller`는 LC3에서 이름·위치 모양만 본다. 보낸 연결의 실제 검증(소유 연결인지)은 실행 중 판정이라 네트워크 연동(EI7)에서 정한다.

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
| 논리·숫자 | `bool`, `int8`–`int64`, `uint8`–`uint64`, `float32`, `float64`, 기본형 `int`(=`int64`)·`float`(=`float64`) | 규칙은 §3.3(SD56·SD57). 엔진 필드·인자는 C++ 타입과 같은 Sparks 타입으로 보이므로(C++ `int32` → `int32`, `float` → `float32`, `double` → `float64`) 경계에서 따로 변환하지 않는다. 수학 타입(`vec3` 등)의 성분은 `float64`이며 엔진의 float32 벡터와 주고받을 때 최근접 짝수로 반올림하고 범위 밖은 `NonFinite`다 |
| 수학 | `vec2`–`vec4`, `quat`, `mat4`, `Rotator`, `WorldPosition`, `Transform`, `Color` | Core 수학 타입의 바인딩(연산과 메서드는 부록 B). `WorldPosition`은 엔진에 아직 없는 자리표시 타입이며(연동 요구 SE-G13) 이름만 예약하고 규격은 엔진 타입이 생길 때 정한다. 스크립트 안의 성분은 `float`(64비트)이고 엔진에 전달할 때 위 규칙으로 변환한다. 단위·축은 엔진 규약(UE 5.8과 같은 왼손 좌표계, +X 전방·+Y 오른쪽·+Z 위, yaw 양수는 오른쪽 회전, 길이 cm·질량 kg·시간 초, `Rotator` 도, 삼각함수·quaternion 축-각·각속도 라디안)을 따른다(전체 계획 §2.2, Math 구현 문서의 Rotator 규약). 각도 단위가 둘이므로 각도를 받는 native 함수·필드는 UE처럼 이름이나 타입으로 단위를 드러내고(`Rotator` 필드는 도, `…InRadians`·`…InDegrees`), 언어 서버가 hover·자동완성에 단위를 표시한다(SD18) |
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
- component는 `let` 필드의 초기값으로만 생성한다(실행 중 추가는 full). 편집기에서 배치한 Actor에 ScriptableComponent를 붙이는 것은 스크립트의 생성이 아니라 레벨·배치 데이터 편집이며 core에서도 된다: 직접 배치 Actor는 그 `components`에 추가해 레벨에 저장하고, prefab 배치는 instance override의 노드 추가로 저장한다(Prefab PF4 선행, [LevelScript](LevelScript.ko.md) LD15). 필드 초기값은 상수식과 component 생성만 허용하고 클래스 기본값 레코드에 반영한다 — 레코드는 가장 가까운 C++ base의 생성자 결과 위에 컴파일된 초기값을 덮어 합성한다(RCM '클래스 기본값 결정'). component 생성 초기값은 클래스 원형의 default subobject(Prefab PF1)가 선행 조건이며 그때까지 미지원이다. 상수식은 리터럴, 검증 리터럴, enum 값, const 이름, 이들의 산술·비교, 수학 타입·struct 생성, 배열·Map 리터럴, `Math.Radians`·`Math.Degrees`이며 그 밖의 함수 호출과 필드 참조는 포함하지 않는다. 컴파일 시 overflow는 오류다. C++ 생성자 안에서 스크립트 코드를 실행하지 않는다.
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
| `on TriggerEnter(other, trigger)` · `TriggerExit` · `on CollisionEnter(other, hit)` · `CollisionExit` | `OnTrigger*` · `OnCollision*` | PostPhysics 물리 이벤트 batch | `OnTriggerEnter` 등 | 자기 Actor의 모든 collider에서 오며 어느 collider인지는 `trigger` 인자로 구분한다. 재진입 판정의 발행자는 `other`다. 값으로 복사한 이벤트를 generation 재검증 후 전달(전체 계획 §16.2). 다른 객체는 같은 통지를 엔진 Actor 이벤트 `TriggerEntered` 등으로 받는다(§5.2). `LevelScript`는 물리 hook을 선언할 수 없다(S0801, LevelScript LD18) |
| `on Validate()` (full) | OnValidate | EditorConstruct | `OnValidate` | 편집기 전용, 저장되지 않는 미리보기 상태만 변경 |

- 수명·물리 hook은 누적된다. 기반 클래스의 hook이 먼저 실행되고 파생 클래스의 것이 이어서 실행된다. `on 경로.이벤트` handler는 누적되지 않으며 클래스 계층 전체에서 같은 경로를 한 번만 선언한다. 파생 클래스가 동작을 바꾸려면 기반의 handler가 호출하는 `fn`을 `override`한다.
- 이름 하나짜리 `on X()`는 hook 전용이다.
- `fn Update()`·`fn Start()`·`fn OnStart()`처럼 hook·`On`+hook·Unity hook 이름의 함수는 오류이며 해당 `on` 형태를 제시한다.
- C++ 가상 함수에 `On` 접두를 쓰면 오디오·타임라인의 일반 `Start()` 메서드와 충돌하지 않는다(SD9). C++ 수명 가상 함수(`On*`·`OnRegister`·`OnUnregister`)는 binding에 노출하지 않는다.
- **LevelScript 순서(LevelScript LD8):** 레벨 편입에서 `LevelScript`의 `FinishSpawning`(Awake·Enable)은 그 Level의 모든 배치 Actor(prefab 배치에서 펼친 Actor 포함)가 마친 뒤에 한다. 그래서 LevelScript의 `on Enable`에서 저장 때 활성이던 대상은 이미 활성이다. Start는 엔진 시작 큐 순서대로 같은 프레임에 준비된 활성 배치 Actor 뒤에 시작하며, 꺼진 채 기다리거나 준비가 안 된 Actor의 Start는 기다리지 않는다. Level을 내릴 때(`DestroyLevel`·World 종료)는 LevelScript의 Disable·Destroy가 먼저다. LevelScript는 `RequestDestroy`로 파괴할 수 없다(요청을 무시하고 위치별 1회 보고 — LevelScript LD16).
- **활성 전환:** 스크립트 단계 안의 활성·비활성 전환 요청은 파괴와 같은 deferred 큐로 처리하여 이벤트 전달이 끝난 뒤, 파괴 적용 전에 요청 순서대로 World `SetEnabled`를 호출한다(SD25). 엔진의 `SetEnabled`는 hook을 동기 실행하므로 Enable·Disable hook은 이 적용 안에서 실행되고, 그때 등록된 emit은 같은 큐로 전달된다. 비활성 instance의 유지 자원 획득 결과는 `null`이다.
- **엔진이 연 진입:** 스크립트 단계 밖에서 엔진이 hook을 부르는 지점(레벨 로드·셀 스트리밍·복제 spawn의 Awake·Enable, 프레임 시작 처리의 Start, 프레임 끝 정리의 Disable·Destroy, 물리 이벤트 batch)은 VM에 들어갈 때 스크립트 단계 하나를 열고, hook 실행 → 발행 큐 전달 → 적용을 마친 뒤 닫는다. 따라서 SD3의 지연 규칙은 모든 스크립트 실행에 성립한다. 적용 루프(활성 전환·파괴가 다시 낳은 전환·파괴)는 한 단계에서 64회까지 돌고, 넘으면 남은 요청을 다음 단계로 넘기고 보고한다.
- 이 절과 §2.4·§5.2의 표는 handler 인자 타입을 생략하였다. 선언할 때는 `on Tick(dt: float)`, `on TriggerEnter(other: Actor, trigger: Collider)`처럼 타입을 명시한다.

종료 경로별 hook(SD20):

| 종료 경로 | Disable | Destroy | Detach |
|---|---|---|---|
| `RequestDestroy`, 부모 파괴, 셀 unload | 활성이면 실행 | 실행 | Destroy 반환 직후 |
| 서브 레벨 unload(`DestroyLevel`) | 활성이면 실행 | 실행 | Destroy 반환 직후. 그 Level의 `LevelScript`가 다른 Actor보다 먼저(LevelScript LD8·LD11) |
| travel | 활성이면 실행 | 실행 | Destroy 반환 직후. 이전 World만 종료하고 Session은 유지 |
| Tryout Stop, process 종료 | 활성이면 실행 | 실행 | Destroy 반환 직후. 그 뒤 CloseSession(§9.1) |
| 격리(§8.3: fault, 필수 `@edit` 참조 대상 제거) | 실행하지 않음 | 그 instance가 파괴될 때 실행 | 격리 시점에 실행·유지 자원·연결 해제(값 필드 보존), Destroy 반환 직후 Detach |
| GC가 도달 불가로 판정 | 실행하지 않음 | 실행하지 않음 | 회수 전 cleanup hook에서 정확히 1회(현재 `Object`에 이 hook이 없으므로 ObjectFoundations·GC 문서의 선행 조건 — 연동 요구 SE-O1). 스크립트 instance는 World 장부에 편입된 객체에만 붙으므로 이 경로는 안전망이다 |

정상 종료 경로의 hook은 Teardown 예산(§9.3) 안에서 실행하며 예산을 넘거나 fault가 나면 그 hook만 끊고 종료를 계속한다. 파괴 경로의 Disable과 Destroy는 safepoint를 열지 않는다(전체 계획 §5.2).

### 5.2 이벤트: on 경로.이벤트

이벤트(클래스 일부):

~~~text
class Health : ScriptableComponent {
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
| Actor 물리 이벤트 | `on GateTrigger.TriggerEntered(other)` | 필드가 가리키는 Actor의 collider 통지. 엔진 `Actor` 이벤트 `TriggerEntered`·`TriggerExited`·`CollisionEntered`·`CollisionExited`(인자는 같은 이름 hook과 같다)이며 같은 물리 batch에서 그 Actor의 hook 다음에 발행한다. hook 이름은 멤버 이름이 될 수 없으므로 과거형이다(LevelScript LD9) | 가리키는 Actor |

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

입력 batch·재개 단계를 실행할 World의 phase 경계 hook은 ER 8절에 계약으로 추가한다(연동 요구 SE-W1). "가변 tick"은 프레임마다 1회, "고정 tick"은 고정 step마다 1회의 실행 시점이며, 재개 순서 key의 tick은 프레임 번호다.

실행 조각 → 발행 큐(FIFO · emit·Changed) → 다음 실행 조각(연쇄 깊이 상한 16) → 활성 전환 적용(World `SetEnabled`, Enable·Disable hook 실행, 이때 등록된 emit은 같은 큐로 전달) → 파괴 적용(World `DestroyActor`·`DestroyComponent` 호출, 실제 hook은 프레임 끝 정리) → 다음 스크립트 단계

- `emit`은 큐에 등록되고, 지금 실행 중인 조각이 반환하거나 `await`에 도달한 뒤 같은 스크립트 단계 안에서 발행 순서대로 전달된다. `World.Spawn`이 실행하는 Awake·Enable만 호출자의 실행 조각 안에서 중첩 실행되며, 그 밖에는 handler가 다른 handler 도중에 끼어드는 재진입이 없다.
- **전달 순회:** 큐 처리기는 항목 하나를 꺼내 그 시점에 연결되어 있고 활성인 handler(C++·Sparks)를 연결 순서대로 모두 실행한 뒤 다음 항목으로 넘어간다. 발행 시점의 연결 목록은 따로 보관하지 않는다. 스크립트 단계 안의 native `Broadcast`도 큐 항목으로 기록한다(binding schema에 선언). 연결 순서는 항목이 delegate에 추가된 순서이며 비활성화·재활성화로 바뀌지 않는다. 배열 원천은 번호 순으로 추가한다.
- **연쇄 상한:** 스크립트 단계를 시작한 실행 조각의 발행은 깊이 1이고 깊이 d인 handler의 발행은 깊이 d+1이다. Start 항목은 새 루트(깊이 1)다. 깊이 17이 될 항목을 등록하려는 조각은 `CascadeLimit` fault가 되고, 같은 루트에서 파생된 미전달 항목만 폐기한다.
- **전달 수와 큐 상한(SD33):** 스크립트 단계당 전달 **항목** 수 상한 N = 65,536이다(handler 호출 수가 아님). 넘는 항목은 다음 스크립트 단계로 이월한다. 연속 60 단계 동안 같은 루트 생성 위치에서 이월이 생기면 그 위치를 `CascadeLimit` fault로 처리하고 Tryout을 일시 정지한다. session 발행 큐 상한은 262,144 항목이다. 스크립트의 `emit`·Changed가 큐를 넘기면 발행한 조각의 `BudgetExceeded` fault이고(조용히 버리지 않는다), 엔진발 항목은 같은 종류를 합치고 넘친 수를 집계한다(§9.4). 값은 프로젝트 설정이며 쿡 시 고정되고 실행 중 바꿀 수 없다.
- **이월 항목의 재검증:** 이월 항목과 스크립트 단계 밖에서 생긴 이벤트는 전달 직전에 인자 객체의 generation을 재검증한다. `?` 없는 객체 인자가 파괴되었으면 그 항목을 폐기하고 위치별로 보고하며(fault 아님), `T?` 인자는 null로 전달한다.
- 파괴는 전달이 끝난 뒤 적용하므로 같은 단계 안에서 이벤트 인자로 전달된 객체는 전달 시점에 유효하다. Disable·Destroy 문맥의 `emit`은 컴파일 오류이며, 적용 중 추가로 생긴 비활성화·파괴는 같은 적용 루프 끝에 요청 순서로 처리한다(64회 상한).
- **일시 정지:** 게임이 일시 정지된 동안에는 입력 batch와 재개 단계(UI 시계 대기만)만 실행하고 Start·Tick 계열·고정 단계는 실행하지 않는다. UI 연출은 `Delay(0.0, Clock.UI)` 반복으로 한다.

### 5.5 연결의 저장과 해제

- **보관 위치:** 발행자 쪽은 Core `MulticastDelegate`다. 이벤트 필드가 없는 확장 이벤트·native 필드 `Changed`·World 이벤트·엔진 Actor 물리 이벤트(`TriggerEntered` 등)는 발행자 객체에 지연 할당하는 이벤트 slot 표에 저장하며 그 객체와 함께 해제된다. 구독이 없는 객체에는 slot 표가 없으므로 Actor 물리 이벤트도 구독이 있을 때만 비용이 든다(1M Actor 기준, LevelScript LD9).
- **항목:** **연결 표**는 session이 소유하는 연결 기록 배열이고 **연결 slot**은 그 원소 번호다. Sparks 연결 항목은 (연결 slot, generation) key와 handler ID만 가지며, 구독 instance의 Detach가 slot의 generation을 올려 key를 무효화한다. delegate thunk는 큐 항목만 기록하며 스크립트 코드는 전달 순회에서만 실행된다(§5.4). revision lease·raw pointer를 보관하지 않는다.
- **비활성화:** Disable은 항목을 삭제하지 않고 비활성으로 표시하며 Enable이 다시 활성화한다. 항목은 Detach, 격리, 원천 필드 변경(해당 발행자분), 선언이 추가·삭제·변경되는 reload, CloseSession에서 삭제된다.
- **구독자가 먼저 끝날 때:** 구독자 Detach가 자기 key를 폐기하면 항목은 즉시 호출 대상에서 제외되고, 발행자의 다음 compact에서 제거된다.
- **발행자가 먼저 끝날 때:** 발행자의 Detach 또는 회수 전 cleanup hook(§5.1, 연동 요구 SE-O1)이 delegate를 비우고, 각 항목의 소멸이 연결 기록을 해제한다. GC 경로로 회수되는 경우에도 cleanup hook이 정확히 1회 실행되므로 누락이 없다.
- **필드·배열 원천:** 필드가 바뀌면(스크립트·로드·복제 모두) 실행 조각이 끝날 때 서로 다른 발행자 집합을 비교해 추가·제거한다. 같은 객체가 배열에 여러 번 있어도 연결은 하나이고 handler는 이벤트마다 1회 호출된다.
- **reload:** `on` 선언이 추가·삭제·변경되면 살아 있는 instance의 연결을 reload 경계에서 다시 구성한다.
- **session 범위:** 스크립트는 자기 session이 소유한 발행자에만 연결한다. 입력 action은 session의 InputConsumerScope 안의 action instance에, 엔진 전역 이벤트는 session World 이벤트 proxy에 연결한다. 공유 에셋은 이벤트를 노출하지 않는다.
- **Core 변경:** 생존 판정을 외부에서 제공하는 회수 가능 대상(C1 — 현재 약한 바인딩은 `SharedPtr`뿐), `TryAdd`의 상한 compact(죽은 항목이 직전 compact 후 살아 있던 수 + 8을 넘지 않음, C2 — 현재 compact 없음), 단일 pass compact(C3 — 현재 `Compact`는 항목을 지울 때마다 처음부터 다시 훑어 최악 O(n²)), `CompactDead`·`Stats`(C4). compact는 할당하지 않는다. C5(`NamePool::TryFind`)는 구현되어 있다.

## 6. 시간과 async

### 6.1 handler 안에서 기다리기

handler 안에서 기다리기(싱글플레이 예):

~~~text
class CutsceneTrigger : ScriptableComponent {
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
- **취소:** 취소는 `await` 지점에서만 일어나고, 실행이 얻은 유지 자원은 엔진이 해제한다. instance 비활성화·파괴, World 종료, Tryout Stop, 호환되지 않는 reload, `ReloadExecutions: Cancel`(§12.4)이 취소 원인이다. 실행 수 예산을 넘어 새 실행이 거부되면 fault가 아니라 건너뛰고 디버거에 횟수를 표시한다. `await`는 실행이 끝날 수 있는 지점이므로 `await` 앞뒤로 나누어 쓴 필드 상태는 취소되면 중간 값에 남는다. 되돌림이 필요하면 `on Disable`에서 권한 측이 정리한다.
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
    let req = Assets.LoadAll<Texture>(Previews);   // 지역 전용 요청 handle (유지 자원)
    while (!req.IsDone) {
        Widget.Progress.Value = req.Progress;
        await NextTick();
    }
    Loaded = try await req;                        // Array<Texture>, 이후 Loaded가 상주를 유지
}
~~~

- `X.Load()`와 `Assets.LoadAll<T>(list)`는 요청 handle `LoadRequest<T>`를 반환하며 `await`의 결과는 `Result<T>`다. 요청 생성은 실패하지 않으며 상한·예산을 넘으면 admission(새 요청의 수락)이 지연된다. 요청 handle은 지역 변수에만 둘 수 있고 한 번만 `await`한다. 상주를 유지하는 방법은 결과 값을 보관하는 것 하나다.
- `LoadAll`은 전부 성공하거나 하나의 오류를 반환하고 전체 진행률을 제공한다.
- 같은 package 요청은 엔진이 하나의 로드로 합친다. 한 실행의 취소는 다른 실행의 요청을 취소하지 않는다(전체 계획 §9.2).
- 결과는 현재 target이 요구하는 준비 mask(CPU 객체·render·physics 등)를 충족한 뒤 반환된다. 임시 fallback 표시 상태를 완료로 보지 않는다. 준비 mask 타입은 아직 코드에 없다(AssetManager 단계 — 연동 요구 SE-A1).
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
- Tryout의 서버 session은 서버 capability와 서버 쿡 filter를 적용해 이 경로를 출시 전에 시험한다.
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
- **LevelScript 코드(초안 1.4):** S0413 `LevelScript`의 `@edit` 객체 필드에 `?`가 없다(§3.2), S0801 `LevelScript`가 물리 hook을 선언했다, S0802 `LevelScript`가 `let` Collider component 필드를 선언했다. 판정 규칙은 §3.12, 메시지와 수정 예는 [LevelScript](LevelScript.ko.md) 4.9절에 있다.
- 진단은 원인 하나를 작성한 코드 그대로 보이고 수정 예를 포함한다. 한 원인에서 파생된 연쇄 오류는 표시하지 않는다.[R23] 수정 예는 이름 규칙(§3.1)을 지키도록 생성한다. 지역 이름은 멤버 이름이 아니라 타입 이름에서 도출한다(`Target: Health?` → `health`). 유사 이름 제시는 Core `EditDistance`와 이름 붙은 문턱 상수를 쓴다.
- **위치:** 줄과 열은 1부터 센다. 열은 그 줄의 처음부터 센 Unicode code point 수이며 tab도 1이다(바이트 수가 아니다 — 주석·문자열에 한글이 있어도 열이 편집기와 맞는다). 언어 서버는 LSP가 요구하는 UTF-16 단위로 바꿔 보낸다. 줄 끝은 LF이고 LF 바로 앞의 CR은 줄 끝의 일부다 — 그래서 CRLF의 CR·LF 위치는 LF만 쓴 파일의 LF 위치와 같은 열(줄 끝)이다. BOM 안의 위치는 1줄 1열이다.
- **출력 세부:** 첫 줄 `경로:줄:열: error S코드: 메시지`, 둘째 줄은 원문 줄을 공백 4칸 들여 보이고(원문 줄의 들여쓰기도 그대로 보인다), 셋째 줄은 공백 4 + (열 − 1)칸 뒤에 `^`를 둔다. `^`의 수는 범위가 그 줄 안에 있는 code point 수이며 최소 1이고, 범위가 줄 끝을 넘으면 그 줄 끝에서 자른다(여러 줄 범위는 첫 줄만 표시, BOM 안에서 시작하는 범위는 BOM 뒤부터 센다). 그 뒤 설명 줄들과 `fix:` 줄은 공백 2칸 들여 쓴다(설명의 빈 줄은 들여쓰지 않는다). 설명이나 수정 예가 없으면 그 줄을 생략한다. 메시지와 수정 예는 한 줄이며(첫 줄을 problem matcher가 읽는다) 설명만 여러 줄일 수 있다. 원문 위치가 없는 진단(파일 전체 — 크기 초과 등)은 첫 줄이 `경로: error S코드: 메시지`이고 원문 줄·밑줄 없이 설명·수정 예를 쓴다. 원문 줄을 보일 때 tab·CR 같은 C0 제어 문자, DEL, C1 제어 문자(U+0080–U+009F), 표시 순서를 바꾸는 bidi 제어 문자(U+061C, U+200E·U+200F, U+202A–U+202E, U+2066–U+2069)는 공백 한 칸으로, 잘못된 UTF-8 byte는 U+FFFD로 바꾼다(열 계산과 맞고, 터미널을 흐트러뜨리거나 원문을 다르게 보이게 하지 않게). 동아시아 넓은 문자는 터미널에서 두 칸을 차지해 `^`가 어긋날 수 있다(표시 한계, 위치 값은 정확하다).
- **순서:** 한 번의 컴파일이 낸 진단은 파일 경로 byte 순 → (같은 경로의 서로 다른 파일이면 더한 순서) → 파일 전체 진단 먼저 → 시작 위치 → 코드 → 낸 순서로 정렬해 낸다. 같은 입력이면 출력이 바이트 단위로 같다.
- **원문 검사(S01xx 앞부분, LC0):** 컴파일러는 원문을 받으면 먼저 다음을 검사한다. 원문 처음의 UTF-8 BOM(EF BB BF)은 허용하고 무시한다(편집기가 붙이는 경우가 많다). 원문 크기 상한은 16 MiB(`MaxSourceBytes`)다.

| 코드 | 이름 | 조건 | 수정 예 |
|---|---|---|---|
| S0101 | `invalid-utf8` | UTF-8이 아닌 바이트 열(overlong·surrogate·U+10FFFF 초과·잘린 sequence). 위치는 처음 잘못된 sequence의 첫 byte(예: `E1 80 5A`면 `E1`)이며 파일당 하나만 낸다 | 파일을 UTF-8로 다시 저장 |
| S0102 | `lone-carriage-return` | LF가 뒤따르지 않는 CR. 줄 끝 해석이 편집기마다 달라지므로 오류다. 처음 하나만 내고, S0101이 있으면 검사하지 않는다. 홀로 선 CR은 줄을 나누지 않는다 | 줄 끝을 LF 또는 CRLF로 |
| S0103 | `source-too-large` | 원문이 16 MiB를 넘는다 | 파일을 나눈다 |

  S0101이나 S0103이 나면 그 파일의 뒤 단계(어휘 분석 이후)를 실행하지 않는다.
- **어휘 분석(S01xx 나머지, LC1):** 원문을 토큰으로 나눈다. 공백은 space·tab·LF·CR이다. 오류가 나도 끝까지 토큰을 만들어(그 자리는 건너뛴다) 구문 분석이 이어지게 한다. 연쇄 오류를 막기 위해 같은 줄에서 같은 코드는 한 번만 낸다.

| 코드 | 이름 | 조건 | 수정 예 |
|---|---|---|---|
| S0104 | `invalid-character` | 문자열·주석 밖에서 쓸 수 없는 문자(ASCII 밖 문자, `'` `#` `\` `` ` `` `~` `^` `$`, 제어 문자, 단독 `&`·`\|`). 단독 `&`·`\|`는 비트 연산자가 없다고 설명한다 | 문자 삭제, 논리 연산은 `&&`·`\|\|` |
| S0105 | `underscore-in-identifier` | 식별자 안의 `_`(`move_speed`, `_x`). 단독 `_`는 따로 토큰이다 | PascalCase·lowerCamel로(`moveSpeed`) |
| S0106 | `unterminated-block-comment` | `/*`가 닫히지 않았다. 위치는 `/*` | `*/` 추가. 블록 주석은 중첩되지 않는다 |
| S0107 | `unterminated-string` | 일반 문자열은 줄 끝이나 파일 끝까지, verbatim 문자열은 파일 끝까지 닫는 `"`가 없다. 위치는 여는 `"`(verbatim은 `@`) | 같은 줄에서 `"`로 닫는다(verbatim은 `"`로 닫는다) |
| S0108 | `invalid-escape` | `\"` `\\` `\n` `\t` `\$` `\u{…}` 밖의 escape, `\u{}`의 자리 수가 0이거나 6을 넘음, surrogate·U+10FFFF 초과 | 허용 escape 목록 제시 |
| S0109 | `integer-literal-too-large` | 정수 리터럴이 쓰는 자리의 타입에 들어가지 않는다(`int`면 int64 — 단항 `-`가 붙은 9223372036854775808만 허용, `uint8`이면 0–255, 부호 없는 자리의 음수). 타입을 아는 검사 단계가 판정한다 | 범위 안의 값이나 더 넓은 타입 |
| S0110 | `malformed-number` | `1.`, 숫자가 없는 지수(`1e`·`1e+`), 숫자가 없는 접두사(`0x`·`0b`), 2진에 쓸 수 없는 숫자(`0b102`), 잘못된 자리의 `_`(`1_`·`1_.5`·`1._5`·`1e_5`), 숫자 바로 뒤의 영문자(`12abc`, 접미사는 S0114) | `1.0`·`1.0e3`·`0xFF`·`1_000` |
| S0114 | `numeric-suffix` | C# 숫자 접미사(`1L`·`1u`·`1.5f`·`1.5d`·`1.5m`) | 접미사를 지운다. 실수가 필요하면 `1.0` |
| S0111 | `string-in-interpolation` | `${ }` 안의 문자열 리터럴 | 지역 변수로 먼저 받는다 |
| S0112 | `range-operator` | `..`(범위는 `..<` 하나다, SD16) | `a..<b` |
| S0113 | `float-literal-out-of-range` | 실수 리터럴이 유한한 double로 표현되지 않는다(`1e400`). 너무 작은 값은 0 또는 비정규 수로 허용 | 범위 안의 값 |

- **토큰:** 식별자, 단독 `_`, 키워드(부록 A — `true`·`false`·`null` 포함), 정수·실수 리터럴, 문자열(시작·조각·`${`·`}`·끝으로 나뉜다), verbatim 문자열(`@"…"` 전체가 한 토큰), 문서 주석 `///`, 구두점 `{ } ( ) [ ] ; , . ?. ? : = == != < <= > >= + - * / % += -= *= /= ! && || ?? ..< @ as?`. `as?`는 `as` 바로 뒤에 공백 없이 `?`가 올 때 한 토큰이다(그 밖의 `as`는 import 별칭 키워드). `>=`는 한 토큰이며 타입 인자를 닫는 자리에서 구문 분석이 둘로 나눈다(§3.6 — 토큰이 `=` 한 글자로 바뀌고 `>`는 타입 인자 노드의 범위에만 남는다). 토큰은 원문 위치만 가지며 값(정수·실수·escape를 푼 문자열)은 구문 분석이 원문에서 다시 읽는다. 어휘 오류 자리에는 Invalid 토큰을 남긴다: 잘못된 문자(그 문자 범위), 닫히지 않은 문자열(닫는 토큰 바로 앞, 빈 범위), 닫히지 않은 verbatim 문자열(그 토큰 뒤, 빈 범위), 닫히지 않은 블록 주석(`/*` 범위). 구문 분석은 Invalid 토큰에서 진단을 더 내지 않는다(아래 "복구").
- **문서 주석:** `///` 바로 뒤가 `/`가 아니면 문서 주석 토큰이고(`////`는 일반 주석), 놓인 자리 검사는 구문 분석이 한다(§3.1).
- **구문 분석(S02xx, LC1):** 토큰을 §3.6 문법의 구문 트리로 만든다. 구문 트리는 원문 위치를 가진 노드 배열이며(노드 배치는 `SparksCompiler/SyntaxTree.h`), 괄호식도 노드로 남겨 formatter와 진단이 원문 모양을 알 수 있게 한다. 문법에 있지만 의미가 제한되는 것(이름 지정 인자를 쓸 수 있는 호출, 함수 결과에 대입 S0731, 프로필별 문법 등)은 구문 분석이 받아들이고 뒤 단계가 판정한다. 노드 종류마다 자식의 자리와 수가 정해져 있고 구문 오류가 있어도 같다(빠진 자리는 Error 노드). 부모 노드의 범위는 자식 범위를 모두 포함하고 형제의 시작 위치는 순서대로다. 구문 트리 덤프(`AppendSyntaxDump`, SD58)는 이 트리를 한 줄에 노드 하나씩 종류·flag·토큰 글자·범위로 적은 결정적 텍스트이며(형식은 `SyntaxTree.h`) corpus의 `.ast` golden(§13.1)이 쓴다.
- **복구(원인 하나에 진단 하나, LC1b에서 확정):**
  - 구문 오류를 내면 복구 상태가 되고, 복구 상태에서는 진단을 더 내지 않는다. 분석은 문법대로 이어 가되 기대한 토큰이 없으면 소비하지 않고 빠진 부분은 Error 노드로 채운다.
  - 문장·멤버·선언 하나를 마쳤을 때 복구 상태이면, 그 단위가 `;`나 `}`로 끝났으면 거기가 동기화 지점이므로 상태만 푼다. 아니면 동기화 지점까지 진단 없이 건너뛴다. 문장은 `;`(소비)·`}`·문장을 시작하는 키워드(`var` `let` `if` `for` `while` `switch` `return` `emit` `break` `continue` `try` `await`), 멤버는 `;`(소비)·`}`·멤버를 시작하는 토큰(`var` `let` `fn` `async` `override` `open` `event` `on` `@`), 선언은 선언을 시작하는 토큰(`import` `class` `struct` `enum` `interface` `event` `const` `fn` `async` `override` `open` `@`)이 동기화 지점이다. 최상위의 `;`·`}`는 그 자체가 오류이므로 건너뛴다. 건너뛸 때 `{ … }`는 짝을 맞춰 통째로 건너뛴다. switch 본문은 `case`·`default`·`}`, enum 본문은 `,`·`}`에서 맞춘다.
  - 본문(블록·클래스·struct·interface·enum·switch)을 여는 `{`를 읽으면 복구 상태가 풀린다. 머리의 오류(`if (x {`)와 본문 안의 오류는 원인이 다르기 때문이다. 머리를 읽고 난 바로 그 자리의 `{`만 본문으로 읽는다 — 머리 오류로 다른 토큰이 남아 있으면(`while (a b) { … }`) 그 본문은 동기화 규칙대로 `{ … }` 통째로 건너뛴다.
  - 파일 끝에서는 복구 상태가 풀리지 않는다. 빠진 `}`·`)`를 같은 원인으로 다시 알리지 않기 위해서다(오류 없이 파일 끝에 닿으면 빠진 `}`는 S0201 하나이며, 여러 겹이 빠져도 하나다).
  - 같은 위치(시작 위치)에는 구문 진단(S02xx)을 하나만 낸다(S0215 제외). 동기화가 오류 난 토큰에서 멈춘 뒤 그 토큰에서 시작한 다음 단위가 같은 자리에서 또 틀리는 경우(`interface` 다음 줄의 `import`, interface 본문의 `override`)다.
  - 선언·멤버·문장을 시작할 수 없는 토큰에서 난 오류(S0203·S0204·S0205)는 그 토큰을 Error 노드로 소비한 뒤 동기화한다(같은 자리에서 같은 진단이 되풀이되지 않게). 본문을 닫는 `}`와 파일 끝은 소비하지 않는다. 그 앞의 문서 주석·attribute는 트리에 남기지 않는다. 이렇게 오류로 소비한 `;`·`}`는 동기화 지점이 아니다(`} x`도 진단 하나).
  - 첫 선언 전(import 단계)의 잡토큰은 S0203이고 import 목록에 Error 노드로 남는다. S0210은 첫 선언(선언을 시작하는 토큰에서 시작한 단위) 뒤의 import다.
  - 이름·타입이 와야 할 자리(S0206·S0207 — 선언 이름, 매개변수·binder, 타입, `.`·`?.` 뒤 멤버 이름)에 앞 토큰과 같은 줄의 키워드가 있으면 이름 자리에 잘못 쓴 것으로 보고 그 키워드를 소비한다(`var class = 1;`의 `class`가 클래스 선언으로, `x.if`의 `if`가 if 문으로 다시 읽히지 않게). 다른 줄이거나 이미 복구 중이면 새 구문의 시작일 수 있으므로 남긴다. attribute 이름 자리(`@class`)는 예외로 소비하지 않는다 — 바로 뒤가 선언 키워드일 수 있기 때문이다.
  - S0213은 `??`와 그 뒤 키워드를 오류의 일부로 소비한다(`return`을 새 문장으로 읽지 않게).
  - 건너뛸 것이 없는 오류는 그 자리에서 내고 복구 상태를 만들지 않는다: `@` 뒤 공백(S0207), S0208, S0209(사슬당 한 번, 왼쪽 결합으로 이어 읽는다), S0210(그 import는 트리에 넣지 않는다), S0211, S0212, S0214(버린 attribute는 트리에 넣지 않는다 — interface 메서드의 것은 남긴다), S0217(case 뒤따름은 switch당 한 번, 두 번째 `default`는 트리에 넣지 않는다), S0218(리터럴당 한 번), S0219, 빈 문장 `;`(S0205), 선언 사이의 `;`(S0203). 복구 중에는 이것들도 내지 않는다.
  - 버린 attribute(S0214) 뒤에 그 자리(문장·매개변수)를 시작할 토큰이 없으면 S0214 하나만 낸다(`fn F() { @edit }`).
  - try·await 값 뒤에 이항 연산자나 `as?`가 이어지면(`let x = await F() + 1;`) 그 키워드 위치에 S0212를 내고 복구 상태가 된다(§3.3 — 지역 변수로 먼저 받는다). 식 안에 잘못 놓인 try·await는 S0212를 그 자리에서 내고 뒤 연산은 이어 읽는다.
  - 현재 토큰이 어휘 오류 자리면 구문 오류를 내지 않고 복구 상태만 된다. 어휘 오류 자리는 Invalid 토큰, 또는 시작 위치에 어휘 진단(S01xx)이 있는 토큰이다(보간 안의 문자열, `_`가 든 식별자 등 — 어휘 분석이 같은 줄의 같은 코드를 한 번만 내므로 그렇게 빠진 토큰은 해당하지 않는다).
  - 문서 주석: 연속한 `///` 줄은 사이에 빈 줄·일반 주석이 있어도 하나의 문서 주석이며, 그다음 토큰이 선언·멤버·enum case를 시작해야 한다. 문서 주석은 줄을 시작해야 한다 — 코드 뒤 같은 줄의 `///`(`var x = 1; /// 설명`)는 다음 선언을 설명하지 않으므로 S0208이다. 문서 주석은 attribute보다 앞에 둔다(`/// 설명` → `@edit` → 선언). S0208은 붙을 자리가 아닌 문서 주석마다(줄마다) 낸다. 복구 중 건너뛴 문서 주석은 알리지 않는다.
- **진단 위치(`.expect`가 비교하는 줄·열):** S0201·S0202·S0203·S0204·S0205·S0206·S0207·S0216은 그 자리의 토큰(파일 끝이면 끝 위치), S0207의 `@` 뒤 공백은 이름 토큰, S0208은 그 `///` 줄, S0209는 두 번째 비교·같음 연산자, S0210은 `import`, S0211은 대입 왼쪽 식의 시작, S0212는 그 `try`·`await`, S0213은 `??`, S0214는 attribute 목록의 처음(`@`), S0215는 넘은 자리의 토큰(트리 깊이면 그 노드의 시작), S0217은 `case` 또는 두 번째 `default`, S0218은 처음으로 종류가 다른 원소의 시작, S0219는 enum 이름이다. 기반 목록·`as?` 대상의 타입 인자·`?`(S0201)는 그 `<`·`?`다.
- **깊이 상한(S0215):**
  - 재귀하는 구문의 중첩은 128단계까지다(`MaxSyntaxNesting`). 단계는 문장 하나, 식 하나(괄호·대괄호·호출 인자·인덱스·보간 안의 식마다 새로 하나), 단항 연산자 하나, `??`의 오른쪽 하나, `try`·`await` 하나, 타입 하나(타입 인자마다 새로 하나), `else if` 하나마다 늘어난다. 넘으면 넘은 자리의 토큰에 S0215를 (복구 중이어도) 내고 그 파일의 나머지는 분석하지 않는다. 악의적이거나 생성된 원문이 컴파일러 stack을 넘치게 하지 못하게 하는 방어다(한 단계가 parser 프레임 여러 개를 쓰므로 Debug 빌드의 기본 stack 1 MiB 안에 들도록 정했다).
  - 구문 트리 전체 깊이는 1024단계까지다(`MaxSyntaxTreeDepth`, 뿌리 File이 1단계, 연산자 사슬 `a + b + c`·`a.b.c`의 연산자도 한 단계씩). 넘으면 끝까지 분석한 뒤 가장 깊은 경로에서 1025단계 노드의 위치에 S0215를 낸다(가장 깊은 자식이 여럿이면 자식 순서에서 앞의 것).
  - S0215가 없는 트리는 뒤 단계(검사·IR 생성·formatter·LSP)가 재귀로 걸어도 된다. 대신 뒤 단계는 재귀 경로의 frame을 작게 둔다: 재귀하는 함수에는 하위 결과 값만 두고 판정·진단 문장(StringBuilder)·임시 값은 재귀가 끝난 뒤 부르는 함수에 둔다(LC2c 타입 검사는 Debug에서 단계당 약 0.5 KiB — 시험 `SparksTypes_DeepChainsStayWithinTheStack`이 1000단계 식 연쇄와 문장 중첩을 돌린다). S0215가 난 파일은 뒤 단계로 넘기지 않는다.

| 코드 | 이름 | 조건 | 수정 예 |
|---|---|---|---|
| S0201 | `expected-token` | 특정 토큰이 와야 할 자리에 다른 것이 있다(`;`·`)`·`}`·`:`·`=` 등). 메시지에 기대한 토큰과 실제 토큰을 쓴다 | 그 토큰을 넣는다 |
| S0202 | `expected-expression` | 식이 와야 할 자리 | 값이나 식을 쓴다 |
| S0203 | `expected-declaration` | 파일 최상위에 선언이 아닌 것 | `class`·`struct`·`enum`·`interface`·`event`·`const`·`fn` |
| S0204 | `expected-member` | 클래스·struct·interface·enum 본문에 멤버가 아닌 것 | `var`·`let`·`fn`·`event`·`on`(종류마다 다르다) |
| S0205 | `expected-statement` | 블록 안에 문장이 아닌 것 | 문장을 쓴다 |
| S0206 | `expected-type` | 타입이 와야 할 자리 | `int`·`Health?`·`Array<Enemy>` |
| S0207 | `expected-name` | 이름(식별자)이 와야 할 자리. `@` 바로 뒤에 공백이 있는 경우 포함 | 이름을 쓴다 |
| S0208 | `misplaced-doc-comment` | `///`가 선언·멤버·enum case 바로 앞이 아니다(§3.1) | `//`로 바꾼다 |
| S0209 | `chained-comparison` | 결합하지 않는 비교·같음 연산자를 잇달아 썼다(`a < b < c`, `a == b == c`) | `a < b && b < c` |
| S0210 | `import-after-declaration` | 선언 뒤의 `import` | import를 파일 처음으로 옮긴다 |
| S0211 | `invalid-assignment-target` | `=`·복합 대입의 왼쪽 맨 바깥이 이름·멤버·원소가 아니다(`a + b = 1`, `F() = 1`, `(a) = 1`, `this = x`). 경로 안의 호출 결과·`?.`·`this.`는 S0731(§3.6) | 대입할 변수나 필드를 쓴다 |
| S0212 | `misplaced-await` | `await`·`try`가 문장 수준(문장 전체, 지역 선언의 초기값, 단순 대입의 우변, `return`)이 아닌 자리, 복합 대입의 우변, 그 값 뒤에 이항 연산자·`as?`가 이어짐(`await F() + 1`), `await try` 순서(§3.3) | 지역 변수로 먼저 받는다 |
| S0213 | `misplaced-exit` | `?? return`이 지역 선언의 초기값 밖에 있거나, `?? break`·`?? continue`(SD39) | `let x = e ?? return;` |
| S0214 | `misplaced-attribute` | 문장 앞처럼 attribute를 둘 수 없는 자리 | attribute를 지운다 |
| S0215 | `nesting-too-deep` | 재귀 구문의 중첩이 128단계를 넘거나(그 파일의 나머지는 분석하지 않는다) 구문 트리 깊이가 1024단계를 넘는다(위 "깊이 상한") | 지역 변수나 함수로 나눈다 |
| S0216 | `range-outside-for` | `..<`가 `for` 머리 밖에 있다(§3.3) | `for (i in a..<b)` |
| S0217 | `misplaced-default` | `switch`의 `default`가 마지막이 아니거나 두 번 나온다 | `default`를 맨 끝에 하나만 |
| S0218 | `mixed-collection-literal` | 컬렉션 리터럴에 값 원소와 `키: 값` 원소가 섞였다(`[1, "a": 2]`) | Array는 값만, Map은 `키: 값`만 |
| S0219 | `empty-enum` | case가 없는 enum(`enum E { }`). 이름이 빠진 enum에는 내지 않고(S0207 하나), case 자리가 하나라도 있으면(그 이름이 빠져도) 내지 않는다 | case를 하나 이상 쓴다 |

- **문법 세부(모호함 없애기):**
  - 빈 문장(`;` 하나)과 단독 블록(`{ … }`을 문장으로 쓰기)은 없다(S0205). 블록은 `if`·`for`·`while`·`switch`·함수·handler의 몸통에만 온다.
  - `if`·`for`·`while`의 몸통은 항상 블록이다(`if (x) return;`은 S0201 — 수정 예는 중괄호로 감싸기).
  - 함수 수식어 순서는 `open`·`override` 중 하나, 그다음 `async`, 그다음 `fn`이다(다른 순서는 S0201).
  - attribute를 둘 수 있는 자리는 최상위 선언, 클래스·struct 멤버, enum case뿐이다. interface 메서드·문장·매개변수 앞은 S0214다.
  - 클래스의 기반 목록은 `Path`다(타입 인자·`?` 없음). 첫 기반은 엔진 타입 또는 Sparks 클래스, 나머지는 interface(full)이며 그 판정은 뒤 단계다. `as?`의 대상도 `Path`다. 둘 다 타입 인자·`?`를 붙이면 그 자리에 S0201을 내고, 그 타입 인자·`?`는 읽고 버린다(본문·나머지 식은 그대로 읽는다).
  - `as`는 import 별칭에만 쓴다. 식 안의 하향 변환은 `as?` 하나뿐이다(`x as T`는 S0201).
  - 최상위 선언 뒤의 `;`(`class A { };`)는 S0203이다.
  - 목록 끝의 쉼표는 enum case 목록만 받는다. 인자·매개변수·컬렉션 리터럴·case 값 목록의 끝 쉼표는 다음 원소가 빠진 것이다(S0202·S0207).
  - `try`와 `await`를 함께 쓸 때는 `try await F()` 순서다(`await try F()`는 S0212, 순서를 고치라는 문장).
  - 식 안의 `?? return`·`?? break`·`?? continue`는 `??`의 오른쪽 피연산자로 읽지 않는다(지역 선언의 Exit이거나 S0213).
- **이름 해석(S03xx, LC2):** import·이름·명명 규칙이다. S0301–S0305는 import 해석(§3.4 "import 해석", LC2a), S0306–S0325는 선언·이름·타입 표기 해석(§3.9, LC2b)이며 위치와 연쇄 방지 규칙은 §3.9에 있다.

| 코드 | 이름 | 조건 | 수정 예 |
|---|---|---|---|
| S0301 | `unknown-module` | import 경로가 현재 패키지·의존 패키지의 모듈이 아니다. 첫 구간이 의존 패키지 이름뿐이면(`import Weapons;`) 패키지는 모듈이 아니라고, 자기 패키지 이름으로 시작하면 패키지 이름 없이 import한다고 설명한다 | 경로를 고친다(비슷한 모듈 이름 제시) |
| S0302 | `undeclared-dependency` | import 경로의 첫 구간이 이 컴파일의 다른 패키지인데 manifest `dependencies`에 없다 | `dependencies`에 그 패키지를 더한다 |
| S0303 | `duplicate-import` | 같은 모듈을 다시 import했다(별칭 유무와 무관, 두 번째 줄부터 줄마다) | 한 줄을 지운다 |
| S0304 | `self-import` | 자기 모듈을 import했다(줄마다) | 지운다 |
| S0305 | `rocks-import` | `Rocks` 패키지를 import했다(엔진 타입·prelude는 자동으로 보인다, SD60) | 지운다 |
| S0306 | `naming-rule` | 이름의 첫 글자가 그 종류의 규칙(§3.1 표 — 타입·멤버·enum case·최상위 선언·별칭은 대문자, 매개변수·지역 이름·binder는 소문자)과 다르다 | 첫 글자를 바꾼 이름 |
| S0307 | `reserved-name` | 사용자 선언 이름이 예약 이름(부록 A — 내장 타입·prelude·핵심 엔진 타입·검증 리터럴·문맥 이름·RPC 접두·`Changed`)이다 | 다른 이름 |
| S0308 | `hook-name` | 수명 클래스의 멤버 이름이 hook 이름·`On`+hook·Unity hook 이름이다 | `on Start()`처럼 hook으로 |
| S0309 | `duplicate-declaration` | 같은 선언 공간에 같은 이름(대소문자 무시)이 다시 나온다(둘째부터) | 한쪽 이름을 바꾼다 |
| S0310 | `name-conflict` | 선언 이름이 다른 출처의 보이는 이름과 겹친다(§3.9 비교 규칙 — 엔진 타입, 최상위 fn·const, import한 선언, 문맥 이름, 둘러싼 지역 이름, Sparks 멤버) | 다른 이름 |
| S0311 | `import-conflict` | import가 도입한 이름이 앞선 import·현재 파일의 최상위 선언과 같거나 별칭이 그것들·엔진 타입과 같다(import 줄당 하나) | 별칭을 붙이거나 이름을 바꾼다 |
| S0312 | `member-type-name` | class·struct 멤버 이름이 보이는 타입 이름과 같다(`var Health: Health?`) | 멤버 이름을 바꾼다(`HealthComp`) |
| S0313 | `hides-inherited-member` | 멤버 이름이 상속 멤버와 같다(`override fn` 예외) | 다른 이름, 재정의 가능하면 `override fn` |
| S0314 | `override-without-target` | `override fn`이 재정의할 상속 함수가 없다(같은 이름이 함수가 아니거나 재정의할 수 없는 경우 포함) | `override`를 지우거나 이름을 고친다 |
| S0315 | `override-signature-mismatch` | `override fn`의 매개변수 타입·수·반환 타입·`async`가 대상과 다르다 | 대상의 시그니처로 |
| S0316 | `misplaced-open` | `open fn`이 `open class` 밖에 있다 | `open`을 지우거나 class에 `open`을 붙인다 |
| S0317 | `unknown-name` | 식의 이름을 찾지 못했다 | import 추가 또는 비슷한 이름 |
| S0318 | `unknown-type` | 타입 이름을 찾지 못했다(별칭 아닌 한정 경로 `Weapons.Sword` 포함) | import 추가, 짧은 이름, 비슷한 이름 |
| S0319 | `not-a-type` | 타입 자리의 이름이 타입이 아니다(prelude 값·문맥 이름·const·fn·멤버 등) | 그 대상의 타입을 쓴다 |
| S0320 | `type-argument-count` | 제네릭 타입의 타입 인자 수가 틀리거나 제네릭이 아닌 타입에 타입 인자가 있다 | `Map<string, int>`처럼 정한 수로 |
| S0321 | `context-name-unavailable` | 그 위치에서 쓸 수 없는 문맥 이름(§3.9 — struct·최상위 fn·필드 초기값의 `World`, `WorldService`의 `Input`, 서버 RPC 밖의 `Caller`) | 문맥 이름이 있는 class 함수로 옮긴다 |
| S0322 | `invalid-base` | 첫 기반이 상속할 수 있는 엔진 타입(`scriptExtensible`)이나 `open class`가 아니거나, 나머지 기반이 interface가 아니거나 거듭된다 | 기반 타입을 고친다 |
| S0323 | `inheritance-cycle` | Sparks 클래스 상속이 순환한다(순환 하나에 하나) | 순환을 끊는다 |
| S0324 | `interface-not-implemented` | class가 기반 interface의 메서드를 같은 이름·시그니처로 구현하지 않았다 | 그 시그니처의 `fn`을 더한다 |
| S0325 | `invalid-service` | manifest `services` 항목이 같은 패키지 모듈의 `WorldService` 파생 최상위 class가 아니다 | 모듈 경로와 class 이름을 고친다 |
| S0326 | `unknown-member` | `.`·`?.` 뒤 멤버나 이름 지정 인자의 필드를 찾지 못했다(LC2c, §3.10) | 비슷한 멤버 이름 |

- **null·타입(S04xx, LC2c)·숫자·상수식(S05xx, LC2c):** 규칙과 위치는 §3.10이다. 위치는 그 식(또는 피연산자·인자·멤버 이름)의 시작이며, S0409는 함수 본문의 닫는 `}`다. S0109(`integer-literal-too-large`)는 어휘 대역 번호이지만 타입을 아는 이 단계가 낸다. S0413은 LevelScript(§8.1 위)이며 LC3에서 검사한다.

| 코드 | 이름 | 조건 | 수정 예 |
|---|---|---|---|
| S0401 | `type-mismatch` | 값의 타입이 자리의 타입에 대입 가능하지 않다(§3.10 "대입 가능"), `?` 타입 인자, `Result<T?>` | 변환하거나 맞는 타입의 값 |
| S0402 | `not-callable` | 함수가 아닌 것을 불렀다, 생성할 수 없는 타입(`constructible: false`, Actor 등 참조 타입, `mat4`) | 함수 이름이나 엔진 생성 API |
| S0403 | `argument-count` | 인자 수가 매개변수 수와 다르다 | 시그니처대로 |
| S0404 | `argument-form` | 함수 호출의 이름 지정 인자, 생성의 위치 인자, 이름 중복, 기본값 없는 필드를 빠뜨림 | 함수는 위치 인자, 생성은 `Field: 값` |
| S0405 | `not-a-value` | 타입 이름·이름 공간·함수·검증 리터럴 이름·`void` 결과를 값으로 썼다, 반환 타입·`Result<void>` 밖의 `void`·`void?` 표기 | 값을 쓴다 |
| S0406 | `invalid-operator` | 연산자가 그 피연산자 타입에 없다(문자열 `+` 포함) | 보간·맞는 타입 |
| S0407 | `not-comparable` | `==`·`!=`를 쓸 수 없는 타입, `?` 없는 값과 `null`, `==`를 쓸 수 없는 Map key·Set 원소 | `Math.NearlyEqual` 등 |
| S0408 | `condition-not-bool` | 조건·논리 연산의 피연산자가 `bool`이 아니다 | `x != null`·`x ?? false` |
| S0409 | `missing-return` | 반환 타입이 있는 함수가 끝나는 문장으로 끝나지 않는다 | `return`을 더한다 |
| S0410 | `return-value` | `void` 함수의 `return 값`, 반환 타입이 있는 함수의 `return;` | 값을 지우거나 더한다 |
| S0411 | `type-argument-rule` | 제네릭 함수의 타입 인자가 없거나 수가 틀리거나 제약을 지키지 않는다, `Soft<T>`·`Class<T>`의 T가 맞지 않는다 | `Find<Health>()` |
| S0412 | `may-be-null` | `T?` 값에 `.`·인덱스를 썼다, `T?` 컬렉션을 `for`로 돌았다 | `?.`, `if (let)`, `??` |
| S0414 | `not-optional` | `?.`·`??`·`if (let)`의 대상이 `T?`가 아니다, `if (let)`의 `bool?` | 그 연산을 지운다 |
| S0415 | `switch-coverage` | enum switch가 case를 빠뜨렸거나 `default`가 있다, enum이 아닌·엔진 `Error` switch에 `default`가 없다 | 빠진 case나 `default` |
| S0416 | `duplicate-case` | 같은 값의 case가 다시 나온다 | 한 case로 |
| S0417 | `no-expected-type` | 기대 타입이 필요한 `[]`·Map 리터럴·`null`·`Asset(...)`·`Row(...)`·`CVar(...)`에 기대 타입이 없다 | 타입을 적는다 |
| S0418 | `mixed-collection-types` | 기대 타입 없는 배열 리터럴의 원소 타입이 다르다 | 타입을 적거나 맞춘다 |
| S0419 | `interpolation-type` | 보간할 수 없는 타입(§3.3) | 그 값의 표시용 문자열 |
| S0420 | `invalid-cast` | `as?`의 피연산자·대상이 맞지 않거나 올림 변환이다 | `as?`를 지운다 |
| S0421 | `await-try-operand` | `await`의 대상이 대기가 아니다, `try`의 대상이 `Result`가 아니다 | 그 키워드를 지운다 |
| S0422 | `unawaited` | 대기(`async fn` 호출·`Delay`·이벤트 등)를 `await` 없이 값으로 썼다, `All`·`Race`의 인자가 결과 없는 대기나 지역 변수의 로드 요청이 아니다(§6.1 SD41) | `await`, 지역 변수 |
| S0423 | `this-misuse` | struct·최상위 fn·const의 `this`, 자기 멤버의 `this.X` | 이름만 쓴다 |
| S0424 | `validated-literal-form` | 검증 리터럴의 첫 인자가 보간 없는 문자열 리터럴이 아니다 | `Tag("A.B")` |
| S0425 | `static-instance-mismatch` | 타입 이름으로 인스턴스 멤버를, 인스턴스로 static 함수를 불렀다 | 맞는 쪽으로 |
| S0426 | `recursive-struct` | struct가 필드로 자신을 값으로 담는다(직접·다른 struct를 거쳐, 순환 하나에 하나) | 컨테이너에 담거나 class로 |
| S0427 | `initializer-too-deep` | 필드·const 초기값이 다른 초기값을 따라가는 연쇄가 64단계를 넘는다(이름 의존은 미리 정하므로 값의 멤버 등으로 이어진 연쇄) | 중간 값을 const·지역으로 나눈다 |
| S0428 | `object-field-needs-optional` | `?` 없는 객체 타입 필드가 §3.11의 허용 자리(component 생성 `let` 필드, Component·Actor 파생 class의 `@edit` 필드)가 아니다 | `T?` |
| S0429 | `struct-default-cycle` | struct 필드 초기값이 생성하는 struct를 따라 같은 struct로 돌아온다(기본값이 끝없다 — §3.11) | 필드를 빈 값(`[]`)으로 시작하고 함수에서 채운다 |
| S0501 | `integer-division` | 정수(상수 포함)끼리의 `/`, 정수 변수의 `/=` | `Math.Div(a, b)`, `float(a) / float(b)` |
| S0502 | `float-to-int-conversion` | 실수 → 정수 타입 변환 호출 | `Math.FloorToInt` 등 |
| S0503 | `constant-overflow` | 타입 없는 정수 상수식이 −2^63..2^64−1을 벗어난다, 정해진 상수 식이 그 타입의 범위를 벗어난다 | 값을 줄인다 |
| S0504 | `constant-division-by-zero` | `%`의 오른쪽, `Math.Div`의 둘째 인자가 값 0인 정수 상수다 | 0이 아닌 값 |
| S0505 | `non-finite-constant` | 실수 상수식이 NaN·Inf다 | 값을 고친다 |
| S0506 | `unsigned-negation` | 부호 없는 정수 타입의 단항 `-` | 부호 있는 타입 |
| S0507 | `mixed-numeric-types` | 다른 숫자 타입끼리 계산·비교했다 | `float(x)` 등으로 맞춘다 |
| S0508 | `invalid-conversion` | 숫자 변환 호출의 인자가 숫자가 아니다, `bool`·`string` 변환 | `Parse.Int` 등 |
| S0509 | `float-constant-in-integer` | 실수 상수를 정수 자리에 썼다 | 정수 상수나 `Math.RoundToInt` |
| S0510 | `not-constant` | const 초기값이 상수식이 아니다, 필드 초기값이 상수식·component 생성이 아니다(§3.11) | 상수식으로, 실행 중 값은 `on Start()`에서 |
| S0511 | `const-cycle` | const 초기값이 순환한다(순환 하나에 하나) | 순환을 끊는다 |
| S0512 | `const-type` | const의 타입이 §3.11의 목록에 없다 | `let` 필드나 함수로 |

- **대입·`let`·미사용(S07xx, LC2d):** 규칙과 위치는 §3.11이다.

| 코드 | 이름 | 조건 | 수정 예 |
|---|---|---|---|
| S0701 | `let-write` | `let`·const 이름·필드에 대입, `let` 값 타입의 멤버·원소 쓰기와 변경 메서드 호출 | `var`로 선언 |
| S0702 | `parameter-write` | 인자·반복 변수·`if (let)` binding을 바꾼다 | 지역 `var`에 복사해 바꾼다 |
| S0703 | `foreign-field-write` | 다른 객체의 Sparks 필드를 쓴다 | 그 class의 함수를 부른다 |
| S0704 | `read-only-property` | 엔진의 읽기 전용 필드·부록 B `let` 속성을 쓴다 | 쓰기 API를 쓴다 |
| S0705 | `struct-self-write` | struct 함수가 자기 필드를 쓴다 | 바꾼 값을 돌려준다 |
| S0706 | `not-assignable` | 문맥 이름·타입 이름·이름 공간·함수·enum case에 대입 | 변수에 대입 |
| S0710 | `unused-local` | 지역 변수·binding·반복 변수를 어디에서도 읽지 않는다 | 지우거나 `_` |
| S0711 | `unread-store` | 지역 `var`에 쓴 값을 그 뒤 어느 경로에서도 읽지 않는다(사본 수정 포함) | 원래 경로에 쓰거나 지운다 |
| S0712 | `unused-parameter` | 본문에서 읽지 않는 인자(시그니처가 정해지지 않은 함수의 `_` 포함) | 지우거나 정해진 자리면 `_` |
| S0713 | `discarded-result` | 식 문장의 값이 `void`가 아니다 | 값을 쓰거나 `_ = ...` |
| S0714 | `unused-import` | import한 모듈의 선언을 쓰지 않는다 | import를 지운다 |
| S0731 | `assign-to-call-result` | 대입·변경 경로에 호출 결과(그 뒤가 참조 타입 값을 지나지 않을 때)나 `?.`가 있다 | 지역 변수에 받아 바꾼 뒤 되돌려 쓴다 |

- **권한·RPC·저장 타입(S10xx, LC3d):** 규칙과 위치는 §3.15다. 저장 호환성(코드 대역의 나머지)은 LC4에서 정한다.

| 코드 | 이름 | 조건 | 수정 예 |
|---|---|---|---|
| S1001 | `not-saveable` | `@save` 필드의 타입이 저장 가능 타입이 아니다 | `Soft<T>`·값 타입 |
| S1002 | `not-replicable` | `@net` 필드·RPC 매개변수가 복제 타입이 아니다, RPC 반환 타입이 `void`가 아니다 | `Soft<T>`·객체 참조 |
| S1003 | `net-write-outside-authority` | 권한 구역 밖의 `@net` 필드 쓰기(직접·호출) | `if (Owner.HasAuthority) { ... }` |
| S1004 | `rpc-call` | `@rpc` 함수를 접두 없이·다른 대상 접두로 부른다, `@rpc`가 아닌 함수에 접두 | `Server.F(...)` |
| S1005 | `data-id-lost` | 패키지 ID manifest의 활성 `type`·`field`·`subobject`·`enumCase` 항목이 현재 선언에도 `@formerly`에도 없다(저장·복제·편집 데이터가 연결을 잃는다 — [SparksBytecode](SparksBytecode.ko.md) 6.1절, LC4d). 위치는 manifest의 그 `name` 값 | 새 이름에 `@formerly("<옛 이름>")`(모듈 이동이면 전체 옛 키)을 붙이거나, `rocks ids rename`으로 옮기거나, `rocks ids retire`로 은퇴시킨다. field와 subobject 사이로 형태만 바뀌었으면 형태를 되돌리거나 은퇴·데이터 이전 |
| S1006 | `invalid-id-manifest` | 패키지 ID manifest(`<패키지>.rocksids.json`)가 공유 형식(`schema: 1`, 항목의 kind·name·id·aliases·retired)이 아니거나, 다른 패키지의 이름·alias, 같은 이름·ID 둘, 주인 type 없는 활성 멤버를 담는다 | 도구로 다시 만든다(손으로 고치지 않는다) |
| S1007 | `id-name-reserved` | 선언의 이름 키가 ID manifest의 alias(다른 ID의 옛 이름)이거나 은퇴한 이름이다([SparksBytecode](SparksBytecode.ko.md) 6.1절). 위치는 선언 이름 | 다른 이름을 쓴다 |
| S1008 | `id-claimed-twice` | `@formerly`가 가리키는 ID를 다른 선언이 이미 쓴다(옛 이름이 아직 선언돼 있거나 두 `@formerly`가 같은 이름). 위치는 `@formerly`를 단 선언 이름 | `@formerly`를 빼거나 다른 선언의 이름을 먼저 바꾼다 |

- **유지 자원·로드(S09xx, LC3c):** 규칙과 위치는 §3.14다.

| 코드 | 이름 | 조건 | 수정 예 |
|---|---|---|---|
| S0901 | `discarded-resource` | 유지 자원·로드 요청을 버린다(식 문장·`_ =`) | 지역 변수나 필드에 담는다 |
| S0902 | `resource-placement` | 담는 자리(§3.14)가 아닌 곳에서 획득한다, 로드 요청을 필드에 담는다 | `let x = ...`로 받는다 |
| S0903 | `resource-moved` | 유지 자원을 담은 이름을 옮기거나 복사한다 | 담긴 이름의 멤버를 호출한다 |
| S0904 | `resource-declaration` | 유지 자원 타입을 담을 수 없는 선언·타입 자리 | `var X: T?` 필드나 지역 변수 |
| S0905 | `resource-without-wait` | 지역 유지 자원 뒤 블록 끝까지 `await`가 없다 | 필드에 담거나 `await Delay(...)` |

- **실행 문맥·효과·attribute(S06xx, LC3b):** 규칙과 위치는 §3.13이다.

| 코드 | 이름 | 조건 | 수정 예 |
|---|---|---|---|
| S0601 | `await-outside-async` | `async`가 아닌 함수의 `await` | `async fn`으로 선언 |
| S0602 | `context-effect` | 문맥(hook·handler·문맥 attribute 함수)이 금지하는 효과를 직접 또는 호출로 쓴다 | 다른 hook으로 옮긴다 |
| S0603 | `while-without-wait` | 대기할 수 없는 본문의 `while`, 회차마다 `await`가 없는 경로 | 반복 안에서 `await NextTick();` |
| S0620 | `unknown-attribute` | 표에 없는 attribute 이름 | 비슷한 이름 |
| S0621 | `attribute-not-allowed` | attribute를 그 선언에 쓸 수 없다, 문맥 attribute가 둘 | attribute를 옮기거나 지운다 |
| S0622 | `duplicate-attribute` | 한 선언에 같은 attribute가 둘 | 하나를 지운다 |
| S0623 | `attribute-argument` | attribute 인자의 수·이름·값이 표와 다르다 | 표의 인자 |

- **이벤트·수명 hook·재진입(S08xx, LC3a):** 규칙과 위치는 §3.12다. S0413(LevelScript의 `?` 없는 `@edit` 객체 필드)도 LC3a가 판정한다.

| 코드 | 이름 | 조건 | 수정 예 |
|---|---|---|---|
| S0413 | `level-script-required-object` | LevelScript의 `@edit` 객체 필드에 `?`가 없다 | `T?` |
| S0801 | `level-script-physics-hook` | LevelScript가 물리 hook을 선언했다 | 트리거 Actor의 `TriggerEntered` 등을 받는다 |
| S0802 | `level-script-collider` | LevelScript가 `let` Collider component 필드를 선언했다 | 트리거 Actor를 배치한다 |
| S0803 | `unknown-hook` | 구간 하나짜리 `on X()`의 X가 hook 이름이 아니다 | 비슷한 hook 이름, 자기 이벤트면 함수를 직접 부른다 |
| S0804 | `hook-unavailable` | 수명 클래스가 아닌 곳의 handler, 물리 hook을 받을 수 없는 class의 물리 hook | 수명 타입에서 파생한다 |
| S0805 | `hook-signature` | hook 인자가 정해진 목록의 앞부분이 아니다(수·타입) | `on Tick(dt: float)` 등 |
| S0806 | `duplicate-handler` | 같은 class의 같은 hook, class 계층의 같은 `on` 경로 | 하나로 합친다 |
| S0807 | `unknown-event-source` | `on`·`emit` 경로의 원천을 해석하지 못한다 | 비슷한 필드 이름 |
| S0808 | `unknown-event` | 원천에 그 이벤트가 없다 | 비슷한 이벤트 이름 |
| S0809 | `changed-unavailable` | `Changed`가 없는 필드의 `Changed`, `Changed`라는 이벤트 선언 | `var` 필드, 다른 이벤트 이름 |
| S0810 | `handler-signature` | handler 인자가 이벤트 인자의 앞부분이 아니다(수·타입) | 이벤트 인자 타입 그대로 |
| S0811 | `emit-not-allowed` | 이 class가 선언하지 않은 멤버 이벤트·엔진 이벤트의 `emit` | 선언한 class의 함수를 부른다 |
| S0812 | `event-call` | 식에서 이벤트를 호출 형태로 썼다 | `emit X.E(...)` 또는 `await X.E` |
| S0813 | `extension-event-target` | 확장 이벤트의 대상이 `Actor`·`World`가 아니다 | `event Actor.X(...)` |
| S0814 | `duplicate-extension-event` | 확장 이벤트 이름이 대상 타입의 이벤트·다른 확장 이벤트와 같다 | 다른 이름 |
| S0815 | `extension-event-not-imported` | 확장 이벤트를 선언한 모듈을 직접 import하지 않았다 | 그 모듈의 import |
| S0816 | `missing-reentrancy-policy` | `await`가 있는 `on` 경로 handler·물리 hook에 재진입 표시가 없다 | `@skipWhileBusy` 또는 `@restart` |
| S0817 | `misplaced-reentrancy-policy` | 재진입 표시가 둘이거나 `await` 없는 handler·수명 hook·함수에 있다 | 표시를 지운다 |

- **패키지(S11xx, LC2a):** manifest·모듈 경로·패키지 의존·binding schema 파일이다. 위치 규칙은 §3.4와 §10.2 "binding schema 파일 형식"에 있다. 코드 대역의 나머지(reload·backend)는 해당 마일스톤에서 더한다.

| 코드 | 이름 | 조건 | 수정 예 |
|---|---|---|---|
| S1101 | `manifest-syntax` | manifest가 JSON5 객체 하나가 아니다(문법 오류, 중복 key, 최상위가 객체가 아님) | JSON5 문법을 고친다 |
| S1102 | `unknown-manifest-key` | 알 수 없는 key | 그 key를 지운다(비슷한 key 제시) |
| S1103 | `missing-manifest-key` | 필수 key(`name`·`sparks`·`profile`)가 없다 | 그 key를 더한다 |
| S1104 | `invalid-manifest-value` | 값의 종류·형식이 틀리다(이름 규칙, `profile` 값, 배열 원소, 중복, 자기 자신·`Rocks` 의존, 길이) | 표(§3.4)의 형식대로 |
| S1105 | `unsupported-language-version` | `sparks`가 이 컴파일러가 아는 언어 버전이 아니다(지금은 1뿐) | `sparks: 1` |
| S1106 | `missing-dependency` | 의존 패키지가 이 컴파일에 없다(이름이 없는 패키지가 있으면 내지 않는다) | 패키지 이름을 고치거나 패키지를 더한다 |
| S1107 | `dependency-cycle` | 패키지 사이 의존이 순환한다(강하게 연결된 묶음 하나에 하나, 메시지에 순환 경로) | 순환을 끊는다 |
| S1108 | `invalid-module-path` | 모듈 경로가 규칙에 맞지 않는다(구간 이름, `.spark` 확장자) | 대문자로 시작하는 영문자·숫자 이름으로 |
| S1109 | `module-path-case-conflict` | 대소문자만 다른 두 경로(처음 철자가 아닌 쪽의 모듈 파일마다) | 한쪽 이름을 바꾼다 |
| S1110 | `module-folder-conflict` | 같은 폴더의 `X.spark`와 폴더 `X/` | 한쪽 이름을 바꾼다 |
| S1111 | `package-folder-conflict` | 의존 패키지 이름(과 `Rocks`)이 현재 패키지의 최상위 폴더·최상위 모듈 이름과 같다 | 폴더 이름을 바꾼다 |
| S1112 | `duplicate-package` | 한 컴파일에 같은 이름(ASCII 대소문자만 다른 것 포함)의 패키지가 둘 있다 | 한쪽 이름을 바꾼다 |
| S1113 | `full-profile-only` | `profile: "core"` 패키지에서 full 문법(§1.3·§3.11)을 썼다 | `profile: "full"` 또는 core 대안 |
| S1120 | `invalid-binding-schema` | binding schema 파일이 §10.2 형식이 아니다(문법, key, kind가 허용하지 않는 key, 값, 이름 규칙·키워드, ID 표기·중복, 타입 표기 문법, 중복 이름, 파일 사이 타입 이름·ID·모듈 중복) | 생성기를 다시 돌린다(손으로 고치지 않는다) |
| S1150 | `function-too-large` | 함수가 bytecode 한계를 넘는다(bank마다 동시에 쓰는 값 256 slot, 명령 32,767개 — [SparksBytecode](SparksBytecode.ko.md) 4.2절, LC4b). 위치는 함수 이름 | 함수를 작은 함수로 나눈다 |

- 사전 대조 리터럴(`Tag`·`Action`·`CVar`)이 사전에서 이름이 바뀐 경우 사전의 redirect(`@formerly`와 같은 역할)를 따라 자동 변환하고 빠른 수정으로 소스를 고친다. redirect도 없으면 오류다.

### 8.2 Result와 try

- 조회 실패는 `T?`, 작업 실패는 `Result<T>`다. 결과를 사용하지 않으면 오류다. 직접 처리할 때는 `r.Value`·`r.Error`를 확인한다.
- `Result<T>`를 반환하는 함수는 성공을 `return 값;`, 실패를 `return Error.NotFound;`처럼 쓴다. `Error`는 엔진이 정한 닫힌 enum이며(case 목록은 부록 B) 사용자 정의 오류 타입은 없다(SD38).
- `try`는 실패하면 실행을 더 진행하지 않는다. `Result`를 반환하는 함수에서는 실패를 호출자에게 전달한다. 그 밖의 함수와 handler에서는 현재 실행(Execution 트리 전체)을 EndExec로 끝내고 위치별로 1회 진단을 기록한다. `try` 실패는 fault가 아니므로 격리 집계와 Tryout 정지에 포함하지 않는다. 예외로 hook이 절반만 실행된 채 남지 않도록 `Awake`·`Enable`·`Start` 안의 `try` 실패는 그 hook의 fault다(SD43).
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
- **보고:** fault는 (모듈 hash, 함수, 위치, 종류)별로 1회 상세 보고하고 이후 횟수만 집계한다. Tryout은 새 fault 위치에서 일시 정지한다. 출시 빌드는 hash·위치를 기록해 오프라인에서 소스 위치로 변환한다.
- 격리는 Sparks 쪽 상태이며 World는 모른다. 스크립트 bridge는 격리된 instance에 대한 엔진의 `SetEnabled(true)`·OnEnable을 무시하고, Tick 중지는 `SetTickEnabled(false)`로 구현하므로 Disable·Enable로 풀리지 않는다. 이 bridge 계약을 ER 7절에 추가한다(§17.2).
- 개발·출시 빌드와 두 backend(VM·AOT)는 같은 검사를 수행한다. 게임 실행에서 이미 엔진에 요청한 외부 효과는 되돌리지 않는다(전체 계획 §23.4). 편집기 문맥은 §8.4를 따른다.

### 8.4 오류 방어 경계

사용자 코드의 어떤 실수도 process·편집기·다른 instance·저장 데이터를 망가뜨리지 않는 것이 목표다(SD32). 범위는 컴파일 → 로드 → 실행 → 편집기 → 출시 순으로 좁혀 막는다.

| 경계 | 규칙 |
|---|---|
| 컴파일 | 오류가 있으면 실행 중인 게임은 마지막 성공 revision을 유지한다. 오류가 있는 상태로 Tryout을 시작하면 마지막 성공 revision으로 실행할지 묻고, 성공 revision이 없으면 그 패키지의 클래스를 데이터 보존 placeholder로 둔다. 쿡은 실패한다 |
| 로드 | `.sparkc`가 verifier에서 거부되거나 interface·binding hash가 맞지 않으면 그 모듈의 클래스를 쓰는 prefab은 원래 필드를 보존하는 placeholder component로 로드하고 게임은 계속 실행한다(§15의 placeholder 규칙 확장) |
| 실행 | fault는 실행 트리 → instance 순으로만 번진다(§8.3). VM 내부 불변식 위반(verifier를 통과한 bytecode의 VM 버그)만 엔진 버그로 크래시 보고하며 사용자 코드 원인은 모두 fault다 |
| native 경계 | 스크립트에 노출하는 C++ 함수는 호출자 입력(인자·대상 상태)으로 Fatal·`check`·assert하지 않고 `Result`나 `ContractViolation`으로 돌려준다. binding thunk는 schema가 선언한 전제조건(non-null·범위·lane·권한·PendingDestroy 여부)을 호출 전에 모두 검사한다. 노출 함수의 인자 검증에 Fatal을 쓰면 policy 검사로 막는다(binding schema `NoFatalOnInput`, §10.2). Borrow는 thunk 안에서만 얻고 fault로 빠져나갈 때도 RAII로 반환한다 |
| 권한 | `@net` 필드 쓰기는 권한 구역(§14)에서만 컴파일된다. 실행 시 권한이 없으면 쓰기를 무시하고 위치별 1회 보고한다(fault 아님 — 클라이언트 연출이 멈추지 않게) |
| 폭주 | 무한 반복은 명령어 예산, 깊은 재귀는 호출 깊이 256과 native stack 검사, 대량 생성은 조각당 1,024·단계당 8,192 Spawn 상한, 이벤트 폭은 전달 수 상한과 연속 이월 검사(§5.4), 로그 폭주는 호출 위치별 초당 10회 상한(초과분은 횟수만 집계)으로 막는다. Tryout에서는 같은 생성 위치의 살아 있는 Actor가 100,000을 넘으면 일시 정지하고 보고한다 |
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
| GC가 도달 불가로 판정 | 회수 전 cleanup hook에서 Detach(hook 없음, 정확히 1회 보장, 전체 계획 §5.2 — 연동 요구 SE-O1) |
| 셀 일괄 unload | Disable·Destroy, 각 Destroy 반환 직후 Detach. 발행자별 compact는 셀 단위 1회 |
| travel | 이전 World 정상 종료(Disable·Destroy·Detach). Session은 유지 |
| Tryout Stop | World 정상 종료(Disable·Destroy) 후 CloseSession |
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
| 일회성 효과 | 엔진 subsystem | 최대 지속 시간 후 스스로 종료. CloseSession(Tryout Stop 포함)이 시작되면 재생 중인 것을 즉시 중지하고 이후 호출은 효과 없이 반환 |
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
- **G1 · Tryout Stop:** CloseSession 직전 연결 표 참조계수 1, CloseSession 후 모든 스크립트 counter 0, revision lease와 이름 pool이 기준선 복귀(전체 계획 §29.6 NL09).
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

Sparks에서는 `var MaxHealth: int32`(편집·저장), 읽기 전용 속성 `Current: int32`, `event Died(killer: Actor?)`, `fn ApplyDamage(amount: int32, instigator: Actor?): Result<void>`로 보인다(Studio hover 표기, 소스 문법이 아님).

| 예시의 이름 | 현재 상태 | 선행 |
|---|---|---|
| `ROCKS_CLASS`·`ROCKS_PROPERTY`·`ROCKS_GENERATED_BODY`, `SaveGame`·`ReadOnly`·`Name =` | 있음 | — |
| `ScriptExtensible`, `ScriptName`, `ScriptWritable` | 없음 | 신설(L1, 메타 컴파일러) |
| `Edit`(편집 메타), `Params` | 없음 | UiToolkit CU1·CU3 |
| `ROCKS_FUNCTION`과 `Lane`·`Effects` 등 | 없음 | CU2(CVar 문서의 `Cheat`·`Authority`·`Help`와 지정자 이름 공간을 함께 정한다) |
| `Replicated` | 없음 | 네트워크 단계(RX 0절 범위 밖) |
| `FieldKind::Event` | 없음 | CU3 |

**Sparks 필드와 엔진 필드 플래그.** Sparks 클래스의 필드는 C++ 필드가 아니라 스크립트 instance 저장소에 놓이며, 런타임에 등록하는 schema(동적 schema — 연동 요구 SE-O3)로 리플렉션·저장·override·Changed에 참여한다. attribute는 다음 플래그에 대응한다.

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

#### binding schema 파일 형식(LC2a 확정, SD61)

파일 하나는 엔진 C++ 모듈 하나의 바인딩이며 이름은 `<모듈>.sparkbind.json`이다. 엄격한 JSON(Core `JsonSyntax::Strict`, 생성기는 Canonical 출력)이고 알 수 없는 key·빠진 필수 key·값 종류 오류는 S1120이다(위치는 그 값의 시작, 빠진 key는 그 객체의 시작). 생성기(EI1, 연동 요구 SE-R4)는 이 형식을 쓴다. 표의 key는 LC2(이름·타입)가 쓰는 것이며, 효과·lane·자원 정책·파괴 지연 등 §10.2 표의 나머지는 그것을 쓰는 마일스톤(LC3 이후)이 key를 더하면서 이 표에 적는다(형식 번호는 EI1 출시 전까지 1을 유지한다).

| 위치 | key | 필수 | 값 |
|---|---|---|---|
| 최상위 | `format` | 예 | `1` |
| 최상위 | `package` | 예 | 패키지 이름. 지금은 엔진 패키지 `Rocks`뿐이다(엔진 플러그인 패키지는 그 단계에서 정한다 — 그 밖의 이름은 S1120). 여러 파일이 같은 패키지를 이룬다 |
| 최상위 | `module` | 예 | C++ 모듈 이름(ASCII 영문자·숫자, 영문 대문자로 시작) |
| 최상위 | `types` | 예 | 타입 객체 배열 |
| 타입 | `name` | 예 | 스크립트 타입 이름(영문 대문자로 시작하는 ASCII 영문자·숫자). 한 패키지 안에서 중복 불가 |
| 타입 | `id` | 예 | TypeId. `.rocksids.json`과 같은 32자리 소문자 16진(모두 `0`은 안 된다). 모든 schema에서 중복 불가(FieldId·FunctionId와도) |
| 타입 | `kind` | 예 | `object`(엔진 객체 — Actor·Component·Service·WidgetController 계열), `asset`(에셋 타입), `struct`(native 값 struct), `enum`, `interface`(CU9) |
| 타입 | `base` | 아니오 | 기반 타입 이름. `object`·`asset`·`interface`만 쓴다 |
| 타입 | `scriptExtensible` | 아니오 | bool(기본 false). Sparks class가 상속할 수 있는지. `object`·`asset`만 |
| 타입 | `constructible` | 아니오 | bool(기본 true). 이름 지정 인자로 생성할 수 있는지. `struct`만 |
| 타입 | `fields` | 아니오 | 필드 객체 배열. `object`·`asset`·`struct`만 |
| 타입 | `functions` | 아니오 | 함수 객체 배열. `enum` 밖 |
| 타입 | `events` | 아니오 | 이벤트 객체 배열. `object`만 |
| 타입 | `held` | 아니오 | bool(기본 false). 이 타입의 값이 유지 자원인지(§3.14, LC3c). `object`만 |
| 타입 | `cases` | `enum`이면 예 | case 이름 문자열 배열(선언 순서, 하나 이상, 중복 불가, 영문 대문자로 시작). `enum`만 |
| 필드 | `name` | 예 | §10.3 변환을 마친 스크립트 이름(영문 대문자로 시작) |
| 필드 | `id` | `computed`가 아니면 예 | FieldId |
| 필드 | `type` | 예 | 타입 표기(아래) |
| 필드 | `access` | 아니오 | `"read"`(기본) 또는 `"readWrite"`. 엔진 객체 필드의 `readWrite`는 `ScriptWritable`이고, struct 필드의 `readWrite`는 대입할 수 있는 경로(§4.4)로 쓸 수 있다는 뜻이다 |
| 필드 | `computed` | 아니오 | bool(기본 false). getter·setter로 만든 계산 속성(§10.2) |
| 필드 | `changed` | 아니오 | bool(기본 false). `X.Changed`를 받을 수 있는지(CU5) |
| 함수 | `name` | 예 | 스크립트 이름(영문 대문자로 시작, `TryX` → `X` 변환을 마친 것) |
| 함수 | `id` | 예 | FunctionId |
| 함수 | `static` | 아니오 | bool(기본 false). 타입 이름으로 부르는 함수 |
| 함수 | `async` | 아니오 | bool(기본 false). `await`로 부르는 함수 |
| 함수 | `overridable` | 아니오 | bool(기본 false). Sparks class가 `override fn`으로 재정의할 수 있는 가상 함수인지(LC2b). `object`·`asset` 타입의 `static`이 아닌 함수만 |
| 함수 | `typeParameters` | 아니오 | `{ "name": 영문 대문자 하나로 시작하는 이름, "constraint": 기반 타입 이름(생략 가능) }` 배열(`Find<T: Component>` 꼴) |
| 함수 | `parameters` | 아니오 | `{ "name": lowerCamel 이름(키워드 아님), "type": 타입 표기, "perSecond": bool(기본 false) }` 배열. 이름 중복 불가 |
| 함수 | `returns` | 아니오 | 반환 타입 표기. 없으면 `void` |
| 함수 | `effects` | 아니오 | 효과 문자열 배열(`spawn`·`acquire`·`readiness`·`io`·`wallClock`, 중복 불가 — §3.13, LC3b) |
| 함수 | `pure` | 아니오 | bool(기본 false). 관찰 가능한 상태를 바꾸지 않는 함수(§3.13) |
| 함수 | `oneShot` | 아니오 | bool(기본 false). handle 없는 일회성 효과(§3.13·§7.1) |
| 이벤트 | `name` | 예 | 이벤트 이름(영문 대문자로 시작) |
| 이벤트 | `id` | 예 | FieldId(`FieldKind::Event`) |
| 이벤트 | `parameters` | 아니오 | `{ "name": lowerCamel 이름(키워드 아님), "type": 타입 표기 }` 배열. 이름 중복 불가 |
| 이벤트 | `publisher` | 아니오 | `"server"`·`"owner"`·`"all"`(기본) |

- **이름(LC2b):** 타입 이름은 언어 소유 이름(내장 타입·prelude 타입·prelude 값 — §3.9 표)일 수 없다(S1120 — 그 파일을 쓰지 않는다). 타입 표기·`base`·`constraint`의 이름 해석과 그 오류는 §3.9 "타입 표기 해석"이다.
- **타입 표기:** §3.6 `Type`과 같은 문자열의 정규형이다(`int32`, `Actor?`, `Array<Actor>`, `Map<string, int32>`, `Result<void>`, `Soft<SoundAsset>`, 타입 매개변수 `T?`). 문법은 `표기 = 이름 {"." 이름} ["<" 표기 {", " 표기} ">"] ["?"]`이고 공백은 쉼표 뒤 한 칸만 쓴다(생성기 출력이 하나로 정해지게). 이름은 ASCII 영문자로 시작하는 영문자·숫자이며 키워드(§3.1)일 수 없고, 꺾쇠 중첩은 §8.1 구문 깊이 상한(128)까지다. schema reader는 표기의 문법만 검사하고(S1120), 이름이 가리키는 타입(같은·의존 패키지의 타입, prelude 타입, 내장 타입, 그 함수의 타입 매개변수)은 이름 해석(LC2b)이 검사한다.
- 한 타입 안의 필드·함수·이벤트 이름은 한 이름 공간이다(§3.1). 중복은 메타 컴파일러가 막지만 reader도 S1120으로 거부한다(손으로 쓴 schema·손상된 파일 방어).
- 타입의 `kind`가 허용하지 않는 key(표의 "만" 조건)는 S1120 하나만 내고 그 값은 검사하지 않는다(진단이 겹치지 않게). `kind`가 없거나 틀리면 어느 key도 거부하지 않고 모든 값을 검사한다(`cases`가 하나 이상이어야 한다는 규칙은 `enum`일 때만).
- 한 파일 안의 중복(타입 이름, 한 타입의 멤버 이름, 매개변수·타입 매개변수 이름, enum case, ID)은 원문에서 뒤에 나온 것마다 하나씩 낸다(세 번 나오면 둘).
- 한 컴파일의 schema 파일들은 패키지 하나의 타입 이름 공간과 ID 공간을 함께 쓰고, C++ 모듈 하나에 파일 하나다. 파일을 더한 순서로 훑으며 앞서 받아들인 파일하고만 비교한다(이미 거부한 파일과 겹치는 것은 문제 삼지 않는다). 겹치는 타입 이름(그 `name` 값)·ID(그 `id` 값)·`module` 값(그 값)마다 S1120을 내고 그 파일을 쓰지 않는다. 형식 오류가 있는 파일과 원문 검사가 막은 파일(S0101·S0103)도 쓰지 않는다.

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
- **3개 bank:** Scalar(태그 없는 64비트 — 좁은 정수 타입도 64비트 칸에 두고 그 타입의 범위로 overflow를 검사하는 타입별 opcode를 쓴다), Ref(엔진 객체 handle+generation), Buf(참조계수 버퍼). 스크립트 상태는 GC root를 만들지 않으며 재개 시 `this`의 generation을 확인한다. 호출 깊이는 256 frame까지다(§9.3).
- **필드 쓰기 opcode**는 Changed 연결 여부를 확인하고 실행 조각 안의 첫 쓰기에서 변경 후보로 기록한다(조각 끝 값 비교, §5.3). 정수 overflow·부동소수점 유한성을 검사하고 FMA 축약을 쓰지 않는다.
- **safepoint:** back-edge, 호출 복귀, `await`, 할당. Teardown 문맥과 파괴 경로의 Disable에서는 safepoint를 열지 않는다(전체 계획 §5.2). Borrow는 native call thunk 안에서만 얻고 반환 전 해제한다(전체 계획 §6.3).
- **verifier:** 로드 시 선형 1회.[R4] 구조(범위·branch·bank 크기), 타입(bank·TypeId·초기화·non-null), 바인딩(FunctionId·시그니처 hash·lane·자원 정책), 문맥(금지 호출, `await` 위치, eval capability)을 검사한다.
- **덤프(필수, SD58):** 검증을 위해 컴파일 결과를 사람이 읽고 golden으로 비교할 수 있는 텍스트로 낸다. 단계마다 하나씩 둔다.
  - **구문 트리 덤프(LC1):** 노드 종류·원문 위치·토큰 글자를 들여쓰기 트리로 쓴다. parser 시험과 corpus의 기대값으로 쓴다.
  - **bytecode 덤프(LC4):** 모듈 header(format·compiler version·content hash·interface hash), 상수 표, 타입·필드·이벤트 표, 함수마다 bank 크기·실행 문맥·await 지점별 레지스터 형태, 그리고 명령마다 주소·opcode 이름·피연산자(레지스터·상수·분기 대상 이름)·원문 위치(`줄:열`)를 쓴다(아래 bytecode 예시와 같은 모양). `.sparkc`를 읽어 다시 쓰는 덤프는 컴파일 직후의 덤프와 바이트 단위로 같아야 한다.
  - **typed IR 덤프(LC4):** bank 할당 전 중간 표현을 쓴다(최적화·AOT 생성의 입력 검증용).
  - 덤프 형식은 영어 텍스트이고 결정적이다(같은 입력 → 같은 바이트, 주소·hash 외에 실행마다 달라지는 값 없음). 형식 변경은 시험 golden과 함께 바꾼다. CLI `sparks dump --ast|--ir|--bytecode`(LC8)와 conformance `.expect`의 덤프 비교 지시가 쓴다. 덤프는 개발 도구이며 출시 실행 파일에 링크하지 않아도 되도록 VM 실행과 분리한다.
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

- TypeId·FieldId·SubobjectGuid·FunctionId·RpcId는 C++와 같은 `.rocksids.json` 형식을 쓴다(전체 계획 §7.1). 현재 메타 컴파일러는 빌드와 분리된 `ids add/rename/retire` 저작 명령으로만 발급하고 이름 키는 C++ 정규 이름이며 kind는 type·field·enum 계열뿐이다. Sparks 컴파일러는 자기 패키지 manifest의 **두 번째 발급자**로서 모듈 경로 기반 이름 키(`<패키지>/<경로>.<선언>`)와 subobject·function·rpc kind를 쓴다(StableIdManifest 확장, 연동 요구 SE-R8). 개발 빌드에서는 저장 후 컴파일이 새 선언의 ID를 발급하고, 입력 중인 미완성 선언에는 발급하지 않는다.
- 데이터가 연결된 ID(`@save`·`@net`·`@edit` 필드, 소유 component, enum case)의 소실은 매 컴파일의 오류이며 빠른 수정이 `@formerly("OldName")`을 삽입한다. `@formerly`는 manifest의 aliases 항목으로 기록된다. 의도한 삭제는 `rocks ids retire`(AppInfoCommandLine 14절 commandlet)로 tombstone을 남긴다. enum은 case 이름으로 저장하므로 case 이름 변경에도 `@formerly`를 쓴다.
- FunctionId는 이벤트 handler·RPC·`@cheat`·`@editorButton`·리플렉션 호출 대상·interface 구현 함수·엔진 가상 함수의 `override fn`에 발급한다.

### 12.3 증분 컴파일과 쿡

파일마다 interface hash와 함수별 body hash를 분리하고 본문 변경은 해당 함수만 다시 컴파일한다(전체 계획 §27.2·§27.3). interface hash에는 선언 시그니처뿐 아니라 `const` 값과 함수별 추론 효과 집합(§6.2)을 포함하므로, 본문 변경으로 효과가 바뀌면 호출하는 모듈이 다시 검사된다. Tag·Action·CVar 사전과 WidgetAsset schema도 그것을 쓰는 모듈의 입력이다. 쿡은 전체 컴파일·검증, 리터럴 재검증, WidgetAsset 경로 대조, 하드 의존 예산, 소프트 대상 chunk 배정, prefab·save·RPC 호환 검사, 디버그 정보 분리를 수행한다.

### 12.4 핫 리로드

핫 리로드는 필수 기능이다(사용자 요구). 편집기·개발 빌드에서 `.spark`를 저장하면 Rocks Build의 Sparks compile action(SD6)이 다시 컴파일하고, 실행 중인 모든 World(Tryout의 모든 process 포함)에 새 revision을 적용한다. 출시 빌드에는 컴파일러가 없으므로 reload가 없다(eval 정책과 별개).

**등급.** 변경마다 가장 가벼운 등급으로 처리한다(SD28).

| 등급 | 대상 변경 | 처리 | 도입 |
|---|---|---|---|
| R1 본문 교체 | 함수·handler 본문 | 새 revision을 게시한다. 이후 시작하는 handler·호출은 새 revision을 쓴다. 대기 중인 실행은 `ReloadExecutions`(아래)를 따른다 | LC7 |
| R2 제자리 migration | Sparks 필드 추가·삭제·형식 변경, `on` 선언 추가·삭제·변경, 필드 기본값, `const`, Tag·Action·CVar 사전, WidgetAsset schema | reload 경계에서 FieldId migration·구독 연결 재구성. 사전·schema 변경은 그것을 쓰는 모듈을 다시 컴파일한 뒤 적용한다 | LC7(Sparks 필드·`on`·const), 사전·schema는 EI5 |
| R3 instance 재생성 | 클래스 계층(기반 타입) 변경, `let` component 구성 변경(subobject 추가·삭제), struct 배치가 바뀌어 제자리 변환이 불가능한 경우 | 현재 필드 값을 FieldId별로 보관 → 정상 종료 hook(Disable·Destroy)과 Detach → 새 클래스로 재생성(subobject는 Prefab 3.2절 표대로, 삭제된 SubobjectGuid를 가리키던 prefab override는 Prefab 3.7절 고아 보존) → 보관한 값을 spawn parameter 단계에 덮어쓰기 → Awake·Enable. 같은 Actor graph의 재생성은 한 묶음이다. 편집 World(Tryout 밖)에 배치된 instance도 같은 경로로 재생성한다(UE reinstancing 대응) | L2 |
| R4 복제 schema 교체 | `@net` 필드 추가·삭제·형식 변경, `@rpc` 추가·삭제·시그니처 변경 | `@rpc` 추가는 RpcId manifest에 덧붙여 즉시 반영한다. 그 밖에는 클래스의 복제 schema hash를 올리고 모든 Tryout process가 같은 프레임 경계에서 새 revision을 적용한다. 삭제·변경된 RpcId의 대기 메시지는 폐기·보고하고, 서버는 해당 클래스 instance의 전체 상태를 late join 경로(§5.3 활성화 전 적용 규칙)로 다시 보내며, 클라이언트는 hash가 맞을 때까지 그 instance의 복제 수신을 보류한다 | L3 |
| Tryout 재시작 | 엔진 C++ 변경, 패키지 의존 graph 변경 | 재시작 안내(자동 재시작하지 않음) | — |

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

**대기 중인 실행(SD30).** 실행을 새 코드로 자동 이전하지 않는다(§20.3 #6). 프로젝트 설정 `ReloadExecutions: Keep | Cancel`이 정하며 Tryout 기본값은 `Cancel`이다.

- `Keep`: 대기 중인 실행은 이전 revision으로 끝까지 실행하고 그 실행이 하는 호출도 이전 revision 안에서 해석한다(전체 계획 §23.4).
- `Cancel`: 본문이 바뀐 함수를 실행 스택에 가진 실행을 취소한다. 취소된 Start는 처음부터 다시 실행하고, 이벤트 handler는 다음 이벤트를 기다린다. `while` + `await NextTick()` 반복이 이전 코드로 계속 도는 일을 막는다.
- R2·R3에서 변경·삭제된 FieldId를 bytecode에서 참조하거나 삭제된 handler에 속한 실행은 설정과 무관하게 취소한다.
- 모듈별 살아 있는 revision은 8개까지다. 넘으면 가장 오래된 revision의 실행을 취소하고 보고한다.

**편집기 표시.** reload마다 요약을 보인다: 적용된 revision, 등급별 대상 수, 취소된 실행 수, 해제된 자원, 손실이 있었던 변환, 재생성·격리된 instance, 다시 시작한 Tick, 이전 revision으로 실행 중인 handler 수와 재시작 명령. 실패한 reload는 원인 진단과 "이전 revision 유지 중"을 표시한다.

**엔진 쪽 계약(연동 요구 SE-O2·O3·A3·W1·W5).** 같은 TypeId 아래 여러 revision 레코드의 공존과 instance별 revision 포인터·경계에서의 교체·마지막 lease 반환 시 retire(RCM "모듈 reload"의 Busy 거부를 대체), 스크립트 필드의 동적 schema 등록과 graph encode/decode·override 경로·Changed/CU5 통지, live instance의 subobject 추가·삭제, World 프레임의 reload 경계 hook.

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
- 편집기와 Tryout은 항상 VM으로 실행한다(핫 리로드). 개발 빌드에서 `Aot`를 고르면 아래 혼합 규칙으로 reload를 유지한다.
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
- **서명:** 쿡 패키지 안의 `.sparkc`는 콘텐츠 서명(RSIG)이 이미 덮는다. 외부 모듈 서명은 RSIG 형식을 재사용하되 label `"rocks.sparks-module.v1\0"`, 모드 서명 전용 키 목록, `contentVersion` 하한과 `scope`를 갖는 별도 봉투다. Release가 매니페스트 밖 입력을 읽지 않는다는 ContentSigning 1절에 `ExternalModules=Signed` 경로를 예외로 등록하고, `ScriptEval=On`인 출시는 매니페스트 밖 입력을 받는다는 점을 target 결정 기록에 남긴다(연동 요구 SE-B6).
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
class DoorLogic : ScriptableComponent {
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
- **편집기 명령 채널(LevelScript LD14):** 편집기(Rocks Studio)는 `sparks-lsp`에 LSP `workspace/executeCommand`(서버 명령 `sparks.levelScript.addReference`·`sparks.levelScript.addEvent`)로 "레벨 스크립트에 참조 추가"·"이벤트 추가"를 요청하고, 서버는 넣을 위치를 정해 `workspace/applyEdit`로 VS Code·Script Editor에 편집을 보낸다(서버는 파일을 직접 쓰지 않는다). 편집기는 새 revision의 컴파일 완료 통지를 받은 뒤 필드 값을 지정한다. "레벨 스크립트 열기"의 생성 경로 `<패키지>/Levels/<레벨 경로 이름>.spark`와 이름 변환 규칙은 [LevelScript](LevelScript.ko.md) 4.8절이다.
- 도입 단계: L1 진단·의미 강조, L2 자동완성(멤버·hook·attribute·리터럴·경로)·hover·정의 이동·빠른 수정·서식·추론 타입 표시, L3 ID를 유지하는 rename·참조 찾기·VS Code 디버거.
- **디버거(DAP, L3):** `await`를 넘는 step(같은 실행의 다음 조각으로), 조건 breakpoint(Pure 문맥으로 평가), watch는 고정된 버퍼를 읽음(§9.4), breakpoint 중 저장은 재개 후 첫 reload 경계에 적용(§12.4), Tryout의 여러 process 동시 연결, pause 중 큐 상한(§9.4), 출시 빌드의 오프라인 위치 변환(§8.3·§12.5).

### 13.1 시험 작성

- `@test fn`(full 또는 패키지 manifest의 test 패키지)은 headless World fixture에서 실행한다. 시험 시계는 `NextTick`·`Delay`를 결정적으로 진행하며 실제 시간에 기대지 않는다(sleep 금지). `Assert`가 실패 판정이다.
- CLI `sparks test <패키지>`가 실행하고 결과는 진단과 같은 형식으로 보고한다. 같은 corpus를 VM·AOT 차등 시험(G6)에도 쓴다.
- **conformance corpus:** `Conformance/<영역>/<사례>.spark`와 `.expect`. `.expect`에는 진단 코드·줄·열, 또는 실행 trace(전달 순서·fault 종류·위치 ID·ledger counter)를 적는다. 본문의 "오류다" 문장과 SD마다 음성 사례를 1개 이상 둔다. 영역은 진단 코드 대역(§8.1)과 같다.
- **`.expect` 형식(LC0):** UTF-8 텍스트(처음의 BOM은 무시, UTF-8이 아니면 형식 오류)이며 한 줄에 지시 하나다. 단어는 공백이나 tab으로 나누고, 숫자는 0–9만 쓴다(`+` 부호 없음). 빈 줄과 `#`로 시작하는 줄은 주석이다. 사례 이름(stem)에는 점이 들어가도 되며 `.spark`와 `.expect`는 stem으로 짝을 맞춘다.
  - `error S0412 14:14` — 이 위치에 이 코드의 진단이 나야 한다. 여러 줄이면 §8.1의 순서대로 모두 일치해야 하며 빠지거나 더 나온 진단은 실패다.
  - `error S0103` — 위치를 적지 않으면 원문 위치가 없는 진단(파일 전체)이다.
  - `clean` — 진단이 없어야 한다. `error`와 함께 쓸 수 없다.
  - `stage syntax`·`stage imports`·`stage names`·`stage types`·`stage flow`·`stage rules`·`stage codegen` — 그 단계까지만 컴파일한다(`syntax`는 manifest·schema·모듈 경로·의존·어휘·구문 분석, `imports`는 거기에 import 해석, `names`는 거기에 이름 해석, `types`는 거기에 타입 검사 — LC2c, `flow`는 거기에 흐름·대입·미사용·상수식·필드·프로필 검사 — LC2d §3.11, `rules`는 거기에 문맥·수명 규칙 — LC3 §3.12–, `codegen`은 거기에 진단 없는 컴파일의 typed IR 내림 — LC4, [SparksBytecode](SparksBytecode.ko.md)). 사례는 `.ir`(typed IR 덤프 golden — 진단 없는 사례만)도 둘 수 있다. 한 번만 쓰며 없으면 전체 단계다. 구문 사례(`Lexical`·`Syntax`·`SyntaxErrors` 영역)와 이름 사례(`Names` 영역)가 뒤 단계 규칙(이름·타입·미사용)에 묶이지 않게 한다(LC2b·LC2c). 단계만으로는 기대 지시가 아니다(`error`나 `clean`이 함께 있어야 한다).
  - 지시가 하나도 없는 `.expect`, 알 수 없는 지시, `.expect`가 없는 `.spark`, `.spark`가 없는 `.expect`는 corpus 오류로 실패한다(빈 기대값이 조용히 통과하지 않게 한다).
  - 실행 결과 지시(`result`·`fault`·`trace`)는 해당 마일스톤(LC5·LC6)에서 더한다.
- **패키지 사례(LC2a):** 사례는 파일 하나(`<사례>.spark`) 또는 폴더 하나(`<사례>/`)다.
  - 파일 사례는 manifest가 `{ name: "Test", sparks: 1, profile: "full" }`인 패키지의 모듈 하나(모듈 경로 `Main` — 사례 이름에는 점이 들어갈 수 있으므로 사례 이름과 무관하다)로 컴파일한다. 진단 경로는 `<영역>/<사례>.spark`이고 `.expect`의 `error` 지시에는 경로를 쓰지 않는다.
  - 폴더 사례는 폴더 바로 아래에 `SparksPackage.json5`가 있으면 그 폴더가 패키지 하나이고, 없으면 바로 아래 폴더 하나하나가 패키지다(각자 `SparksPackage.json5` 필수). 패키지 안에는 `.spark`와 `SparksPackage.json5`(루트)만 둔다. `.expect`는 폴더와 같은 stem으로 영역에 두며 `error` 지시에 사례 폴더 기준 경로(`/` 구분)를 쓴다: `error S0301 Game/Combat/Score.spark 3:8`, 파일 전체 진단은 `error S1108 Game/combat.spark`. 진단 경로도 사례 폴더 기준이다. 그 밖의 파일, 파일이 하나도 없는 사례 폴더, manifest 없는 패키지 폴더, 패키지 루트가 아닌 곳의 `SparksPackage.json5`는 corpus 오류다(모듈이 없는 패키지 — manifest만 있는 폴더 — 는 쓸 수 있다).
  - 모든 사례는 시험용 가짜 엔진 schema(`SparksTestHost/Schema/*.sparkbind.json`, 패키지 `Rocks`)를 함께 컴파일한다. 진단 경로는 `Schema/<파일 이름>`이다.
  - `.ast`(구문 트리 golden)는 파일 사례에만 둔다(폴더 사례 옆의 `.ast`는 corpus 오류).
- **이름 해석 golden(LC2b, SD58):** 사례 옆에 `<사례>.names`가 있으면 이름 해석 덤프(`AppendNameDump`, 형식은 `Compilation.h`)가 파일 내용과 바이트 단위로 같아야 한다. 파일 사례는 그 모듈의 덤프, 폴더 사례는 이름 해석을 한 모듈마다 `== <사례 폴더 기준 경로>` 줄과 그 덤프를 경로 byte 순으로 이은 것이다. 판정·실패 보고는 `.ast`와 같다.
- **식 타입 golden(LC2c, SD58):** 사례 옆에 `<사례>.types`가 있으면 식 타입 덤프(`AppendTypeDump`, §3.10)가 파일 내용과 바이트 단위로 같아야 한다. 묶는 방식(파일·폴더 사례)과 판정·실패 보고는 `.names`와 같고, 타입 검사를 하지 않은 사례(`stage`가 `types`보다 앞)에 `.types`가 있으면 실패다.
- **구문 트리 golden(LC1b, SD58):** 사례 옆에 `<사례>.ast`가 있으면 그 사례의 구문 트리 덤프(`AppendSyntaxDump`, 형식은 `SyntaxTree.h`)가 파일 내용과 바이트 단위로 같아야 한다(끝의 LF 포함). `.ast`는 선택이며 `.spark` 없이 있으면 corpus 오류이고, 원문 검사가 막아(S0101·S0103) 트리가 없는 사례에 `.ast`가 있어도 실패다. 진단(`.expect`)과 트리는 따로 판정한다. 다르면 첫 다른 줄과 실제 트리 전체를 보인다(검토 뒤 golden 갱신에 쓴다). 사례 실행은 컴파일(LC2a부터 manifest·schema·모듈 경로·어휘·구문 분석·import 해석, 마일스톤마다 늘어난다)이다. IR·bytecode 덤프 golden도 같은 방식(`.ir`·`.bc` — `.bc`는 `.sparkc` 왕복도 본다)이다. 실행 golden `.run`(LC5 — [SparksVm](SparksVm.ko.md) 7절)은 진단 없는 파일 사례의 최상위 함수 호출을 줄마다 적고, 실행기가 `.sparkc` 왕복을 거친 모듈을 VM으로 실행해 결과(값·fault 종류와 위치·실행 끝)와 명령 수를 다시 써서 비교한다.
- corpus는 `Engine/Source/Developer/SparksTestHost/Conformance`에 두고 시험이 실행 중 엔진 디렉터리 탐색으로 찾는다. 저장소가 줄 끝을 LF로 정규화하므로 CRLF·잘못된 UTF-8·BOM처럼 바이트가 중요한 사례는 C++ 시험에 직접 넣는다.
- 제안 목표(미측정): 파일 1,000개 기준 자동완성 p95 100 ms, 편집 후 진단 p95 300 ms. 검증: 후보 golden test, prefix fuzz, CLI와의 진단 동등성, rename 후 저장 데이터 보존.

## 14. 네트워크

replication · RPC:

~~~text
class Weapon : ScriptableComponent {
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

class Chest : ScriptableComponent {
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

- **저장 가능 타입:** `bool`·모든 숫자 타입·수학 타입·`string`·enum·`Tag`·`TagSet`·`Soft<T>`·`Row<T>`, 이들의 `T?`, 그리고 이들로만 구성된 struct·컨테이너. 엔진 객체(자기 component 포함)·로드한 에셋 값·`Text`·`Result`는 저장할 수 없다(전체 계획 §7.2). C++ 대응(리플렉션 B9 — ReflectionExtensions 19절): `Tag` → Name, `TagSet` → `OrderedSet<Name>`, `Row<T>` → Name, `T?`(단일 값·문자열·struct) → `Optional<T>`, `Tag?`·`Row<T>?`는 None Name(없음은 하나 — SD19).
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

개정 대상은 전체 계획, 입력 상세안, UI 툴킷, Core 구현 코드다. 위치 열의 절 번호는 접두어가 없으면 전체 계획의 절이다. "현재 문구"는 전체 계획 개정 1.6 기준이며(개정 1.8 행은 1.7 기준), 문서 항목은 모두 전체 계획 개정 1.7·1.8, 입력 상세안, UI 툴킷에 반영하였다. 수명 단계 이름(BeginPlay 등)을 쓰는 전체 계획 §5.3과 입력 상세안 586행은 엔진 내부 단계 이름이므로 그대로 두고 전체 계획 §20.2의 대응표로 연결한다. Core 구현 코드 C1–C4는 연동 단계 EI3의 선행이다(연동 요구 SE-C1–C4, C5는 구현됨).

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
| §29.6 NL03·NL04·NL06 | Tryout 1,000회 등 | NL09 신설(G1–G4 Sparks Gate), 무기한 재시도 대신 모듈 사용 중지·실패 보고 |
| UiToolkit CU2·CU3·CU5·CU8, CU9 신설 | UI 사용처 중심 | 언어 L1 선행. CU3은 `MulticastDelegate` 필드와 `FieldKind::Event`. CU9는 §10.3에서 정의 |
| 입력 상세안 §22.3 | subscription이 module/script lease 보유, VM reload 시 해제 | 연결 항목은 handler ID만 보유, Disable은 비활성 표시, 선언이 바뀌는 reload에서만 재구성 |
| Core `Delegate.h`·`NamePool.h`(구현) | 약한 바인딩은 `SharedPtr`만, TryAdd compact 없음 | C1–C4(C5 `TryFind`는 구현됨) |
| §23.3·§2.4 | Studio language service, 언어 경계 명명 보류 | LSP·DAP, 패키지 `Rocks`, 필드 첫 글자 대문자 변환 |
| §2.4 C++ 오버로드(개정 1.8) | 서로 다른 `ScriptName`이 있어야 노출 | 노출 함수의 오버로드에 서로 다른 `ScriptName`이 없으면 메타 컴파일 오류 |
| §5.2·§20.2·§23.2(개정 1.8) | 스크립트가 요청한 파괴만 지연, 연쇄 단계당 16회, Enable·Start fault는 instance 비활성화 | 스크립트 단계 동안의 모든 파괴 요청 지연, 인과 깊이 16, 격리, 종료 경로별 hook(SD20), 재진입 정책 표시(SD11) |
| §22.6 session 구성(개정 1.8) | VM globals·coroutine·root | 스크립트 실행·연결 표·대기(전역 변수와 GC root 없음) |
| §23.4 conformance(개정 1.8) | 순환 import를 오류 사례로 열거 | 패키지 간 순환 의존(파일 간 순환 참조는 허용) |

### 17.2 다른 문서의 후속 개정

엔진·도구 **코드**에 필요한 사항은 [Sparks 엔진 연동 요구 목록](SparksEngineIntegration.ko.md)이 정본이다(초안 1.5에서 분리). 항목마다 현재 상태와 그것이 필요한 연동 단계를 적고, 엔진이 준비되는 대로 하나씩 더한다. 이 절에는 다른 **문서**의 개정만 남긴다.

| 대상 | 필요한 변경 | 시점 |
|---|---|---|
| 전체 계획 §5.2·§9.2·§20.2–20.5·§23 | Entity → Actor, TransformComponent → SceneComponent, `OnAwake(ComponentContext&)` → 현재 시그니처, tick group 이름을 World tick phase로(ER 2절이 예고한 개정의 미반영분), §23의 도입 단계표를 언어 코어·엔진 연동 두 갈래(§18)로, "LLVM 기반 AOT"를 스크립팅 backend 선택(SD31)으로, §29.6 NL09에 G5·G6 추가 | 다음 전체 계획 개정 |
| ER 7.2·7.4·8절 | Component 시작 준비 hook과 Start bridge 계약(SD24), 파괴·활성 bridge 큐와 격리 bridge 계약(SD25, §8.3), 스크립트 단계 hook(§5.4) — 연동 요구 목록 SE-W1–W3의 설계 | EI3 전 |
| ObjectFoundations·GC 대규모 처리 | 회수 전 cleanup hook(SE-O1)의 설계 | EI2 전 |
| RCM·TypeRegistry | 타입 revision과 스크립트 클래스 등록(SE-O2·O3)의 설계 | EI2 전 |
| Prefab | Sparks reload(R3)에 따른 subobject 표 변경 행, live instance의 subobject 추가·삭제 | EI6 전 |
| UiToolkit CU1–CU9 | CU9 규격(§10.3) 반영, CU1 attribute 대응(§10.1), CU5 계산 속성 setter 통지(§10.2) | EI1 전 |
| CompilerDiagnostics | 엔진 공통 진단 코드 등록부(Sparks S0100–S1199, 메타 컴파일러 RMC####, UI validate 코드) | LC0와 함께 |
| ContentSigning 1절 | `ExternalModules=Signed` 예외와 모듈 서명 봉투(§12.7) | EI9 전 |
| CVarSystem 14.5절 | Sparks 노출 문법(§10.4) 반영 | 다음 CVar 문서 개정 |
| 게임 타입 자리표시 | `Character`·`Collider`·`SphereCollider`·`SoundAsset`·`Sequence`·`DataAsset`·`ScriptableComponent`·`WidgetController`·`PlaySession`의 Rocks 이름 확정(Rocks 고유 이름 원칙) | 해당 모듈 진입 시 |
| ID 개념 이름 | `SoftEntityId`·영속 Entity Guid·파생 Entity ID를 Actor 계열 이름으로 바꿀지 Prefab과 함께 결정 | Prefab PF 단계 |
| (초안 1.4) 편집기 | Outliner "레벨 스크립트" 항목, 레벨 스크립트 명령·템플릿(전체 계획 §22.3), `sparks-lsp` 명령 `sparks.levelScript.*`(§13) | 편집기 단계 |

끝난 항목: 코드의 `WeakRef<T>` → `WeakObjectRef<T>` 개명(커밋 `e0e78953`, SD23), Core C5 `NamePool::TryFind`.

## 18. 도입 단계

구현은 두 갈래로 나눈다(SD51, 2026-11-20 사용자 결정). 엔진 요소와 무관한 **언어 코어**(LC0–LC9)를 먼저 만들고, 엔진 쪽이 준비되는 대로 **엔진 연동**(EI1–EI9)을 하나씩 더한다. 언어 코어는 엔진의 다른 작업(리플렉션 확장·World·에셋)을 기다리지 않고, 그 작업들도 Sparks를 기다리지 않는다.

### 18.1 언어 코어와 host 경계

언어 코어가 엔진 없이 완결되도록 경계를 둘로 정한다(SD52).

- **binding schema는 파일이다.** 컴파일러는 엔진 API를 C++ 리플렉션에서 직접 읽지 않고 binding schema 파일(`<모듈>.sparkbind.json`)과 사전 파일(Tag·Action·CVar·에셋·`Loc` catalog)에서만 읽는다. 내용은 §10.2의 표(타입·필드·계산 속성·함수·자원 정책·파괴 지연·이벤트)다. 언어 코어는 손으로 쓴 시험용 schema(가짜 엔진: `Actor`·`Component`·`ScriptableComponent`·`SceneComponent`와 §2 예시가 쓰는 타입, `Engine/Source/Developer/SparksTestHost/Schema/*.sparkbind.json`)로 개발하고, 실제 엔진 선언에서 이 파일을 생성하는 것은 연동 단계 EI1이다. 언어 서버가 엔진 없이 동작하는 것(§13)도 같은 파일 덕분이다. 파일 형식은 §10.2 "binding schema 파일 형식"(LC2a 확정)이다.
- **VM은 엔진을 host interface로만 부른다.** VM이 밖으로 부르는 것은 객체 handle의 생존·타입 판정, native 필드 읽기·쓰기, native 함수 호출(FunctionId와 bank 창), native 이벤트 연결·해제, 파괴·활성 전환 적용 요청(§4.3), 유지 자원 해제, 로드 요청과 lease 반환, 시계(§6.1), 로그·fault 보고다. host가 VM에 들어오는 것은 instance 생성·Detach, 수명 hook 실행, 스크립트 단계 열기·닫기, 이벤트 항목 넣기, 재개, revision 게시, CloseSession이다. 엔진의 `ScriptRuntime` 모듈(EI2)이 이 interface를 ObjectRuntime·World 위에 구현한다. 세부 API는 LC5(1차)·LC6(2차) 착수 때 이 절에 확정한다.
- **Sparks 필드의 저장소는 VM이 가진다.** Sparks 클래스의 필드는 VM의 instance 저장소에 있고, 엔진에는 연동 단계에서 런타임 타입 등록으로 보인다(연동 요구 SE-O3). 그래서 필드 migration(R2)까지 언어 코어 안에서 완결된다.
- **가짜 host로 시험한다.** `SparksTestHost`는 가짜 객체표(handle + generation), 결정적 시험 시계, 수명 hook·tick phase driver, 가짜 native 함수·이벤트·유지 자원·로드를 가진 headless host다. conformance corpus(§13.1)와 `@test`가 이 host에서 돈다. 실제 시간에 기대지 않는다.

모듈 배치(SD53):

| 모듈 | 위치 | 내용 | 의존 |
|---|---|---|---|
| `SparksVm` | `Engine/Source/Runtime` | bytecode와 `.sparkc` 형식, verifier, interpreter, 값 버퍼, 실행·발행 큐·ledger, 핫 리로드, host interface | Core |
| `SparksCompiler` | `Engine/Source/Runtime` | 원문 관리, 진단, lexer, parser, 검사기, typed IR, bytecode 생성, schema·manifest reader, formatter | Core(LC4부터 SparksVm) |
| `SparksTestHost` | `Engine/Source/Developer` | 가짜 host, conformance runner | Core, SparksCompiler(LC5부터 SparksVm) |
| `Sparks`(CLI) | `Programs` | `sparks check`·`build`·`test`·`fmt` | 위 모듈, Launch |
| `SparksLsp` | `Programs` | 언어 서버 `sparks-lsp` | SparksCompiler, Launch |
| `ScriptRuntime`(연동, EI2) | `Engine/Source/Runtime` | host 구현, native call thunk, Sparks 클래스의 타입 등록 | SparksVm, ObjectRuntime, MetaSchema, Task |

- 모듈은 필요해질 때 만든다. LC0은 `SparksCompiler`(원문·진단)와 `SparksTestHost`(conformance runner)만 만들고, `SparksVm`은 bytecode 형식이 생기는 LC4에서 만든다(빈 자리표시 모듈을 두지 않는다).
- 언어 모듈을 Engine 층에 두는 이유는 Core 층을 가볍게 유지하기 위해서다(엔진 기능 모듈은 Engine 층 — SpatialIndex와 같다). 의존은 Core뿐이므로 World·RHI 없이 링크된다.
- `SparksCompiler`가 Runtime에 있는 이유는 런타임 eval이 켜진 target의 실행 파일에 들어갈 수 있어야 하기 때문이다(SD5). `ScriptEval=Off`인 target은 링크하지 않는다(§12.6).
- 언어 코어가 쓰는 기반은 모두 이미 있다: Core의 컨테이너(`Array`·`HashMap`·`OrderedMap`·`OrderedSet`·`Span`), 문자열(`String`·`StringView`·`StringBuilder`의 최단 실수 표기·`Unicode`·`NumberText`·`EditDistance`·`Path`), `ArenaAllocator`·`BudgetAllocator`, `Result`, `Hash`·`Sha256`, `Json`(읽기·쓰기, JSON5 읽기), `ByteReader`·`ByteWriter`, `RandomStream`, 로그, Base의 overflow 검사 도우미(`Base/Checked.h`), PlatformBase의 파일 API(읽기·디렉터리 열거)와 원자 연산.
- 시험은 기존 관례를 따른다. conformance runner의 `TEST`는 `SparksTestHost/Tests`에 두고 EngineTests에 등록한다(`TestSupport`는 모듈이 아니라 시험 실행 파일이 직접 끌어오는 소스다). corpus는 `SparksTestHost/Conformance`에 두고 실행 중 엔진 디렉터리 탐색(`HostConfig::FindEngineDirectory`)으로 찾는다. 저장소가 줄 끝을 LF로 정규화하므로(`.gitattributes`) CRLF·잘못된 UTF-8처럼 바이트가 중요한 사례는 C++ 시험에 직접 넣는다.

### 18.2 언어 코어 마일스톤

각 마일스톤은 conformance corpus의 해당 영역이 통과하는 것으로 끝난다. LC0–LC7과 LC9는 밖의 선행 작업이 없고, LC8만 표준 입출력 API(연동 요구 SE-B7)를 기다린다. "함께 하는 일"은 Sparks 쪽이 그 마일스톤에서 직접 하는 다른 모듈·문서의 작은 변경이다.

| 마일스톤 | 내용 | 완료 기준 | 밖의 선행 |
|---|---|---|---|
| LC0 기반 | 모듈 뼈대, 소스 관리(UTF-8 검증·줄·열), 진단 인프라(§8.1 형식·S 코드 표·수정 예), conformance runner(`.spark` + `.expect`), TestHost 뼈대 | 진단 출력 형식 golden, runner 동작 | 없음. 함께 하는 일: 진단 코드 등록부 문서(SE-R9) |
| LC1 어휘·구문 | lexer(§3.1·§3.6, LC1a), 오류 허용 parser와 구문 트리(core·full 문법 전부, LC1b), 구문 트리 덤프(SD58). 진단 S01xx·S02xx | §3.6의 생성 규칙마다 양성·음성 사례, 깨진 입력에서 연쇄 오류 없이 복구 | 없음 |
| LC2 이름·타입 | binding schema 형식과 reader, 시험용 가짜 엔진 schema, 패키지 manifest·import, prelude와 부록 B 선언, 이름 해석(§3.1·§3.4), 타입 검사(§3.2·§3.3·§4 — 숫자, null 흐름, await 이후 `T?`, 대입, `let`, 미사용), 상수식, 프로필 검사. 진단 S03xx–S05xx·S07xx·S11xx | 해당 절의 "오류다" 문장마다 음성 사례, §2 예시가 시험용 schema로 통과 | 없음 |
| LC3 문맥·수명 규칙 | 효과 추론과 실행 문맥(§6.2), hook·이벤트 선언 검사(§5), 재진입 표시(SD11), 유지 자원 규칙(§7.1), 권한 구역(SD45), 저장 가능 타입과 호환성(§15). 진단 S06xx·S08xx–S10xx | 컴파일러 앞단 완성 — 본문의 "오류다" 문장과 SD 전부에 음성 사례 | 없음 |
| LC4 IR·bytecode | typed IR, bank 할당, 32비트 명령 인코딩, `.sparkc` 읽기·쓰기(§12.1), verifier(§11), 패키지 ID manifest(§12.2), interface·body hash(§12.3), **typed IR·bytecode 덤프(SD58)** | 같은 입력 → 같은 바이트, 변조 bytecode의 verifier 거부, 재컴파일 범위 시험, 덤프 golden(`.sparkc`를 읽어 다시 쓴 덤프와 일치) | 없음. 함께 하는 일: 메타 컴파일러 `ids` 파서에 Sparks kind·이름 키 추가(SE-R8의 파서 부분 — CI가 모든 `.rocksids.json`을 검사하므로 그 전에는 Sparks ID manifest를 커밋하지 않는다) |
| LC5 VM 핵심 | interpreter(Scalar·Ref·Buf bank), 값 버퍼(COW 참조계수), 숫자 검사와 fault(§8.3), 예산(명령어·호출 깊이·메모리), 부록 B 표준 API, host interface 1차(native call·객체 handle 검증·native 필드) | 동기 함수 corpus의 결과·fault·예산 계수 golden, 종료 시 메모리 ledger 0 | 없음 |
| LC6 실행 모델 | instance와 수명 hook, 스크립트 단계·발행 큐·`emit`·`Changed`·연쇄 상한(§5.4), async 실행·`await`·`All`/`Race`·취소(§6.1), 유지 자원 해제(§7.1), 파괴·활성 지연 큐(§4.3), 소유 범위와 EndExec·Detach·CloseSession(§9), fault 처리와 격리(§8.3), host interface 2차(이벤트·자원·로드·시계) | TestHost에서 §2 예시 시나리오의 전달 순서 golden, Gate G1·G4 | 없음 |
| LC7 핫 리로드 | R1 본문 교체, R2 스크립트 필드 migration과 구독 연결 재구성, `ReloadExecutions`, 계획·판정·원자적 적용의 언어 쪽(§12.4), 증분 재컴파일 | Gate G5의 R1·R2 행, 본문 reload 2 s 측정 | 없음 |
| LC8 도구 | CLI(`sparks check·build·test·fmt·dump`), formatter, `@test` 실행, 언어 서버(진단·의미 강조 → 자동완성·hover·빠른 수정) | CLI와 언어 서버의 진단 동등성, 자동완성 후보 golden | 표준 입출력 API(연동 요구 SE-B7 — CLI의 표준 출력, 언어 서버의 표준 입력). formatter와 `@test` 실행기 라이브러리는 없음 |
| LC9 AOT 생성 | typed IR → C++ 생성기, 동등성 규칙(§12.5), VM·AOT 혼합 진입 표 | Gate G6(TestHost에서 VM·AOT 차등) | 없음. 시험은 corpus의 생성 C++을 golden으로 커밋해 시험 실행 파일에 컴파일하고, 다시 생성한 바이트가 golden과 같은지 대조한다. target 빌드 편입은 EI8(SE-B2) |

LC2는 범위가 커서 하위 단계로 나눈다(각 단계가 corpus 영역 하나 이상과 리뷰 1회로 끝난다).

| 하위 단계 | 내용 |
|---|---|
| LC2a 컴파일 입력 | binding schema 파일 형식과 reader, 시험용 가짜 엔진 schema, 패키지 manifest reader, 모듈 경로 규칙, 패키지 의존(없음·순환), import 해석, 컴파일 단위(`Compilation`), corpus 패키지 사례. 진단 S0301–S0305, S1101–S1112, S1120 |
| LC2b 선언·이름 | prelude와 부록 A·B 선언 표, 선언 수집, 명명 규칙, 예약 이름, 이름 하나에 대상 하나, import한 이름의 충돌, 별칭, 문맥 이름, 상속 멤버·`override` 대상, 기반 목록·interface 구현, 타입 표기 해석(소스·schema), manifest `services` 해석, 이름 해석 덤프(`.names`). 규칙은 §3.9. 진단 S0306–S0325, schema S1120(이름) |
| LC2c 타입·숫자 | 식의 타입, 숫자(타입 없는 상수·변환·나눗셈·비교·상수 overflow), 문자열 보간, 컨테이너·리터럴과 기대 타입, 멤버 접근(부록 B 시그니처·schema), 호출·인자 형태·생성, 검증 리터럴(형식만), `as?`, `await`·`try`의 타입, switch, `for`, 조건, 반환 경로, 식 타입 덤프(`.types`). 규칙은 §3.10. 진단 S0326, S0401–S0427(S0413 제외), S0501–S0509, S0109 |
| LC2d 흐름·대입·마무리 | await 이후 `T?`, 대입 경로·`let`, 미사용 이름·값·결과·import, 상수식과 const 순환·타입, `?` 없는 객체 필드, 프로필 검사, §2 예시 통과. 규칙은 §3.11. 진단 S0412(await 뒤), S0428, S0510–S0512, S0701–S0706, S0710–S0714, S0731, S1113 |

LC3도 하위 단계로 나눈다(컴파일 단계 `Rules` — 각 단계가 corpus `Rules` 영역 사례와 리뷰 1회로 끝난다).

| 하위 단계 | 내용 |
|---|---|
| LC3a 이벤트·hook | hook 이름·인자·자리, `on` 경로 해석(Changed·객체·배열·`Owner`·`this`·입력·World·Actor 물리 이벤트), handler 인자, 중복, `emit` 대상, 이벤트 호출, 확장 이벤트(대상·유일성·import), 재진입 표시(SD11), LevelScript. 규칙은 §3.12. 진단 S0413, S0801–S0817 |
| LC3b 실행 문맥·효과 | 효과 추론(await·생성·자원 획득·로드·emit·쓰기)과 호출 그래프 전이 폐포, hook 문맥별 금지(§6.2), async 규칙(`await`를 쓸 수 있는 곳·`while`), `All`·`Race` 인자(SD41), full 문맥(`@pure`·`@worker`·`@deterministic`·`@editorButton`·`on Validate`), attribute 이름·인자·자리. 진단 S06xx |
| LC3c 유지 자원·로드 | 유지 자원의 담는 자리·해제·`_ =` 버림 금지, 로드 요청 handle(§7). 진단 S09xx |
| LC3d 권한·저장 | 권한 구역과 `@net` 쓰기(§14, SD45), `@rpc`, 저장 가능 타입과 호환성(§15). 진단 S10xx |

순서는 LC0 → LC7이 한 줄이다. LC8의 CLI는 LC3 뒤부터 단계마다 넓히고(check → build → test), 언어 서버는 LC3 뒤에 시작할 수 있다. LC9는 LC6 뒤다.

### 18.3 엔진 연동 단계

연동 단계와 단계마다 엔진·도구에 있어야 하는 항목, 그 현재 상태는 [Sparks 엔진 연동 요구 목록](SparksEngineIntegration.ko.md)이 정본이다. 요지는 다음과 같다.

| 단계 | 내용 | 먼저 끝나야 하는 언어 코어 |
|---|---|---|
| EI1 binding 생성 | 실제 엔진 선언에서 binding schema 생성(함수 리플렉션 CU2·이벤트 필드 CU3·스크립트 지정자) | LC2 |
| EI2 ScriptRuntime | ObjectRuntime 위의 host 구현, native call, Sparks 클래스의 타입 등록 | LC5 |
| EI3 World bridge | `ScriptableComponent`, 수명 hook, 스크립트 단계, 이벤트 연결, 파괴·활성 지연 | LC6 |
| EI4 에셋·생성 | `Load`·`LoadAll`, `World.Spawn(prefab, at)` | LC6 |
| EI5 게임 시스템 | 입력·Tag·`Loc`·CVar·물리·UI·오디오(시스템마다 따로) | LC6 |
| EI6 편집기 핫 리로드 | 저장 → compile action → 실행 중 World 적용, R3 instance 재생성 | LC7 |
| EI7 네트워크·저장·결정성 | `@net`·RPC·R4, `@save`, `@deterministic`, 디버거(DAP) | LC6·LC7 |
| EI8 AOT 빌드 편입 | 생성 C++을 target 빌드에 포함 | LC9 |
| EI9 출시 | 쿡·서명·eval 정책 | LC4 |

### 18.4 전체 계획 단계와의 대응

전체 계획 §23.1의 도입 단계 L0–L4는 완료 기준으로 유지하고, 위 두 갈래가 그것을 채운다.

| 도입 단계 | 채우는 마일스톤 | 완료 기준(전체 계획 §23.1) |
|---|---|---|
| L0 · M3 | 문서 규격(초안 1.3에서 확정), LC0의 conformance 틀 | Core/Object 계약에 맞는 언어 규칙 확정 |
| L1 · M3 | LC1–LC5, LC8의 CLI와 언어 서버 기본, EI1·EI2 | 조건문·함수·객체 필드·배열·오류 처리와 바인딩 테스트 |
| L2 · M3/M4 | LC6, LC7(R1·R2), LC8의 자동완성·빠른 수정과 VS Code 확장, LC9의 동기 함수, EI3·EI4·EI5·EI6(R3) | 상호작용·문·카메라·컷씬 trigger를 C++ 수정 없이 제작, G5의 R1–R3 행, G2의 reload 항목 |
| L3 · M5 | EI7, LC9 완성과 EI8, eval(`DevOnly`), soak G2·G3 | 취소·GC·replication·중단점·migration·누수 Gate 통합 검증, G5 전체, G6 전체 |
| L4 · M6 이후 | profiler UI, 최적화 IR, AOT profile 기반 최적화와 cold module VM 유지, EI9, target별 eval 정책 | hot function 비용과 마켓 규정 근거로 결정 |

## 19. 결정 사항

2026.11.12 검토에서 초안의 권고안 10건(SD1–SD10)을 채택하였고, 같은 날 4차 검증에서 도출된 11건(SD11–SD21)을 채택하였다. 2026.11.18 5차 검증(초안 1.3)에서 사용자 요구(핫 리로드 필수·오류 방어·선택형 AOT·`WeakObjectRef` 명칭)와 권장안으로 24건(SD22–SD45)을 확정하였다. 2026.11.20 초안 1.5에서 구현 순서 3건(SD51–SD53)을, 같은 날 언어 코어 구현 중 리터럴·숫자 타입·덤프 5건(SD54–SD58)을 사용자 결정으로 더하였다. 언어 코어 구현의 LC2a에서 컴파일 입력 3건(SD59–SD61)을, LC2b(2026.11.21)에서 이름 해석 4건(SD62–SD65)을, LC2c에서 타입 검사 7건(SD66–SD72 — SD69–SD72는 교차 검증 뒤의 보완)을, LC2d에서 흐름·대입 4건(SD73–SD76)을 권장안으로 확정하였다. 2026.11.20 레벨 스크립트 설계 확정([LevelScript](LevelScript.ko.md) LD1–LD20, 모두 권장안)에서 언어에 걸리는 결정을 5건(SD46–SD50)으로 요약해 더하였다. SD4는 기본값만 확정하고 target별 값은 마켓 규정 검토 후 정한다. SD3·SD9·SD20은 엔진 수준 결정이며 전체 계획 개정 1.7·1.8의 §5.2·§20.2에 반영되어 있다.

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
- **SD17. 정수 나눗셈:** 정수 타입끼리의 `/`는 오류다(SD56 이후 모든 정수 타입). 몫은 `Math.Div`, 비율은 `float(a) / float(b)`로 쓴다(§3.3). 2차 검증의 "`/`는 항상 `float`"를 대체한다.
- **SD18. 단위:** 스크립트의 값은 엔진 규약을 따르며 엔진 규약은 UE 5.8과 같다: 길이 cm, 질량 kg, 시간 초, `Rotator`(`Pitch`·`Yaw`·`Roll`) 도, 삼각함수·quaternion 축-각·각속도 라디안(각속도·토크 API는 `…InRadians`·`…InDegrees` 쌍). `Math.Radians(...)`·`Math.Degrees(...)`는 상수식에서 허용한다(§4.2·§4.5). 각도를 받는 native 함수·필드는 이름이나 타입으로 단위를 드러내고 언어 서버가 hover에 표시한다. **2026-11-17 번복:** 처음 결정은 미터·라디안이었고 "한 언어 안에 각도 단위 2개"를 피하려 했으나, 사용자 요구로 엔진 규약이 UE와 같은 cm·도(`Rotator`)·라디안(삼각함수)이 되어 이를 따른다. 두 각도 단위는 UE와 같이 API 이름·타입으로 구분한다.
- **SD19. 빈 Soft·Row 제거:** `Soft<T>`·`Row<T>`에는 빈 값이 없고 선택 값은 `Soft<T>?`·`Row<T>?`로 쓴다. "없음"은 `null` 하나다(§3.2).
- **SD20. 종료 경로별 hook:** `RequestDestroy`·부모 파괴·셀 unload·travel·Tryout Stop·process 종료는 Disable(활성이면)·Destroy를 실행한 뒤 Detach한다. fault 격리와 GC 도달 불가 경로는 Disable을 실행하지 않는다(§5.1).
- **SD21. NextTick의 시계:** `NextTick()`은 게임 시계를 따라 일시 정지 중 재개하지 않으며 `Time.Delta`는 GameClock delta다. UI 시계의 다음 프레임 대기는 `Delay(0.0, Clock.UI)`다(§6.1).
- **SD22. 엔진 이름 정렬:** 게임 객체는 `Actor`(Entity 대체, ER 2절), 조종 대상은 `Pawn`, `Character`는 Pawn 파생 자리표시, 장면 component는 `SceneComponent`다. `WorldService`는 Object 기반 스크립트 공유 상태 타입이며 엔진 쪽 호스트는 `WorldSubsystem`이다. ID 개념(`SoftEntityId` 등)은 Prefab과 함께 결정할 때까지 유지한다.
- **SD23. 약한 객체 참조 이름:** 처음 이름 `WeakRef<T>`를 `WeakObjectRef<T>`로 바꾼다(사용자 결정, 코드 개명은 커밋 `e0e78953`에서 완료). `ObjectRef`·`WeakObjectRef`·`SoftObjectRef`로 가족을 맞추고 Core `WeakPtr`와 구분한다. 
- **SD24. Start 시점:** C++ `OnStart`의 엔진 규칙(ER 7.2: 다음 프레임 시작 처리, `IsReadyToStart`)을 따르고, Sparks `on Start`는 그 시점에 시작하는 실행이다. 준비 대기 중에는 bridge가 Tick을 보류한다(§5.1).
- **SD25. 파괴·활성 전환의 두 시점:** 스크립트 단계 끝에 World API를 호출하는 적용 시점과, 실제 hook·Detach가 실행되는 프레임 끝 정리를 구분한다. 적용 이후 스크립트는 대상을 null로 읽는다(§4.3).
- **SD26. 프레임 순서:** Unity hook의 종류·의미를 따르되 프레임 안 순서는 엔진 World tick phase(UE tick group)를 따른다. FixedTick은 Tick 뒤이고 물리 이벤트는 프레임당 1 batch다(§5.4).
- **SD27. 상속:** core에서 Sparks 클래스는 상속할 수 없고 `override fn`은 엔진 가상 함수에만 쓴다. full의 `open class`·`open fn`만 Sparks 파생과 재정의를 허용한다(§3.2).
- **SD28. 핫 리로드 등급과 단계:** R1 본문 교체·R2 제자리 migration(언어 코어 LC7), R3 instance 재생성(연동 EI6), R4 복제 schema 교체(연동 EI7). Tryout 재시작은 엔진 C++ 변경과 패키지 의존 graph 변경에만 남긴다(§12.4).
- **SD29. reload 원자성:** 계획·검증이 모두 끝난 변경만 패키지와 의존 패키지 단위로 한 번에 적용하고, 하나라도 실패하면 이전 revision을 유지한다. instance 단위 실패는 그 instance만 격리한다(§12.4).
- **SD30. 대기 중인 실행의 reload:** 자동 이전은 하지 않고 `ReloadExecutions: Keep | Cancel`(Tryout 기본 `Cancel`)과 모듈별 살아 있는 revision 상한 8을 둔다(§12.4).
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
- **SD46. 기반 타입 LevelScript:** Blueprint 제외를 유지하고, 레벨마다 하나인 연결 스크립트(UE Level Blueprint 대응)를 Actor 파생 기반 타입 `LevelScript`(레벨마다 숨은 Actor 0 또는 1개, 배치 불가)로 둔다. core에 포함하며 핵심 엔진 타입(§3.7)이다. 한 클래스를 여러 레벨이 쓸 수 있고, 프로젝트 공통 기반은 C++ 파생과 설정 `DefaultLevelScriptClass`로 고른다. 노드 시각 편집기는 보류 선택지로만 둔다(LevelScript LD1–LD5·LD13·LD19·LD20).
- **SD47. LevelScript의 참조:** `@edit` 객체 필드는 `T?`만 허용하고(S0413), 대상은 같은 Level의 배치 Actor와 그 component다. 다른 Level은 저장 거부, 셀 사이는 영속 Entity 참조 전까지 거부(§4.3), prefab 배치가 펼친 Actor는 배치 정체와 노드 경로로 저장한다(Prefab D22). 레벨 사이 연락은 World 이벤트로 하며 이름 문자열 호출은 없다(LevelScript LD6·LD7·LD10·LD17).
- **SD48. LevelScript의 수명:** 레벨 편입에서 그 Level의 모든 배치 Actor 뒤에 Awake·Enable하고, Start는 같은 프레임에 준비된 활성 배치 Actor 뒤에 시작하되 기다리지 않는다. Level을 내릴 때는 먼저 Disable·Destroy한다. `RequestDestroy`로 파괴할 수 없고 Level 파괴만 파괴한다(§5.1, LevelScript LD8·LD11·LD16).
- **SD49. Actor 물리 이벤트와 LevelScript의 물리 금지:** 엔진 `Actor`가 `TriggerEntered`·`TriggerExited`·`CollisionEntered`·`CollisionExited` 이벤트를 노출해 다른 객체가 `on X.TriggerEntered(...)`로 받는다. 같은 물리 batch에서 hook 다음에 발행하고 지연 slot 표로 구독이 있을 때만 비용이 든다. 이름은 hook과 겹치지 않게 과거형이다. LevelScript는 물리 hook(S0801)과 Collider component(S0802)를 가질 수 없다(§5.2·§5.5, LevelScript LD9·LD18).
- **SD50. 편집기 진입:** "레벨 스크립트 열기"·"참조 추가"·"이벤트 추가" 명령과 언어 서버 명령 채널(§13), 생성 경로 `<패키지>/Levels/<레벨 경로 이름>.spark`를 둔다. 배치한 Actor(직접 배치·prefab 배치)에 편집기에서 ScriptableComponent를 붙이는 것은 core에서도 되며 실행 중 추가(full)와 다르다(§4.5, LevelScript LD14·LD15).
- **SD51. 언어 코어 우선:** 엔진 요소와 무관한 언어 코어(LC0–LC9)를 먼저 구현하고 엔진 연동(EI1–EI9)은 엔진이 준비되는 대로 하나씩 더한다(사용자 결정 2026-11-20). 엔진·도구 쪽 요구는 [연동 요구 목록](SparksEngineIntegration.ko.md)에 따로 모으고, 구현 중 새로 드러난 요구도 코어 안에서 우회하지 않고 그 목록에 더한다(§18).
- **SD52. host 경계:** 컴파일러는 엔진 API를 binding schema 파일로만 알고, VM은 엔진을 host interface로만 부른다. Sparks 필드의 저장소는 VM이 가진다. 언어 코어는 Core 층에만 의존하고 가짜 host(`SparksTestHost`)로 시험한다(§18.1).
- **SD53. 모듈 배치:** `SparksVm`·`SparksCompiler`는 `Engine/Source/Runtime`, `SparksTestHost`는 `Engine/Source/Developer`, CLI와 언어 서버는 `Programs`에 둔다. 엔진 쪽 host 구현은 `ScriptRuntime` 모듈이다(§18.1).
- **SD54. 숫자 리터럴은 C# 형태:** 자릿수 구분자 `_`, 16진 `0x`·2진 `0b`, `.5` 형태의 실수, 앞자리 0(10진)을 최신 C#처럼 받는다(사용자 결정 2026-11-20). 리터럴은 Go처럼 쓰는 자리의 타입을 따르므로(SD56) 접미사는 받지 않고 S0114로 안내한다(§3.6).
- **SD55. verbatim 문자열:** C#의 `@"..."`를 받는다. escape·보간이 없고 `""`가 `"`이며 여러 줄일 수 있다. 값의 줄 끝은 LF로 맞춘다(사용자 결정 2026-11-20, §3.6).
- **SD56. 숫자 타입은 Go 수준:** `int8`–`int64`, `uint8`–`uint64`, `float32`, `float64`를 두고 기본형 `int`(=`int64`)·`float`(=`float64`)를 유지한다. 변환은 Go처럼 타입 이름으로만 하고 서로 다른 숫자 타입은 섞지 않으며, 리터럴은 Go의 untyped 상수처럼 쓰는 자리의 타입을 따른다(사용자 결정 2026-11-20, §3.3).
- **SD57. 정수 overflow는 fault:** 모든 정수 타입의 범위 초과 산술과 정확하지 않은 변환은 `IntegerOverflow` fault다. Go의 wrap과 다르게 정한 이유는 방어적 설계(조용히 틀린 값을 내지 않음)다(사용자 결정 2026-11-20).
- **SD58. 덤프는 필수:** 구문 트리(LC1)·typed IR·bytecode(LC4)를 결정적인 텍스트로 덤프하는 기능을 둔다. 검증 목적이며 golden 시험과 CLI `sparks dump`가 쓴다(사용자 결정 2026-11-20, §11).
- **SD59. manifest 의존은 이름 목록:** `dependencies`는 패키지 이름 배열이다. 패키지에 아직 버전이 없으므로 초안의 "interface 호환 범위"는 패키지 버전이 생기는 단계(배포·EI9)에서 더한다(LC2a, 2026-11-20).
- **SD60. `Rocks`는 import하지 않는다:** 엔진 binding schema의 `Rocks` 패키지 타입은 모두 import 없이 보이고(§2 예시가 그렇게 쓴다) `import Rocks…`는 S0305다. 충돌은 이름 하나에 대상 하나 규칙으로 판정한다(LC2a).
- **SD61. binding schema 파일 형식:** 모듈마다 `<모듈>.sparkbind.json`, 엄격한 JSON, 타입·필드·함수·이벤트·enum case와 32자리 16진 ID, 타입은 Sparks 타입 표기 문자열(§10.2). key는 쓰는 마일스톤이 더한다(LC2a).

- **SD62. 이름의 출처:** 내장 타입·prelude 타입·prelude 값(부록 B의 대상)은 언어가 소유하는 컴파일러 표이고, 엔진 객체·값 타입(핵심 엔진 타입, `Connection`·`PlaySession`·`Hit`, 닫힌 enum `Error`)은 binding schema(`Rocks`)가 제공한다. schema는 언어 소유 이름을 정의할 수 없다(§3.9, LC2b).
- **SD63. 대소문자 비교:** 이름 찾기는 철자가 정확히 같은 대상이다. 충돌 판정은 같은 선언 공간 안과 값 이름(지역 이름·매개변수·Sparks 멤버·문맥 이름)끼리는 ASCII 대소문자를 무시하고, 그 밖은 정확히 같을 때만이다(§3.1의 규칙을 선언 공간 안으로 넓힌 것, §3.9).
- **SD64. 이름 해석의 연쇄 방지:** 어휘·구문 진단이 있는 모듈, 해석하지 못한 import가 있는 모듈, 어휘·구문 진단이 있는 모듈을 직접 import한 모듈 안에서는 "찾지 못함"(S0317·S0318)을 내지 않고, 기반을 해석하지 못한 class 안에서는 기반에 기대는 검사도 하지 않는다. 같은 철자의 대상이 둘이면 진단을 받지 않은 쪽을 가리킨다(원인 하나에 진단 하나, §3.9).
- **SD65. 재정의 가능 함수의 표시:** binding schema 함수에 `overridable`을 두어 엔진 가상 함수 중 Sparks가 재정의할 수 있는 것을 표시한다. `override fn`의 대상은 이것과 Sparks `open fn`·`override fn`뿐이다(§3.9, 연동 요구 SE-R4).

- **SD66. 타입 없는 숫자 상수의 범위:** 타입 없는 정수 상수는 −2^63..2^64−1의 정확한 정수(int64·uint64 자리를 모두 덮는 범위)로 계산하고 벗어나면 S0503, 실수 상수는 double로 계산하고 NaN·Inf는 S0505다. 자리에 들어갈 때 그 타입의 범위를 다시 본다(S0109, §3.10).
- **SD67. 부록 B는 시그니처 목록이다:** 표준 API의 정확한 시그니처를 부록 B 시그니처로 확정하고 컴파일러는 그 목록을 언어 소유 멤버 표로 가진다. `InputAction.Value`는 UE `FInputActionValue`처럼 3축 `vec3`이고(1축·bool은 X), `Input`의 타입은 엔진 schema `InputSystem`이다(§3.10).
- **SD68. 타입 검사의 경계:** 숫자 사이 암묵 변환 없음, 제네릭 무공변, 타입 인자는 추론하지 않는다(명시), 생성은 이름 지정 인자, 함수 호출은 위치 인자, 반환 경로는 "끝나는 문장"(Go의 terminating statement) 규칙이다(§3.10).
- **SD69. 진단 하나 원칙과 모르는 기대 타입:** 타입 진단은 자리(피연산자·인자·원소·이름 지정 인자)마다 하나이고 바깥 식은 안쪽 진단 뒤 해석하지 못한 것이다. 앞선 오류로 기대 타입을 모르는 자리는 기대 타입이 없는 자리와 구별해 S0417을 내지 않고, 표기를 해석하지 못한 이름은 초기값의 타입을 받지 않는다(§3.10 연쇄 방지).
- **SD70. 정해진 상수 접기:** `const`와 상수만 쓰는 정해진 타입의 식은 컴파일 때 값을 정하고(범위 밖 S0503, NaN·Inf S0505) switch case·0 나누기 판정에 쓴다. 상수식 요구와 const 순환 진단은 LC2d다(§3.10 숫자 상수).
- **SD71. 값 struct 순환과 초기값 깊이:** struct가 필드로 자신을 값으로 담으면(추론한 필드 포함) 순환 하나에 S0426, 이름이 아닌 길로 64단계를 넘는 초기값 연쇄는 S0427이다. 이름 의존은 의존 순서로 미리 정해 컴파일러가 재귀 깊이를 쓰지 않는다(§3.10).
- **SD72. 확정한 세부 규칙:** 로드 요청은 대기가 아닌 값, 제네릭 함수(부록 B 포함)의 타입 인자는 언제나 명시, `?.`로 부른 대기의 결과는 `R?`, switch 대상은 정수·`bool`·`string`·enum, 복합 대입은 `x = x op v`의 규칙(원소 대상은 읽기 타입 `T?`), 생성은 읽기 전용 필드도 정한다, `Math` 자리표시 함수의 상수만 인자는 자리표시가 `int`를 받지 않으면 `float`(§3.10).
- **SD73. 대입 경로:** 대입·변경 메서드의 경로를 뿌리부터 보아 처음 어긋난 곳의 진단 하나를 낸다. 참조 타입 값을 지나면 그 뒤는 객체 속성 쓰기이며 다른 객체의 Sparks 필드는 같은 class라도 읽기만 한다(§3.11).
- **SD74. 미사용 판정:** 읽지 않는 지역·binding·반복 변수·인자와 그 뒤 읽지 않는 대입 값은 오류이고 선언 초기값의 덮어쓰기는 오류가 아니다. `_` 인자는 시그니처가 정해진 자리에서만 쓴다(§3.11).
- **SD75. await 뒤 판정:** 문장 단위의 경로 분석(분기 합류·back-edge, 고정점)으로 "기다린 뒤" 상태를 정하고 객체 타입 지역 이름의 그 읽기만 `T?`로 본다(§3.11).
- **SD76. 상수식과 const 타입:** 상수식 목록과 const 타입 목록(§3.11)을 확정하고, 순환·비상수 const의 값은 정하지 않아 쓰는 자리는 조용하다.

## 20. 교차 검증 기록

초안 0.1·0.4·0.5·1.0·1.2는 이 문서의 작업 초안이다. 초안 1.5(구현 순서 — §18, SD51–SD53)는 사용자 결정에 따른 개정이며, 연동 요구 목록의 현재 상태는 작성 때 코드(커밋 `03c13ddb`)로 확인하였다. 초안 1.4의 LevelScript 반영은 [LevelScript](LevelScript.ko.md) 초안 0.1을 리뷰어 1명이 문서 원문·UE 5.8 소스와 대조한 결과(정정 9건 — Prefab 판, 진단 등록처, 배치 불가 표지, GameDirector 이름, 레벨 로드 순서, UE 인용, UE BeginPlay 순서, `Find<T>`, Sparks 이름)를 따른다. 기록은 그 문서 10절에 있다.

### 20.1 5차 검증(초안 1.2, 스펙 완비)

2026-11-18 사용자 지시로 구현 착수 전 문서만으로 규격을 완비하였다. 리뷰어 3명이 독립적으로 (1) 문서 내부 정합성·규격 완결성과 사용자 핵심 요구(핫 리로드·오류 방어·선택형 AOT), (2) 엔진 설계 문서·현재 코드(World·Actor·메타 컴파일러·Core)와의 정합성, (3) 새 주변 시스템 스펙(로깅·CVar·입력·UI·커브·물리·공간 인덱스·동기화·콘텐츠 서명)과의 정합성과 빠진 영역을 검토하였다. 사용자 결정은 `WeakObjectRef` 명칭(SD23), AOT는 IL2CPP식 선택 옵션(SD31), 이번 개정은 문서만(코드 후속은 당시 §17.2, 지금은 연동 요구 목록)이며, 나머지는 권장안으로 확정하였다.

| 관점 | 주요 지적 | 조치 | 위치 |
|---|---|---|---|
| 엔진 이름 | Entity·TransformComponent·`OnAwake(ComponentContext&)`, 없는 지정자(`Edit`·`Replicated`·`ScriptExtensible`), C5 구현 여부 | SD22·SD23, §10.1 예시 재작성과 선행 표, C5 완료 표기 | 0·2·10·17 |
| 수명·시점 | Start 시점, 파괴·활성 전환 즉시 전이, World tick phase, Component Awake 실패 범위, BeginDestroy hook 부재, 엔진이 연 진입 | SD24·SD25·SD26·SD43, 엔진이 연 진입도 스크립트 단계, cleanup hook 선행 조건 | 4.3·5.1·5.4·17.2 |
| 내부 모순 | Sparks 클래스 상속, 이월 항목의 파괴된 인자, 필드 초기값 유지 자원, `Race`에서 진 요청, 클라이언트 전용 판정 근거, FunctionId 대상, 증분 컴파일의 효과, 같은 묶음 파괴의 격리, 단위·상수식 잔재 | SD27·SD33 재검증·SD41·SD45, 규칙 정정 | 3·4·5·6·7·12 |
| 미확정 값 | 전달 수 N·큐 상한, 예산, interface 문법, CU9, conformance, zero value, `await` 정지, 일시 정지, 문맥, `Error` 목록, `Assert` | SD33·SD34, EBNF·CU9 규격, 부록 B, §6.1 정지 지점, §13.1 | 3·5·6·8·9·10·13 |
| 핫 리로드 | L3로 늦음, Tryout 재시작 범위, 재생성 경로, 보존 규칙, 오래 기다리는 실행, 롤백, 편집기 표시, 엔진 쪽 계약(TypeRegistry revision·동적 schema·subobject) | SD28–SD30, §12.4 재작성, G5, §17.2 | 12.4·9.3·18 |
| 오류 방어 | 클라이언트 `@net` 쓰기, 폭주, 편집기 문맥 fault, 진단 누락, 격리 복구, spawn 중 Enable, native Fatal, 로드 실패·컴파일 오류 상태 Tryout, softlock | SD32·SD43·SD45, §8.4 신설 | 8·14 |
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
| 수명·종료 | spawn 묶음·Enable 시점, "비활성화"의 3가지 의미, Detach 시점, Teardown 접근 범위, Tryout Stop의 hook 미실행 | Spawn 안 Awake·Enable과 큐의 Start, "격리" 단일 상태, Destroy 반환 직후 Detach, SD12·SD20 | 5.1·6.2·8.3·9.1 |
| 이름 해석 | 가림 금지가 지역 변수에만 적용, 암묵 멤버 미예약, import 별칭 의미, 확장 이벤트 충돌, SD2 변환 충돌, Character의 `Owner` | 이름 하나에 대상 하나, 부록 A, 모듈 별칭, 확장 이벤트 유일성과 안정 ID, 메타 컴파일 오류, 기반 타입별 문맥 이름 | 3.1·3.4·3.7·5.2·10.3 |
| 타입·값 | await 이후 `T?` 적용 대상, 중첩 optional, `?.` 단락 범위, `== null` 좁히기, `==`·보간 허용 타입, `let` 깊이, 버린 결과, `try` 실패 범위 | 흐름 기준 규칙과 제외 목록, `?` 중첩 금지, 한 단계 단락, 닫힌 허용 목록, 깊은 `let`, Execution 트리 종료, SD13·SD14·SD19 | 3.2·3.3·4.2·4.3·8.2 |
| 구문·어휘 | `try`·`await` 적용 범위, `<` 판정, FloatLit 미정의로 `0..count` 오토큰화, 문자열 escape, 예약어 목록 부재, 미사용 인자를 피할 수 없음 | `Value` 문법, 계층형 EBNF, 어휘 규칙, `_` 인자, SD16 | 3.3·3.6·3.7 |
| 사용자 오독 | 지역 유지 자원이 즉시 멈춤, `int / int`, 예시의 cm·도 수치와 엔진 미터·라디안의 불일치(2026-11-17 엔진 규약이 cm·도가 되어 해소 — SD18), 일시 상태에 묶은 소리, 모든 collider에서 오는 Trigger | await 없는 지역 자원 오류, SD17·SD18, 예시 수정, `trigger` 인자 판정 | 2·4.2·7.1 |
| 문서 간 정합 | 오버로드 처리(노출 생략과 오류), session의 VM globals·root, 순환 import, CU9 정의 부재, LSP 도입 단계, Gate 번호 | 전체 계획·입력 상세안 정정, CU9 정의 | 10.3·13·17 |

에이전트 간 권고가 일치하지 않은 6건은 다음과 같이 판정하였다.

- **물리 이벤트의 재진입 발행자:** 자기 Actor가 아니라 상대 `other`로 하였다. 자기 Actor로 보면 대기 중 다른 Actor의 Enter가 버려지고 Exit만 전달되어 짝이 깨진다.
- **`?.`의 단락:** 사슬 전체가 아니라 한 단계만 단락한다. 기존 S0412 규칙으로 판정되며 새 규칙이 필요 없다.
- **Disable 시 연결:** 항목을 삭제하지 않고 비활성으로 둔다. 활성 전환을 반복해도 handler 순서가 유지된다.
- **`try` 실패와 Tryout 정지:** Tryout을 정지하지 않는다. 복구 가능한 실패는 `Result`라는 원칙을 따른다. 단, Start 안의 `try` 실패는 Start fault다(§8.2).
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
| 11 | 전용 서버에서 외형 에셋 로드 실패로 서버·클라이언트 결과 차이 | 로딩 검토 | 준비 mask 교집합과 효과 없는 stub, Tryout 서버 session 시험 | 7.5 |
| 12 | 필드 초기값 하드 참조가 세션 내내 상주 | 로딩 검토 | instantiate하는 prefab·셀의 의존성으로 처리, 하드 의존 예산 쿡 오류 | 7.2 |
| 13 | 상주 유지 경로 2개(필드 요청·값), `Get`의 cache 의존 | 로딩 검토 | 요청은 지역 전용, 상주는 값 보관 하나, `Get`·`IsLoaded` 제거 | 7.3 |
| 14 | Teleport 관심 공백, travel 호출 실행 취소, 로딩 화면 미해제 | 로딩 검토 | `Owner.Teleport` 내부 prefetch, `Session.Travel` 접수형·엔진 관리 로딩 화면 | 7.6 |
| 15 | `emit` 전달 시점이 절마다 다름, C++ handler 즉시 호출로 재진입 | 정합성 | 실행 조각 종료 후 FIFO 하나로 통일, C++ handler도 전달 시점 | 5.4 |
| 16 | 필드 쓰기 권한 규칙과 native 속성 예시 충돌, setter·속성 중복 | 정합성 | `ScriptWritable` native 속성 예외, 속성이 있으면 setter 비노출 | 3.2·10.3 |
| 17 | 예시의 규칙 위반(대소문자만 다른 이름, 미사용 인자, 버린 결과, 서버 handler의 UI 호출, 암묵 전역) | 정합성 | 예시 재작성, handler 인자 앞부분 선언, `_` binder, `NextTick`·`Race` 무반환, 암묵 멤버 닫힌 목록 | 2·3·6 |
| 18 | EBNF 누락(generic 메서드, BinaryOp·Literal, 복합 대입의 await, `?? Exit`, emit 형태) | 정합성 | 문법 보완 | 3.6 |
| 19 | 초기 적용 시 Changed 발행 여부 미정, late join 일회성 재생 | 정합성 | 초기·로드·late join은 Changed 없음, Start에서 반영 | 5.3 |
| 20 | AOT 방식·clock 이름·Start 개명 범위·소유 component 이중 소유가 스펙 개정 목록에 없음 | 정합성 | §17에 추가, 소유 component는 handle·SubobjectGuid | 4.1·4.5·17 |
| 21 | 사용자 결정: hook은 Unity 체계, 이름은 Rocks 어휘(Tick·FixedTick·LateTick) | 사용자 | hook 표, 일반 `Start()` 메서드와의 충돌은 C++ `On*`로 해소, 종료 hook 후보 `Stop`과 Tryout `Stop`의 충돌은 `Destroy` 채택으로 해소 | 5.1 |
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
