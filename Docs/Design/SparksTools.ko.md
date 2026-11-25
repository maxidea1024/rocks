# Sparks 도구(LC8)

이 문서는 Sparks 언어 코어의 LC8(도구)을 정한다: CLI `sparks`(`check`·`dump`·`build`·`ids`·`fmt`·`test`), 공식 formatter, `@test` 실행기, 언어 서버 `sparks-lsp`(진단·의미 강조 → 자동완성·hover·정의 이동·빠른 수정·서식). 언어 의미·진단 형식은 SparksLanguage(SL) §8.1·§13이 정본이고, 이 문서는 그것을 어떤 모듈·프로그램·형식으로 실현하는지 정한다. 선행은 표준 입출력 API(SparksEngineIntegration SE-B7)뿐이며 LC8a가 함께 만든다.

- 개정 11(2026-11-25): LC8e 구현·리뷰 반영(10.9절 — `initialized` 때 첫 분석, 드라이브 `:`는 `%3A`, 지움을 먼저 발행, 작업 공간 없으면 분석하지 않음, 가장 안쪽 패키지, schema 상대 경로 절대화, 잘못된 UTF-8 본문은 -32700, 머리 전체 상한을 줄마다, class 기반 경로 강조·prelude enum case `defaultLibrary`, 경로 표기의 UTF-8·대소문자 수정).
- 개정 10(2026-11-24): LC8e 설계(10절 — 틀·수명·문서 저장소·분석·진단·의미 강조·위치 변환·시험).
- 개정 9(2026-11-24): SL 3.13 표 `@test` 행 개정(SL 문서를 장별 폴더로 나눈 뒤).
- 개정 8(2026-11-23): LC8d 구현·리뷰 반영(9.7절 — `@test` 반환 타입 금지(S0621), 엔진 enum 값 수정, 취소 판정·메서드 거부, 실행기 거부는 1, 토큰 밑줄).
- 개정 7(2026-11-23): LC8d 설계(9절 — `@test`는 최상위 함수만(SL 3.13 표·컴파일러와 맞춤, 클래스 `@test` 삭제), 실행기는 SparksTestHost 공개 API, VM 최상위 진입 `CallFunction`, 결과 형식). T1·T10 개정.
- 개정 6(2026-11-23): LC8c 구현·리뷰 반영(8.5절 — 주석 자리 규칙 보강, BOM 제거 확정, 인자 아래 모듈만 대상, 변형 corpus 시험).
- 개정 5(2026-11-23): LC8c 설계(8절 — 서식 알고리즘·주석 수집·`fmt` 명령·시험). 3절 규칙을 구체화(이항 연산자 표에서 없는 `->` 삭제, 블록 안 enum case 줄, 이어지는 줄의 들여쓰기 단위).
- 개정 4(2026-11-23): LC8b 구현·리뷰 반영(7.4절 — 컴파일러의 승계 판정 순서·은퇴/이미 이은 승계 진단, manifest의 모르는 키 S1006, 도구의 이름 겹침 검사·확인 실패 진단 출력, 결과 줄 정렬, 잠금 공유 모드 한계).
- 개정 3(2026-11-23): LC8b 설계(7절 — build 쓰기 규칙, ids의 manifest 편집·검증·잠금, C# 직렬화와 byte 동일). T7·T8 개정.
- 개정 2(2026-11-23): LC8a 구현(6절).
- 개정 1(2026-11-23): 첫 판. 하위 단계 LC8a–LC8g, 결정 T1–T14.

## 1. 하위 단계

| 단계 | 내용 | 완료 기준 |
|---|---|---|
| LC8a 표준 입출력·작업 공간·check | PlatformBase 표준 스트림 API(SE-B7), `SparksTools`(Developer) 작업 공간 적재(패키지 폴더 찾기·schema 폴더·원문 읽기)와 명령 실행 라이브러리, 프로그램 `sparks`의 `check`·`dump` | 명령 시험(종료 코드·출력 golden), 작업 공간 적재의 할당 실패 전수, corpus 사례를 CLI로 돌린 진단이 conformance와 같음 — 완료(개정 2, 6절) |
| LC8b build·ids | `build`(모듈마다 `.sparkc` — 출력 폴더), `ids`(발급 대기 안정 ID를 `.rocksids.json`에 GUID로 발급) | 출력 byte golden, manifest 왕복 시험 — 완료(개정 4, 7절) |
| LC8c formatter·fmt | lexer의 주석 보존 모드, 공식 서식 규칙(3절)의 token 기반 formatter(SparksCompiler), `fmt`·`fmt --check` | corpus 전체에서 멱등(fmt(fmt(x)) = fmt(x))·token 열 보존, 서식 golden — 완료(개정 6, 8절) |
| LC8d @test·test | `@test fn` 실행기(headless 가짜 host — 엔진 함수는 "엔진 필요" 실패), 결과를 진단 형식으로 | 실행기 시험(통과·Assert 실패·fault·엔진 필요·끝나지 않음) — 완료(개정 8, 9절) |
| LC8e 언어 서버 기본 | `sparks-lsp`: JSON-RPC(stdio, Content-Length), 문서 저장소(열린 문서 덮어쓰기·버전), 진단 발행, 의미 강조(semanticTokens/full) | CLI와 언어 서버의 진단 동등성, 프로토콜 시험(메시지 golden) — 10절 |
| LC8f 언어 서버 편집 | 자동완성(멤버·hook·attribute·리터럴·경로·Unity 이름 제시), hover, 정의 이동, 빠른 수정(SL §13 목록), 문서 서식 | 자동완성 후보 golden, prefix fuzz, 빠른 수정 golden |
| LC8g VS Code 확장 | `Tools/VSCode/sparks`(TypeScript — 언어 등록·문법 강조·`sparks-lsp` 시작) | 확장 패키징 시험(C# 도구 시험에서 manifest 검사) |

## 2. 구조

- **모듈(T1):** formatter는 `SparksCompiler`(SL 18.1 — 컴파일러 라이브러리가 같은 규칙을 쓴다). `@test` 실행기는 `SparksTestHost`(SL 18.1 — conformance와 `@test`가 같은 가짜 host에서 돈다, 공개 API `SparksTestHost/TestRunner.h`). 작업 공간 적재·명령 실행·언어 서버 핵심은 `Engine/Source/Developer/SparksTools`(Developer — 출시 실행 파일에 링크하지 않는다, 시험은 EngineTests에서 같은 프로세스로 부른다). 프로그램 `Programs/SparksCli`(실행 파일 `SparksCli` — 설치 이름 `sparks`는 배포 도구의 몫, 사용법 문장은 `sparks`)와 `Programs/SparksLsp`(`SparksLsp` — 설치 이름 `sparks-lsp`)는 Launch 진입(CrashHandlingAndLaunch)과 명령 함수 호출만 한다(Commandlet과 같은 배치).
- **표준 입출력(T2 — SE-B7):** `Platform/StandardStream.h` — `WriteStandardOutput(StringView utf8)`·`WriteStandardError(StringView utf8)`(콘솔이면 Windows는 UTF-16 `WriteConsoleW`, 리디렉션·pipe는 byte 그대로 — Commandlet 구현과 같은 규칙), `ReadStandardInput(Span<char> buffer)` → 읽은 byte 수(0은 끝, pipe·파일은 byte 그대로 — 언어 서버의 Content-Length 틀), 실패는 `DependencyFailed`(handle 없음·쓰기 실패)·`InvalidArgument`(UTF-8 아님 — 출력만). Windows는 `GetStdHandle`+`ReadFile`/`WriteFile`/`WriteConsoleW`, macOS·Linux는 `read`/`write`(EINTR 재시도). CRT stdio를 쓰지 않는다(text 모드 변환이 Content-Length를 깨지 않게). Commandlet의 전용 구현은 이 API로 옮길 수 있다(그 세션의 일 — 알림).
- **작업 공간(T3):** 명령 인자의 경로(폴더)마다 아래를 훑어 `SparksPackage.json5`가 있는 폴더를 패키지 뿌리로, 그 아래 `.spark`를 모듈로 넣는다(conformance 폴더 사례와 같은 규칙 — 모듈 경로는 패키지 뿌리 기준). schema는 `--schema <폴더>`(여러 번 — `*.sparkbind.json`), ID manifest는 패키지 뿌리의 `.rocksids.json`. 설정 파일(`SparksWorkspace.json5`)은 엔진 연동(EI — 엔진이 schema 위치를 안다)에서 더한다. 파일 읽기는 PlatformBase `FileSystem`.
- **출력(T4):** 진단은 SL §8.1 텍스트(`AppendDiagnostics`)를 표준 출력에, 요약 한 줄(`N errors in M files`)을 끝에. 명령 사용법 오류는 표준 오류. 종료 코드: 0 성공(진단 없음·시험 통과·서식 같음), 1 진단·시험 실패·`fmt --check`의 다름, 2 사용법 오류, 3 입출력·할당 실패. 출력은 결정적이다(경로 byte 순, 진단은 §8.1 순서).
- **명령 인자(T5):** 명령 이름 뒤 `--이름 값`·`--플래그`·위치 인자만(AppInfoCommandLine의 생성 바인더는 설계만이라 `SparksTools`의 작은 자체 분해기 — 그 모듈이 생기면 옮긴다). 알 수 없는 옵션은 사용법 오류(2)다.
- **dump(T6):** `sparks dump --ast|--names|--types|--ir|--bytecode <파일 또는 폴더>` — conformance golden과 같은 덤프 함수(`AppendSyntaxDump`·`AppendNameDump`·`AppendTypeDump`·`AppendIrDump`·`AppendBytecodeDump`)를 쓴다. 폴더면 모듈마다 `== <경로>` 줄을 앞에(경로 byte 순).
- **build(T7):** `sparks build <경로> --out <폴더>` — 진단 없으면 모듈마다 `<출력>/<패키지 이름>/<모듈 경로의 .spark를 .sparkc로>`(SparksBytecode `.sparkc` 형식). 진단이 있으면 아무것도 쓰지 않는다(1). 파일마다 원자적으로 쓰고 내용이 같으면 쓰지 않는다(7절).
- **ids(T8):** `sparks ids <경로>` — 컴파일의 발급 대기 목록(`PendingIds`)마다 새 GUID(Core `TryNewGuid` — OS 보안 난수 UUID v4, C# `Guid.NewGuid`와 같은 종류)를 그 패키지 ID manifest에 더하고, 옛 이름이 있는 대기는 C# `StableIdManifest.Rename`과 같은 규칙으로 옮긴다(옛 이름은 aliases, type rename은 멤버 항목도 함께). 출력 byte는 C# `StableIdManifest`의 직렬화와 같다(항목은 kind·name ordinal 순, aliases ordinal 순 — C# 도구가 다시 써도 바뀌지 않는다. 기존 항목의 원문 순서는 지키지 않는다). 쓰기 전에 새 manifest로 다시 컴파일해 진단·대기가 0인지 보고, C#과 같은 잠금 파일·임시 파일 교체로 쓴다(7절).
- **formatter(T9 — 3절):** token 기반이다. lexer의 주석 보존 모드가 주석을 trivia 목록(종류·범위)으로 따로 돌려주고, formatter는 token 열과 trivia를 원래 순서로 걸으며 공백·줄바꿈·들여쓰기만 정한다(token 글자는 바꾸지 않는다 — 의미 보존을 token 열 비교로 시험). 구문 오류(S02xx)가 있는 파일은 서식하지 않는다(그대로 — 진단). 설정이 없다(SL §3.1 R21).
- **@test(T10 — 개정 7):** `@test fn`(최상위 — 인자 없음, `async`도 된다 — SL 3.13 표)을 시험마다 새 VM session의 최상위 진입(`VmSession::CallFunction`)으로 돌린다. 클래스 안 `@test`는 없다(SL 3.13 — 클래스는 시험 함수가 만들어 쓴다, 객체 생성은 엔진 fixture가 필요하다). 시계는 결정적 프레임으로 진행(정지한 시험은 `--max-frames`(기본 600)까지 프레임을 진행하고 남으면 "did not finish"). 결과: 통과, `Assert` 실패·fault(진단 형식 — 원문 위치), 엔진 기능 사용(객체·필드·native 함수·문맥·로드 — "needs the engine" — 실패로 센다). headless World fixture(SL §13.1)는 엔진 연동(EI)이 host를 바꿔 끼운다(9절).
- **언어 서버(T11):** LSP 3.17, JSON-RPC 2.0, stdio Content-Length 틀(Core JSON reader·writer). 문서 동기화는 전체 내용(`TextDocumentSyncKind.Full`) — 증분은 측정 뒤. 분석은 작업 공간 원문에 열린 문서 내용을 덮은 snapshot으로 전체를 컴파일한다(SL §13 "문서 버전별 arena" — snapshot마다 새 allocator 영역). 진단은 같은 `Compilation`이라 CLI와 같다(동등성 시험). 위치는 LSP 규약대로 UTF-16 code unit 열(컴파일러 위치는 byte·code point — 변환 함수와 시험). 처리 중 들어온 같은 문서의 새 변경은 이전 분석 결과를 버린다(취소 — 순서는 메시지 순서로 결정적).
- **의미 강조(T12):** semanticTokens/full — 이름 해석 결과로 type·class·struct·enum·enumMember·event·function·method·property(필드)·parameter·variable·namespace(모듈) + modifier(declaration·readonly(`let`)·static(최상위)·async·defaultLibrary(엔진 schema)).
- **편집 기능(T13 — LC8f):** 자동완성 후보는 위치의 문법 자리(멤버 접근·`on`·attribute·리터럴 인자·import 경로)와 이름 해석 범위에서 만든다. 리터럴(태그·에셋 경로)은 컴파일 검증과 같은 사전(엔진 연동 전에는 schema·manifest만). Unity 이름(`Update` 등)은 Rocks 이름 후보에 별칭으로 붙인다. core 패키지는 full 후보를 숨긴다. hover는 타입·문서 주석·선언 위치(엔진 native는 schema 위치), 빠른 수정은 진단 코드별 편집 생성기(SL §13 목록 — 해당 진단이 있는 것부터).
- **VS Code(T14):** 확장은 언어 id `sparks`, `.spark`, TextMate 문법(키워드·주석·문자열 — 의미 강조가 덮는다), 서버 경로 설정(`sparks.server.path`, 기본은 Rocks 출력 폴더). 배포·마켓 등록은 하지 않는다(저장소 안 개발용).

## 3. 공식 서식(LC8c)

- 들여쓰기 4 공백, 탭 없음. 줄 끝 공백 없음(주석·문서 주석 포함 — 문자열 값 안은 그대로), 파일 끝 LF 하나. 줄 끝은 LF(CRLF는 LF로). UTF-8 BOM은 지운다(정규형은 BOM 없는 UTF-8 — 편집기가 BOM으로 저장한 파일은 `fmt --check`가 다르다고 알린다).
- 중괄호는 여는 줄 끝(`class X : Base {`, `fn F() {`, `if (c) {`), 닫는 중괄호는 자기 줄. `} else {` 한 줄. 빈 블록은 `{ }`가 아니라 `{\n}`(SL 예제와 같게).
- 문장마다 한 줄. 선언·멤버 사이 빈 줄은 원문을 따르되 연속 빈 줄은 하나로, 블록 처음·끝의 빈 줄은 없앤다. 최상위 선언 사이는 빈 줄 하나(없으면 넣는다).
- 이항 연산자·`=`·`:`(타입 표기 앞은 붙이고 뒤는 한 칸)·`??` 앞뒤 한 칸, `,` 뒤 한 칸, 괄호·대괄호 안쪽 공백 없음, 단항 연산자 붙임, `?.`·`.`·`?` 붙임, 키워드(`if`·`for`·`while`·`return` …) 뒤 한 칸.
- 주석: `//` 줄 주석은 앞 token과 한 칸(같은 줄) 또는 자기 줄(들여쓰기 맞춤), `/* */`는 위치 그대로 앞뒤 한 칸. 문서 주석 `///`는 선언 바로 위.
- 줄 길이 제한·자동 줄바꿈은 하지 않는다(원문 줄바꿈을 존중 — 식 안의 줄바꿈은 들여쓰기만 다시 맞춘다: 이어지는 줄은 문장 들여쓰기 + 4, 괄호 안에서 줄을 바꾸면 그 괄호를 연 줄마다 + 4 — 8.2절). 자동 줄바꿈은 측정·사용자 의견 뒤(결정 T9 대안).

## 4. 결정

- T1 대안(CLI·서버를 SparksCompiler에)은 Runtime 모듈에 도구 의존(파일 시스템 훑기·JSON-RPC)을 넣는다(엔진 층 핵심만 원칙). formatter만 컴파일러에 둔다(SL 18.1 — 편집기·Rocks Build·서버가 같은 규칙).
- T2 대안(CRT `fwrite`·`fread`)은 Windows text 모드가 CRLF·Ctrl-Z를 바꿔 LSP 틀을 깬다. 대안(프로그램마다 전용 구현 — Commandlet 방식)은 SE-B7이 막으려는 중복이다.
- T4 종료 코드는 컴파일러 도구 관례(0·1·2) + 입출력 실패 3.
- T7 대안(모듈을 바로 덮어쓰기)은 쓰는 도중 실패하면 반쪽 `.sparkc`가 남는다(읽는 쪽이 형식 오류로 거부하지만 이전의 맞는 파일을 잃는다). 대안(바뀌지 않은 파일도 다시 쓰기)은 수정 시각이 바뀌어 뒤 단계(쿠킹·패키징)가 할 일을 늘린다. 지운 모듈의 옛 `.sparkc`는 지우지 않는다(출력 폴더 정리는 호출자의 몫 — 다른 도구의 파일을 지우지 않게).
- T8 대안(기존 항목 순서 유지)은 C# 도구가 같은 manifest를 다시 쓸 때마다 순서가 바뀌어 diff가 생긴다 — 두 도구가 같은 정규형을 쓴다. 대안(발급 뒤 쓰고 나서 다시 컴파일해 확인)은 확인이 실패해도 이미 쓴 manifest가 남는다 — 쓰기 전에 메모리에서 확인한다.
- T9 대안(구문 트리 재출력)은 괄호·주석·빈 줄 보존에 노드마다 규칙이 필요하고 의미 보존 시험이 어렵다. token 기반은 token 열 동일성으로 의미 보존을 기계적으로 보인다. 줄 길이 자동 줄바꿈은 구문 트리가 필요해 뒤로 미룬다(Go `gofmt`처럼 줄바꿈을 사람이 정한다).
- T10 대안(엔진 없이 `@test` 금지)은 언어 코어의 시험 작성(SL §13.1)을 막는다. 대안(실행기를 `SparksTools`에 두고 schema 묶기를 따로 구현)은 FakeHost의 schema 구동 묶기(서술자 대조 — 약 500줄)를 베낀다 — 실행기를 `SparksTestHost`에 두고 FakeHost의 묶기를 그대로 쓴다(SL 18.1의 "conformance와 `@test`가 이 host에서 돈다"와도 같다). 대안(클래스 `@test` — 개정 1)은 SL 3.13 표·컴파일러(S0621)와 어긋나고 객체 없는 headless host에서 instance를 붙일 엔진 객체가 없다.
- T11 증분 동기화·증분 분석은 SL §13 목표(파일 1,000개 진단 p95 300 ms)를 재 본 뒤 넣는다(LC7c3처럼 측정 먼저).

## 5. 시험

- 명령은 `SparksTools`의 `RunSparksCommand(arguments, 입출력 객체)`를 EngineTests에서 같은 프로세스로 부르고 출력·종료 코드를 golden과 비교한다(입출력 객체가 표준 스트림 대신 버퍼에 쓴다). 프로그램 `sparks`는 이 함수와 표준 스트림을 잇기만 한다.
- 진단 동등성: conformance corpus의 폴더·파일 사례를 임시 폴더에 쓰고 `sparks check`의 진단이 `.expect`와 같은지(LC8a), 언어 서버의 진단이 CLI와 같은지(LC8e).
- 할당 실패 전수: 작업 공간 적재, 명령 실행(작은 사례), formatter, 언어 서버 메시지 하나의 처리.
- formatter: corpus 전체 멱등·token 보존, `Formatting/` 서식 golden(입력 → 기대 출력).

## 6. LC8a 구현(개정 2)

- **표준 스트림:** PlatformBase `Platform/StandardStream.h`(PlatformLayer "표준 스트림" 절). 결과 형식은 PlatformBase 규약(`FileTransfer`)이다(PlatformBase는 Core의 `Result`·`StringView`를 쓰지 않는다).
- **SparksTools:** `RunCommand(allocator, files, arguments, CommandOutput&)` → 종료 코드(T4). 작업 공간(`Private/Workspace`)은 인자 경로(폴더·`.spark`)에서 그 자신이나 조상 폴더의 `SparksPackage.json5`를 찾아 그 뿌리부터, 없으면 그 폴더 아래 패키지들을 훑는다(숨은 폴더 `.*` 제외, 깊이 32, 패키지 뿌리 중복은 한 번). 패키지 밖 `.spark`, 패키지가 하나도 없음, ID manifest 둘은 배치 오류(1), 읽기 실패는 3. 원문은 상한 + 1 byte까지 읽어 컴파일러가 S0103을 낸다. 진단 경로는 인자 표기 기준(`\` → `/`)이다. `check`는 진단과 `N errors in M modules`, `dump`는 모듈마다 `== 경로` 머리 줄과 덤프(그 단계 전에 멈춘 모듈은 안내 한 줄), 파일 인자면 그 모듈만.
- **프로그램:** `Programs/SparksCli`(대상 `SparksCli` — Launch Tool 진입, 표준 스트림 출력). 수동 확인(2026-11-23 Windows): 콘솔·리디렉션 출력, 종료 코드 0·1·2.
- **시험:** `SparksTools_CheckReportsDiagnosticsAndExitCodes`(폴더·파일 인자, 숨은 폴더·낯선 파일 무시, 패키지 밖·없는 경로), `SparksTools_UsageErrors`(8가지), `SparksTools_DumpPrintsEachModule`(경로 순·파일 인자·진단 있는 IR), `SparksTools_CommandAllocationFailure`(check·dump 전수), `SparksTools_CheckMatchesConformanceCorpus`(stage 지시 없는 corpus 사례 63개 — 폴더 사례는 그 폴더, 파일 사례는 임시 패키지 — 진단 열과 종료 코드가 `.expect`와 같다), `StandardStream_*`.
- **한계:** `stage` 지시가 있는 corpus 사례(113개)는 CLI가 모든 단계를 컴파일해 비교하지 않는다. 표준 입력 읽기의 실제 사용(언어 서버)은 LC8e.

## 7. LC8b 설계(개정 3)

### 7.1 build

- **인자:** `sparks build [--schema <폴더>]... --out <폴더> <경로>...`. `--out`은 `build`에만, 한 번만 쓴다(사용법 오류: `'build' needs --out <folder>`, `'--out' is only for 'build'`, `'build' takes one --out`).
- **컴파일:** check와 같다(모든 단계). 진단이 있으면 진단과 요약 줄만 쓰고 파일은 하나도 쓰지 않는다(1).
- **쓰기:** 모듈(진단 경로 byte 순)마다 `<출력>/<패키지 이름>/<모듈 경로>`(끝 `.spark` → `.sparkc`, `/` 구분 — 폴더는 `MakeDirectoryTree`로 만든다). byte는 `WriteBytecodeFile`. 대상이 이미 있고 byte가 같으면 쓰지 않는다. 다르면 같은 폴더의 임시 파일(`<대상>.<GUID 32자>.tmp`, CreateNew)에 다 쓰고 Flush·Close한 뒤 `Replace`로 교체한다(실패하면 임시 파일을 지운다 — 대상은 그대로). 파일마다 원자적이고 여러 파일 사이는 원자적이지 않다(중간 실패면 앞 모듈은 새 것, 뒤 모듈은 옛 것 — 다시 돌리면 맞춰진다).
- **출력:** 요약 줄 `0 errors in M modules`과 `wrote K of M modules`(K는 내용이 바뀌어 쓴 수). 입출력 실패는 표준 오류에 `<대상 경로>: cannot write the file`(또는 `cannot create the folder`) 한 줄과 3.

### 7.2 ids

- **인자:** `sparks ids [--schema <폴더>]... <경로>...`.
- **컴파일:** 모든 단계. 진단이 있으면 진단과 요약만 쓰고 발급하지 않는다(1 — SparksBytecode 6.1절 "오류가 있는 컴파일에서 도구는 발급하지 않는다"). 대기가 없으면 요약과 `no pending IDs`(0, 아무것도 쓰지 않는다).
- **편집(패키지마다):** 대기 이름 키의 첫 `/` 앞이 패키지 이름이다. 기존 manifest(작업 공간의 `*.rocksids.json` — 컴파일러가 S1006 없이 읽은 것)를 Core JSON reader로 항목 목록(kind·name·id·aliases·retired)으로 읽고 대기를 세 차례로 적용한다. (1) 옛 이름이 있는 type 대기: 활성 `type` 항목 `옛 이름`을 `새 이름`으로 바꾸고 옛 이름을 aliases에 더한다. 주인이 `옛 이름`인 멤버 항목(field·subobject·function·rpc·enumCase — 은퇴 항목 포함, C# `Rename`과 같다)도 주인 부분을 바꾸고 옛 이름을 aliases에 더한다. (2) 옛 이름이 있는 멤버 대기: 같은 규칙으로 그 항목만(옛 이름과 새 이름의 주인이 다르면 실패 — C#의 "explicit migration"). (3) 옛 이름이 없는 대기: 새 항목(새 GUID, aliases 없음, 활성). 주인(owner)은 C# `StableIdManifest.Owner`와 같다(마지막 `/` 뒤 첫 `.`의 다음 `.` 앞까지). 옮길 항목이 없거나(활성 항목 없음) 겹치면 내부 불일치로 실패한다(1 — 컴파일러가 대기를 냈다면 일어나지 않는다).
- **직렬화:** `{"schema": 1, "entries": [...]}` — 항목은 kind, 같으면 name의 byte(ordinal) 순, 항목 안 키 순서는 `kind`·`name`·`id`·`aliases`·`retired`, aliases는 byte 순, 들여쓰기 2칸·LF·끝 LF 하나(Core `JsonWriter` 기본 형식 = C# `System.Text.Json` `WriteIndented` + `"\n"`). 이름은 ASCII 식별자·`/`·`.`·`(`·`)`뿐이라 두 writer의 escape가 같다(C# 기본 encoder가 escape하는 비 ASCII·`<>&'+`는 Sparks 키에 없다). 16 MiB(C# `MaximumIdentityBytes`)를 넘으면 실패.
- **확인:** 새 manifest 원문들로 작업 공간을 다시 컴파일해 진단 0·대기 0이어야 쓴다(아니면 쓰지 않고 1 — `issuing the IDs did not settle the packages`).
- **쓰기(C# `StableIdManifest.Update`와 같은 규약):** 대상은 기존 manifest 경로, 없으면 패키지 뿌리의 `<패키지 이름>.rocksids.json`. `<대상>.lock`을 CreateNew로 만든다(있으면 `<대상>.lock: the manifest is locked by another tool` — 1, 비정상 종료로 남은 잠금은 사람이 지운다). 잠금 아래에서 대상을 다시 읽어 적재 때 byte와 같은지(없던 파일은 여전히 없는지) 본다(다르면 `the manifest changed while the IDs were issued` — 1). 임시 파일 `<대상>.<GUID 32자>.tmp`(CreateNew)에 쓰고 Flush·Close·`Replace`, 끝에 잠금을 지운다(실패 경로 포함). 패키지 사이는 원자적이지 않다.
- **출력:** 쓴 manifest마다(manifest 진단 경로 byte 순 — 쓰기 순서도 같다) `<manifest 진단 경로>: N new, R renamed`(R은 type·멤버 rename 대기 수 — 멤버 승계로 함께 옮긴 항목은 세지 않는다), 종료 0.

### 7.3 시험(LC8b)

- `SparksTools_BuildWritesModules`: 출력 경로·byte가 `WriteBytecodeFile`과 같고 다시 읽혀 verifier를 통과, 두 번째 build는 `wrote 0 of M`이고 수정 시각이 그대로, 원문을 바꾸면 그 모듈만 다시 씀, 진단이 있으면 출력 폴더를 만들지 않음, 사용법 오류 3가지.
- `SparksTools_IdsIssuesAndRenames`: type rename(멤버 승계·은퇴 멤버 포함)·멤버 rename·새 enumCase·handler 발급을 한 번에 — 결과 byte가 golden `Tests/Data/IdsGolden.rocksids.json`(새 ID 두 자리만 실제 값으로 바꿔 대조)과 같고, 다시 `ids`는 `no pending IDs`, `check`는 0. manifest 없는 패키지는 `<패키지>.rocksids.json`을 새로 만든다. 진단이 있으면 발급하지 않는다.
- `SparksTools_IdsLockAndConcurrentChange`: 잠금 파일이 있으면 1(파일 그대로), 잠금을 잡은 뒤 manifest가 바뀌면 1(끼운 FileSystem이 잠금 생성 직후 manifest를 바꾼다 — 시각에 기대지 않는 gate), `Replace` 실패면 3이고 잠금·임시 파일이 남지 않으며 원본 그대로.
- C# 왕복(`Rocks.ToolTests` `StableIdManifestTests`): 같은 golden을 `StableIdManifest.Load`·직렬화하면 byte가 같다(두 도구의 정규형이 같음).
- 할당 실패 전수: build·ids(매 시도 전에 원본 manifest·출력 폴더를 되돌린다 — 실패해도 잠금·임시 파일이 남지 않는다).

### 7.4 구현과 리뷰 반영(개정 4)

- **파일:** `Private/Command.cpp`(인자·분기 — 명령 이름을 옵션보다 먼저 검사), `CommandInternal.h`(명령 문맥), `BuildCommand.cpp`, `IdsCommand.cpp`, `IdManifest`(항목 읽기·rename·추가·직렬화), `ToolFiles`(비교·임시 파일 교체·GUID 32자), `Workspace`(파일 시스템 경로·패키지 뿌리·ID manifest 덮어쓴 컴파일). C# `StableIdManifest.Serialize`는 시험이 부르도록 `internal`이다.
- **컴파일러 승계 판정(SparksBytecode 6.1절 개정):** `@formerly`와 주인 rename 승계를 차례 넷으로 맞춘다 — (a) type의 `@formerly`, (b) `@formerly` 없는 멤버의 승계, (c) 멤버의 `@formerly`, (d) `@formerly`로 잇지 못한 멤버의 승계. 승계 대상이 은퇴 항목이면 S1007, 이미 다른 선언이 이었으면 S1008이다. 전에는 선언 순서대로 한 번에 맞춰 (1) 은퇴 멤버와 같은 이름을 type rename 뒤 다시 선언하면 진단 없이 새 발급 대기가 되고, (2) `@formerly("Old") var Fresh`가 `var Old`보다 앞이면 `Old`가 새 발급 대기가 됐다 — 둘 다 도구의 type rename이 옮긴 항목과 이름이 겹쳐 manifest가 C# 검사를 통과하지 못한다(`ids`는 확인 단계에서 막혔다). 발급 대기 목록은 맞춤 뒤 선언 순으로 만든다.
- **S1006 보강:** 최상위의 `schema`·`entries` 밖 키, 항목의 다섯 키 밖 키를 거부한다(C# `Load`와 같다 — 도구가 다시 쓸 때 모르는 키가 사라지지 않게). 같은 키 둘은 JSON reader가 거부한다.
- **도구의 자체 검사:** rename·추가가 같은 kind의 이름·alias(은퇴 포함 — C# `Validate`의 이름 집합)와 겹치면 그 대기에서 멈추고 1(`cannot rename …`·`cannot add …: the name is taken`). 확인 컴파일이 실패하면 그 진단과 남은 대기를 표준 오류에 쓴다. 이 셋과 manifest 읽기 실패(`cannot read the ID manifest`)는 컴파일러가 S1006·S1007·S1008 없이 대기를 냈다면 일어나지 않는 방어 경로다(명령 시험으로는 닿지 않는다 — 컴파일러 판정은 `SparksStableIds_*`가 시험한다).
- **build 오류 문장:** `--out`을 경로로 쓸 수 없음(`cannot use the output folder`), 모듈이 `.sparkc` 표 상한을 넘음, 교체 뒤 폴더 기록 실패(NotDurable — `the file was replaced, but the folder could not be flushed to the storage`, 새 내용은 이미 보인다)를 한 줄로 알린다(3). 할당 실패만 문장 없이 3이다.
- **한계 — 잠금 공유 모드:** PlatformBase `File`은 Windows에서 언제나 읽기·쓰기·삭제 공유로 연다. 그래서 C#(`FileShare.None`)과 달리 `sparks ids`가 잡은 잠금은 다른 프로세스가 지울 수 있다. 사람이 쓰는 중인 잠금을 지우고 두 번째 실행이 잠금을 잡으면 첫 실행이 끝에서 그 잠금을 지울 수 있다(같은 원본에서 시작한 두 쓰기 중 하나의 발급을 잃는다 — 손상은 없다, 잠금 아래 비교가 원본 변경을 잡는다). POSIX에는 공유 모드가 없어 C#도 같은 한계다. 배타 열기 모드는 PlatformBase에 요구가 생기면 더한다.
- **시험(7.3절 + 리뷰):** `SparksTools_BuildWritesModules`(NotDurable 포함), `SparksTools_IdsIssuesAndRenames`, `SparksTools_IdsSeveralPackages`(기존·새 manifest 섞임, `Game`·`GameX`, 인자 순서와 무관한 결과 순서), `SparksTools_IdsLockAndConcurrentChange`, `SparksTools_BuildIdsAllocationFailure`, `SparksTools_UsageErrors`(build 인자 4가지·알 수 없는 명령 우선), `SparksStableIds_CarriedMemberRetiredOrClaimed`(두 선언 순서), `SparksStableIds_InvalidManifestsS1006`(모르는 키·같은 키), C# `StableIds.SerializationMatchesSparksIds`. 수동 확인(2026-11-23 Windows): `SparksCli`의 `ids`(발급·재실행 `no pending IDs`), `build`(재실행 `wrote 0 of 2`), `--out` 없음(2).

## 8. LC8c 설계(개정 5)

### 8.1 API와 입력

- `SparksCompiler/Formatter.h` — `Result<bool> FormatSource(const SourceSet&, SourceId, DiagnosticList&, StringBuilderBase& out)`: 원문 하나의 공식 서식을 out에 붙인다. 값 false는 어휘·구문 진단(S01xx·S02xx — `ParseFile`이 내는 것)이 있어 서식하지 않음이다(진단은 diagnostics에, out은 그대로). 의미 진단(이름·타입 등)은 보지 않는다(서식은 파일 하나의 구문만 쓴다).
- **주석 수집:** lexer에 주석 범위 수집(`TokenizeWithComments` — 공개, 언어 서버도 쓴다)을 더한다. 일반 줄 주석(`//`, 문서 주석 `///`는 이미 토큰)과 닫힌 블록 주석의 범위를 원문 순서로 돌려준다. 컴파일 경로(`Tokenize`)는 그대로다(주석 배열을 만들지 않는다).
- **구문 정보:** 토큰은 `ParseFile`의 트리 토큰을 쓴다. 트리에서 가린다: 타입 인자의 `<`·`>`(`Type`·`Identifier`·`Member` 노드의 타입 인자 목록 — 여는 `<`는 이름 토큰 다음, 닫는 `>`는 깊이를 세어 찾는다. 타입 인자를 닫는 `>=`는 트리 토큰에서 `=`로 바뀌어 있으므로 그 앞 한 글자 `>`를 닫는 토큰으로 되살린다), 단항 `!`·`-`(`Unary` 노드), attribute의 끝 토큰(`Attribute` 노드 범위의 끝), enum 본문의 `{`(`Enum` 노드).
- **문자열:** 문자열 리터럴(`"`부터 닫는 `"`까지, 보간 포함)과 verbatim 문자열은 원문 그대로 하나의 단위다(보간 안은 서식하지 않는다 — 값 보존이 확실하다. 보간 서식은 요구 시).

### 8.2 알고리즘(token 열 위 한 번 걷기)

단위(토큰·문자열·주석)를 원문 순서로 걸으며 단위 사이에 줄바꿈 여부·빈 줄·공백만 정한다(token 글자는 그대로 — 줄 끝 CRLF는 LF, 주석의 줄 끝 공백은 지운다).

- **줄바꿈:** 앞이 줄 주석이면 반드시. 주석이 원문에서 앞 단위와 같은 줄이면 그 줄에 둔다(뒤 주석). `{`·`,`·`;`·(`}` 뒤의) `else` 앞은 줄을 바꾸지 않는다(앞 줄에 붙인다). `;`·`{`·`}` 뒤, `}` 앞, 문서 주석 앞뒤는 반드시. 그 밖은 원문에 줄바꿈이 있을 때만.
- **빈 줄:** 원문의 빈 줄은 하나로 줄이고, `{` 바로 뒤와 `}` 바로 앞은 없앤다. 최상위에서 선언(`;`·`}`로 끝난 것) 뒤에 다음 최상위 항목이 오면 빈 줄 하나를 넣는다(둘 다 `import`면 원문을 따른다 — 그 앞의 주석·문서 주석·attribute도 항목에 든다). 파일 처음의 빈 줄은 없애고 끝은 LF 하나.
- **들여쓰기:** 열린 `{` 수 × 4 + 단계 × 4. 단계는 그 줄 앞에서 열려 아직 닫히지 않은 `(`·`[`를 연 줄의 수(같은 줄에서 연 여러 괄호는 하나), 그리고 문장이 괄호 밖에서 줄을 바꿔 이어지면 1을 더한다(경계 — `;`·`{`·`}`·문서 주석·attribute 끝·enum 본문의 `,` — 뒤의 줄은 이어지는 줄이 아니다). 닫는 `}`·`)`·`]`로 시작하는 줄은 그 줄에서 닫는 것을 빼고 센다. 자기 줄의 주석은 그 자리 다음 줄과 같은 들여쓰기다(닫는 괄호 조정 없음).
- **공백(같은 줄):** 없음 — `,`·`;`·`)`·`]`·`.`·`?.`·`..<`·`:`·후위 `?`·타입 인자 `<`·`>` 앞, `(`·`[`·`.`·`?.`·`..<`·`@`·단항 연산자·타입 인자 `<` 뒤, 이름·`)`·`]`·`this`·문자열·타입 인자 `>` 뒤의 `(`(호출), 이름·`)`·`]`·`this`·문자열 뒤의 `[`(색인). 그 밖은 한 칸(키워드 뒤 `(`, 이항 연산자 앞뒤, `,`·`:`·`;` 뒤, `{` 앞, 주석 앞뒤).
- **멱등:** 출력은 위 규칙의 고정점이다(다시 서식하면 같다 — 줄바꿈은 원문에 있던 것과 강제한 것만, 빈 줄은 0·1, 공백은 규칙만으로 정해진다).

### 8.3 fmt 명령

- `sparks fmt [--check] <경로>...` — 작업 공간(T3)의 모듈 원문 중 인자 아래 모듈(폴더 인자는 그 폴더 아래 — 패키지 안 하위 폴더면 그 아래만, 파일 인자는 그 파일, 섞어도 합집합)을 서식한다. `dump`도 같은 대상 규칙이다. 컴파일하지 않는다(패키지 manifest·schema 진단은 보지 않는다).
- `fmt`: 서식이 다른 파일을 원자적으로 바꾼다(LC8b `ReplaceFile`). 출력 `formatted K of M files`. `--check`: 쓰지 않고 다른 파일마다 `<경로>: not formatted`, 출력 `K of M files need formatting`.
- 어휘·구문 진단이 있는 파일은 진단을 쓰고 건드리지 않는다. 종료: 0(모두 서식됨·바꿈), 1(진단 있음, `--check`에서 다른 파일 있음), 2(사용법), 3(입출력·할당). `--check`는 `fmt`에만.

### 8.4 시험(LC8c)

- 서식 golden: `SparksCompiler/Tests/Formatting/<이름>.spark`(입력) → `<이름>.fmt`(기대 출력) — 규칙마다 사례(공백·연산자·타입 인자·`>=` 분리·단항·괄호 줄바꿈·이어지는 줄·블록·`} else`·빈 줄·최상위 빈 줄·주석 자리·문서 주석·attribute·enum·switch·문자열·verbatim·CRLF·탭).
- corpus 전체: conformance의 모든 `.spark`(폴더 사례·revision 포함) 중 어휘·구문 진단 없는 것마다 멱등(fmt(fmt(x)) = fmt(x)), token 열 보존(토큰 글자 열이 같다 — `>=` 분리는 `>`·`=`로 맞춰 비교), 주석 글자 열 보존, 서식 결과도 진단 없음.
- 구문 오류 파일은 false·out 그대로. 할당 실패 전수(FormatSource, fmt 명령).
- `fmt` 명령 시험: 바꿈·같으면 쓰지 않음·`--check`(1)·구문 오류 파일(1, 그대로)·파일 인자.

### 8.5 구현과 리뷰 반영(개정 6)

- **파일:** `SparksCompiler` — `Public/Formatter.h`·`Private/Formatter.cpp`, `Lexer.h`의 `TokenizeWithComments`(공개 — 언어 서버도 쓴다). `SparksTools` — `Private/FmtCommand.cpp`, `Command.cpp`(`--check` — `fmt`에만, 한 번만), 작업 공간의 인자 선택(`WorkspaceFile::selected` — 폴더 인자 아래·파일 인자).
- **주석 자리 규칙(리뷰 반영):** 줄 처음 블록 주석 뒤라도 `}`는 자기 줄로 간다. `}`와 `else` 사이에 블록 주석이 있어도 `} /* c */ else {` 한 줄이다(줄 주석이 있으면 줄을 바꾼다 — 바꿀 수 없다). `{` 뒤 첫 줄바꿈까지(같은 줄 뒤 주석이 있어도)와 문서 주석 뒤에는 빈 줄을 두지 않는다. 자기 줄에 오는 `{`(앞이 줄 주석)는 이어지는 줄 들여쓰기를 받지 않는다. 문서 주석도 줄 끝 공백을 지운다.
- **한계:** `;`·`,` 앞의 줄 주석(`y() // c` 다음 줄 `;`)은 줄을 바꿀 수밖에 없어 `;`가 이어지는 줄로 들여쓰인다(멱등). 줄 처음 블록 주석 뒤의 `)`·`]`는 그 줄에 남는다. 보간 `${ }` 안은 서식하지 않는다. `fmt` 쓰기 실패는 그 자리에서 3으로 끝난다(앞에서 바꾼 파일은 남는다 — build와 같다).
- **시험:** `SparksFormatter_Golden`(`Formatting/` 5사례 — Spacing·Blocks·Continuation·Comments·Edges), `SparksFormatter_CrlfTabsAndBlankEdges`(CRLF·탭·빈 파일·주석만·BOM), `SparksFormatter_SyntaxErrorsAreNotFormatted`, `SparksFormatter_CorpusIdempotentAndTokenPreserving`(conformance 전체), `SparksFormatter_CorpusVariantsIdempotentAndTokenPreserving`(corpus 원문마다 토큰 경계에 블록 주석·줄 주석을 끼우거나 모든 경계를 줄바꿈으로 바꾼 결정적 변형 셋 — 멱등·token 열·주석 열), `SparksFormatter_AllocationFailureEverywhere`(실패를 견딘 시도의 출력이 기준과 같음), `SparksTools_FmtRewritesAndChecks`·`SparksTools_FmtSelectsArgumentModules`·`SparksTools_FmtAllocationFailure`. 수동 확인(2026-11-23 Windows): `SparksCli fmt --check`(1)·`fmt`(0)·재검사(0).

## 9. LC8d 설계(개정 7)

### 9.1 실행기 API(SparksTestHost)

- `SparksTestHost/TestRunner.h`(공개): `Result<void> RunTests(Allocator&, const Compilation&, const SourceSet& sources, Span<const uint32> modules, const TestOptions&, TestReport& out)`. compilation은 진단 없이 끝난 것(모든 단계 — `ModuleBytecode`가 모든 모듈에 있다)이고, modules는 시험을 찾을 모듈 번호(진단 경로 byte 순으로 호출자가 정한다)다. 링크는 compilation의 모든 모듈을 한 host로 묶고 `LinkImports`로 모듈 사이 호출을 푼다(시험 모듈이 다른 모듈 함수를 부른다). 실패: 링크 거부(InvalidArgument — 사유는 `out.reason`), 할당 실패(OutOfMemory). 시험 하나의 실패는 실행기 실패가 아니라 결과다.
- **시험 찾기:** 모듈마다 `Symbols()` 중 그 모듈의 최상위 `Function`이고 `@test` attribute가 있는 것을 선언 순서(원문 위치)로. `@test`는 인자와 반환 타입이 없는 함수다(개정 8 — 반환 타입이 있으면 S0621: 실행기가 반환 값을 보지 않으므로 `Result` 오류 반환이 조용히 통과하지 않게. SL 3.13 표도 "인자·반환 타입 없는 최상위 함수"로 고쳤다 — 개정 9). 컴파일러 공개 API `Compilation::HasAttribute(symbol, name)`(언어 서버도 쓴다)을 더한다. bytecode 함수는 같은 이름의 최상위 함수(이름에 `.`가 없는 것)다.
- **결과(`TestResult`):** 모듈·시험 심볼·결과 종류·위치(SourceId와 byte 범위 — 없으면 시험 이름)·상세 글자·진행한 프레임 수. 종류: `Passed`, `Faulted`(VM fault 종류 — `AssertFailed` 포함), `EndedWithError`(`Result`가 아닌 시험의 `try` 실패 — SL 8.4, 오류 코드), `NeedsEngine`(엔진 기능 이름), `DidNotFinish`(프레임 상한).
- **옵션(`TestOptions`):** `maxFrames`(기본 `DefaultTestMaxFrames` = 600 — 60 Hz 10초), 시험 프레임 시간은 `TestFrameSeconds` = 1/60.

### 9.2 headless host

- FakeHost의 묶기(`Bind` — schema 구동 서술자 대조)·리터럴(`MakeLiteral` — 이름 표)·이름 표기(`AppendName`·`AppendEnumName`)를 그대로 쓰고, 엔진 객체를 만들거나 읽는 호출은 모두 거부한다: `GetContext`(`World`·`Session` …), `GetField`·`SetField`, `CallNative`, `ResolveVirtual`, `InitializeStruct`(엔진 struct 기본값), `StartLoad`(0 — 계약 위반, 거부 이름 `Soft.Load`), 객체를 만드는 리터럴(Ref bank — 값 리터럴 `Tag`·`Name`은 만든다). 거부는 `ContractViolation` fault이고 host가 처음 거부한 묶음 이름(`World`·`Actor.Destroy` …)을 기억한다 — 실행기는 그 시험의 결과를 `NeedsEngine`으로 바꾼다(fault 위치 그대로). 살아 있는 객체가 없으므로 `IsAlive`·`IsA`는 거짓, `ReleaseHeld`는 할 일이 없다.
- 엔진 연동(EI)은 같은 실행기에 World fixture host를 끼운다(그때 `TestOptions`에 host 선택을 더한다 — 지금은 headless 하나).

### 9.3 실행

- 시험마다 새 `VmSession`(host·링크는 공유 — 시험 사이 상태는 host의 "처음 거부한 이름"뿐이고 시험마다 지운다)에 모든 링크 모듈을 `AddModule`하고 `CallFunction`으로 부른다. 반환이면 통과, fault·`Ended`면 그 결과다.
- 정지(`Suspended`)면 프레임을 진행한다. 프레임 하나 = `FixedStep()` → `Advance(TestFrameSeconds, TestFrameSeconds)` → `ApplyRequests()` → `EndFrame()`(SL 5.4 순서의 고정 재개 → 가변 재개 → 적용 → 끝 정리 — instance가 없어 뒤 둘은 할 일이 없지만 엔진 fixture와 같은 틀). 프레임마다 `Deliveries()`에서 이 시험 실행의 끝(재개 기록 중 하위 실행이 아닌 것 — `Delivery::child` 거짓, 결과가 정지가 아님)을 찾는다. `maxFrames` 프레임 뒤에도 정지해 있으면 `DidNotFinish`(위치는 시험 이름)이고 session을 닫아 실행을 취소한다. 끝 기록은 FixedStep·Advance·ApplyRequests·EndFrame 뒤마다 찾고, 끝 기록 없이 정지 실행이 사라지면(엔진 fixture의 취소 경로) 실행기 거부(InvalidArgument — `a test execution was cancelled without a result`)다(조용히 `DidNotFinish`가 되지 않게).
- 시험 뒤 `CloseSession` — 실행 문맥 ledger가 0이어야 한다(아니면 실행기 버그 — `ContractViolation` 결과가 아니라 실행기 실패 InvalidArgument). 대기·정지 실행은 session과 함께 사라진다.
- **순서·결정성:** 모듈 순서는 호출자, 모듈 안은 원문 위치 순. 시험 사이 공유 상태가 없으므로 결과는 시험 하나만 돌려도 같다. 시계는 프레임 수로만 진행한다(실제 시간 없음 — SL 13.1).

### 9.4 VM 최상위 진입(SparksRuntime 2.8절)

- `VmSession::CallFunction(module, function, scalars, refs, buffers)`: instance 없이(this 없음) 최상위 함수를 부른다(메서드 — 이름 `Owner.Name` — 는 InvalidArgument). `Call`과 같은 진입 규칙(발행 전달·반환 값 보관·fault 기록)이고 다른 점은 instance가 없다는 것뿐이다 — 정지 실행의 instance는 `NoBytecodeIndex`(동시 실행 상한·격리 대상 아님 — instance 예산이 없다), fault는 위치 기록(`Faults()`)만 남고 격리 계수가 없다. `Detach`·`DetachAll`이 건드리지 않으며 `CloseSession`이 취소한다. `Detach`의 "이 instance를 this로 쓰는 frame" 판정은 클래스 메서드 frame(이름 `Class.Name`의 Class가 그 모듈 클래스)만 본다 — 최상위 함수·struct 메서드 frame의 Ref 0번은 this가 아니다(개정 8 리뷰 반영, 전에는 모든 frame의 Ref 0번을 this로 보았다).
- `Delivery::child`: 재개 기록이 하위 실행(CALLW — `All`·`Race` 인자)의 것인지다. 하위 fault로 부모가 끝난 기록은 부모의 것이다(`child`는 부모가 하위 실행인지).
- 엔진 연동에서 최상위 진입을 쓰는 곳은 `@test`(World fixture)와 편집기 명령(요구 시)이다 — 게임 실행 경로는 언제나 instance가 있다.

### 9.5 test 명령

- `sparks test [--schema <폴더>]... [--max-frames <N>] <경로>...` — check와 같이 작업 공간 전체를 컴파일하고(진단이 있으면 진단과 요약, 시험은 돌리지 않는다 — 1), 인자 아래 모듈(8.3절 대상 규칙)의 시험을 돌린다. `--max-frames`는 `test`에만, 한 번만, 1–`MaxTestFrames`(1,000,000)의 10진수다(사용법 오류: `'--max-frames' is only for 'test'`, `'test' takes one --max-frames`, `'--max-frames' needs a number from 1 to 1000000`).
- **출력(표준 출력):** 실패한 시험마다 진단과 같은 형식(SL 8.1 — `AppendSourceReport`)으로 한 보고: 첫 줄 `<경로>:<줄>:<열>: error <종류>: Test '<이름>' failed.`(종류 자리는 S코드 대신 결과 이름 — `AssertFailed`·`IntegerOverflow`·…·`EndedWithError`·`NeedsEngine`·`DidNotFinish`), 원문 줄과 밑줄(fault 위치의 토큰 — 위치가 없으면 시험 이름), 설명 줄(종류별 — `NeedsEngine`은 `'<이름>' needs the engine; tests run on the headless host until engine integration (EI).`, `DidNotFinish`는 `The test was still waiting after <N> frames.`, `EndedWithError`는 `A 'try' failed with 'Error.<case>'.`(컴파일러가 해석한 엔진 타입 `Error`의 case — 찾지 못하면 `error code <N>`), 다른 fault는 `The test stopped with the fault '<종류>'.`). 보고 순서는 시험 순서. 끝 줄 `<P> passed, <F> failed`(시험이 없으면 `no tests`).
- 종료: 0(모두 통과·시험 없음), 1(컴파일 진단·실패한 시험·실행기 거부 — 표준 오류 `sparks: cannot run the tests: <사유>`, 링크·import 거부 등), 2(사용법), 3(입출력·할당).
- 컴파일러에 `AppendSourceReport(out, sources, source, hasSpan, span, code, message, explanation, fix)`(공개)를 더하고 `AppendDiagnostic`이 그것을 쓴다(같은 원문 줄·밑줄·제어 문자 처리 — 형식이 갈라지지 않게).

### 9.6 시험(LC8d)

- 실행기(`SparksTestHost/Tests`): 통과(동기·`async` — `NextTick`·`Delay`·`NextFixedTick`·`All`·`Race`·다른 모듈 함수), `Assert` 실패(위치), 다른 fault(정수 넘침·0 나눗셈·색인), `try` 실패(`EndedWithError`), 엔진 기능마다 `NeedsEngine`(문맥·native 함수·필드·로드 — 이름), 끝나지 않음(`while` + `NextTick` — 프레임 상한과 정확한 프레임 수), 하위 실행 fault가 부모 결과로, 시험 사이 독립(앞 시험의 정지 실행·거부 이름이 뒤 시험에 남지 않음 — 같은 모듈을 다시 돌려도 결과 같음), 여러 모듈 순서, 할당 실패 전수(결과가 기준과 같거나 OutOfMemory).
- VM(`SparksTestHost` 시험 — 원문을 컴파일해 쓰므로 컴파일러가 있는 곳): `CallFunction` — 동기 반환, 정지·재개, 하위 실행의 `child` 표시, fault 기록, `CloseSession` 취소 뒤 ledger 0, this가 있는 함수·잘못된 인자 모양 거부.
- 명령(`SparksTools`): 출력 golden(통과·실패 섞임 — 보고 형식, 요약), 진단 있는 작업 공간(시험 안 돎, 1), 시험 없음(0), `--max-frames` 사용법 오류 셋과 적용, 할당 실패 전수.

### 9.7 구현과 리뷰 반영(개정 8)

- **파일:** `SparksTestHost` — `Public/SparksTestHost/TestRunner.h`, `Private/TestRunner.cpp`(headless host `TestHeadlessHost` — FakeHost 묶기 위임, 실행 루프, 보고), FakeHost `BindingAt`. `SparksTools` — `Private/TestCommand.cpp`, `Command.cpp`(`--max-frames`). `SparksVm` — `VmSession::CallFunction`·`Enter`(Call과 공통 진입)·`Delivery::child`, `CancelExecutions`의 this frame 판정. `SparksCompiler` — `Compilation::HasAttribute`, `AppendSourceReport`(`AppendDiagnostic`이 쓴다), `@test` 반환 타입 S0621.
- **엔진 enum 값 수정(컴파일러 버그):** 엔진 enum case 값이 schema 전체 case 표의 번호(`Symbol::item`)로 내려가고 있었다(규격은 그 enum 안의 case 번호 — SparksBytecode 2절 `Error` case 번호 + 1, host `AppendEnumName`). `IrBuilder::EnumIndex`가 enum의 첫 case를 뺀다. conformance golden 7개(`Codegen/Edges`·`Flow`의 `.ir`·`.bc`, `Run/Async`·`Structs`·`Values`의 `.run`)가 그 값만 바뀌었다. 엔진 연동 host(EI)도 native·로드 오류를 "enum 안 번호 + 1"로 낸다.
- **리뷰 반영:** `Result`를 반환하는 `@test`의 오류 반환이 통과로 세어짐 → 반환 타입 금지(S0621, conformance `Rules/Attributes` 사례). instance 없는 정지 실행이 `Detach`의 this frame 판정으로 조용히 취소될 수 있음 → 판정을 클래스 메서드 frame으로 좁히고 실행기는 끝 기록 없는 소멸을 거부. 링크 거부 종료 코드 3 → 1. `CallFunction`의 메서드 거부와 시험. 밑줄을 구문 트리 토큰으로. 로드 거부 이름 `Soft.Load`. `Error` case 이름은 컴파일러 심볼로. 엔진 struct 기본값·로드 거부·Tag 값 리터럴·색인 fault 시험. 프레임 수 해석은 상한을 넘는 순간 멈춘다(자리 수 상수 대신 — 앞의 0은 된다).
- **한계:** 최상위 함수에는 문맥 이름이 없어(S0321) `@test`가 `World`를 쓸 수 없다 — headless World fixture(SL 13.1)는 EI에서 `@test` 문맥 규칙(SL 개정)과 함께 정한다. `Assert` 문구는 원문 줄로만 보인다(설명 줄에 따로 넣지 않는다). 시험의 `Log` 출력은 보이지 않는다(요구 시 `--verbose`). 정지 실행의 재개 순서에서 instance 없는 실행은 생성 번호 0이라 같은 프레임의 instance 실행보다 먼저다(결정적).
- **시험:** `SparksTestRunner_ResultsOfEachKind`(11가지 — 프레임 수·위치), `SparksTestRunner_EngineRefusalsAndIndexFault`, `SparksTestRunner_ReportFormat`(보고 golden), `SparksTestRunner_TestsAreIndependent`, `SparksTestRunner_RejectsBadInput`, `SparksTestRunner_CallFunctionSession`, `SparksTestRunner_AllocationFailureEverywhere`, `SparksTools_TestRunsAndReports`·`SparksTools_TestNoTestsAndDiagnostics`·`SparksTools_TestAllocationFailure`, `SparksTools_UsageErrors`(`--max-frames` 8가지). 수동 확인(2026-11-23 Windows): `SparksCli test`(통과·Assert·0 나눗셈·끝나지 않음·엔진 필요 섞임 — 1).

## 10. LC8e 설계(개정 10)

### 10.1 구조

- `SparksTools/LanguageServer.h`(공개): `MessageChannel`(입력 읽기 `Read(Span<char>) → Result<usize>`(0은 입력 끝), 출력 쓰기 `Write(StringView)`, 기록 `Log(StringView)` — 표준 오류)과 `int32 RunLanguageServer(Allocator&, platform::FileSystem&, Span<const StringView> arguments, MessageChannel&)`. 프로그램 `Programs/SparksLsp`(대상 `SparksLsp`, Launch Tool)는 표준 입력·출력·오류(SE-B7)를 잇는 채널로 이 함수를 부르기만 한다. 시험은 각본 입력(조각 크기를 정한 채널)으로 같은 프로세스에서 부른다.
- 인자: `--schema <폴더>`(여러 번 — CLI와 같다), `--stdio`(VS Code가 넘긴다 — 받아서 무시). 그 밖은 표준 오류에 사용법 한 줄과 2. 초기화 옵션 `initializationOptions.schemas`(문자열 배열 — 첫 작업 공간 뿌리 기준 또는 절대 경로)도 schema 폴더에 더한다. 상대 경로는 받을 때 절대 경로로 바꾼다(명령줄은 작업 디렉터리 기준, 초기화 옵션은 첫 뿌리 기준 — 뿌리가 없으면 작업 디렉터리) — schema 파일 진단의 URI가 절대 경로여야 한다.
- 파일: `Private/LspFraming`(Content-Length 틀), `LspServer`(JSON-RPC 해석·응답 쓰기·수명·문서 저장소·분석·발행 — 요청 종류가 적어 프로토콜 층을 따로 두지 않는다), `LspText`(UTF-16 위치 변환·`file` URI·경로 비교 key), `LspSemanticTokens`. 작업 공간 덮기는 `Workspace::Overlay`(LC8a 작업 공간에 더함).

### 10.2 틀과 JSON-RPC

- 메시지 = 머리 줄들(`이름: 값\r\n`) + 빈 줄(`\r\n`) + 본문 `Content-Length` byte(UTF-8 JSON). `Content-Length`는 필수·10진수·`MaxLspMessageBytes`(64 MiB) 이하, `Content-Type`은 받고 무시, 다른 머리는 무시, 머리 줄 하나는 `MaxLspHeaderBytes`(1,024 byte) 이하, 머리 전체는 `MaxLspHeaderBlockBytes`(4,096 byte) 이하(줄 끝 없는 입력이 버퍼를 끝없이 키우지 않게 — 줄 하나를 다 읽을 때마다와 줄을 기다릴 때 모두 본다: 조각 크기와 상관없이 같은 판정). 틀이 깨지면(필수 머리 없음·숫자 아님·상한 초과·줄 끝 없이 입력 끝) 표준 오류에 한 줄을 쓰고 1로 끝난다(틀을 다시 맞출 수 없다). 출력도 같은 틀(`Content-Length: N\r\n\r\n` + 본문 — 본문은 Core `JsonWriter` compact).
- JSON-RPC 2.0: 요청(id·method), 알림(id 없음), 응답(클라이언트 응답은 받고 버린다 — 서버는 요청을 보내지 않는다). 본문이 JSON이 아니면(잘못된 UTF-8·글자 중간에서 끊긴 본문 포함 — 틀은 byte 그대로 넘긴다) id null로 ParseError(-32700), 모양이 틀리면 InvalidRequest(-32600), 모르는 요청은 MethodNotFound(-32601), 인자가 틀리면 InvalidParams(-32602). 모르는 알림과 `$/`로 시작하는 알림(`$/cancelRequest`·`$/setTrace`)은 무시한다 — 요청은 받은 순서로 하나씩 끝까지 처리하므로(10.4절) 취소할 진행 중 요청이 없다.

### 10.3 수명

- `initialize` 전의 요청은 ServerNotInitialized(-32002), 알림은 버린다(`exit` 제외). `initialize` 결과: `capabilities` = `positionEncoding: "utf-16"`, `textDocumentSync: { openClose: true, change: 1 }`(전체 내용 — T11), `semanticTokensProvider: { legend, full: true }`, `serverInfo: { name: "sparks-lsp" }`. 두 번째 `initialize`는 InvalidRequest.
- 작업 공간 뿌리: `workspaceFolders`(있으면 전부), 없으면 `rootUri`, 둘 다 없으면 작업 공간 없음 — 분석하지 않는다(진단·강조 없음, initialize 때 표준 오류에 한 줄). 편집기는 폴더를 열고 쓰는 것이 기본이고, 단일 파일 모드(열린 문서의 패키지를 찾아 올라가기)는 저장 전 문서·패키지 밖 문서가 섞이면 작업 공간 읽기 전체가 실패하므로 LC8g에서 필요를 본 뒤 정한다. `file` URI만 받는다(다른 scheme 폴더는 무시).
- `shutdown` → null 결과, 그 뒤 요청은 InvalidRequest. `exit` 알림 → shutdown을 받았으면 0, 아니면 1로 끝난다. `exit` 없이 입력이 끝나면 1.

### 10.4 문서 저장소와 분석

- `didOpen`(uri·version·text)은 문서를 열린 문서 표에 넣고, `didChange`(전체 내용 — 마지막 change의 text)는 내용과 version을 바꾸고, `didClose`는 표에서 뺀다(디스크 내용으로 돌아간다). 문서 수 상한 `MaxLspDocuments`(4,096) — 넘는 didOpen은 표준 오류에 알리고 무시.
- **분석:** `initialized` 알림(디스크 작업 공간의 첫 진단)과 문서 알림마다 곧바로(받은 순서 — 결정적) 작업 공간 전체를 다시 읽어(T3 — 디스크에서 바뀐 파일도 반영) 열린 문서 내용으로 덮고 컴파일한다(모든 단계 — CLI `check`와 같은 `Compilation`). 덮기: 경로가 같은 작업 공간 파일의 내용을 바꾸고, 작업 공간에 없는 `.spark`가 어떤 패키지 뿌리 아래면 그 패키지 모듈로 더한다(저장 전 새 파일). 뿌리가 겹치면(중첩 패키지) 가장 안쪽 패키지이고, `.`로 시작하는 폴더·파일 아래는 모듈이 아니다(작업 공간 읽기와 같다). 그 밖의 열린 문서(패키지 밖)는 분석하지 않는다(진단 없음). 경로 비교는 `/` 구분으로 맞추고 Windows 드라이브 글자만 소문자로 본다(그 밖의 대소문자는 구분한다). 작업 공간 읽기가 실패하면(패키지 밖 원문 등) 진단을 모두 지우고 그 이유를 표준 오류에 쓴다 — 이유가 바뀔 때만.
- 매 분석이 전체를 새로 컴파일한다 — 증분 분석·디스크 결과 보관은 SL 13절 목표(파일 1,000개 p95 300 ms)를 잰 뒤(T11).

### 10.5 진단 발행

- 분석마다 작업 공간 파일(모듈·manifest·ID manifest·schema)과 열린 문서마다 진단 목록을 만들고, 지난번 발행과 다른 URI만 `textDocument/publishDiagnostics`(uri, 열린 문서면 version, diagnostics)로 보낸다(이전에 진단이 있던 URI가 이번 대상에 없으면 — 지운 파일·닫은 새 문서·다른 철자로 다시 연 문서 — 빈 목록으로 지운다). 발행 순서는 지움 먼저, 그다음 URI byte 순(두 철자를 한 key로 보는 클라이언트에서 뒤에 온 지움이 새 진단을 덮지 않게). 지난 발행 표에는 빈 목록이 아닌 URI만 둔다(없음 = 빈 목록). 작업 공간 파일의 URI는 서버가 경로에서 만들고(10.6절), 열린 문서는 클라이언트가 보낸 URI 그대로 쓴다. 같은 파일을 다른 철자(대문자 드라이브·`:`)로 열면 문서 표에는 URI마다 따로 있고, 덮기·발행은 경로 key로 한 파일이다(마지막에 연 문서의 내용·URI·version).
- 진단 하나: `range`(10.6절 — 원문 위치가 없는 진단은 0:0–0:0), `severity: 1`, `code: "S0412"`, `source: "sparks"`, `message` = 메시지, 설명이 있으면 `\n` + 설명, 수정 예가 있으면 `\nfix: ` + 수정 예(CLI의 줄들과 같은 내용 — 원문 줄·밑줄은 편집기가 보이므로 뺀다). 순서는 SL 8.1 순서.

### 10.6 위치 변환

- LSP 위치는 0부터 센 줄과 UTF-16 code unit 열이다(T11). 줄은 SL 8.1 줄(LF 기준 — CR은 줄 끝)과 같다. 열: 줄 처음부터 그 byte 위치까지 code point마다 U+10000 미만은 1, 이상은 2, 잘못된 UTF-8 byte는 1(U+FFFD — SL 8.1 표시와 같은 대체). BOM은 1줄 0열 앞(BOM 안 위치는 0:0).
- `file` URI ↔ 경로: `file://` + (Windows `/c:/…`·`/c%3A/…`의 드라이브 글자 앞 `/` 제거) + percent decoding(UTF-8), 경로 → URI는 segment마다 RFC 3986 unreserved 밖을 `%XX`(대문자 — Core `TryPercentEncode`)로, 드라이브 글자는 소문자, 드라이브 `:`도 `%3A`(VS Code가 보내는 모양 `file:///c%3A/…`와 같게). authority는 비었거나 `localhost`(RFC 8089)만 받는다. 잘못된 percent·UTF-8 아님·다른 authority·query·fragment·percent로 쓴 경로 구분자(`%2F`·`%5C`)는 그 URI를 무시한다(진단 없음). 경로 비교 key는 `/` 구분·끝 `/` 없음·드라이브 소문자다(대소문자는 그 밖에 그대로 — 파일 체계가 돌려준 철자를 쓴다).

### 10.7 의미 강조(T12)

- `textDocument/semanticTokens/full` → `{ data }`(5개씩: 줄 차이·시작 차이·길이·종류·modifier bit — UTF-16). 열린 문서·작업 공간 모듈만(그 밖은 빈 data). 마지막 분석 결과를 쓴다(요청이 분석을 다시 하지 않는다 — 문서 알림마다 분석했다).
- 토큰: 모듈 구문 트리의 `Name`·`Identifier`·`Member` 노드 중 심볼이 있는 것(이름 토큰), `Type` 노드와 class 기반 목록 `Path` 노드의 타입(경로 마지막 이름 토큰 — 이름 노드에 심볼이 따로 없을 때), import 경로의 이름(namespace). 원문 위치 순, 겹치면 앞의 것(같은 토큰이 둘이면 하나). 속성(`@test` 등)은 심볼이 없어 강조하지 않는다(편집기 문법 강조 몫 — LC8g).
- 종류(legend 순서): `namespace`(import 별칭·모듈·prelude 이름 공간·RPC 접두), `type`(내장·prelude 타입), `class`(Sparks·엔진 클래스), `struct`, `enum`, `interface`, `typeParameter`, `enumMember`, `event`, `function`(최상위 fn·prelude 함수), `method`(메서드·엔진 함수·interface 메서드), `property`(필드·엔진 필드·언어 필드), `parameter`, `variable`(지역·const·문맥 이름). modifier: `declaration`(선언 이름), `readonly`(`let`·const·읽기 전용 속성·문맥 이름), `static`(최상위 fn·const·schema·prelude static — `Delay`·`Log.Info`), `async`, `defaultLibrary`(엔진·prelude·언어 소유 — 엔진·prelude enum의 case 포함). 엔진 클래스의 필드(`Owner` 등)는 `property defaultLibrary`다.

### 10.8 시험(LC8e)

- 틀: 조각 크기 1·2·7·전체로 같은 각본을 나눠도 같은 출력, 머리 오류 5가지(없음·숫자 아님·상한 초과·머리 줄 상한·입력 중간 끝 — 1), 여러 머리·`Content-Type`.
- 프로토콜 golden: 각본 세션(initialize → initialized → didOpen → semanticTokens → didChange → didClose → shutdown → exit)의 출력 전체 byte. 수명 오류(초기화 전 요청 -32002, 두 번째 initialize, shutdown 뒤 요청, 모르는 요청 -32601, JSON 아님 -32700, 모양 틀림 -32600, 인자 틀림 -32602, exit만 — 1).
- 진단 동등성: conformance 폴더·파일 사례(LC8a 동등성 시험과 같은 집합)를 작업 공간으로 열어 발행 진단(경로·코드·위치 — UTF-16을 code point로 되돌려)이 `sparks check`와 같다. 열린 문서 덮기(디스크와 다른 내용·저장 전 새 파일·닫으면 디스크로)와 진단 지우기.
- 위치·URI: 한글·이모지(서로게이트 쌍)·CRLF·BOM·잘못된 UTF-8, Windows 드라이브·`%3A`·공백·한글 경로 왕복, 잘못된 percent.
- 의미 강조 golden(종류·modifier 전부 나오는 사례), 할당 실패 전수(짧은 세션 — ExitError(3)이거나 같은 출력).

### 10.9 구현(LC8e)

- **모양:** 위 설계대로다. 설계와 달라진 곳: (1) `initialized` 알림도 분석한다 — 문서를 열기 전에 디스크 작업 공간의 진단이 보인다. (2) 경로 → URI의 드라이브 `:`는 `%3A`(10.6절 — 설계는 `:` 그대로). (3) 작업 공간이 없으면 분석하지 않는다(10.3절 — 설계는 "열린 문서만"). (4) 프로토콜 층 파일(`LspProtocol`)을 두지 않았다(10.1절). (5) 의미 강조에서 `Attribute`를 빼고 class 기반 `Path`를 더했다(10.7절).
- **리뷰 반영:** 잘못된 UTF-8 본문이 3으로 끝남(틀이 본문을 `String`에 담았다) → 틀은 reader 버퍼의 byte를 그대로 넘기고 JSON 해석이 -32700. 머리 전체 상한을 줄을 기다릴 때만 봐 조각 크기에 따라 판정이 달랐음 → 줄마다. 지움을 URI 순서에 합치면 다른 철자로 다시 연 문서의 새 진단을 뒤의 지움이 덮을 수 있음 → 지움 먼저. 중첩 패키지의 저장 전 새 파일이 바깥 패키지로 감 → 가장 안쪽, `.` 폴더 제외. 상대 schema 경로의 발행 URI가 `file:///Schemas/…` → 절대 경로로. 작업 공간 없음의 설계 문구 불일치 → 설계를 고침. 읽기 실패 기록이 문서 알림마다 → 이유가 바뀔 때만. `localhost` authority 받음, `%2F`·`%5C` 거부. 발행의 열린 문서 대조를 O(문서²) → O(문서). 경로 접두 비교가 기본 비교(ASCII 대소문자 무시)였음 → 대소문자 구분(덮기의 패키지 뿌리, LC8a 폴더 인자의 모듈 고르기).
- **함께 고친 것(LC8a):** 작업 공간의 진단 경로 표기(`Display` — `\` → `/`)가 한 byte씩 `String`에 붙여 UTF-8 sequence가 쪼개지면 실패했다 — 한글이 든 폴더에서 `sparks check`가 할당 실패(3)로 끝났다. `\` 사이 구간을 통째로 붙인다. 언어 서버의 경로 key(`LspPathKey`)도 같다. 시험: `SparksTools_CheckNonAsciiFolder`, 언어 서버 시험은 모두 공백·한글이 든 임시 폴더에서 돈다.
- **시험(`SparksToolsLspProtocolTests`·`SparksToolsLspAnalysisTests`, 준비는 `SparksToolsLspTestSupport.h`):** `SparksLsp_LifecycleGolden`(초기화 전 요청 -32002·초기화 전 알림 무시·`initialize` 인자 틀림 -32602·두 번째 initialize -32600·JSON 아님·잘못된 UTF-8·글자 중간에서 끊긴 본문 -32700·배열·bool id·문자열 아닌 method -32600·클라이언트 응답·`$/` 알림 무시·모르는 요청 -32601·uri 없는 강조 -32602·shutdown 뒤 요청 -32600 — 출력 byte 전체 golden), `SparksLsp_ExitCodesAndUsage`(exit만·shutdown 없음·입력 끝·빈 입력 1, 인자 오류 2, 읽기·쓰기 오류 3), `SparksLsp_FramingChunksAndHeaders`(조각 1·2·7·전체, 머리 이름 대소문자·값 공백·`Content-Type`·1,024 byte 머리 줄, 깨진 틀 17가지 — 없음·숫자 아님·음수·상한+1·자리 넘침·두 번·`:` 없음·머리 줄 상한·줄 끝 없는 긴 머리·머리 전체 상한(끝나지 않은 줄·다 끝난 줄들)·본문 모자람·64 MiB 그대로(입력 끝)·머리 중간 끝 — 조각 1과 전체), `SparksLsp_PublishesAndOverlaysDiagnostics`(디스크 진단 → 열어 고침 → 바꿈 → 같은 진단 바꿈은 발행 없음 → 다른 모듈을 깨 닫힌 모듈 진단 → 닫아 지움 → 저장 전 새 파일 → 닫아 지움 → 닫아 디스크로, 패키지 밖·file 아님·잘못된 percent·authority 문서는 진단 없음, 디스크에서 지운 파일의 지움과 대문자 드라이브·`:` URI 문서, 진단이 있던 파일을 다른 철자로 열기 — 지움 먼저), `SparksLsp_WorkspaceRoots`(workspaceFolders 우선·file 아닌 폴더 기록·잘못된 항목, 없는 폴더 — 기록·진단 없음·빈 강조·같은 이유는 한 번만 기록, 작업 공간 없음 — 기록, URI 철자 — `localhost`·소문자 `%3a` 받음·`%2F` 거부), `SparksLsp_NestedPackagesAndHiddenFolders`(가장 안쪽 패키지, `.` 폴더), `SparksLsp_DocumentStoreEdges`(4,096개 상한, 열지 않은 문서의 바꿈·닫기, 같은 URI 다시 열기, 부분 바꿈 거부, manifest 덮기 — 깨진 manifest면 패키지 모듈 진단이 지워졌다 닫으면 돌아옴), `SparksLsp_SchemaFolders`(schema 없음, 초기화 옵션 상대 경로, 명령줄 상대 경로 — 깨진 schema 진단 URI가 절대 경로), `SparksLsp_PositionsAreUtf16`(BOM·CRLF·한글·이모지, 디스크의 잘못된 UTF-8 — 앞 글자와 범위 끝), `SparksLsp_SemanticTokensGolden`(full profile·schema — 종류 13가지·modifier 5가지, 같은 줄 한글·이모지 뒤 열, 디스크 모듈·밖 URI·file 아닌 URI), `SparksLsp_MatchesConformanceCorpus`(LC8a 동등성 시험과 같은 사례 집합 — 발행 진단 = `.expect`), `SparksLsp_AllocationFailure`(세 세션 — 열기·강조·바꿈·JSON 아님·닫기 / workspaceFolders·초기화 옵션 schema·저장 전 새 파일·지움 / 없는 작업 공간·잘못된 UTF-8 본문 — 모든 할당 실패가 3이거나 같은 출력, 누수 없음).
- **한계:** `typeParameter`는 사용자 원문에 나올 자리가 없다(Sparks에 사용자 generic이 없다 — 언어·엔진 타입 인자만) — legend에는 둔다(LC8f hover가 쓴다). 매 분석이 디스크 전체를 다시 읽고 컴파일한다(10.4절 — 증분은 측정 뒤). lexer가 첫 잘못된 UTF-8 byte에서 멈추므로 그 뒤 열은 진단으로 볼 수 없다(변환 함수는 1로 센다). 열린 문서가 4,096개를 넘으면 무시한다. 짝 없는 서로게이트(`"\ud800"`)가 든 본문은 Core JSON이 거부해 -32700이다(그 문서는 열리지 않는다). 덮기는 분석마다 열린 문서 × 작업 공간 파일만큼 경로 key를 만든다(측정 뒤 색인). POSIX 경로·드라이브 뿌리(`file:///c:`)의 URI 왕복은 Windows에서 돌리는 시험에 없다(macOS·Linux CI가 같은 시험을 POSIX 임시 폴더로 돈다). 할당 실패 때 표준 오류 한 줄이 빠질 수 있다(출력은 같다). 수동 확인(2026-11-24 Windows): `SparksLsp --stdio`에 각본 입력(initialize·열기·강조·닫기·shutdown·exit — 0).
