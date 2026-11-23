# 공통 실행 진입점과 크래시 수집 사용 안내

기준일: 2026-11-19. [크래시 처리·실행 진입점 설계](../Design/CrashHandlingAndLaunch.ko.md)의 LC1–LC7 구현을 쓰는 법이다. 설계 근거·결정·미확인 사실은 그 문서에 있고, 여기서는 실행 파일을 만들고 크래시 산출물을 읽는 데 필요한 것만 적는다. PlatformBase가 맡는 OS 경계는 [플랫폼 계층](../Design/PlatformLayer.ko.md) "크래시 수집과 프로세스 진입" 절에 있다.

## 1. 새 실행 파일

모든 Rocks native 실행 파일은 `Launch` 모듈의 진입점 매크로를 쓴다. `main`·`wmain`·`WinMain`·`wWinMain`을 직접 정의하면 policy `process-entry-point`가 실패한다.

```cpp
#include <Launch/EntryPoint.h>

namespace {

int Run(rocks::launch::ApplicationContext& context) noexcept {
    for (const rocks::StringView argument : context.Arguments()) {
        // 인자는 UTF-8이고 각 Data()는 NUL로 끝난다.
    }

    return 0; // 프로세스 종료 코드
}

const rocks::launch::ApplicationDescription& Describe() noexcept {
    static constexpr rocks::launch::ApplicationDescription description{
        .name = "MyTool", // 크래시 폴더·메타데이터 program. ASCII 글자·숫자·'.'·'_'·'-'
        .kind = rocks::launch::ApplicationKind::Tool,
        .settings = {.toolLogging = rocks::launch::ToolLogging::Stderr},
        .run = &Run};
    return description;
}

} // namespace

ROCKS_IMPLEMENT_APPLICATION(Describe)
```

- 빌드 규칙(`*.Build.cs`)의 `Dependencies`에 `Launch`를 넣는다. 게임 템플릿(`Templates/Game/Foundation`)이 예다.
- 실행 파일도 C++ 모듈이므로 모듈 진입점 `ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, MyTool)`을 담은 `*Module.cpp`가 따로 있어야 한다([모듈 로더](../Design/ModuleLoader.ko.md) 3.2절, 빠지면 모듈 표 링크 오류). 프로세스 진입점(`ROCKS_IMPLEMENT_APPLICATION`)과는 다른 것이고 둘 다 필요하다. 같은 소스를 여러 실행 파일이 컴파일하면 이름 자리에 `ROCKS_MODULE_NAME`을 쓴다.
- Windows GUI 프로그램은 `ROCKS_IMPLEMENT_WINDOWED_APPLICATION`과 빌드 규칙 `WindowsSubsystem = Windows`를 함께 쓴다. 둘이 어긋나면 진입 기호가 없어 링크가 실패한다.
- Windows 진입점은 `wmain`·`wWinMain`뿐이다. 인자는 CRT가 나눈 넓은 문자 argv를 UTF-8로 바꾼 값이다. POSIX는 `main`이고 인자가 UTF-8이 아니면 시작하지 않는다.
- 명령줄을 캡처하지 못하면(잘못된 인코딩, 인자 32768개·2 MiB 초과) stderr에 한 줄을 남기고 `commandLineFailureExitCode`(기본 1)로 끝난다.
- Rocks를 자기 진입점에 내장하는 외부 호스트는 `rocks::launch::RunEmbeddedApplication(Describe(), argc, argv)`를 부른다. 호스트의 예외 filter·신호 처리기·콘솔 처리기를 바꾸지 않는다.

`ApplicationKind`는 기본 정책을 정한다.

| 종류 | 쓰는 곳 | 차이 |
|---|---|---|
| `Tool` | 명령줄 도구·벤치 | 기본값 그대로 |
| `EngineHost` | Sandbox·게임 | 본문이 Engine·LogHost를 쓴다(도구 로그는 보통 None) |
| `TestRunner` | 시험 실행기와 자식 probe | OS·CRT 오류 대화상자를 늘 끈다 |

## 2. 설정

`ApplicationDescription::settings`가 기본값이다. 명령줄을 보고 바꿔야 하면 `configure`를 둔다. configure는 명령줄 캡처 뒤, 본문 전에 한 번 불린다.

| 칸 | 기본 | 뜻 |
|---|---|---|
| `crashReporting` | `Full` | `Full`: OS 처리기 + 덤프 도우미 프로세스(실패하면 프로세스 안 덤프). `InProcess`: 프로세스 안 덤프. `Off`: 설치하지 않음 |
| `dumpDetail` | `Standard` | `Detailed`·`Full`은 기본 덤프 뒤 `Minidump.detail.dmp`를 더 쓴다(도우미 마감 `helperDumpDeadlineMilliseconds`) |
| `crashDirectory` | `Saved/Crashes` | 크래시 폴더의 뿌리. 설치 때 작업 폴더 기준 절대 경로로 굳힌다 |
| `crash*Milliseconds` | 0(기본값) | 종료 콜백 전체 예산(기본 2초), 도우미 캡처 마감, 프로세스 안 덤프 마감, 겹친 실패 스레드 여유. 범위 밖이면 경고하고 이전 설정을 유지한다 |
| `reportWhenDebuggerAttached` | false | 디버거가 붙어 있어도 보고한다. 환경 변수 `ROCKS_CRASH_REPORT_WITH_DEBUGGER=1`로도 켠다(Windows) |
| `captureHeapCorruption` | false | Windows 힙 손상(`STATUS_HEAP_CORRUPTION`)을 VEH로 받아 보고한다 |
| `suppressSystemErrorDialogs` | false | OS·CRT 오류 대화상자를 끈다(`TestRunner`는 늘 켠다) |
| `handleTerminationRequests` | false | 첫 Ctrl-C·Ctrl-Break(POSIX SIGINT·SIGTERM)를 원자 표시로 받는다. 두 번째는 OS 기본 동작(즉시 종료)이다 |
| `toolLogging` | `None` | `Stderr`면 Launch가 Sync LogSystem과 stderr sink를 설치한다(`context.ToolLog()`) |
| `defaultAllocator` | `Binned` | 범용 할당자(`context.DefaultAllocator()`)의 종류. `Binned`: Launch의 `SystemAllocator` 위에 둔 `BinnedAllocator`. `System`: `SystemAllocator` 그대로(두 할당자에서 같은 프로그램을 비교할 때) |

configure가 크래시 설정을 바꾸면 Launch가 먼저 `platform::ValidateCrashReportingOptions`로 검사한다. 잘못되면 이전 설정을 유지하고 경고한다. 다시 걸기가 실패하면 이전 설정과 이전 폴더로 되돌린다.

## 3. 엔진 호스트

- **할당자.** 엔진·모듈 로더·설정 문맥·LogHost에는 `context.DefaultAllocator()`(프로세스의 범용 할당자 — 기본 `BinnedAllocator`)를 넘긴다. 본문은 돌아가기 전에 거기서 받은 할당을 모두 반환하고 그 할당자를 쓰는 스레드를 끝낸다. 할당을 남기면 Launch의 정리가 Fatal(`BinnedAllocator destroyed with live allocations`)로 끝난다 — 아래 "정적 소멸을 건너뛰는 종료"를 요청한 경우는 검사하지 않는다. `context.SystemAllocator()`는 범용 할당자의 상위이고 Launch의 도구 로그가 쓴다(비교·진단용). 큰 작업을 끝낸 뒤 쥐고만 있는 메모리를 OS에 돌려주려면 `context.DefaultAllocator().TrimMemory()`를 부른다 — 엔진은 전체 수집의 회수가 끝났을 때와 World를 파괴한 뒤에 스스로 부른다(`WorldGcSchedule::trimMemory`).
- **종료 요청.** `handleTerminationRequests`를 켜면 처리기는 `launch::RequestTermination()` 표시만 남긴다. 게임 스레드가 프레임 첫 단계(`EngineLoopHooks::PumpEvents`)에서 `launch::IsTerminationRequested()`를 보고 `Engine::RequestExit()`를 부른다. Sandbox `TerminationRequestBridge`가 예다. `RequestExit`는 `Initialize`를 부른 게임 스레드에서만 부른다(Debug·Development에서 검사).
- **단계 표시.** Engine은 `Engine.Initialize`·`Engine.Ready`·`Engine.Run`·`Engine.Shutdown`·`Engine.Stopped`를 스스로 남긴다(`Engine/Engine.h`의 `EngineStage*`). 본문이 따로 남기려면 `context.SetStage(platform::RunPhase::Running, "MyTool.Import")`처럼 정적 문자열을 넘긴다. 서로 다른 이름은 프로세스에 64개(`platform::MaxRunStageNames`)까지 기록되고, 넘으면 이름 없이 phase만 남는다.
- **정적 소멸을 건너뛰는 종료.** 멈춘 로그 출력 스레드를 포기한 경우처럼 정적 소멸이 위험하면 `context.RequestExitWithoutStaticTeardown()`을 부르고 본문에서 돌아온다. Launch가 정리(도구 로그, 종료 요청 처리기, 크래시 수집 해제)를 마친 뒤 `platform::ExitProcessWithoutTeardown`으로 끝낸다(Windows `TerminateProcess` — `_Exit`는 Windows에서 공유 라이브러리의 정적 소멸을 돌린다).
- **unhandled filter 교체 감지.** Engine은 초기화 끝과 종료 시작에 `platform::CheckUnhandledCrashFilter()`로 처리되지 않은 예외 filter가 바뀌었는지 보고, 바뀌었으면 Warning을 남긴다(남의 filter는 빼앗지 않는다).

## 4. 크래시 산출물

크래시 폴더는 `<뿌리>/<program>-<프로세스 시작 UTC YYYYMMDDTHHMMSSZ>-<pid>/`이다. 정상 실행에서는 폴더가 생기지 않는다. 폴더를 만들 수 없으면 임시 폴더 아래 `Rocks/Crashes`에 한 번 더 시도한다. stderr에 `Rocks crash report: <폴더>` 줄이 남는다.

| 파일 | 내용 |
|---|---|
| `Minidump.dmp` | Windows만. 도우미가 snapshot에서 쓰거나(기본) 프로세스 안에서 쓴다. 사용자 stream `RocksCrashRecord/1`에 예비 메타데이터가 들어 있다 |
| `Minidump.detail.dmp` | `dumpDetail`이 `Detailed`·`Full`일 때 |
| `CrashInfo.json` | 형식 `rocks.crash/1`. 예비본은 `"complete": false`이고 최종본으로 덮는다 |

`CrashInfo.json`의 주요 칸:
- 정체: `crashId`·`program`·`executable`·`processId`·`processStartUtc`·`crashUtc`, `build`(엔진 버전·도구 빌드 ID·구성·플랫폼·아키텍처·sanitizer), `module`(PDB GUID·age·이름 — 디버거가 찾는 PDB와 같아야 한다).
- 원인: `kind`(Exception·Signal·Fatal·Assert·Abort·RuntimeCheck·Sanitizer), `exitCode`, `exceptionAddress`, `faultAccess`(읽기·쓰기와 주소), `fatal`(메시지·file·line·호출 위치), `phase`·`stage`(실행 단계).
- 스레드: `failingThreadId`·`failingThreadName`·`reporterThreadId`·`concurrentCrashes`(겹쳐 실패한 스레드).
- 처리 결과: `dump`(status·writer·오류), `helper`(도우미 상태·job 여부, 도우미가 없으면 null), `terminationCallbacks`(콜백마다 결과와 걸린 시간), `limitations`(`in-process`·`sanitizer build`·`embedded host` 등), `incompleteReasons`.
- 연관 파일: `associatedFiles`(로그 폴더·로그 파일·trace 파일, 아래 5절).

덤프를 여는 법(Windows, 심볼 보관은 설계 14.4절):

```
cdb -z <크래시 폴더>\Minidump.dmp -y "srv*<심볼 저장소>;<bin>" -i "<심볼 저장소>;<bin>" -c ".ecxr; kn 64; ~*kn 16; lm v m <program>; q"
```

- `lm v`의 PDB GUID·age가 `CrashInfo.json`의 `module`과 같은지 본다.
- Fatal·assert는 `.ecxr`이 `RaiseException` 지점을 보인다. 호출자는 `fatal.returnAddress`로 찾는다.

POSIX(macOS·Android)는 덤프가 없다. `CrashInfo.json`만 쓰고 이전 신호 동작으로 넘겨 OS 수집기(debuggerd tombstone·ReportCrash)가 받게 한다.

종료 코드: Windows 하드웨어 예외는 그 예외 코드(`0xC0000005` 등), Fatal·assert·abort·CRT 치명은 Rocks 코드 `0xE0524B46`다. POSIX는 원래 신호로 끝난다(셸 종료 코드 128 + 신호, Fatal 계열은 SIGABRT 134).

## 5. 다른 모듈이 꽂는 것

- **종료 콜백.** `platform::RegisterTerminationCallback(callback, user, handle)`. 크래시 처리 스레드가 프로세스당 한 번, 등록 역순으로 부른다. 막히는 lock·할당·레지스트리 순회 없이 `context.deadlineNanoseconds` 전에 돌아와야 한다. 칸은 4개이고 로깅이 하나를 쓴다(첫 `LogInstallScope` 때 등록, 해제하지 않음). 결과는 `CrashInfo.json`에 남는다.
- **연관 파일 칸.** `platform::SetCrashAnnotation(CrashAnnotation::TraceFile, path, bytes)`처럼 파일을 열거나 바꿀 때 경로를 알린다(칸마다 쓰는 스레드는 하나, 512 B까지). Sandbox는 `--trace-file`을 연 뒤 `TraceFile`을 쓴다. `LogFile`·`LogDirectory`는 로깅이 크래시 수집 병합 뒤 붙인다.
- **스레드.** `platform::Thread`는 시작 때 크래시 처리 준비(Windows stack 보장·SEH 보호 프레임, POSIX 대체 stack)를 스스로 한다. OS API로 직접 만든 스레드는 `PrepareThreadForCrashHandling`·`ReleaseThreadCrashHandling`을 부른다.

## 6. 디버거와 ASan

- 디버거가 붙어 있으면 기본으로 보고하지 않고 디버거에 넘긴다(접근 위반은 두 번째 기회로 실패 지점에서 멈추고, Fatal은 중단점으로 멈춘다). 디버거 아래에서도 덤프가 필요하면 `ROCKS_CRASH_REPORT_WITH_DEBUGGER=1`이다.
- Asan 구성은 덤프·도우미를 쓰지 않는다. 하드웨어 fault는 ASan이 먼저 보고하고, 그 뒤 death callback이 공통 Fatal 경로로 종료 콜백·`CrashInfo.json`(limitations `sanitizer build`)을 남긴다. 종료 코드는 Windows `0xE0524B46`, POSIX SIGABRT다. `ASAN_OPTIONS=handle_abort=1`은 지원하지 않는다. 한계는 설계 13.2절에 있다.

## 7. 시험

- C++: `FoundationTests.exe --filter CrashReporting_`·`--filter Launch_`·`--filter ProcessCommandLine`, `EngineTests.exe --filter Engine_`.
- 자식 프로세스 시험(C#): `Rocks.ToolTests native --native-executable <FoundationTests> ... --filter Native.Crash,Native.Launch,Native.Logger.Crash`. 크래시 probe(`--negative-control crash-<mode> <폴더>`)와 Launch 시험 실행 파일(`LaunchProbe`·`LaunchWindowedProbe`·`LaunchEmbeddedHost`)을 띄워 종료 코드·stderr·`CrashInfo.json`·덤프(cdb)를 단언한다. cdb(Windows Kits 10 Debuggers)가 필요하다(`ROCKS_CDB`로 경로 지정).
- Android는 C# 시험을 돌리지 않는다. 같은 probe를 adb로 직접 돌려 대조한다(설계 25절 LC7).
- 실행하지 않은 환경(macOS·POSIX Asan·Android 실기기·Windows arm64)은 설계 24.2절에 있다.
