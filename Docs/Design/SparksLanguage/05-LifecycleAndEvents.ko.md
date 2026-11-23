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
