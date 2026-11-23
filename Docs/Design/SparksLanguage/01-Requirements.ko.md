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
