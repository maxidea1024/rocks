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
