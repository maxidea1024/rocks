# Sparks 도구(LC8)

이 문서는 Sparks 언어 코어의 LC8(도구)을 정한다: CLI `sparks`(`check`·`dump`·`build`·`ids`·`fmt`·`test`), 공식 formatter, `@test` 실행기, 언어 서버 `sparks-lsp`(진단·의미 강조 → 자동완성·hover·정의 이동·빠른 수정·서식). 언어 의미·진단 형식은 SparksLanguage(SL) §8.1·§13이 정본이고, 이 문서는 그것을 어떤 모듈·프로그램·형식으로 실현하는지 정한다. 선행은 표준 입출력 API(SparksEngineIntegration SE-B7)뿐이며 LC8a가 함께 만든다.

- 개정 4(2026-11-23): LC8b 구현·리뷰 반영(7.4절 — 컴파일러의 승계 판정 순서·은퇴/이미 이은 승계 진단, manifest의 모르는 키 S1006, 도구의 이름 겹침 검사·확인 실패 진단 출력, 결과 줄 정렬, 잠금 공유 모드 한계).
- 개정 3(2026-11-23): LC8b 설계(7절 — build 쓰기 규칙, ids의 manifest 편집·검증·잠금, C# 직렬화와 byte 동일). T7·T8 개정.
- 개정 2(2026-11-23): LC8a 구현(6절).
- 개정 1(2026-11-23): 첫 판. 하위 단계 LC8a–LC8g, 결정 T1–T14.

## 1. 하위 단계

| 단계 | 내용 | 완료 기준 |
|---|---|---|
| LC8a 표준 입출력·작업 공간·check | PlatformBase 표준 스트림 API(SE-B7), `SparksTools`(Developer) 작업 공간 적재(패키지 폴더 찾기·schema 폴더·원문 읽기)와 명령 실행 라이브러리, 프로그램 `sparks`의 `check`·`dump` | 명령 시험(종료 코드·출력 golden), 작업 공간 적재의 할당 실패 전수, corpus 사례를 CLI로 돌린 진단이 conformance와 같음 — 완료(개정 2, 6절) |
| LC8b build·ids | `build`(모듈마다 `.sparkc` — 출력 폴더), `ids`(발급 대기 안정 ID를 `.rocksids.json`에 GUID로 발급) | 출력 byte golden, manifest 왕복 시험 — 완료(개정 4, 7절) |
| LC8c formatter·fmt | lexer의 주석 보존 모드, 공식 서식 규칙(3절)의 token 기반 formatter(SparksCompiler), `fmt`·`fmt --check` | corpus 전체에서 멱등(fmt(fmt(x)) = fmt(x))·token 열 보존, 서식 golden |
| LC8d @test·test | `@test fn` 실행기(headless 가짜 host — 엔진 함수는 "엔진 필요" 실패), 결과를 진단 형식으로 | 실행기 시험(통과·Assert 실패·fault·엔진 필요) |
| LC8e 언어 서버 기본 | `sparks-lsp`: JSON-RPC(stdio, Content-Length), 문서 저장소(열린 문서 덮어쓰기·버전), 진단 발행, 의미 강조(semanticTokens/full) | CLI와 언어 서버의 진단 동등성, 프로토콜 시험(메시지 golden) |
| LC8f 언어 서버 편집 | 자동완성(멤버·hook·attribute·리터럴·경로·Unity 이름 제시), hover, 정의 이동, 빠른 수정(SL §13 목록), 문서 서식 | 자동완성 후보 golden, prefix fuzz, 빠른 수정 golden |
| LC8g VS Code 확장 | `Tools/VSCode/sparks`(TypeScript — 언어 등록·문법 강조·`sparks-lsp` 시작) | 확장 패키징 시험(C# 도구 시험에서 manifest 검사) |

## 2. 구조

- **모듈(T1):** formatter는 `SparksCompiler`(SL 18.1 — 컴파일러 라이브러리가 같은 규칙을 쓴다). 작업 공간 적재·명령 실행·`@test` 실행기·언어 서버 핵심은 `Engine/Source/Developer/SparksTools`(Developer — 출시 실행 파일에 링크하지 않는다, 시험은 EngineTests에서 같은 프로세스로 부른다). 프로그램 `Programs/SparksCli`(실행 파일 `SparksCli` — 설치 이름 `sparks`는 배포 도구의 몫, 사용법 문장은 `sparks`)와 `Programs/SparksLsp`(`SparksLsp` — 설치 이름 `sparks-lsp`)는 Launch 진입(CrashHandlingAndLaunch)과 명령 함수 호출만 한다(Commandlet과 같은 배치).
- **표준 입출력(T2 — SE-B7):** `Platform/StandardStream.h` — `WriteStandardOutput(StringView utf8)`·`WriteStandardError(StringView utf8)`(콘솔이면 Windows는 UTF-16 `WriteConsoleW`, 리디렉션·pipe는 byte 그대로 — Commandlet 구현과 같은 규칙), `ReadStandardInput(Span<char> buffer)` → 읽은 byte 수(0은 끝, pipe·파일은 byte 그대로 — 언어 서버의 Content-Length 틀), 실패는 `DependencyFailed`(handle 없음·쓰기 실패)·`InvalidArgument`(UTF-8 아님 — 출력만). Windows는 `GetStdHandle`+`ReadFile`/`WriteFile`/`WriteConsoleW`, macOS·Linux는 `read`/`write`(EINTR 재시도). CRT stdio를 쓰지 않는다(text 모드 변환이 Content-Length를 깨지 않게). Commandlet의 전용 구현은 이 API로 옮길 수 있다(그 세션의 일 — 알림).
- **작업 공간(T3):** 명령 인자의 경로(폴더)마다 아래를 훑어 `SparksPackage.json5`가 있는 폴더를 패키지 뿌리로, 그 아래 `.spark`를 모듈로 넣는다(conformance 폴더 사례와 같은 규칙 — 모듈 경로는 패키지 뿌리 기준). schema는 `--schema <폴더>`(여러 번 — `*.sparkbind.json`), ID manifest는 패키지 뿌리의 `.rocksids.json`. 설정 파일(`SparksWorkspace.json5`)은 엔진 연동(EI — 엔진이 schema 위치를 안다)에서 더한다. 파일 읽기는 PlatformBase `FileSystem`.
- **출력(T4):** 진단은 SL §8.1 텍스트(`AppendDiagnostics`)를 표준 출력에, 요약 한 줄(`N errors in M files`)을 끝에. 명령 사용법 오류는 표준 오류. 종료 코드: 0 성공(진단 없음·시험 통과·서식 같음), 1 진단·시험 실패·`fmt --check`의 다름, 2 사용법 오류, 3 입출력·할당 실패. 출력은 결정적이다(경로 byte 순, 진단은 §8.1 순서).
- **명령 인자(T5):** 명령 이름 뒤 `--이름 값`·`--플래그`·위치 인자만(AppInfoCommandLine의 생성 바인더는 설계만이라 `SparksTools`의 작은 자체 분해기 — 그 모듈이 생기면 옮긴다). 알 수 없는 옵션은 사용법 오류(2)다.
- **dump(T6):** `sparks dump --ast|--names|--types|--ir|--bytecode <파일 또는 폴더>` — conformance golden과 같은 덤프 함수(`AppendSyntaxDump`·`AppendNameDump`·`AppendTypeDump`·`AppendIrDump`·`AppendBytecodeDump`)를 쓴다. 폴더면 모듈마다 `== <경로>` 줄을 앞에(경로 byte 순).
- **build(T7):** `sparks build <경로> --out <폴더>` — 진단 없으면 모듈마다 `<출력>/<패키지 이름>/<모듈 경로의 .spark를 .sparkc로>`(SparksBytecode `.sparkc` 형식). 진단이 있으면 아무것도 쓰지 않는다(1). 파일마다 원자적으로 쓰고 내용이 같으면 쓰지 않는다(7절).
- **ids(T8):** `sparks ids <경로>` — 컴파일의 발급 대기 목록(`PendingIds`)마다 새 GUID(Core `TryNewGuid` — OS 보안 난수 UUID v4, C# `Guid.NewGuid`와 같은 종류)를 그 패키지 ID manifest에 더하고, 옛 이름이 있는 대기는 C# `StableIdManifest.Rename`과 같은 규칙으로 옮긴다(옛 이름은 aliases, type rename은 멤버 항목도 함께). 출력 byte는 C# `StableIdManifest`의 직렬화와 같다(항목은 kind·name ordinal 순, aliases ordinal 순 — C# 도구가 다시 써도 바뀌지 않는다. 기존 항목의 원문 순서는 지키지 않는다). 쓰기 전에 새 manifest로 다시 컴파일해 진단·대기가 0인지 보고, C#과 같은 잠금 파일·임시 파일 교체로 쓴다(7절).
- **formatter(T9 — 3절):** token 기반이다. lexer의 주석 보존 모드가 주석을 trivia 목록(종류·범위)으로 따로 돌려주고, formatter는 token 열과 trivia를 원래 순서로 걸으며 공백·줄바꿈·들여쓰기만 정한다(token 글자는 바꾸지 않는다 — 의미 보존을 token 열 비교로 시험). 구문 오류(S02xx)가 있는 파일은 서식하지 않는다(그대로 — 진단). 설정이 없다(SL §3.1 R21).
- **@test(T10):** `@test fn`(최상위 — 인자 없음)과 클래스의 `@test fn`(그 클래스 instance를 headless 가짜 객체에 붙여 `$init` 뒤 호출)을 VM session으로 돌린다. 시계는 `Advance`로 결정적으로 진행(정지한 시험은 `--max-frames`(기본 600)까지 프레임을 진행하고 남으면 "did not finish"). 결과: 통과, `Assert` 실패·fault(진단 형식 — 원문 위치), 엔진 함수 호출(가짜 host가 모르는 native — "needs the engine (EI)" — 실패로 센다). headless World fixture(SL §13.1)는 엔진 연동(EI)이 host를 바꿔 끼운다.
- **언어 서버(T11):** LSP 3.17, JSON-RPC 2.0, stdio Content-Length 틀(Core JSON reader·writer). 문서 동기화는 전체 내용(`TextDocumentSyncKind.Full`) — 증분은 측정 뒤. 분석은 작업 공간 원문에 열린 문서 내용을 덮은 snapshot으로 전체를 컴파일한다(SL §13 "문서 버전별 arena" — snapshot마다 새 allocator 영역). 진단은 같은 `Compilation`이라 CLI와 같다(동등성 시험). 위치는 LSP 규약대로 UTF-16 code unit 열(컴파일러 위치는 byte·code point — 변환 함수와 시험). 처리 중 들어온 같은 문서의 새 변경은 이전 분석 결과를 버린다(취소 — 순서는 메시지 순서로 결정적).
- **의미 강조(T12):** semanticTokens/full — 이름 해석 결과로 type·class·struct·enum·enumMember·event·function·method·property(필드)·parameter·variable·namespace(모듈) + modifier(declaration·readonly(`let`)·static(최상위)·async·defaultLibrary(엔진 schema)).
- **편집 기능(T13 — LC8f):** 자동완성 후보는 위치의 문법 자리(멤버 접근·`on`·attribute·리터럴 인자·import 경로)와 이름 해석 범위에서 만든다. 리터럴(태그·에셋 경로)은 컴파일 검증과 같은 사전(엔진 연동 전에는 schema·manifest만). Unity 이름(`Update` 등)은 Rocks 이름 후보에 별칭으로 붙인다. core 패키지는 full 후보를 숨긴다. hover는 타입·문서 주석·선언 위치(엔진 native는 schema 위치), 빠른 수정은 진단 코드별 편집 생성기(SL §13 목록 — 해당 진단이 있는 것부터).
- **VS Code(T14):** 확장은 언어 id `sparks`, `.spark`, TextMate 문법(키워드·주석·문자열 — 의미 강조가 덮는다), 서버 경로 설정(`sparks.server.path`, 기본은 Rocks 출력 폴더). 배포·마켓 등록은 하지 않는다(저장소 안 개발용).

## 3. 공식 서식(LC8c)

- 들여쓰기 4 공백, 탭 없음. 줄 끝 공백 없음, 파일 끝 LF 하나. 줄 끝은 LF(CRLF는 LF로).
- 중괄호는 여는 줄 끝(`class X : Base {`, `fn F() {`, `if (c) {`), 닫는 중괄호는 자기 줄. `} else {` 한 줄. 빈 블록은 `{ }`가 아니라 `{\n}`(SL 예제와 같게).
- 문장마다 한 줄. 선언·멤버 사이 빈 줄은 원문을 따르되 연속 빈 줄은 하나로, 블록 처음·끝의 빈 줄은 없앤다. 최상위 선언 사이는 빈 줄 하나(없으면 넣는다).
- 이항 연산자·`=`·`:`(타입 표기 앞은 붙이고 뒤는 한 칸)·`->`·`??` 앞뒤 한 칸, `,` 뒤 한 칸, 괄호·대괄호 안쪽 공백 없음, 단항 연산자 붙임, `?.`·`.`·`?` 붙임, 키워드(`if`·`for`·`while`·`return` …) 뒤 한 칸.
- 주석: `//` 줄 주석은 앞 token과 한 칸(같은 줄) 또는 자기 줄(들여쓰기 맞춤), `/* */`는 위치 그대로 앞뒤 한 칸. 문서 주석 `///`는 선언 바로 위.
- 줄 길이 제한·자동 줄바꿈은 하지 않는다(원문 줄바꿈을 존중 — 식 안의 줄바꿈은 들여쓰기만 다시 맞춘다: 이어지는 줄은 문장 들여쓰기 + 4). 자동 줄바꿈은 측정·사용자 의견 뒤(결정 T9 대안).

## 4. 결정

- T1 대안(CLI·서버를 SparksCompiler에)은 Runtime 모듈에 도구 의존(파일 시스템 훑기·JSON-RPC)을 넣는다(엔진 층 핵심만 원칙). formatter만 컴파일러에 둔다(SL 18.1 — 편집기·Rocks Build·서버가 같은 규칙).
- T2 대안(CRT `fwrite`·`fread`)은 Windows text 모드가 CRLF·Ctrl-Z를 바꿔 LSP 틀을 깬다. 대안(프로그램마다 전용 구현 — Commandlet 방식)은 SE-B7이 막으려는 중복이다.
- T4 종료 코드는 컴파일러 도구 관례(0·1·2) + 입출력 실패 3.
- T7 대안(모듈을 바로 덮어쓰기)은 쓰는 도중 실패하면 반쪽 `.sparkc`가 남는다(읽는 쪽이 형식 오류로 거부하지만 이전의 맞는 파일을 잃는다). 대안(바뀌지 않은 파일도 다시 쓰기)은 수정 시각이 바뀌어 뒤 단계(쿠킹·패키징)가 할 일을 늘린다. 지운 모듈의 옛 `.sparkc`는 지우지 않는다(출력 폴더 정리는 호출자의 몫 — 다른 도구의 파일을 지우지 않게).
- T8 대안(기존 항목 순서 유지)은 C# 도구가 같은 manifest를 다시 쓸 때마다 순서가 바뀌어 diff가 생긴다 — 두 도구가 같은 정규형을 쓴다. 대안(발급 뒤 쓰고 나서 다시 컴파일해 확인)은 확인이 실패해도 이미 쓴 manifest가 남는다 — 쓰기 전에 메모리에서 확인한다.
- T9 대안(구문 트리 재출력)은 괄호·주석·빈 줄 보존에 노드마다 규칙이 필요하고 의미 보존 시험이 어렵다. token 기반은 token 열 동일성으로 의미 보존을 기계적으로 보인다. 줄 길이 자동 줄바꿈은 구문 트리가 필요해 뒤로 미룬다(Go `gofmt`처럼 줄바꿈을 사람이 정한다).
- T10 대안(엔진 없이 `@test` 금지)은 언어 코어의 시험 작성(SL §13.1)을 막는다. 가짜 host는 conformance의 FakeHost와 같은 schema 구동 방식을 쓰되 공개 API로 `SparksTools`에 둔다(FakeHost는 corpus 전용 — 기록 형식이 다르다).
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
