## 18. 도입 단계

구현은 두 갈래로 나눈다(SD51, 2026-11-20 사용자 결정). 엔진 요소와 무관한 **언어 코어**(LC0–LC9)를 먼저 만들고, 엔진 쪽이 준비되는 대로 **엔진 연동**(EI1–EI9)을 하나씩 더한다. 언어 코어 가운데 LC9(AOT)는 급하지 않아 뒤로 미룬다(SD77, 2026-11-25 사용자 결정 — LC8 다음은 연동 단계). 언어 코어는 엔진의 다른 작업(리플렉션 확장·World·에셋)을 기다리지 않고, 그 작업들도 Sparks를 기다리지 않는다.

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

순서는 LC0 → LC7이 한 줄이다. LC8의 CLI는 LC3 뒤부터 단계마다 넓히고(check → build → test), 언어 서버는 LC3 뒤에 시작할 수 있다. LC9는 LC6 뒤에 할 수 있지만 미룬다(SD77): LC8이 끝나면 선행 항목이 갖춰진 연동 단계(18.3절)와 다른 작업을 먼저 하고, LC9는 AOT가 실제로 필요해질 때(L3 · M5, EI8과 함께) 착수한다. 미루는 동안에도 VM 쪽 변경은 동등성 규칙(§12.5)을 지킨다 — AOT가 따라올 수 없는 VM 동작을 새로 만들지 않는다.

### 18.3 엔진 연동 단계

연동 단계와 단계마다 엔진·도구에 있어야 하는 항목, 그 현재 상태는 [Sparks 엔진 연동 요구 목록](../SparksEngineIntegration.ko.md)이 정본이다. 요지는 다음과 같다.

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
| L2 · M3/M4 | LC6, LC7(R1·R2), LC8의 자동완성·빠른 수정과 VS Code 확장, EI3·EI4·EI5·EI6(R3) | 상호작용·문·카메라·컷씬 trigger를 C++ 수정 없이 제작, G5의 R1–R3 행, G2의 reload 항목 |
| L3 · M5 | EI7, LC9(동기 함수부터 — SD77로 L2에서 옮김)와 EI8, eval(`DevOnly`), soak G2·G3 | 취소·GC·replication·중단점·migration·누수 Gate 통합 검증, G5 전체, G6 전체 |
| L4 · M6 이후 | profiler UI, 최적화 IR, AOT profile 기반 최적화와 cold module VM 유지, EI9, target별 eval 정책 | hot function 비용과 마켓 규정 근거로 결정 |
