# 앱 정보와 명령줄 — 3. UE 5.8 `FApp` 조사

[목차](README.ko.md)

## 3. UE 5.8 `FApp` 조사

### 3.1 전체 모양

- **선언 수.** `FApp`(`APP` 35–885)은 static 선언 99개(함수 + 변수)를 가진 클래스다. 인스턴스가 없다. 같은 헤더 끝에 온라인 서버 판정 delegate가 있다(`APP` 889–900, 구현 `Runtime/Core/Private/Misc/CoreMisc.cpp:230,242`).
- **동기화.** 저장소는 private static 변수다(`APP` 842–884, 초기값 `APPC` 27–44). 원자 값도 잠금도 없다. 쓰는 쪽은 주로 게임 스레드의 부트 코드와 플랫폼·플러그인 모듈이고, 읽는 쪽은 아무 스레드다.
- **명령줄을 읽는 조회 함수.** 함수 안의 `static bool` 지역 변수로 처음 한 번 명령줄을 읽어 고정한다(`IsUnattended` `APPC` 278, `CanEverRender` `APP` 405, `IsAudioMuted` `APP` 455). 첫 호출이 `FCommandLine::Set` 전이면 assert다(`APPC` 276–277 주석).
- **시간.** 5.8에서 `FAppTime`(`AT` 22)으로 옮겨졌다. 게임 스레드만 `FAppTime::Set`으로 값을 바꾸고(`ATC` 62–67 `checkf(IsInGameThread())`), 태스크와 렌더 명령은 만들 때의 값을 상속 문맥으로 받는다(3.6절).

### 3.2 프로젝트·이름

| 멤버 | 의미 | 쓰는 곳·시점 | 읽는 스레드 | Rocks 현황 | Rocks 처리 |
|---|---|---|---|---|---|
| `GetProjectName` (`APP` 127), 저장소 `GInternalProjectName[64]`(`Runtime/Core/Public/CoreGlobals.h:450`) | 현재 프로젝트 이름 | 초기값은 빌드 매크로 `UE_PROJECT_NAME`(`Runtime/Core/Private/Misc/CoreGlobals.cpp:277`, 모노리식은 `IMPLEMENT_PRIMARY_GAME_MODULE` `Runtime/Core/Public/Modules/ModuleManager.h:1069–1102`). 실행 중 덮어쓰기는 `SetProjectName` 행 | 아무 스레드(Runtime 60개 파일에서 호출) | `HostSettingsOptions::projectName`(`Core/Source/Runtime/HostConfig/Public/HostConfig/HostSettings.h:36`)이 있으나 **채우는 호스트가 없다**. 쓰는 곳은 사용자 층 경로 `<userDataDir>/<projectName>/Config`(`Core/Source/Runtime/Config/Private/ConfigLoader.cpp:156–161`)뿐 | `AppInfo::projectName`. 게임 대상은 빌드가 주입하고(11절), 도구는 `--project`로 받는다 |
| `SetProjectName` (`APP` 200) | 이름 덮어쓰기(64자에서 자름) | 게임 스레드 PreInit: `LaunchSetGameName`(`LEL` 666–770, 671·680·691·707·757행), `LaunchFixGameNameCase`(`LEL` 783에서 시작, 799·812행), 최근 프로젝트 자동 열기(`LEL` 1246), 설정 오류(`Runtime/Core/Private/Misc/ConfigCacheIni.cpp:6794`), 도구 프로그램(`Programs/BuildPatchTool/Private/BuildPatchTool.cpp:276`) | 게임 스레드 | 없음 | 두지 않는다. 부트 한 번에 정한다 |
| `HasProjectName` (`APP` 162), `IsProjectNameEmpty` (`APP` 190) | 이름이 있고 `"None"`이 아닌지 / 비었는지 | — | 아무 스레드 | 없음 | `projectName.IsEmpty()`. `"None"` 특례는 두지 않는다 |
| `GetName` (`APP` 138) | 실행 파일 이름에서 `-`·`.` 앞부분 | — | 아무 스레드 | 없음 | `AppInfo::executableName`(확장자를 뗀 파일 이름). 잘라 내는 규칙은 두지 않는다 |

### 3.3 빌드 정보

UE는 별도 모듈 `BuildSettings`가 UBT 정의 매크로(`Runtime/BuildSettings/BuildSettings.Build.cs:26–40`)를 받아 문자열을 돌려준다(`Runtime/BuildSettings/Private/BuildSettings.cpp`). 빌드 날짜는 그 TU의 `__DATE__`(같은 파일 51행)다.

| 멤버 | 의미 | 값의 출처 | 읽는 스레드 | Rocks 현황 | Rocks 처리 |
|---|---|---|---|---|---|
| `GetBranchName` (`APP` 44) | 버전 관리 브랜치 | `BRANCH_NAME`(`Build.version`의 BranchName) | 아무 스레드 | 없음 | `BuildDescription::branch`(생성 TU) |
| `GetBuildConfiguration` (`APP` 51, `APPC` 119–136) | Debug·DebugGame·Development·Shipping·Test | 컴파일 매크로 + `bIsDebugGame` | 아무 스레드 | 매크로 `ROCKS_BUILD_DEBUG`·`_DEVELOPMENT`·`_RELEASE`(`Programs/Rocks.Build/NativeBuildModels.cs:54–60`), 상수 `IsReleaseBuild`(`Core/Source/Runtime/Base/Public/Base/Compiler.h:215`) | `BuildDescription::configuration`(enum `BuildConfiguration`) |
| `SetDebugGame` (`APP` 66), `bIsDebugGame` (`APP` 844) | Development 엔진 + DebugGame 게임 모듈 표시 | Launch 모듈이 PreInit에서(`LEL` 1773) | 게임 스레드 | 해당 구성 없음 | 두지 않는다 |
| `GetBuildTargetType` (`APP` 58, `APPC` 138–153) | Client·Game·Editor·Server·Program | 컴파일 매크로 | 아무 스레드 | 빌드 규칙의 `TargetType { Game, Program }`(`Programs/Rocks.Build.Abstractions/BuildRules.cs:43`). **C++에 주입하지 않는다** | `BuildDescription::targetType`(생성 TU) |
| `GetBuildVersion` (`APP` 74) | 빌드 고유 문자열 | `BUILD_VERSION` | 아무 스레드 | `build::CurrentEngineVersion()`·`ToolBuildId()`(`Core/Source/Runtime/Core/Public/Core/Build/BuildInfo.h:6–27`) | 기존 두 함수 + `BuildDescription::commit` |
| `GetBuildURL` (`APP` 81, `APPC` 60–67) | 바이너리를 만든 CI job 주소 | `FCoreDelegates::OnGetBuildURL` 또는 `BUILD_SOURCE_URL` | 아무 스레드 | 없음 | `BuildDescription::buildUrl`(빌드 때 환경 변수 `ROCKS_BUILD_URL`, 없으면 빈 값) |
| `GetExecutingJobURL` (`APP` 87, `APPC` 79–112) | 지금 실행 중인 Horde job 주소 | 실행 시 환경 변수 `UE_HORDE_*` | 아무 스레드 | 없음 | 두지 않는다(Horde 전용) |
| `GetIsWithDebugInfo` (`APP` 90) | 디버그 정보 포함 빌드 | `UE_WITH_DEBUG_INFO` | 아무 스레드 | 없음 | `BuildDescription::withDebugInfo` |
| `GetBuildDate` (`APP` 97) | 빌드 날짜 | `__DATE__` | 아무 스레드 | 없음 | `BuildDescription::commitDate`(커밋 시각 — D11) |
| `GetEngineIsPromotedBuild` (`APP` 115) | 배포용 승격 빌드 | `ENGINE_IS_PROMOTED_BUILD` | 아무 스레드 | 없음 | 두지 않는다(배포 체계가 생기면 다시 본다) |
| `GetEpicProductIdentifier` (`APP` 120) | 제품 식별자 상수 | `EPIC_PRODUCT_IDENTIFIER` | 아무 스레드 | 없음 | 두지 않는다 |
| `PrintStartupLogMessages` (`APP` 838, `APPC` 369–435) | 시작 로그 묶음(실행 파일·빌드·플랫폼·OS·CPU·GPU·컴파일러·구성·브랜치·명령줄·할당기) | `AppInit`(`LEL` 6871), Unix 크래시 경로(`Runtime/Core/Private/Unix/UnixPlatformProcess.cpp:2014`) | 게임 스레드 | 없음 | LogHost 설치 직후 `App` 카테고리 Info 기록 한 묶음(D14) |

### 3.4 인스턴스·세션

| 멤버 | 의미 | 쓰는 곳·시점 | 읽는 스레드 | Rocks 현황 | Rocks 처리 |
|---|---|---|---|---|---|
| `GetInstanceId` (`APP` 267), `InstanceId` (`APP` 848) | 실행 인스턴스 GUID | 정적 초기화에서 `FGuid::NewGuid()`(`APPC` 31). 비출시 빌드는 `InitializeSession`이 `-InstanceId=`로 덮어쓴다(`APPC` 182–188) | 아무 스레드(Runtime 18개 파일) | 없음 | `AppInfo::instanceId`(Guid). 매 실행 `platform::TryFillSecureRandom`(`Core/Source/Runtime/PlatformBase/Public/Platform/SecureRandom.h:22`)으로 만들고, 비출시 빌드만 `--instance-id`로 덮어쓴다 |
| `GetInstanceName` (`APP` 276) | `컴퓨터이름-PID` | 매번 계산 | 아무 스레드 | 없음 | 두지 않는다(시작 로그에 PID를 남긴다) |
| `IsThisInstance` (`APP` 366) | ID 비교 | — | — | 없음 | 두지 않는다(Guid 비교로 충분) |
| `GetSessionId` (`APP` 292), `SessionId` (`APP` 851) | 같이 실행한 묶음의 ID | 정적 초기화(`APPC` 32), `-SessionId=`(`APPC` 191–201). 명령줄은 비출시만 읽는다(`InitializeSession` 호출이 `!UE_BUILD_SHIPPING` — `LEL` 6880–6886) | 아무 스레드 | 없음 | `AppInfo::sessionId`(`Optional<Guid>`, `--session-id`로만, 비출시만) |
| `GetSessionObjectId` (`APP` 303, `APPC` 490–494) | 세션 ID의 Compact Binary 형태 | 첫 호출 | 아무 스레드 | 없음 | 두지 않는다 |
| `GetSessionName`/`SetSessionName` (`APP` 310·377), `SessionName` (`APP` 854) | 세션 이름 | `-SessionName=`(`APPC` 203), Insights 시험(`Developer/TraceInsightsFrontend/Private/InsightsFrontend/Common/InsightsAutomationController.cpp:64`) | 아무 스레드 | 없음 | `AppInfo::sessionName`(`--session-name`, 비출시만). setter 없음 |
| `GetSessionOwner`/`SetSessionOwner` (`APP` 325·388), `SessionOwner` (`APP` 857) | 세션을 띄운 사용자 | `-SessionOwner=` 또는 OS 사용자 이름(`APPC` 205–208), 세션 서비스(`Runtime/SessionServices/Private/SessionServicesModule.cpp:72`) | 아무 스레드 | 없음 | 두지 않는다(원격 세션 허가 없음) |
| `InitializeSession` (`APP` 333, `APPC` 177–209) | 명령줄의 세션 인자 읽기 | `AppInit`, 비출시만(`LEL` 6880–6886) | 게임 스레드 | 없음 | 호스트 부트가 공용 옵션 struct에서 채운다(8.3절) |
| `IsStandalone` (`APP` 355), `Standalone` (`APP` 866) | 세션 ID가 명령줄에 없음 | `InitializeSession` | 아무 스레드 | 없음 | `!sessionId.HasValue()` |
| `AuthorizeUser`·`DenyUser`·`DenyAllUsers`·`IsAuthorizedUser` (`APP` 214–342), `SessionUsers` (`APP` 863) | 원격 조작 허가 목록 | 엔진 서비스 메시지(`Runtime/Engine/Private/EngineService.cpp:127,142`) | 메시지 처리 스레드 | 없음 | 범위 밖 |
| `GetZenStoreProjectId`·`GetZenStoreProjectIdForProject` (`APP` 246·255, `APPC` 437–481) | Zen 저장소 프로젝트 ID | 계산 | 아무 스레드 | 없음 | 범위 밖 |

### 3.5 실행 성격

| 멤버 | 의미 | 쓰는 곳·시점 | 읽는 스레드 | Rocks 현황 | Rocks 처리 |
|---|---|---|---|---|---|
| `IsGame` (`APP` 176) | 게임처럼 동작 중(에디터의 Tryout 포함) | `GIsEditor`·`GIsPlayInEditorWorld`. Tryout 전환 때 게임 스레드가 바꾼다. 주석이 "worker에서 믿을 수 없다"고 적는다(`APP` 169) | 게임 스레드 | 없음 | AppInfo에 두지 않는다. 에디터·Tryout 구분은 World 종류가 맡는다 |
| `CanEverRender` (`APP` 400) | 렌더링할 수 있는 프로세스 | `UE_SERVER`, commandlet, `-nullrhi`를 처음 한 번 읽는다 | 아무 스레드(Runtime 91개 파일) | 없음. 헤드리스 실행은 Sandbox `--engine`의 고정 delta(`Programs/Sandbox/Private/EngineHost.cpp:27,42`)뿐이고 Null graphics backend가 없다(`Engine/Source/Runtime/Graphics/Public/Graphics/DeviceTypes.h:19–25`) | `RunTraits::headless`(10절) |
| `CanEverRenderOrProduceRenderData` (`APP` 419) | 렌더하거나 렌더 데이터를 만든다(쿠커) | 계산 | 아무 스레드 | 없음 | 두지 않는다. 쿠커는 Program 대상이고 렌더 데이터 생성 여부는 쿠커가 안다 |
| `CanEverRenderAudio` (`APP` 431), `IsAudioMuted` (`APP` 453) | 소리 장치를 쓸지 / 마지막 출력만 끌지 | `-nosound`·`-enablesound`·`-muteaudio`, benchmark·서버·commandlet | 오디오 스레드·게임 스레드 | 오디오 모듈 없음 | `RunTraits::audioOutput`·`audioMuted`(8절 struct, 10.2절 — 개정 4) |
| `IsInstalled` (`APP` 473, `APPC` 212–242), `IsEngineInstalled` (`APP` 485, `APPC` 245–272) | 설치형 실행(설치 폴더에 쓰지 않음) | 출시 데스크톱 기본 + 표지 파일 + `-Installed`·`-NotInstalled` | 아무 스레드 | Config가 "설치 경로 규칙은 패키징 단계에서 정한다"고 적었다([설정 계층](../ConfigSystem.ko.md) 5.4절) | 패키징 단계로 미룬다 |
| `IsUnattended` (`APP` 495, `APPC` 274–280) | 사람이 지켜보지 않음(대화상자 금지) | `-UNATTENDED`를 처음 한 번 읽고, `GIsAutomationTesting`과 OR | 아무 스레드(48개 파일) | 없음. PlatformBase에 대화상자 정책 API도 없다 | `RunTraits::unattended` |
| `AllowUnattendedInput` (`APP` 505) | 무인 실행이어도 입력 모듈을 켠다 | `-UnattendedInput` | 입력 초기화 | 없음 | `RunTraits::unattendedInput` |
| `ShouldUseThreadingForPerformance` (`APP` 515, `APPC` 290–302) | 렌더·태스크 성능 스레드를 쓸지 | `-onethread`·`-noperfthreads`·`-useperfthreads`, 코어 수, 서버 | 아무 스레드(47개 파일) | Task 모듈에 `InlineExecutor`가 있다(`Core/Source/Runtime/Task/Private/InlineExecutor.cpp`). 호스트 수준 선택은 없다 | 이름만 예약한다(`--single-thread`). 엔진 호스트가 모두 inline으로 돌 수 있는지 확인한 뒤 연다(20절) |
| `IsMultithreadServer` (`APP` 520) | 다중 스레드 전용 서버 | `UE_DEFAULT_MULTITHREAD_SERVER` | 아무 스레드 | 서버 대상 없음 | 두지 않는다 |
| `IsBenchmarking`/`SetBenchmarking` (`APP` 527·537), `bIsBenchmarking` (`APP` 869) | 벤치마크 모드(고정 step·고정 시드·포커스 무시·소리 끔) | PreInit `-BENCHMARK`(비출시), 출시는 false(`LEL` 2449–2454) | 아무 스레드: 게임 스레드 시간 갱신(`UEN` 3005), 렌더 통계(`Runtime/Engine/Private/UnrealClient.cpp:364`), 포커스(`APPC` 347) | 없음 | `RunTraits::benchmark`(비출시만) |
| `bUseFixedSeed` (`APP` 835) | 무작위 시드를 상수로 | PreInit `-Deterministic`·`-FixedSeed`·benchmark(`LEL` 2456–2463), 시드 초기화(`LEL` 2465–2480). `WITH_FIXED_TIME_STEP_SUPPORT` 안이고 이 매크로는 기본 켜짐이라(`Programs/UnrealBuildTool/Configuration/Rules/TargetRules.cs:1605`, `UEBuildTarget.cs:6450–6457`) Shipping에서도 동작한다 | 아무 스레드(파티클·이동·네트워크·스레드 생성 `Runtime/Core/Private/Windows/WindowsRunnableThread.cpp:66`) | 시험 도구의 `--seed`만 있다(FoundationTests, ToolTests) | `RunTraits::fixedSeed` |
| `GetGraphicsRHI`/`SetGraphicsRHI` (`APP` 104·109), `GraphicsRHI` (`APP` 860) | 실제로 쓰는 그래픽 API 이름 | RHI 초기화(`Runtime/RHI/Private/Windows/WindowsDynamicRHI.cpp:1366` 등 플랫폼별 4곳) | 자동화 보고·HLOD(`Runtime/AutomationWorker/Private/AutomationWorkerModule.cpp:332`) | Graphics `Backend`와 `BackendName`(`DeviceTypes.h:19–28`), 설정 `render.backend`(CVar 예시) | AppInfo에 두지 않는다. Graphics 장치 정보가 공개하고 시작 로그에 남긴다(D13) |

### 3.6 시간

UE 5.8은 시간 값을 `FAppTime`(`AT` 22)이라는 값 타입 하나에 모으고, 게임 스레드가 프레임마다 통째로 게시한다.

| 칸(`AT` 66–95) | 형식 | 의미 | 쓰는 곳 |
|---|---|---|---|
| `bUseFixedTimeStep` | bool(`WITH_FIXED_TIME_STEP_SUPPORT`일 때만 칸, 기본 켜짐 — Shipping 포함) | 고정 step 사용. 실제 사용 여부는 `IsBenchmarking() \|\| UseFixedTimeStep()`(`UEN` 3005) | PreInit `-UseFixedTimeStep`·`-Deterministic`(`LEL` 2460), DumpGPU(`Runtime/RenderCore/Private/DumpGPU.cpp:2284`), 영상 캡처(`Runtime/MovieSceneCapture/Private/MovieSceneCapture.cpp:835`) |
| `FixedDeltaTime` | double 초, 기본 1/30 | 고정 step 길이. `-FPS=`는 이 길이만 바꾸고 고정 step을 켜지 않는다 | `InitTime`의 `-FPS=`(`LEL` 4724–4731, 비출시만 — `!UE_BUILD_SHIPPING \|\| ENABLE_PGO_PROFILE`). `1 / FixedFPS`가 float 나눗셈이다 |
| `CurrentTime` | relaxed 원자 double 초 | 이번 프레임의 논리 현재 시각. 실제 시간 모드면 `FPlatformTime::Seconds()`이고, 고정 step 모드면 이전 값 + 고정 delta다(실제 시각과 어긋난다) | `InitTime`(`LEL` 4701), `UpdateTimeAndHandleMaxTickRate`(`UEN` 2980–3218, 3022·3028·3133–3145·3209행), 사용자 time step(`Runtime/Engine/Private/Engine/EngineCustomTimeStep.cpp:14`, `Runtime/TimeManagement/Private/*CustomTimeStep.cpp`) |
| `LastTime` | double 초 | 이전 프레임의 `CurrentTime` | `UpdateLastTime`(`UEN` 3012) |
| `DeltaTime` | double 초, 기본 1/30 | 이번 프레임 delta | `UEN` 3020·3155·3163(음수면 0.01)·3194(`MaxDeltaTime` 상한, 단독 게임만)·3207(`OverrideFPS`, 비출시) |
| `IdleTime` | double 초 | 최대 tick rate를 맞추려고 잔 시간 | `UEN` 3156 |
| `IdleTimeOvershoot` | double 초 | 요청보다 더 잔 시간(음수는 0) | `UEN` 3157 |
| `GameTime` | double 초 | `DeltaTime` 누적(pause·배율 없음 — World 시간과 다르다) | `UEN` 2993·3217 |
| `CurrentFrameTime` | `TOptional<FQualifiedFrameTime>` | timecode provider가 동기화됐을 때의 프레임 시각 | `UpdateTimecode`(`UEN` 3287–3298) |

**스레드 규칙**
- **쓰기.** 게임 스레드만 쓴다. `FAppTime::Set`이 `checkf(IsInGameThread())`로 막고, 게임 스레드 사본(`GameThreadInstance`)과 TLS 공유 사본을 함께 바꾼다(`ATC` 62–67).
- **읽기(게임 스레드와 그 자손 작업).** `FAppTime::Get`(`ATC` 25–47)은 TLS 사본이 있으면 그것을 돌려준다.
  - 태스크(`Runtime/Core/Public/Tasks/TaskPrivate.h:192,521`)와 렌더 명령(`Runtime/RenderCore/Public/RenderingThread.h:250–257,334`)은 만들 때 `CaptureInheritedContext`로 그 시점 값을 잡는다.
  - 실행 때는 `FInheritedContextScope`로 그 값을 되살린다(`Runtime/Core/Private/Async/InheritedContext.cpp:174,185,259`).
  - 그래서 작업은 자기를 만든 프레임의 시간을 본다.
- **읽기(그 밖의 스레드).** Slate·로딩 스레드는 경쟁을 알고도 게임 스레드 사본을 읽는다(`ATC` 31–35, `@todo`). 다른 스레드는 ensure다(`ATC` 38–43).
- **대략값.** 아무 스레드나 `GetApproximateCurrentTime`(`AT` 201)으로 relaxed 원자 값을 읽는다. 엔진 안에서 쓰는 곳은 hazard pointer 하나다(`Runtime/Core/Private/Experimental/Containers/HazardPointer.cpp:68`).
- **단위.** 모든 칸이 double 초다. 다만 `UEN` 3019는 고정 delta를 `const float`로 받아 다시 double에 넣는다. 그래서 고정 step delta는 float로 반올림된 값이다(D5에서 다룬다).

FApp 조회 함수(`APP` 547–654)는 이 값을 그대로 넘긴다. setter(`APP` 661–756)는 폐기 예정이라 대응하지 않는다.

### 3.7 포커스·볼륨

| 멤버 | 의미 | 쓰는 곳·시점 | 읽는 스레드 | Rocks 처리 |
|---|---|---|---|---|
| `GetVolumeMultiplier`/`SetVolumeMultiplier` (`APP` 763·773), `VolumeMultiplier` (`APP` 872) | 앱 전체 볼륨 배수 | 플랫폼 메시지 펌프가 포커스에 따라(`Runtime/ApplicationCore/Private/Windows/WindowsPlatformApplicationMisc.cpp:259`, Mac·Linux 같음), 에디터(`Editor/UnrealEd/Private/EditorEngine.cpp:1914–1924`) | 오디오 장치(`Runtime/Engine/Private/AudioDevice.cpp:4776`) | 오디오 모듈 설계로 넘긴다 |
| `GetUnfocusedVolumeMultiplier`/`SetUnfocusedVolumeMultiplier` (`APP` 783·788, `APPC` 311–326) | 포커스를 잃었을 때 배수. 처음 읽을 때 `[Audio] UnfocusedVolumeMultiplier`(GEngineIni), 쓰면 config에도 쓴다 | 영상 캡처·Pixel Streaming·Movie Render Pipeline 플러그인 | 게임 스레드 | 오디오 설정 CVar로 넘긴다 |
| `SetUseVRFocus`·`UseVRFocus`·`SetHasVRFocus`·`HasVRFocus` (`APP` 795–819) | 헤드셋 안 포커스 | OpenXR(`Plugins/Runtime/OpenXR/Source/OpenXRHMD/Private/OpenXRHMD.cpp:1098–1131,2505`) | 게임 스레드 | XR 설계 때 |
| `SetHasFocusFunction` (`APP` 826), `HasFocus` (`APP` 832, `APPC` 345–367), `HasFocusFunction` (`APP` 884) | 앱이 포그라운드인지. benchmark면 늘 true, VR이면 VR 값, 데스크톱은 플랫폼 함수 | ApplicationCore 초기화(`WindowsPlatformApplicationMisc.cpp:41`, Mac 287, Linux 278) | 게임 스레드(`LEL` 5334 idle 판정), RHI(`Runtime/D3D12RHI/Private/D3D12Adapter.cpp:1926` 상주 자원 해제) | 8.6절 |

### 3.8 관찰

- UE 전역은 "부트 때 정해지는 값"과 "실행 중 바뀌는 값"이 한 클래스에 섞여 있다. 앞의 것은 프로젝트 이름·빌드·실행 성격이고, 뒤의 것은 볼륨·포커스·세션 이름·그래픽 API 이름이다. 뒤의 것은 동기화 없이 여러 스레드가 읽는다.
- 명령줄 조회가 함수 안 static 지역 변수로 흩어져 있다. 그래서 같은 인자를 읽는 곳이 여러 곳이고, 처음 부른 시점에 값이 굳는다.
- 시간만은 5.8에서 "게임 스레드가 값 하나를 게시하고, 자손 작업이 만든 시점 값을 상속"하는 구조로 바뀌었다. Rocks는 이 방향을 앱 정보 전체에 넓힌다. 부트 값은 한 번 게시하고 불변이며, 프레임 값은 lane별로 상속한다.
