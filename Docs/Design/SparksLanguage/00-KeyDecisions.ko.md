## 0. 핵심 결정

| 항목 | 결정 |
|---|---|
| 이름 | Sparks. 소스 `.spark`, 컴파일 모듈 `.sparkc`(SD1). 파일 경로가 import 경로다(`Weapons/Sword.spark` → `import Weapons.Sword;`). 이 경로가 엔진 공통 타입 도구 이름(asset 명령·클래스 참조 텍스트)이다([리플렉션 B9](../ReflectionExtensions.ko.md) 5.2절). |
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
