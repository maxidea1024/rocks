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
