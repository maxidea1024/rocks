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
