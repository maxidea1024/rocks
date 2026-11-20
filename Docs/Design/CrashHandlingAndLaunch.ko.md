# 공통 실행 진입점과 네이티브 크래시 수집 설계

기준일: 2026-11-19, 개정 4(LC1–LC7 구현 반영, LC8 문서 마무리). 결정은 22절, 단계는 23절, 미확인 사실은 24절, 바뀐 점과 단계별로 설계와 다르게 정한 점은 25절에 있다. 사용법은 [공통 실행 진입점과 크래시 수집 사용 안내](../Implementation/CrashHandlingAndLaunch.ko.md)에 있다.

이 문서는 두 가지를 정한다.
- **공통 실행 진입점(Launch).** 실행 파일마다 흩어진 `main`·`wmain`의 시작·종료 처리를 한 계층으로 모은다. 플랫폼 진입점 연결, 이른 크래시 처리 설치, 인자 전달, 공통 서비스 시작·종료, 프로그램 본문 호출, 종료 코드 전달, 크래시 처리 해제 시점이 대상이다. 엔진 수명(`Engine::Initialize`·`Run`·`Shutdown`)은 다시 만들지 않는다.
- **네이티브 크래시 수집.** C++ 예외를 쓰지 않는 정책(`/EHs-c-`, `-fno-exceptions`)을 유지한다. 그 위에서 접근 위반·stack overflow·명시적 Fatal·assert·abort·CRT 치명 경로를 한 보고 경로로 모은다. Windows에서는 실제로 열어 분석할 수 있는 minidump와 메타데이터를 남긴다.

요구 원문은 사용자 요구(2026-11-18, "공통 실행 진입점과 네이티브 크래시 수집") 1–16절이다. 이 문서의 절 번호와 요구의 절 번호는 다르다. 요구 절을 가리킬 때는 "요구 N절"로 쓴다.

**관계 문서.**
- [로깅 시스템](LoggingSystem.ko.md) 11절·14.1.8절(LG18): 종료 hook과 최소 처리기. 처리기 소유권은 이 문서로 옮긴다(19절). 로깅 문서 개정 13(`feature/logging`, 이 구현 병합 대기)이 이관을 반영한다(19.6절).
- [앱 정보와 명령줄](AppInfoCommandLine.ko.md) 개정 3: `ProcessCommandLine`·대화상자 억제 API(AP1의 일부)를 이번에 구현했다. 나머지 경계는 21절에 적었다.
- [엔진 구현 계획](EngineImplementationPlan.ko.md) 29.4절(크래시 수집은 사전 확보 저장소·제한 시간·불완전 표시), 종료 코드 계약(2479행).
- [플랫폼 계층](PlatformLayer.ko.md): OS 경계와 자식 프로세스.
- [동기화](Synchronization.ko.md): 설계만 있다. 크래시 경로는 그 잠금을 쓰지 않는다(OS 이벤트·원자 변수만 쓴다).
- [모듈 로더](ModuleLoader.ko.md) 개정 2(ML0 병합 `77cb1817`): 모든 C++ 모듈·실행 파일의 모듈 진입점. Launch 통합은 그 문서의 ML1이다(21절).
- [리플렉션 생성 소스](ReflectionGeneratedSources.ko.md)(GS1 병합 `493f9a0d`): 모듈 생성 헤더는 등록 함수 선언만 담는다. 이 구현의 새 소스는 모듈 생성 헤더를 include하지 않는다.

UE 근거는 UE 5.8.4(로컬 클론 `C:\github\UnrealEngine`, `Engine/Source/` 기준)다. 레거시·deprecated 경로는 보지 않았다. 아래 약자를 쓴다.

| 약자 | 파일 |
|---|---|
| `LW` | `Runtime/Launch/Private/Windows/LaunchWindows.cpp` |
| `L` | `Runtime/Launch/Private/Launch.cpp` |
| `LEL` | `Runtime/Launch/Private/LaunchEngineLoop.cpp` |
| `WCC` | `Runtime/Core/Private/Windows/WindowsPlatformCrashContext.cpp` |
| `WPM` | `Runtime/Core/Private/Windows/WindowsPlatformMisc.cpp` |
| `WRT` | `Runtime/Core/Private/Windows/WindowsRunnableThread.cpp` |
| `MRT` | `Runtime/Core/Private/Microsoft/MicrosoftRunnableThread.h` |
| `WSW` | `Runtime/Core/Private/Windows/WindowsPlatformStackWalk.cpp` |
| `WEO` | `Runtime/Core/Private/Windows/WindowsErrorOutputDevice.cpp` |
| `GCC`·`GCH` | `Runtime/Core/Private/GenericPlatform/GenericPlatformCrashContext.cpp`·`Runtime/Core/Public/GenericPlatform/GenericPlatformCrashContext.h` |
| `UCC` | `Runtime/Core/Private/Unix/UnixPlatformCrashContext.cpp` |
| `CRC` | `Programs/CrashReportClient/Private/CrashReportClientApp.cpp` |

UE 이름은 5절 대응표와 근거 문장에만 쓴다.

## 1. 요약

- **진입점은 매크로 한 줄이다.** 실행 파일은 `ROCKS_IMPLEMENT_APPLICATION(Describe)`와 본문 함수만 쓴다.
  - 매크로가 플랫폼 진입점(Windows `wmain`·`wWinMain`, POSIX `main`)을 만들고 `launch::RunApplication`을 부른다.
  - 새 Core 런타임 모듈 `Launch`(의존: Core·PlatformBase)가 맡는다. Engine·World·Renderer·Window에는 의존하지 않는다.
- **크래시 수집기는 PlatformBase가 하나만 소유한다.** OS 처리기 설치·제거, 크래시 처리 전용 스레드, stack 보장, 신호 처리기, CRT 치명 hook을 모두 PlatformBase의 크래시 수집기(`Platform/CrashReporting.h`)가 가진다.
  - LG18의 `Platform/CrashHandler.h`는 이 안으로 흡수한다.
  - Core는 첫 로그 설치 때 정적 종료 콜백 하나를 등록한다. 로그 flush는 그 콜백이 한다(19절).
- **보고는 처음 들어온 스레드 하나만 한다.** 크래시한 스레드 T는 프로세스 전역 원자 변수로 보고 소유권을 얻는다.
  - T는 예외 레코드와 CONTEXT(XSTATE 비트를 지운 사본)를 미리 잡아 둔 정적 저장소에 복사한다. 그다음 전용 스레드 W에 넘기고 제한 시간만큼 기다린 뒤 `TerminateProcess`로 끝낸다.
  - 겹쳐 들어온 스레드는 스스로 끝내지 않는다. 소유자가 끝내기를 기다린다.
  - 같은 스레드의 재귀는 즉시 끝낸다.
- **크래시 처리 스레드 W는 heap을 쓰지 않는 일만 한다.** W가 하는 일은 셋이다.
  - 정적 버퍼에 메타데이터 조각을 서식한다.
  - 교환 블록에 쓰고 도우미에 알린다.
  - 종료 콜백(로그 flush)을 부른다.
  - 폴더 생성, 덤프, `CrashInfo.json` 디스크 쓰기는 도우미가 한다. 경로 변환(`CreateDirectoryW`·`CreateFileW`)이 프로세스 heap을 할당하기 때문이다.
- **덤프는 미리 띄운 도우미가 snapshot으로 쓴다.** 같은 실행 파일을 도우미 모드(`--rocks-crash-helper`)로 시작 때 한 번 띄운다. 크래시 뒤에 프로세스를 새로 만들지 않는다.
  - 크래시가 나면 도우미는 `PssCaptureSnapshot`(VA clone)으로 부모를 짧게 캡처하고 "캡처 끝"을 알린다.
  - 부모는 그때부터 로그 flush 뒤 끝나도 된다.
  - 도우미는 snapshot을 대상으로 `MiniDumpWriteDump`를 쓴다. Standard 덤프(`Minidump.dmp`)를 먼저 쓰고, Detailed·Full은 같은 snapshot에서 별도 파일로 쓴다. 마감을 넘기면 CancelCallback으로 취소하고 그 파일을 지운 뒤 `Cancelled`로 적는다(취소된 덤프는 열리지 않는다).
  - 도우미가 없으면 W가 프로세스 안에서 쓰고(flush를 먼저), 그 한계를 결과에 남긴다.
- **보고 없이 끝난 크래시도 기록한다.** 도우미는 부모가 보고 없이 NTSTATUS 오류 코드(`0xC…`, `STATUS_CONTROL_C_EXIT` 제외)로 끝나면 `kind=UnreportedExit` 기록을 남긴다. fast-fail 등이 여기에 해당한다.
- **Fatal도 같은 경로로 간다.** `platform::Fatal`은 작은 보호 함수 안에서 Rocks 전용 예외 코드로 `RaiseException`을 부르고, 다음 줄에서 무조건 끝낸다. 그래서 덤프의 예외 문맥이 Fatal을 부른 스레드의 실제 stack이 된다.
  - 수집기가 없으면 raise하지 않고 바로 끝낸다. 디버거가 붙어 있으면 `__debugbreak()` 뒤 끝낸다.
  - SIGABRT·invalid parameter·pure virtual call hook은 반환하지 않는다. Fatal은 `abort()`를 부르지 않는다.
- **내장(Embedded) 모드에서는 Rocks가 호스트 프로세스를 끝내지 않는다.** 콜백과 메타데이터까지만 하고 호스트 처리기로 넘긴다(8.8절).
- **정상 실행 비용은 없다.** 프레임마다 할당·전역 잠금·원자 연산을 더하지 않는다. 고정 비용은 다음뿐이다(18절).
  - 프로세스당 대기 스레드 하나와 도우미 프로세스 하나
  - 스레드마다 stack 보장 32 KiB
  - 정적 저장소 약 40 KiB
- **덤프 여는 법까지 정한다.**
  - 결과는 `Saved/Crashes/<프로그램>-<UTC 시각>-<pid>/`에 둔다. 파일은 `Minidump.dmp`와 `CrashInfo.json`이다.
  - 덤프에는 PE CodeView(PDB GUID·age)와 ToolBuildId, Rocks 크래시 기록 사용자 stream을 함께 넣는다.
  - PDB 경로가 파일 이름만 담도록 링크 인자를 고친다(`/PDBALTPATH`). 보관과 여는 절차는 14절이다.
- **플랫폼 수준을 숨기지 않는다.**
  - Windows는 덤프까지 지원한다.
  - macOS·Android는 신호 처리·로그 flush·메타데이터까지 한다. 덤프는 `Unsupported`로 표시하고, 이전 처리기를 복원해 OS 수집기(ReportCrash·tombstone)에 넘긴다.
  - 지원 수준은 capability로 조회한다(15절).

## 2. 범위

**범위 안**
- 공통 실행 계층 `Launch`: 플랫폼 진입점, 명령줄 캡처, 도구용 공통 로그 서비스, 단계 표시, 종료 코드, 프로세스 종료 요청(Ctrl-C) 전달
- PlatformBase 크래시 수집기: Windows SEH 경계(주 스레드·`platform::Thread`), 최종 unhandled filter, CRT 치명 hook, POSIX 치명 신호, 보고 소유권, 전용 스레드, 도우미 프로세스와 fallback, 메타데이터, 종료 콜백
- `platform::Fatal`·`ROCKS_ASSERT`·`Log::Fatal`의 공통 경로 연결
- LG18 코드 이관(`CrashHandler.h`·`SetTerminationHook`)
- 빌드: Windows subsystem 선택, PDB 경로 정리
- 실행 프로그램 적용: Sandbox, RocksCommandlet(Win64·Mac), FoundationTests·CoreFoundationTests·EngineTests, 벤치 넷, ShaderToolSpike, 게임 템플릿
- 크래시 시험(자식 프로세스)과 덤프 판독 검증

**범위 밖**
- 크래시 업로드 서버, 사용자 계정, 원격 분석, 전용 UI(요구 1절)
- 크래시 처리기 안의 심볼화·stack walk. 덤프와 빌드 식별 정보로 사후 분석한다(요구 8절).
- 정적 초기화 전·CRT 종료 뒤 보호(9절 표). 외부 수집(WER LocalDumps)과 연결할 지점만 적는다.
- `AppInfo`·`RunTraits`·명령줄 파서(CL1–CL7)·`BuildDescription`(AP2) 전체. 이번에 필요한 연결만 한다(21절).
- 모바일 수명주기(Android NativeActivity 등). 데스크톱 `main` 루프로 대신하지 않는다.
- C# 도구의 예외 정책(요구 14절).
- GPU 크래시 진단(사용자 보류, 2026-11-18).

## 3. Rocks 현황(2026-11-18, `55109b18` + 로깅 브랜치)

### 3.1 실행 진입점

| 실행 파일 | 진입점 | 시작·종료에서 하는 일 |
|---|---|---|
| Sandbox | `Programs/Sandbox/Private/Main.cpp:71` `main(int, char**)` | 인자를 손으로 파싱한다. `--self-test`는 Sync LogSystem과 stderr sink를 직접 시작·정지한다. `--engine`은 `RunEngineHost`(`EngineHost.cpp:181–250`)를 부른다: `FindEngineDirectory` → `HostSettings`(Initialize·LoadFiles·AddFile·ApplyCommandLine·CompleteStartup) → `LogHost::Start` → `LogInstallScope` → `TraceSession` → `TraceController` → `RunEngine`(Engine Initialize·CreateWorld·Run·Shutdown) → trace 정지 → `LogHost::Stop`. 출력 스레드가 멈춘 sink에 남으면 `_Exit`(`EngineHost.cpp:243–247`). 로깅 브랜치는 `CrashHandlerScope`를 LogHost 앞에 둔다(`6d8ed9ed`·`5ecb29a9`) |
| RocksCommandlet Win64 | `Programs/Commandlet/Private/Win64/Main.cpp:63` `wmain` | UTF-16 인자를 정적 `argumentStorage`에 UTF-8로 바꾼다(`Platform/Win64/WideString.h`). 출력은 콘솔이면 `WriteConsoleW`이고 아니면 `WriteFile`이다. 그다음 `commandlet_host::Run`(Sync LogSystem·stderr sink·`HostSettings`)을 부른다 |
| RocksCommandlet Mac | `Programs/Commandlet/Private/Mac/Main.cpp:21` `main` | SIGPIPE를 무시하고, UTF-8을 검증한 뒤 `commandlet_host::Run`을 부른다 |
| FoundationTests·CoreFoundationTests·EngineTests | `Programs/FoundationTests/Private/Main.cpp:138` 하나를 세 모듈이 공유(`FoundationTests.Build.cs:94–108`) | `PrepareAutomatedTestProcess`. `--process-probe`·`--logger-failure-probe`·`--negative-control`·`--number-corpus` 등 자식 모드가 있다. 그다음 시험 실행기를 돈다 |
| CompressionBench·CryptoBench·LogBench·MathBench | `Programs/*Bench/Private/Main.cpp` `main` | 손 파싱 후 `Run`. 결과는 printf다 |
| ShaderToolSpike | `Programs/ShaderToolSpike/Private/Common/Main.cpp:33` `main` | `PrepareSpikeProcess`(Windows `SetErrorMode`, `SpikeHostWin64.cpp:20`), Sync LogSystem. 로그 시작에 실패해도 본문은 돈다 |
| 게임 템플릿 | `Templates/Game/Foundation/Source/Main.cpp` `int main()` | TypeRegistry·ObjectRegistry만 쓴다(의존 ObjectRuntime) |

공통으로 빠진 것은 다음과 같다.
- 크래시 처리가 없다. 로깅 브랜치의 Sandbox만 LG18 최소 처리기를 건다.
- Windows 인자는 Commandlet만 넓은 문자로 받는다. 나머지 `main(char**)`는 ANSI 코드 페이지라 비 ASCII 인자가 깨진다.
- 대화상자 억제 코드가 TestSupport와 ShaderToolSpike 두 곳에 복사돼 있다.
- 종료 코드 규약이 프로그램마다 다르다(0·1·2, Sandbox 1·2, 벤치 1·2).

### 3.2 치명 오류 경로

- `ROCKS_ASSERT`(`Core/Public/Core/Assert.h:11`) → `AssertFailed(expr, file, line)`(`Core/Private/Diagnostics.cpp:7`) → `ReportAssertFailure`(`Log.cpp`): 비상 출력 → 설치된 LogSystem에 Fatal 기록 전달 → `platform::Fatal`.
- `Log::Fatal` → `LogFatal`(`Log.cpp:156–179`): 스레드별 `fatalInProgress`로 재귀를 막는다. 고정 버퍼에 렌더하고 비상 출력한 뒤, 기록을 LogSystem에 넘기고 제한 시간 동안 flush를 기다린다. 마지막에 `platform::Fatal`을 부른다.
- `platform::Fatal`(`PlatformBase/Private/Win64/Platform.cpp:119–132`, POSIX 같은 모양): stderr에 최대 4096 byte를 쓰고 `abort()`. 위치 정보가 없다. 헤더 인라인 코드(`Array.h:731` 등)가 직접 부른다.
- 로깅 브랜치(LG18)는 `platform::Fatal`이 abort 전에 종료 hook을 크래시한 스레드에서 직접 한 번 부른다(`Termination.cpp`).
- `abort()`가 SEH 경계로 들어오지 않는다. Windows UCRT의 abort는 SIGABRT 처리기를 부르고, `_CALL_REPORTFAULT`가 켜져 있으면 fail-fast로 WER에 간다. TestSupport가 `_set_abort_behavior(0, ...)`로 그것만 끈다.

### 3.3 스레드

- `platform::Thread` → `detail::RunThread`(`Private/Common/Thread.cpp:73–93`) → `ApplyCurrentThreadOptions` → `entry(context)`. Windows 시작 루틴은 `_beginthreadex`의 `ThreadEntry`(`Win64/Concurrency.cpp:71–76`)다. 보호 경계가 없다.
- 로깅 브랜치는 `ApplyCurrentThreadOptions`에서 `PrepareThreadForCrashHandling`(Windows `SetThreadStackGuarantee` 32 KiB, POSIX `sigaltstack` 32 KiB)을 부른다.
- Task worker·DedicatedLane·로그 출력 스레드·렌더 lane은 모두 `platform::Thread` 위에 있다. 외부 라이브러리·드라이버 스레드는 그렇지 않다.

### 3.4 LG18(로깅 브랜치 `feature/logging`, 아직 engine-runtime에 없음)

| 부분 | 내용 | 커밋 |
|---|---|---|
| `Platform/Fatal.h` | `TerminationHook`, `SetTerminationHook`(원자 교환), `TerminatingThreadId` | `4c09b09b`·`7b9b4717` |
| `Private/Common/Termination.cpp` | 종료 hook을 프로세스에서 한 번만 부른다(`terminationRan`). 종료를 부른 스레드 id는 처음 한 번만 기록한다(CAS) | 같음 |
| `Platform/CrashHandler.h`·`Win64/CrashHandler.cpp` | `SetUnhandledExceptionFilter`(앞 filter 연쇄), flush 전용 스레드(64 KiB, `CreateThread`), 수동 리셋 이벤트 둘. 첫 스레드만 hook을 맡기고 모두 제한 시간(기본 2 s) 대기 뒤 앞 filter로 간다 | 같음 |
| `Posix/CrashHandler.cpp` | SIGSEGV·SIGBUS·SIGILL·SIGFPE·SIGABRT, `SA_SIGINFO\|SA_ONSTACK`. `sem_post`로 flush 스레드를 깨우고 단조 시계로 폴링한 뒤 앞 동작을 복원해 다시 낸다. 스레드별 sigaltstack은 할당한다. 이미 있으면(ASan) 덮지 않는다 | 같음 |
| `Sync.h` | `ManualResetEvent::TrySignal(attempts)` | 같음 |
| `LogSystem::FlushForTermination` | 시작 전·sink 안·출력 스레드 자신이면 하지 않는다. 종료 스레드가 출력 스레드면 비상 출력만 한다. Sync는 `sinkMutex_.TryLock`, Async는 `PushPosition`까지 `drainedPosition_`를 기다리며 `wake_.TrySignal`과 양보를 반복한다(제한 시간). Manual·비상 모드는 false | 같음 |
| `LogTerminationHook`·`LogInstallScope` | `LogInstallScope`가 hook을 걸고 앞 hook을 기억한다. hook은 이 스레드의 `fatalInProgress`가 아니면 flush하고 앞 hook에 연쇄한다 | 같음 |
| Sandbox `CrashHandlerScope` | LogHost 앞에서 설치하고 범위 끝에서 뗀다 | `6d8ed9ed`·`5ecb29a9` |
| FoundationTests `LoggerProcessProbe` | 자식 probe: `platform::Fatal`(처리기 미설치), 접근 위반, stack overflow, 멈춘 sink 안의 Fatal | 같음 |

LG18 코드에서 이 설계가 바꾸는 점은 다음과 같다.
- Windows Fatal 경로에서 hook이 크래시한 스레드에서 돈다(stack 여유·보유 lock 문제가 그대로 남는다).
- 처리기가 앞 filter로 연쇄해 프로세스를 끝내는 주체가 정해지지 않았다. WER·디버거·다른 filter에 맡긴다.
- 보고 소유권이 hook 실행 한 번으로만 표현된다.

### 3.5 빌드·심볼

- 플랫폼은 `NativePlatform { Windows, MacOS, Android }`(`NativeBuildModels.cs:4`)뿐이다. Linux 대상은 없다.
- 구성은 `Debug, Development, Release, Asan`(`NativeBuildModels.cs:44`)이다. 모듈은 정적 라이브러리와 실행 파일뿐이다(DLL 없음, `NativeModuleKind`).
- Windows는 clang-cl과 lld-link를 쓴다(`ActionGraph.cs`).
  - 컴파일: `/EHs-c- /GR- /Z7`, Debug `/MDd`, 그 밖 `/MD`, Development·Release `/O2`, Release ThinLTO(`-flto=thin`, `ActionGraph.cs:193–195, 404–405`)
  - 링크: `/INCREMENTAL:NO /SUBSYSTEM:CONSOLE /MANIFEST:EMBED /DEBUG:FULL /PDB:<exe>.pdb`(`ActionGraph.cs:310–317`)
  - Asan: `/fsanitize=address`, `/INCLUDE:__asan_seh_interceptor /WHOLEARCHIVE:<asan thunk>`(`ActionGraph.cs:290–313`)
- 실제 PE(`out/dotnet-windows-sandbox-debug/bin/Sandbox.exe`, `llvm-readobj --coff-debug-directory`):
  - CodeView `PDBGUID {1FD3B109-0FC1-6339-4C4C-44205044422E}`. 뒤 8 byte가 ASCII "LLD PDB."다. lld가 PDB 내용 해시로 만든 GUID다.
  - `PDBAge 1`
  - `PDBFileName`이 링크 임시 폴더 `...\.native-tmp\<guid>\Sandbox.pdb`다. 배포 뒤 이 경로는 없다. 디버거는 기호 경로로 찾는다.
  - PE `TimeDateStamp`는 실제 시각이다(`/Brepro` 없음).
- `build::ToolBuildId`·`CurrentEngineVersion`·`ToolchainFingerprint`(`Core/Public/Core/Build/BuildInfo.h`)는 Core 수준 생성 TU다. PlatformBase는 볼 수 없다.

### 3.6 저장 경로·대화상자·엔진 종료

- 로그 파일 폴더 기본값은 `Saved/Logs`다(`LogHost.h:20`). 현재 작업 폴더 기준 상대 경로다. `HostSettingsOptions::savedDir`(`HostSettings.h:38`)를 채우는 호스트는 없다. trace 파일 예시는 `Saved/Traces/...`다.
- 대화상자 억제는 TestSupport `PrepareAutomatedTestProcess`(`Private/Win64/HostFailure.cpp`)가 한다. 내용은 `_set_abort_behavior`, `_CrtSetReportMode` → stderr, `SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX)`다. `PrepareFatalTestProcess`는 abort 동작만 바꾼다. VirtualMemoryProbe도 `SetErrorMode`를 쓴다.
- `Engine::RequestExit()`(`Engine.h:288–291`)는 일반 `bool exitRequested_`(400행)에 쓴다. 그런데 주석은 "언제 호출해도 안전하다"고 한다. 다른 스레드(콘솔 제어 처리기 등)에서 부르면 data race다(20절).
- DbgHelp·stack walk·`SetUnhandledExceptionFilter`·`AddVectoredExceptionHandler`를 쓰는 곳은 LG18 브랜치 말고 없다.

## 4. UE 5.8 조사

### 4.1 진입 구조

- **진입 순서.** `WinMain` → `LaunchWindowsStartup` → (SEH) `GuardedMain` → `LaunchWindowsShutdown`(`LW` 317–342, 303–313).
  - `SetupWindowsEnvironment`가 `_set_invalid_parameter_handler`를 건다(`LW` 87–98).
  - `-unattended`는 `SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX)`를 건다(`LW` 213).
- **보호를 생략하는 조건.** 비Debug는 `bIsEmbedded || bNoExceptionHandler || (bIsDebuggerPresent && !GAlwaysReportCrash)`면 SEH 없이 `GuardedMain`을 부른다(`LW` 255–266). Debug는 `-crashreports`가 없으면 늘 생략한다.
- **SEH 이중 구조.**
  - 안쪽(`GuardedMainWrapper`, `LW` 113–139): filter 식이 `(ReportCrash(...), EXCEPTION_CONTINUE_SEARCH)`다. 보고는 stack을 풀기 전에 filter 안에서 한다.
  - 바깥(`LW` 272–294): `EXCEPTION_EXECUTE_HANDLER`. `__except` 본문은 오류 처리, `LaunchStaticShutdownAfterError`, `RequestExit(true)` 순서다.
- **`GuardedMain`(`L` 87–205).**
  - 게임 스레드 id를 정하고, `-waitforattach`를 처리하고, PreMainInit 델리게이트를 부른다.
  - RAII 정리 객체의 소멸자가 늘 `EngineExit`를 부른다.
  - `EnginePreInit`가 실패하면 바로 반환한다. 그래도 `EngineExit`는 돈다.
  - 그 뒤 `while (!IsEngineExitRequested()) EngineTick()`.
- **역할 분담.** 플랫폼 진입점은 명령줄·SEH·종료 호출을 맡는다. `FEngineLoop`는 PreInit·Init·Tick·Exit를 맡는다. Rocks는 같은 분담으로 Launch와 `Engine`을 나눈다.

### 4.2 스레드와 최종 filter

- **`GuardedRun`(`WRT` 57–120).** 디버거·`-noexceptionhandler`면 보호 없이 `Run()`을 부른다. 아니면 `__try`, filter `ReportCrash`, `__except` 본문 `RequestExit(true)`다. 본문이 다시 크래시하면 `TerminateProcess(777004)`다.
- **엔진 밖 스레드.** `SetUnhandledExceptionFilter(EngineUnhandledExceptionFilter)`로 덮는다(`WCC` 1410–1412, 1808–1823). 정적 초기화 동안에는 디버거가 없을 때만 따로 filter를 건다(`WCC` 1379–1382).
- **없는 것.** `SetThreadStackGuarantee`와 크래시용 vectored handler는 Core·Launch에 없다.

### 4.3 보고 소유권과 전용 스레드

- **보고 소유권(`WCC` 1826–1854).** `GLog->Panic()`을 크래시한 스레드에서 먼저 부른다. 그다음 `InterlockedIncrement(&ReportCrashCallCount) == 1`인 스레드만 전용 스레드에 넘긴다. 모든 크래시 스레드는 `WaitUntilCrashIsHandled`에서 기다리고, 저마다 `TerminateProcess`로 간다.
- **전용 스레드(`WCC` 1249–1349, 1399–1403).**
  - 정적 초기화 때 만든다. stack은 기본값, 우선순위는 BELOW_NORMAL이다. 주석이 stack overflow 대비라고 적는다.
  - `IE_Crash`·`IE_StopThread`를 500 ms 주기로 기다린다. 크래시는 한 번만 처리하고 루프를 빠져나간다. 루프 자체의 크래시는 `TerminateProcess(777003)`다.
- **넘기는 방식(`WCC` 1508–1516, 직접 확인).**
  - `EXCEPTION_POINTERS` 포인터만 저장한다. 수명은 크래시 스레드가 filter 안에서 기다리는 동안뿐이다.
  - `CrashingThreadHandle = GetCurrentThread()`를 넘기는데, 이것은 의사 핸들이다. 전용 스레드에서 쓰면 자기 자신을 가리킨다. Rocks는 스레드 id만 넘기고 핸들은 넘기지 않는다.
- **제한 시간.** 기본 60 s다(`WCC` 112, 191–201). 넘겨도 프로세스는 끝난다.

### 4.4 외부 프로세스

- **에디터(monitor 모드).** 정적 초기화 때 CrashReportClient를 익명 파이프 둘과 `-MONITOR=<pid>`로 띄운다(`WCC` 512–556). 크래시가 나면 고정 크기 구조체를 파이프로 보낸다. 구조체 안에 엔진 프로세스 안의 `EXCEPTION_POINTERS*` 주소가 들어 있다(`WCC` 741–927).
  - CRC는 `OpenProcess(PROCESS_ALL_ACCESS)`, `ReadProcessMemory`, `MiniDumpWriteDump(ClientPointers=TRUE)` 순서로 덤프를 쓴다(`CRC` 526–692, `WCC` 138–153). 끝나면 4 byte 응답을 보낸다.
- **게임 빌드.** 크래시 시점에 엔진 프로세스 안에서 덤프를 쓰고 그 뒤 CRC를 띄운다(`WCC` 1629–1676). CRC가 없으면 덤프만 남는다.

### 4.5 덤프 종류·문맥·특수 코드

- **덤프 종류(`WCC` 165–183).** 기본은 `MiniDumpWithThreadInfo|MiniDumpWithIndirectlyReferencedMemory`다(비continuable, CVar 기본). 전체 덤프는 `-fullcrashdump`로 켜고 `WithFullMemory|FullMemoryInfo|HandleData|ThreadInfo`를 쓴다. 컨텍스트 XML을 사용자 stream으로 넣는다.
- **DbgHelp.** stack walk만 전역 lock으로 직렬화한다(`WSW` 41–42). `MiniDumpWriteDump`에는 lock이 없다. 심볼화는 크래시 처리기 안에서 한다(`WCC` 1678–1711).
- **assert·Fatal.** `check`·`UE_LOG Fatal`은 오류 출력 장치로 간다. `GIsGuarded`면 `RaiseException(0x4000, 0, 1, &FAssertInfo)`로 각 스레드의 SEH filter에 들어간다(`WEO` 70–85, `WCC` 1981–1997). 그래서 덤프의 예외 문맥은 `RaiseException` 지점이다. 메시지와 PC는 `FAssertInfo`로 덮는다.
- **특수 예외 코드.** 0x4000 assert, 0x8000 GPU, 0xC000 OOM, 777008 ensure(`WCC` 108–111).

### 4.6 디버거·ASan·CRT·종료

- **디버거.** 디버거가 있으면 SEH 보호를 생략하고 check는 break한다. `IsDebuggerPresent`는 비Shipping에서만 본다(`WPM` 1364–1368).
- **ASan.** `__SANITIZE_ADDRESS__`면 `WriteMinidump`가 바로 false다. 사유는 MiniDumpWriteDump hang과 DbgHelp 경합이다(`WCC` 77–90, 134–135). `__asan_set_error_report_callback`으로 ASan 보고를 Fatal 경로에 합류시킨다(`LW` 18–34).
- **CRT hook.**
  - SIGABRT: `signal(SIGABRT, AbortHandler)`. 처리기는 `__try { RaiseException(0x4000) } __except(ReportCrash)` 뒤 `TerminateProcess`로 끝나고 반환하지 않는다(`WCC` 1226–1244, 1457–1469).
  - purecall: 두 번째 호출이면 CRT 기본으로 넘긴다(`WPM` 848–869).
  - 없는 것: `set_terminate`, `_set_abort_behavior`(`WPM` 1498 주석 처리).
- **정상 종료와 오류 종료(`WPM` 1458–1523).** 정상 종료는 `GuardedMain` 반환값이다. 오류 종료는 `RequestExit(true)` → `TerminateProcess(3)`이다. 보고 체계 자체가 실패하면 777003·777004·777006을 쓴다(`GCH` 36–61).
- **stack overflow.** 특별한 분기가 없다. 크래시 스레드는 넘친 stack 위에서 `GLog->Panic()`과 대기를 돈다(`WCC` 1833–1845). stack 보장이 없어 위험하다.
- **SEH를 우회하는 종료.** 주석이 STATUS_STACK_BUFFER_OVERRUN과 fast-fail이 SEH를 우회한다고 적는다(`WCC` 1797–1803). `WerRegister*`·`RaiseFailFastException`은 쓰지 않는다. `WINDOWS_USE_WER`(기본 0)를 켜면 filter가 `CONTINUE_SEARCH`로 WER에 넘긴다.
- **크래시 폴더.** `<Saved>/Crashes/<UECC-...GUID>/`에 `CrashContext.runtime-xml`·`UEMinidump.dmp`·로그 복사본을 둔다(`GCC` 291–335, 1695–1701). XML에 명령줄(`GetOriginalForLogging`)과 사용자 이름이 들어간다.
- **POSIX(`UCC` 1340–1404, 1278–1291, 1069–1083).**
  - 신호 8종(SIGQUIT·ABRT·ILL·FPE·BUS·SEGV·SYS·TRAP)을 `SA_SIGINFO|SA_RESTART|SA_ONSTACK`, `sigfillset`로 받는다.
  - 스레드별 sigaltstack(SIGSTKSZ+192 KiB, guard page)을 둔다.
  - 처리 뒤 `SIG_DFL`로 돌리고 `raise`한다(core dump). 강제 종료는 `_exit`다(abort는 SIGABRT 루프 위험).

### 4.7 Rocks가 다르게 하는 점

| UE | Rocks | 이유 |
|---|---|---|
| 크래시 스레드에서 `GLog->Panic()`(lock 최대 1 s 대기) | 로그 flush는 전용 스레드의 종료 콜백에서 한다. 크래시 스레드는 복사와 신호만 한다 | 크래시 스레드는 stack이 모자라거나 lock을 쥐었을 수 있다(요구 9절) |
| `EXCEPTION_POINTERS` 포인터 저장(수명 계약 없음) | 예외 레코드·CONTEXT를 정적 저장소로 복사하고, 도우미는 그 정적 주소를 읽는다 | 요구 8절. 시간 초과 뒤에도 dangling이 없다 |
| 전용 스레드에 `GetCurrentThread()` 의사 핸들 전달 | 스레드 id만 넘긴다 | 실패 스레드와 작성 스레드 혼동 방지 |
| `SetThreadStackGuarantee` 없음 | 주 스레드와 모든 `platform::Thread`에 32 KiB 보장 | stack overflow filter가 넘치지 않게 |
| 게임 빌드는 크래시 뒤 프로세스 안에서 덤프, 에디터만 monitor. 덤프 동안 엔진 프로세스가 살아 있어야 한다 | 모든 실행 파일이 같은 실행 파일 도우미를 시작 때 띄운다. 도우미는 `PssCaptureSnapshot`(VA clone)으로 짧게 캡처한 뒤 snapshot에서 덤프를 쓴다. 부모는 캡처 뒤 끝나도 된다. fallback만 프로세스 안 | 요구 8절(외부 프로세스 우선, 크래시 뒤 생성에 기대지 않음), 덤프 시간과 부모 종료의 분리 |
| 크래시 처리 스레드가 폴더·XML·덤프·로그 복사를 프로세스 안에서 한다 | W는 heap을 쓰지 않는 일(서식·교환 블록·신호·콜백)만 하고, 디스크 쓰기는 도우미가 한다 | 경로 API가 heap을 할당한다(요구 9절) |
| 처리기 안 심볼화(GMalloc) | 하지 않는다 | 요구 8절 |
| 명령줄·사용자 이름을 XML에 기록 | 기록하지 않는다. 인자 개수만 적는다(가린 명령줄은 CL2 뒤) | 요구 12절, AppInfo D16 |
| 크래시 뒤 `TerminateProcess(3)` | 종료 코드 = 예외 코드(Fatal은 Rocks 예외 코드) | 계획 2479행 "OS crash code 보존" |
| 정적 초기화 크래시 filter | 없다(외부 수집 범위) | 9절 표 |

## 5. 대응표

| UE | Rocks | 위치 |
|---|---|---|
| `WinMain`·`LaunchWindowsStartup`·`LaunchWindowsShutdown` | `ROCKS_IMPLEMENT_APPLICATION`이 만드는 `wmain`·`wWinMain`·`main` → `launch::RunApplication` | `Launch/EntryPoint.h`·`Launch/Application.h` |
| `GuardedMain` | `RunApplication`의 본문(설치·캡처·서비스·본문 호출·정리) | Launch |
| `GuardedMainWrapper`(SEH) | `platform::RunWithCrashGuard` | PlatformBase `Private/Win64` |
| `FEngineLoop`·`EnginePreInit`·`EngineTick`·`EngineExit` | `Engine::Initialize`·`RunFrame`·`Run`·`Shutdown`(기존) | Engine |
| `FRunnableThreadWin::GuardedRun` | `detail::RunThread`의 보호 호출 | PlatformBase |
| `EngineUnhandledExceptionFilter` | 크래시 수집기의 최종 unhandled filter | PlatformBase `Private/Win64` |
| `ReportCrash`·`ReportCrashCallCount` | 보고 소유권 확보(`crashOwner` 원자 변수) | PlatformBase |
| `FCrashReportingThread` | 크래시 처리 스레드 | PlatformBase |
| CrashReportClient monitor 모드 | 덤프 도우미 프로세스(같은 실행 파일, `--rocks-crash-helper`, snapshot 덤프) | PlatformBase `Private/Win64` |
| `FSharedCrashContext`(파이프) | 크래시 교환 블록(공유 메모리) | PlatformBase `Private/Win64` |
| `FGenericCrashContext`·`FWindowsPlatformCrashContext` | 크래시 기록(고정 크기 정적 저장소) | PlatformBase |
| `CrashContext.runtime-xml`·`UEMinidump.dmp` | `CrashInfo.json`·`Minidump.dmp` | 크래시 폴더 |
| `FDebug::AssertFailed` → `ReportAssert`(`RaiseException(0x4000)`) | `platform::Fatal` → 보호 함수 안 `RaiseException(RocksFatalExceptionCode)` | PlatformBase |
| `GLog->Panic()` | 종료 콜백 → `LogSystem::FlushForTermination` | PlatformBase 등록, Core 구현 |
| `-crashreports`·`GAlwaysReportCrash` | `CrashReportingOptions::reportWhenDebuggerAttached` | PlatformBase |
| `-unattended` → `SetErrorMode` | `platform::SuppressSystemErrorDialogs` | PlatformBase |
| `-fullcrashdump` | `CrashDumpDetail::Full` | PlatformBase |
| `RequestExit(bForce=true)` → `TerminateProcess` | 크래시 종료(`TerminateProcess(예외 코드)`) | PlatformBase |
| `RequestEngineExit`·`IsEngineExitRequested` | `Engine::RequestExit`(게임 스레드)과 `launch::RequestTermination`(아무 스레드, 원자 변수)을 나눈다 | Engine·Launch |
| `PureCallHandler`·`InvalidParameterHandler`·`AbortHandler` | 수집기가 거는 CRT 치명 hook | PlatformBase `Private/Win64` |
| `UnhandledStaticInitException` | 없음(범위 밖, 9절) | — |

## 6. 모듈 배치와 의존 방향

```
실행 파일(Programs/*, 게임)                 ROCKS_IMPLEMENT_APPLICATION(Describe) + 본문
   │
   ├── Engine(Engine/Source/Runtime/Engine)   엔진 호스트만. Initialize·Run·Shutdown(기존)
   │
   └── Launch(Core/Source/Runtime/Launch)     진입점·명령줄 캡처·도구 로그 서비스·단계 표시·종료 코드
          │
          ├── Core                            ProcessCommandLine(Core/App), LogSystem, BuildInfo
          │      └── 종료 콜백 등록(첫 LogInstallScope 설치 때 정적 콜백 1회, 해제 없음)
          │
          └── PlatformBase                    크래시 수집기, Fatal, Thread 보호, 대화상자 억제
                 └── Base
```

- **의존 방향은 위에서 아래로만 간다.** PlatformBase는 Core의 `LogSystem`·`StringView`·`Result`·`Allocator`를 모른다. 위 계층이 PlatformBase에 넘기는 것은 다음뿐이다.
  - C 문자열(설치 때 복사)
  - 함수 포인터와 사용자 포인터(종료 콜백)
  - 고정 크기 POD
- **Launch는 Engine을 모른다.** 콘솔 도구(Commandlet·벤치·ShaderToolSpike·시험)는 Launch만 링크한다. Engine·World·Renderer·Window에 닿지 않는다. 엔진 호스트는 본문에서 Engine을 쓴다.
- **엔진 호스트 공용 단계(HostSettings → LogHost → Trace → Engine)는 이번에 모듈로 빼지 않는다.** 지금 쓰는 곳이 Sandbox 하나다. 게임 템플릿은 ObjectRuntime만 쓴다. Engine을 쓰는 게임 템플릿이 생길 때 `Engine/Source/Runtime` 밖의 호스트 모듈로 뺀다(D11).

| 계층 | 책임 | 하지 않는 것 |
|---|---|---|
| 실행 파일 | `ApplicationDescription`(이름·종류·설정 조정·본문), 프로그램 인자 해석, 본문 | 크래시 처리 설치, 명령줄 변환, 도구 로그 시작·정지, 종료 코드 변환 |
| Launch | 플랫폼 진입점, 도우미 모드 분기, 이른 크래시 처리 설치, 명령줄 캡처, 빌드 식별 문자열 준비, 설정 조정 호출, 대화상자 정책, 도구 로그 서비스, 단계 표시, 본문 호출, 초기화 실패 역순 정리, 종료 코드, 크래시 처리 해제, 종료 요청(Ctrl-C) 원자 표시 | 엔진 수명, 창·입력, 설정 계층(HostSettings), 크래시 처리 내부 |
| Engine | 초기화·프레임·종료(실패 시 역순 정리, World·Subsystem·GC·작업 정리) | 프로세스 진입·크래시 처리. 단계 이름만 PlatformBase에 표시한다(선택, LC6) |
| PlatformBase 크래시 수집기 | OS 처리기 설치·제거, 보호 호출, 보고 소유권, 문맥 복사, 전용 스레드, 도우미, 덤프, 메타데이터, 종료 콜백 호출, 크래시 종료 | 로그 형식, 엔진 객체 순회, 심볼화, 업로드 |
| Core(로깅) | 종료 콜백 구현(`FlushForTermination`), Fatal·assert 기록 | OS 처리기 설치, 전용 스레드 |

## 7. 공개 API 초안

이름은 바뀔 수 있다. 여기서는 모양과 계약을 정한다. 실제 헤더 주석에는 목적·사용 예·인자·반환·오류·수명을 모두 쓴다.

### 7.1 PlatformBase `Platform/CrashReporting.h`

LG18 `Platform/CrashHandler.h`를 대체한다(하위호환 없음).

```cpp
namespace rocks::platform {

// 이 플랫폼·구성에서 크래시 수집이 실제로 할 수 있는 것이다(비트 묶음). 빈 구현이 성공을 꾸미지 않도록
// 할 수 없는 것은 비트를 끈다.
enum class CrashCapability : uint32 {
    None = 0,
    OsFaultHandling = 1u << 0,       // 접근 위반·치명 신호를 잡는다
    ThreadGuard = 1u << 1,           // platform::Thread 진입을 보호한다(Windows SEH)
    Minidump = 1u << 2,              // 분석 가능한 덤프 파일
    OutOfProcessDump = 1u << 3,      // 도우미 프로세스가 덤프를 쓴다
    Metadata = 1u << 4,              // CrashInfo.json
    TerminationCallbacks = 1u << 5,  // 전용 스레드에서 종료 콜백을 부른다
    SystemCrashReport = 1u << 6,     // 처리 뒤 OS 수집기(ReportCrash·tombstone)에 넘긴다
    UnreportedExitRecord = 1u << 7,  // 도우미가 보고 없는 비정상 종료를 기록한다
};
CrashCapability QueryCrashCapabilities() noexcept;

enum class CrashDumpDetail : uint8 {
    Standard,  // 스레드·모듈·stack·간접 참조 메모리(기본)
    Detailed,  // + 데이터 섹션·핸들·PEB/TEB(명령줄·환경 블록이 들어갈 수 있다)
    Full,      // 전체 프로세스 메모리
};

enum class CrashHelperPolicy : uint8 {
    Preferred,  // 도우미를 띄우고, 실패하면 프로세스 안 fallback(결과에 표시)
    InProcess,  // 도우미 없이 전용 스레드가 프로세스 안에서 쓴다
    Deferred,   // 설치 때는 띄우지 않고 StartCrashHelper()가 띄운다(Launch가 설정을 읽은 뒤, LC3)
};

struct CrashReportingOptions {
    // 기본값은 10.6절 표와 같다.
    static constexpr uint32 DefaultCaptureTimeoutMilliseconds = 10000;
    static constexpr uint32 DefaultCallbackBudgetMilliseconds = 2000;
    static constexpr uint32 DefaultInProcessDumpTimeoutMilliseconds = 30000;
    static constexpr uint32 DefaultMetadataTimeoutMilliseconds = 2000;
    static constexpr uint32 DefaultConcurrentCrashGraceMilliseconds = 5000;
    static constexpr uint32 DefaultHelperDumpDeadlineMilliseconds[3] = {60000, 120000, 600000};  // Standard·Detailed·Full

    const char* programName = nullptr;      // 필수. 파일 이름 문자만. 설치 때 복사한다
    const char* outputDirectory = nullptr;  // nullptr이면 "Saved/Crashes". 설치 때 절대 경로로 복사한다
    const char* identity = nullptr;         // 빌드 식별 JSON 조각(Launch가 만든다, 14.2절). 복사한다
    const char* helperExecutable = nullptr; // nullptr이면 자기 실행 파일(11.2절). 복사한다
    bool embedded = false;                  // true: 호스트 내장. OS 처리기를 걸지 않고, 보고 뒤 호스트로 전파한다(8.8절)
    bool captureHeapCorruption = false;     // STATUS_HEAP_CORRUPTION 전용 VEH(8.6절, 선택)
    bool reportWhenDebuggerAttached = false;
    CrashHelperPolicy helper = CrashHelperPolicy::Preferred;
    CrashDumpDetail dumpDetail = CrashDumpDetail::Standard;
    uint32 captureTimeoutMilliseconds = DefaultCaptureTimeoutMilliseconds;
    uint32 callbackBudgetMilliseconds = DefaultCallbackBudgetMilliseconds;
    uint32 inProcessDumpTimeoutMilliseconds = DefaultInProcessDumpTimeoutMilliseconds;
    uint32 metadataTimeoutMilliseconds = DefaultMetadataTimeoutMilliseconds;
    uint32 concurrentCrashGraceMilliseconds = DefaultConcurrentCrashGraceMilliseconds;
    uint32 helperDumpDeadlineMilliseconds = 0;  // 0이면 dumpDetail의 기본값
    uint32 argumentCount = UnknownArgumentCount; // 메타데이터 arguments.count(Launch가 argc로 채운다, LC3)
};

enum class CrashReportingStatus : uint8 {
    Installed,
    AlreadyInstalled,  // 두 번째 설치(부트 순서 버그). 상태는 바뀌지 않는다
    InvalidArgument,   // 이름·경로 문법, 길이 상한
    OutOfResources,    // 전용 스레드·이벤트 준비 실패. 아무것도 걸지 않는다
};

// 프로세스에 하나 설치한다(시작 단계, 다른 스레드가 생기기 전 권장). 도우미 시작 실패는 설치 실패가 아니다.
// 크래시 때 fallback이 쓰이고 결과에 표시된다.
CrashReportingStatus InstallCrashReporting(const CrashReportingOptions& options) noexcept;

// InstallCrashReporting이 InvalidArgument로 거절할 옵션인지 상태를 바꾸지 않고 미리 본다(LC4). 설치된 수집기를 다른
// 설정으로 바꾸기 전에 쓴다. true여도 폴더를 절대 경로로 굳히지 못하거나 자원이 모자라면 설치는 실패할 수 있다.
bool ValidateCrashReportingOptions(const CrashReportingOptions& options) noexcept;

// 도우미에 정상 종료를 알리고 기다린 뒤(제한 시간) 처리기를 떼고 전용 스레드를 끝낸다. 다른 Rocks 스레드가
// 모두 끝난 종료 단계에서만 부른다. 설치하지 않았으면 아무 일도 하지 않는다.
void RemoveCrashReporting() noexcept;

// 출력 폴더를 바꾼다(설정을 읽은 뒤, 엔진 시작 전 부트 단계에서만). 실패하면 이전 값을 유지한다.
bool SetCrashOutputDirectory(const char* directory) noexcept;

// 실행 단계다. stageName은 정적 수명 문자열(리터럴)이어야 한다. 크래시 기록의 "phase"·"stage"가 된다.
enum class RunPhase : uint8 { Startup, Running, Shutdown };
void SetRunPhase(RunPhase phase, const char* stageName) noexcept;

// 연관 파일·식별 정보다. 고정 칸에 복사하고 크래시 때 메타데이터에 넣는다. 정상 실행 중 드물게(파일
// 열기·회전 때) 부른다. 칸마다 쓰는 쪽은 하나여야 한다.
enum class CrashAnnotation : uint8 { LogDirectory, LogFile, TraceFile, Custom0, Custom1, Count };
inline constexpr usize MaxCrashAnnotationBytes = 512;
bool SetCrashAnnotation(CrashAnnotation slot, const char* text, usize bytes) noexcept;

// 자동 실행의 OS·CRT 오류 대화상자 끄기(13.3절). 프로세스 설정이다.
void SuppressSystemErrorDialogs() noexcept;

// 보호 호출. Windows는 SEH 경계 안에서 body를 부르고, 처리되지 않은 예외를 공통 보고 경로로 보낸다.
// 다른 플랫폼은 그냥 부른다(신호 처리기가 프로세스 전역이다). body가 돌려준 값을 돌려준다.
using GuardedBody = int (*)(void* context) noexcept;
int RunWithCrashGuard(GuardedBody body, void* context) noexcept;

// 스레드 준비. platform::Thread가 시작·끝에서 부르고, 설치가 설치한 스레드(주 스레드)에 부른다.
// 외부 라이브러리가 만든 스레드는 직접 부를 수 있다(선택).
inline constexpr usize CrashStackReserveBytes = 32 * 1024;
void PrepareThreadForCrashHandling() noexcept;
void ReleaseThreadCrashHandling() noexcept;

// 종료 콜백(로그 flush 등)은 19절에 계약을 따로 적는다.

// 도우미 모드. Launch 진입점이 다른 무엇보다 먼저 부른다. argv가 도우미 호출이면 도우미를 돌린 뒤 종료
// 코드를 exitCode에 넣고 true다. 아니면 false다(아무것도 하지 않는다).
bool RunCrashHelperIfRequested(int argc, wchar_t** argv, int& exitCode) noexcept;  // Windows(char 판도 있다)

// Deferred로 설치한 수집기의 도우미를 띄운다(LC3). 결과는 Started, NotApplicable(Deferred가 아님·내장·sanitizer·
// POSIX·이미 부름), Failed(띄우지 못함, 크래시 때 프로세스 안 fallback)다(LC4).
enum class CrashHelperStart : uint8 { Started, NotApplicable, Failed };
CrashHelperStart StartCrashHelper() noexcept;

} // namespace rocks::platform
```

### 7.2 `Platform/Fatal.h` 변경

```cpp
// 복구할 수 없는 invariant 실패. stderr 비상 출력 뒤 크래시 수집기가 설치돼 있으면 공통 보고 경로(덤프·
// 종료 콜백·메타데이터)를 거쳐 프로세스를 끝낸다. 설치돼 있지 않으면 비상 출력만 하고 끝낸다.
// abort()를 부르지 않는다(SIGABRT 처리기와의 순환 방지). 부른 위치(return address)를 기록한다.
[[noreturn]] void Fatal(const char* message) noexcept;
// 위치를 아는 호출자(assert)용이다. file은 정적 수명 문자열이다.
[[noreturn]] void FatalAt(const char* message, const char* file, uint32 line) noexcept;
```

- `SetTerminationHook`·`TerminatingThreadId`는 지운다. 종료 콜백(19절)이 대신한다.
- `WriteDiagnostic`은 그대로 둔다. 크래시 경로의 비상 출력도 이것을 쓴다.

### 7.3 Core `Core/App/ProcessCommandLine.h`(AppInfo 12절의 부분 구현)

```cpp
namespace rocks {
class ProcessCommandLine {
public:
    static constexpr usize MaxArguments = 32 * 1024;            // LC4: OS 한계에 맞춤
    static constexpr usize MaxTotalBytes = 2 * 1024 * 1024;
    static constexpr usize MaxExecutablePathBytes = 96 * 1024;
    // 진입점이 한 번 부른다. 오류: 두 번째 호출 AlreadyExists, 잘못된 UTF-16·UTF-8 InvalidArgument,
    // 상한 초과 CapacityExceeded, 실행 파일 경로 오류는 그 파일 오류.
    static Result<void> Capture(int argc, wchar_t** argv) noexcept;  // Windows wmain·wWinMain(__wargv)
    static Result<void> Capture(int argc, char** argv) noexcept;     // POSIX
    static const ProcessCommandLine& Get() noexcept;                 // 전제: Capture 성공
    static bool IsCaptured() noexcept;
    StringView ExecutablePath() const noexcept;         // OS에서 얻는다(GetModuleFileNameW·GetExecutablePath)
    Span<const StringView> Arguments() const noexcept;  // 실행 파일 제외, 원문 순서
};
}
```

- 저장소는 정적 BSS다(인자 view 32768개, 텍스트 2 MiB, 실행 파일 경로, 약 2.5 MiB이고 쓴 만큼만 실제 메모리가 된다). 할당하지 않는다(LC4, 25절).
- Windows `wWinMain`은 CRT가 이미 나눈 `__argc`·`__wargv`를 쓴다. 그래서 AppInfo 12절의 `GetCommandLineW` 직접 분해는 필요 없다. Launch 진입점을 거치지 않는 `main(char**)`는 Windows에서 지원하지 않는다(D7).

### 7.4 Launch `Launch/Application.h`·`Launch/EntryPoint.h`

```cpp
namespace rocks::launch {

enum class ApplicationKind : uint8 {
    Tool,        // 명령줄 도구·벤치
    EngineHost,  // 게임·Sandbox 엔진 실행
    TestRunner,  // 시험 실행기와 그 자식 probe. 기본: 대화상자 억제
};

enum class ToolLogging : uint8 { None, Stderr };  // Stderr: Sync LogSystem + stderr sink + LogInstallScope

enum class CrashReportingMode : uint8 {
    Full,       // OS 처리기 + 도우미(실패하면 fallback)
    InProcess,  // OS 처리기 + 프로세스 안 덤프
    Off,        // 설치하지 않는다(의도적 크래시 probe 등). Fatal은 raise 없이 즉시 종료
};

struct ApplicationSettings {
    CrashReportingMode crashReporting = CrashReportingMode::Full;
    platform::CrashDumpDetail dumpDetail = platform::CrashDumpDetail::Standard;
    uint32 helperDumpDeadlineMilliseconds = 0;  // 0: 종류별 기본(11.7절)
    // 크래시 처리 예산(LC4). 0이면 CrashReportingOptions 기본값이다. 범위 밖이면 설정 단계가 경고하고 이전 설정을 유지한다.
    uint32 crashCallbackBudgetMilliseconds = 0;
    uint32 crashCaptureTimeoutMilliseconds = 0;
    uint32 inProcessDumpTimeoutMilliseconds = 0;
    uint32 concurrentCrashGraceMilliseconds = 0;
    bool reportWhenDebuggerAttached = false;
    bool captureHeapCorruption = false;         // 힙 손상 VEH(선택, 8.6절)
    const char* crashDirectory = nullptr;       // nullptr: 기본(Saved/Crashes)
    const char* crashHelperPath = nullptr;      // nullptr: 자기 실행 파일(배포 구성·시험용)
    bool suppressSystemErrorDialogs = false;  // TestRunner 기본 true
    bool handleTerminationRequests = false;   // Ctrl-C·SIGINT·SIGTERM을 원자 표시로(첫 번째만)
    ToolLogging toolLogging = ToolLogging::None;
};

class ApplicationContext {
public:
    Span<const StringView> Arguments() const noexcept;
    StringView ExecutablePath() const noexcept;
    Allocator& SystemAllocator() noexcept;  // Launch가 소유한 SystemAllocator
    LogSystem* ToolLog() noexcept;          // ToolLogging::Stderr일 때만
    void SetStage(platform::RunPhase phase, const char* stageName) noexcept;  // 정적 문자열
    // 본문이 정적 소멸·CRT 종료를 돌리면 안 된다고 판단했을 때(멈춘 로그 출력 스레드 등). Launch가 정리 뒤
    // _Exit(code)로 끝낸다.
    void RequestExitWithoutStaticTeardown() noexcept;
};

struct ApplicationDescription {
    const char* name;  // 정적. 크래시 폴더·메타데이터·도구 로그 호스트 이름
    ApplicationKind kind;
    ApplicationSettings settings;  // 기본 설정. 이른 크래시 수집은 이 값으로 건다(LC3)
    // 선택. 명령줄 캡처 뒤, 본문 전에 설정을 고친다(시험 probe의 크래시 수집 끄기 등).
    void (*configure)(Span<const StringView> arguments, ApplicationSettings& settings) noexcept = nullptr;
    int (*run)(ApplicationContext& context) noexcept;  // 본문. 반환값이 종료 코드다
    // 명령줄 캡처 실패의 종료 코드(LC4). 사용법 오류를 따로 정한 도구가 바꾼다(Commandlet은 2).
    int commandLineFailureExitCode = CommandLineFailureExitCode;  // 기본 1
};

// 진입점 공통 본문. 직접 부르지 않고 매크로를 쓴다.
int RunApplication(const ApplicationDescription& description, int argc, wchar_t** argv) noexcept;  // Windows
int RunApplication(const ApplicationDescription& description, int argc, char** argv) noexcept;     // POSIX
int RunWindowedApplication(const ApplicationDescription& description) noexcept;  // wWinMain(__argc·__wargv, LC3)
// 내장 호스트의 진입점에서 부른다(LC4). 내장 여부는 이 진입 함수가 정한다(CrashReportingMode에 Embedded가 없다).
// Full·InProcess는 내장 보고(OS 처리기·도우미 없음, 보고 뒤 호스트로 전파, 종료는 호스트, 8.8절), Off는 끈다.
int RunEmbeddedApplication(const ApplicationDescription& description, int argc, wchar_t** argv) noexcept;  // Windows
int RunEmbeddedApplication(const ApplicationDescription& description, int argc, char** argv) noexcept;     // POSIX

// 아무 스레드·신호 처리기에서 부를 수 있는 종료 요청이다(원자 표시). 엔진 호스트는 게임 스레드
// (PumpEvents)에서 IsTerminationRequested()를 보고 Engine::RequestExit()로 옮긴다.
void RequestTermination() noexcept;
bool IsTerminationRequested() noexcept;

} // namespace rocks::launch

// EntryPoint.h — 실행 파일의 소스 하나에 한 번 쓴다.
//   const rocks::launch::ApplicationDescription& Describe() noexcept;
//   ROCKS_IMPLEMENT_APPLICATION(Describe)           // 콘솔 subsystem(기본)
//   ROCKS_IMPLEMENT_WINDOWED_APPLICATION(Describe)  // Windows GUI subsystem(빌드 규칙 WindowsSubsystem=Windows)
```

- **매크로가 만드는 것.**
  - Windows 콘솔: `int wmain(int argc, wchar_t** argv) { return RunApplication(Describe(), argc, argv); }`
  - Windows GUI: `int WINAPI wWinMain(...)`. 인자는 `__argc`·`__wargv`에서 얻는다.
  - POSIX: `int main(int, char**)`
  - 매크로를 담은 헤더는 `<Windows.h>`를 넣지 않고 선언만 쓴다.
- **빌드 규칙.** `NativeModuleInput`에 `WindowsSubsystem { Console, Windows }`(기본 Console)를 더한다.
  - 링크가 `/SUBSYSTEM:CONSOLE` 또는 `/SUBSYSTEM:WINDOWS`를 고른다.
  - 진입점 매크로와 맞지 않으면 링크 오류(진입 기호 없음)로 드러난다.
  - 기존 도구는 모두 Console로 남는다(요구 4절).
- **본문 서명이 `ApplicationContext&` 하나인 이유.** 인자·할당기·도구 로그·단계 표시를 모두 거기서 얻는다. 그래서 프로그램마다 시작 코드를 되풀이하지 않는다.

## 8. 흐름

### 8.1 시작(Windows 콘솔 기준, 요구 10절의 "플랫폼 진입 이후 초기화")

| 순서 | 일 | 실패 처리 |
|---|---|---|
| 0 | `wmain` → `RunApplication`. 처음에 `platform::RunCrashHelperIfRequested`를 부른다. 도우미 호출이면 도우미를 돌리고 반환한다 | 도우미 인자 오류는 종료 코드 2 |
| 1 | `Describe()`의 정적 설명으로 **이른 크래시 처리**를 설치한다. OS 처리기, CRT hook, 전용 스레드, 주 스레드 stack 보장, 크래시 기록 초기화를 한다. 도우미는 아직 없다(InProcess 수준). 설명이 `Off`면 건너뛴다. 빌드 식별 문자열(엔진 버전·ToolBuildId·구성·플랫폼·아키텍처·PE CodeView)은 여기서 고정 버퍼에 만든다 | 설치 실패는 stderr 한 줄, 계속한다(크래시 수집 없이 실행) |
| 2 | `RunWithCrashGuard`로 3–9를 보호 프레임 안에서 돈다 | — |
| 3 | `ProcessCommandLine::Capture` | 종료 코드 `description.commandLineFailureExitCode`(기본 1), stderr 한 줄(원인, 상한 초과면 상한)(AppInfo 8.3절 0단계) |
| 4 | `configure(arguments, settings)`(선택) | — |
| 5 | 설정 적용: 대화상자 억제, 크래시 모드·예산 변경, 출력 폴더, **도우미 시작**(Full). 수집 방식·예산이 바뀌면 새 옵션을 `ValidateCrashReportingOptions`로 먼저 검사하고(잘못되면 지금 설치 유지), 1단계를 뗀 뒤 다시 건다. 다시 걸기가 실패하면 이전 설정·이전 폴더로 되돌려 건다. 폴더만 바뀌었거나 되돌린 경우의 새 폴더는 `SetCrashOutputDirectory`로 바꾼다(실패하면 이전 폴더 유지). `Off`면 1단계를 뗀다(도우미는 아직 없으므로 알릴 것이 없다). 내장 여부는 진입 함수가 정하므로 이 단계에서 바뀌지 않는다 | 검사 실패·재설치 실패·폴더 실패·도우미 시작 실패는 `Log::Warning` 뒤 계속한다(fallback) |
| 6 | 종료 요청 처리기(선택) | 실패는 경고만 |
| 7 | 도구 로그 서비스(`ToolLogging::Stderr`) | stderr 한 줄, 로그 없이 계속(D22) |
| 8 | `SetRunPhase(Running, "Main")` → `run(context)` | 본문의 반환값 |
| 9 | 7 → 6 역순 정리, `SetRunPhase(Shutdown, ...)` | 정리 실패는 stderr. 종료 코드는 본문 값을 유지한다 |
| 10 | 보호 프레임을 나와 `RemoveCrashReporting`(도우미에 `Shutdown`을 먼저 쓴다) → 종료 코드 반환(또는 `_Exit`) | — |

- **초기화 실패는 성공한 단계만 역순으로 정리한다.** Launch의 단계는 3–7뿐이다. 각 단계의 정리는 "시작됐는지" 표시로 판단한다. 엔진 호스트의 내부 단계(HostSettings·LogHost·Trace·Engine)는 지금처럼 본문의 범위 객체와 `Engine`의 역순 정리가 맡는다.
- **크래시 처리를 설정보다 먼저 건다(요구 10절).** 설정 로딩·LogHost·Engine 초기화는 모두 8단계 본문 안이다.
- **모듈 로더(ML1, 아직 없음).** [모듈 로더](ModuleLoader.ko.md) 3.9절은 7단계 뒤·8단계 전에 로더 생성·`Prepare`·부트 모듈과 `EarliestPossible` 시작을, 9단계에서 도구 로그 정리 전에 프로세스 묶음 종료를 넣는다. 지금(ML0)은 Launch가 로더를 만들지 않는다. 모듈 진입점(`ROCKS_IMPLEMENT_MODULE`)은 상수 기술과 함수뿐이라 실행 순서가 없고, 프로세스 진입점(`ROCKS_IMPLEMENT_APPLICATION`)과 겹치지 않는다(21절).
- **정상 종료 정리는 한 번만 한다.** `RunApplication`은 반환 경로가 하나다. 크래시 종료는 이 경로를 타지 않는다(8.3절).

### 8.2 정상 종료

1. 본문이 반환한다. 엔진 호스트는 그 전에 `Engine::Shutdown` → trace 정지 → `LogHost::Stop`을 마쳤다.
2. Launch가 도구 로그를 멈추고 종료 요청 처리기를 뗀다.
3. `RemoveCrashReporting`이 다음 순서로 정리한다.
   1. `crashOwner`를 sentinel(`ShutdownOwner`)로 CAS한다. 실패하면 이미 보고가 진행 중이라는 뜻이다. 그때는 돌아오지 않고 기다리며, 종료는 소유자 T가 한다(B11).
   2. 교환 블록 상태를 `Shutdown`으로 쓰고 도우미를 깨운다.
   3. 도우미 종료를 최대 2 s 기다린다. 넘으면 도우미를 `TerminateProcess`로 끝낸다.
   4. filter·CRT hook·SIGABRT 처리기를 이전 값으로 되돌린다.
   5. W를 깨워 join한다.
4. 반환값이 `wmain`의 반환값이 되고, CRT가 `exit`를 돌린다(정적 소멸). `RequestExitWithoutStaticTeardown`이면 같은 `RemoveCrashReporting`(`Shutdown` 기록 포함)을 마친 뒤 `_Exit(code)`다.

정상 종료(`exit`·`_Exit`)는 어떤 종료 코드든 도우미가 기록하지 않는다. `Shutdown` 상태가 먼저 쓰이고, 기록 조건(11.4절)도 NTSTATUS 오류 코드로 좁혀져 있다.

### 8.3 크래시 종료(Windows 접근 위반, 도우미 있음)

```
실패 스레드 T (filter, 넘친 stack일 수 있음)       크래시 처리 스레드 W(heap 없음)           도우미 H
─────────────────────────────────────────────      ────────────────────────────────          ───────────
보호 filter(RunWithCrashGuard·스레드 보호·unhandled)
 ├ 현재 스레드가 W면 → reporterFailed, SetEvent(reportFinished), 무한 대기
 ├ 디버거가 있고 강제 옵션이 없으면 → CONTINUE_SEARCH
 ├ ASan 구성이고 하드웨어 예외면 → CONTINUE_SEARCH(13.2절)
 ├ crashOwner CAS(0 → T)
 │   ├ 이미 T면(재귀) → 비상 출력 + TerminateProcess(RecursiveCrash)
 │   └ 다른 스레드면 → 칸 기록 후 소유자의 종료를 기다림(10.2절)
 ├ EXCEPTION_RECORD·CONTEXT → 정적 사본(XSTATE 비트 제거, 정렬 16)
 ├ 정적 EXCEPTION_POINTERS·크래시 기록(스레드 id·이름·단계·시각)
 ├ SetEvent(crashRequested) ───────────────────────▶ 깨어남
 │                                                   ├ 정적 버퍼에 메타데이터 조각 서식 → 교환 블록
 │                                                   ├ 상태=CrashRequested, SetEvent(request) ─────▶ 폴더 만들기, 예비 CrashInfo.json
 │                                                   │                                              PssCaptureSnapshot(VA clone)
 │                                                   ├ captureDone 대기(captureTimeout) ◀───────── SetEvent(captureDone)
 │                                                   ├ 종료 콜백(로그 flush), 공용 마감               MiniDumpWriteDump(snapshot,
 │                                                   ├ 콜백 결과 → 교환 블록, SetEvent(parentDone) ─▶   종류별 마감·CancelCallback)
 │                                                   └ SetEvent(reportFinished)                       최종 CrashInfo.json, snapshot 해제
 ├ WaitForSingleObject(reportFinished, 총 예산) ◀───┘
 └ TerminateProcess(GetCurrentProcess(), 예외 코드)
```

- **부모가 멈춰 있는 시간은 snapshot 캡처 동안과 종료 콜백 예산뿐이다.**
  - 덤프 작성 시간은 부모 종료와 분리된다. 그래서 Full 덤프도 부모를 붙잡지 않는다.
  - 캡처(`PssCaptureSnapshot`)는 취소할 수 없는 호출이라 그 상한은 도우미에 달려 있다.
  - 부모는 `captureTimeout`(기본 10 s)까지만 기다린다. 넘으면 캡처 실패로 기록하고 fallback 판단(11.5절)으로 간다.
- **filter 안에서 끝낸다.** `EXCEPTION_EXECUTE_HANDLER`로 stack을 풀지 않는다. 깨진 stack을 푸는 일 자체가 다시 실패할 수 있다. 실패 스레드의 stack은 캡처가 끝날 때까지 filter 안에 그대로 있다.
- **T가 하는 일은 고정 크기 복사, 원자 연산, `SetEvent`, `WaitForSingleObject`, `TerminateProcess`뿐이다.** 힙·서식화·사용자 공간 lock은 쓰지 않는다. stack 사용량은 수백 byte다.
- **엔진의 정상 종료를 돌리지 않는다.** `Engine::Shutdown`·GC·`LogHost::Stop`·정적 소멸·`atexit`가 돌지 않는다(`TerminateProcess`).
- **종료 코드.** 하드웨어 예외는 예외 코드(예: `0xC0000005`, `0xC00000FD`)다. Fatal·assert·abort·CRT hook은 Rocks 예외 코드다(D8).
- **도우미가 없을 때(fallback).** W가 다음 순서로 직접 한다. 결과에 `limitations: ["in-process"]`를 남긴다(11.5절).
  1. 종료 콜백(로그 flush)
  2. 폴더 만들기·예비 `CrashInfo.json`
  3. 프로세스 안 덤프
  4. 최종 `CrashInfo.json`
  - flush를 덤프보다 먼저 하는 이유가 있다. 다른 스레드가 쥔 heap·loader lock 때문에 덤프가 막히면 flush까지 잃기 때문이다.
  - 디스크 쓰기는 heap을 쓰므로 막힐 수 있다. 그래도 총 예산이 끝을 보장한다.

### 8.4 Fatal·assert(Windows)

```
Log::Fatal / ROCKS_ASSERT                    → 비상 출력, LogSystem 전달·flush(기존, 이 스레드),
                                               프로세스 표시 "Fatal flush 시도함"
  → platform::Fatal(message) / FatalAt(...)  → stderr 비상 출력, 정적 FatalRecord(종류·메시지·file·line·return address)
     ├ 수집기 미설치·Off                     → TerminateProcess(RocksFatalExceptionCode)   (raise 없음)
     ├ 디버거 있음(강제 옵션 없음)           → __debugbreak() → TerminateProcess(RocksFatalExceptionCode)
     └ [noinline] RaiseFatalException()
          __try { RaiseException(RocksFatalExceptionCode, NONCONTINUABLE, 1, {&FatalRecord}) }
          __except (CrashFilter(GetExceptionInformation())) { }   ← 8.3절과 같은 filter
       다음 줄: 무조건 TerminateProcess(RocksFatalExceptionCode)
```

- **보호 함수가 Fatal 안에 있다.** 바깥 보호 프레임이나 unhandled filter에 기대지 않는다. 다른 라이브러리가 filter를 바꿔도, 보호 없는 외부 스레드에서 불려도 같은 경로로 간다(UE `AbortHandler`의 모양).
- **`__try` 다음 줄은 무조건 종료다.** filter가 어떤 값을 돌려주든 실행이 Fatal 밖으로 이어지지 않는다. `[[noreturn]]` 함수가 반환하는 UB를 없앤다.
  - 정상 경로에서는 filter 안에서 이미 끝난다.
  - Embedded 모드는 8.8절에서 따로 다룬다.
- **ASan 구성.** Fatal 예외 코드는 하드웨어 예외가 아니다. 그래서 filter가 소유권을 얻고 공통 경로를 탄다(덤프 제외, 13.2절).
- **발생 위치.** 실패 스레드의 덤프 stack은 `RaiseException` → `RaiseFatalException` → `Fatal` → 호출자 순서로 남는다. 메타데이터와 덤프 사용자 stream은 다음을 따로 담는다(요구 8절).
  - `returnAddress`: `Fatal`의 `__builtin_return_address(0)`
  - assert의 `file`·`line`
  - 메시지(최대 2 KiB 복사)
- **Fatal 재귀.**
  - T 또는 W에서 Fatal이 불리면 `crashOwner`가 이미 서 있다. T는 비상 출력 뒤 `RecursiveCrash`로 즉시 끝낸다.
  - W는 `reporterFailed`를 남긴다. 종료 콜백 단계에서 실패했으면 남은 콜백만 포기하고 파일 단계(메타데이터·덤프)를 이어 간 뒤, 파일 단계에서 실패했으면 바로 `reportFinished`를 알리고 멈춘다(종료는 T가 한다, LC1b).
  - `Log::Fatal`의 스레드별 `fatalInProgress`는 로그 재귀만 막는다. 프로세스 전역 중복은 `crashOwner`가 막는다(요구 7절).

### 8.5 abort·CRT 치명 경로(Windows)

세 hook은 **반환하지 않는다**. 동작은 모두 같다.

| `crashOwner` 상태 | hook 동작 |
|---|---|
| 0(보고 없음) | 해당 kind로 `FatalRecord`를 채우고 `RaiseFatalException`. 다음 줄은 무조건 종료 |
| 자기 스레드 | 처리기 안 재귀다. 비상 출력 뒤 `TerminateProcess(RecursiveCrash)` |
| 다른 스레드 | 겹친 크래시와 같다(10.2절). 칸을 기록하고 소유자의 종료를 기다린다 |
| `ShutdownOwner`(정상 종료 중) | 비상 출력 뒤 `TerminateProcess(RocksFatalExceptionCode)` |

| 경로 | 거는 것 | 비고 |
|---|---|---|
| `abort()`(CRT·외부 라이브러리, 외부 스레드 포함) | `signal(SIGABRT, handler)`, `_set_abort_behavior(0, _WRITE_ABORT_MSG \| _CALL_REPORTFAULT)` | 근거는 UCRT 소스(Windows Kits 10.0.22621.0·10.0.19041.0 같음)와 시제품 실험이다.<br>- `raise`는 처리기를 부르기 전에 그 신호를 `SIG_DFL`로 되돌린다(`ucrt/misc/signal.cpp` 506–520). 실험에서 처리기가 다시 걸 때 이전 값이 `SIG_DFL`이었다.<br>- abort는 처리기가 있으면 `raise`하고, 처리기가 반환하면 `_CALL_REPORTFAULT`일 때 `__fastfail`, 아니면 `_exit(3)`이다(`ucrt/startup/abort.cpp` 61–89). 실험에서 종료 코드가 3이었다.<br>그래서 처리기는 첫 줄에서 자신을 다시 걸고, 반환하지 않는다. `_CALL_REPORTFAULT`는 끈다(프로세스 CRT 설정).<br>**한계:** SIGABRT 동작 칸은 프로세스 전역 하나다(`signal.cpp` 26). 처리기가 다시 걸기 전의 짧은 창에서 다른 스레드가 abort하면, 그 스레드는 `SIG_DFL`을 보고 보고 없이 `_exit(3)`으로 프로세스를 끝낸다. 종료 코드 3은 `0xC…`가 아니라 도우미도 기록하지 않는다 |
| invalid parameter | `_set_invalid_parameter_handler` | `kind=RuntimeCheck`. Debug CRT는 식·파일·줄을 받는다. 한계: 어떤 스레드가 `_set_thread_local_invalid_parameter_handler`로 덮으면 그 스레드에서는 불리지 않는다 |
| pure virtual call | `_set_purecall_handler` | `kind=RuntimeCheck` |
| `std::terminate` | 없음 | C++ 예외를 쓰지 않으므로 경로가 없다. 생기면 abort로 들어온다 |
| `_CrtDbgReport`(Debug CRT assert) | 대화상자 억제가 켜졌을 때만 `_CrtSetReportMode` → stderr | 그 뒤 CRT가 abort하면 위 경로 |

- 설치 때 이전 처리기를 보관하고 제거 때 되돌린다.
- **abort가 SEH로 들어온다고 가정하지 않는다(요구 7절).** abort는 SIGABRT 처리기로만 들어오고, 처리기가 명시적으로 공통 경로를 부른다.

### 8.6 스레드

- **`platform::Thread`.** `RunThread`가 `entry(context)`를 `RunWithCrashGuard`로 감싼다. Windows는 그 스레드 진입의 SEH 경계다(UE `GuardedRun`).
  - `ApplyCurrentThreadOptions`가 `PrepareThreadForCrashHandling`을 부른다.
  - 끝에서 `ReleaseThreadCrashHandling`을 부른다.
- **외부 스레드.** 드라이버·외부 라이브러리 스레드는 최종 unhandled filter가 받는다. stack 보장이 없으므로 그 스레드의 stack overflow는 filter가 넘칠 수 있다(15절 한계).
- **다른 filter와의 경쟁.** 다른 코드가 나중에 `SetUnhandledExceptionFilter`로 바꾸면 외부 스레드 보호를 잃는다.
  - Rocks 스레드와 주 스레드는 보호 프레임이 따로 있어 영향받지 않는다.
  - 현재 filter를 읽는 API는 없다. 그래서 진단할 때는 엔진 초기화 끝과 종료 단계에서 교환으로만 확인한다. `previous = SetUnhandledExceptionFilter(ours)`로 바꿔 보고, `previous`가 우리 것이 아니면 **즉시 `previous`로 복원**하고 Warning을 남긴다. 남의 선택을 조용히 빼앗지 않는다.
- **힙 손상(선택 기능).** `STATUS_HEAP_CORRUPTION`(0xC0000374)만 처리하는 vectored exception handler를 둔다.
  - 기본은 끈다. `ApplicationSettings::captureHeapCorruption`으로 켠다.
  - 힙 손상은 VEH·WER에서만 보이고 일반 filter에 오지 않는다(UE `WCC` 1797–1803 주석).
  - 이 처리기는 같은 소유권·W·도우미 경로를 쓴다. 다른 예외 코드는 건드리지 않고 `EXCEPTION_CONTINUE_SEARCH`를 돌려준다.

### 8.7 POSIX(macOS·Android)

- **처리기.** SIGSEGV·SIGBUS·SIGILL·SIGFPE·SIGABRT·SIGTRAP에 `SA_SIGINFO|SA_ONSTACK`로 건다. 처리 중 같은 신호는 막고(`sa_mask`), 설치 때 이전 동작을 보관한다.
- **처리 순서.**
  1. `crashOwner` CAS
  2. 신호 번호와 `si_addr`를 정적 기록에 남김(`ucontext` 레지스터는 복사하지 않는다)
  3. 미리 만든 pipe에 1 byte를 `write`해 W를 깨움(macOS는 이름 없는 세마포어 `sem_init`이 ENOSYS라 쓰지 않는다)
  4. `reportFinished` 원자 표시를 단조 시계로 제한 시간까지 확인
- **이전 동작 복원(B8).** 처리를 마친 뒤 신호 출처에 따라 다르게 넘긴다.
  - **하드웨어 fault**(SIGSEGV·SIGBUS·SIGILL·SIGFPE, 보낸 신호가 아닌 것): 보관한 이전 동작을 복원하고 처리기에서 **반환**한다. 같은 명령이 다시 fault하면서 원래 siginfo로 이전 처리기(Android debuggerd·macOS ReportCrash, 없으면 기본 동작)에 전달된다.
  - **보낸 신호**(kill·tgkill·sigqueue·raise·abort): 이전 동작을 복원하고 `pthread_kill`(지금 스레드)로 다시 낸다. 판정은 `IsSentSignal`이다. Linux·Android는 `si_code <= 0`(`SI_USER` 0, `SI_QUEUE` -1, `SI_TKILL` -6 등)이다. macOS(XNU `bsd/sys/signal.h`)는 `SI_USER` 0x10001부터 `SI_MESGQ` 0x10005까지 양수 범위를 쓰므로 그 범위도 보낸 신호로 본다(LC7 리뷰 12, macOS는 실행 확인 없음).
  - **abort의 SIGABRT**: libc abort가 보낸 신호(Linux·Android `SI_TKILL`)이므로 위와 같이 이전 동작을 복원하고 `pthread_kill`로 다시 낸다.
- **Fatal.** `RaiseException`이 없으므로 직접 소유권을 얻고 기록한 뒤 W를 깨워 기다린다. 그다음 **보관한 이전 SIGABRT 동작을 복원**하고 `abort()`한다. 그래서 Android debuggerd tombstone이 남는다. 처리기는 이미 소유권이 있음을 보고 다시 처리하지 않는다.
- **W 깨우기.** self-pipe에 1 byte를 쓴다. 양쪽 `FD_CLOEXEC`, 쓰는 쪽 `O_NONBLOCK`(처리기가 막히지 않는다, 가득 차면 이미 깨울 byte가 있다), `EINTR`이면 다시 쓴다. 설정이 하나라도 실패하면 설치는 `OutOfResources`다(origin `8749b85b`가 LG18 처리기에 넣은 방식과 같다, 이식 때 맞춤).
- **W.** 종료 콜백과 메타데이터(`open`·`write`)만 한다. 도우미가 없으므로 디스크 쓰기는 W가 한다(한계 표시). 덤프는 `Unsupported`다.
- **async-signal-safe 범위.** 처리기 쪽은 원자 연산, `write`, `clock_gettime`, `sched_yield`, `sigaction`, `kill`, `pthread_kill`, `pause`만 쓴다.
- **겹친 스레드.** 소유자의 완료를 보고 돌아가지 않고 소유자 마감과 여유까지 계속 기다린다. 그래도 끝나지 않으면 소유자의 신호(Fatal 소유자는 SIGABRT)의 이전 동작을 되돌리고 `kill`로 그 신호를 낸다. 소유자보다 먼저 다른 신호로 프로세스를 끝내지 않는다.
- **Asan 구성(LC7).** 하드웨어 fault 신호의 이전 동작이 처리기면(SIG_DFL·SIG_IGN이 아니면) 보고하지 않고 넘긴다. 그 처리기가 ASan 것인지는 확인하지 않는다. ASan 보고 뒤 death callback이 Fatal 경로로 기록한다. 크래시 처리 스레드 자신의 fault는 넘기기 전에 본다(13.2절).
- **SIGTRAP.** x86 `int3`은 명령 뒤에서 멈추므로 반환하면 다시 나지 않는다. si_code와 관계없이 보낸 신호처럼 다시 낸다. SIGABRT의 종료 사유는 `Abort`다. W 쪽은 신호 문맥 밖이지만 깨진 스레드가 쥔 lock은 TryLock 실패로 건너뛴다(최선 노력).

### 8.8 내장(Embedded) 모드

Rocks가 다른 호스트 프로세스 안에 라이브러리로 들어간 경우다(요구 4절). **Rocks는 호스트 프로세스를 끝내지 않는다.** 종료는 호스트가 정한다.

| 항목 | Full·InProcess | Embedded |
|---|---|---|
| unhandled filter·SIGABRT·CRT hook·신호 처리기 | 건다 | 걸지 않는다(호스트 소유) |
| Rocks 보호 프레임(`RunWithCrashGuard`·`platform::Thread`) | 보고 뒤 filter 안에서 `TerminateProcess` | 보고(종료 콜백·메타데이터) 뒤 `EXCEPTION_CONTINUE_SEARCH`로 호스트 처리기에 전파 |
| Fatal·assert | 보고 뒤 종료 | 보고 뒤 `RaiseException`을 그대로 전파(우리 filter는 `CONTINUE_SEARCH`). 호스트가 처리하지 않으면 OS 기본(호스트의 unhandled filter·WER). 비연속 예외라 실행이 돌아올 수 없다. 돌아오면 `STATUS_NONCONTINUABLE_EXCEPTION`이 난다 |
| 도우미·덤프 | 있음 | 띄우지 않는다. 덤프는 호스트의 크래시 수집이 맡는다 |
| 메타데이터 | 도우미가 디스크에 쓴다 | W가 쓴다(heap 사용 한계 표시). 호스트가 메타데이터 폴더를 정한다 |
| 종료 콜백(로그 flush) | W, 공용 마감 | W, 공용 마감. 끝난 뒤 실패 스레드가 전파한다 |
| 겹친 크래시 | 소유자의 종료를 기다림 | 소유자의 보고 완료를 기다린 뒤 각자 전파 |
| 재귀 | 즉시 종료 | 보고 없이 즉시 `CONTINUE_SEARCH`. 재귀·정상 종료 중의 Fatal도 `TerminateProcess` 대신 `RaiseFatalException`으로 호스트에 넘긴다 |
| 보고 뒤 호스트가 실행을 이어 감 | 해당 없음(프로세스가 끝난다) | 콜백이 돌아온 칸은 등록 상태로 돌아가므로 `UnregisterTerminationCallback`이 기다리지 않는다. 처리 스레드가 보고를 마쳤으면(`ReportCompleted`) `RemoveCrashReporting`도 기다리지 않고 정리한다. 소유권은 그대로 남아 이후 실패는 보고 없이 호스트로 넘어간다. 콜백 안에서 처리 스레드가 실패했으면 그 칸은 실행 중으로 남고, 그 칸의 해제는 돌아오지 않는다(user 객체 보호) |
| 종료 코드 | 예외 코드 | 호스트가 정한다 |

## 9. 보호 범위(요구 10절)

| 구간 | 보호 | 수집되는 것 | 외부 경로 |
|---|---|---|---|
| `main` 이전 정적 초기화(CRT 초기화, 전역 생성자) | **없음.** `wmain`에서 설치하는 처리기는 이 구간을 보호하지 않는다 | 없음 | WER(사용자가 LocalDumps를 직접 설정한 경우), 디버거. Rocks 코드는 `constinit` 위주라 이 구간이 짧다 |
| 진입 후 설치 전(`RunApplication`의 첫 몇 줄, 도우미 분기) | 없음 | 없음 | 위와 같다 |
| 설치 후 명령줄 캡처·설정 적용(1–7단계) | OS 처리기, 주 스레드 보호 프레임 | 덤프는 InProcess 수준(도우미는 5단계부터), 메타데이터 단계 `Startup` | — |
| 본문 실행(설정·LogHost·Engine 초기화, 엔진 루프, 엔진 종료) | 전부(도우미 포함) | 덤프·메타데이터·로그 flush | — |
| 정상 종료 처리(9단계 정리, `RemoveCrashReporting` 직전까지) | 전부. 도우미는 `Shutdown` 기록 전까지 | 단계 `Shutdown` | — |
| `RemoveCrashReporting` 뒤, `main` 반환 후 정적 소멸·`atexit`·CRT 종료 | **없음** | 없음 | WER |
| fast-fail(`__fastfail`, `/GS` 실패 `STATUS_STACK_BUFFER_OVERRUN`, CFG, 처리기를 켜지 않은 힙 손상 종료) | SEH를 우회한다. 처리기가 불리지 않는다 | 도우미가 살아 있으면 `kind=UnreportedExit`(종료 코드만, 덤프 없음, 11.4절 조건) | WER. `WerSetFlags(WER_FAULT_REPORTING_NO_UI)`는 프로세스 설정으로 대화상자만 끈다 |
| 사용자 중단(Ctrl-C·Ctrl-Break·콘솔 닫기) | 처리기 없음(Launch 종료 요청을 켠 프로그램은 정상 종료) | 기록하지 않는다(`STATUS_CONTROL_C_EXIT`는 제외 조건) | — |
| 외부 강제 종료(`TerminateProcess`, 작업 관리자, harness kill, 부모 job 종료) | 없음 | 기록하지 않는다. 종료 코드는 호출자가 정한 값이다. .NET `Process.Kill`의 `0xFFFFFFFF`와 작업 관리자의 1은 `0xC…`가 아니다. 같은 job이면 도우미도 함께 끝난다 | — |
| 디버거 연결 중 크래시 | 기본은 디버거에 넘긴다(13절) | 강제 옵션일 때만 | 디버거 |

- **외부 수집 연동.** `HKLM\...\Windows Error Reporting\LocalDumps`와 `WerRegisterRuntimeExceptionModule`(HKLM·HKCU 레지스트리 등록 필요)은 쓰지 않는다. 사용자 시스템의 전역 설정을 바꾸지 않는다(요구 10절). 14.4절에는 사용자가 직접 켜는 절차만 적는다.
- **수집기를 설치하지 않은 프로세스.** 크래시 때 로그 flush도 없다. Rocks 실행 파일은 모두 Launch를 거치므로 이런 프로세스로 남는 것은 C# 도구와 외부 호스트뿐이다.

## 10. 문맥 저장소·보고 소유권·제한 시간

### 10.1 미리 준비하는 것(설치 때)

| 대상 | 크기·종류 | 비고 |
|---|---|---|
| 크래시 기록 | `constinit` 정적 구조체 약 8 KiB | 다음을 담는다.<br>- 프로그램 이름(64 B), 크래시 id(128-bit, 설치 때 `SecureRandom`), pid, 프로세스 시작 UTC<br>- 빌드 식별 블록(1 KiB)<br>- 단계·단계 이름 포인터(원자 변수)<br>- 연관 파일 칸 5개(각 512 B와 순번)<br>- 출력 폴더(UTF-16 1024 단위와 UTF-8 사본)<br>- 크래시로 멈춘 스레드 id 칸 8개 |
| 실패 문맥 | `alignas(16)` CONTEXT 사본, EXCEPTION_RECORD 사본(연쇄 포인터는 끊는다), 정적 EXCEPTION_POINTERS | 첫 소유자만 쓴다. CONTEXT 사본의 `ContextFlags`에서 `CONTEXT_XSTATE` 비트를 지운다. 확장 영역 없이 복사한 사본이 그 비트를 갖고 있으면 DbgHelp·디버거가 없는 확장 영역을 읽는다 |
| Fatal 기록 | kind, 메시지 2 KiB, 파일 포인터, 줄, return address | Fatal·assert·abort·CRT hook이 채운다 |
| 실패 스레드 정보 | 스레드 id, 이름 사본 64 B(`platform::Thread` 이름이면 thread_local 상태에서 복사) | — |
| W의 서식 버퍼 | 16 KiB | 메타데이터 조각(JSON)을 고정 버퍼 함수로만 만든다. `FormatTo`·할당을 쓰지 않는다 |
| 동기화 | Windows는 `crashRequested`(자동 리셋)·`reportFinished`(수동 리셋) 이벤트. POSIX는 pipe 하나(신호 처리기가 `write`, 처리 스레드가 `read`, `FD_CLOEXEC`)와 원자 표시. macOS는 `sem_init`이 ENOSYS라 세마포어를 쓰지 않는다 | 사용자 공간 lock 없음 |
| 크래시 처리 스레드 W | stack 256 KiB 예약(`STACK_SIZE_PARAM_IS_A_RESERVATION`). `CreateThread`로 직접 만든다 | 프로세스 안 덤프(DbgHelp)에 쓸 여유다. `platform::Thread`를 쓰지 않는다. W의 실패는 filter의 W 분기가 받는다(10.2절) |
| DbgHelp | `LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)`과 `MiniDumpWriteDump` 주소 | fallback용이다. 크래시 뒤에는 loader lock 때문에 로드하지 않는다 |
| fallback 폴더 | `%TEMP%\Rocks\Crashes`를 미리 절대 경로로 만든다(폴더 자체는 만들지 않는다) | 14.1절 |
| 도우미 교환 블록 | 공유 메모리 64 KiB(Full) | 11.3절 |

정적 저장소 합계는 약 40 KiB(BSS)다. 정상 실행 중에는 건드리지 않는다.

**W가 하는 일은 heap을 쓰지 않는 일뿐이다(B1).**
- 정적 버퍼 서식
- 교환 블록 쓰기
- 이벤트 신호
- 종료 콜백 호출

폴더 만들기·`CreateFileW`·덤프는 도우미가 한다. Win32 경로 API의 NT 경로 변환이 프로세스 heap을 할당하고, 크래시한 스레드가 그 heap lock을 쥐고 있을 수 있기 때문이다. W가 디스크에 직접 쓰는 경우는 도우미가 없을 때(fallback·InProcess·Embedded·POSIX)뿐이고, 결과에 그 한계를 남긴다.

### 10.2 보고 소유권

- **`crashOwner`(uint64).** 값은 셋 중 하나다.
  - 0: 없음
  - 실패 스레드 id
  - `ShutdownOwner` sentinel(정상 종료 중)

  첫 실패 스레드가 CAS로 자기 id를 넣는다. 이것이 프로세스 전체에서 단 하나의 보고 소유권이다(요구 7절). 스레드별 `fatalInProgress`는 이것을 대신하지 않는다.
- **`reportState`.** `Idle → Capturing → Reporting → Finished`. W가 옮기고 `Finished`에서 `reportFinished`를 알린다.
- **소유자의 종료 정보.** 소유자 T는 소유권을 얻자마자 자기 종료 코드와 마감 시각(단조 시계)을 정적 저장소에 쓴다.
- **겹친 크래시(B4).** 소유자가 아닌 실패 스레드는 스스로 끝내지 않는다.
  1. `concurrentCrashes`를 올린다. 처음 8개는 스레드 id와 예외 코드를 칸에 남긴다. 이 칸은 종료 콜백에 "크래시로 멈춘 스레드 목록"으로 넘긴다(19.3절).
  2. `reportFinished`와 소유자의 종료를 계속 기다린다. 소유자가 `TerminateProcess`를 부르면 이 스레드도 함께 끝난다.
  3. 소유자의 마감에 여유(5 s)를 더한 시각을 넘긴 경우에만 직접 끝낸다. 이때 쓰는 종료 코드는 정적 저장소에 있는 **소유자의 코드**다. 소유자가 넘친 stack 등으로 대기 중에 멈춘 경우를 위한 것이다.

  두 번째 덤프는 쓰지 않고, 재시도도 하지 않는다.
- **같은 스레드 재귀.** 소유자 스레드가 다시 filter에 들어오면 처리기 안 실패다. `WriteDiagnostic`으로 한 줄 남기고 `TerminateProcess(RecursiveCrashExitCode)`로 끝낸다.
- **W의 실패(filter 공통 분기).** filter·CRT hook·Fatal은 맨 앞에서 "현재 스레드가 W인가"를 본다.
  - W면 `reporterFailed`를 표시한다. 종료 콜백 단계였으면 남은 콜백만 포기하고 파일 단계를 이어 간다(프로세스 안 덤프는 콜백 뒤에 쓰므로, 멈추면 그 크래시의 덤프가 사라진다). 그다음 `reportFinished`를 알린 뒤 멈춘다(무한 대기). 종료는 기다리던 T가 한다.
  - 일반 보고 경로로 다시 들어가지 않는다.
- **정상 종료와의 경합(B11).** `RemoveCrashReporting`은 먼저 `crashOwner`를 `ShutdownOwner`로 CAS한다.
  - 실패하면 보고가 진행 중이다. 그때는 돌아오지 않고 기다리며, 종료는 T가 한다.
  - 성공한 뒤에 들어온 실패는 8.5절 표의 `ShutdownOwner` 행처럼 비상 출력 뒤 끝난다(보고 없음).

### 10.3 실패 스레드와 덤프 작성 스레드

- 덤프의 `MINIDUMP_EXCEPTION_INFORMATION.ThreadId`는 **실패 스레드 id**다. W나 도우미 스레드가 아니다.
- 메타데이터에는 `failingThreadId`와 `reporterThreadId`(W)를 따로 적는다.
- 스레드 핸들은 넘기지 않는다. UE의 의사 핸들 혼동을 피한다.

### 10.4 `GetExceptionInformation` 포인터의 수명 계약

- filter가 받은 포인터는 그 filter가 돌아오기 전까지만 유효하다.
- 크래시 스레드가 다른 스레드·프로세스에 넘기는 것은 다음 둘뿐이다. 원본 포인터는 넘기지 않는다.
  - 받은 즉시 정적 저장소로 복사한 EXCEPTION_RECORD·CONTEXT
  - 그 사본을 가리키는 정적 EXCEPTION_POINTERS
- 도우미는 snapshot(VA clone) 안의 같은 정적 주소를 `ClientPointers=TRUE`로 읽는다. clone은 캡처 시점 메모리의 사본이다. 그래서 부모가 끝난 뒤에도 유효하다.
- **한계.** CONTEXT 사본은 `CONTEXT_ALL`만 담고 XSTATE(AVX 상위 레지스터)는 담지 않는다. snapshot의 스레드 문맥(`PSS_CAPTURE_THREAD_CONTEXT`)에는 그 스레드의 filter 프레임 문맥이 따로 들어간다. `.ecxr`는 사본 문맥을 쓴다.

### 10.5 stack overflow

- 주 스레드(설치 때)와 모든 `platform::Thread`(시작 때)가 `SetThreadStackGuarantee(32 KiB)`를 부른다. 보장분은 그 스레드에만 적용된다(요구 9절). 이미 더 크면 줄이지 않는다.
- 크래시 스레드는 복사·신호·대기만 하므로 보장분 안에서 끝난다. 덤프는 도우미(또는 W)가 쓴다. 실패 스레드의 남은 stack에 기대지 않는다.
- 외부 스레드에는 보장분이 없다. filter가 넘치면 OS가 프로세스를 `0xC00000FD` 등으로 끝낸다. 이때는 도우미의 `UnreportedExit` 기록만 남는다(15절 한계).

### 10.6 제한 시간(기본값, D24)

| 구간 | 기본 | 넘으면 |
|---|---|---|
| 부모: 도우미 캡처 끝 대기(`captureTimeoutMilliseconds`) | 10 s | `dump.status = "CaptureTimedOut"`. 남은 총 예산 안에서 fallback(11.5절) |
| 부모: 종료 콜백(로그 flush, 공용 마감) | 2 s | 남은 콜백은 `Skipped`. 막힌 콜백도 총 예산이 끝을 보장한다 |
| 부모: 실패 스레드의 총 대기(도우미 있음) | 캡처 + 콜백 + 1 s = 13 s | `TerminateProcess` |
| 부모: 총 대기(fallback, 프로세스 안 덤프) | 콜백 2 s + 덤프 30 s + 메타데이터 2 s + 1 s = 35 s | `TerminateProcess` |
| 겹친 실패 스레드 | 소유자 마감 + 5 s | 소유자의 종료 코드로 `TerminateProcess` |
| 도우미: 기본 덤프(`Minidump.dmp`, 늘 Standard) | 60 s | CancelCallback으로 취소한다. 취소된 덤프는 열 수 없으므로 지우고 `dump.status = "Cancelled"`로 적는다 |
| 도우미: 추가 덤프(`Minidump.detail.dmp`, Detailed·Full일 때만) | Detailed 120 s, Full 600 s(설정 가능) | 같다. 기본 덤프는 이미 있다 |
| 도우미: 부모 콜백 결과 대기(`parentDone`) | 부모 총 대기와 같음 | 부모가 먼저 끝나면 `terminationCallbacks.status = "Unknown"` |
| 정상 종료 때 도우미 종료 대기 | 2 s | 도우미 `TerminateProcess` |

일반 `LogSystem::Flush(timeout)`는 쓰지 않는다. 그 경로는 lock 획득, sink 호출, writer 깨우기를 거치므로 timeout 인자가 있어도 크래시 안전하지 않다(요구 9절). 크래시 경로는 종료 콜백(`FlushForTermination`)만 부른다(19절).

## 11. 덤프 도우미 프로세스와 fallback(Windows)

### 11.1 준비 시점

- Launch 시작 5단계(`configure` 뒤, 본문 전)에서 띄운다. `Full`일 때만이다.
- 크래시 뒤에는 프로세스를 만들지 않는다. heap·loader lock 상태에 기대게 되기 때문이다(요구 8절).
- 시작 때 도우미 준비를 기다리지 않는다. 크래시 때 준비 상태와 프로세스 생존을 확인한다. 그래서 시작 지연이 생기지 않는다.

### 11.2 생성과 권한

- **실행 파일과 인자.** 실행 파일은 자기 자신이다(`GetModuleFileNameW`). 인자는 `--rocks-crash-helper <프로토콜 버전> <핸들 값 7개>`다. `ApplicationSettings::crashHelperPath`로 다른 실행 파일을 지정할 수 있다(배포 구성·시험 용도).
- **생성 방식.** `CreateProcessW`에 `STARTUPINFOEXW`와 `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`를 써서 넘길 핸들만 상속한다.
  - 넘기는 핸들: 교환 블록 section, 부모 프로세스 핸들, 이벤트 5개(request·captureDone·parentDone·ready·shutdown)
  - 표준 입출력은 상속하지 않는다. 부모 stdout pipe를 읽는 harness가 EOF를 못 받는 일을 막는다.
  - `DETACHED_PROCESS`로 콘솔을 붙이지 않는다. 그래서 Ctrl-C가 도우미를 먼저 죽이지 않는다.
- **상속 끄기(B2).** 생성 직후 부모는 위 핸들 모두에 `SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0)`을 부른다. 이후 다른 코드가 상속 켠 자식 프로세스를 만들어도 이 핸들이 새지 않는다.
- **부모 프로세스 핸들 권한.** `DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), ...)`로 만든다. 도우미는 `OpenProcess`를 하지 않는다. 권한은 다음과 같다.

| 권한 | 쓰는 곳 |
|---|---|
| `PROCESS_CREATE_PROCESS` | `PssCaptureSnapshot`의 `PSS_CAPTURE_VA_CLONE`(clone 프로세스 생성). 없으면 캡처가 `ERROR_ACCESS_DENIED` |
| `PROCESS_QUERY_INFORMATION` | 캡처. `PROCESS_QUERY_LIMITED_INFORMATION`으로는 `ERROR_ACCESS_DENIED` |
| `SYNCHRONIZE` | 도우미의 부모 종료 대기. 없으면 대기가 실패해 프로토콜이 서지 않는다 |
| `PROCESS_DUP_HANDLE` | `Detailed`·`Full`의 핸들 캡처(`PSS_CAPTURE_HANDLES`)일 때만. 없으면 그 캡처가 `ERROR_ACCESS_DENIED` |

  - **최소 조합(시제품 확인).** Standard는 `PROCESS_CREATE_PROCESS | PROCESS_QUERY_INFORMATION | SYNCHRONIZE`(0x100480)다. `PROCESS_VM_READ`는 필요 없다(덤프는 clone을 읽는다). Detailed·Full은 여기에 `PROCESS_DUP_HANDLE`을 더한다.
  - UE CRC의 `PROCESS_ALL_ACCESS`보다 좁다.
  - `PROCESS_CREATE_PROCESS`는 자기 프로세스 이름으로 자식을 만들 수 있는 권한이다. 같은 사용자 도우미에게만 주고, 생성 뒤 상속을 끈다.
- **구현 위치.** Win64 private 코드가 직접 만든다. 일반 `platform::Process`에는 핸들 상속 목록이 없고, 공개 API에 HANDLE을 노출하지 않으려고 크래시 전용 private 경로로 둔다. 인용 규칙은 `Process` 구현의 Win64 private 함수를 재사용한다.

### 11.3 교환 블록(공유 메모리 64 KiB)

| 칸 | 쓰는 쪽 | 내용 |
|---|---|---|
| 머리 | 부모(설치) | magic, 프로토콜 버전, 블록 크기, 부모 pid, 도우미 pid |
| 상태 | 양쪽 | 원자 uint32. 값은 `Ready`(도우미), `CrashRequested`(W), `Captured`(도우미), `ParentDone`(W), `DumpDone`(도우미), `Withdrawn`(W: 캡처 시간 초과로 요청을 거둠), `Shutdown`(부모) |
| 고정 정보 | 부모(설치·폴더 변경) | 예정 크래시 폴더 경로(UTF-16 1024 단위), fallback 폴더, 프로그램 이름, 빌드 식별 블록, 크래시 id, 덤프 종류·마감 |
| 크래시 요청 | W | 실패 스레드 id, snapshot 안 정적 EXCEPTION_POINTERS 주소(uint64), 정적 크래시 기록·Fatal 기록 주소와 크기(메모리 포함용), 메타데이터 조각(W가 정적 버퍼로 서식한 JSON, 최대 16 KiB), 크래시 기록 사본(덤프 사용자 stream용) |
| 부모 결과 | W | 종료 콜백 결과·걸린 시간, `reporterFailed` |
| 도우미 결과 | 도우미 | 캡처 결과(Win32 오류), 기본·추가 덤프 상태(`Written`·`Cancelled`·`Failed`), 오류 코드, 걸린 시간 |

### 11.4 도우미 루프

```
준비: 교환 블록 검사(머리·버전·크기, GetProcessId(parent) == parentPid, 덤프 종류 ≤ Full, 마감 ≤ 상한)와
      고정 값 사본, 상태=Ready
반복: WaitForMultipleObjects({request, shutdown, parentProcess})
  request →
      상태 == CrashRequested 확인(아니면 아무것도 쓰지 않고 끝낸다)
      요청 사본(크래시 기록·경로·범위)을 지역 칸으로 옮기고 글자 칸 NUL·길이 칸을 배열 크기로 고친다
      PssCaptureSnapshot(parent, VA_CLONE | THREADS | THREAD_CONTEXT | CREATE_BREAKAWAY_OPTIONAL
                         [| HANDLES ...], CONTEXT_ALL)
        (ERROR_INVALID_PARAMETER면 CREATE_BREAKAWAY_OPTIONAL 없이 다시)
      CAS(CrashRequested → Captured). 지면(부모가 거둠) snapshot을 버리고 파일을 하나도 쓰지 않는다
      폴더 만들기, 예비 CrashInfo.json(조각 + "complete": false), Minidump.dmp를 삭제 예약으로 연다
        (폴더·파일이 실패하면 fallback 폴더로 옮기고 primary의 예비본은 지운다) → 폴더 칸 확정
      SetEvent(captureDone)        ← 부모는 이제 끝나도 된다(폴더 칸은 더 바뀌지 않는다)
      기본 덤프: MiniDumpWriteDump(snapshot 핸들, parentPid, Minidump.dmp, Standard 종류,
                        {failingThreadId, 정적 EXCEPTION_POINTERS 주소, ClientPointers=TRUE},
                        user streams(Rocks 크래시 기록·Fatal 기록·CommentStreamA 요약),
                        callback{IsProcessSnapshotCallback → S_FALSE,
                                 IncludeVmRegion/MemoryCallback → 정적 기록 범위 포함,
                                 CancelCallback → 기본 마감을 넘으면 Cancel})
      SetEvent(standardDump)       ← job 안 도우미를 기다리는 부모가 본다
      Detailed·Full이면 추가 덤프: 같은 snapshot으로 Minidump.detail.dmp(그 종류, 그 마감)
      취소·실패한 파일은 지운다(취소된 덤프는 cdb가 열지 못한다 — 24절)
      PssFreeSnapshot
      parentDone 또는 parentProcess 종료 대기(부모 총 대기만큼)
      최종 CrashInfo.json(조각 + 덤프 결과 + 부모 결과), 상태=DumpDone → 종료 코드 0
  shutdown → 종료 코드 0
  parentProcess 종료(요청·shutdown 없이) →
      GetExitCodeProcess = code
      (code & 0xF0000000) == 0xC0000000 이고 code != STATUS_CONTROL_C_EXIT(0xC000013A)이면
          예정 폴더에 CrashInfo.json { kind: "UnreportedExit", exitCode, phase: 알 수 없음 }
      아니면 아무것도 쓰지 않는다
      종료 코드 0
```

- **기록 조건을 좁힌 이유(A 결정).** 정상 `exit`·`_Exit`(임의 코드), Ctrl-C·콘솔 닫기(`STATUS_CONTROL_C_EXIT`), harness kill(.NET `0xFFFFFFFF`), 작업 관리자(1)를 크래시로 기록하지 않기 위해서다. 남는 것은 fast-fail(`0xC0000409`), 보장 없는 스레드의 stack overflow(`0xC00000FD`), 처리기 밖 접근 위반 같은 NTSTATUS 오류다. 기록은 `kind=UnreportedExit`이라 처리기가 보고한 크래시와 구분된다.
- **순서의 이유(LC2 리뷰 반영).** 캡처와 소유 CAS를 파일 I/O보다 먼저 한다. 출력 폴더 I/O가 캡처 마감을 써 버리지 않고, 부모가 마감을 넘겨 요청을 거둔 뒤 늦게 깨어난 도우미가 부모가 쓴 최종본을 덮지 않는다. 폴더 확정(덤프 파일 열기)은 captureDone 앞이다. 부모의 `Rocks crash report:` 줄이 실제 폴더와 같다. CAS 뒤의 I/O가 캡처 마감을 넘겨도 부모는 거두기 CAS에서 Captured를 보고 도우미 결과를 쓴다.
- **job.** 도우미는 `CREATE_BREAKAWAY_FROM_JOB`으로 먼저 띄우고 `ERROR_ACCESS_DENIED`면 플래그 없이 띄운다. 그래도 job 안이면(`IsProcessInJob`) 결과 `helper.inJob = true`이고, 부모 W는 콜백 결과를 넘긴 뒤 기본 덤프 끝(standardDump 이벤트, 이전 ready 이벤트 자리) 또는 도우미 종료까지 기본 덤프 마감 안에서 기다리고(Standard면 도우미 종료까지 메타데이터 몫을 더 기다린다), 그만큼 부모 총 대기 예산에 더한다. job을 닫는 harness(KILL_ON_JOB_CLOSE)에서도 덤프가 남는다.
- **교환 블록 불신.** 부모는 블록을 채운 뒤 view를 `PAGE_READONLY`로 두고 크래시 요청·폴더 변경·정상 종료 때만 쓰기를 연다. 주 폴더는 칸 두 개와 index로 바꿔 끼운다(`SetCrashOutputDirectory`가 크래시와 겹쳐도 섞인 경로를 읽지 않는다). 핸들은 상속 불가로 만들고 CreateProcessW 바로 앞뒤에서만 상속을 켠다.
- **도우미 자체.** 단일 스레드다. Rocks LogSystem·설정을 쓰지 않고, 오류는 결과 칸과 파일로만 남긴다. 도우미가 크래시하면 부모가 도우미 프로세스 종료를 보고 fallback으로 간다.

### 11.5 fallback

| 상황 | 감지 | 처리 | 메타데이터 |
|---|---|---|---|
| `InProcess` 모드 | 설정 | W가 8.3절 fallback 순서(flush → 파일 → 프로세스 안 덤프 → 최종본)로 한다 | `dump.writer = "InProcess"`, `limitations: ["in-process"]` |
| `Embedded` 모드 | 설정 | 덤프 없음, W가 메타데이터 | 8.8절 |
| 도우미 시작 실패 | `CreateProcessW` 실패(시작 때) | InProcess와 같다 | `helper.status = "StartFailed"`와 Win32 오류 |
| 도우미가 크래시 전에 죽음 | 크래시 때 도우미 핸들이 이미 signaled | 같다 | `"ExitedEarly"`와 종료 코드 |
| 도우미가 캡처 전·중에 죽음 | `captureDone`·도우미 핸들 함께 대기 | 남은 총 예산 안에서 같다 | `"ExitedDuringCapture"` |
| 캡처 시간 초과 | `captureTimeout` | W가 상태를 `Withdrawn`으로 CAS한 뒤 남은 총 예산 안에서 같다. 도우미는 캡처를 마친 뒤 상태가 `Withdrawn`이면 덤프를 쓰지 않고 snapshot을 버린다(덤프가 둘 생기지 않는다) | `"CaptureTimedOut"` |
| 캡처 뒤 도우미가 죽거나 덤프가 실패함 | 부모는 이미 끝났을 수 있다 | fallback 하지 않는다(부모가 없다). 예비 `CrashInfo.json`이 남는다 | `"complete": false` |
| 덤프 마감 초과 | CancelCallback | 취소된 파일은 지운다. 추가 덤프가 취소돼도 기본 덤프는 남는다 | `dump.status`·`detailDump.status = "Cancelled"` |

- **프로세스 안 덤프의 한계를 결과에 표시한다.** 다른 스레드가 heap·loader lock을 쥔 채 멈춰 있으면 `MiniDumpWriteDump`와 파일 API가 막힐 수 있다. 예산이 끝을 보장한다.
- **외부 프로세스를 쓴다고 모든 크래시를 잡는다고 주장하지 않는다.** 도우미가 캡처하려면 부모의 처리기가 먼저 돌아야 한다. fast-fail·외부 강제 종료·정적 초기화 크래시는 9절 표대로 잡지 못한다.

### 11.6 DbgHelp 직렬화

- 덤프 작성은 도우미(단일 스레드)나 W(하나)만 한다. 지금 Rocks에는 다른 DbgHelp 사용처가 없다(3.6절).
- 나중에 stack walk·심볼화 기능이 생기면 PlatformBase 내부 lock 하나로 DbgHelp 호출을 직렬화한다. 크래시 경로는 그 lock을 TryLock으로만 잡고, 못 잡으면 프로세스 안 덤프를 건너뛴다(결과에 표시). DbgHelp는 스레드 안전하지 않다.

### 11.7 덤프 종류와 덤프 안 Rocks 정보(D13)

| `CrashDumpDetail` | 파일 | `MINIDUMP_TYPE` | snapshot 캡처 플래그 | 도우미 마감 |
|---|---|---|---|---|
| `Standard`(기본) | `Minidump.dmp`만 | `MiniDumpWithThreadInfo \| MiniDumpWithIndirectlyReferencedMemory \| MiniDumpWithUnloadedModules` | `PSS_CAPTURE_VA_CLONE \| THREADS \| THREAD_CONTEXT` | 60 s |
| `Detailed` | `Minidump.dmp` + `Minidump.detail.dmp` | 추가 덤프: + `MiniDumpWithDataSegs \| MiniDumpWithHandleData \| MiniDumpWithProcessThreadData \| MiniDumpWithFullMemoryInfo` | + `PSS_CAPTURE_HANDLES \| HANDLE_NAME_INFORMATION \| HANDLE_BASIC_INFORMATION \| HANDLE_TYPE_SPECIFIC_INFORMATION` | 120 s |
| `Full` | 같음 | 추가 덤프: Detailed + `MiniDumpWithFullMemory` | Detailed와 같음 | 600 s |

- **기본 덤프를 늘 먼저 쓴다(시제품 결과).** 취소된 `MiniDumpWriteDump`는 `ERROR_CANCELLED`로 끝나고, 남은 파일은 크기와 관계없이 cdb가 열지 못한다. 그래서 "잘린 덤프"는 쓸모가 없다. 빠른 Standard 덤프(소형 프로세스 약 13 ms)를 먼저 확보하고, 큰 덤프는 같은 snapshot에서 별도 파일로 쓴다.
- **종류와 캡처 플래그를 맞춘다.** `MiniDumpWithHandleData`를 핸들 캡처 없는 snapshot에 쓰면 `ERROR_NOT_FOUND`(0x80070490)로 실패한다.

- **`Standard`의 범위.** UE 기본과 같은 계열이다. PEB·환경 블록 전체를 넣는 `MiniDumpWithProcessThreadData`는 뺀다. stack이 가리키는 메모리에는 무엇이든 들어갈 수 있다(완전한 가림은 불가능하다고 문서화).
- **`Detailed`의 범위.** 명령줄·환경 변수가 들어간다. 사용자가 명시적으로 켠다.
- **선택 방법.** `ApplicationSettings::dumpDetail`로 고른다. CL4 전까지는 환경 변수 `ROCKS_CRASH_DUMP`를 쓴다. 마감은 `ApplicationSettings`로 바꿀 수 있다.
- **Rocks 정보(B12).** 덤프에 두 가지로 넣는다.
  - 사용자 stream(`MINIDUMP_USER_STREAM`, 형식 이름 `RocksCrashRecord/1`)에 크래시 기록과 Fatal 기록(메시지·file·line·return address)을 넣는다. 도우미가 교환 블록 사본에서 만든다.
  - 메모리 콜백으로 부모의 정적 기록 범위를 포함시킨다. 디버거가 그 기호를 바로 볼 수 있다.

## 12. Fatal·assert·abort 순환 방지

| 상황 | 막는 장치 |
|---|---|
| Fatal → abort → SIGABRT 처리기 → Fatal | Fatal이 abort를 부르지 않는다. Windows는 `TerminateProcess`로 끝낸다. POSIX는 소유권을 쥔 채 이전 SIGABRT 동작을 복원한 뒤 abort한다 |
| SIGABRT 처리기 재진입(UCRT가 처리기 호출 전 `SIG_DFL`로 되돌림) | 처리기가 첫 줄에서 자신을 다시 건다. 소유자가 자기면 재귀 종료, 다른 스레드면 겹친 크래시 대기. 반환하지 않는다 |
| Log::Fatal → 로그 렌더 중 Fatal | 기존 스레드별 `fatalInProgress`가 막는다(바로 `platform::Fatal`) |
| 종료 콜백 안 Fatal·크래시(W) | filter·hook·Fatal 공통 분기가 W임을 보고 `reporterFailed` → (콜백 단계였으면 파일 단계) → `reportFinished`. 일반 보고 경로(소유권)로 다시 들어가지 않는다 |
| 실패 스레드의 filter 안 재크래시 | 같은 스레드 재귀 → `RecursiveCrash`로 즉시 종료 |
| 여러 스레드 동시 Fatal·크래시 | `crashOwner` CAS 하나. 나머지는 소유자의 종료를 기다린다 |
| purecall·invalid parameter가 보고 중 다시 불림 | SIGABRT와 같은 표(8.5절). 반환하지 않는다 |
| 정상 종료 중 Fatal(`ShutdownOwner`) | 비상 출력 뒤 즉시 종료 |
| 수집기 미설치·Off Fatal | raise 없이 비상 출력 뒤 즉시 종료(콜백 없음) |

## 13. 디버거·ASan·시험 대화상자 정책

### 13.1 디버거

- **판정 시점.** 설치 때가 아니라 크래시 때 `IsDebuggerPresent()`로 본다. 실행 중 붙인 디버거도 같게 다룬다.
- **기본: 디버거에 넘긴다.** filter는 소유권을 얻지 않고 `EXCEPTION_CONTINUE_SEARCH`를 돌려준다. 디버거는 두 번째 기회 예외로 원래 실패 지점에서 멈춘다. OS unhandled filter도 디버거가 있으면 우리 filter를 부르지 않는다.
- **강제 옵션.** `reportWhenDebuggerAttached`(설정, 그리고 환경 변수 `ROCKS_CRASH_REPORT_WITH_DEBUGGER=1`)를 켜면 디버거가 있어도 보고한다. UE `-crashreports`에 해당한다. 보고 뒤 종료하므로 디버거는 종료만 본다.
- **Fatal.** 디버거가 있고 강제 옵션이 없으면 `RaiseException` 없이 `__debugbreak()`로 `Fatal` 프레임에서 멈춘다. stack에서 호출자를 바로 볼 수 있다. 디버거에서 계속하면 프로세스가 끝난다(8.4절).
- **POSIX.** 디버거(ptrace)는 신호 전달 전에 먼저 멈추므로 따로 정책을 두지 않는다.

### 13.2 ASan(`Asan` 구성, D12)

- **현재 연결.** Windows Asan 링크는 `/INCLUDE:__asan_seh_interceptor`와 동적 runtime thunk를 쓴다(`ActionGraph.cs:290–313`). ASan은 자체 진단 출력과 SEH 가로채기를 가진다.
- **정책.**
  - capability에서 `Minidump`·`OutOfProcessDump`를 끈다. 도우미를 띄우지 않는다. UE도 ASan에서는 덤프를 끈다(hang·DbgHelp 경합, `WCC` 77–90).
  - 하드웨어 예외의 보호 프레임·힙 손상 VEH는 소유권을 얻지 않고 `EXCEPTION_CONTINUE_SEARCH`를 돌려준다. ASan의 "AddressSanitizer: access-violation" 보고를 삼키지 않기 위해서다.
  - Fatal·assert·abort·CRT hook은 공통 경로를 탄다. 종료 콜백과 메타데이터는 남기고, 덤프는 `Unsupported("sanitizer build")`로 적는다.
  - ASan 자체 보고(use-after-free 등)는 `__sanitizer_set_death_callback`으로 종료 콜백(로그 flush)과 메타데이터(kind `Sanitizer`)만 남긴다. 보고 문구는 콜백 전에 이미 출력되어 있다. 예산은 같다.
  - **POSIX(LC7).** death callback을 POSIX Asan 구성에도 건다(설치 때 걸고 해제 때 nullptr). POSIX Asan 구성은 macOS만 해당한다(Android Asan·Coverage는 빌드가 거부하고, Linux는 대상 플랫폼이 아니다). 신호 처리기는 Asan 구성에서 하드웨어 fault 신호(SIGSEGV·SIGBUS·SIGILL·SIGFPE)의 이전 동작이 처리기면(SIG_DFL·SIG_IGN이 아니면, ASan `handle_segv` 기본값에서는 ASan 처리기다) 보고하지 않고 곧바로 이전 동작으로 넘긴다(`DeferToSanitizer`). Windows의 "보호 프레임·VEH는 넘긴다"와 같은 이유다(ASan 보고를 삼키지 않는다). ASan이 보고한 뒤 death callback이 공통 Fatal 경로(reason `Sanitizer`)로 종료 콜백·`CrashInfo.json`을 남기고 abort하므로 종료는 SIGABRT(셸 종료 코드 134)다. 이전 동작이 기본·무시면(ASan이 그 신호를 맡지 않음, 예: `handle_segv=0`) Rocks가 보고하고 원래 신호로 끝난다. SIGABRT·SIGTRAP은 넘기지 않는다(ASan이 맡지 않는다). macOS도 같은 코드다(ASan의 `handle_segv` 기본값이 켜져 있다). LC5 리뷰 때 적은 "POSIX Asan 하드웨어 fault는 ASan 종료 코드 1"은 death callback이 없을 때의 판단이었고, LC7에서 Windows와 같은 "ASan 보고 → Rocks Fatal 경로"로 맞췄다. POSIX Asan 구성은 이 환경에서 빌드·실행하지 않았다(코드 검토만, 24.2절 11).
  - **POSIX Asan의 한계(LC7 리뷰).**
    - `DeferToSanitizer`는 이전 처리기가 ASan 것인지 보지 않는다(ASan은 자기 처리기 주소를 공개 API로 알려 주지 않는다). Asan 구성에서 Rocks보다 먼저 다른 라이브러리가 그 신호에 처리기를 걸었으면 그쪽으로 넘기고 Rocks는 보고하지 않는다.
    - 크래시 처리 스레드 자신의 fault는 `DeferToSanitizer`보다 먼저 보고 처리 스레드 실패 경로(`FailReporter`)로 간다. death callback이 도는 동안 ASan은 보고 잠금을 쥐고 있어서, 처리 스레드의 fault를 ASan에 넘기면 그 잠금에서 멈추고 `CrashInfo.json`을 잃는다.
    - 처리 스레드에서 ASan이 직접 감지한 오류(use-after-free 등, 신호가 아니다)는 피할 수 없다. ASan 보고가 같은 잠금에서 멈추므로 소유자는 처리 스레드를 마감까지 기다린 뒤 끝내고, 종료 콜백·`CrashInfo.json`이 빠지거나 불완전할 수 있다. 처리 스레드는 정적 저장소만 쓰므로 이런 오류는 Rocks 결함이다.
    - `ASAN_OPTIONS=handle_abort=1`은 지원하지 않는다. Fatal 경로의 abort와 abort()의 SIGABRT를 ASan이 다시 보고하므로 보고가 두 번 남고 종료 코드가 ASan 종료 코드(기본 1)가 된다(실행 확인 없음). 기본값(`handle_abort=0`)에서는 SIGABRT를 Rocks가 보고한다.
  - **death callback 칸은 프로세스 전역이다.** 설치 때 Rocks 콜백으로 덮어쓰고 해제 때 nullptr로 둔다(다른 코드가 건 콜백을 이어 부르지 않는다). 내장 진입은 걸지 않는다.
  - **최종 filter(LC5 리뷰).** Asan 구성에서 보호 프레임·힙 손상 VEH는 하드웨어 예외를 넘기고(ASan이 먼저 본다), 최종 unhandled filter는 보고한다. ASan은 `SetUnhandledExceptionFilter`를 가로채 자기 처리기를 최종 filter로 두고, 치명으로 보는 코드만 보고한 뒤 나머지를 가로챈 사용자 filter(Rocks)로 넘긴다. 그래서 사용자 예외 코드·잘못된 핸들·`handle_segv=0`의 접근 위반은 Rocks가 보고한다(시험 `Native.Crash.asan-policy-user-exception`, 종료 코드는 그 예외 코드).
  - **LC5 확인(Windows Asan 구성).** filter가 넘긴 접근 위반·stack overflow는 ASan이 `AddressSanitizer: access-violation`·`stack-overflow`로 보고하고, 그 뒤 death callback이 공통 Fatal 경로(reason `Sanitizer`)로 종료 콜백·`CrashInfo.json`(덤프 없음, limitations `sanitizer build`, helper null)을 남긴다. 그래서 Asan 구성의 하드웨어 예외 종료 코드는 예외 코드가 아니라 Rocks Fatal 코드(`0xE0524B46`)다. use-after-free도 같다. Fatal은 ASan과 무관하게 공통 경로다. 내장 진입은 death callback을 걸지 않으므로(호스트 정책) Asan 구성의 내장 호스트에서는 ASan이 호스트 filter보다 먼저 보고하고 끝낸다.
- **지원하지 않는 조합.** ASan과 덤프, ASan과 도우미. capability와 메타데이터에 표시한다.
- **확인할 것.** ASan 구성 시험(LC5)으로 다음을 실행해 확인한다. ASan 일반 실행은 하지 않는다.
  - `asan-uaf` negative control: 보고 문구가 남는지, 제한 시간 안에 끝나는지
  - 접근 위반 probe: ASan 보고가 남는지

### 13.3 시험과 무인 실행의 대화상자

- **공용 API.** PlatformBase `platform::SuppressSystemErrorDialogs()`가 하는 일:
  - `SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX)`
  - `_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT)`
  - `_CrtSetReportMode`·`_CrtSetReportFile`(경고·오류·assert → stderr)
  - `WerSetFlags(WER_FAULT_REPORTING_NO_UI)`(프로세스 설정)
  - TestSupport `PrepareAutomatedTestProcess`·`PrepareFatalTestProcess`, ShaderToolSpike `PrepareSpikeProcess`, VirtualMemoryProbe의 복사본을 이것으로 바꾼다(AppInfo AP1의 "대화상자 억제 API").
- **누가 켜나.** `TestRunner`는 기본으로 켠다. 다른 종류는 `ApplicationSettings::suppressSystemErrorDialogs`로 켠다. `SetErrorMode`는 자식에게 상속된다(`Process.h:137`).
- **시스템 설정을 바꾸지 않는다.** 레지스트리·전역 WER 정책은 건드리지 않는다.
- **크래시를 성공으로 바꾸지 않는다.** 종료 코드는 예외 코드·Rocks 예외 코드라 늘 0이 아니다. 시험은 정확한 코드를 단언한다.

## 14. 저장 경로·메타데이터·심볼

### 14.1 폴더와 파일(D14)

- **뿌리.** 기본은 `Saved/Crashes`다. 로그(`Saved/Logs`)와 같은 상대 규칙이고, 설치 때 현재 작업 폴더 기준 절대 경로로 굳힌다. 엔진 호스트는 설정을 읽은 뒤 `SetCrashOutputDirectory`로 바꿀 수 있다.
- **크래시 폴더.** `<뿌리>/<프로그램>-<프로세스 시작 UTC YYYYMMDDTHHMMSSZ>-<pid>/`
  - 프로세스당 보고는 하나이므로 시작 시각과 pid로 충돌하지 않는다.
  - 이름은 설치 때 고정 버퍼에 만든다. 폴더는 크래시 때 도우미가 만든다(도우미가 없으면 W). 정상 실행에는 빈 폴더가 생기지 않는다.
- **파일.**
  - `Minidump.dmp`
  - `CrashInfo.json`. 예비본은 `"complete": false`이고, 최종본으로 덮는다.
  - 로그·trace는 복사하지 않고 경로만 적는다(요구 12절).
- **실패.** 폴더나 파일을 만들 수 없으면 `%TEMP%\Rocks\Crashes\` 아래 같은 이름으로 한 번 다시 시도한다. 그것도 실패하면 `WriteDiagnostic`으로 한 줄(경로·Win32 오류)을 남긴다. 그래도 종료 콜백은 돈다.

### 14.2 빌드 식별 블록

설치 때 Launch가 Core 값으로 만들어 PlatformBase에 C 문자열로 넘긴다.
- `engineVersion`(`build::CurrentEngineVersion`)
- `toolBuildId`(hex 64)
- `configuration`(`ROCKS_BUILD_*`)
- `platform`
- `architecture`

PlatformBase Win64는 설치 때 주 모듈 PE의 CodeView 레코드를 자기 메모리 이미지에서 읽는다(`IMAGE_DEBUG_DIRECTORY`). 읽는 값은 다음과 같다.
- `pdbGuid`·`pdbAge`·`pdbName`
- `imageTimeDateStamp`·`imageSize`

모듈은 실행 파일 하나다(DLL 없음). 덤프 안 모듈 목록에도 같은 값이 있다. 나중에 `BuildDescription`(AP2)이 생기면 commit·branch·dirty를 더한다.

### 14.3 `CrashInfo.json`

```json
{
  "format": "rocks.crash/1",
  "complete": true,
  "incompleteReasons": [],
  "crashId": "5f0c2a9e4b1d4e6f8a7b3c2d1e0f9a8b",
  "program": "Sandbox",
  "executable": "Sandbox.exe",
  "processId": 1234,
  "processStartUtc": "2026-11-18T09:12:00Z",
  "crashUtc": "2026-11-18T09:15:42Z",
  "phase": "Running",
  "stage": "Engine.Run",
  "kind": "Exception",
  "exceptionCode": "0xC0000005",
  "exceptionAddress": "0x00007FF6A1B2C3D4",
  "faultAccess": { "operation": "Read", "address": "0x0000000000000010" },
  "failingThreadId": 5678,
  "failingThreadName": "Worker 3",
  "reporterThreadId": 4321,
  "fatal": null,
  "concurrentCrashes": { "count": 0, "threads": [] },
  "build": { "engineVersion": "0.1.0", "toolBuildId": "…", "configuration": "Release", "platform": "Windows", "architecture": "x64" },
  "module": { "pdbName": "Sandbox.pdb", "pdbGuid": "1FD3B109-0FC1-6339-4C4C-44205044422E", "pdbAge": 1, "imageTimeDateStamp": "0x6AFBF903", "imageSize": 12345678 },
  "dump": { "status": "Written", "writer": "Helper", "detail": "Standard", "file": "Minidump.dmp", "error": null, "limitations": [] },
  "helper": { "status": "Running" },
  "terminationCallbacks": { "status": "Completed", "milliseconds": 14 },
  "associatedFiles": { "logDirectory": "Saved/Logs", "logFile": null, "traceFile": null },
  "arguments": { "count": 4 }
}
```

- **Fatal 계열.** `"fatal": { "kind": "Assert", "message": "...", "file": "...", "line": 42, "returnAddress": "0x..." }`
- **보고 없는 종료.** `"kind": "UnreportedExit"`과 `exitCode`만 담고, 나머지는 null이다. 기록 조건은 11.4절이다.
- **경로와 개인정보(B14).** 실행 파일·작업 폴더·출력 폴더가 사용자 프로필(`C:\Users\<이름>\...`) 아래면 경로에 사용자 이름이 들어간다.
  - 메타데이터는 실행 파일을 파일 이름으로만 적는다.
  - 다른 경로는 크래시 폴더 뿌리나 작업 폴더 기준 상대 경로를 우선한다. 상대로 나타낼 수 없을 때만 절대 경로를 쓴다.
  - 덤프의 모듈 경로에는 절대 경로가 남는다(OS가 기록한다).
- **수집하지 않는 것(요구 12절).** 명령줄 원문(개수만 적는다), 환경 변수, 사용자·기기 이름, 인증 정보.
  - 가린 명령줄(AppInfo D16 `RedactedArguments`)은 CL2 뒤에 더한다(21절).
- **쓰기 규칙.** 키는 영어이고 UTF-8이다. 고정 버퍼 JSON 쓰기 함수(문자열 escape 포함)를 쓴다. 버퍼가 모자라면 칸을 잘라 `incompleteReasons`에 적는다.

### 14.4 심볼 보관과 덤프 여는 절차

**빌드 쪽.**
- 링크에 `/PDBALTPATH:%_PDB%`를 더한다. 그러면 CodeView의 PDB 경로가 링크 임시 폴더 경로(`.native-tmp\<guid>\Sandbox.pdb`, 3.5절)가 아니라 파일 이름(`Sandbox.pdb`)만 담는다. 빌드 호스트 경로가 배포본에 새지 않고, 디버거는 기호 경로로 찾는다(D16).
- 디버거의 심볼 일치 검사는 PDB GUID·age로 한다. ToolBuildId는 "어느 빌드 산출물인가"를 찾는 보조 식별이고, 그 검사를 대신하지 않는다(요구 12절).

**보관(배포 때마다).** 실행 파일은 PE `TimeDateStamp`·`SizeOfImage`로, PDB는 GUID·age로 저장된다.
```
symstore add /r /f <bin>\*.exe /s <심볼 저장소> /t Rocks /v <engineVersion>-<toolBuildId 앞 12자>
symstore add /r /f <bin>\*.pdb /s <심볼 저장소> /t Rocks /v <같음>
```
배포 패키지에는 PDB를 넣지 않는다. 저장소에만 둔다.

**여는 법.**
```
cdb -z <크래시 폴더>\Minidump.dmp -y "srv*<심볼 저장소>;<bin>" -i "<심볼 저장소>;<bin>" -c ".ecxr; kn 64; ~*kn 16; lm v m <program>; q"
```
- `lm v`의 PDB GUID·age가 `CrashInfo.json`의 `module`과 같은지 본다. `symchk /id <dump> /s <저장소>`로도 확인한다.
- Visual Studio·WinDbg는 덤프를 열고 같은 기호 경로를 지정한다.
- Fatal은 `.ecxr`이 `RaiseException` 지점을 보인다. 호출자는 `kn`의 셋째 프레임 이후다. 메타데이터 `fatal.returnAddress`로도 찾는다.

**외부 수집(선택, 사용자가 직접).** 보호 밖 구간(9절)까지 덤프가 필요하면 관리자가 다음 레지스트리 값을 직접 설정한다.
- 키: `HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\<exe 이름>`
- 값: `DumpFolder`·`DumpType`

Rocks는 이 값을 쓰지 않는다.

## 15. 플랫폼별 지원 수준

| 기능 | Windows x64 | Windows arm64 | macOS | Android |
|---|---|---|---|---|
| Launch 진입점·명령줄 캡처 | 구현·시험 | 빌드 미확인 | 구현, 이 환경에서 컴파일 미확인 | 구현·NDK 컴파일 확인, 에뮬레이터(x86_64, API 35) 실행 확인(LC7). 실행 파일 시험 수명(데스크톱 `main`)만이고, 앱 수명주기는 범위 밖 |
| OS 예외·신호 처리 | SEH·unhandled filter·CRT hook | 같은 코드(CONTEXT 차이) | 신호(LG18 기반) | 신호(LG18 기반) |
| 스레드 보호 | SEH 보호 프레임 + stack 보장 | 같음 | sigaltstack | sigaltstack |
| 덤프 | 도우미 + fallback | 미확인 | `Unsupported`, 처리 뒤 OS ReportCrash로 넘김 | `Unsupported`, 처리 뒤 tombstone으로 넘김 |
| 메타데이터 | 예 | 예 | 예(덤프 없음) | 예(앱 쓰기 가능 경로 필요, 실행 파일 시험은 작업 폴더) |
| 종료 콜백 | 예 | 예 | 예(최선 노력) | 예(최선 노력) |
| 보고 없는 종료 기록 | 예(도우미) | 미확인 | 아니오 | 아니오 |

- `QueryCrashCapabilities()`는 이 표의 실제 값을 돌려준다. 빈 구현이 성공을 돌려주지 않는다(요구 13절).
- 공개 API에 HANDLE·EXCEPTION_POINTERS·CONTEXT·`siginfo_t`를 노출하지 않는다.
- **후속 작업**
  - macOS·Android 덤프: Crashpad 수준의 Mach 예외 포트·ptrace 도우미
  - Android 앱 수명주기 진입점
  - Windows arm64 실행 시험

## 16. 실행 프로그램별 적용 계획

| 실행 파일 | 종류 | 크래시 | 도구 로그 | `configure`·본문 변화 | 보존할 것 |
|---|---|---|---|---|---|
| Sandbox | EngineHost | Full | `--self-test`면 Stderr, `--engine`이면 None(LogHost) | 인자 파싱을 `Span<const StringView>`로 바꾼다. LG18 `CrashHandlerScope`를 지운다. `WriterAbandoned`면 `RequestExitWithoutStaticTeardown`. `--engine`의 `handleTerminationRequests`는 PumpEvents 다리와 함께 LC6에서 켠다(25절 LC4) | 인자·출력·종료 코드(1·2), self-test 자식 실행 |
| RocksCommandlet | Tool | Full | Stderr(commandlet_host의 자체 LogSystem을 대신한다) | Win64·Mac `Main.cpp` 두 개를 공통 진입점 하나로 줄인다. 플랫폼 출력 함수(`WriteConsoleW`·`fwrite`)는 Private/<Platform>에 남긴다. Mac SIGPIPE 무시는 본문 시작에서 한다 | 인자 상한 문구("Too many or invalid arguments."), 종료 코드 0·1·2. 인코딩 오류는 Launch 문구가 되고, 길이 초과 문구는 Win64도 "Invalid or oversized UTF-8 argument."로 바뀐다(25절 LC4). Win64 출력은 `dda5c1e6`의 동작을 그대로 둔다: 4096 byte 상한 없이 쓰고, 콘솔이면 UTF-8 글자 경계에서 자른 조각으로 나눠 `WriteConsoleW`로 쓴다 |
| FoundationTests·CoreFoundationTests·EngineTests | TestRunner | 실행기 본체: Full, 폴더는 `--crash-dir`이 있으면 그것. 자식 probe: `Off`(크래시 probe는 자기 모드를 명시) | None | `PrepareAutomatedTestProcess`를 설정(대화상자 억제)으로 바꾼다. probe 모드 분기는 `configure`가 인자를 보고 정한다 | 모든 시험 모드와 종료 코드, harness 문구 |
| CompressionBench·CryptoBench·MathBench | Tool | Full | None | 손 파싱 입력이 `Span<const StringView>` | 출력(printf)·종료 코드 |
| GcBench·WorldBench(engine-runtime 이식 때 추가) | Tool | Full, `suppressSystemErrorDialogs` | None | TestSupport `PrepareAutomatedTestProcess`·HostFailure 링크를 Launch 설정으로 바꾼다. 손 파싱 입력이 `Span<const StringView>` | 인자·출력·종료 코드 |
| LogBench | Tool | Full | None(자체 LogSystem을 잰다) | 같음 | 같음 |
| ShaderToolSpike | Tool | Full | Stderr | `PrepareSpikeProcess`를 `suppressSystemErrorDialogs`로 바꾼다 | 인자·종료 코드. 도구 로그 시작 실패 때 본문을 계속 도는 동작(Launch 정책도 같다, D22) |
| 게임 템플릿(`Templates/Game/Foundation`) | EngineHost(게임 기본값) | Full | None | `int main()`을 매크로와 `Run`으로 바꾼다. `Game.Build.cs` 의존에 `Launch`를 더한다 | 종료 코드 0–4 |
| C# 도구(Rocks.Build.Cli·MetaCompiler·ToolTests 등) | — | 대상 아님 | — | 바꾸지 않는다(요구 14절) | — |

- **새 실행 프로그램.** 템플릿과 이 문서의 예(7.4절)를 따른다. 엔진이 소유한 native 소스가 `main`·`wmain`·`WinMain`·`wWinMain`을 직접 정의하면 policy `process-entry-point`가 실패로 알린다(예외 목록은 `Launch/EntryPoint.h`와 내장 호스트 시험 실행 파일뿐, 25절 LC4).
- **설치 SDK.** SDK 내보내기는 대상(target)의 모듈과 그 의존을 담는다. `Engine` 대상(설치 SDK 기본)과 통합 시험의 `ReflectionFixtures` 대상에 `Launch`를 더했다(게임 템플릿이 쓴다).

## 17. 시험 계획(요구 15절)

**원칙.**
- 크래시 시험은 모두 자식 프로세스에서 한다. 시험 실행기를 크래시시키지 않는다.
- 부모(C# `Rocks.ToolTests`의 `Native.Crash.*`)는 자식마다 제한 시간(기본 60 s)을 둔다. 넘기면 kill하고 실패로 기록한다.
- 순서는 명시 gate(이벤트·파이프 줄)로 만든다. sleep을 쓰지 않는다.
- 의도적 장애 주입은 FoundationTests probe(테스트 전용 코드)에만 둔다. 도우미 오작동은 시험용 도우미 실행 파일 지정(11.2절)으로 만든다.
- **시험용 짧은 예산.** probe는 설정으로 예산을 줄인다. 캡처 2 s, 콜백 500 ms, 덤프 Standard 20 s, 겹친 스레드 여유 1 s를 쓴다. 시간 초과 시험이 빨리 끝나게 하려는 것이다.
- **LC1 probe의 설치 방식.** LC1의 probe는 Launch가 아직 없으므로 `InstallCrashReporting`을 직접 부른다. LC4에서 Launch 설정으로 바꾼다.

**job 시험의 전제(바깥 job의 breakaway 허용 여부).**
- 도우미는 `CREATE_BREAKAWAY_FROM_JOB`으로 먼저 띄운다(11.2절). job이 중첩돼 있으면 한 단계라도 breakaway를 허락하지 않을 때 이 호출이 `ERROR_ACCESS_DENIED`로 실패하고, 도우미는 플래그 없이 job 안에 뜬다. 그래서 probe가 만든 job이 breakaway를 허락해도, 시험 실행기가 이미 breakaway를 막은 바깥 job 안에 있으면 도우미는 job 안에 남는다. GitHub Windows 러너가 그렇다(run 44643005452, `job-kill-on-close-breakaway`가 `inJob: true`로 실패). 수집기의 동작(job 안이면 부모가 기본 덤프까지 기다림)은 두 경우 모두 맞다. 틀린 것은 환경과 무관하게 "빠져나간다"를 단언한 시험이었다.
- **기대값은 측정한 전제에서 정한다. 건너뛰지 않는다.** 시험 실행기(C#)가 시작 때 같은 호출을 직접 해 본다: `CreateProcessW`에 `CREATE_BREAKAWAY_FROM_JOB | CREATE_SUSPENDED | CREATE_NO_WINDOW`로 무해한 프로세스(시험 실행 파일 자신, 실행되지 않음)를 띄우고, 성공하면 `IsProcessInJob(child, null)`을 본 뒤 곧바로 `TerminateProcess`한다. 성공하고 자식이 어느 job에도 없으면 "breakaway 가능"이다. `QueryInformationJobObject(null)`는 바로 위 job만 보므로 중첩 job에서 쓰지 않는다.
  - breakaway 가능: `job-kill-on-close-breakaway`의 도우미 `inJob`은 false다.
  - 불가능: `inJob`은 true다. 덤프와 `CrashInfo.json`이 온전한지(`AddCrash`의 공통 단언) 본다.
- **두 갈래를 로컬에서도 결정적으로 돈다.** 변형 `job-kill-on-close-breakaway-blocked`는 probe가 breakaway를 막은 바깥 job(제한 없음, KILL_ON_JOB_CLOSE 없음)에 먼저 들어간 뒤 breakaway 허용 KILL_ON_JOB_CLOSE job을 만든다. 환경과 무관하게 `inJob`은 true이고 덤프가 온전해야 한다. 그래서 개발 기계에서도 CI 조건의 경로가 검증된다.
- `job-kill-on-close`(breakaway 금지)는 환경과 무관하게 `inJob` true다. `job-after-helper`는 `inJob`을 단언하지 않는다(도우미는 부모가 job에 들어가기 전에 뜬다. 바깥 job이 breakaway를 막으면 바깥 job 안에 있다). 두 시험 모두 공통 단언(덤프·메타데이터)만으로 환경에 기대지 않는다.

**덤프 판독.**
- **구조 판독.** C# 최소 minidump 판독기(`Rocks.ToolTests`)로 다음을 확인한다.
  - 머리·stream 목록이 유효한가
  - `ExceptionStream`이 있는가
  - 예외 코드가 기대한 값인가
  - 예외 스레드 id = 메타데이터 `failingThreadId` = probe가 알린 id인가
  - `RocksCrashRecord/1` 사용자 stream이 있는가
  - 모듈 CodeView GUID = 메타데이터 `module.pdbGuid`인가
- **심볼 판독.** 명령을 고정한다.

  ```
  cdb -z <dump> -y "<bin>" -i "<bin>" -c ".symopt+0x40; sxd av; sxd eh; .ecxr; kn 32; lm v m <program>; q"
  ```

  출력에서 기대 함수 이름(probe의 실패 함수)과 PDB GUID를 찾는다.
- **cdb가 없으면 시험은 실패다.** 건너뛰지 않는다. 지금 환경에는 cdb·symchk·symstore가 있다(Windows Kits 10 Debuggers).

| 요구 항목 | probe(자식) | 단언 |
|---|---|---|
| 정상 시작·종료 | 빈 본문 | 종료 코드 0, 크래시 폴더 없음, 도우미 프로세스 종료 확인 |
| 초기화 중 일반 오류·역순 정리 | 도구 로그 시작 실패 주입(시험 sink), 본문이 오류 코드 반환 | 시작한 단계만 정리됐다는 stderr 표시, 종료 코드 |
| 메인 스레드 접근 위반 | null 읽기 | 덤프 판독 전부, 종료 코드 0xC0000005 |
| 엔진 스레드 접근 위반 | `platform::Thread` 안 | 실패 스레드 id = 그 스레드, 스레드 이름 |
| 래퍼 밖 네이티브 스레드 | `CreateThread`·`pthread_create`로 직접 만든 스레드 | unhandled filter 경로로 같은 결과 |
| `Log::Fatal`·`ROCKS_ASSERT`·`platform::Fatal` | 각각 | Rocks 예외 코드, `fatal.kind`·메시지·file/line, stack에 호출 함수, 로그 파일에 Fatal 기록 |
| `abort()`(주 스레드·외부 스레드) | 직접 `abort()` | `kind=Abort`, 덤프, 종료 코드가 3이 아님(처리기가 반환하지 않음) |
| invalid parameter | CRT 함수에 잘못된 인자 | `kind=RuntimeCheck`, 덤프 |
| pure virtual call | 생성 중 순수 가상 호출 | `kind=RuntimeCheck`, 덤프 |
| abort 재진입 | 종료 콜백 안에서 `abort()` | `reporterFailed`, 예산 안 종료, 덤프는 이미 있음 |
| Off 모드 Fatal | 수집기를 끈 probe에서 `platform::Fatal` | 크래시 폴더 없음, stderr 문구, Rocks 예외 코드, raise 흔적 없음 |
| stack overflow | 무한 재귀(주 스레드, `platform::Thread`) | 0xC00000FD, 덤프 유효 |
| 처리기 재귀 | 종료 콜백 안 접근 위반·Fatal | 예산 안 종료, `reporterFailed`, 덤프는 이미 있음 |
| 여러 스레드 동시 실패 | 스레드 A가 실패한다. 시험 전용 종료 콜백이 "보고 진입" 이벤트를 알리고 gate에서 기다린다. 부모가 그 이벤트를 본 뒤 스레드 B를 실패시킨다. B가 칸에 기록됐다는 표시를 보고 gate를 연다 | `concurrentCrashes.count == 1`, 덤프·메타데이터 정확히 하나, 칸의 스레드 id = B, 종료 코드 = A의 코드 |
| 초기화·종료 도중 크래시 | `configure` 직후 실패, 본문 반환 뒤 정리 단계에서 실패 | `phase`가 `Startup`·`Shutdown` |
| 로그 sink·writer 멈춤 | LG18 멈춘 sink probe + 접근 위반 | 덤프 있음, `terminationCallbacks.status = TimedOut`, 예산 안 종료 |
| 덤프 저장 경로 실패 | 출력 폴더 자리에 파일을 둔다 | fallback 폴더에 결과, 또는 stderr 경로 진단 |
| 도우미 시작 실패·응답 없음·중도 종료 | 도우미 경로를 없는 파일로 / 시험 도우미가 캡처 신호를 보내지 않음 / 바로 끝남 | `helper.status`, `dump.writer = InProcess` 또는 `CaptureTimedOut`, 예산 안 종료 |
| Full 덤프 마감 | 시험 도우미 마감 1 s, `Full`, 큰 메모리 | `Minidump.dmp` 있음·cdb로 열림, `detailDump.status = Cancelled`이고 그 파일은 없음, 부모는 캡처 뒤 바로 끝남 |
| 보고 없는 비정상 종료 | `__fastfail` | 도우미가 쓴 `kind=UnreportedExit`, 종료 코드 0xC0000409 |
| Ctrl-C·harness kill | `GenerateConsoleCtrlEvent`로 Ctrl-C를 보낸 자식, `Process.Kill` | 두 경우 모두 크래시 폴더가 생기지 않는다 |
| 정상 `_Exit`(0이 아닌 코드) | `RequestExitWithoutStaticTeardown` 경로로 종료 코드 7 | 크래시 폴더 없음 |
| Embedded 모드 | 시험 호스트가 바깥 `__try`로 받는다 | 콜백·메타데이터 뒤 호스트 처리기에 같은 예외 코드가 도달, 프로세스는 호스트가 끝냄 |
| 디버거 정책 | `cdb -g -G -c "sxe av; g; .lastevent; q"`로 자식 실행 | 기본: 디버거가 원래 지점에서 멈춤, 크래시 폴더 없음. 강제 옵션: 보고됨 |
| ASan 정책 | Asan 구성의 `asan-uaf`·접근 위반 | ASan 보고 문구 있음, `dump.status = Unsupported`, 예산 안 종료 |
| Debug·Release·ThinLTO | 위 Windows 시험을 Debug와 Release(ThinLTO)로 | 둘 다 통과. Release stack에 실패 함수가 남는다(noinline 확인) |
| POSIX | Android NDK 빌드(단계마다), 에뮬레이터가 있으면 SIGSEGV·Fatal probe | 이전 동작 복원·tombstone, `Unsupported` 표시. 못 돌린 것은 통과로 적지 않는다 |
| job과 도우미 breakaway | probe가 KILL_ON_JOB_CLOSE job(breakaway 허용·금지)에 들어간 뒤 접근 위반. 바깥 job이 breakaway를 막는 변형은 probe가 breakaway를 막은 job에 먼저 들어가고 그 안에 breakaway 허용 job을 만든다 | 도우미 `inJob`이 아래 전제에서 정한 값, 덤프·`CrashInfo.json` 온전(부모가 기본 덤프까지 기다림) |
| 유니코드 인자 | 한글·이모지·`"` 포함 인자로 자식 실행 | 본문이 받은 UTF-8이 같다 |
| GUI subsystem | `WindowsSubsystem=Windows` 시험 실행 파일 | 링크·실행, 인자 전달, 종료 코드 |

## 18. 정상 실행 비용

| 항목 | 비용 | 측정(LC5) |
|---|---|---|
| 프레임마다 | 없음. 할당·전역 lock·원자 연산을 더하지 않는다. `SetRunPhase`는 단계가 바뀔 때만 부른다 | Release Sandbox `--engine` 프레임 시간 비교(앞뒤 차이가 측정 잡음 안) |
| 스레드 시작마다 | `SetThreadStackGuarantee` 한 번(32 KiB가 그 스레드 stack 안에서 예약된다). SEH 보호는 x64 표 기반이라 실행 비용이 없다 | `platform::Thread` 시작·join 왕복 시간 비교 |
| 프로세스당 | W 스레드 하나(256 KiB 예약, 실제 commit은 수 KiB), 이벤트 둘(도우미가 있으면 다섯 더), 정적 저장소 약 40 KiB(BSS), 교환 블록 64 KiB(Full), dbghelp.dll 로드, 도우미 프로세스 하나(Full) | 시작 시간(설치~본문 진입), 도우미 작업 집합, FoundationTests 전체 시간 비교 |
| 시작 지연 | 도우미 `CreateProcessW` 한 번(기다리지 않는다), dbghelp 로드 | 같음. 시험 실행기의 자식 probe는 `Off`라 비용이 없다 |
| 크래시 때 | 부모 최대 총 대기(도우미 있음 13 s, fallback 35 s). 도우미의 덤프 작성은 부모 종료 뒤에도 종류별 마감까지 이어진다 | 시험에서 예산 안 종료를 단언 |

- 도우미 프로세스 비용이 크게 나오면 Tool 종류의 기본값을 `InProcess`로 낮추는 것을 다시 본다(D25).
- **LC5 측정(2026-11-19, Release ThinLTO, 같은 기기에서 다른 세션의 빌드가 돌던 상태라 잡음이 크다).**
  - 시작부터 정상 종료까지(LaunchProbe `exit 0`, 프로세스 생성 포함, 30회 중앙값, 두 차례): Full 92·68 ms, InProcess 57·61 ms, Off 52·51 ms. 도우미(생성과 종료 때 Shutdown 대기)가 약 15–40 ms, 수집기 설치(전용 스레드·이벤트·dbghelp)가 약 6–10 ms다.
  - `platform::Thread` 시작·join 왕복(5000회 평균, 세 차례): Full 0.62–1.21 ms, Off 0.46–0.73 ms. 범위가 겹쳐 차이를 잡음과 구분할 수 없다. stack 보장(`SetThreadStackGuarantee`)은 수집기 설치와 무관하게 늘 건다.
  - 도우미 작업 집합 11.5 MiB, 개인 commit 23 MiB, 핸들 120개(시험 실행기 FoundationTests의 도우미. 도우미는 같은 실행 파일이라 개인 commit은 그 실행 파일의 쓰기 섹션 크기를 따른다). 부모가 kill되면 도우미도 끝난다.
  - 프레임마다 비용은 재지 않았다: 프레임 경로에 수집기 코드가 없다(`SetRunPhase`는 단계가 바뀔 때만, 코드 확인). Release Sandbox에는 수집을 끄는 스위치가 없어 앞뒤 비교를 하지 않았다.
  - **D25 판단.** 도우미 비용은 실행마다 수십 ms·11 MiB 수준이다. 엔진 호스트·시험 실행기에는 무시할 만하고, 짧은 도구를 스크립트로 수백 번 부를 때만 체감된다. 크래시 수집의 견고성(힙·stack 손상에서도 덤프)이 이 비용보다 크다고 보고 D25(모든 종류 Full)를 유지한다. 짧은 도구를 반복 호출하는 사용처가 생기면 그 프로그램의 설명에서 `InProcess`를 고른다.

## 19. 로깅 연계: LG18 이관과 종료 콜백 인터페이스

이 절은 로깅 세션(rocks-1e)에 보내는 부분이다. 따로 떼어 읽을 수 있게 썼다. 개정 2에서 로깅 세션 의견(등록 방식, 계약 6, 미설치 프로세스, 연관 파일 칸)을 반영했다.

### 19.1 합의된 소유권

- **처리기는 수집기 하나가 소유한다.** OS 처리기 설치·제거, 크래시 처리 전용 스레드, stack 보장, 신호 처리기, CRT 치명 hook은 PlatformBase 크래시 수집기 하나가 소유한다. 기존 PlatformBase LG18 코드는 옮기거나 바꾼다. 하위호환은 두지 않는다.
- **로깅은 flush 콜백만 꽂는다.** 하위 계층 등록 인터페이스에 flush 콜백만 꽂고, 독자 처리기·독자 전용 스레드를 두지 않는다(요구 5절).
- **덤프와 flush의 순서는 이 문서가 정한다.** 순서는 다음과 같다(19.5절).
  - 도우미가 있으면 snapshot 캡처 → flush(덤프 작성과 병행)
  - 도우미가 없으면 flush → 프로세스 안 덤프

### 19.2 LG18에서 옮기는 것·지우는 것

| LG18 | 이관 뒤 | 누가 |
|---|---|---|
| `Platform/CrashHandler.h`(`InstallCrashHandler`·`RemoveCrashHandler`·`CrashHandlerOptions`) | 지운다. `Platform/CrashReporting.h`의 `InstallCrashReporting`·`RemoveCrashReporting`(Launch가 부른다) | 이쪽 |
| `PrepareThreadForCrashHandling`·`ReleaseThreadCrashHandling`·`CrashStackReserveBytes` | `CrashReporting.h`로 옮긴다(동작 같음) | 이쪽 |
| `Win64/CrashHandler.cpp`·`Posix/CrashHandler.cpp` | 수집기 구현으로 다시 쓴다. flush 전용 스레드는 크래시 처리 스레드 W가 된다 | 이쪽 |
| `platform::SetTerminationHook`·`TerminationHook`·`TerminatingThreadId` | 지운다. 종료 콜백 등록(19.3절)이 대신한다 | 이쪽(선언), 로깅(사용처) |
| `detail::RunTerminationHook`·`RecordTerminatingThread` | 수집기 내부로 흡수한다 | 이쪽 |
| `platform::Fatal`의 hook 직접 호출(실패 스레드에서) | 지운다. Fatal은 공통 경로로 가고 콜백은 W에서 돈다 | 이쪽 |
| `ManualResetEvent::TrySignal` | 그대로 둔다(`FlushForTermination`이 쓴다) | 로깅 |
| `LogSystem::FlushForTermination` | 그대로 둔다. 콜백이 부른다 | 로깅 |
| `detail::LogTerminationHook`·`SetChainedTerminationHook`·`LogInstallScope`의 hook 교체 | 지운다. Core가 **첫 `LogInstallScope` 설치 때 정적 콜백 하나를 한 번 등록하고 해제하지 않는다**(user는 nullptr). 콜백은 `LogReadScope`(세대 카운터)로 그때 설치된 LogSystem을 lock 없이 찾는다. `LogInstallScope`는 등록·해제하지 않는다 | 로깅 |
| Sandbox `CrashHandlerScope` | 지운다(Launch가 설치한다) | 이쪽(LC4) |
| FoundationTests `LoggerProcessProbe` 크래시 모드 | 남긴다. 설치는 LC1에서는 직접 설치 코드, LC4부터는 Launch 설정으로 한다. mode 0(`platform::Fatal`, 처리기 미설치)은 수집기를 설치한 상태로 바꾼다. 미설치 Fatal은 콜백을 부르지 않기 때문이다 | 로깅(probe), 이쪽(설치) |
| `LoggingSystem.ko.md` 14.1.8·14.2·L24·L25·9.2 | "처리기 소유는 CrashHandlingAndLaunch"로 고친다 | 로깅 |

### 19.3 인터페이스(`Platform/CrashReporting.h`)

```cpp
namespace rocks::platform {

enum class TerminationReason : uint8 {
    Fatal,         // platform::Fatal·Log::Fatal
    Assert,        // ROCKS_ASSERT
    Exception,     // 하드웨어 예외(Windows)
    Signal,        // 치명 신호(POSIX)
    Abort,         // abort()·SIGABRT
    RuntimeCheck,  // invalid parameter·pure virtual call
    Sanitizer,     // ASan 등 sanitizer 보고 뒤
};

enum class TerminationCallbackResult : uint8 { Completed, Skipped, TimedOut };

inline constexpr uint32 MaxStoppedCrashThreads = 8;

// 콜백에 넘기는 값이다. 포인터는 콜백 동안 유효하다(정적 저장소).
struct TerminationContext {
    TerminationReason reason;
    uint64 failingThreadId;            // CurrentThreadId() 값. 콜백을 부르는 스레드(W)가 아니다
    const uint64* stoppedThreadIds;    // 크래시로 멈춘 다른 스레드(겹친 실패, 10.2절 칸)
    const uint32* stoppedThreadCount;  // 콜백 중에도 늘 수 있으므로 AtomicLoad로 읽는다(LC1a)
    const char* message;               // NUL 종료 종료 사유, 비어 있을 수 있다
    usize messageBytes;
    uint64 deadlineNanoseconds;        // MonotonicNanoseconds() 기준 마감. 콜백 전체 공용
};

// 막히는 lock을 쓰지 않고 deadline 전에 돌아와야 한다. 결과는 메타데이터에 남는다.
using TerminationCallback = TerminationCallbackResult (*)(const TerminationContext& context,
                                                          void* user) noexcept;

inline constexpr uint32 MaxTerminationCallbacks = 4;

struct TerminationCallbackHandle {
    uint32 slot = NumericLimits<uint32>::Max();  // "없음" 표지
    uint32 generation = 0;
};

// 콜백을 등록한다. 칸이 모자라면 false다. 수집기 설치 여부와 무관하게 등록할 수 있다(설치 전 등록 가능).
bool RegisterTerminationCallback(TerminationCallback callback, void* user,
                                 TerminationCallbackHandle& handle) noexcept;

// 등록을 푼다. 크래시 처리 중이고 그 콜백이 이미 불리고 있으면 돌아오지 않는다(프로세스가 곧 끝난다).
// 그래서 user가 가리키는 객체를 콜백 도중에 파괴하지 않는다. 로깅은 해제하지 않으므로 이 계약은 다른
// 등록자를 위한 것이다.
void UnregisterTerminationCallback(TerminationCallbackHandle& handle) noexcept;  // 푼 뒤 handle을 비운다

// 연관 파일 칸(LC1b). 크래시 때 CrashInfo.json "associatedFiles"에 들어간다. 칸마다 쓰는 스레드는
// 하나(seqlock), 512 B까지, 넘으면 false이고 칸은 그대로다. 설치 전에도 부를 수 있다.
enum class CrashAnnotation : uint8 { LogDirectory, LogFile, TraceFile, Custom0, Custom1, Count };
inline constexpr usize MaxCrashAnnotationBytes = 512;
bool SetCrashAnnotation(CrashAnnotation slot, const char* text, usize bytes) noexcept;

} // namespace rocks::platform
```

구현과 같게 맞췄다(LC8). 공개 헤더 `Platform/CrashReporting.h`가 기준이다.

**계약.**
1. **부르는 스레드.** 크래시 처리 스레드 W만 부른다. 실패 스레드에서는 부르지 않는다.
   - 수집기를 설치하지 않은 프로세스에서는 부르지 않는다. 그런 프로세스는 크래시 때 로그 flush가 없다. Rocks 실행 파일은 모두 Launch를 거치므로 남는 것은 C# 도구와 외부 호스트뿐이다.
   - `Log::Fatal`·assert는 지금처럼 자기 스레드에서 비상 출력과 flush를 한다.
2. **한 번.** 프로세스에서 첫 보고 때 한 번만 부른다. 재진입·중복은 수집기가 막는다(보고 소유권). 콜백은 자기 중복 판정을 하지 않아도 된다.
3. **시간 예산.** `deadlineNanoseconds`가 공용 마감이다(기본 2 s 뒤, `callbackBudgetMilliseconds`).
   - 등록 역순으로 부른다.
   - 마감이 지나면 남은 콜백은 `Skipped`다.
   - 막힌 콜백이 W를 붙잡아도 실패 스레드의 총 예산이 프로세스 종료를 보장한다.
4. **막히는 lock 금지.** 쓸 수 있는 것은 TryLock, 원자 위치 확인, 양보, `TrySignal`뿐이다(LG18 `FlushForTermination`이 이미 이렇다).
5. **실패 스레드 id.** `failingThreadId`나 `stoppedThreadIds` 중 하나가 로그 출력 스레드 자신이면 큐를 비울 주체가 없다. 그때는 기다리지 않고 비상 출력만 한다(`TerminatingThreadId()` 대신 이 칸을 쓴다).
6. **Fatal의 중복 flush.** 로깅은 프로세스 원자 표시 "Fatal flush 시도함"을 둔다. `Log::Fatal`·assert가 실패 스레드에서 flush를 시도할 때 세운다.
   - 콜백은 이 표시를 봐도 바로 `Skipped`하지 않는다. 마감 안에서 남은 큐를 한 번 더 기다린다.
   - 실패 스레드의 flush는 그 스레드의 제한 시간에 끊겼을 수 있다. 그 뒤에 들어온 다른 스레드의 기록도 있을 수 있다.
7. **할당 금지.** 콜백 안에서 엔진 allocator·`operator new`·레지스트리 순회를 하지 않는다.

### 19.4 로깅 세션이 고칠 것

- **등록.** Core가 첫 `LogInstallScope` 설치 때 정적 콜백 하나를 `RegisterTerminationCallback(callback, nullptr, handle)`로 한 번 등록하고 해제하지 않는다. 콜백은 `LogReadScope`로 현재 설치된 시스템을 찾는다. 없으면 `Skipped`다.
- **콜백 본문.** `FlushInstalledLogForTermination` 결과를 `TerminationCallbackResult`로 돌려준다. `TerminatingThreadId()` 대신 `context.failingThreadId`·`stoppedThreadIds`를 쓴다.
- **Fatal 표시.** 19.3절 계약 6의 프로세스 원자 표시를 두고, 스레드별 `fatalInProgress` 판정을 대신한다. 스레드별 표시는 W에서 보이지 않는다.
- **probe 수정.** `LoggerProcessProbe`·`LoggerOutputTests`의 크래시 모드를 수집기 설치 기준으로 바꾼다. 기대 종료 코드는 0이 아닌 값에서 정확한 코드(D8)로 좁힐 수 있다.
- **연관 파일 칸(확정).** `FileLogSink`가 파일을 열 때와 회전할 때 출력 스레드에서 `SetCrashAnnotation(CrashAnnotation::LogFile, ...)`로 현재 파일 경로를 알린다. 이 칸을 쓰는 쪽은 출력 스레드 하나다.
- **문서.** `LoggingSystem.ko.md`를 개정한다(19.2절 마지막 줄).

### 19.5 순서와 근거

```
도우미 있음:
  T: 소유권·문맥 복사 → W 깨움 → 대기
  W: 메타데이터 조각 서식 → 교환 블록 → 도우미 요청 → 캡처 끝 대기 → 종료 콜백(로그 flush) → 결과 기록 → 완료
  H: 폴더·예비본 → snapshot 캡처 → 캡처 끝 알림 → 덤프 작성(부모 flush와 병행) → 최종본

도우미 없음(InProcess·fallback):
  W: 종료 콜백(로그 flush) → 폴더·예비본 → 프로세스 안 덤프 → 최종본 → 완료
```

- **도우미가 있을 때.** 캡처가 먼저다. 증거를 크래시 시점에 가깝게 잡으려면 flush 동안 다른 스레드가 상태를 바꾸기 전에 캡처해야 한다.
  - 캡처는 짧다. 덤프 작성은 snapshot을 대상으로 하므로 flush와 겹쳐도 부모를 붙잡지 않는다.
  - 캡처 동안은 부모 스레드가 잠시 멈추고, 출력 스레드도 그 사이 멈췄다가 이어서 flush한다.
- **도우미가 없을 때.** flush가 먼저다. 프로세스 안 덤프는 다른 스레드가 쥔 heap·loader lock 때문에 막힐 수 있고, 그러면 flush까지 잃는다(B1).

### 19.6 로깅 문서와의 대응(LC8)

로깅 문서는 로깅 세션(rocks-1e) 소유다. 이 문서는 참조와 인터페이스만 맞춘다.

- **이관 반영.** 로깅 문서의 이관 개정(`feature/logging` `060b687b`까지, 이 구현이 engine-runtime에 병합되기를 기다림, 그 브랜치에서 "개정 13")이 19.2절 표의 마지막 줄을 반영했다. engine-runtime의 로깅 문서도 "개정 13"(2026-11-20, sink 수명 계약 L28과 POSIX self-pipe)이라 번호가 겹친다. 병합 때 로깅 세션이 번호를 다시 매긴다. 14.1.8절이 "처리기는 수집기 소유, 로깅은 종료 콜백 하나", 콜백 본문(`FlushInstalledLogForTermination`·`WriterThreadFailed`·결과 값), Fatal 표시(`MarkFatalFlushAttempted`), `LogFile` 칸 쓰는 쪽·512 B 초과 때 비우기를 적는다. engine-runtime 판의 14.1.8은 아직 로깅 소유 처리기(flush 전용 스레드, self-pipe)를 적는다.
- **코드 쪽 상태.** 19.4절의 등록·콜백 본문·Fatal 표시·probe 수정은 LC1a에서 이쪽이 함께 바꾸고 로깅이 리뷰했다(`LogSystem.cpp`의 `RegisterTerminationCallback`, `MarkFatalFlushAttempted`). 남은 것은 로깅 몫이다.
  - `FileLogSink`가 파일을 열 때와 회전할 때 `SetCrashAnnotation(CrashAnnotation::LogFile, ...)`(병합 뒤).
  - `CrashAnnotation::LogDirectory` 칸은 쓰는 쪽을 정하지 않았다(로깅 문서 개정 13에도 없다). 로그 폴더를 정하는 LogHost가 쓰는 것을 권한다.
- **인터페이스 차이 확인.** 로깅 문서 개정 13의 서술은 19.3절의 지금 인터페이스(멈춘 스레드 수는 원자 포인터, `Unregister`는 참조로 받아 비움, 공용 마감 기본 2초는 Launch `crashCallbackBudgetMilliseconds`로 바꿀 수 있음)와 어긋나는 곳이 없다. 마감을 Launch 설정으로 바꿀 수 있다는 점은 그 문서에 없다.

## 20. 종료 요청과 엔진 스레드 규약

- **`Engine::RequestExit()`는 게임 스레드 API다(LC6 반영: 주석 고침, Debug·Development는 `Initialize`를 부른 스레드인지 assert).**
  - 지금 일반 `bool`이라 다른 스레드에서 부르면 data race다. 그런데 주석은 "언제 호출해도 안전하다"(`Engine.h:288–291`)고 한다.
  - 이 주석을 "엔진을 소유한 게임 스레드에서만"으로 고치고, Debug·Development에서는 소유 스레드 검사를 더한다(LC6).
  - 원자 변수로 바꾸지 않는 이유가 있다. 다른 스레드의 종료 요청과 엔진 프레임 경계를 섞으면 "어느 프레임에서 멈추는가"가 경쟁에 달린다.
- **프로세스 종료 요청은 Launch가 받는다.**
  - Windows는 `SetConsoleCtrlHandler`, POSIX는 SIGINT·SIGTERM `sigaction`이 `launch::RequestTermination()`(원자 표시)만 부른다.
  - 두 번째 요청은 OS 기본 동작(즉시 종료)으로 간다. Windows는 처리기가 FALSE를 돌려 기본 처리기로 넘기고, POSIX는 설치 전 동작을 되돌린 뒤 같은 신호를 다시 낸다.
  - `handleTerminationRequests`가 켜진 프로그램만 건다. 지금 도구들은 Ctrl-C에 바로 끝나는 동작을 유지한다.
- **초기화 중 요청.** 엔진 초기화 동안 온 Ctrl-C는 표시만 남고, 초기화를 마친 뒤 첫 프레임의 PumpEvents에서 정상 종료로 이어진다(LC6 리뷰).
- **다리는 게임 스레드에서 놓는다.** 엔진 호스트가 `EngineHooks::PumpEvents`에서 `launch::IsTerminationRequested()`를 보고 `engine.RequestExit()`를 부른다. 그 뒤는 정상 종료(8.2절)다.
- **크래시 종료와 섞지 않는다.** 크래시는 `RequestExit`·`Shutdown`을 부르지 않는다(8.3절).
- **창 메시지·입력.** `WM_CLOSE`·입력은 Application 수명에 넣지 않는다. PlatformWindow·`ui::Platform` 이벤트가 PumpEvents에서 같은 방식으로 `RequestExit`로 이어진다(요구 4절).

## 21. 다른 설계와의 경계

- **[앱 정보와 명령줄](AppInfoCommandLine.ko.md)**
  - **이번에 구현한다(AP1의 일부).**
    - `ProcessCommandLine`(7.3절)
    - PlatformBase 대화상자 억제 API(13.3절)
    - Sandbox·RocksCommandlet·FoundationTests 진입점 연결
  - **AppInfo 12절을 바꿨다(D7, AppInfo 개정 3).** Windows `main(char**)`·`WinMain`의 `GetCommandLineW` 직접 분해는 하지 않는다. Launch 진입점은 늘 `wmain` 또는 `wWinMain`(CRT `__wargv`)이다. 그 문서의 12절(구현된 상한·`IsCaptured`), 7.1절, 8.3절 0단계, 17절, 18절 AP1, D15를 고쳤다.
  - **AP1에 남는 것.** `AppInfo`·`RunTraits`·`HostKind`·`AppInfoScope`. 그때 `ApplicationKind`는 `HostKind`로 합치고, `suppressSystemErrorDialogs`는 `RunTraits::unattended`에서 정한다.
  - **CL2 뒤.** 가린 명령줄(`RedactedArguments`)을 메타데이터에 더한다.
  - **CL4 뒤.** `--crash-dump=`·`--crash-dir=`·`--crash-report-with-debugger`를 호스트 공용 옵션으로 옮기고, 환경 변수 임시 경로를 지운다.
  - **AP2 뒤.** `BuildDescription`(commit·branch·dirty)을 메타데이터 `build`에 더한다.
- **[로깅 시스템](LoggingSystem.ko.md)**: 19절. 처리기 소유권이 이쪽으로 옮겨 온다.
- **[엔진 구현 계획](EngineImplementationPlan.ko.md)**
  - 29.4절의 "사전 확보 storage·제한 시간·truncated 표시·잠긴 heap·registry를 순회하지 않음"을 이 설계가 그대로 따른다.
  - 종료 코드 계약(2479행)의 "OS crash code는 별도로 보존"을 D8로 구체화한다. 도구들의 0·1·2 정리는 CL4 출력 규약이 맡는다.
- **[모듈 로더](ModuleLoader.ko.md)(ML0 병합, 개정 2)**
  - **지금(ML0).** Launch 모듈과 Launch 시험 실행 파일(`LaunchProbe`·`LaunchWindowedProbe`·`LaunchEmbeddedHost`, 같은 소스라 이름 자리에 `ROCKS_MODULE_NAME`)도 다른 모듈처럼 `ROCKS_IMPLEMENT_MODULE` 진입점을 가진다(`Launch/Private/LaunchModule.cpp`, `Programs/LaunchTests/Private/Common/LaunchTestsModule.cpp`). 빌드가 실행 파일마다 모듈 표를 만들고 진입점 모듈 소스에 `ROCKS_MODULE_NAME`을 넣는다. 모듈 진입점은 상수 기술과 함수뿐이어서 프로세스 진입점(`ROCKS_IMPLEMENT_APPLICATION`이 만드는 `wmain`·`main`)과 실행 순서가 얽히지 않는다. Launch는 아직 `ModuleLoader`에 의존하지 않는다.
  - **ML1(그 문서, 이 구현 병합 뒤).** Launch가 로더를 소유한다(`ApplicationContext`, 전역 접근자 없음, MD4). 8.1절 7단계(도구 로그) 뒤·8단계(본문) 전에 로더 생성·`Prepare`·부트 모듈(Launch 의존 폐포)과 `EarliestPossible` 시작, 9단계에서 도구 로그 정리 전에 프로세스 묶음 종료를 넣는다. 실패 정리는 Launch 단계 실행기의 "시작한 단계만 역순"을 그대로 쓴다. Launch 모듈 규칙에 `ModuleLoader` 의존을 더한다. 이 문서 8.1·8.2절과 7.4절(`ApplicationContext`)의 고침은 ML1에서 한다(그 문서 8절).
  - **경계.** 크래시 수집 설치(1단계)와 명령줄 캡처(3단계)는 로더보다 먼저다. 모듈 `Startup`의 크래시도 그래서 보고된다. 로더는 크래시 경로에 끼지 않는다.
- **[플랫폼 계층](PlatformLayer.ko.md)**: "크래시 수집과 프로세스 진입" 절(공개 헤더·구현 파일·OS 경계·capability 표·동기화 primitive를 쓰지 않는 규칙)과 UE 대조 후속 목록의 크래시 처리 행을 더했다(LC8).
- **[동기화](Synchronization.ko.md)**: 크래시 경로는 그 잠금을 쓰지 않는다. `WaiterSignal`·주소 대기가 생겨도 크래시 경로는 OS 이벤트·세마포어를 유지한다. 크래시 때 사용자 공간 대기열 상태를 믿지 않기 위해서다.
- **breadcrumb(계획 29.4절 "사전 확보 breadcrumb 저장소")**: 이번 범위 밖이다. 연관 파일 칸(`SetCrashAnnotation`)을 그 시작점으로 둔다.

## 22. 결정

모두 확정이다(개정 2, 2026-11-18). D7·D25는 사용자 확인을 거쳐 권장안으로 확정했다. D4·D27·D28은 교차검증 뒤 조정자 결정으로 정했다.

| # | 결정 | 확정안 | 근거 |
|---|---|---|---|
| D1 | 수집기 위치 | PlatformBase(`Platform/CrashReporting.h`, Private/Win64·Posix·Common) | `platform::Fatal`·`RunThread`가 PlatformBase에 있고 OS 경계 코드다. 별도 모듈이면 PlatformBase에 함수 표 우회가 또 필요하다. Core 값은 C 문자열로 받는다 |
| D2 | 공통 실행 계층 | 새 Core 런타임 모듈 `Launch`(`Core/Source/Runtime/Launch`, 의존 Core·PlatformBase, `rocks::launch`) | 콘솔 도구가 Engine을 끌어오지 않는다. 게임 템플릿(ObjectRuntime만)도 쓴다 |
| D3 | 진입점 형태 | `ROCKS_IMPLEMENT_APPLICATION(Describe)` 매크로가 `wmain`·`wWinMain`·`main`을 만든다 | 진입 기호가 실행 파일 소스에 보인다. UE `IMPLEMENT_APPLICATION`과 같은 방향 |
| D4 | 덤프 작성 | 같은 실행 파일을 도우미 모드로 시작 때 띄운다(Full). 크래시 때 도우미가 `PssCaptureSnapshot`(VA clone)으로 캡처하고 "캡처 끝"을 알린다. 부모는 그 뒤 끝나도 된다. 덤프는 snapshot을 대상으로 `MiniDumpWriteDump`(`IsProcessSnapshotCallback`, 종류별 마감, CancelCallback)로 쓴다. 도우미가 없으면 W가 프로세스 안 fallback | 덤프 시간을 부모 종료와 분리한다(Full 덤프 포함). 버전이 늘 맞고 배포·빌드 변경이 없다. 크래시 뒤 생성에 기대지 않는다. 한계: 부모가 멈춰 있는 시간은 캡처 동안뿐이지만, 그 상한은 도우미에 달려 있다. 부모 쪽 `captureTimeout`이 끝을 보장한다 |
| D5 | 처리 순서 | 도우미 있음: 캡처 → flush(덤프 작성과 병행) → 최종 메타데이터. 도우미 없음: flush → 프로세스 안 덤프 → 메타데이터 | 19.5절 |
| D6 | 크래시 종료 방식 | filter 안에서 `TerminateProcess`(stack을 풀지 않는다). WER로 넘기지 않는다. Embedded는 예외(D27) | 깨진 stack unwind 재실패 방지, 중복 보고 방지 |
| D7 | Windows 인자 | `wmain`·`wWinMain`(`__wargv`)만 쓴다. `main(char**)`는 Windows에서 지원하지 않는다. AppInfo 12절의 `GetCommandLineW` 직접 분해를 지운다(LC8에서 그 문서를 고쳤다, AppInfo 개정 3) | 사용자 확인으로 확정(권장안) |
| D8 | 종료 코드 | 하드웨어 예외는 예외 코드다(Windows Asan 구성은 ASan이 보고한 뒤 death callback이 Fatal 경로로 끝내므로 Rocks Fatal 코드 `0xE0524B46`다, LC5). Fatal·assert·abort·CRT hook은 Rocks 예외 코드 `0xE0524B46` 하나다(종류는 메타데이터와 예외 인자). 처리기 재귀는 `0xE0524B52`다. 겹친 스레드가 직접 끝낼 때는 소유자의 코드를 쓴다. POSIX는 이전 동작 복원 뒤 신호로 끝난다(Fatal 계열은 SIGABRT. POSIX Asan 구성의 하드웨어 fault는 ASan 보고 뒤 death callback의 Fatal 경로라 SIGABRT다, LC7) | 계획 2479행 "OS crash code 보존" |
| D9 | Fatal 경로 | 미설치·Off는 raise 없이 즉시 종료한다. 디버거가 있으면 `__debugbreak()` 뒤 종료한다. 그 밖은 noinline 보호 함수 안 `RaiseException`이고, `__try` 다음 줄은 무조건 종료다. abort를 부르지 않는다 | 반환 UB 제거, filter 교체와 무관한 경로, 순환 차단 |
| D10 | 디버거 | 크래시 때 판정한다. 기본은 디버거에 넘긴다. `reportWhenDebuggerAttached`로 강제한다 | 원래 실패 지점에서 조사한다(요구 11절) |
| D11 | 엔진 호스트 공용 단계 | 이번에는 Sandbox 본문에 둔다. Engine 게임 템플릿이 생길 때 호스트 모듈로 뺀다 | 사용처가 하나다 |
| D12 | ASan | 덤프·도우미를 끈다. 하드웨어 예외는 ASan에 넘긴다. Fatal 계열은 콜백·메타데이터만 남긴다. ASan 보고는 death callback으로 flush한다 | UE와 같은 이유(hang·DbgHelp 경합), 보고 보존 |
| D13 | 덤프 종류·마감 | 기본 `Standard`(11.7절). 늘 Standard `Minidump.dmp`를 먼저 쓰고, Detailed·Full은 같은 snapshot에서 `Minidump.detail.dmp`로 따로 쓴다. 마감은 기본 60 s, Detailed 120 s, Full 600 s(설정 가능)이다. 마감을 넘기면 취소하고 파일을 지운 뒤 `Cancelled`로 적는다(취소된 덤프는 열리지 않는다). CL4 전까지 선택은 환경 변수 `ROCKS_CRASH_DUMP`로 한다 | 기본으로 전체 메모리·환경 블록을 남기지 않는다(요구 8·12절) |
| D14 | 저장 경로 | `Saved/Crashes/<프로그램>-<시작 UTC>-<pid>/`, 설치 때 절대화, `%TEMP%\Rocks\Crashes` fallback | 로그와 같은 규약, 충돌 없음 |
| D15 | 메타데이터 | `CrashInfo.json`(14.3절) 예비본과 최종본, 디스크 쓰기는 도우미. 명령줄 원문·환경·사용자 이름은 수집하지 않는다. 경로는 실행 파일 이름과 상대 경로를 우선한다 | 불완전 표시, 개인정보(B14) |
| D16 | PDB | 링크에 `/PDBALTPATH:%_PDB%`를 넣는다. 보관은 `symstore`. `/Brepro`는 도입하지 않는다 | 3.5절 임시 경로 문제 |
| D17 | 로깅 연결 | 종료 콜백 등록(19.3절)이 `SetTerminationHook`을 대체한다. 칸 4개. Core는 정적 콜백 하나를 한 번 등록하고 해제하지 않는다(D33) | 소유권 합의 |
| D18 | 종료 요청 | Launch 원자 표시를 게임 스레드에서 `Engine::RequestExit`로 옮긴다. `RequestExit` 주석을 고치고 소유 스레드를 검사한다 | 20절 |
| D19 | GUI subsystem | 빌드 속성 `WindowsSubsystem`과 `ROCKS_IMPLEMENT_WINDOWED_APPLICATION`을 구현하고 시험 실행 파일 하나로 확인한다 | 요구 4절 |
| D20 | POSIX 수준 | 신호·종료 콜백·메타데이터까지 하고, 덤프는 `Unsupported`로 이전 처리기에 넘긴다(D32). Android NDK 컴파일은 단계마다 유지하고, macOS는 미확인으로 기록한다 | 요구 13절 |
| D21 | 시험 | 자식 probe는 기본 `Off`이고, 크래시 probe만 명시적으로 켠다. 시험용 짧은 예산을 쓴다. 덤프 판독은 C# 판독기와 고정된 cdb 명령을 필수로 한다 | 요구 14·15절 |
| D22 | 도구 로그 시작 실패 | stderr 한 줄을 남기고 본문을 계속한다(로그는 비상 경로로 나간다) | ShaderToolSpike 동작 보존 |
| D23 | AppInfo 범위 | `ProcessCommandLine`과 대화상자 억제만 구현한다. `AppInfo`·`RunTraits`는 AP1 잔여로 남긴다 | 요구 5절 |
| D24 | 제한 시간 | 부모 쪽은 캡처 10 s, 콜백 2 s, 총 13 s(fallback 35 s). 겹친 스레드 여유 5 s. 도우미 쪽은 D13. 정상 종료 때 도우미 대기 2 s | 10.6절 |
| D25 | 도우미 기본 범위 | 모든 Launch 실행 파일의 기본을 Full로 한다. 측정(18절) 결과 비용이 크면 Tool 기본을 InProcess로 바꾼다 | 사용자 확인으로 확정(권장안) |
| D26 | LG18 이름 | `CrashHandler.h`를 지우고 `CrashReporting.h`로 한다 | 범위가 크래시 수집으로 넓어졌다 |
| D27 | Embedded | Rocks는 호스트 프로세스를 끝내지 않는다. 처리기는 콜백·메타데이터까지만 하고 호스트 처리기로 넘긴다(`CONTINUE_SEARCH`, 전파). 도우미는 없고 종료는 호스트가 한다(8.8절) | 요구 4절 |
| D28 | 보고 없는 종료 기록 | 도우미는 `(code & 0xF0000000) == 0xC0000000`이고 `STATUS_CONTROL_C_EXIT`가 아닐 때만 `kind=UnreportedExit`로 기록한다. Ctrl-C·콘솔 닫기·harness kill·정상 `exit`·`_Exit`는 기록하지 않는다 | 크래시와 구분, 거짓 기록 방지 |
| D29 | W의 일 | heap을 쓰지 않는 일만 한다(서식·교환 블록·신호·콜백). 디스크 쓰기는 도우미가 한다. 도우미가 없을 때만 W가 쓰고 한계를 표시한다 | B1. 경로 API가 heap을 쓴다 |
| D30 | 힙 손상 VEH | `STATUS_HEAP_CORRUPTION`만 처리하는 VEH를 선택 기능으로 둔다(기본 끔) | UE `WCC` 1797–1803 주석 |
| D31 | CRT hook | SIGABRT·invalid parameter·purecall hook은 반환하지 않는다. 동작은 같다(8.5절 표). SIGABRT 처리기는 첫 줄에서 자신을 다시 건다 | UCRT `raise`·`abort` 동작(B3) |
| D32 | POSIX 넘기기 | 하드웨어 fault는 이전 동작을 복원하고 반환한다(원래 siginfo로 재전달). 보낸 신호는 복원 뒤 `raise`한다. Fatal은 이전 SIGABRT 동작을 복원한 뒤 abort한다 | tombstone·ReportCrash 보존(B8) |
| D33 | 로깅 콜백 등록 | Core가 첫 설치 때 정적 콜백 하나를 등록하고 해제하지 않는다. `LogReadScope`로 현재 시스템을 찾는다 | 로깅 세션 의견 |
| D34 | 보호 블록 모양 | `__try` 블록에는 noinline 함수 호출 하나만 둔다. 블록 안 명령이 직접 낸 fault는 그 `__try`가 잡지 못하고 바깥 처리기로 간다(clang 동기 SEH, 시제품 확인). `RaiseException`도 호출이므로 잡힌다 | Debug·Release ThinLTO 실험 |

## 23. 단계(LC)

단계 기호는 LC(Launch·Crash)다. AppInfo 문서의 CL(명령줄)과 겹치지 않게 했다. 구현자는 한 명이고, 단계는 순서대로 한다.

**모든 단계의 완료 기준.**
- worktree에서 `dotnet msbuild Rocks.proj -t:Test -p:Configuration=Debug -p:RocksTarget=Foundation`을 돌리고, 이어 `RocksTarget=EngineTests`를 돌린다.
- `Rocks.ToolTests format`과 `policy`를 통과한다.
- **Android NDK 컴파일을 유지한다.** PlatformBase·Core·Launch를 포함한 Android 빌드가 깨지지 않아야 한다.
- 리뷰어 1명이 본다.
- 크래시 단계(LC1a·LC1b·LC2·LC5)는 Release(ThinLTO) 구성에서도 해당 시험을 돌린다.

| 단계 | 범위 | 선행 | 검증 |
|---|---|---|---|
| LC0 | 이 문서 확정, 로깅 세션에 19절 전달 | — | 리뷰 |
| LC1a | PlatformBase 수집기 핵심.<br>- `CrashReporting.h`, 정적 기록, 보고 소유권(`ShutdownOwner` 포함), W(heap 없는 일만)<br>- unhandled filter, `RunWithCrashGuard`, `RunThread` 보호·stack 보장<br>- `Fatal`·`FatalAt` 재작성(미설치·디버거 분기, 무조건 종료)<br>- 종료 콜백 인터페이스, LG18 이관<br>- POSIX 최소 이식(신호 경로·소유권·이전 동작 복원, 컴파일 유지)<br>- probe는 `InstallCrashReporting`을 직접 부른다 | LG18이 engine-runtime에 병합되고 worktree 병합 지시 | probe: 주 스레드·`platform::Thread`·외부 스레드 접근 위반, `Log::Fatal`·assert·`platform::Fatal`, Off Fatal, stack overflow, 동시 실패(gate), 콜백 안 재귀, 멈춘 sink. 덤프가 아직 없으므로 종료 코드·stderr·콜백 결과로 단언한다 |
| LC1b | CRT hook(반환하지 않음), 프로세스 안 덤프(flush 먼저), W의 fallback 파일 쓰기와 `CrashInfo.json`, 덤프 사용자 stream, XSTATE 제거, `SuppressSystemErrorDialogs`, 힙 손상 VEH(선택) | LC1a | abort(주·외부 스레드)·invalid parameter·purecall·abort 재진입, 저장 경로 실패, C# 덤프 판독기, cdb 심볼 |
| LC2 | 도우미: 같은 실행 파일 모드, 교환 블록, 핸들 상속 목록과 상속 끄기, `PssCaptureSnapshot`·snapshot 덤프·종류별 마감·CancelCallback, 캡처 끝 신호, fallback 표, `UnreportedExit`, 정상 종료 `Shutdown` | LC1b | 도우미 시작 실패·캡처 무응답·중도 종료, Full 마감 취소(기본 덤프는 남음), `__fastfail`, Ctrl-C·harness kill·`_Exit(7)` 무기록, 정상 종료 때 도우미가 남지 않음 |
| LC3 | Core `ProcessCommandLine`, `Launch` 모듈(`RunApplication`·`ApplicationContext`·도구 로그·단계·종료 요청·Embedded), `EntryPoint.h`, Rocks.Build `WindowsSubsystem`·`/PDBALTPATH`, 빌드 식별 블록 | LC2 | 유니코드 인자, 정상 시작·종료, 초기화 실패 역순 정리, 종료 코드, GUI subsystem 시험 실행 파일, Embedded 시험 호스트, Rocks.Build 단위 시험(링크 인자) |
| LC4 | 실행 프로그램 적용(16절), TestSupport·ShaderToolSpike 정리, 템플릿, SDK 내보내기, probe 설치를 Launch 설정으로 이전, 진입점 policy 검토 | LC3 | 기존 시험 전체(Foundation·EngineTests·ToolTests·Commandlet 시험·Sandbox self-test·Scaffold 시험) |
| LC5 | 디버거·ASan 정책 확인, Release·ThinLTO 전체 크래시 시험, 비용 측정(18절, D25 재검토) | LC4 | cdb 디버거 시험, Asan 구성 `asan-uaf`·접근 위반, 측정 보고 |
| LC6 | 엔진 연계: `Engine` 단계 표시, 종료 요청 다리(Sandbox PumpEvents), `RequestExit` 규약·검사, unhandled filter 교환 확인(즉시 복원), 로그·trace 파일 연관 칸(로깅 세션과 함께) | LC5 | 초기화·종료 중 크래시의 `stage` 값, `RequestTermination` 시험 |
| LC7 | POSIX 마무리: 메타데이터·capability, Launch POSIX 진입점, 에뮬레이터 시험(가능할 때) | LC6 | Android 실행은 가능할 때만 하고, 못 하면 미실행으로 기록한다. macOS는 미확인으로 기록한다 |
| LC8 | 문서 반영: LoggingSystem 14.1.8(로깅 세션), AppInfo 12절(D7)·18절 AP1 범위, PlatformLayer, `Docs/Implementation` 구현 문서, 묶음 끝 리뷰 | 전부 | 문서 리뷰 |

## 24. 미확인 사실

### 24.1 시제품으로 확인한 것(2026-11-18)

**실험 환경.**
- 코드는 저장소 밖 scratchpad `crash-spike\`에 두었다.
- 툴체인은 clang-cl·lld-link 21.1.6(저장소 빌드와 같은 Swift 툴체인)이다.
- 인자는 `ActionGraph.cs`에서 그대로 옮겼다. 공통 인자는 `--no-default-config /W4 /WX /GR- /EHs-c- /Z7`이다.
  - Debug: `/MDd /Od`
  - Release: `/MD /O2 -flto=thin`
  - 링크: `-fuse-ld=lld /DEBUG:FULL /INCREMENTAL:NO`

| 항목 | 결과 | 근거 | 반영 |
|---|---|---|---|
| noinline 보호 함수의 `__try/__except`(`/EHs-c-`) | **확인**(Debug·Release ThinLTO) | 주 스레드·`CreateThread` 스레드에서 접근 위반·`RaiseException(0xE0524B46)`·stack overflow(32 KiB 보장)가 모두 보호 filter로 들어왔다. 정적 저장소 복사와 `TerminateProcess(예외 코드)`까지 확인했다. 종료 코드는 `0xC0000005`·`0xE0524B46`·`0xC00000FD`였다. 보호 없는 스레드는 unhandled filter로 갔다. 예외 주소는 Release에서도 `FaultAccess+0x7` 안이었다(noinline 유지). **블록 안 명령이 직접 낸 fault는 그 `__try`가 잡지 못하고** unhandled filter로 갔다(두 구성 모두) | D34 |
| 예외 CONTEXT의 XSTATE | **확인** | filter가 받은 CONTEXT의 `ContextFlags`에 `CONTEXT_XSTATE`가 켜져 있었다. 사본에서 지워야 한다(10.1절 그대로) | — |
| `PssCaptureSnapshot`(VA clone)과 snapshot 덤프(별도 도우미) | **확인** | `IsProcessSnapshotCallback → S_FALSE`, `ClientPointers=TRUE`(clone 안 정적 EXCEPTION_POINTERS)로 덤프를 썼다. 부모는 `captureDone`을 받은 뒤 끝났고, 덤프는 부모 종료(`0xC0000005`·`0xC00000FD`) 뒤에 써졌다. cdb `.lastevent`·`.ecxr`·`kn` 결과는 다음과 같았다.<br>- 예외 스트림 있음, 이벤트 스레드 = 실패 스레드<br>- Release ThinLTO stack `FaultAccess+0x7 → RunGuarded → ThreadMain`, 레지스터 정상<br>- 주 스레드 stack overflow 덤프는 `Recurse` 프레임이 이어짐<br>- private PDB 심볼 해석 | 11절 그대로 |
| 최소 권한 | **확인**(문서 추정을 고침) | 결과는 다음과 같다.<br>- Standard는 `CREATE_PROCESS \| QUERY_INFORMATION \| SYNCHRONIZE`면 된다. `VM_READ`는 필요 없다.<br>- `QUERY_LIMITED_INFORMATION`으로 바꾸면 캡처가 `ERROR_ACCESS_DENIED`다. `CREATE_PROCESS`를 빼도 같다.<br>- `SYNCHRONIZE`를 빼면 도우미의 부모 대기가 실패해 부모가 캡처 시간 초과(10 s)까지 기다렸다.<br>- 핸들 캡처(Detailed)는 `DUP_HANDLE`이 없으면 `ERROR_ACCESS_DENIED`다 | 11.2절 표 |
| 캡처 동안 부모가 멈추는 시간 | **측정** | 소형 프로세스는 대개 약 1 ms였고 가끔 40–106 ms였다. 2 GB를 쓴 프로세스는 185–252 ms였다. 덤프 작성 시간은 Standard 약 13 ms, Detailed+Full 512 MB 1.7 s, Full 2 GB 6.3 s(2.17 GB)였다. 모두 부모 종료 뒤의 시간이다 | 18절, D24 값 유지 |
| 덤프 종류와 캡처 플래그 | **반증 1건** | `MiniDumpWithHandleData`를 핸들 캡처 없는 snapshot에 쓰면 `ERROR_NOT_FOUND`(0x80070490), 0 byte였다 | 11.7절 표 |
| CancelCallback으로 자른 덤프 | **반증** | `ERROR_CANCELLED`(0x800704C7)로 끝났다. 남은 파일(15 KB·549 MB)을 cdb가 열지 못했다(HRESULT 0x80004005). "잘린 덤프 표시"는 쓸 수 없다 | D13·11.4·11.5·11.7·10.6·17절: 기본 Standard 덤프를 먼저 쓰고, 큰 덤프는 별도 파일로 쓴다. 취소된 파일은 지우고 `Cancelled`로 적는다 |
| lld-link `/PDBALTPATH:%_PDB%` | **확인** | CodeView `PDBFileName`이 `spikealt.pdb`(파일 이름만)가 됐다. 이 옵션이 없으면 전체 경로였다. cdb `-y <bin>`으로 private 심볼이 해석됐다 | D16 그대로 |
| UCRT `wWinMain`의 `__wargv` | **확인** | `/SUBSYSTEM:WINDOWS` 실행 파일(Debug·Release)에서 `__argc=6`이었다. 한글·이모지·안쪽 `"`·빈 인자가 그대로 왔다. `__argv`는 null이다(좁은 문자는 채우지 않음) | D7 그대로 |
| UCRT SIGABRT | **확인 + 한계 발견** | 처리기 안에서 다시 건 이전 값이 `SIG_DFL`이었다. `_set_abort_behavior(0, ...)`에서 처리기가 반환하면 종료 코드가 3이었다. `raise(SIGABRT)`는 처리기 뒤 반환했다. 소스를 직접 열어 보니 `signal.cpp` 506–520(SIG_DFL로 되돌림), `abort.cpp` 61–89(반환 뒤 `__fastfail` 또는 `_exit(3)`)였다. SIGABRT 동작 칸은 프로세스 전역(`signal.cpp` 26)이라, 다시 걸기 전 창에 다른 스레드의 abort가 들어오면 보고 없이 `_exit(3)`으로 끝난다 | 8.5절 표 한계 |
| 힙 손상 VEH(LC1b) | **확인**(Debug·Release ThinLTO) | 사설 heap 블록 머리를 덮고 해제하자 `0xC0000374`가 `AddVectoredExceptionHandler(1, ...)` 처리기에 첫 기회로 왔다. fail-fast 전에 보고(콜백·`CrashInfo.json`·프로세스 안 덤프)를 마치고 종료 코드 `0xC0000374`로 끝났다. 힙 lock을 쥔 채 오는 경로라 W의 heap 사용은 막힐 수 있다(총 예산이 끝을 보장) | 8.6절 그대로 |
| CRT hook(LC1b) | **확인**(Debug·Release ThinLTO) | `abort()`(주 스레드·`CreateThread` 스레드)·`_close(-1)` invalid parameter·생성 중 순수 가상 호출이 모두 처리기로 들어와 반환하지 않았다. 종료 코드는 3이 아니라 `0xE0524B46`이었다. Debug UCRT는 invalid parameter에 식·함수·파일·줄을 준다. cdb stack은 `RaiseException → RaiseFatalException → FailFatally → AbortSignalHandler → ucrtbased!raise → abort → AbortFromProbe` 순서였다 | 8.5절 그대로 |
| harness 방식 자식과 pipe EOF | **확인** | PowerShell .NET `Process`로 stdout·stderr를 리다이렉트하고 `ReadToEnd`로 C# harness와 같게 실행했다.<br>- 핸들 상속 목록 + `DETACHED_PROCESS` 도우미: 부모 종료 8 ms 뒤 EOF가 왔고 도우미는 살아 있었다.<br>- 대조군(목록 없이 상속): EOF가 도우미가 끝날 때까지 8.2 s 늦었다.<br>- 크래시 부모(처리기·도우미 설치)도 종료 즉시 EOF가 왔고 덤프는 그 뒤 완성됐다 | 11.2절 그대로 |

### 24.2 남은 미확인

1. **소멸자와 `__try`.** 소멸자가 있는 지역 객체와 `__try`를 섞을 때의 clang 진단을 확인하지 않았다. 보호 함수에 지역 객체를 두지 않는다(D34).
2. ~~**ASan과 우리 filter.**~~ LC5에서 확인했다(13.2절). ASan은 `SetUnhandledExceptionFilter`를 가로챈다: Asan 구성 내장 호스트 시험에서 호스트가 건 filter 대신 ASan이 접근 위반을 받아 보고하고 끝냈다(`a1-native.log` embedded-host). 치명으로 보지 않는 예외는 가로챈 사용자 filter로 넘긴다(LC5 리뷰 반영 뒤 사용자 예외 코드 시험). POSIX Asan은 LC7에서 구현했지만 실행하지 않았다(11번). 원래 메모:
   - `SetUnhandledExceptionFilter`를 가로채는지
   - `__asan_seh_interceptor`와 보고 순서
   - `CONTINUE_SEARCH` 때 ASan 접근 위반 보고가 나오는지
3. **`_set_thread_local_invalid_parameter_handler`.** 외부 라이브러리가 쓰는지 확인하지 않았다.
4. ~~**힙 손상 VEH.**~~ LC1b에서 확인했다(24.1절 표).
5. ~~**Ctrl-C.**~~ LC6에서 실제 콘솔 이벤트로 확인했다: LaunchProbe가 자기 콘솔에 `GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0)`을 보내고(POSIX는 SIGINT) 처리기가 종료 요청 표시를 켜는지, 두 번째 요청과 처리기 없음은 `STATUS_CONTROL_C_EXIT`로 끝나는지(`Native.Launch.termination-signal`). POSIX SIGINT 경로는 LC7에서 Android 에뮬레이터로 확인했다(처리됨 0, 두 번째 요청·처리기 없음 130). job 아래 kill은 아래 원래 메모대로 남는다. 원래 메모: **Ctrl-C와 job kill.** 실제 콘솔 Ctrl-C(`GenerateConsoleCtrlEvent`)는 보내지 않았다. LC2 시험은 `STATUS_CONTROL_C_EXIT`로 끝나는 자식과 .NET `Process.Kill`(자식만)로 "기록하지 않음"과 도우미가 부모를 따라 끝나는 것을 확인했다. job 아래 kill에서 도우미가 함께 끝나는지는 확인하지 않았다.
6. ~~**도우미 비용.**~~ LC5에서 쟀다(18절 LC5 측정). dbghelp 로드만 따로 재지 않았다(설치 비용에 포함).
7. **symstore.** lld PDB를 받는지 확인하지 않았다(`/PDBALTPATH`와 cdb 해석은 확인).
8. **`WerSetFlags(WER_FAULT_REPORTING_NO_UI)`.** fast-fail 대화상자에 효과가 있는지 확인하지 않았다.
9. ~~**Asan의 stack 보장.**~~ LC5에서 확인했다: Asan 구성 주 스레드 stack overflow에서 ASan 보고 뒤 death callback의 공통 보고가 끝났다(`CrashInfo.json`·콜백 줄). filter 자체는 Asan에서 하드웨어 예외를 곧바로 넘기므로 stack을 거의 쓰지 않는다. 원래 메모: 32 KiB 안에서 filter가 끝나는지 Asan 구성에서 확인하지 않았다(LC5). Debug·Release ThinLTO는 LC1b의 실제 filter(소유권·문맥 복사·스레드 이름 복사·대기)로 주 스레드·`platform::Thread` stack overflow 덤프까지 확인했다.
10. **캡처 시간의 편차.** 소형 프로세스에서 1 ms와 106 ms가 섞였다. 원인(첫 실행, 시스템 부하 등)을 보지 않았다. `captureTimeout` 10 s 안이다.
11. **POSIX.** LC7에서 Android NDK(x86_64 Debug)로 컴파일하고 에뮬레이터(`emulator-5554`, x86_64, API 35)에서 C++ 시험(CrashReporting·ProcessEntry·Launch·ProcessCommandLine)과 크래시·Launch probe를 직접 실행했다(25절 LC7). 남은 미확인:
    - macOS: 빌드 환경이 없다. 컴파일·실행 모두 하지 않았다. 신호 처리·death callback은 Android와 같은 POSIX 코드이고, 차이(세마포어 대신 pipe, ReportCrash)는 코드 검토만 했다.
    - POSIX Asan 구성(macOS만 해당. Android Asan은 빌드가 거부하고 Linux는 대상 플랫폼이 아니다): 빌드·실행하지 않았다. death callback·`DeferToSanitizer`·종료 코드 SIGABRT는 코드 검토만이다(13.2절).
    - macOS `si_code` 범위(`SI_USER` 0x10001 등)로 보낸 신호를 가리는 판정(8.7절)은 XNU 헤더 값 기준의 코드 검토만이다.
    - fault를 반환해 재전달하는 방식이 Android debuggerd와 맞는지: 에뮬레이터에서 원래 신호(SIGSEGV 139, SIGABRT 134)로 끝나는 것만 봤다. tombstone 생성은 보지 않았다(adb shell 실행 파일은 앱 프로세스가 아니다).
    - Android 실제 기기·arm64, Android 앱 수명주기 진입점: 실행하지 않았다.
    - ToolTests(C#) 시험은 POSIX에서 돌리지 않았다. 에뮬레이터에는 같은 probe를 adb로 직접 돌려 종료 코드와 출력 줄을 손으로 대조했다.
12. **Windows arm64.** 빌드·실행하지 않았다.
13. **UE 행 번호.** 4절은 조사 에이전트가 확인했다. 직접 재확인한 곳은 `WCC` 1508–1516, 1826–1846과 `LW` 255–268이다.
14. **`InstalledSdk.cs`.** SDK 내보내기 방식을 확인하지 않았다(LC4).
15. **Sandbox self-test 시간.** 자식 `--engine`이 도우미를 띄울 때의 영향을 재지 않았다.
16. **사용자 프로필 경로.** 덤프 모듈 경로에 사용자 이름이 남는다(14.3절).

## 25. 변경 이력

- 개정 1(2026-11-18): 초안. Rocks 진입점·Fatal·스레드·LG18(로깅 브랜치 `4c09b09b`·`7b9b4717`·`6d8ed9ed`·`5ecb29a9`)·빌드 심볼을 조사했다. UE 5.8.4 Launch·CrashContext·RunnableThread·CrashReportClient·Unix를 조사해 대응표·API·흐름·보호 범위·도우미 프로토콜·정책·경로·시험·결정 D1–D26·단계 LC0–LC8을 정했다.
- 개정 2(2026-11-18): 교차검증 지적과 로깅 세션 의견을 반영했다.
  - **사용자 결정.** D7(`wmain`·`wWinMain`만)과 D25(도우미 기본 Full) 확정.
  - **조정자 결정.**
    - 도우미가 `PssCaptureSnapshot`(VA clone)으로 캡처한 뒤 snapshot 덤프를 쓴다. 종류별 마감·CancelCallback을 둔다(D4·D13).
    - 보고 없는 종료 기록은 `0xC…`이면서 `STATUS_CONTROL_C_EXIT`가 아닐 때만 `kind=UnreportedExit`로 남긴다(D28).
    - Embedded는 호스트로 전파한다(D27, 8.8절).
  - **교차검증 반영.**
    - W는 heap 없는 일만 하고 디스크 쓰기는 도우미가 한다. fallback은 flush를 먼저 한다(D29).
    - 도우미 핸들은 생성 뒤 상속을 끈다.
    - CRT hook은 반환하지 않는다. SIGABRT는 처리기를 다시 건다(D31).
    - 겹친 스레드는 소유자 종료를 기다린다. W 분기를 둔다.
    - Fatal 흐름: 미설치·Off는 즉시 종료, 디버거 분기, `__try` 뒤 무조건 종료(D9).
    - `Shutdown` 기록 순서를 정했다.
    - POSIX는 이전 동작을 복원한다(D32).
    - 시험 계획: gate 동시 실패, CRT 경로, Off Fatal, Ctrl-C·kill 무기록, 짧은 예산, 고정 cdb 명령.
    - LC1을 a·b로 나눴다. 단계마다 Android 컴파일을 유지하고 병행 표기를 지웠다.
    - `RemoveCrashReporting` sentinel CAS를 넣었다.
    - CONTEXT XSTATE 비트를 지운다. 덤프에 Rocks 사용자 stream과 정적 기록 메모리를 넣는다.
    - 힙 손상 VEH(선택, D30)를 두었다. filter 확인은 교환 후 즉시 복원한다.
    - 경로 개인정보 규칙을 정했다.
    - `TerminationContext`에 멈춘 스레드 목록을 넣었다.
  - **로깅 세션 반영.**
    - 정적 콜백 1회 등록, 해제 없음(D33)
    - "Fatal flush 시도함" 표시가 있어도 마감 안에서 한 번 더 기다린다
    - 수집기 미설치 프로세스에는 flush가 없다는 점을 명시했다
    - `FileLogSink` 연관 파일 칸을 확정했다
- 개정 3(2026-11-18): 시제품 실험(scratchpad `crash-spike`, clang-cl·lld-link 21.1.6, Debug·Release ThinLTO)으로 24절 일부를 확인했다(24.1절).
  - 확인: noinline 보호 함수 SEH, XSTATE 비트, PssCaptureSnapshot·snapshot 덤프와 cdb 판독, `/PDBALTPATH`, `wWinMain`의 `__wargv`, UCRT SIGABRT 동작, harness pipe EOF.
  - 고침: 최소 권한은 `CREATE_PROCESS|QUERY_INFORMATION|SYNCHRONIZE`(+ Detailed는 `DUP_HANDLE`), `VM_READ` 불필요(11.2절).
  - 반증 반영: 취소된 덤프는 열리지 않으므로 기본 Standard 덤프를 먼저 쓰고 큰 덤프는 별도 파일로 쓴다(D13). 핸들 데이터는 핸들 캡처와 맞춘다(11.7절).
  - 새 결정: D34(보호 블록에는 noinline 호출 하나만). 새 한계: 동시 abort 경쟁 창(8.5절).
- LC1a 구현(2026-11-18, 커밋 전): 설계와 다르게 또는 더 자세히 정한 점.
  - `CrashReportingOptions`는 이 단계에 쓰는 칸만 둔다(`embedded`·`reportWhenDebuggerAttached`·`callbackBudgetMilliseconds`·`concurrentCrashGraceMilliseconds`). 출력 폴더·도우미·덤프 칸은 LC1b·LC2에서 더한다.
  - `TerminationContext::stoppedThreadCount`는 개수 값이 아니라 정적 원자 칸의 포인터다(콜백 도중 늘 수 있어 `AtomicLoad`로 읽는다).
  - `UnregisterTerminationCallback`은 handle을 참조로 받아 "없음" 표지로 되돌린다.
  - 호출자 복귀 주소는 `Base/Compiler.h`의 `ROCKS_RETURN_ADDRESS()`로 얻는다(policy `compiler-builtin` 허용 목록에 `__builtin_return_address`를 더했다).
  - `ROCKS_ASSERT` 실패는 `platform::FatalAt`(file·line)으로 끝나 종료 사유가 `Assert`다.
  - 로깅 연결(19.4절)도 이 단계에서 구현했다: 첫 `LogInstallScope` 때 정적 콜백 1회 등록, 프로세스 표시 `MarkFatalFlushAttempted`, `FlushForTermination(timeout, failingThreadId)`, 멈춘 스레드 목록으로 출력 스레드 실패 판정.
- LC1b 구현(2026-11-19, 커밋 전): 설계와 다르게 또는 더 자세히 정한 점.
  - **옵션.** `CrashReportingOptions`에 다음을 더했다. 도우미 칸(`helperExecutable`·`helper`·`captureTimeoutMilliseconds`·`helperDumpDeadlineMilliseconds`)은 LC2에서 더한다.
    - `programName`(필수, ASCII 글자·숫자·`.`·`_`·`-`, 최대 63 byte, 점만으로 된 이름 금지)
    - `outputDirectory`(최대 1023 byte, 빈 문자열 금지, 설치 때 절대 경로)
    - `identity`: JSON 객체의 **멤버 조각**(중괄호 없이, 최대 1023 byte)이다. `build` 객체에서 PlatformBase가 쓰는 `configuration`·`platform`·`architecture`·`sanitizer` 뒤에 붙는다. 올바른 조각인지는 Launch가 책임진다. 다 들어가지 않으면 조각 대신 `"identityTruncated": true`를 적는다.
    - `captureHeapCorruption`, `dumpDetail`, `inProcessDumpTimeoutMilliseconds`(기본 30 s, Detailed·Full의 추가 덤프까지 합친 마감), `metadataTimeoutMilliseconds`(기본 2 s, 대기 예산에만 쓴다)
  - **연관 파일 칸(로깅 세션용 확정).** `enum class CrashAnnotation : uint8 { LogDirectory, LogFile, TraceFile, Custom0, Custom1, Count }`, `MaxCrashAnnotationBytes = 512`, `bool SetCrashAnnotation(CrashAnnotation slot, const char* text, usize bytes) noexcept`.
    - 칸마다 seqlock(쓰는 쪽 하나, 내용은 8 byte word 원자 저장)이다. 할당·lock이 없고 설치 전에도 부를 수 있다. 값은 프로세스 수명 동안 남는다.
    - `bytes == 0`이면 칸을 비운다. 칸 범위 밖·512 byte 초과·`text == nullptr && bytes != 0`이면 false이고 칸은 그대로다.
    - 크래시 처리 스레드가 64번까지 다시 읽고, 끝내 일관되지 않으면 그 칸을 null로 적고 `incompleteReasons`에 `annotationTorn`을 남긴다.
    - FileLogSink는 파일을 열 때와 회전할 때 출력 스레드에서 `SetCrashAnnotation(CrashAnnotation::LogFile, path.Data(), path.ByteCount())`를 부른다. 메타데이터 키는 `associatedFiles.logFile`이다.
  - **`SetCrashOutputDirectory`.** 설치 전이거나 경로가 잘못되면 false이고 이전 값을 유지한다.
  - **`SuppressSystemErrorDialogs`.** 13.3절 그대로다(`SEM_NOOPENFILEERRORBOX` 포함). 프로세스 설정이고 되돌리지 않는다. POSIX는 할 일이 없다. TestSupport `PrepareAutomatedTestProcess`·`PrepareFatalTestProcess`·`PrepareVirtualMemoryAccessProbe`를 이것으로 바꿨다. ShaderToolSpike `PrepareSpikeProcess`는 LC4(Launch 설정)에서 바꾼다.
    - 제거 때 CRT abort 설정을 설치 전 값으로 되돌린다. 단 그 사이 `SuppressSystemErrorDialogs`가 불렸으면 `_WRITE_ABORT_MSG`·`_CALL_REPORTFAULT`는 다시 켜지 않는다.
  - **처리 스레드 실패(8.4절·10.2절과 다름).** 처리 스레드가 **종료 콜백 단계에서** 실패하면(콜백 안 접근 위반·Fatal·abort) 남은 콜백만 포기하고 파일 단계(메타데이터·덤프)를 이어 간다. 프로세스 안 덤프는 콜백 뒤에 쓰므로, 멈추기만 하면 그 크래시의 덤프가 사라지기 때문이다. 메타데이터에는 `reporterFailed: true`, `terminationCallbacks.status = "ReporterFailed"`, `incompleteReasons`에 `reporterFailed`가 남는다. 파일 단계에서 다시 실패하면 설계대로 `reporterFailed`만 남기고 멈춘다. 그 콜백 칸은 실행 중으로 남는다(19.3절 해제 계약 그대로).
  - **CRT hook.** 8.5절 그대로다. 세 처리기 모두 `FailFatally`(Fatal과 같은 경로)로 들어가 반환하지 않는다. SIGABRT 처리기는 첫 줄에서 자신을 다시 걸고, 메시지는 `"abort() was called"`다. invalid parameter는 Debug CRT가 준 식·함수·파일·줄을 UTF-8로 바꿔 메시지에 붙인다(stack 버퍼, 할당 없음). 종료 사유는 `Abort`·`RuntimeCheck`이고 종료 코드는 `RocksFatalExceptionCode`다.
  - **힙 손상 VEH.** `AddVectoredExceptionHandler(1, ...)`로 걸고 `0xC0000374`만 받는다. 내장 모드에서는 걸지 않는다.
  - **T의 총 대기.** 콜백 예산 + 프로세스 안 덤프 마감(덤프를 쓸 때만) + 메타데이터 예산 + 1 s다. 덤프를 쓰지 않는 경우(내장·sanitizer·DbgHelp를 열지 못함)는 덤프 항을 뺀다. 기본값은 2 + 30 + 2 + 1 = 35 s로 10.6절과 같다.
  - **파일.** 폴더 이름은 설치 때 `<program>-<프로세스 시작 UTC>-<pid>`로 만든다. 시작 시각은 Windows `GetProcessTimes`, POSIX 설치 시각이다. 최종 `CrashInfo.json`은 `CrashInfo.json.tmp`에 쓴 뒤 바꿔 넣는다. 파일을 썼으면 stderr에 `Rocks crash report: <폴더>` 한 줄을 남긴다. 두 폴더 모두 실패하면 `Rocks crash report directory unavailable; no crash files were written` 뒤에 `primary`·`fallback` 경로와 Win32 오류(POSIX errno)를 한 줄씩 남긴다.
  - **덤프 안 Rocks 정보.** 사용자 stream 종류는 `0x524B0001`("RK")이다. 내용은 `RocksCrashRecord/1` 한 줄과 예비 `CrashInfo.json`이다. `CommentStreamA`에 `Rocks crash <crashId> <program>: <메시지>`를 넣는다. `MemoryCallback`으로 정적 범위 넷(메시지·메타데이터 버퍼·연관 파일 칸·멈춘 스레드 칸)을 넣는다.
  - **`CrashInfo.json` 칸(14.3절에 더함).** 최상위 `message`, `reporterFailed`, `detailDump`, `limitations`(`dump.limitations` 대신 최상위), `reportDirectory`(`Primary`·`Fallback`), `dumpDetail`, `module.name`. POSIX는 `exceptionCode` 대신 `signal`(번호)을 쓴다. `helper`·`arguments`는 LC2·LC3까지 null이다. `failingThreadName`은 `platform::Thread` 이름(thread_local 상태를 읽음)이고 POSIX는 null이다(신호 처리기 안 TLS 접근을 피함). `incompleteReasons` 값은 `pending`·`reporterFailed`·`callbacksTimedOut`·`dumpNotWritten`·`detailDumpNotWritten`·`annotationTorn`·`metadataTruncated`다.
  - **POSIX.** 처리 스레드가 종료 콜백 뒤 `CrashInfo.json`만 쓴다(`dump.status = "Unsupported"`). 임시 폴더는 `$TMPDIR`(없으면 `/tmp`) 아래 `Rocks/Crashes`다. capability에 `Metadata`를 켰다.
- LC1b 리뷰 반영(2026-11-19, 커밋 전).
  - **메타데이터 잘림.**
    - 고정 문자열(열거 이름·크래시 id·프로그램 이름·시각)은 `Constant`로 reserve 자리까지 쓴다.
    - 가변 문자열(실행 파일 이름·단계·스레드 이름·`fatal.file`·`message`·연관 파일 5칸)은 문서 끝에 모은다. 칸마다 escape 뒤 상한(256·768·3072·1024 byte)을 둔다.
    - 잘리면 `incompleteReasons`에 `metadataTruncated`가 늘 남는다. identity·module 조각이 들어가지 않을 때도 같다.
    - `fatal.message`는 지우고 최상위 `message`만 둔다.
  - **dbghelp 없음.** 설치 때 정한 "덤프를 쓰려 하는가"를 따로 보관한다. DbgHelp를 열지 못하면 `dump.status = "Failed"`, `reason = "dbghelp.dll unavailable"`, `dumpNotWritten`이 남는다. `QueryCrashCapabilities`는 설치 뒤 실제 준비 상태를 반영한다.
  - **파일 쓰기 실패.**
    - 폴더를 만들었더라도 예비 CrashInfo.json이나 덤프 파일을 만들 수 없으면 임시 폴더로 한 번 옮긴다.
    - 최종본은 임시 파일을 쓰지 못하면 예비본을 그대로 두고, 바꿔 넣기만 실패하면 직접 쓴 뒤 임시 파일을 지운다.
    - `Rocks crash report:` 줄은 실제로 파일을 쓴 폴더에만 남긴다. 못 쓰면 경로·오류 줄을 남긴다. POSIX도 같다.
  - **덤프 마감.** 프로세스 안 덤프 마감은 `min(지금 + inProcessDumpTimeout, 소유자 마감 - 메타데이터 몫 - 1 s)`이다. 남은 시간이 없으면 `Skipped`다.
    - 덤프 파일은 다 쓸 때까지 삭제 예약(`FileDispositionInfo`) 상태로 둔다. 쓰는 프로세스가 도중에 끝나도 열 수 없는 파일이 남지 않는다. 도우미 덤프도 같다.
  - **PE 범위 검사.** CodeView 읽기를 Common의 바이트 판독기(`ParsePeCodeView`·`PeImageSize`)로 옮겼다.
    - e_lfanew·PE32+ Magic·NumberOfRvaAndSizes·SizeOfOptionalHeader·디버그 디렉터리·`AddressOfRawData + SizeOfData`·SizeOfImage가 모두 이미지 안이어야 한다. 하나라도 어긋나면 `module: null`이다.
    - 손상 이미지 단위 시험이 있다.
  - **limitations.** 처리 스레드가 디스크에 직접 쓰면 늘 `in-process`를 남긴다. 덤프를 건너뛴 사유(`embedded host owns crash dumps`·`sanitizer build`)도 남는다. 도우미가 쓰면 비어 있다.
- LC2 구현(2026-11-19, 커밋 전): 설계와 다르게 또는 더 자세히 정한 점.
  - **진입점 API.** `RunCrashHelperIfRequested(int argc, char** argv, int& exitCode)`(좁은 문자)로 두었다.
    - 도우미 인자는 ASCII다. 지금 진입점이 모두 `main(char**)`이기 때문이다. 넓은 문자 판은 LC3 Launch에서 더한다.
    - FoundationTests·Sandbox `main`이 맨 앞에서 부른다.
  - **명령줄.** `<exe> --rocks-crash-helper 1 <section> <parent> <request> <captureDone> <parentDone> <ready> <shutdown>`이다. 핸들 값은 10진수다.
  - **도우미 시작.** 설치(`InstallCrashReporting`)가 띄운다. `CrashReportingOptions`에 다음을 더했다.
    - `helper`(`Preferred`·`InProcess`)
    - `helperExecutable`
    - `captureTimeoutMilliseconds`(10 s)
    - `helperDumpDeadlineMilliseconds`(0이면 Detailed 120 s·Full 600 s)
    - 기본 덤프 마감은 60 s 고정이다.
    - 진단용 `CrashHelperProcessId()`와 `CopyCrashReportFolder()`를 공개했다.
  - **교환 블록.** 공유 메모리 64 KiB 한 덩어리다. 크래시 기록은 포인터 없는 고정 크기 사본(`CrashRecordSnapshot`)으로 넘긴다. 도우미는 같은 서식 함수로 예비·최종 CrashInfo.json을 만든다.
    - 상태: `Starting`·`Ready`·`CrashRequested`·`Captured`·`Withdrawn`·`DumpDone`·`Shutdown`
    - 도우미가 고른 폴더 칸(`Primary`·`Fallback`·`None`)과 오류 코드: 부모가 `Rocks crash report:` 줄이나 경로·오류 줄을 이 칸으로 남긴다.
  - **CrashInfo.json `helper`.** 도우미가 쓴 결과는 `{"status": "Running"}`이다. 프로세스 안 fallback은 다음 중 하나와 `error`·`exitCode`를 남긴다. 도우미를 쓰지 않는 설정이면 null이다.
    - `StartFailed`
    - `ExitedEarly`
    - `ExitedDuringCapture`
    - `CaptureTimedOut`
    - `CaptureFailed`(PssCaptureSnapshot 실패. 도우미는 더 쓰지 않고 부모가 fallback한다)
  - **캡처 실패와 폴더 없음.** 도우미가 폴더를 만들지 못해도 캡처는 한다(덤프는 없다). 부모는 경로·오류 줄을 남긴다.
  - **UnreportedExit.** 상태가 `Starting`·`Ready`일 때만 기록한다. 보고 중이거나, 요청을 거둬 부모가 프로세스 안에서 쓴 경우는 기록하지 않는다.
    - `kind = "UnreportedExit"`, `exitCode`, 설치 때 식별 정보만 담는다. 크래시 칸은 null이고 덤프도 없다.
  - **부모 총 대기.** 도우미가 떠 있으면 다음 합이다. 도우미가 캡처하면 실제 대기는 캡처와 콜백뿐이다.
    - 캡처 마감
    - 콜백 예산
    - 프로세스 안 덤프 마감(DbgHelp가 있을 때)
    - 메타데이터 몫
    - 1 s
  - **도우미의 콜백 결과 대기.** 콜백 예산 + 메타데이터 몫 + 1 s다. 넘으면 `terminationCallbacks.status = "Unknown"`이고 `callbacksUnknown`이 남는다.
  - **시험의 고장 난 도우미.** FoundationTests 진입점이 환경 변수 `ROCKS_CRASH_PROBE_HELPER`(`hang`·`exit-on-request`·`exit-now`)를 보고 TestSupport의 시험 도우미를 돌린다. 장애 주입은 시험 코드에만 있다.
  - **Ctrl-C 시험.** 콘솔 Ctrl-C를 실제로 보내지 않았다. 대신 `STATUS_CONTROL_C_EXIT` 종료 코드로 끝나는 자식으로 "기록하지 않음"을 확인했다(24.2절 5).
- LC2 리뷰 반영(2026-11-19, 커밋 전).
  - **도우미 순서(11.4절).** 요청 상태 확인 → 캡처 → 소유 CAS → 폴더·예비본·덤프 파일 열기(폴더 확정) → captureDone → 덤프 순서다. CAS를 이기기 전에는 파일을 하나도 쓰지 않는다. 리뷰 제안 순서와 달리 폴더 확정을 captureDone 앞에 두었다(리뷰 3의 첫 안). CAS 뒤의 I/O가 캡처 마감을 넘겨도 부모의 거두기 CAS가 Captured를 보므로 덤프가 둘 생기지 않는다.
  - **job.** `CREATE_BREAKAWAY_FROM_JOB` 먼저, `ERROR_ACCESS_DENIED`면 플래그 없이 다시 띄운다. 캡처는 먼저 `PSS_CREATE_BREAKAWAY_OPTIONAL`을 더해 청하고, 이 환경(Windows 11 26200)에서는 그 조합이 `ERROR_INVALID_PARAMETER`라 플래그 없이 다시 캡처한다(그때 VA clone은 job 안이다. 부모가 기본 덤프 끝까지 job을 닫지 않으므로 기본 덤프는 남는다). job 안이면 `helper.inJob = true`이고 부모 W가 기본 덤프 끝(옛 ready 이벤트를 `standardDump` 이벤트로 바꿨다)까지, Standard면 도우미 종료까지 기다린다. 교환 프로토콜 버전은 2다.
  - **교환 블록 불신.** 도우미는 시작 때 머리·부모 pid(`GetProcessId`)·덤프 종류·마감을 검사하고, 요청 때 기록·경로·범위를 지역 사본으로 옮겨 글자 칸 NUL과 길이 칸(메시지·identity·연관 파일·멈춘 스레드)을 배열 크기로 고친다. 부모 view는 평소 `PAGE_READONLY`다. 메타데이터의 identity 조각에도 상한을 둔다.
  - **핸들 상속.** 상속 불가로 만들고 CreateProcessW 바로 앞뒤에서만 켠다(설치는 시작 단계라 그 사이 다른 스레드가 자식을 만들 일이 없다).
  - **주 폴더 이중 칸.** `SetCrashOutputDirectory`는 쓰지 않는 칸을 채운 뒤 원자 index로 바꿔 끼운다. 교환 블록도 칸 두 개와 index다. 폴더 만들기는 경로를 쓰는 쪽 칸에 복사해 다룬다(공유 칸을 잠시도 바꾸지 않는다).
  - **fallback 때 예비본.** 덤프 파일만 실패해 임시 폴더로 옮기면 primary의 예비 CrashInfo.json을 지운다.
  - **시험.** C# 시험은 도우미 pid 줄을 읽는 순간 Process 객체를 잡는다. 새 시험: `dump-blocked`·`dump-blocked-inprocess`(primary에 파일이 남지 않음), `job-kill-on-close`(breakaway 금지 KILL_ON_JOB_CLOSE job에서 크래시 → job 닫힘, 덤프와 `helper.inJob`), `helper-gate-parent-first`(시험 gate가 실제 도우미의 캡처와 CAS 사이에서 부모 종료를 기다린다: 부모가 거두고 프로세스 안에서 쓰며 도우미는 파일을 쓰지 않는다. 반대 순서는 `main-access-violation`). gate는 시험 실행 파일이 도우미 진입 전에 거는 함수 포인터(`detail::SetCrashHelperCaptureHook`)이고 제품 코드에는 빈 칸만 있다.
- LC3 구현(2026-11-19, 커밋 전): 설계와 다르게 또는 더 자세히 정한 점.
  - **OS 경계.** 진입점 인자 변환·GUI 진입점 인자·종료 요청 처리기는 PlatformBase `Platform/ProcessEntry.h`에 두었다(Core·Launch에는 OS 코드를 두지 않는다, 정책 platform-boundary). `ConvertEntryArgument`는 wchar_t 판(Windows, `Platform/Win64/WideString.h`의 고정 버퍼 변환)과 char 판(POSIX 복사)이 있고, 플랫폼에 맞지 않는 판은 `Unsupported`다. `GetWindowedEntryArguments`는 UCRT `__argc`·`__wargv`다.
  - **`ProcessCommandLine`(7.3절).** Core `Core/App/ProcessCommandLine.h`. 두 판을 모든 플랫폼에 선언하고 맞지 않는 판은 인자가 없어도 `Unsupported`다. 인자 텍스트는 각 인자 끝 NUL을 포함해 `MaxTotalBytes` 안이고, 각 `Data()`는 NUL로 끝난다(설정 칸에 C 문자열로 넘길 수 있다). 실행 파일 경로 상한 `MaxExecutablePathBytes = 96 KiB`(UTF-16 32767 단위의 UTF-8 최대)를 더했다. 실패하면 캡처하지 않은 상태로 남는다. 저장소는 `detail::CommandLineStorage`이고 시험은 지역 저장소를 쓴다.
  - **Launch API(7.4절).**
    - `ApplicationDescription`에 `settings`(기본 설정)를 더했다. 이른 크래시 수집(8.1절 1단계)은 명령줄보다 먼저라 이 기본값으로 건다.
    - Full은 1단계에서 `CrashHelperPolicy::Deferred`(새 값)로 설치하고, 설정 단계에서 `platform::StartCrashHelper()`(새 API)로 도우미를 띄운다. configure가 수집 방식·덤프 설정·도우미 경로를 바꾸면 수집기를 떼고 다시 건다(그 사이 아주 짧게 보호가 없다). 폴더만 바뀌면 `SetCrashOutputDirectory`다.
    - `RunWindowedApplication(description)`을 더했다(wWinMain 매크로가 부른다). `ApplicationContext::Settings()`를 더했다.
    - 내장 호스트는 매크로 대신 자기 진입점에서 `RunApplication`을 직접 부르고 `crashReporting = Embedded`를 둔다. `RequestExitWithoutStaticTeardown`은 Embedded에서 무시한다.
    - 시작 단계 이름: `Launch.CommandLine`(실패하면 종료 코드 1과 `Rocks launch: the command line could not be captured (<ErrorCode>)`), `Launch.Settings`, `Launch.TerminationRequests`, `Launch.ToolLog`, 본문 `Main`. 단계 실행기는 `Launch/Private/LaunchStages.h`(시작한 단계만 역순 정리, Abort·Continue 실패)이고 시험이 가짜 단계로 순서를 확인한다.
    - 명령줄 캡처 전 진단(수집기 설치 실패·도구 로그 시작 실패)은 stderr 한 줄, 설정 단계의 경고(도우미 시작 실패·종료 요청 처리기 실패·잘못된 크래시 폴더)는 `Log::Warning`(카테고리 `Launch`, 도구 로그 전이면 비상 경로)이다.
  - **빌드 식별 블록(14.2절).** Launch가 `"engineVersion": "x.y.z", "toolBuildId": "<hex 64>"` 조각을 만들어 `identity`로 넘긴다. `CrashReportingOptions::argumentCount`(새 칸, 모르면 `UnknownArgumentCount`)로 메타데이터 `arguments.count`를 채운다.
  - **종료 요청.** 첫 Ctrl-C·Ctrl-Break(POSIX SIGINT·SIGTERM)는 `launch::RequestTermination()`만 부르고, 두 번째는 OS 기본 동작이다(Windows는 처리기가 FALSE를 돌려 기본 처리기로, POSIX는 이전 동작을 되돌리고 같은 신호를 다시 낸다). 판정은 Common `DispatchTerminationRequest`에 있고 시험은 OS 신호 없이 그것을 부른다. 실제 콘솔 Ctrl-C 전달 시험은 LC6에 남긴다(24.2절 5).
  - **내장 모드의 바깥 보호 프레임.** Fatal 안 보호 함수에서 보고를 마친 예외가 바깥 `RunWithCrashGuard`를 지날 때는 재귀가 아니므로 진단 줄 없이 호스트로 넘긴다(보고 완료 표시로 구분).
  - **진입점 매크로(D3).** `Launch/EntryPoint.h`만 `#if ROCKS_PLATFORM_WINDOWS` 분기를 쓴다(정책 platform-boundary의 예외로 등록, OS 헤더는 여전히 금지). `wWinMain`은 `struct HINSTANCE__`의 앞 선언으로 쓴다.
  - **빌드 규칙.** `NativeModuleInput.WindowsSubsystem { Console, Windows }`(실행 파일에만, 정적 라이브러리에 Windows는 입력 오류). 모든 Windows 링크에 `/PDBALTPATH:%_PDB%`를 넣었다. Rocks.Build.Tests `WindowsSubsystemTests`가 링크 인자·key·검증을 본다.
  - **시험 실행 파일.** `Programs/LaunchTests`(LaunchProbe 콘솔, LaunchWindowedProbe GUI subsystem, LaunchEmbeddedHost 내장 호스트)를 Foundation·EngineTests·Core 타깃에 넣었다. 내장 호스트의 OS 코드(`Private/Win64`·`Private/Posix`)는 정책 native backend로 등록했다. C# `Native.Launch.*`가 자식으로 띄운다.
- LC4 구현(2026-11-19, 커밋 전): 실행 프로그램 적용과 LC3 리뷰 반영. 설계와 다르게 또는 더 자세히 정한 점.
  - **적용한 진입점.** Sandbox, RocksCommandlet(Win64·Mac 공통 `Private/Common/Main.cpp`, 플랫폼 출력은 `Private/<Platform>/StandardOutput.cpp`), FoundationTests·CoreFoundationTests·EngineTests 공용 Main, CompressionBench·CryptoBench·LogBench·MathBench·SpatialIndexBench(조사 때 빠졌던 벤치), ShaderToolSpike, 게임 템플릿. 모두 `ROCKS_IMPLEMENT_APPLICATION`이다. 예외는 내장 호스트 시험 실행 파일(`Programs/LaunchTests/Private/{Win64,Posix}/EmbeddedHost.cpp`, 호스트를 흉내 내므로 자기 진입점)뿐이다. C# 도구와 시험이 만드는 임시 C++ 프로그램(통합 시험의 게임 main 등)은 대상이 아니다.
  - **진입점 policy.** `process-entry-point`: 엔진 소유 native 소스(Core·Engine·Programs·Templates, 외부 원본 제외)에서 `main`·`wmain`·`WinMain`·`wWinMain` 직접 정의는 실패다. 경고가 아니라 실패로 정했다(모든 검사가 실패만 있고, 경고는 무시되기 쉽다). 예외 목록은 `PolicyChecks.ProcessEntryPointExceptions`(EntryPoint.h와 내장 호스트 두 파일)다. `Launch` 모듈 의존 자체는 보지 않는다(매크로를 쓰면 의존이 필요하므로 링크가 잡는다).
  - **Launch API 변경(LC3 리뷰 2·3·7·8).**
    - 내장 여부는 진입 함수가 정한다. `CrashReportingMode::Embedded`를 지우고 `RunEmbeddedApplication(description, argc, argv)`를 더했다. 내장 진입에서 Full·InProcess는 내장 보고이고 Off만 끈다(configure가 내장과 독립 실행을 오갈 수 없다). 내장 진입은 도우미 모드로 분기하지 않고, 종료 요청 처리기를 걸지 않으며(호스트의 SIGINT·SIGTERM·콘솔 처리기 보존), `RequestExitWithoutStaticTeardown`을 무시한다.
    - `ApplicationSettings`에 크래시 예산 칸 네 개(`crashCallbackBudgetMilliseconds`·`crashCaptureTimeoutMilliseconds`·`inProcessDumpTimeoutMilliseconds`·`concurrentCrashGraceMilliseconds`, 0이면 기본값)를 더했다. 크래시 probe가 설치를 Launch 설정으로 옮기는 데 필요했고, 제품 호스트도 로그 flush 예산 등을 고칠 수 있다.
    - 설정이 수집 방식·예산을 바꾸면 떼기 전에 `platform::ValidateCrashReportingOptions`(새 공개 API, 이름 문법·문자열 길이·예산 범위)로 검사한다. 잘못되면 이전 설정을 유지하고 `Log::Warning`이다. 다시 걸기가 실패하면(자원 부족 등) 이전 설정으로 다시 걸고 경고한다.
    - `platform::StartCrashHelper()`는 `CrashHelperStart { Started, NotApplicable, Failed }`를 돌려준다. Launch는 `Failed`일 때만 경고한다(LC3의 경고는 시작 뒤 capability를 봐서 늘 꺼져 있었다).
    - `ApplicationDescription::commandLineFailureExitCode`(기본 1)를 더했다. Commandlet은 2(사용법 오류, README 계약)다. 실패 문구는 `Rocks launch: the command line arguments could not be captured (<ErrorCode>)`이고 상한 초과면 `; at most <n> arguments and <n> bytes`를 붙인다.
    - `ProcessCommandLine` 상한을 OS 한계 이상으로 올렸다: 인자 32768개, 텍스트 2 MiB(Windows 명령줄 32767 UTF-16 단위는 인자 최대 16384개·UTF-8 96 KiB, Linux·Android ARG_MAX 2 MiB, macOS 1 MiB). 정적 BSS는 약 2.5 MiB이고 쓴 만큼만 실제 메모리가 된다. Windows는 자식 프로세스로 상한을 넘길 수 없어 상한 시험은 지역 저장소 시험만 한다. POSIX는 ARG_MAX가 커서 짧은 인자 수십만 개를 넘길 수 있으므로 인자 수 상한은 OS 한계보다 작다(LC4 리뷰 반영, 헤더 주석에 절충 근거).
    - POSIX 인자는 UTF-8이어야 한다. 아니면 `InvalidArgument`로 시작하지 않는다(대체 문자로 바꾸면 파일 경로가 다른 파일을 가리킨다. 원문 byte 보존 판은 필요해지면 따로 둔다).
  - **PlatformBase(LC3 리뷰 1·4·5·6).**
    - VA clone job 대기: 도우미가 캡처 뒤 clone 핸들(`PssQuerySnapshot` VA_CLONE_INFORMATION)로 `IsProcessInJob`을 보고 교환 블록 `cloneInJob`에 쓴다(알 수 없으면 부모가 job 안인지). 부모는 캡처 끝 신호 뒤 이 값을 읽어 도우미나 clone이 job 안이면 기본 덤프까지 기다린다. 예산은 보수적으로 "부모가 job 안"이면 대기 몫을 넣는다. 시험 `Native.Crash.job-kill-on-close-breakaway`(breakaway 허용 KILL_ON_JOB_CLOSE job).
    - 늦은 캡처(마감 뒤 CAS로 확인한 Captured)는 `FinishCrashHelperReport`가 메타데이터 몫 안에서 캡처 끝 신호를 기다린 뒤 폴더 칸을 읽는다. 크래시 처리 예산에 도우미가 있으면 메타데이터 몫 하나를 더했다.
    - `UpdateCrashHelperFolders`는 크래시 소유자가 있으면 교환 블록을 읽기 전용으로 되돌리지 않는다.
    - POSIX 종료 요청 처리기는 설치 전 동작이 `SIG_IGN`인 신호에 걸지 않는다(nohup 등).
  - **실행 프로그램별 결정.**
    - Sandbox: `--engine`이면 configure가 도구 로그를 끈다(LogHost). `--self-test`는 Launch 도구 로그다. 그래서 자체 검사가 로그를 시작하지 못할 때 종료 코드 1로 끝나던 동작은 "로그 없이 계속"(D22)으로 바뀐다. `handleTerminationRequests`는 켜지 않았다: 첫 Ctrl-C가 원자 표시만 남기고 아무도 보지 않으면 Ctrl-C로 끝나지 않게 되므로 PumpEvents 다리(LC6)와 함께 켠다. `WriterAbandoned`는 LogHost·설정 문맥을 정적 저장소에 만들어 파괴하지 않고 `RequestExitWithoutStaticTeardown`이다(Launch가 크래시 수집을 정상 종료로 뗀 뒤 `_Exit`, 도우미가 UnreportedExit를 남기지 않는다).
    - Commandlet: 도구 로그는 Launch(Stderr)가 맡고 `commandlet_host::Run`은 명령만 돈다. 인수 상한(128개·4096 byte)과 문구 "Too many or invalid arguments."는 본문이 확인한다. 인자 UTF-8 변환은 Launch가 하므로 Windows의 길이 초과 문구는 macOS와 같은 "Invalid or oversized UTF-8 argument."가 됐다(종료 코드 2 그대로). 잘못된 UTF-16·UTF-8은 Launch 문구와 종료 코드 2다. Win64 출력(`dda5c1e6`: 4096 byte 상한 없음, 콘솔은 글자 경계 조각)은 함수 본문을 그대로 옮겼다.
    - 시험 실행기: 이름은 층별 정의 `FOUNDATION_TEST_RUNNER_LAYER`로 고른다(크래시 폴더·메타데이터 program). 본체는 Full(`--crash-dir <path>`로 폴더). configure가 자식 probe의 수집을 명시한다: `--negative-control crash-*`는 probe 설정(`ConfigureCrashProbe`: 방식·덤프 종류·시험 예산·도우미 경로·job 진입), `log-crash-*`는 InProcess와 `<dir>/Crashes`, 그 밖의 `--negative-control`·`--process-probe`·`--logger-failure-probe`는 Off(수집기 없는 Fatal 계약 그대로). probe는 설치를 확인한 뒤 실패를 낸다. 그래서 크래시 메타데이터의 program은 실행기 이름, build는 실제 빌드 식별이다(C# 시험 갱신). 고장 난 도우미·도우미 gate는 정적 초기화에서 `InstallCrashHelperTestMode`가 건다(Launch가 다른 무엇보다 먼저 도우미로 분기하므로). 고장 난 도우미 본체는 PlatformBase 시험 칸 `detail::SetCrashHelperOverride`로 넘겨 인자 해석을 PlatformBase가 한다.
    - 시험 probe 서명은 `int (*)(int, const char* const*)`로 바꿨다(Launch 인자를 const 그대로 넘긴다). `PrepareAutomatedTestProcess`는 지웠다(TestRunner가 대화상자를 끈다). `PrepareFatalTestProcess`는 `SuppressSystemErrorDialogs`를 부르는 얇은 함수로 남겼다(probe가 실패 지점에서 의도를 드러낸다). 시험 실행기 안의 크래시 시험은 "설치 안 됨" 대신 "Launch가 설치함"과 `ValidateCrashReportingOptions`를 단언한다. `ProcessCommandLine_CapturesOnce`는 실행기가 이미 캡처한 상태에서 두 번째 캡처가 AlreadyExists이고 값이 그대로인지 본다.
    - 벤치: 손 파싱이 `Span<const StringView>`다. SpatialIndexBench JSON의 `argv[0]`은 실행 파일 절대 경로가 됐다(전에는 셸이 준 철자).
    - ShaderToolSpike: `PrepareSpikeProcess`(Win64 파일 하나)를 지우고 `suppressSystemErrorDialogs`·도구 로그 Stderr다.
    - 게임 템플릿: `Run(context)` 본문, kind EngineHost, Full. `Game.Build.cs` 의존에 `Launch`를 더했다(템플릿 소스는 영어 주석 정책).
    - 내장 호스트 시험 실행 파일의 `SetErrorMode`는 그대로 뒀다(Rocks를 내장한 외부 호스트 자신의 정책을 흉내 낸다).
  - **시험 추가.** `Native.Launch.settings-warnings`(잘못된 예산 경고와 유지, 없는 도우미 경로 경고), `Native.Launch.off-no-teardown`, `Native.Launch.reinstalled-crash`(InProcess로 다시 건 뒤 프로세스 안 덤프·helper null), 내장 호스트의 `--in-process`·`no-teardown`, `Launch_FailedContinuingStageIsNotStopped`, `Launch_TerminationRequestFlag` 되돌림(`detail::ClearTerminationRequest`), `Policy.ProcessEntryPoints`, `CrashReporting_ValidationRejectsInvalidOptions`.
- LC4 리뷰 반영(2026-11-19, LC5와 함께, 커밋 전).
  - **재설치 롤백(중간 1).** 새 설정으로 다시 걸기가 실패하면 이전 설정과 **이전 폴더**로 되돌려 건 뒤, 새 폴더는 `SetCrashOutputDirectory` 분기가 따로 시도한다(실패하면 경고하고 설정의 폴더도 이전 값으로 되돌린다). 새 설정으로 설치에 성공하면 폴더는 그때 적용되므로 다시 바꾸지 않는다. 시험 `Native.Launch.settings-rollback`(작업 폴더와 합쳐 상한을 넘는 긴 상대 폴더 + `--in-process`).
  - **설계 본문(중간 2).** 7.1절(`ValidateCrashReportingOptions`, `CrashHelperStart`), 7.4절(예산 칸 넷, `commandLineFailureExitCode`), 8.1절(3단계 종료 코드, 5단계 검사·교체·롤백 순서, Embedded 문장 삭제), 16절 Commandlet 행, 25절 LC4의 상한 문장을 고쳤다.
  - **늦은 job 진입(낮음 3).** 부모의 job 여부를 도우미 시작 때 굳히지 않고 크래시 때(예산 계산) 다시 읽는다. 시험 `Native.Crash.job-after-helper`(도우미 시작 뒤 부모만 breakaway 허용 KILL_ON_JOB_CLOSE job에 들어감).
  - **인자 수 상한(낮음 4).** POSIX ARG_MAX로는 짧은 인자가 수십만 개일 수 있어 32768개는 OS 한계보다 작다. 상한은 그대로 두고(정적 view 저장소 비용과 절충) "OS 한계 이상" 주장을 고쳤다. 넘으면 CapacityExceeded로 시작하지 않는다.
  - **폴더 갱신 경합(낮음 5).** 읽기 전용으로 되돌린 뒤 소유자를 다시 보고 생겼으면 쓰기로 되연다. 남는 짧은 틈은 부트 단계 전용 계약 밖이라고 주석을 고쳤다.
  - **진입점 policy(낮음 6).** `_tmain`·`_tWinMain`, `INT`, `CALLBACK`·`PASCAL`·`WINAPIV`, `auto main() -> int`, `[[nodiscard]]`, 반환형이 윗줄에 있는 형태, `.cc`·`.cxx`·`.hpp`·`.hh`·`.inl`·`.ipp`를 본다. 진입점을 만드는 다른 매크로(전처리기 전개)는 lexical 검사로 잡지 못한다(한계로 남김).
  - **실행기 인자 해석(낮음 7).** `ConfigureRunner`가 `RunTestRunner`와 같은 위치 규칙을 쓴다(`--process-probe`는 첫 인자, `--logger-failure-probe`는 유일한 인자, 값을 받는 옵션의 값은 건너뜀). `crash-` 길이는 `CrashProbePrefixBytes` 상수다.
  - **Commandlet 인코딩 오류(낮음 8).** 동작을 바꾸지 않고 README·이 절에 적었다: 잘못된 인코딩은 Launch가 캡처하지 못한 경우라 stderr 쓰기 실패와 무관하게 2다(그 밖의 사용법 오류는 문구를 쓰지 못하면 1). Windows 잘못된 UTF-16 시험을 Commandlet 시험에 더했다.
  - **시험 추가.** `Native.Launch.install-invalid`(수집기 없는 프로세스에서 설치가 InvalidArgument로 거절하고 아무것도 걸지 않음). 못 더한 것: POSIX `sigint-ignored`(POSIX 실행 환경 없음), 내장 진입이 호스트의 콘솔 처리기·SIGINT를 보존하는지(설치 여부를 밖에서 볼 API가 없다. 코드상 내장 진입은 설치 단계를 건너뛴다), 늦은 Captured 폴더 줄(부모의 마감과 철회 CAS 사이에 gate가 필요하다).
- LC5 구현(2026-11-19, 커밋 전): 디버거·ASan 정책 확인, Release ThinLTO 크래시 시험 전체, 비용 측정.
  - **디버거(13.1절).** `ROCKS_CRASH_REPORT_WITH_DEBUGGER=1`을 Windows 설치 때 읽는다(설정 `reportWhenDebuggerAttached`와 OR). cdb로 실제 실행해 확인했다: 기본은 filter가 넘겨 디버거가 두 번째 기회로 `FaultAccess`에서 멈추고 크래시 폴더가 없다(`Native.Crash.debugger-stops-at-fault`). 강제 옵션이면 디버거 아래에서도 도우미 덤프·`CrashInfo.json`을 남긴다(`debugger-forced-report`). Fatal은 디버거가 있으면 중단점으로 `FatalFromProbe` 프레임에서 멈춘다(`debugger-fatal-break`).
  - **ASan(13.2절).** death callback(`SanitizerDeathCallback` → 공통 Fatal 경로, reason `Sanitizer`)을 Windows Asan 구성에서 건다. 시험 실행기 Asan 구성은 덤프·도우미를 기대하는 크래시 시험 대신 `Native.Crash.asan-policy-*`(use-after-free·접근 위반·stack overflow·platform Fatal)를 등록한다. Launch·Logger 크래시 시험은 Asan 기대값(도우미·덤프 없음, Rocks Fatal 종료 코드)으로 분기했다. LC1–LC4 동안 Asan 구성을 돌리지 않아, 이 분기 전에는 CI Asan 단계에서 크래시 시험이 실패했을 것이다.
  - **시험 도구.** `Rocks.ToolTests`에 `--filter a,b`(이름 조각)를 더했다(반복 중 바뀐 부분만 돈다). probe 모드 `asan-uaf`, `inprocess-platform-fatal`, `job-after-helper`, LaunchProbe `threads <n>`(측정)·`install-invalid`를 더했다.
  - **측정과 D25.** 18절 LC5 측정. D25(모든 종류 Full)를 유지한다.
- LC5 리뷰 반영(2026-11-19, LC6와 함께, 커밋 전).
  - **POSIX Asan(높음 1).** `asan-policy-*` 블록을 Windows Asan으로만 한정했다. POSIX Asan은 일반 크래시 시험을 돌고, 하드웨어 fault의 종료 코드로 ASan 종료 코드(1)도 받는다(코드 검토: Rocks 처리기가 보고 뒤 이전 동작인 ASan 처리기로 넘김. SIGABRT는 ASan이 다루지 않음). 실행 확인과 POSIX death callback은 LC7이다(13.2절).
  - **최종 filter(중간 2).** Asan 건너뛰기를 보호 프레임·VEH에만 두고 최종 unhandled filter는 보고한다. probe `user-exception`(TestSupport `RaiseUserException`, 0xE0001234)과 시험(Asan·일반 둘 다)을 더했다.
  - **Asan 종료 코드(중간 3).** `asan-policy-*`가 종료 코드를 정확히 단언한다(Rocks Fatal 넷, 사용자 예외는 그 코드). Launch 크래시 시험의 Windows Asan 분기도 Rocks Fatal 코드를 단언한다.
  - **낮음 4–9.** 디버거 시험은 셸의 `ROCKS_CRASH_REPORT_WITH_DEBUGGER`를 지운다. 강제 보고 시험은 도우미 pid 줄을 단언하고 도우미 종료를 늘 기다린다. InProcess 주석을 제자리로 옮겼다. D8 표·LaunchProbe 주석(stack 보장은 늘 건다)을 고쳤다. `settings-rollback`은 `state`가 쓰는 크래시 폴더 줄(`CopyCrashReportFolder`)이 이전(기본) 폴더 아래인지 단언한다. death callback 전역 칸을 13.2절에 적었다.
- LC6 구현(2026-11-19, 커밋 전): 엔진 연계.
  - **Engine 단계 표시.** `Engine.Initialize`(Startup) → `Engine.Ready`(초기화 끝) → `Engine.Run`(Run 시작) → `Engine.Shutdown` → `Engine.Stopped`(Shutdown phase)를 단계가 바뀔 때만 `platform::SetRunPhase`로 남긴다(프레임마다 쓰지 않는다). 이름은 `Engine.h`의 `EngineStage*` 상수다. 진단·시험용 읽기 API `platform::CurrentRunPhase()`·`CurrentRunStage()`를 더했다. 시험 `Engine_MarksRunStagesForCrashRecords`(client 시작·프레임·종료 hook 안에서 읽은 단계). 이 값이 CrashInfo.json `stage`로 가는 경로는 LC1b·LC3 시험(`CrashProbe.Run`·`LaunchProbe.Crash`)이 본다. 초기화·종료 중 실제 크래시로 `stage`를 단언하는 시험은 더하지 않았다(엔진 hook에서 크래시를 내는 probe가 필요하다).
  - **종료 요청 다리.** Sandbox `TerminationRequestBridge`(EngineLoopHooks)가 PumpEvents에서 `launch::IsTerminationRequested()`를 보고 `RequestExit`를 부른다. `--engine`은 `handleTerminationRequests`를 켠다(첫 Ctrl-C는 다음 프레임 경계에서 정상 종료, 두 번째는 즉시 종료). 시험용 `--request-termination-at-frame N`(N번째 PumpEvents에서 같은 표시를 남김)으로 Sandbox self-test가 "RequestExit" 사유의 정상 종료(0)를 확인한다(순서는 프레임 번호로 정해진다). 실제 OS 이벤트 경로는 `Native.Launch.termination-signal`이 본다(24.2절 5).
  - **`RequestExit` 규약.** 주석을 "엔진을 소유한 게임 스레드(Initialize를 부른 스레드)에서만"으로 고치고 Debug·Development에서 소유 스레드를 assert한다(Release는 검사하지 않는다). 시험 `Engine_RequestExitFromAnotherThreadIsFatal`(자식 프로세스).
  - **unhandled filter 교환 확인.** `platform::CheckUnhandledCrashFilter()`(Windows: 교환으로 확인하고 우리 것이 아니면 그 filter로 바로 되돌림, 다른 플랫폼·미설치·내장은 true). Engine이 초기화 끝과 종료 시작에서 부르고 바뀌었으면 `Log::Warning`이다. 시험 `CrashReporting_ReplacedUnhandledFilterIsDetectedAndKept`(TestSupport 시험 filter로 바꿨다가 되돌림).
  - **연관 파일 칸.** 칸 정의(`CrashAnnotation::LogDirectory·LogFile·TraceFile`)는 LC1b에 있다. 엔진 쪽 연결은 Sandbox가 `--trace-file`을 연 뒤 `TraceFile` 칸을 쓴다. `LogFile`(FileLogSink가 파일을 열거나 회전할 때)과 `LogDirectory`는 로깅 세션(rocks-1e)이 병합 뒤 건다(합의). 이 단계에서 FileLogSink·LogSystem은 고치지 않았다.
- LC6 리뷰 반영(2026-11-19, LC7과 함께, 커밋 전).
  - **POSIX SIGINT 상속(중간 1).** 부모가 SIGINT를 무시로 물려준 환경(nohup·백그라운드)에서는 Launch가 처리기를 걸지 않으므로(의도) `termination-signal`이 결정적으로 실패한다. 두 가지를 다 했다. probe의 `--reset-sigint`가 configure(Launch 설치 전)에서 무시를 기본 동작으로 되돌린다. C#에서 자식의 처분을 고치는 방식은 쓸 수 없다(.NET `Process`는 시작 때 무시였던 신호를 자식에게 무시로 물려준다. 셸 `trap`도 비대화형 셸에서 물려받은 무시를 되돌리지 못한다). 그래도 무시면 signal-* 모드가 신호를 보내기 전에 "LAUNCH SIGINT IGNORED"와 종료 코드 65로 끝난다. C#은 `--reset-sigint`를 줬는데도 65면 되돌리기의 회귀로 보고 실패로 처리한다(처음에는 미실행 줄을 쓰고 넘겼으나, TestSuite에 건너뜀 상태가 없어 통과로 집계되므로 LC7 리뷰 1에서 바꿨다). 처분 확인은 Launch 설치 뒤 본문에서 한다(Launch는 무시 처분을 바꾸지 않으므로 설치 전 확인과 같다).
  - **Windows 콘솔 공유(낮음 7).** Ctrl-Break는 같은 콘솔의 모든 프로세스에 가므로 보내기 전에 `GetConsoleProcessList`가 1인지 본다. 아니면 "LAUNCH CONSOLE SHARED"와 종료 코드 66으로 끝나고 C#은 실패로 본다.
  - **해제 때 filter(낮음 2).** `RemoveCrashReporting`도 교환으로 확인해 우리 filter일 때만 이전 filter로 되돌리고, 남의 filter면 그대로 다시 건다(8.6절 "조용히 빼앗지 않는다"). 남의 filter를 다시 걸 때는 설치 전 filter 칸을 비우지 않는다. 그 filter가 우리 filter로 이어 주면 우리 filter가 설치 전 filter(CRT 등)로 계속 넘긴다(LC7 리뷰 6). 교환 창은 두 가지다. 해제의 두 호출 사이에는 다른 스레드의 처리되지 않은 예외를 설치 전 filter가 받는다(`RemoveCrashReporting` 주석). 확인(`CheckUnhandledCrashFilter`)의 두 호출 사이에는 우리 filter가 잠시 다시 걸린다(그 주석).
  - **경고 문구(낮음 3).** 교체한 filter가 우리 filter로 이어 주는 경우가 흔하므로 "may not be reported"로 낮췄다(Engine 경고, 헤더 주석).
  - **단계 한 워드(낮음 4).** 단계 이름과 phase를 한 워드(이름 표 번호 + phase)로 저장하고 크래시 기록·읽기 API가 한 번에 읽는다. 이름 표는 정적 문자열 포인터 64칸이고(처음 쓸 때 등록), 차면 이름 없이 phase만 남긴다. 등록 방식은 LC7 리뷰 5에서 칸별 CAS로 바꿨다.
  - **초기화 실패·Shutdown 반복(낮음 5·6).** Initialize 실패 경로는 Teardown 동안 `Engine.Shutdown`, 뒤에 `Engine.Stopped`다. Shutdown 재시도는 단계 쓰기와 filter 경고를 처음 한 번만 한다.
  - **문서·주석(낮음 8·9·10).** 20절 두 번째 요청 서술과 "초기화 중 요청", 13.2절 "보호 프레임·VEH", `Engine.h` "Initialize를 부른 스레드", Sandbox `RunEngine` 주석 위치, LaunchProbe.h 줄바꿈이 깨진 문장, EngineTests 버퍼 크기 상수.
- LC7 구현(2026-11-19, 커밋 전): POSIX 마무리.
  - **POSIX ASan.** death callback(`SanitizerDeathCallback` → 공통 Fatal, reason `Sanitizer`)과 `DeferToSanitizer`(Asan 구성에서 하드웨어 fault 신호의 이전 동작이 처리기면 곧바로 넘김)를 더했다. 종료 코드 판단을 "ASan 종료 코드 1"에서 SIGABRT(134)로 바꿨다(13.2절). C# 시험은 `asan-policy-*`를 모든 플랫폼 Asan 구성에서 돌고, POSIX 기대값은 SIGABRT와 `AddressSanitizer: SEGV`다(`user-exception`은 Windows만). Launch 크래시 시험의 POSIX Asan 기대값도 SIGABRT다. 실행하지 않았다(24.2절 11).
  - **메타데이터·capability.** POSIX `QueryCrashCapabilities`는 15절 표대로(OS 처리·종료 콜백·시스템 보고 넘김·메타데이터, 덤프 없음)이고 바꾸지 않았다. 메타데이터는 Asan 구성에서 limitations에 Windows와 같은 `sanitizer build`를 더했다(C# `asan-policy-*` 단언과 맞춤). 에뮬레이터에서 각 크래시 probe의 `CrashInfo.json` kind·reason이 맞는 것을 봤다.
  - **Launch POSIX 진입점.** 에뮬레이터에서 LaunchProbe로 확인했다: 종료 코드 전달(42), UTF-8 인자, 상태·크래시 폴더 줄, 크래시 폴더 작성, `--crash-off no-teardown`(7), SIGINT 처리됨 0·두 번째 요청 130·처리기 없음 130.
  - **에뮬레이터 결과(Debug, LC6 리뷰 반영까지 넣은 빌드).** C++ 시험 CrashReporting 18·ProcessEntry 3·`Launch_` 7·ProcessCommandLine 5 통과. 크래시 probe 종료 코드: 주 스레드 접근 위반 139, platform Fatal 134, abort 134, assert 134, 스레드 접근 위반 139, stack overflow 139(모두 `CrashInfo.json` 작성). SIGINT 무시 상속(셸 `trap '' INT` 아래): `--reset-sigint` 없이 "LAUNCH SIGINT IGNORED"·65, 있으면 처리됨 0·두 번째 요청 130·처리기 없음 130.
  - **엔진 단계 크래시 시험.** probe `engine-crash-stage <init|shutdown> <dir>`가 프로세스 안 수집을 걸고 client 시작(`OnEngineStart`)·`BeginShutdown` 안에서 접근 위반을 낸다. 시험 `Engine_CrashDuringInitializeAndShutdownRecordsTheStage`가 `CrashInfo.json`의 stage가 `Engine.Initialize`·`Engine.Shutdown`인지 본다(LC6에서 못 더한 것).
  - **안 한 것.** macOS 빌드·실행, POSIX Asan 구성, Android 실제 기기·arm64, tombstone 확인, POSIX에서 C# ToolTests 실행(24.2절 11).
- LC7 리뷰 반영(2026-11-19, LC8과 함께, 커밋 전, 빌드·시험은 다른 세션의 빌드 보류로 아직 돌리지 않음).
  - **SIGINT 미실행 집계(중간 1).** TestSuite에 건너뜀 상태가 없어 `[NOT RUN]` 뒤 return이 통과로 집계됐다. `--reset-sigint`를 줬는데도 65면 실패로 바꿨다(25절 LC6 리뷰 1 문구도 맞춤).
  - **ASan 문맥의 처리 스레드 실패(중간 2).** POSIX 신호 처리기가 처리 스레드 확인(`FailReporter`)을 `DeferToSanitizer`보다 먼저 한다. 처리 스레드에서 ASan이 직접 감지한 오류는 피할 수 없는 한계로 13.2절에 적었다.
  - **`DeferToSanitizer` 판정(낮음 3).** 코드대로 "이전 동작이 처리기면"으로 문서를 고치고, ASan 처리기인지 확인하지 않는 한계를 13.2절에 적었다.
  - **`handle_abort=1`(낮음 4).** 지원하지 않는다고 13.2절에 적었다. 리뷰가 낸 두 안 가운데 간단한 쪽이다. 다른 안(Fatal 경로에서 이전 SIGABRT 동작이 처리기면 SIG_DFL로 끝냄)은 Asan 구성에만 "이전 동작으로 넘긴다"(B8)의 예외를 하나 더 만들고, 기본값(`handle_abort=0`)에서는 차이가 없어 택하지 않았다.
  - **단계 이름 표(낮음 5).** 개수 카운터를 없애고 칸마다 nullptr → 이름 CAS로 채운다(`detail::RunStageNameTable`). 끝없이 오르는 카운터가 없어 칸을 덮어쓰지 않고, 같은 포인터를 동시에 처음 등록해도 한 칸만 쓴다. 공개 헤더에 `MaxRunStageNames`(64)·포인터 동일성 등록·표가 차면 이름 없음을 적었다. 시험: 가득 참(`CrashReporting_RunStageNameTableStopsWhenFull`), 동시 등록(`CrashReporting_RunStageNameTableConcurrentRegistration`, 스레드 넷이 gate 하나로 함께 시작해 서로 다른 순서로 128개를 등록하고 어느 엇갈림에서도 성립하는 불변식을 단언)이다. 둘 다 지역 표를 써서 프로세스 전역 표를 건드리지 않는다.
  - **해제 때 filter 사슬(낮음 6).** 남의 filter를 다시 걸 때 설치 전 filter 칸을 비우지 않는다. 해제 교환 창을 `RemoveCrashReporting` 주석(공개 헤더·구현)에 적었다.
  - **POSIX Asan 음성 측면(낮음 7).** `assert`·`log-fatal`·`abort-main`·`abort-unmanaged-thread`를 POSIX Asan에서도 일반 기대값(SIGABRT, reason Assert·Fatal·Abort)으로 등록한다.
  - **Asan 종료 코드(낮음 8).** Launch 크래시 시험은 POSIX Asan이면 134만 받는다. 셸 종료 코드는 이름 있는 상수(`PosixAbortExitCode` 등)다.
  - **엔진 단계 크래시 probe(낮음 9).** 크래시 폴더를 사용자 임시 폴더 아래에 만들고, `RunProcess`에 대기 상한(멈춘 자식을 끊는 장치)을 주고, probe가 `PrepareFatalTestProcess`를 부르고, 종료 코드 범위를 이름 있는 상수로 바꿨다.
  - **SIGINT 무시 보존 시험(낮음 10).** LaunchProbe `--ignore-sigint`(configure에서 SIG_IGN)와 `Native.Launch.sigint-ignored-preserved`(POSIX만: `--terminate`여도 Launch가 처리기를 걸지 않아 65)를 더했다.
  - **문서(낮음 11).** POSIX Asan은 macOS만 해당(Android Asan은 빌드가 거부, Linux는 대상 아님), 8.7절 처리 순서 2(`ucontext`를 복사하지 않음), 보낸 신호와 abort의 SIGABRT는 `pthread_kill`로 다시 냄.
  - **macOS `si_code`(낮음 12).** 보낸 신호 판정을 `IsSentSignal`로 모았다. Linux·Android는 `si_code <= 0`, macOS는 XNU의 `SI_USER`(0x10001)–`SI_MESGQ`(0x10005) 범위도 보낸 신호다(8.7절). macOS 실행 확인은 없다.
  - **주석 정리.** clang-format이 깨뜨린 줄바꿈(`ProcessEntry.h` 머리 주석, Win64 수집기 머리 주석, `Application.h` 두 곳, Sandbox `EngineHost.h`, `LaunchTests.cpp`)을 다시 썼다.
- LC8 문서 반영(2026-11-19, 커밋 전).
  - 이 문서: 머리말 개정 4, 7.3절 상한, 19.3절 인터페이스를 공개 헤더와 맞춤(멈춘 스레드 수 원자 포인터, `Unregister` 참조, `SetCrashAnnotation`), 19.6절 로깅 문서 대응, 21절·D7의 "LC8에서 고친다"를 반영 완료로.
  - [앱 정보와 명령줄](AppInfoCommandLine.ko.md) 개정 3: 12절 구현 상태와 상한, `GetCommandLineW` 직접 분해 삭제(D7), 7.1절·8.3절·17절·18절 AP1·D15.
  - [플랫폼 계층](PlatformLayer.ko.md): "크래시 수집과 프로세스 진입" 절, UE 대조 후속 목록의 크래시 처리 행.
  - `Docs/Implementation`: 사용 안내 [CrashHandlingAndLaunch.ko.md](../Implementation/CrashHandlingAndLaunch.ko.md)를 새로 썼다. `FoundationStatus.ko.md`에 2026-11-19 절을 더하고 로깅 줄의 "남은 것: crash 수집"을 고쳤다. `CoreUtilities.ko.md`의 `PrepareAutomatedTestProcess`를 Launch TestRunner로 고쳤다.
  - 로깅 문서([LoggingSystem.ko.md](LoggingSystem.ko.md) 14.1.8·15.1절, `Docs/Implementation/Logging.ko.md`)는 로깅 세션 소유라 고치지 않았다. 고칠 내용은 19.6절과 보고서에 따로 적었다.
- engine-runtime 이식(2026-11-20, `493f9a0d` 위, 커밋 전, 빌드·시험은 같은 기계의 다른 검증이 끝난 뒤).
  - **충돌 정리.** `Engine.cpp` include(둘 다), `Engine.h` 스레드 주석(Initialize를 부른 스레드 + Shutdown·소멸도 같은 스레드 + RequestExit 검사 + RequestFullCollection 예외), Commandlet·Sandbox·ShaderToolSpike 본문(origin의 sink 선언 순서 정정은 Launch 도구 로그로 옮겨 사라진 코드라 이쪽 판, Launch는 sink보다 LogSystem을 먼저 파괴한다), `FoundationTests.Build.cs`(Launch와 origin `ModuleLoader`·`ObjectDeveloperModules`를 모두), ToolTests policy 목록(`ForeignEngineBoundary`와 `ProcessEntryPoints` 둘 다), 게임 템플릿 README(origin의 모듈 진입점·GS1 서술과 이쪽 Launch 서술을 함께).
  - **POSIX 깨우기(origin `8749b85b`).** LG18 처리기를 세마포어에서 self-pipe로 바꾼 변경은 우리 수집기가 이미 pipe라 기능 차이는 없었다. 쓰는 쪽 `O_NONBLOCK`, `EINTR` 다시 쓰기, `fcntl` 실패 때 설치 실패, 이름 있는 끝 상수를 옮겼다(8.7절). base 이후 LG18 처리기의 다른 변경(겹친 크래시 대기·대체 stack·연쇄)은 없었고, 그 기능은 LC1a에서 이미 옮겼다(대체 stack은 같은 코드, 겹친 대기는 소유자 마감까지 기다리는 더 강한 판, 연쇄는 이전 동작 복원과 재전달).
  - **모듈 로더 ML0.** Launch 모듈과 Launch 시험 실행 파일에 모듈 진입점을 더했다(21절). origin이 새로 더한 실행 파일 GcBench·WorldBench는 `main`을 직접 정의하고 지운 `PrepareAutomatedTestProcess`를 불러 Launch 진입점으로 바꿨다(16절).
  - **GS1.** 이 구현의 새 소스는 모듈 생성 헤더를 include하지 않고 `GeneratedType<T>`를 쓰지 않는다. 게임 템플릿 `Main.cpp`는 origin 판(`<GameObject.generated.h>` 직접 include)과 Launch 본문이 합쳐졌다.
  - **rocks-73 정정.** 시험의 sink 선언 순서와 FoundationTests 결과 줄 뒤 `fflush(stdout)`는 origin 그대로다. 결과 줄 출력은 Launch 본문(`RunTestRunner`) 안에 있어 Launch 진입점에서도 그대로 돈다.
  - **Windows Asan의 로그 flush(로깅 세션 rocks-af 보고).** origin CI(run 44572075116, `4e02fd48`)의 Windows Asan에서 `Native.Logger.Crash.log-crash-access-violation`이 "log file record: False"로 실패했다. LG18 처리기는 unhandled exception filter에서 flush했는데, ASan이 `SetUnhandledExceptionFilter`를 가로채 접근 위반을 먼저 보고하고 끝내므로 그 filter가 돌지 못했다(24.2절 2에서 확인한 동작). 이 수집기에서는 ASan 보고 뒤 death callback이 공통 Fatal 경로로 가고, 크래시 처리 스레드가 종료 콜백(로그 flush)을 부른 뒤 끝난다(13.2절). 이식본 Windows Asan 구성에서 Logger.Crash 4건이 모두 통과했다(접근 위반은 기록이 남고 종료 코드는 Rocks Fatal). 기대값은 LC5에서 바꾼 대로다(Asan의 하드웨어 예외 종료 코드 = Rocks Fatal 코드, 기록은 남음).
- engine-runtime 병합(2026-11-20, 이식 커밋 `03523968` 위, origin `1d5f9270`까지).
  - **충돌.** Engine.cpp include(둘 다), EngineTests(origin 파일 고유 namespace `engine_tests_file` 안에 이쪽 시험을 두고 `rocks::test::RegisterProcessProbe` 선언은 그 밖 전역으로), GcBench(origin `--allocator-micro`·`--allocator-kind`를 Launch 인자 해석에 맞춤), WorldBench(origin ML2 모듈 로더 include와 Launch 진입점), ToolTests(origin `--only`와 이쪽 `--filter`를 둘 다), Sandbox EngineHost(origin ML2의 프로세스 로더 감싸기 `RunEngineHost`·`RunEngineHostWithModules`에 Launch 문맥·종료 요청 다리를 얹음), POSIX `CrashHandler.cpp`는 삭제 유지(origin 변경은 유니티 이름 변경 하나, 우리 판에 해당 이름을 따로 고침).
  - **ML2와 정적 소멸 없는 종료.** 멈춘 로그 출력 스레드를 포기하면 설정 문맥이 모듈 lease를 잡은 채 남는다. 그때는 프로세스 로더를 내리거나 파괴하지 않고 정적 저장소로 옮겨 둔 채 돌아온다(Launch가 `_Exit`로 끝낸다). 로더를 파괴하면 남은 lease로 Fatal이다.
  - **유니티 빌드(UB2·UB3).** 유니티를 켠 PlatformBase·FoundationTests에서 겹친 파일 지역 이름을 파일 고유 이름으로 바꿨다(CrashModule·CrashHelper·POSIX CrashReporting·POSIX ProcessEntry·CrashProcessProbe, 동작 불변). 새 시험 파일 넷(LaunchTests·CrashReportingTests·ProcessEntryTests·ProcessCommandLineTests)은 파일 지역 부분을 `<파일>_file` namespace로 감싸고 `detail::`을 `platform::detail::`로 한정했다.
  - **origin 결함 하나.** WorldBench가 ML2 뒤에도 옛 `ModuleRegistration`의 `modules.Unregister()`를 불러 컴파일되지 않았다(origin은 이 실행 파일을 빌드하지 않는다). 모듈 타입은 `DetachTypes`가 되돌리므로 그 호출을 뺐다.
