# 앱 정보와 명령줄 설계

기준일: 2026-11-22, 개정 4(오디오 실행 성격 `RunTraits::audioOutput`·`audioMuted`와 `--no-sound`·`--mute-audio`·`--allow-commandlet-audio` — [오디오 시스템](AudioSystem.ko.md) 3.8.4절). 개정 3: 크래시 수집·실행 진입점 반영. 사용자 지시("설계문서만 작성해놔")로 쓴 설계이고 구현하지 않는다. 단, AP1의 일부(`ProcessCommandLine`·대화상자 억제 API·진입점 연결)는 [크래시 처리·실행 진입점](CrashHandlingAndLaunch.ko.md)(LC3·LC4)이 먼저 구현했다(12절·18절). 결정은 권장안으로 확정했다(19절). 범위가 큰 항목은 19절에 따로 표시했다. 바뀐 점은 21절에 있다.

**착수 시점.** 고정 구현 순서(Curve → A0-G → A1–A6 → 화면 마일스톤)를 바꾸지 않는다. 이 문서의 단계(AP·CL — 18절)는 그 순서 안에 끼워 넣지 않으며 착수 시점은 사용자가 정한다. AP3(프레임 시간 전달)은 렌더 기반 RB1의 프레임 packet과 CVar 설계의 worker 묶음 장치에 기대므로 그 단계들과 함께 간다.

이 문서는 두 가지를 정한다.
- **앱 정보.** 프로세스 전역 정보(프로젝트 이름, 인스턴스·세션 ID, 빌드 정보, 실행 성격, 프레임 시간)를 어디에 두고, 누가 언제 쓰고, 어느 스레드가 읽는지.
- **명령줄.** 프로세스 명령줄 보관과, C++·C# 도구가 함께 쓰는 선언형 명령줄 틀(선언 한 곳에서 파싱·검증·도움말을 모두 만든다). 편의성 기준점은 C#의 System.CommandLine이고, C++이 그 수준을 따라간다.

UE 근거는 UE 5.8.4(`release`, commit `f45729c0b6f097038e72b52d366a62baeca3275b`, 로컬 클론 `C:\github\UnrealEngine`)다. 경로는 `Engine/Source/` 기준이다. 아래 약자를 쓴다.

| 약자 | 파일 |
|---|---|
| `APP` | `Runtime/Core/Public/Misc/App.h` |
| `APPC` | `Runtime/Core/Private/Misc/App.cpp` |
| `AT` | `Runtime/Core/Public/Misc/AppTime.h` |
| `ATC` | `Runtime/Core/Private/Misc/AppTime.cpp` |
| `LEL` | `Runtime/Launch/Private/LaunchEngineLoop.cpp` |
| `UEN` | `Runtime/Engine/Private/UnrealEngine.cpp` |
| `CLH` | `Runtime/Core/Public/Misc/CommandLine.h` |
| `CLC` | `Runtime/Core/Private/Misc/CommandLine.cpp` |
| `PAR` | `Runtime/Core/Public/Misc/Parse.h`, 구현 `Runtime/Core/Private/Misc/Parse.cpp`(`PARC`) |
| `CMD` | `Runtime/Engine/Classes/Commandlets/Commandlet.h` |

레거시·deprecated 경로는 대응하지 않는다. 구체적으로 다음을 뺐다.
- `FAppTime::Get_UnsafeNoCheck`(`AT` 107–112, `UE_DEPRECATED(5.8)`)
- `FApp`의 시간 setter 열 개(`APP` 661–756). 각 줄에 `// UE_DEPRECATED(5.8, "@todo delta time refactor - deprecate this function")`가 붙어 있어 폐기 예정이다. Rocks는 setter 대신 "게임 lane이 프레임 값 하나를 통째로 게시"하는 `FAppTime::Set`(`AT` 115) 방식만 따른다.

UE 이름은 6절 대응표와 근거 문장에만 쓴다.

## 1. 요약

- **앱 정보는 부트 때 한 번 채우고 불변이다.** Rocks는 `AppInfo`라는 불변 레코드를 둔다. 호스트 부트가 명령줄·빌드 정보·설정 부트 결과로 한 번 채우고, `AppInfoScope`로 프로세스에 설치한 뒤에는 바꾸지 않는다.
  - UE `FApp`처럼 아무 데서나 `Set`하는 전역은 두지 않는다. UE에서는 `SetProjectName`·`SetBenchmarking`·`SetGraphicsRHI`·`SetVolumeMultiplier`를 여러 스레드·모듈이 동기화 없이 쓰고 읽는다(3절).
- **프레임 시간은 따로 둔다.** Core 값 타입 `FrameTiming`(double 초, UE `FAppTime`과 같은 단위)이다.
  - 엔진 `RunFrame`이 프레임마다 게임 lane에서 새 값을 만든다.
  - 렌더 lane은 프레임 packet 안 사본을, worker는 작업을 만든 lane의 값을 물려받아 읽는다.
  - 그 밖의 스레드는 프레임 번호(relaxed 원자 값)와 단조 시계만 읽는다. UE 5.8의 `FAppTime` 상속 구조(`AT` 17–21, `ATC` 25–67)와 같은 방향이다.
- **실행 성격은 명령줄과 빌드 기본값에서만 정한다.** 대상은 `--unattended`·`--benchmark`·`--deterministic` 같은 것이다. 설정 파일·CVar로 정하지 않는다. 봉인 전(Fatal 처리)부터 필요하고, 출시 빌드의 세션 층 제한(Config D5)과 부딪히지 않기 때문이다(10절).
  - 뜻·기본값·출시 빌드에서 받는지는 UE 5.8과 같다. 고정 step 기본 길이는 1/30이고, `--fps`는 길이만 바꾸며, 고정 step·고정 시드는 출시 빌드에서도 동작한다(10.2절).
- **빌드 정보는 Rocks.Build가 생성 TU로 주입한다.** 이미 있는 `<exe>.BuildInfo.cpp` 옆에 커밋·브랜치·구성·대상 종류를 담은 별도 생성 TU를 만든다. 날짜는 빌드 시각이 아니라 커밋 시각을 쓴다(재현성 — 11절).
- **프로세스 명령줄 보관.** `ProcessCommandLine`이 진입점에서 한 번 UTF-8로 캡처하고 불변으로 둔다. Windows는 넓은 문자 원문에서 바꾼다(ANSI argv는 비 ASCII 인자를 깨뜨린다). 로그에는 `Secret` 옵션 값을 가린 형태만 남긴다(12절).
- **C++ 명령줄 파서는 자체 구현이다(확정).** 외부 라이브러리는 검토하고 제외했다. 이유는 크기다(사용자 결정 2026-11-18).
  - 선언은 리플렉션 지정자 문법을 쓴다. 옵션 struct 하나를 `ROCKS_STRUCT(Command(...))`·`ROCKS_PROPERTY(Help=..., Alias=...)`로 선언한다.
  - MetaCompiler는 Command struct마다 명령줄 스키마와 **필드별 바인더 코드**를 생성한다. 런타임은 그 스키마와 바인더로 파싱·검증·도움말을 모두 만든다(D19).
  - 값 변환은 Core `NumberText`와 생성 enum 표가 맡는다. ReflectionText·TypeRegistry·ObjectRegistry에 기대지 않는다. 그래서 리플렉션을 쓰지 않던 낮은 계층 도구(벤치·FoundationTests)도 옮길 수 있다.
  - Command struct는 직렬화하지 않으므로 identity 면제다. `rocksids` 항목과 `ids add`가 필요 없다(D30).
  - 실행 함수는 결과 struct를 `const&`로 받는다. 옵션마다 `GetValue`를 부르는 C#보다 꺼내 쓰기가 짧다(15절).
- **C# 도구는 System.CommandLine을 파싱에만 쓴다.** 외부 패키지(MIT)를 처음 들여온다. 별도 어셈블리 `Rocks.Tooling.Cli`의 `RocksCli`가 도움말·오류 출력을 모두 그린다.
- **양쪽 출력은 "Rocks 명령줄 출력 규약"(14.4·14.5절)을 따른다.** 규약은 이 문서가 직접 정의한다. 줄 끝 `\n`, UTF-8, 영어, 너비·열 규칙, 오류 문구, 종료 코드(0·1·2)가 그 내용이다. C++과 C#은 각자 이 규약대로 렌더한다. 같은 시험 명령을 양쪽으로 정의해 도움말·오류 출력이 byte 단위로 같은지 golden 시험으로 확인한다(17절).

## 2. 범위

**범위 안**
- UE `FApp`·`FAppTime`에 대응하는 프로세스 정보와 프레임 시간
- UE `FCommandLine`·`FParse`·`UCommandlet::ParseCommandLine`에 대응하는 명령줄 보관과 파서
- 명령줄 도구(RocksCommandlet, Sandbox, ShaderToolSpike, 벤치, FoundationTests)와 C# 도구(Rocks.Build.Cli, Rocks.MetaCompiler, Rocks.ToolTests)의 인자 처리 통일
- 빌드 정보 주입(Rocks.Build)

**범위 밖**
- UE `FGenericApplication`(OS 창·메시지·포커스 원천). [UI 툴킷](UiToolkit.ko.md) 3절의 `ui::Platform` 계약과 PlatformWindow 모듈이 맡는다. 이 문서는 포커스 상태를 어디서 읽는지만 정한다(8.6절).
- UE `FEngineLoop` 구조 자체(PreInit·Init·Tick 순서). Rocks 엔진 루프는 [엔진 런타임](EngineRuntime.ko.md)과 `Engine::RunFrame`이 정한다. 이 문서는 부트 순서에 끼우는 지점만 적는다(8.3절).
- UE Zen 저장소 프로젝트 ID(`GetZenStoreProjectId`). Rocks 파생 데이터 캐시는 에셋 너겟이고 Zen 대응이 없다.
- UE 원격 세션 도구의 사용자 허가 목록(`AuthorizeUser`·`DenyUser`). 대응하는 원격 세션 도구가 없다.
- 오디오 볼륨 배수와 VR 포커스. 오디오는 [오디오 시스템](AudioSystem.ko.md)이 정했고(8.6절), XR은 그 설계 때 정한다.

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
| `IsInstalled` (`APP` 473, `APPC` 212–242), `IsEngineInstalled` (`APP` 485, `APPC` 245–272) | 설치형 실행(설치 폴더에 쓰지 않음) | 출시 데스크톱 기본 + 표지 파일 + `-Installed`·`-NotInstalled` | 아무 스레드 | Config가 "설치 경로 규칙은 패키징 단계에서 정한다"고 적었다([설정 계층](ConfigSystem.ko.md) 5.4절) | 패키징 단계로 미룬다 |
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

## 4. UE 5.8 명령줄 조사

| 대상 | 내용 | 근거 |
|---|---|---|
| `FCommandLine` 저장 | 고정 크기 `TCHAR` 배열 넷(현재·원본·로그용 둘), 각 `MaxCommandLineSize = 16384` | `CLH` 48, 185–191 |
| 바꾸기 | `Set`·`Append`로 실행 중 바꾼다. 응답 파일 펼치기(`-CmdLineFile=*.txt`, `LEL` 1093–1170)와 게임 이름 처리(`LEL` 2361·2401)가 다시 `Set`한다. 첫 `Set`은 `PreInitPreStartupScreen`(`LEL` 1808) | `CLH` 88–94 |
| 환경 변수 추가 | 비출시·에디터 데이터 빌드는 환경 변수 `UE-CmdLineArgs`를 ` -EnvAfterHere ` 뒤에 덧붙인다. 출시 게임은 악용을 막으려고 받지 않는다 | `LEL` 1814–1827 |
| 로그 가리기 | 컴파일 때 정한 목록(`FILTER_COMMANDLINE_LOGGING`)의 인자를 로그용 사본에서 지운다. 허용 목록 모드(`UE_COMMAND_LINE_USES_ALLOW_LIST`)도 있다 | `CLC` 221–278 |
| 하위 프로세스 상속 | `RegisterArgument(이름, 문맥 플래그 + Inherit, 설명)`, `BuildSubprocessCommandLine` | `CLH` 24–41, 102–120 |
| 토큰 나누기 | `FCommandLine::Parse` → Tokens·Switches | `CLH` 136 |
| `FParse::Value` | 명령줄 **문자열 전체**에서 `Key=`를 대소문자 무시로 찾는다(`FCString::Strifind`). 정의되지 않은 인자·오타는 아무 진단 없이 무시된다 | `PAR` 30–83, `PARC` 265 |
| `FParse::Param` | 공백 뒤 `-`·`/`로 시작하는 단어 전체를 대소문자 무시로 찾는다 | `PAR` 127, `PARC` 325–350 |
| `FParse::Bool`·`Token`·`Command`·`QuotedString`·`GrammarBasedCLIParse` | 값 읽기·토큰·콘솔 명령·따옴표·문법 기반 콜백 | `PAR` 28, 85, 117–132, 223 |
| `UCommandlet` 도움말 | `HelpDescription`·`HelpUsage`·`HelpWebLink`·`HelpParamNames`·`HelpParamDescriptions` 칸을 손으로 채운다. 파싱과 따로 놀아 둘이 어긋날 수 있다 | `CMD` 46–62 |
| `UCommandlet::ParseCommandLine` | `-`로 시작하면 Switch, 아니면 Token. `Key=Value` Switch는 Params 맵으로 옮긴다 | `CMD` 115–160 |
| `ApplyCommandLineSwitches` | 에디터 전용. Switch를 UObject 프로퍼티에 리플렉션으로 대입한다 | `CMD` 160–170, `Runtime/Engine/Private/Commandlets/Commandlet.cpp:41` |

**관찰과 Rocks 방향**
- UE는 인자를 선언하지 않고 조회하는 쪽이 문자열로 찾는다. 그래서 알 수 없는 인자·오타·형식 오류가 조용히 넘어간다. 도움말은 파싱과 별도 칸이다.
- Rocks는 선언 한 곳(옵션 struct)에서 파싱·검증·도움말을 모두 만든다. 알 수 없는 인자는 오류다.
- 선언된 필드에 인자를 대입하는 발상은 UE `ApplyCommandLineSwitches`와 같다. Rocks는 이것을 에디터 전용 보조가 아니라 기본 경로로 삼는다. 다만 실행 중 리플렉션 조회가 아니라 MetaCompiler가 생성한 필드별 바인더로 대입한다(D19).
- 환경 변수로 인자를 더하는 `UE-CmdLineArgs`는 대응하지 않는다. CI·개발자는 응답 파일(`@file`, 도구만)이나 실행 명령줄을 쓴다. 원문 보존(12절)과 비밀 가리기가 한 경로로 남는다.

## 5. Rocks 현황

### 5.1 앱 정보 쪽

| 항목 | 현황 | 근거 |
|---|---|---|
| 프로젝트 이름·디렉터리 | `HostSettingsOptions`(`mode`·`engineDir`·`projectDir`·`projectName`·`userDataDir`·`savedDir`·등록 콜백 둘)가 있다. `engineDir`만 호스트가 채운다(`FindEngineDirectory`). 나머지는 채우는 호스트가 없다 | `HostSettings.h:30–43`, Commandlet `Programs/Commandlet/Private/Common/CommandletHost.cpp:166–175`, Sandbox `EngineHost.cpp:182–186` |
| 부트 순서 | 등록 → 층 파일 → `--config` → `--set` → `CompleteStartup` → LogHost → trace·`--trace` → 엔진 루프 | [설정 계층](ConfigSystem.ko.md) 5.4절, `EngineHost.cpp:181–236` |
| 프레임 시간 | `EngineTime{frameIndex, deltaSeconds, realTimeSeconds, averageFrameSeconds}`, 모두 double 초. `RunFrame` 2단계에서 단조 시계 차로 계산한다. `EngineConfig::fixedFrameDeltaSeconds > 0`이면 그 값을 쓴다 | `Engine/Source/Runtime/Engine/Public/Engine/Engine.h:172–173,209–216`, `Engine/Source/Runtime/Engine/Private/Engine.cpp:452–463` |
| 프레임 번호 공유 | `Log::SetFrame`이 LogSystem의 `frame_`을 relaxed 원자 값으로 둔다 | `Core/Source/Runtime/Core/Private/Diagnostics/LogSystem.cpp:1006–1007` |
| World 시간 | World마다 `GameClock`(pause·배율·고정 step) | [엔진 런타임](EngineRuntime.ko.md) 5절 |
| 빌드 정보 | 구성·플랫폼 매크로, 로그 하한 매크로, 생성 TU의 `ToolchainFingerprint`·`ToolBuildId`·`CurrentEngineVersion`. 대상 종류·브랜치·커밋·날짜는 없다 | `Programs/Rocks.Build/BuildInfo.cs:298–370,596–616`, `ActionGraph.cs:376–486` |
| 실행 성격 | `Unattended`·`nullrhi`·`FixedSeed`가 코드에 없다. 실행 모드 구분은 `LogSystemConfig::ForTool()`(Sync)과 `ForEngineHost()`(Async)뿐이다 | `Core/Source/Runtime/Core/Public/Core/Diagnostics/LogSystem.h:104–112` |
| 대화상자 억제 | PlatformBase 공용 API가 없다. 같은 Windows 억제 코드(`_set_abort_behavior`·`_CrtSetReportMode`·`SetErrorMode`)가 두 곳에 복사돼 있다 | `Core/Source/Developer/TestSupport/Private/Win64/HostFailure.cpp:19–34`, `Programs/ShaderToolSpike/Private/Win64/SpikeHostWin64.cpp:13–18` |
| 프레임 값 상속 | Task 모듈에 작업 문맥 상속이 없다. CVar 설계가 "worker는 Task 생성 때 만든 lane의 묶음을 넘긴다"를 계획으로 적었다 | `Core/Source/Runtime/Task/Public/Task/WorkerScheduler.h:31`, [CVar](CVarSystem.ko.md) 10.1·16절 |

### 5.2 C++ 명령줄 도구

공용 명령줄 파서가 없다. 공용 구조는 두 가지다. `CommandletRegistry`는 하위 명령 선택과 `--help`·`--list`만 맡는다. 나머지는 인자 배열 전체에서 자기 접두사만 골라 쓰는 필터 셋(`--set=` 둘, `--trace=` 하나)이다.

| 도구 | 하위 명령 | 값 형식 | 반복 | 필수 검사 | `--help` | 알 수 없는 인자 | `@file` | 파서 위치 |
|---|---|---|---|---|---|---|---|---|
| RocksCommandlet 진입 | 있음(registry) | — | — | 인자 128개·4096 B·UTF-8 | 명령 목록만("이름<TAB>설명"), 명령별 도움말 없음 | "Unknown command. Use --list." 종료 2 | 없음 | `Programs/Commandlet/Private/Win64/Main.cpp:58–82`, `Core/Source/Runtime/Core/Private/Commandlet/Commandlet.cpp:97–165` |
| `core-check` | 없음 | — | — | 인자 0개 | 없음 | 종료 2 | 없음 | `CommandletHost.cpp:17–21` |
| `trace-report` | 없음 | 위치 `<in> [<out>]` | — | 개수 1~2 | 사용법 문자열 | 종료 2 | 없음 | `CommandletHost.cpp:58–63` |
| `log` | `keygen`·`decode`·`seal` | `--key <f>`·`--public <f>`를 **고정 위치**로 비교, `--json` | 없음 | 개수 비교 | 있음(손으로 쓴 문자열) | Usage 전체를 stderr, 종료 2 | 없음 | `Programs/Commandlet/Private/Common/LogCommandlet.cpp:238,313,366–389` |
| `config-dump` | 없음 | — | — | 인자 0개 | 없음 | 종료 2 | 없음 | `Core/Source/Runtime/Config/Private/ConfigDump.cpp:97–108` |
| Sandbox | 사실상 모드 둘 | `--frames N`·`--config <f>`(공백), `--set=`·`--trace=`(`=` 결합) | `--set`·`--trace` 64개까지 | `--frames` 1 이상 | 없음 | 한 줄 Usage, 종료 2 | 없음 | `Programs/Sandbox/Private/Main.cpp:24–105` |
| ShaderToolSpike | 없음 | `--out <d>`(필수), `--sm 6_x` | 없음 | 있음 | 없음 | Usage, **종료 64** | 없음 | `Programs/ShaderToolSpike/Private/Common/Main.cpp:33–52` |
| CompressionBench | 없음 | 값 다섯 개 | 없음 | 범위. **숫자 뒤 문자를 받아들인다**(`--size 12abc` → 12) | 없음 | usage를 **stdout**, 종료 1 | 없음 | `Programs/CompressionBench/Private/Main.cpp:562–602` |
| CryptoBench·MathBench | 없음 | 값 둘 | 없음 | `ParseNumber`(두 파일이 글자까지 같은 복사본) | 없음 | stdout, 종료 1 | 없음 | `Programs/CryptoBench/Private/Main.cpp:58–71`, `Programs/MathBench/Private/Main.cpp:91–104` |
| LogBench | 없음 | 위치 `[samples]` | — | 범위 | 없음 | stdout, 종료 2 | 없음 | `Programs/LogBench/Private/Main.cpp:129–145` |
| FoundationTests | `--process-probe <mode> ...`(mode 뒤 인자는 mode마다 자유 형식) | 값 다섯 개(`strcmp`) | 없음 | `ParseUnsigned` | 없음 | stderr, 종료 2 | 없음 | `Programs/FoundationTests/Private/Main.cpp:115–385` |

엔진 런타임에서 명령줄을 읽는 곳은 셋이다. Config `AddCommandLineAssignments`(`Core/Source/Runtime/Config/Private/ConfigLoader.cpp:222–252`), HostSettings `ApplyCommandLine`(`Core/Source/Runtime/HostConfig/Private/HostSettings.cpp:216–241` — `--set=`를 **한 번 더 훑는다**), TraceController `ApplyCommandLine`(`Core/Source/Runtime/TraceControl/Private/TraceController.cpp:248–272`)이다. Commandlet 호스트는 `ApplyCommandLine`을 부르지 않아 `--set`·`--trace`를 쓸 수 없다(`CommandletHost.cpp:166–175`).

**프로세스 명령줄 보관이 없다.** PlatformBase에 `GetCommandLine` 류가 없다. 진입점마다 처리가 다르다.
- RocksCommandlet은 `wmain`에서 UTF-16을 UTF-8로 바꾼다.
- Sandbox·ShaderToolSpike·벤치·FoundationTests는 `main(int, char**)`이다(RocksCommandlet의 Mac 진입점도 같다). Windows에서 이 argv는 시스템 ANSI 코드 페이지 문자열이다. Rocks.Build는 컴파일 인자 `/utf-8`과 기본 manifest(`/MANIFEST:EMBED`, `ActionGraph.cs:310,429`)만 주고 `activeCodePage` 설정은 없다.
  - **비 ASCII 인자는 깨진다.** Rocks는 argv를 UTF-8로 취급한다. 한국어 로캘에서도 CP949 바이트는 잘못된 UTF-8이므로, ACP가 UTF-8이 아닌 한 모든 비 ASCII 인자가 깨진다. 코드 페이지에 없는 글자(이모지 등)는 CRT 변환에서 이미 사라진다.
  - **보안 문제.** CRT의 넓은 문자 → ANSI 변환은 best-fit 대응을 쓴다. 전각 `－`(U+FF0D)·`＂`(U+FF02) 같은 글자가 ASCII `-`·`"`로 바뀌어, 사용자가 값으로 준 글자가 옵션이나 따옴표로 해석될 수 있다.

### 5.3 C# 도구

외부 파서 패키지가 없다. 모든 csproj·props에 `PackageReference`가 0건이고 `Directory.Packages.props`도 없다. **`--help`를 지원하는 C# 도구는 0개다.**

| 도구 | 하위 명령 | 값 형식 | 반복 | 필수 검사 | `--help` | 알 수 없는 인자 | `@file` | 파서 위치 |
|---|---|---|---|---|---|---|---|---|
| Rocks.Build.Cli | `build`·`test`·`sdk`·`game`·`bootstrap` | `--key value`만. `--offline`도 `true`/`false` 값이 있어야 한다 | 중복 거부 | 명령별 허용 키·필수 키 | 없음 | `ROCKS-BUILD-ERROR: Unknown, duplicate, or incomplete option: X` 종료 2 | 없음 | `Programs/Rocks.Build.Cli/Program.cs:35–66,382–395` |
| Rocks.MetaCompiler | `ids` 분기 | `--key value`·`--key=value` 둘 다 | 6종 반복 | 필수 셋 + 경로 검증 | 없음 | `error RMC1000: unknown option X` + `hint:` 종료 1 | 없음 | `Programs/Rocks.MetaCompiler/Program.cs:7–37`, `CompilerOptions.cs:45–260` |
| MetaCompiler `ids` | 7개 | `--key value` | 있음 | `One()` | 없음 | "Unknown ids option --X" | 없음 | `Programs/Rocks.MetaCompiler/IdentityCommands.cs:7–227` |
| Rocks.ToolTests | 12개 | 둘 다 | `--clang-arg`만 | `Required()`·`Optional()` | 없음 | "Unknown argument: X" 등, 종료 2 | 없음 | `Programs/Rocks.ToolTests/Program.cs:9–145`, `TestSupport.cs:278–325` |
| ToolTests `scaffold` | `plan`·`apply`·`validate` | `--key value` | 없음 | `Get()` | 없음 | "Invalid scaffold option" | 없음 | `Programs/Rocks.ToolTests/Program.cs:147–197` |

### 5.4 중복 패턴

| 패턴 | 횟수 | 곳 |
|---|---|---|
| C++ `for (index = 1; index < argc)` + 문자열 비교 + `index + 1 < argc` 값 확인 | 6 | Sandbox, ShaderToolSpike, CompressionBench, CryptoBench, MathBench, FoundationTests |
| C++ 10진수 손 파싱 | 함수 4 + 인라인 4 | Sandbox `ParseFrames`, CryptoBench·MathBench `ParseNumber`, FoundationTests `ParseUnsigned`, CompressionBench 인라인(꼬리 문자 허용 결함), LogBench |
| C++ 접두사 필터 | 4 | Sandbox(`--set=`·`--trace=`), HostSettings·ConfigLoader(`--set=` 두 번), TraceController(`--trace=`) |
| 손으로 쓴 usage 문자열 + 종료 코드 불일치(1·2·64) + 출력 대상 불일치(stdout·stderr) | 8 도구 | 5.2절 표 |
| C# 키-값 쌍 루프(`index += 2`) | 4 | Build.Cli, MetaCompiler ids, ToolTests scaffold, IntegrationTests |
| C# `--key`·`--key=value` 겸용 루프 | 2 | MetaCompiler, ToolTests |
| C# 하위 명령 `switch (args[0])` + "Unknown command" | 4 | Build.Cli, MetaCompiler ids, ToolTests, scaffold |

### 5.5 리플렉션 기반

옵션 struct 선언에 쓸 기반을 확인했다. 지정자 파서는 `Programs/Rocks.MetaCompiler/Specifiers.cs`이고 표에 없는 키는 빌드 오류다. 개정 2에서 바인딩을 생성 바인더로 바꾼 이유(아래 표의 "제약" 행들)도 여기에 모았다.

| 필요한 것 | 지금 | 근거 |
|---|---|---|
| struct 선언 | `ROCKS_STRUCT(...)`·`ROCKS_PROPERTY(...)`. base 없는 비템플릿 struct이고 `S() noexcept` 또는 `explicit S(Allocator&) noexcept`가 필요하다 | `Core/Source/Runtime/MetaSchema/Public/Meta/Annotations.h:17,42–61,76` |
| struct 수준 묶음 지정자 | `Settings(Branch=…, Section=…)`가 선례다. 생성 코드는 `GeneratedType<S>::Settings()` → `SettingsSchema`다 | `Specifiers.cs:168–193`, `Core/Source/Runtime/MetaSchema/Public/Meta/SettingsSchema.h:56–69`, `Programs/Rocks.MetaCompiler/CppEmitter.cs:796–815` |
| 필드 이름 | `Name=`(도구 이름, ASCII 식별자)·`EffectiveToolName` | `Specifiers.cs:204–259`, `Core/Source/Runtime/MetaSchema/Public/Meta/TypeSchema.h:864–890` |
| 설명·별칭·위치 인자·예시·비밀·범위 | **없다.** `Tooltip`·`Description`·`ClampMin` 같은 키가 없고 문서 주석도 가져가지 않는다. `Required`는 "참조 non-null" 뜻이라 명령줄 필수와 다르다 | `Specifiers.cs`, `TypeSchema.h:60–68`, `SchemaValidator.cs:650` |
| 기본값 | 멤버 초기화식을 읽지 않는다. 기본값은 생성자를 실행한 결과다 | [런타임 클래스 메타데이터](RuntimeClassMetadata.ko.md) 47–76행 |
| 값 변환 | ReflectionText `ParseTextScalar`·`ParseTextValue`·`FormatTextValue`·`PrepareAssignments`. 명령행 대입을 전제로 설계됐다(`TextSourceKind::Assignment`) | `Core/Source/Runtime/ReflectionText/Public/ReflectionText/TextValue.h:268–302`, `TextAssignments.h:66,84–86,288` |
| struct 사이 검증 | `static Result<void> Validate(const S&) noexcept` hook을 MetaCompiler가 인식한다(CV3) | `Programs/Rocks.MetaCompiler/ClangAstParser.cs:269–300`, `CppEmitter.cs:788–790` |
| 인자 struct 선례 | 콘솔 명령 인자 struct `TraceChannelArguments`와 `ConfigContext::RegisterConsoleCommand<Args>` | `Core/Source/Runtime/TraceControl/Public/TraceControl/TraceSettings.h:52–69`, `Core/Source/Runtime/Config/Public/Config/ConfigContext.h:280–294` |
| 출시 빌드 메타 문자열 제외 | 조건이 없다. category·displayName·toolName이 늘 실린다 | `CppEmitter.cs:877–891` |
| 오류 처리 | `Result<T>`·`ErrorCode`. 예외·RTTI는 빌드 플래그로 금지한다(`/EHs-c-`·`/GR-`, `-fno-exceptions`·`-fno-rtti`) | `Core/Source/Runtime/Core/Public/Core/Result.h:12`, `Error.h:10–35`, `ActionGraph.cs:439–440,472` |
| 제약: 반복 대입 | ReflectionText 대입 표기는 `<경로>=<값>`·Unset·Erase뿐이고 원소 추가 표기가 없다(`$append`는 Config JSON 층 전용). 같은 경로 두 번은 `DuplicateAssignment`다. 목록 텍스트는 `,;`만 escape하고 그 밖의 `\`는 글자 그대로라, `\`로 끝나는 원소(`C:\dir\`)를 이어 붙이면 다음 쉼표와 합쳐진다 | `ReflectionText/TextAssignments.h:40–46,265`, `TextValue.h:20–24`, [설정 계층](ConfigSystem.ko.md) 104행 |
| 제약: Validate 이유 | struct `Validate` hook은 `Result<void>`만 돌려준다. 대입 묶음은 `Publish` 안에서 hook을 부르고 고정 문구 "the value failed its struct validation hook"과 InvalidArgument로 바꾼다. 사람에게 보일 이유 문자열이 없다 | `ClangAstParser.cs:268–271`, `ReflectionText/Private/TextAssignments.cpp:884–896`, `Object/ValueCopy.h:333` |
| 제약: enum·bool 표기 | ReflectionText enum 입력은 대소문자 무시·접두 일치(`j` → `json`), bool은 on·off도 받는다. 일반 enum 도구 이름은 `[A-Za-z0-9_]+`라 `-`를 쓸 수 없다 | `TextValue.h:12–14`, `Specifiers.cs:323` |
| 제약: 설정 지정자 | `DevelopmentOnly`는 설정 지정자다(`HasSettings`). Settings struct 밖에서 쓰면 RMC2210이다. CVar VD5·Config 개정 4가 `Cheat`로 바꾸기로 했다 | `Specifiers.cs:71`, `SchemaValidator.cs:1059–1066`, [CVar](CVarSystem.ko.md) VD5 |
| 제약: identity | Metadata가 있는 모듈은 `<Name>.rocksids.json`이 필요하고(RMC2100), 새 선언은 `ids add`로 등록해야 한다. 일반 빌드는 manifest를 바꾸지 않는다. 지금 Programs 아래 실행 모듈에는 Metadata가 하나도 없고 HostConfig에도 없다 | `ClangAstParser.cs:566–582`, [리플렉션 기반](ReflectionFoundations.ko.md) 33행 |
| 제약: 계층 | MathBench·LogBench는 Core만, CryptoBench는 Crypto, CompressionBench는 Compression, ShaderToolSpike는 ShaderToolLoader만 링크한다. FoundationTests Core 계층은 ReflectionText를 일부러 빼고 링크한다 | 각 `Programs/*/*.Build.cs`, `Programs/FoundationTests/FoundationTests.Build.cs:63` |
| 수 변환 | Core `NumberText`의 `TryParseInt64`·`TryParseUInt64`·`TryParseDouble` 등. 전체 소비, 꼬리 문자 거부, 범위 넘침 `Overflow` | `Core/Source/Runtime/Core/Public/Core/Text/NumberText.h:28–41` |
| Guid 텍스트 | Base `Guid`에 텍스트 변환이 없다. 36자 소문자 표기는 ReflectionText 안에만 있다 | `Core/Source/Runtime/Base/Public/Base/Guid.h` |

## 6. 타입과 이름

Rocks 이름은 서술형이다. UE 이름은 이 표에만 둔다.

| UE 5.8 | Rocks | 한 줄 설명 |
|---|---|---|
| `FApp`(부트 값 부분) | `AppInfo` | 부트 때 한 번 채우는 프로세스 정보 불변 레코드 |
| (없음) | `AppInfoScope` | `AppInfo`를 프로세스에 설치·해제하는 범위 객체(`LogInstallScope`와 같은 방식) |
| `IsUnattended`·`IsBenchmarking`·`bUseFixedSeed`·`CanEverRender` 등 | `RunTraits` | 실행 성격 값 묶음 |
| `IsRunningCommandlet` 등 실행 종류 전역 | `HostKind` | `Game`·`Commandlet`·`Tool`·`TestRunner`(에디터·서버는 생길 때 더한다) |
| `GetBuildConfiguration`·`GetBuildTargetType`·`GetBranchName`·`GetBuildDate` 등 | `build::Description()` → `BuildDescription` | 빌드가 주입한 상수 레코드 |
| `EBuildConfiguration` | `build::BuildConfiguration` | `Debug`·`Development`·`Release` |
| `EBuildTargetType` | `build::TargetType` | `Game`·`Editor`·`Client`·`Server`·`Program`(빌드 규칙의 `TargetType`과 같은 이름·같은 값 — [빌드 타깃](BuildTargets.ko.md) BD1) |
| `FAppTime` | `FrameTiming` | 한 프레임의 시간 값(double 초). 지금의 `EngineTime`을 대체한다 |
| `FAppTime::GetApproximateCurrentTime` | `LatestFrameIndex()` + `platform::MonotonicNanoseconds()` | 아무 스레드가 읽는 대략값 |
| `FCommandLine` | `ProcessCommandLine` | 진입점에서 캡처한 불변 UTF-8 인자 배열 |
| `FParse`·`UCommandlet::ParseCommandLine` | `ParseCommandLine<T>()` | 스키마 기반 파서(옵션 struct로 결과를 낸다) |
| `UCommandlet`의 `Help*` 칸 | `CommandLineSchema` | MetaCompiler가 만드는 명령줄 스키마(이름·별칭·도움말·예시)와 필드별 바인더 표 |
| `UCommandlet::ApplyCommandLineSwitches` | 생성 바인더(`GeneratedCommand<S>`) | 필드 하나에 값 텍스트 하나를 변환해 넣는 생성 함수 |
| `UCommandlet`·commandlet registry | `CommandSet` | 명령 묶음(하위 명령·공용 옵션·`help`·`--version`) |
| `-CmdLineFile=` | 응답 파일 `@file` | 도구 전용 |
| `FILTER_COMMANDLINE_LOGGING` | `Secret` 지정자 | 로그에서 값을 가릴 옵션 |
| `#if !UE_BUILD_SHIPPING`으로 감싼 명령줄 조회 | `NotInRelease` 지정자 | Release 구성에서 받지 않는 옵션 |
| (없음) | `ValidateCommand` hook | 옵션 사이 검증. 영어 이유 문자열을 진단으로 남긴다 |
| (없음) | `Remaining` 지정자 | 해석하지 않고 넘길 남은 인자 |

## 7. 모듈 배치와 코드 크기

### 7.1 배치

Engine/Source/Runtime에는 핵심만 둔다. 게임 실행 파일이 쓰는 최소 부분과 도구·commandlet 전용 부분을 나눈다.

| 위치 | 담는 것 | 누가 링크하나 |
|---|---|---|
| Core 런타임 `Core`: `Core/App/AppInfo.h`, `Core/App/ProcessCommandLine.h`, `Core/Time/FrameTiming.h` | `AppInfo`·`RunTraits`·`HostKind`·`AppInfoScope`, `ProcessCommandLine`, `FrameTiming`·`LatestFrameIndex` | 모든 실행 파일 |
| Core 런타임 `Core`: `Core/Build/BuildInfo.h`(기존 헤더 확장) | `build::Description()` 선언. 정의는 Rocks.Build 생성 TU | Rocks.Build로 링크한 모든 실행 파일 |
| Core `PlatformBase` | 실행 파일 경로, 터미널 너비 조회, 대화상자 억제 정책(`platform::SuppressSystemErrorDialogs`, 크래시 수집 LC1b에서 TestSupport·ShaderToolSpike 복사본을 합쳤다). Windows 명령줄은 CRT가 나눈 넓은 문자 argv를 쓰므로 직접 분해는 두지 않는다(개정 3, 12절) | 모든 실행 파일 |
| Core 런타임 `Core`: `Core/Text/EditDistance.h`, Base `Guid` 텍스트 변환 | 이름 제안용 편집 거리(CVar 12절과 같은 함수), Guid 36자 소문자 텍스트 변환(ReflectionText도 이 함수를 쓰게 한다) | 공용 |
| 새 Core 런타임 모듈 `CommandLine`(의존: `Core`만) | `CommandLine/CommandLineSchema.h`(생성 스키마·바인더 표 형식), 토큰화·옵션 매칭·생성 바인더 호출·필수·범위·`ValidateCommand`·진단·이름 제안·비밀 가리기·짧은 사용법 줄 | 게임 호스트, Sandbox, 도구. Command struct를 선언한 모듈 |
| `HostConfig`(기존) | 호스트 공용 옵션 struct `HostCommandLineOptions`(`--set`·`--config`·`--trace`·`--project`·실행 성격)와 부트 도우미. 이 모듈에 Metadata 선언이 새로 생긴다(Command struct만 있으므로 identity 면제 — D30) | 게임 호스트, Sandbox, RocksCommandlet |
| 새 Core Developer 모듈 `CommandLineTools`(`Core/Source/Developer/` 아래 — Game 대상의 Release는 도달하면 policy 위반이고, Program 대상은 Release에서도 쓴다. `ReleaseExclusion.cs:28–33`) | 도움말 writer(너비·열·예시)·응답 파일·`CommandSet`·`--version` 출력 | 도구·commandlet, Game 대상의 Debug·Development(구성별 의존 — `Sandbox.Build.cs`의 `shaderCompiler` 분기와 같은 방식) |
| Engine 런타임 `Engine` | `FrameTiming` 갱신과 게시, `RunTraits` 소비(고정 step) | 엔진 호스트 |
| `Programs/Rocks.Build` | 생성 TU `<exe>.BuildStamp.cpp`, 대상 종류 정의 `ROCKS_TARGET_KIND_GAME`·`ROCKS_TARGET_KIND_PROGRAM` | — |
| 새 C# 어셈블리 `Programs/Rocks.Tooling.Cli` | `RocksCli`(System.CommandLine 파싱 위에 Rocks 출력 규약 렌더러). `Rocks.Tooling.Common`과 분리해, 설치 SDK의 게임 규칙 프로젝트(`InstalledSdk.cs:293–294`가 `Rocks.Tooling.Common.dll`을 참조)로 패키지 의존이 번지지 않게 한다 | C# 도구 |

지금 Core 런타임에 있는 `CommandletRegistry`(`Core/Source/Runtime/Core/Public/Core/Commandlet/Commandlet.h`)는 `CommandLineTools`의 `CommandSet`으로 대체하고 지운다(하위호환 없음, D24). 게임 실행 파일은 commandlet을 쓰지 않는다.
- Core 런타임 `Config`가 공개하는 `RegisterConfigDumpCommandlet(CommandletRegistry&)`(`Core/Source/Runtime/Config/Public/Config/ConfigDump.h:47–48`)는 Runtime 모듈이 Developer `CommandSet`을 볼 수 없으므로 옮긴다. 덤프 본체(`ConfigDump`)는 Config에 두고, 명령 등록과 `ConfigDumpOptions`는 `Programs/Commandlet`으로 옮긴다(CL4).
- [셰이더 시스템](ShaderSystem.ko.md) 96·147행(`shader-compile`)과 [에셋 시스템](AssetSystem.ko.md) 37행(`asset-*`)은 `CommandletRegistry`를 전제로 썼다. CL7에서 `CommandSet`으로 고친다.

### 7.2 코드 크기 목표

외부 라이브러리를 쓰지 않은 이유가 크기이므로, 자체 구현에도 상한을 둔다. 단계 완료 때 측정해 보고한다.

| 부분 | 소스 상한(시험 제외) | Release x64 실행 파일 증가 상한 | 측정 |
|---|---|---|---|
| Core `App`·`Time`·`ProcessCommandLine`(PlatformBase 분해 포함) | 800줄 | 8 KiB | Release Sandbox 크기 비교 |
| `CommandLine`(런타임 파서) | 1,500줄 | 24 KiB | Release Sandbox 크기 비교. 의존은 Core뿐이다(수 변환은 이미 링크된 `NumberText`) |
| 생성 바인더 | 명령 하나에 필드 수에 비례 | Command struct 하나에 2 KiB 안팎(필드 10개 기준, CL2에서 측정해 상한을 고친다) | Release Sandbox(`HostCommandLineOptions` 하나)와 Release MathBench 크기 비교 |
| `CommandLineTools`(Developer) | 2,000줄 | Game 대상 Release 0(링크하지 않는다) | Game 대상 Release 링크 맵에 기호가 없음을 확인 |
| MetaCompiler 추가 | 1,200줄(C# — Command 생성 방식, 바인더·enum 표 생성, identity 면제, 검사) | — | — |
| 생성 스키마 문자열 | — | Game 대상 Release는 도움말·예시·값 이름 문자열 0 B | 생성 코드가 `ROCKS_BUILD_RELEASE && ROCKS_TARGET_KIND_GAME`이면 그 칸을 빈 값으로 낸다(CVar의 출시 빌드 치트 문자열 제거와 같은 방향 — [CVar](CVarSystem.ko.md) 13.1절). Program 대상의 Release(RocksCommandlet·벤치·SDK 도구)는 도움말을 그대로 싣는다 |
| `Rocks.Tooling.Cli`(C#) | 900줄(도움말·오류 렌더러, 수·bool·enum 사용자 파서, 이름 제안, `help <명령>`, `--version`, 파서 설정) | — | — |

- **대상 종류 정의.** Rocks.Build가 Game 대상 모듈에 `ROCKS_TARGET_KIND_GAME=1`, Program 대상 모듈에 `ROCKS_TARGET_KIND_PROGRAM=1`을 넣는다. 같은 구성에서 한 모듈 object를 Game·Program 대상이 공유하면 정의가 달라 공유할 수 없다. 지금 Rocks.Build가 대상 사이 object를 공유하는지는 확인하지 못했다(20절). 공유한다면 생성 스키마 TU만 대상별로 컴파일하는 방식으로 좁힌다.

## 8. 앱 정보 `AppInfo`

### 8.1 레코드

```cpp
namespace rocks {

// 실행 종류다. 빌드 대상 종류(build::TargetType)와 다르다. Program 대상 하나가 commandlet으로도 도구로도 돈다.
enum class HostKind : uint8 { Game, Commandlet, Tool, TestRunner };

// 실행 성격이다. 명령줄과 빌드·호스트 기본값에서만 정하고(10절) 실행 중 바뀌지 않는다.
// 뜻과 기본값은 UE 5.8 FApp·FAppTime과 같다(10.2절 표).
struct RunTraits {
    bool unattended = false;        // 사람이 보지 않는다: 대화상자·입력 대기 금지
    bool unattendedInput = false;   // 무인이어도 입력 장치를 연다
    bool benchmark = false;         // 비출시 빌드만. 고정 step·고정 시드를 함께 뜻한다
    bool fixedSeed = false;         // 무작위 시드를 상수로
    bool useFixedTimeStep = false;  // 엔진 프레임 delta를 fixedDeltaSeconds로 고정한다
    double fixedDeltaSeconds = 1.0 / 30.0; // 고정 step 길이(초). UE FixedDeltaTime 기본값과 같다
    bool headless = false;          // 창·그래픽 장치를 만들지 않는다
    bool audioOutput = true;        // 소리 장치를 만든다. 부트가 Server 대상·commandlet(--allow-commandlet-audio가 없으면)·benchmark·--no-sound면 false로 정한다(UE FApp::CanEverRenderAudio)
    bool audioMuted = false;        // 장치·믹서는 돌리고 마지막 출력만 끈다(--mute-audio, UE FApp::IsAudioMuted)

    // 엔진이 실제로 고정 step을 쓰는지다(UE UpdateTimeAndHandleMaxTickRate와 같은 판정).
    bool UsesFixedTimeStep() const noexcept { return benchmark || useFixedTimeStep; }
};

// 프로세스 정보다. 호스트 부트가 한 번 만들고 AppInfoScope로 설치한 뒤 바꾸지 않는다.
// 문자열 view는 AppInfo가 소유한 블록을 가리키며 AppInfo 수명 동안 유효하다.
class AppInfo {
public:
    // 설치된 AppInfo다. 전제: AppInfoScope가 살아 있다(Debug·Development는 assert).
    static const AppInfo& Get() noexcept;
    // 설치 여부다. 부트 전 경로(Fatal 처리 등)가 확인할 때만 쓴다.
    static bool IsInstalled() noexcept;

    StringView ProjectName() const noexcept;       // 없으면 빈 값(도구)
    StringView ProjectDirectory() const noexcept;
    StringView ExecutableName() const noexcept;    // 확장자를 뗀 실행 파일 이름
    HostKind Host() const noexcept;
    const Guid& InstanceId() const noexcept;
    const Optional<Guid>& SessionId() const noexcept; // 없으면 단독 실행
    StringView SessionName() const noexcept;
    const RunTraits& Traits() const noexcept;
    const build::BuildDescription& Build() const noexcept; // build::Description()과 같다
};

} // namespace rocks
```

- 만드는 길은 `AppInfo::Create(const AppInfoDescription&, Allocator&) → Result<AppInfo>` 하나다. 입력 문자열을 한 블록으로 복사한다. 길이 상한은 이름 붙은 상수다(`MaxProjectNameBytes` 등). 넘으면 `CapacityExceeded`다. UE처럼 64자에서 조용히 자르지 않는다.
- 문자열은 UTF-8이고, 프로젝트 이름은 파일 이름에 쓸 수 있는 문자만 허용한다. 문법은 Config 사용자 층 경로가 쓰는 규칙을 따른다.

### 8.2 수명과 쓰기 규칙

- **한 번 채우고 불변이다.** setter가 없다. 바꿔야 하는 값이라면 AppInfo에 두지 않는다(8.6절).
- **설치는 범위 객체로 한다.** `AppInfoScope installed(appInfo);`가 프로세스 설치 포인터를 release 저장으로 바꾸고, 소멸이 되돌린다. `Get()`은 acquire 읽기다.
  - 이미 설치돼 있으면 생성자가 Fatal이다(두 번 설치는 부트 순서 버그다). 이 점이 `LogInstallScope`와 다르다. `LogInstallScope`는 이전 값을 보관해 중첩을 허용한다(`LogSystem.h:368–377`).
  - 시험은 시험마다 scope를 만든다. 시험 실행기(`HostKind::TestRunner`)는 기본 AppInfo 하나를 설치하고, 다른 값이 필요한 시험은 TestSupport의 `ReplaceAppInfoForTest` scope로 잠시 바꾼다. 이 scope만 이전 값을 보관하고 되돌린다. 그동안 다른 스레드가 읽지 않는 시험만 쓴다(문서화한 전제).
- **왜 전역인가(D3).** 로그·trace·크래시 처리·Fatal 대화상자 정책은 Core 깊은 곳에서 프로젝트 이름·instance id·unattended를 읽어야 한다. 이것을 모두 인자로 전하면 Core API가 앱 정보를 줄줄이 받아야 한다. 값이 불변이고 설치·해제가 범위로 묶이므로 UE 전역의 스레드·수명 문제(동기화 없는 쓰기, 첫 호출 시점에 굳는 static)가 없다.
  - AppInfo를 설치하지 않는 실행(일부 도구·시험)이 있으므로, Core 깊은 곳의 읽는 쪽은 `IsInstalled()`를 먼저 본다.

### 8.3 부트 순서

Config 5.4절 부트 순서 앞뒤에 다음을 끼운다.

| 순서 | 일 | 실패 처리 |
|---|---|---|
| 0 | Launch 진입점(`RunApplication`)이 `ProcessCommandLine`을 캡처한다(Windows는 CRT의 넓은 문자 argv에서 — 12절) | 종료 코드 `ApplicationDescription::commandLineFailureExitCode`(기본 1, RocksCommandlet 2), stderr 한 줄 |
| 1 | 호스트가 공용 옵션 struct(`HostCommandLineOptions`)와 자기 옵션 struct를 파싱한다(14절) | 사용법 오류: 진단 + 짧은 사용법, 종료 2. 로그가 없으므로 stderr에 직접 쓴다. 게임 호스트 Release의 알 수 없는 인자는 오류가 아니라 9단계 경고로 미룬다(D29) |
| 2 | `RunTraits`를 정하고 Fatal 대화상자 정책을 PlatformBase에 바로 적용한다(unattended면 대화상자 없음) | — |
| 3 | `AppInfo::Create` → `AppInfoScope` 설치. 입력은 0–2단계 결과와 `build::Description()`뿐이다 | 종료 코드 1 |
| 4 | `HostSettings::Initialize` — `projectName`·`projectDir`를 AppInfo에서 채운다. 설정 등록 코드는 여기서 `AppInfo::Get().Traits()`를 읽을 수 있다(10.1절) | 기존과 같다 |
| 5–8 | 층 파일 → `--config` 파일 → `--set` 값 → `CompleteStartup`(기존 2–5단계). `--set` 값은 파싱한 배열을 그대로 받는다(14.3절) | 기존과 같다 |
| 9 | LogHost 설치 → 시작 로그 묶음(D14) → 1단계에서 미룬 경고 | 기존과 같다 |
| 10 | trace 세션 → `--trace` 목록 적용 → 엔진 루프 | 기존과 같다 |

- 2단계의 대화상자 정책은 AppInfo를 거치지 않고 PlatformBase 값 하나로 둔다. 0–2단계의 Fatal도 같은 값을 본다.
- Commandlet 호스트도 같은 순서다. 그래서 commandlet에서도 `--set`·`--trace`가 동작한다. 지금은 쓸 수 없다(5.2절).

### 8.4 스레드

| 대상 | 쓰는 쪽 | 읽는 쪽 | 동기화 |
|---|---|---|---|
| AppInfo 내용 | 만들 때 한 번(게임 lane, 부트) | 모든 스레드 | 설치 포인터의 release·acquire. 내용은 불변 |
| 설치 포인터 | `AppInfoScope` 생성·소멸(부트 3단계·종료) | `Get()` | 원자 포인터. 로그 출력 스레드·worker·렌더 lane은 설치 뒤에 시작하고 해제 전에 끝난다 |
| `RunTraits`가 PlatformBase에 넣은 대화상자 정책 | 부트 2단계 한 번 | Fatal 경로(아무 스레드) | relaxed 원자 값 하나(정책 하나라 순서 문제가 없다) |

### 8.5 빌드 정보 조회

`AppInfo::Build()`는 `build::Description()`(11절)과 같은 레코드를 가리킨다. 상수라 스레드 안전하다. AppInfo를 설치하기 전에도 `build::Description()`을 직접 부를 수 있다.

### 8.6 AppInfo에 두지 않는 것

| UE 값 | Rocks에서 맡는 곳 | 이유 |
|---|---|---|
| 포커스(`HasFocus`) | PlatformWindow 이벤트를 받은 엔진이 프레임 상태로 공개한다: `Engine::IsForeground()`(게임 lane). 다른 lane이 필요하면 프레임 packet에 넣는다. benchmark면 늘 true(UE `APPC` 347–350과 같다) | 실행 중 바뀐다. 원천은 `ui::Platform`·PlatformWindow다 |
| 볼륨 배수·포커스 잃었을 때 배수 | 오디오 모듈의 `audio.*` CVar(`audio.master.volume`·`audio.unfocused.volume` — [오디오 시스템](AudioSystem.ko.md) 3.8.5절) | 설정 값이다. UE도 config에서 읽는다(`APPC` 316) |
| 그래픽 API 이름 | Graphics 장치 정보 | 장치를 만든 뒤에야 안다 |
| 게임 중인지(`IsGame`) | World 종류 | World마다 다르다 |
| 세션 이름 바꾸기 | 없음 | 실행 중 바꿀 이유가 없다 |

## 9. 프레임 시간 `FrameTiming`

### 9.1 값

```cpp
namespace rocks {

// 한 프레임의 시간 값이다. 모든 시간은 double 초다(UE FAppTime과 같은 단위·형식).
// 엔진 RunFrame이 프레임마다 게임 lane에서 새로 만든다. 만든 뒤 바꾸지 않는다.
struct FrameTiming {
    uint64 frameIndex = 0;              // 1부터. 0은 첫 프레임 전이다
    double currentSeconds = 0.0;        // 이번 프레임의 논리 시각(단조 시계 초). 고정 step이면 이전 값 + 고정 delta
    double lastSeconds = 0.0;           // 이전 프레임의 currentSeconds
    double deltaSeconds = 0.0;          // 이번 프레임 delta
    double idleSeconds = 0.0;           // 프레임 상한을 맞추려고 잔 시간
    double idleOvershootSeconds = 0.0;  // 요청보다 더 잔 시간(0 이상)
    double accumulatedSeconds = 0.0;    // deltaSeconds 누적. pause·배율이 없다(World 시간은 GameClock)
    double averageFrameSeconds = 0.0;   // 최근 프레임 지수 이동 평균
    Optional<double> fixedDeltaSeconds; // 고정 step을 쓰는 프레임이면 그 길이(RunTraits::UsesFixedTimeStep)
};

// 아무 스레드나 읽는 마지막 프레임 번호다(relaxed). 시간 값이 필요한 다른 스레드는
// platform::MonotonicNanoseconds()를 쓴다.
uint64 LatestFrameIndex() noexcept;

} // namespace rocks
```

| Rocks 칸 | UE 칸(`AT` 66–95) | 차이 |
|---|---|---|
| `frameIndex` | (`GFrameCounter`, FAppTime 밖) | 한 값에 묶는다 |
| `currentSeconds`·`lastSeconds`·`deltaSeconds` | `CurrentTime`·`LastTime`·`DeltaTime` | 같다. 실제 시간 모드의 원천은 `platform::MonotonicNanoseconds()`다(UE `FPlatformTime::Seconds()`) |
| `idleSeconds`·`idleOvershootSeconds` | `IdleTime`·`IdleTimeOvershoot` | 같다. Rocks 엔진에는 아직 프레임 상한 대기가 없어 0이다 |
| `accumulatedSeconds` | `GameTime` | 이름만 다르다. 지금 `EngineTime::realTimeSeconds`가 이 값이다. 고정 step이면 실제 시간이 아니므로 이름을 바꾼다 |
| `fixedDeltaSeconds` | `bUseFixedTimeStep` + `FixedDeltaTime` | 두 칸을 Optional 하나로 묶는다 |
| `averageFrameSeconds` | (없음) | Rocks에 이미 있다 |
| (나중) | `CurrentFrameTime`(timecode) | timecode provider가 생길 때 `Optional<…>` 칸을 더한다. CV2의 `FrameRate`·`FrameTime`을 쓴다 |

- 지금의 `EngineTime`을 `FrameTiming`으로 대체한다. `Engine::Time()`은 `Engine::Timing()`이 된다(하위호환 없음).
- **고정 step delta는 double 그대로다(D5).** UE는 고정 delta를 float 지역 변수로 거쳐 넣는다(`UEN` 3019). `-FPS=`의 `1 / FixedFPS`도 float 나눗셈이다(`LEL` 4730). 그래서 UE의 1/30은 float로 반올림된 값이다. 단위·형식(double 초)과 기본값(1/30)은 UE와 같고, Rocks는 중간 float 반올림만 따르지 않는다. 이 차이는 D5에 결정으로 적었다. 결정적 실행의 비트 결과가 UE와 같아야 하는 기능(UE 데이터와 비트 대조)이 생기면 다시 본다.
- **음수 delta 처리 없음.** 단조 시계(`uint64` 차)라 음수가 없다. UE의 "음수면 0.01"(`UEN` 3160–3164)은 필요 없다.
- **엔진 delta에 상한이 없다.** UE 단독 게임의 `MaxDeltaTime` 상한(`UEN` 3184–3196)은 Rocks에서 World `GameClock`의 `maxFrameDeltaSeconds`가 맡는다([엔진 런타임](EngineRuntime.ko.md) 5절). 엔진 프레임 값은 측정값 그대로다.

### 9.2 스레드별 읽기

| 읽는 스레드 | 읽는 법 | 보는 값 |
|---|---|---|
| 게임 lane | `Engine::Timing()` | 이번 프레임 값 |
| 렌더 lane | 프레임 packet(RenderBase `FramePacket`) 안 `FrameTiming` 사본 | 그 packet을 만든 프레임의 값 |
| worker 작업 | 작업 문맥이 물려받은 값. CVar 설계가 계획한 "Task 생성 때 만든 lane의 묶음을 작업에 넘긴다" 장치를 그대로 쓴다. 게임 lane이 프레임마다 게시하는 refcount 묶음(CVar snapshot과 같은 묶음)에 그 프레임의 `FrameTiming`을 넣고, 작업에는 묶음 포인터 하나만 넘긴다. 작업마다 `FrameTiming`을 복사하지 않는다 | 작업을 만든 프레임의 값 |
| 그 밖(로그 출력 스레드, trace writer, 플랫폼 콜백) | `LatestFrameIndex()`(relaxed), 시각은 `platform::MonotonicNanoseconds()` | 대략값 |

- 엔진 밖 스레드가 "현재 프레임 시간"을 정확히 읽는 전역 API는 두지 않는다. UE도 그 경우를 데이터 경쟁으로 본다(`AT` 98–103 주석).
- `LatestFrameIndex()`는 엔진이 2단계에서 relaxed 저장한다. 지금 `Log::SetFrame`이 따로 쥔 프레임 번호는 이 값을 읽도록 나중에 정리할 수 있다. 필수는 아니다.
- worker 상속 장치가 생기기 전(AP3 전)에는 worker가 `FrameTiming`을 읽을 길이 없다. 필요한 작업은 제출할 때 값을 인자로 넘긴다.

## 10. 실행 성격과 명령줄·설정

### 10.1 출처

실행 성격은 **명령줄 + 빌드·호스트 기본값**에서만 정한다. 설정 파일과 CVar로는 정하지 않는다(D6). 이유는 셋이다.
- 봉인(`CompleteStartup`) 전부터 필요하다. Fatal 대화상자 정책은 설정을 읽기 전에도 정해져 있어야 한다.
- 출시 빌드의 세션 층(`--set`)은 서버 대상만 받는다(Config D5). 실행 성격을 CVar로 두면 출시 게임에서 `--unattended`를 쓸 수 없다.
- 실행 한 번의 성격이다. 프로젝트 파일에 굳혀 둘 값이 아니다. CI 기계별 설정은 실행 명령줄로 준다.

설정 쪽과는 이렇게 잇는다.
- **보기.** 콘솔 명령 `app.info`가 AppInfo 전체(실행 성격 포함)를 출력한다. 콘솔 이름 영역 표에 `app`을 더한다(Config 5.1).
- **값 기본값.** 엔진 설정이 실행 성격에 따라 기본값이 달라야 하면 그 설정의 등록 코드가 `AppInfo::Get().Traits()`를 보고 Code 층에 쓴다. 예: benchmark면 프레임 상한을 끈다. 출처 스택에 `app traits`로 남아 Explain에서 보인다. 등록은 부트 4단계이고 AppInfo는 3단계에 설치되므로 읽을 수 있다(8.3절).
- **엔진 고정 step.** 호스트가 `EngineConfig::fixedFrameDeltaSeconds`를 `Traits().UsesFixedTimeStep() ? Traits().fixedDeltaSeconds : 0`으로 채운다.
  - 지금 Sandbox는 `--engine` 실행마다 상수 `HeadlessFrameSeconds`(1/60)를 넣는다(`Programs/Sandbox/Private/EngineHost.cpp:27,42`). 이 상수를 지우고 `--deterministic`(기본 1/30)으로 옮긴다.
  - Sandbox 자체 시험이 띄우는 자식(`--engine --frames N`)은 `--deterministic`을 함께 넘긴다. 고정 step 길이가 1/60에서 1/30으로 바뀌므로, 자식 결과를 시간 값으로 비교하는 곳이 있으면 함께 고친다(CL4).
- **게임플레이 고정 step과의 경계.** 프로젝트가 늘 고정 step으로 돌아야 하는 경우(결정적 시뮬레이션 게임)는 실행 성격이 아니라 World `TimeStepPolicy`(설정)가 맡는다. `RunTraits`의 고정 step은 실행 한 번의 엔진 프레임 delta만 바꾼다.

### 10.2 명령줄 플래그

공용 옵션 struct `HostCommandLineOptions`(14.3절)의 필드다.

뜻·기본값·출시 빌드 수용 여부는 UE 5.8과 같다(절대 규칙 — 시간). 다른 점은 고정 delta의 중간 float 반올림을 따르지 않는 것 하나다(D5).

| 옵션 | 효과 | 출시 빌드 | UE 대응 |
|---|---|---|---|
| `--unattended` | `unattended = true` | 받는다 | `-UNATTENDED`(`APPC` 278) |
| `--unattended-input` | `unattendedInput = true` | 받는다 | `-UnattendedInput`(`APPC` 286) |
| `--benchmark` | `benchmark = true`. 고정 step(`UsesFixedTimeStep`)과 `fixedSeed = true`를 함께 뜻한다. 포커스는 늘 true, 소리는 끈다(`audioOutput = false`) | **받지 않는다** | `-BENCHMARK`(`LEL` 2449–2454, 출시는 false), 고정 step(`UEN` 3005), 시드(`LEL` 2462), 소리(`APP` 437) |
| `--use-fixed-time-step` | `useFixedTimeStep = true` | 받는다 | `-UseFixedTimeStep`(`LEL` 2460) |
| `--deterministic` | `useFixedTimeStep = true`, `fixedSeed = true`. 길이는 `fixedDeltaSeconds`(기본 1/30) | 받는다 | `-Deterministic`(`LEL` 2456–2462, "-UseFixedTimeStep -FixedSeed"의 줄임) |
| `--fixed-seed` | `fixedSeed = true` | 받는다 | `-FixedSeed`(`LEL` 2462) |
| `--fps <rate>` | `fixedDeltaSeconds = 1 / rate`. **고정 step을 켜지 않는다**(길이만 바꾼다). `Range`로 rate는 0보다 크고 유한하다 | 받지 않는다 | `-FPS=`(`LEL` 4724–4731, 비출시) |
| `--headless` | `headless = true` | 받는다 | `-nullrhi`(뜻이 넓다 — 창·장치를 아예 만들지 않는다) |
| `--no-sound` | `audioOutput = false` | 받는다 | `-nosound`(`APP` 436, `-enablesound`는 두지 않는다 — 뒤 값이 앞 값을 덮는다) |
| `--mute-audio` | `audioMuted = true` | 받는다 | `-muteaudio`(`APP` 443–453) |
| `--allow-commandlet-audio` | commandlet에서도 `audioOutput`을 끄지 않는다 | 받지 않는다 | `IsAllowCommandletAudio()`(`APP` 437) |
| `--instance-id <guid>` | AppInfo `instanceId` | 받지 않는다 | `-InstanceId=`(`APPC` 182–188) |
| `--session-id <guid>`·`--session-name <text>` | AppInfo `sessionId`·`sessionName` | 받지 않는다 | `-SessionId=`·`-SessionName=`(`APPC` 191–203). UE도 `InitializeSession`을 비출시에서만 부른다(`LEL` 6880–6886) |

- UE 고정 step 계열(`-UseFixedTimeStep`·`-Deterministic`·`-FixedSeed`)은 `WITH_FIXED_TIME_STEP_SUPPORT` 안에 있고, 이 매크로는 기본 켜짐이라 Shipping에서도 동작한다(3.5절). Rocks도 출시 빌드에서 받는다.
- 시험 실행기(`HostKind::TestRunner`)는 `unattended`가 기본 true다(UE `GIsAutomationTesting` OR와 같은 뜻).
- 이름만 예약하고 지금 받지 않는 것: `--single-thread`(20절). `--no-sound`·`--mute-audio`는 개정 4에서 오디오 설계와 함께 열었다(구현은 오디오 AU3 또는 AP1 중 먼저 오는 쪽).
- 출시 빌드에서 받지 않는 옵션에는 Command 전용 지정자 `NotInRelease`를 붙인다(13.3절). Release 구성의 파서는 이 옵션을 "알 수 없는 옵션"으로 다룬다. 기준은 대상 종류와 무관한 Release 구성이다(UE `!UE_BUILD_SHIPPING`과 같은 뜻).

## 11. 빌드 정보 주입

### 11.1 레코드

```cpp
namespace rocks::build {

enum class BuildConfiguration : uint8 { Debug, Development, Release };
enum class TargetType : uint8 { Game, Editor, Client, Server, Program };   // 빌드 타깃 BD1

// 빌드가 실행 파일마다 주입한 상수 레코드다. 실패하지 않고 프로세스 수명 동안 같은 값이다(스레드 안전).
struct BuildDescription {
    BuildConfiguration configuration;
    TargetType targetType;
    StringView targetName;      // 빌드 대상 이름(예: "Sandbox")
    StringView projectName;     // Game 대상의 프로젝트 이름, Program은 빈 값
    StringView branch;          // 모르면 빈 값
    StringView commit;          // 40자 16진, 모르면 빈 값
    bool commitDirty;           // 커밋되지 않은 변경 위에서 빌드했다
    StringView commitDate;      // 커밋 시각 ISO 8601 UTC(예: "2026-11-18T09:12:00Z"), 모르면 빈 값
    StringView buildUrl;        // CI가 준 job 주소(빌드 때 환경 변수 ROCKS_BUILD_URL), 없으면 빈 값
    bool withDebugInfo;
};

const BuildDescription& Description() noexcept;

} // namespace rocks::build
```

### 11.2 주입 방식

- Rocks.Build가 실행 파일마다 생성 TU 두 개를 만든다. 하나는 기존 `<exe>.BuildInfo.cpp`이고, 다른 하나가 새 `<exe>.BuildStamp.cpp`(`Description()` 정의)다.
  - 매크로가 아니라 생성 TU에 두는 이유가 있다. 매크로로 주입하면 값이 바뀔 때 그 매크로를 보는 모든 TU를 다시 컴파일한다. 생성 TU면 그 TU 하나만 다시 컴파일한다.
- **값 얻기.** 브랜치·커밋·커밋 시각은 빌드 시작 때 `git rev-parse HEAD`·`git symbolic-ref --short HEAD`·`git show -s --format=%cI HEAD`·`git status --porcelain`(dirty)로 한 번 얻는다. git이 없거나 저장소가 아니면(설치 SDK) 빈 값이다. 빌드 실패가 아니다.
  - CI는 보통 detached HEAD로 받으므로 `symbolic-ref`가 실패해 branch가 빈 값이 된다. 환경 변수 `ROCKS_BUILD_BRANCH`가 있으면 그 값을 쓴다(UE는 CI가 `Build.version`의 BranchName을 채운다).
  - **비용.** 커밋이 바뀌거나 dirty 여부가 바뀌면 BuildStamp TU 하나만 다시 컴파일되지만 모든 실행 파일이 다시 링크된다(Release는 ThinLTO 링크). `git status --porcelain`은 빌드마다 한 번 돈다. 지금 BuildInfo TU도 엔진 버전이 바뀌면 같은 방식으로 다시 링크된다(`Programs/Rocks.Build.Tests/BuildInfoTests.cs:649–651`). 다시 링크하는 비용은 AP2 완료 때 측정해 보고한다.
- **날짜는 커밋 시각이다(D11).** UE는 `__DATE__`(컴파일 시각)를 쓴다. 이것은 같은 소스에서 빌드할 때마다 byte가 달라진다. Rocks는 재현 빌드를 위해 커밋 시각을 쓴다. dirty 빌드는 `commitDirty`로 구분한다.
- **캐시 무효화와 분리(D12).** `ToolBuildId()`는 입력 내용으로 정해지는 진단·캐시 무효화 키다(`BuildInfo.h:20–24`). BuildStamp TU는 그 입력에서 뺀다. 커밋만 바뀌고 코드가 같으면 `ToolBuildId`가 같아야 한다. 쿠킹 byte·에셋 너겟 키에도 넣지 않는다(엔진 버전과 같은 규칙).
- **구성·대상 종류.** 이미 있는 매크로(`ROCKS_BUILD_*`)와 빌드 규칙의 `TargetType`에서 생성 TU에 상수로 쓴다. C++ 코드의 `if constexpr` 분기는 계속 `IsReleaseBuild` 상수를 쓴다. `Description()`은 진단·출력용이다.
  - 대상 종류는 컴파일 정의 `ROCKS_TARGET_KIND_GAME`·`ROCKS_TARGET_KIND_PROGRAM`으로도 넣는다. 생성 명령줄 스키마가 "Game 대상의 Release"에서 도움말 문자열을 빼는 데 쓴다(7.2절). Base `Compiler.h`가 정확히 하나만 1인지 확인한다(구성 매크로와 같은 방식).
- **사용처.** `--version` 출력(14.6절), 시작 로그(D14), 크래시 보고·trace 파일 머리(나중).

`--version` 출력 예(C++·C# 같은 모양):

```
RocksCommandlet 0.1.0 (Development, Program, commit 4a28102d, 2026-11-18)
```

## 12. 프로세스 명령줄 `ProcessCommandLine`

구현됨(개정 3): `Core/Source/Runtime/Core/Public/Core/App/ProcessCommandLine.h`. 크래시 처리·실행 진입점 설계의 LC3이 만들고 LC4에서 상한을 고쳤다. 캡처는 Launch 진입점(`RunApplication`·`RunEmbeddedApplication`)이 하고, 실행 파일 본문은 `ApplicationContext::Arguments()`로 받는다.

```cpp
namespace rocks {

// 진입점이 한 번 캡처하는 프로세스 명령줄이다. 캡처 뒤 불변이고 모든 스레드가 읽을 수 있다.
class ProcessCommandLine {
public:
    // OS 한계(Windows 32767 UTF-16 단위, Linux·Android ARG_MAX 2 MiB, macOS 1 MiB)에 맞춘 상한이다.
    // 저장소는 정적 BSS이고 쓴 만큼만 실제 메모리가 된다.
    static constexpr usize MaxArguments = 32 * 1024;
    static constexpr usize MaxTotalBytes = 2 * 1024 * 1024;
    static constexpr usize MaxExecutablePathBytes = 96 * 1024;

    // 진입점에서 한 번 부른다. 오류: 두 번째 호출 AlreadyExists, 잘못된 UTF-16·UTF-8 InvalidArgument,
    // 상한 초과 CapacityExceeded, 실행 파일 경로를 얻지 못하면 그 파일 상태의 오류.
    // Windows wmain·wWinMain 진입점: CRT가 나눈 넓은 문자 argv(__wargv)를 UTF-8로 바꾼다.
    static Result<void> Capture(int argc, wchar_t** argv) noexcept;
    // POSIX main 진입점: argv를 UTF-8로 검증해 복사한다(UTF-8이 아니면 InvalidArgument).
    static Result<void> Capture(int argc, char** argv) noexcept;

    // 전제: Capture 성공(아니면 assert 실패).
    static const ProcessCommandLine& Get() noexcept;
    static bool IsCaptured() noexcept;

    // 실행 파일 경로다. argv[0]이 아니라 OS에서 얻는다(Windows GetModuleFileNameW,
    // 그 밖은 platform::GetExecutablePath).
    StringView ExecutablePath() const noexcept;
    Span<const StringView> Arguments() const noexcept;  // 실행 파일 제외, 원문 순서
};

} // namespace rocks
```

- **원문 보존.** 캡처한 인자는 바꾸지 않는다. UE `Set`·`Append`(`CLH` 88–94)에 대응하는 것이 없다. 응답 파일을 펼친 결과는 파서가 따로 만드는 값이고, 보관된 원문은 그대로다.
- **정규화.** UTF-8 검증만 한다. 대소문자·경로 구분자·따옴표는 건드리지 않는다. 따옴표 처리는 OS·CRT가 이미 했다.
- **Windows(개정 3, 크래시 처리 설계 D7).**
  - 진입점은 Launch의 `ROCKS_IMPLEMENT_APPLICATION`이 만드는 `wmain`(콘솔) 또는 `wWinMain`(GUI subsystem)뿐이다. `wWinMain`은 CRT가 이미 나눈 `__argc`·`__wargv`를 쓴다. 둘 다 CRT가 넓은 문자로 나눈 argv를 UTF-8로 바꾸므로 ANSI 변환(5.2절의 깨짐·best-fit 보안 문제)을 거치지 않는다.
  - `main(char**)` 진입점은 Windows에서 지원하지 않는다. 진입점 직접 정의는 policy `process-entry-point`가 막는다. 그래서 개정 2의 `GetCommandLineW` 직접 분해(MSVC CRT 규칙 구현과 순수 함수 공개)는 하지 않는다. 버린 이유: Launch가 모든 Rocks 실행 파일의 진입점을 만들므로 직접 분해가 필요한 진입점이 남지 않고, CRT 규칙을 다시 구현하면 CRT와 어긋날 위험만 남는다.
  - 보조 수단으로 실행 파일 manifest에 `activeCodePage`=`UTF-8`(Windows 10 1903 이상)을 넣는 것을 검토한다. 그러면 남은 ANSI API도 UTF-8이 된다. 다만 OS 버전에 기대므로 캡처 경로의 근거로 삼지 않는다(20절).
- **Android.** argv가 없다. 런처 intent의 추가 인자 문자열을 같은 형태로 넣는 경로는 Android 호스트를 만들 때 정한다(20절).
- **로그용 형태와 비밀 가리기(D16).** 원문은 로그·trace·크래시 보고에 직접 쓰지 않는다. 파서가 스키마를 알고 난 뒤 `RedactedArguments(schema, out)`로 `Secret` 옵션의 값을 `<redacted>`로 바꾼 형태를 만든다. 시작 로그는 이 형태만 쓴다.
  - 파싱 실패로 스키마 판정을 못 한 인자도 원문 대신 `<unparsed>`로 남긴다. 비밀 값이 오타 때문에 그대로 새는 것을 막는다.
  - `--set` 값은 설정 쪽 비밀 표시가 생기면 그 대상의 값을 가린다. 지금 설정에는 비밀 표시가 없다.
- **AppInfo와의 관계.** 둘은 따로 설치되는 불변 값이다. `ProcessCommandLine`은 0단계, AppInfo는 3단계에서 설치된다. AppInfo는 명령줄을 소유하지 않는다. AppInfo의 칸 일부(`sessionId`·`instanceId`·실행 성격)가 파싱 결과에서 온다.
- **하위 프로세스 상속.** UE `RegisterArgument(…, Inherit)`처럼 옵션에 "자식에게 물려준다" 표시를 두는 것은 이번 범위에서 뺀다. 자식을 띄우는 도구(셰이더 컴파일 worker 등)가 생길 때 `Inherit` 지정자로 더한다(20절).

## 13. 명령줄 파서(C++) — 선언

### 13.1 원칙

- 편의성의 기준점은 C#의 System.CommandLine이다. 명령 하나를 만들 때의 코드 양, 옵션 선언 한 줄로 파싱·검증·도움말이 나오는지, 형식 변환이 자동인지, 결과를 꺼내 쓰기가 편한지를 C#과 같은 수준 이상으로 맞춘다(15절 비교).
- **정의 한 곳.** 명령·하위 명령·옵션·위치 인자마다 이름·별칭·형식·기본값·필수 여부·설명·예시를 옵션 struct에 한 번 선언한다. 그 정의 하나로 파싱·검증·도움말을 모두 만든다. 설명을 두 번 쓰지 않는다.
- **외부 라이브러리 없음.** CLI11·cxxopts·argparse·Lyra·Boost.Program_options를 검토하고 제외했다. 이유는 크기다(사용자 결정 2026-11-18).
- **리플렉션은 선언 문법만 쓴다(D19·D30).** 선언은 `ROCKS_STRUCT`·`ROCKS_PROPERTY` 지정자 문법으로 하지만, Command struct는 TypeRegistry에 등록하는 리플렉션 타입이 아니다.
  - MetaCompiler는 Command struct에서 명령줄 스키마와 필드별 바인더만 생성한다. FieldSchema·`GeneratedType<S>` 등록·stable ID를 내지 않는다.
  - 그래서 실행 파일은 ObjectRuntime·ReflectionText를 링크하지 않아도 된다. 선언 매크로(`Meta/Annotations.h`) 때문에 MetaSchema 헤더에만 기대고, MetaSchema는 Core에만 의존한다. Core만 링크하는 벤치와 FoundationTests Core 계층도 옮길 수 있다(5.5절 "제약: 계층").
  - 개정 1의 "ReflectionText 대입 하나 = 옵션 하나" 방식은 버렸다. 원소 추가 표기가 없고 같은 경로 두 번이 오류이며, Validate 이유를 전할 수 없고, enum·bool 표기가 C#과 맞지 않기 때문이다(5.5절 "제약" 행).

### 13.2 선언 예

```cpp
// 명령줄 전용 enum이다. 이름 표만 생성하고 identity가 필요 없다(D30).
ROCKS_ENUM(Command)
enum class TraceReportFormat : uint8 {
    Text ROCKS_ENUMERATOR(Name = "text"),
    Json ROCKS_ENUMERATOR(Name = "json")
};

// 명령 하나 = 옵션 struct 하나. 도움말 문자열은 실행 중 출력이라 영어다.
ROCKS_STRUCT(Command(Name = "trace-report", Help = "Convert a trace file into a text report.",
                     Examples = ("trace-report run.rtrace",
                                 "trace-report run.rtrace report.json --format json")))
struct TraceReportOptions {
    ROCKS_GENERATED_BODY()

    explicit TraceReportOptions(Allocator& allocator) noexcept : input(allocator), channel(allocator) {
    }

    ROCKS_PROPERTY(Argument, Help = "Trace file to read")
    String input;

    ROCKS_PROPERTY(Argument, Help = "Report file to write; standard output when omitted")
    Optional<String> output;

    ROCKS_PROPERTY(Alias = "-f", Help = "Report format")
    TraceReportFormat format = TraceReportFormat::Text;

    ROCKS_PROPERTY(Help = "Only include these channels")
    Array<String> channel;
};
```

- 옵션 이름은 필드 이름(또는 `Name=`)을 kebab-case로 바꿔 만든다. 예: `channel` → `--channel`, `maxFrames` → `--max-frames`.
- 받는 필드 형식과 값 표기(C#도 같은 표기만 받는다 — 16.2절):

| 형식 | 표기 | 변환 |
|---|---|---|
| `bool` | 값 없는 플래그. 명시할 때는 `--flag=true`·`--flag=false`만(소문자 정확 일치) | 생성 바인더 |
| `int8`–`int64`, `uint8`–`uint64` | 10진. 전체 소비, 꼬리 문자 거부, 형식 범위를 넘으면 범위 오류 | Core `TryParseInt64`·`TryParseUInt64` 뒤 형식 범위 확인 |
| `float`·`double` | Core `TryParseDouble`·`TryParseFloat` 문법. 유한한 값만 받는다(바인더가 `nan`·무한대를 거부한다) | Core `NumberText` |
| `String` | 그대로(UTF-8) | 복사 |
| `Guid` | 36자 소문자 하이픈 표기 | Base `Guid` 텍스트 변환(새 공용 함수 — 7.1절) |
| 명령줄 enum(`ROCKS_ENUM(Command)`) 또는 일반 `ROCKS_ENUM` | 도구 이름 정확 일치, 대소문자 구분. 명령줄 enum의 도구 이름은 `[a-z][a-z0-9-]*`(kebab-case 허용). 일반 enum은 기존 `[A-Za-z0-9_]+` 규칙(`Specifiers.cs:323`)을 따르므로 `-`를 쓸 수 없다 | 생성 enum 표 |
| `Optional<T>` | T의 표기. 주지 않으면 빈 값 | — |
| `Array<T>` | 옵션을 반복할 때마다 원소 하나를 더한다. 쉼표로 나누지 않는다 | 원소마다 T 변환 |

- 그 밖의 형식(Name·struct·Map·ObjectRef 등)은 RMC 오류다. Name은 NamePool이 필요해 Core만 링크하는 도구가 쓸 수 없다.
- enum을 대소문자 구분으로 정한 이유: 옵션 이름이 대소문자를 구분하므로 한 규칙으로 맞춘다. 틀린 대소문자에는 이름 제안이 붙는다(`'JSON' is not a valid value for '--format'. Did you mean 'json'?`).
- 기본값은 생성자를 실행한 결과다(멤버 초기화식). 도움말의 `[default: …]`는 기본 인스턴스를 생성 바인더의 필드별 출력 함수로 써서 만든다. 수는 Core 숫자 출력(최단 왕복), enum은 도구 이름, bool은 `true`·`false`다.
- 필드 사이 검증은 Command 전용 hook `ValidateCommand`를 쓴다(14.2절). 예: Sandbox의 `--frames`는 `--engine`과 함께만 쓴다.

### 13.3 지정자(MetaCompiler에 더할 것)

| 대상 | 지정자 | 값 | 뜻 | 검사(RMC) |
|---|---|---|---|---|
| STRUCT | `Command(...)` 묶음 | 아래 키 | 이 struct는 명령줄 정의다. `Settings(...)`와 같은 묶음 지정자다 | `Settings`·`Version`·`Atomic`·`Binds`와 함께 쓰지 않는다. base 없는 비템플릿 struct, `S() noexcept` 또는 `explicit S(Allocator&) noexcept` |
| `Command` 안 | `Name` | 문자열 | 명령 경로. 하위 명령은 공백으로 잇는다(`"log keygen"`). 각 마디는 commandlet 이름 문법(소문자·숫자·`-`) | 문법, 예약어 `help` |
| `Command` 안 | `Help` | 문자열 | 한 줄 설명(명령 목록과 Description 절) | 비어 있지 않다 |
| `Command` 안 | `Examples` | 문자열 목록 `("…", "…")` | 예시 줄. 프로그램 이름을 빼고 쓴다 | **새 값 형태**: 지정자 문법에 문자열 목록을 더한다(`Specifiers.cs` 86–92·337행의 값 분류) |
| `Command` 안 | `Global` | 플래그 | 공용 옵션 struct(모든 명령에서 받는다 — System.CommandLine의 Recursive) | `Name`·`Group`과 함께 쓰지 않는다 |
| `Command` 안 | `Group` | 플래그 | 하위 명령 묶음의 설명만 선언한다(`Command(Name = "log", Group, Help = "Create log keys and decode or seal log files.")`). 필드가 없는 struct다 | 필드가 있으면 오류. 같은 이름의 묶음이 이미 자동으로 생기면 그 설명이 된다 |
| PROPERTY | `Help` | 문자열 | 옵션·인자 설명. 계획된 함수 리플렉션의 `ROCKS_FUNCTION(…, Help = "…")`([CVar](CVarSystem.ko.md) 14.3절)과 같은 키 이름이다 | `Hidden`이 아니면 필수 |
| PROPERTY | `Alias` | 문자열(쉼표 목록) | 다른 이름. `-f`(한 글자) 또는 `--other-name` | 문법, 같은 명령·공용 옵션과 겹침 |
| PROPERTY | `Argument` | 플래그 | 위치 인자. 선언 순서대로 받는다 | `Alias`와 함께 못 쓴다. 필수 인자는 선택 인자 앞, `Array` 위치 인자는 마지막 하나만 |
| PROPERTY | `Mandatory` | 플래그 | 반드시 줘야 하는 옵션. 기존 `Required`(참조 non-null)와 뜻이 달라 다른 이름을 쓴다 | `Optional`·`Array`·`bool`에는 못 쓴다 |
| PROPERTY | `ValueName` | 문자열 | 도움말의 `<값>` 자리 이름. 없으면 옵션 이름(대시 뺀 것) | 문법 |
| PROPERTY | `Range` | 문자열 `"1..1000000"`, 한쪽만 `"0<..1000"`·`"1.."` | 수 범위. `<`는 그 끝을 뺀다. MetaCompiler가 필드 형식으로 양 끝을 해석해 검사한다 | 수 형식만, 최소 ≤ 최대 |
| PROPERTY | `Hidden` | 플래그 | 도움말에 보이지 않는다(받기는 한다) | — |
| PROPERTY | `Secret` | 플래그 | 로그용 명령줄에서 값을 가린다(12절) | `bool`에는 못 쓴다 |
| PROPERTY | `NotInRelease` | 플래그 | Release 구성에서 받지 않는다(10.2절). 기존 설정 지정자 `DevelopmentOnly`(설정 전용, `Cheat`로 바뀔 예정 — 5.5절)와 다른 Command 전용 키다 | — |
| PROPERTY | `Remaining` | 플래그 | 해석하지 않은 남은 인자를 원문 순서대로 받는다(14.1절, D29) | `Array<String>`만, 명령마다 하나, `Array` 위치 인자와 함께 못 쓴다, `Global`에는 못 쓴다 |

- **Command 키는 Command struct 안에서만.** `Command` 묶음이 없는 struct의 필드에 위 PROPERTY 키(`Help`·`Alias`·`Argument`·`Mandatory`·`ValueName`·`Range`·`Hidden`·`Secret`·`NotInRelease`·`Remaining`)를 쓰면 RMC 오류다(설정 지정자의 RMC2210과 같은 방식). 거꾸로 Command struct의 필드에는 설정 지정자와 직렬화 지정자(`Transient`·`SaveGame` 등)를 쓸 수 없다.
- **위치 인자 필수 규칙.** `Optional`·`Array`가 아닌 위치 인자는 필수다. 따로 표시하지 않는다.
- **도움말은 문서 주석에서 가져오지 않는다(D18).** 손으로 쓴 주석은 한국어이고 도움말은 실행 중 출력이라 영어다. 문서 주석을 가져오면 이 규칙이 깨진다. 주석 추출은 MetaCompiler에 Clang 주석 노드 수집을 새로 넣는 일이기도 하다.
- **identity 면제(D30).** Command struct·명령줄 enum은 직렬화하지 않으므로 stable ID가 없다.
  - `rocksids` 항목이 필요 없고 `ids add`를 부르지 않는다. 필드를 더하거나 이름을 바꿔도 빌드만 하면 된다.
  - 모듈 안 중복 판정과 descriptor 왕복에는 MetaCompiler가 정규 이름(`rocks::HostCommandLineOptions`)에서 만든 키를 쓴다. 이 키는 저장하지 않는다.
  - Command 선언만 있는 모듈은 표지 헤더만 있으면 되고(빌드가 찾는다 — [리플렉션·빌드 보강](ReflectionFoundations.ko.md) 1.10절) `<Name>.rocksids.json`이 없어도 된다(RMC2100 면제). 반대로 `rocksids`에 Command struct 항목이 있으면 RMC 오류다.
  - 실행 파일 모듈(Programs 아래)도 리플렉션 헤더를 가질 수 있다(`ModuleDirectory`가 있으면 빌드가 찾는다). 지금 그런 모듈이 없으므로 CL1에서 Rocks.Build 경로를 함께 시험한다.
- **생성 코드.** Command struct마다 `GeneratedCommand<S>`를 낸다(`CppEmitter.cs` 796–815의 `Settings()` 선례와 같은 자리).
  - `Schema()` → `CommandLineSchema`(아래).
  - 필드마다 바인더 함수: 값 텍스트 하나를 그 필드 형식으로 바꿔 넣거나(배열은 원소 추가) 형식 오류를 진단에 남긴다. 함수표 하나로 묶어 스키마 필드 순서로 찾는다.
  - 필드마다 기본값 출력 함수(도움말 `[default: …]`)와 `Range` 검사 함수.
  - `ValidateCommand`가 있으면 그것을 부르는 함수.
  - 생성 헤더는 `CommandLine/CommandLineSchema.h`만 include한다. ObjectRuntime 헤더를 보지 않는다.
  - 모듈 사이 import descriptor 왕복(`Programs/Rocks.MetaCompiler/ModuleDescriptor.cs:31,243–244,676–677`)에 Command 항목을 더한다(공용 옵션 struct를 다른 모듈의 명령이 쓴다).
  - Game 대상의 Release 생성(`ROCKS_BUILD_RELEASE && ROCKS_TARGET_KIND_GAME`)에서는 `Help`·`Examples`·`ValueName` 칸을 빈 값으로 낸다(7.2절). `NotInRelease` 플래그는 모든 구성에서 스키마에 남는다. 받을지는 파서가 정한다(14.2절).

```cpp
// CommandLine/CommandLineSchema.h (생성 코드가 채운다). 모든 view는 생성 코드의 정적 저장소를 빌린다.
struct CommandLineFieldSchema {
    Span<const char> field;      // C++ 필드 이름(진단·시험용)
    Span<const char> option;     // "--max-frames", 위치 인자·Remaining은 빈 값
    Span<const char> aliases;    // "-n" 또는 "-n,--count"
    Span<const char> valueName;
    Span<const char> help;
    Span<const char> range;      // "1..1000000" 또는 빈 값
    Span<const Span<const char>> enumValues; // enum 필드의 도구 이름(이름순), 그 밖은 빈 값
    CommandLineValueKind kind;   // Bool·Int·UInt·Float·String·Guid·Enum
    CommandLineFieldFlags flags; // Argument·Mandatory·Hidden·Secret·NotInRelease·Remaining·Optional·Array
};

struct CommandLineSchema {
    Span<const char> name;       // "trace-report", 공용 옵션은 빈 값
    Span<const char> help;
    Span<const Span<const char>> examples;
    CommandLineSchemaFlags flags; // Global·Group
    Span<const CommandLineFieldSchema> fields;
    const CommandLineBinder* binder; // 생성 바인더 함수표(필드 순서)
};
```

- 문자열 칸은 `SettingsSchema`(`Meta/SettingsSchema.h:55–69`)와 같이 `Span<const char>`다. 필드는 stable ID가 없으므로 `Guid` 대신 스키마 안 순번(배열 index)으로 가리킨다.

### 13.4 콘솔 명령과의 관계

콘솔 명령 인자 struct(`trace.enable channels=cpu,gpu`)는 리플렉션 struct이고 ReflectionText로 대입한다(5.5절). 명령줄 Command struct는 리플렉션 타입이 아니고 생성 바인더로 대입한다. 문법도 다르다. 콘솔은 `name=value`, 명령줄은 `--name value`다. Config D9(콘솔 명령과 commandlet registry는 따로)를 유지한다. 다만 `Help` 지정자는 같은 키이므로, 콘솔 `help <명령>`이 나중에 같은 문자열을 쓸 수 있다.

## 14. 명령줄 파서(C++) — 동작

### 14.1 문법 규칙(양쪽 공통 — D20)

| 규칙 | 내용 |
|---|---|
| 옵션 철자 | `--kebab-case`. 한 글자 별칭은 `-x`. 대소문자를 구분한다 |
| 값 | `--name value`와 `--name=value` 둘 다. 구분자는 `=` 하나다(`--name:value`는 받지 않는다). 값이 `-`로 시작하면 `=` 형태만 된다(음수 등) |
| bool | `--flag`는 true. 명시하려면 `--flag=true`·`--flag=false`. 다음 토큰을 값으로 먹지 않는다(`--flag false`의 `false`는 위치 인자다) |
| 반복 | `Array<T>` 필드는 옵션을 반복할 때마다 원소를 하나 더한다. 쉼표로 나누지 않는다(값 안의 쉼표는 그 값의 일부 — `--set=log.levels=a:debug,b:info`). 스칼라 옵션을 두 번 주면 오류다 |
| 위치 인자 | 옵션이 아닌 토큰을 선언 순서대로 채운다. 옵션과 섞어 써도 된다 |
| `--` | 그 뒤는 모두 위치 인자다(남으면 `Remaining`) |
| 하위 명령 | 첫 위치 토큰부터 명령 경로를 찾는다(`log keygen`). 공용 옵션은 명령 앞뒤 어디서나 받는다 |
| 묶음 `-abc`, `/x`, `[directive]` | 받지 않는다 |
| 알 수 없는 인자(D29) | 명령에 `Remaining` 필드가 있으면 알 수 없는 옵션과 남는 위치 토큰을 원문 순서대로 그 필드에 넣는다. 없으면 정책을 따른다: 도구·commandlet·Debug·Development 게임 호스트는 오류(종료 2), **Game 대상 Release 호스트는 경고**로 남기고 무시한다. 경고에는 옵션 이름(`-`로 시작하는 토큰의 `=` 앞)만 쓰고 값과 위치 토큰은 `<unparsed>`로 가린다(9단계 로그 — 8.3절) |
| 응답 파일 | `@path`(도구·commandlet만 — 14.7절) |
| 도움말 | `--help`·`-h`·`-?`, `help [명령…]` |
| 버전 | `--version`(최상위만) |

- Game 대상 Release를 경고로 둔 이유: 스토어 런처와 플랫폼이 실행 파일에 자기 인자를 붙인다. 출시 게임이 그 인자 때문에 시작하지 못하면 안 된다. 개발 빌드는 오타를 잡으려고 오류로 둔다.
- `Remaining` 쓰임: FoundationTests의 `process-probe <mode> …`처럼 뒤 인자를 다른 해석기(probe mode)에 넘기는 명령.

### 14.2 값 변환과 바인딩(D19)

- **생성 바인더.** 옵션 값 하나는 생성 바인더 함수 호출 하나다. 바인더는 값 텍스트를 필드 형식으로 바꿔 넣는다. `Array` 필드는 원소 하나를 더하고, 스칼라 필드는 넣는다. 변환은 13.2절 표대로 Core `NumberText`·Base `Guid` 텍스트 변환·생성 enum 표만 쓴다. ReflectionText·TypeRegistry·ObjectRegistry를 만들지도 부르지도 않는다.
- **검사 순서.** 다음 순서로 진행하고, 각 단계의 진단은 모두 모은다.
  1. **토큰 단계.** 명령 경로를 찾고, 토큰마다 필드를 찾아 바인더를 부른다. 이 단계의 오류는 알 수 없는 옵션·명령·인자, 값 없음, 형식 오류, 스칼라 중복, Release의 `NotInRelease` 옵션이다. Release의 `NotInRelease` 옵션은 알 수 없는 옵션으로 다룬다.
  2. **필수 단계.** `Mandatory` 옵션과 필수 위치 인자가 채워졌는지 본다.
  3. **범위 단계.** 값이 들어온 필드의 `Range`를 본다. 기본값은 범위를 보지 않는다. 기본값이 범위 밖이면 MetaCompiler가 거부한다.
  4. **검증 단계.** `ValidateCommand` hook이 있으면 부른다. 1–3단계에 오류가 하나라도 있으면 부르지 않는다. 덜 채워진 struct를 검증하지 않기 위해서다.
- **`ValidateCommand` hook.** Command struct의 static 멤버다. 영어 이유 문자열을 진단으로 남긴다. 리플렉션 struct의 `Validate`(`Result<void>`만 돌려준다 — 5.5절)는 Command struct에 쓸 수 없다(RMC).

```cpp
struct SandboxOptions {
    // …
    static void ValidateCommand(const SandboxOptions& options, CommandValidation& validation) noexcept {
        if (options.frames.HasValue() && !options.engine) {
            validation.Fail("Option '--frames' requires '--engine'.");
        }
    }
};
```

- `validation.Fail`의 문자열은 그대로 `error: ` 뒤에 나온다. 문구 규칙은 14.4절을 따른다(영어, 끝에 마침표, 옵션 이름은 작은따옴표).
- **공용 옵션과 명령 옵션을 한 번에.** 파서는 대상 struct 여럿을 받는다. 보통 공용 옵션 하나와 명령 하나다. 두 struct 사이의 조합 검사는 실행 함수가 한다. `ValidateCommand`는 struct 하나만 본다.

```cpp
// 런타임 API(CommandLine 모듈). 도움말은 그리지 않는다(짧은 사용법 줄만 만든다).
struct CommandLineRules {
    bool release = IsReleaseBuild;                     // NotInRelease 옵션을 거부한다. 시험은 Debug에서 true로 준다
    UnknownArgumentPolicy unknown = UnknownArgumentPolicy::Error; // Game 대상 Release 호스트는 WarnAndIgnore
};

// target은 GeneratedCommand<S>의 스키마와 S 값 하나를 묶는다: CommandLineTarget::Of(options).
Result<CommandLineOutcome> ParseCommandLine(Span<const StringView> arguments,
                                            Span<const CommandLineTarget> targets,
                                            const CommandLineRules& rules,
                                            CommandLineDiagnostics& diagnostics) noexcept;

enum class CommandLineOutcome : uint8 {
    Parsed,       // targets가 채워졌다(경고만 있을 수 있다)
    HelpRequested,
    VersionRequested,
    UsageError    // diagnostics에 오류가 있다. 종료 코드 2
};
```

- `rules.release`가 런타임 값이므로, `NotInRelease` 동작은 Debug에서 `release = true`로 시험한다(17절). 도움말 문자열을 빼는 것은 컴파일 시점 일이라 MetaCompiler 생성 결과 시험으로 확인한다.
- 결과는 옵션 struct 값이다. 실행 함수는 `const Options&`로 받는다.

### 14.3 호스트 공용 옵션과 `--set`·`--trace`·`--config`(D25)

```cpp
// HostConfig 모듈. 모든 호스트(게임·Sandbox·RocksCommandlet)가 받는 공용 옵션이다.
ROCKS_STRUCT(Command(Global))
struct HostCommandLineOptions {
    ROCKS_GENERATED_BODY()

    explicit HostCommandLineOptions(Allocator& allocator) noexcept;

    ROCKS_PROPERTY(Help = "Override a setting for this run", ValueName = "target=value")
    Array<String> set;

    ROCKS_PROPERTY(Help = "Read an extra project-layer config file", ValueName = "file")
    Optional<String> config;

    ROCKS_PROPERTY(Help = "Enable trace channels or presets", ValueName = "channels")
    Array<String> trace;

    ROCKS_PROPERTY(Help = "Project directory", ValueName = "path")
    Optional<String> project;

    ROCKS_PROPERTY(Help = "Never show dialogs or wait for user input")
    bool unattended = false;

    ROCKS_PROPERTY(Help = "Use a fixed time step and a fixed random seed")
    bool deterministic = false;

    ROCKS_PROPERTY(NotInRelease, Range = "0<..", ValueName = "rate",
                   Help = "Fixed time step rate in frames per second")
    Optional<double> fps;

    // … 10.2절 나머지
};
```

- `--set`·`--trace`·`--config`를 같은 파서로 옮긴다. 인자 배열은 한 번만 읽는다.
  - `HostSettings::ApplyCommandLine`은 원시 인자 대신 `Span<const String>`(파싱한 `set` 배열)을 받는다. `--set=`를 두 번 훑는 일(5.4절)이 없어진다.
  - `TraceController::ApplyCommandLine`도 `trace` 배열을 받는다.
- **값의 의미는 그대로다.** `--set` 값은 지금처럼 Config가 ReflectionText 대입으로 해석하고 출처는 `--set`이다. 명령줄 파서는 `--set` 값을 문자열로만 옮긴다. `--trace` 값은 지금처럼 쉼표 목록을 TraceController가 나눈다. 바뀌는 것은 인자를 찾는 곳뿐이다.
- `--set=a=b`와 `--set a=b`를 둘 다 받는다(첫 `=`까지가 옵션 이름).
- 출시 빌드의 `--set` 제한(Config D5)은 파서가 아니라 Config가 그대로 판정한다.
- 도움말에서 공용 옵션은 각 명령의 Options 절에 함께 나온다.

### 14.4 출력 규약: 공통과 오류(D21·D22)

**Rocks 명령줄 출력 규약**은 이 절과 14.5절이다. C++(`CommandLine`·`CommandLineTools`)과 C#(`RocksCli`)은 각자 이 규약대로 렌더한다. 어느 쪽도 다른 라이브러리의 기본 출력을 따라가지 않는다. System.CommandLine의 기본 도움말·오류 출력은 쓰지 않는다(16.2절). 모양은 System.CommandLine 기본 도움말에서 가져왔지만, 기준은 이 규약이다.

| 항목 | 규약 |
|---|---|
| 인코딩 | UTF-8, BOM 없음. C++은 표준 스트림에 byte 그대로 쓰고, Windows 콘솔이면 넓은 문자 API로 쓴다. C#은 stdout·stderr writer를 `new UTF8Encoding(false)`로 만든다 |
| 줄 끝 | 모든 플랫폼에서 `\n` 하나. C++은 CRT 텍스트 모드 변환을 거치지 않는다. C#은 writer의 `NewLine`을 `"\n"`으로 둔다 |
| 언어 | 영어. 문화권 형식을 쓰지 않는다(수는 13.2절 표기). 사용자가 준 토큰·값을 다시 보일 때는 작은따옴표로 감싸고, 제어 문자(U+0000–U+001F, U+007F)는 `\xHH`로 쓴다 |
| 출력 대상과 종료 코드 | 아래 표 |
| 너비 | 터미널이면 그 열 수(C++ `platform::TerminalColumns(stream)`, C# 출력을 돌리지 않았을 때 `Console.WindowWidth`), 터미널이 아니면(파일·파이프) 80. 범위는 [40, 200]으로 자른다. 시험은 너비를 인자로 준다 |
| 글자 수 | UTF-8 코드 포인트로 센다. 도움말 문자열은 영어 ASCII이고 기본값 표시만 비 ASCII일 수 있다 |

| 결과 | 출력 | 종료 코드 |
|---|---|---|
| 성공 | 명령 출력 | 0 |
| 도움말·`--version` | stdout | 0 |
| 실행 실패(명령이 실패를 돌려줌) | stderr `error: <이유>` 한 줄 | 1 |
| 사용법 오류 | stderr: 오류 줄들 + 짧은 사용법 줄 + 안내 줄 | 2 |
| 무시한 알 수 없는 인자(Game 대상 Release) | 로그 Warning(stderr가 아니다) | 영향 없음 |

오류 문구(양쪽 같음, C++·C# 모두 이 표에서만 문구를 만든다):

| 종류 | 문구 |
|---|---|
| 알 수 없는 옵션 | `Unrecognized option '--fromat'. Did you mean '--format'?` |
| 알 수 없는 명령 | `Unrecognized command 'trace-reprot'. Did you mean 'trace-report'?` |
| 남는 위치 인자 | `Unrecognized argument 'extra'.` |
| 값 없음 | `Option '--format' requires a value.` |
| 형식 오류 | `Cannot parse 'forty-two' for option '--frames' as uint64.`(형식 이름은 C++ 필드 형식 이름: `bool`·`int32`·`uint64`·`float`·`double`·`string`·`guid`) |
| 범위 밖 | `Value '0' for option '--frames' is out of range 1..1000000000.` |
| enum 값 오류 | `'xml' is not a valid value for '--format'. Allowed values: json, text.` 이름 제안이 있으면 `Did you mean 'json'?`를 대신 붙인다 |
| bool 값 오류 | `'yes' is not a valid value for '--unattended'. Allowed values: false, true.` |
| 필수 옵션 없음 | `Option '--out' is required.` |
| 필수 인자 없음 | `Required argument 'input' was not provided.` |
| 명령 없음 | `Required command was not provided.` |
| 스칼라 중복 | `Option '--format' was specified more than once.` |
| 옵션 사이 검증 | `ValidateCommand`가 `Fail`로 준 문자열 그대로 |
| 응답 파일 | `Cannot read response file 'x.rsp': NotFound.`(14.7절) |
| 무시 경고(로그) | `Ignored unrecognized option '--epicapp'.`, `Ignored unrecognized argument '<unparsed>'.` |

- 오류는 stderr에 `error: ` 접두로 모두 쓴다. 그다음 그 명령의 짧은 사용법 한 줄과 안내 한 줄을 쓴다. 종료 코드는 2다.

```
error: Unrecognized option '--fromat'. Did you mean '--format'?
Usage: RocksCommandlet trace-report <input> [<output>] [options]
Run 'RocksCommandlet trace-report --help' for more information.
```

- **이름 제안(공용 함수).** 거리 계산은 Core `EditDistance`(Damerau-Levenshtein, 인접 전치 하나 — OSA) 하나다. [CVar](CVarSystem.ko.md) 12절의 `ReportUnresolved`와 같은 함수다. 문턱은 호출자가 이름 붙은 상수로 준다.
  - 명령줄 문턱: `max(MinCommandLineSuggestionDistance, 길이 / CommandLineSuggestionLengthDivisor)`. 값은 1과 3이다. 짧은 옵션 이름에 CVar 문턱(3)을 쓰면 `--fps`→`--out` 같은 엉뚱한 제안이 나온다.
  - 후보는 그 명령의 옵션·별칭과 공용 옵션이다. 명령 오류면 형제 명령, enum 값 오류면 enum 값이다. 대상은 대시를 뗀 이름이고 ASCII 대소문자를 무시하고 비교한다. 가장 가까운 것을 고르고, 같은 거리면 사전순 첫째다. 후보가 없으면 제안 문장을 붙이지 않는다.
  - C#은 같은 알고리즘·문턱을 `RocksCli`에 옮겨 쓰고, 같은 사례 표로 시험한다(17절).
- 진단은 `CommandLineDiagnostics`에 모두 모은다(첫 오류에서 멈추지 않는다). 출력 순서는 14.2절의 단계 순서이고, 같은 단계 안에서는 인자 순서다.

### 14.5 출력 규약: 도움말(D21)

절은 Description·Usage·Arguments·Options·Commands·Examples 순서다. 비어 있는 절은 쓰지 않는다. 절 머리 다음 줄부터 두 칸 들여 쓰고, 절 사이에는 빈 줄 하나를 둔다.

**최상위**(`RocksCommandlet --help`, `RocksCommandlet help`). 이 예는 10.2절 실행 성격 옵션을 줄였다:

```
Description:
  Rocks command-line tools.

Usage:
  RocksCommandlet [command] [options]

Options:
  --set <target=value>  Override a setting for this run
  --config <file>       Read an extra project-layer config file
  --trace <channels>    Enable trace channels or presets
  --project <path>      Project directory
  --unattended          Never show dialogs or wait for user input
  --version             Show version information
  -?, -h, --help        Show help and usage information

Commands:
  config-dump                     Print the effective settings as JSON.
  core-check                      Run the core self-check.
  log                             Create log keys and decode or seal log files.
  trace-report <input>            Convert a trace file into a text report.
    [<output>]
```

이 예는 너비 80이다. Commands 절의 왼쪽 열은 32(80의 40%)이고, `trace-report <input> [<output>]`(31자)은 두 칸 간격을 둘 수 없어 이름 칸을 접었다.

**명령 상세**(`RocksCommandlet trace-report --help`, `RocksCommandlet help trace-report`):

```
Description:
  Convert a trace file into a text report.

Usage:
  RocksCommandlet trace-report <input> [<output>] [options]

Arguments:
  <input>   Trace file to read
  <output>  Report file to write; standard output when omitted

Options:
  -f, --format <json|text>  Report format [default: text]
  --channel <channel>       Only include these channels
  --set <target=value>      Override a setting for this run
  --config <file>           Read an extra project-layer config file
  --trace <channels>        Enable trace channels or presets
  --project <path>          Project directory
  --unattended              Never show dialogs or wait for user input
  -?, -h, --help            Show help and usage information

Examples:
  RocksCommandlet trace-report run.rtrace
  RocksCommandlet trace-report run.rtrace report.json --format json
```

**하위 명령 묶음**(`RocksCommandlet log --help`): Description(`Command(Name = "log", Group, Help = …)`의 설명)·Usage(`RocksCommandlet log [command] [options]`)·Options·Commands(`keygen`·`decode`·`seal`) 절이 나온다.

배치 규칙:
- **너비.** 14.4절 공통 표를 따른다.
- **왼쪽 열.** 너비는 `min(가장 긴 이름 칸 + 2, 전체 너비의 40%)`다(소수점 아래는 버린다). 절마다 따로 정한다. 이름 칸 + 두 칸 간격이 열보다 길면 이름 칸을 공백 경계에서 접고, 이어지는 줄은 두 칸 더 들여 쓴다(위 `trace-report <input> [<output>]`).
- **오른쪽 열.** 설명은 공백 경계에서 접는다. 한 단어가 열보다 길면 그 단어만 열 너비에서 자른다.
- **이름 칸 모양.** 별칭이 먼저이고 긴 이름이 뒤다(`-f, --format`). 값 자리는 `<ValueName>`이다. enum은 `<값|값>`(도구 이름, 이름순)이고, bool은 값 자리가 없다. 위치 인자는 `<이름>`, 선택 위치 인자는 Usage에서 `[<이름>]`, 반복 위치 인자는 `<이름>...`이다.
- **정렬.** 옵션은 선언 순서이고, 공용 옵션이 그 뒤에, 그다음 `--version`(최상위만)·`--help`가 온다. 명령 목록은 이름순이다. `Hidden` 옵션은 쓰지 않는다. Release 구성에서는 `NotInRelease` 옵션도 쓰지 않는다.
- **필수 표시.** 필수 옵션은 설명 끝에 ` (REQUIRED)`를 붙인다.
- **기본값 표시.** `[default: …]`는 기본값이 빈 문자열·빈 배열·빈 Optional·false가 아닐 때만 붙인다. 값은 13.2절의 필드별 출력 함수로 쓴다.
- **출력 대상.** 도움말은 stdout이고 종료 코드 0이다.
- **Game 대상 Release.** 게임 호스트는 `CommandLineTools`를 링크하지 않고 도움말 문자열도 비어 있다. `--help`는 `Help is not available in Release builds.` 한 줄을 stdout에 쓰고 종료 0이다. Program 대상의 Release는 도움말이 그대로 나온다(7.2절).

### 14.6 commandlet 구조(D24)

```cpp
// Programs/Commandlet의 등록 함수. 명령 하나에 한 줄이다.
Result<void> RegisterCommands(CommandSet& commands) noexcept {
    if (Result<void> added = commands.Add<TraceReportOptions>(&RunTraceReport); !added) {
        return added;
    }

    // config-dump 등록은 Config(Runtime)에서 이 실행 파일로 옮겼다(7.1절, CL4).

    if (Result<void> added = commands.Add<ConfigDumpOptions>(&RunConfigDump); !added) {
        return added;
    }

    return Result<void>::Success();
}

// 실행 함수: 파싱·검증이 끝난 옵션 struct를 받는다. 반환은 0..125다.
Result<int32> RunTraceReport(CommandContext& context, const TraceReportOptions& options) noexcept;
```

- `CommandSet`(Developer `CommandLineTools`)이 하는 일:
  - 명령 등록. 이름·설명·예시는 스키마에서 온다.
  - 하위 명령 묶음(`log keygen`의 `log`)을 자동으로 만든다. 묶음 설명은 `Command(..., Group)` struct를 `commands.AddGroup<LogGroup>()`으로 등록해 준다(13.3절).
  - 공용 옵션 struct를 연결한다.
  - `CommandLineRules`를 정한다: 도구는 `UnknownArgumentPolicy::Error`이고 `release`는 실제 구성이다.
  - `--help`·`help <명령…>`·`--version`을 처리한다.
  - 응답 파일을 펼친다.
  - 진단을 출력하고 종료 코드를 정한다(0 성공, 1 실행 실패, 2 사용법 오류).
- 명령이 등록되면 최상위 도움말의 Commands 목록과 그 명령의 상세 도움말이 저절로 생긴다. 별도 도움말 코드가 없다.
- `CommandContext`는 다음을 준다.
  - 표준 출력·오류 writer(지금 `CommandletOutput`의 역할)
  - 할당기
  - 부트가 끝난 `HostSettings`
  - `AppInfo`
- 단일 소유 스레드 규칙, 이름 문법(소문자·숫자·`-`), 반환 범위 0..125는 지금 `CommandletRegistry`(`Commandlet.h:24–57`)와 같다. 개수·길이 상한은 이름 붙은 상수다.
- `--list`는 없앤다. `help`가 목록이다.
- 명령의 실행 함수는 옵션 struct를 받으므로 `log` 하위 명령의 "인자 위치 고정 비교"(5.2절) 같은 코드가 없어진다.

### 14.7 응답 파일(D23)

- `@path` 토큰을 그 파일의 토큰으로 바꾼다. `CommandSet`(도구·commandlet)만 한다. 게임 호스트는 받지 않는다. 게임은 긴 인자 목록이 필요 없고, 받지 않으면 출시 빌드가 임의 파일을 읽는 경로가 하나 줄어든다.
- 토큰 문법은 Rocks가 정한다. C#도 System.CommandLine의 응답 파일 처리를 끄고 `RocksCli`가 같은 문법으로 펼친다(16.2절).
  - 파일은 UTF-8이다(BOM이 있으면 뗀다). 줄 끝은 `\n`·`\r\n`을 받는다.
  - 앞뒤 공백을 뗀 줄이 비었거나 `#`로 시작하면 건너뛴다.
  - 줄을 공백(스페이스·탭)으로 나눈다. 큰따옴표로 묶은 부분은 공백을 포함한 한 토큰의 일부이고 따옴표는 지운다. 따옴표 안에서 `""`는 따옴표 글자 하나다. `\`는 어디서나 글자 그대로다. 그래서 `"C:\dir\"`가 그대로 `C:\dir\`이 된다(escape를 `\`로 하면 `\`로 끝나는 Windows 경로가 깨진다).
  - 닫히지 않은 따옴표는 오류다(`x.rsp:3`).
- 상한(이름 붙은 상수):
  - 파일 크기 1 MiB
  - 중첩 깊이 4
  - 펼친 뒤 인자 수 `ProcessCommandLine::MaxArguments`
- 순환(같은 파일을 다시 펼침)은 오류다.
- 파일을 못 읽으면 `Cannot read response file 'x.rsp': NotFound.`이고 종료 2다. 원래 오류 코드 이름을 붙인다.
- 펼친 위치는 진단에 `x.rsp:3`처럼 파일·줄로 나온다.

## 15. C++과 C# 나란히 보기

같은 명령(`trace-report` — 예시라 지금 명령에 없는 `--format`·`--channel`을 더했다)을 양쪽으로 쓴다.

**C# (System.CommandLine 파싱 + `RocksCli` 출력)**

```csharp
Argument<FileInfo> input = new("input")
{
    Description = "Trace file to read",
};
Argument<FileInfo?> output = new("output")
{
    Description = "Report file to write; standard output when omitted",
    Arity = ArgumentArity.ZeroOrOne,
};
Option<ReportFormat> format = RocksCli.EnumOption("--format", "-f",
    ReportFormat.Text, "Report format");   // 도구 이름 정확 일치 파서와 기본값 표시를 붙인다
Option<string[]> channel = new("--channel")
{
    Description = "Only include these channels",
};

Command command = new("trace-report", "Convert a trace file into a text report.")
{
    input,
    output,
    format,
    channel,
};
RocksCli.AddExamples(command,
    "trace-report run.rtrace",
    "trace-report run.rtrace report.json --format json");
// 루트에서 RocksCli.Invoke(root, args)가 파서 설정·도움말·오류 출력·종료 코드를 맡는다(16.2절).
command.SetAction(result => TraceReport.Run(
    result.GetValue(input)!,
    result.GetValue(output),
    result.GetValue(format),
    result.GetValue(channel) ?? []));
```

**C++ (Command 옵션 struct)** — 13.2절 선언(약 25줄, enum 포함) + 등록 한 줄 + 실행 함수:

```cpp
commands.Add<TraceReportOptions>(&RunTraceReport);

Result<int32> RunTraceReport(CommandContext& context, const TraceReportOptions& options) noexcept {
    // options.input, options.output, options.format, options.channel을 바로 쓴다.
}
```

| 기준 | C# (System.CommandLine) | C++ (Rocks) |
|---|---|---|
| 명령 하나의 코드 양 | 선언 약 25줄 + action | 선언 약 25줄 + 등록 1줄 |
| 옵션을 더할 때 할 일 | `Option<T>` 하나를 만들고 명령에 넣고 action에서 꺼낸다 | 필드 하나와 `ROCKS_PROPERTY` 한 줄. `ids add`·`rocksids` 편집은 없다(identity 면제 — D30) |
| 실행 파일 하나에 한 번 드는 비용 | 패키지 참조(중앙 버전) | 헤더에 `ROCKS_STRUCT(Command(...))` 표지(빌드가 찾고 MetaCompiler가 그 모듈을 처리한다 — 규칙 수정 없음). `rocksids` 파일은 없다 |
| 옵션 한 줄로 파싱·검증·도움말 | 된다(`Option<T>` + `Description`) | 된다(필드 + `ROCKS_PROPERTY(Help=…)`) |
| 형식 변환 | 자동(`T`에서). 받는 표기는 `RocksCli` 사용자 파서가 C++과 같게 좁힌다 | 자동(필드 형식 + 생성 바인더) |
| 결과 꺼내기 | 옵션마다 `GetValue(option)` | struct 필드 그대로(`options.format`) |
| 필드 사이 검증 | `Validators.Add`(문구는 14.4절 규칙) | `ValidateCommand` hook |
| 예시 절 | `RocksCli.AddExamples`(System.CommandLine에 없음) | `Command(Examples=…)` |
| 빌드 시간 | 없음 | 그 모듈의 MetaCompiler 실행(생성 TU가 바뀔 때만 다시 컴파일) |

두 쪽의 `--help` 출력은 14.5절 "명령 상세" 예와 byte 단위로 같아야 한다. 둘 다 출력 규약(14.4·14.5절)을 각자 렌더한 결과다. 프로그램 이름만 다르고, 시험은 같은 이름을 준다. 17절 golden 시험이 이를 확인한다.

## 16. C# 도구

### 16.1 파서 선택(D26)

| 항목 | System.CommandLine | 자체 작은 라이브러리 |
|---|---|---|
| 기능 | 선언형 옵션·인자·하위 명령, 자동 도움말, 형식 변환, 응답 파일, 이름 제안, 탭 완성 지시자 | 모두 새로 만든다 |
| 안정성 | 안정판 있다. NuGet 기준 3.0.0(2026-11-10), 2.0.x 줄(2.0.15)도 유지된다(판 번호·날짜는 미확인 — 20절) | 우리 책임 |
| 라이선스 | MIT | — |
| AOT·트리밍 | NuGet 페이지가 "trimming과 AOT 완전 지원"으로 적는다. 2.0부터 리플렉션 바인딩을 뺐다 | — |
| 의존 | net10.0 대상은 의존 없음 | 없음 |
| 이전 비용 | 도구 셋(Build.Cli·MetaCompiler·ToolTests)과 그 안의 하위 명령 묶음 둘(MetaCompiler `ids`·ToolTests `scaffold`)의 손 루프를 명령 정의로 바꾼다. 손 루프 코드가 줄어든다 | 같은 이전 + 라이브러리 작성 |
| 단점 | 저장소 첫 외부 NuGet 패키지다. restore가 필요하다(오프라인 빌드는 NuGet 캐시·사내 feed). 공급망 관리(버전 고정·lock 파일)가 생긴다. 메이저 판 사이 API 변화가 있었다(2.0 beta 동안 큰 변경) | 기능 차이를 계속 메워야 한다 |

**결정: System.CommandLine을 파싱에만 쓴다.** 사용자가 외부 패키지를 허용했다(2026-11-18). 선언형 명령 모델·하위 명령·형식 변환·필수·arity는 이 패키지를 쓴다. 도움말·오류·응답 파일·이름 제안은 출력 규약을 지키려고 `RocksCli`가 맡는다(16.2절).

들여올 때 지킬 것:
- **중앙 버전 관리.** 저장소 루트 `Directory.Packages.props`(Central Package Management)에 버전 하나를 고정한다. 착수 때의 최신 안정판을 쓴다.
- **lock 파일.** `packages.lock.json`과 `RestoreLockedMode`(CI)를 켜 공급망을 고정한다.
- **NuGet 감사와 경고 처리.** `Programs/Directory.Build.props`는 `TreatWarningsAsErrors`·`MSBuildTreatWarningsAsErrors`를 켠다. 그대로 두면 NuGet 감사 경고가 빌드 오류가 된다. 오프라인이면 NU1900이 나고, 취약점이 새로 공개되면 NU1901–NU1904가 난다. 같은 소스의 빌드 결과가 시간·네트워크에 따라 달라진다. 그래서 `NuGetAudit=false`로 두고, 감사는 CI의 별도 작업(`dotnet list package --vulnerable`)으로 돌린다. 감사 결과는 빌드를 막지 않고 보고로 남긴다.
- **빌드 증거.** `SourceSnapshot`의 루트 파일 목록(`Programs/Rocks.Tooling.Common/BuildEvidence.cs:28–29`)에 `Directory.Packages.props`와 `packages.lock.json`을 더한다. 빼면 패키지 판을 바꿔도 증거 digest가 그대로다.
- **policy.** 지금 policy에 PackageReference를 막거나 0건을 전제한 검사는 없다(`git grep` 확인, 2026-11-18). 대신 허용 목록 검사를 새로 둔다. PackageReference는 `Directory.Packages.props`에 있는 패키지만 쓸 수 있고, 각 패키지는 라이선스 기록이 있어야 한다.
- **라이선스 기록 자리.** native 외부 코드는 `ThirdParty.json`이 맡지만 NuGet 패키지는 대상이 아니다. `Programs/ThirdPartyPackages.json`(패키지 이름·판·SPDX 라이선스·출처 URL·도입 결정 문서)을 두고 위 policy 검사가 읽는다. 설치 SDK에 실을 때는 이 목록에서 고지 파일을 만든다.
- **Rocks.Build의 offline bootstrap**(`ThirdPartyBootstrapCommand.cs`)과 NuGet 캐시 관계를 정한다. 빌드 도구 자신(Rocks.Build.Cli)이 패키지를 쓰므로 첫 restore는 네트워크가 필요하다. 오프라인 기계는 NuGet 캐시나 사내 feed를 쓴다.
- **설치 SDK.** `Rocks.Build.Cli`가 `Rocks.Tooling.Cli`(그리고 System.CommandLine.dll)를 쓰므로 SDK의 `Tools/Rocks.Build.Cli`에 두 DLL을 함께 싣는다. 게임 규칙 프로젝트가 참조하는 `Rocks.Build.Abstractions`·`Rocks.Build`·`Rocks.Tooling.Common`(`InstalledSdk.cs:293–294`)은 System.CommandLine에 의존하지 않아야 한다. 게임 규칙 컴파일이 패키지 restore 없이 끝나야 하기 때문이다. 이 의존 방향을 policy 검사(어셈블리 참조 확인)로 막는다.

### 16.2 `RocksCli`(새 어셈블리 Rocks.Tooling.Cli)

System.CommandLine은 토큰을 명령 모델에 맞춰 나누고 값을 변환하는 데만 쓴다. 출력은 모두 `RocksCli`가 출력 규약(14.4·14.5절)대로 그린다. 그래서 System.CommandLine 판이 바뀌어도 출력 byte는 바뀌지 않는다.

**파서 설정 범위**(C++ 문법 규칙 14.1절과 같은 입력 집합을 받도록):

| 설정 | 값 | 이유 |
|---|---|---|
| 문화권 | 진입 때 `CultureInfo.DefaultThreadCurrentCulture`·`DefaultThreadCurrentUICulture`와 현재 스레드의 두 값을 `InvariantCulture`로 둔다 | System.CommandLine 리소스 문구와 수 형식이 OS 언어를 따르지 않게 한다 |
| 위성 리소스 | 도구 csproj에 `<SatelliteResourceLanguages>en</SatelliteResourceLanguages>` | ko 등 현지화 리소스 DLL을 출력에 싣지 않는다 |
| POSIX 묶음 | 끈다(`-abc`를 받지 않는다) | 14.1절 |
| `:` 구분자 | `--name:value`를 받지 않는다. 설정으로 끌 수 없는 판이면 토큰을 넘기기 전에 `RocksCli`가 `--name:` 형태를 알 수 없는 옵션으로 거부한다 | 구분자는 `=` 하나 |
| bool 값 소비 | bool 옵션의 arity를 0으로 둔다(`--flag false`의 `false`를 먹지 않는다). `--flag=true`·`--flag=false`는 `RocksCli`가 `=` 형태만 따로 받는다 | 14.1절 |
| 지시자 `[...]` | 루트 명령의 지시자를 모두 뺀다 | 14.1절 |
| 응답 파일 | System.CommandLine의 응답 파일 처리를 끄고, `RocksCli`가 14.7절 문법으로 먼저 펼친다 | 양쪽 같은 토큰 열 |
| 알 수 없는 토큰 | `TreatUnmatchedTokensAsErrors`는 명령에 `Remaining`이 있을 때만 끄고, 남은 토큰은 `ParseResult.UnmatchedTokens`에서 원문 순서로 꺼낸다 | D29 |
| 값 변환 | 수·bool·enum·Guid 옵션에 사용자 파서를 붙인다(`NumberText`와 같은 문법, enum 도구 이름 정확 일치). 기본 `Enum.TryParse`(대소문자 무시·숫자 허용)·`int.Parse`(문화권·앞뒤 공백 허용)를 쓰지 않는다 | 13.2절 표 |
| 기본 도움말·오류 action | 쓰지 않는다. `--help`·`-h`·`-?`·`--version` action과 파싱 오류 처리를 `RocksCli`의 것으로 바꾼다 | 14.4·14.5절 |

**일**

| 일 | 방법 |
|---|---|
| 출력 writer | stdout·stderr를 `UTF8Encoding(false)`, `NewLine = "\n"`인 writer로 감싼다 |
| 오류 출력·종료 코드 | `ParseResult.Errors`의 종류(알 수 없는 토큰·값 없음·변환 실패·필수 누락·명령 없음·중복)를 14.4절 표의 문구로 다시 만든다. System.CommandLine 문구는 쓰지 않는다. `error: ` 접두 + 짧은 사용법 + `--help` 안내, 종료 2 |
| 도움말 렌더러 | 명령 모델(`Command`·`Option`·`Argument`의 이름·별칭·설명·기본값·필수·숨김)을 읽어 14.5절 배치 규칙대로 그린다. Examples 절은 `AddExamples`가 등록한 줄이다 |
| 이름 제안 | 14.4절과 같은 거리 함수·문턱 상수·동점 규칙을 옮긴다. System.CommandLine 자체 제안은 쓰지 않는다 |
| `help <명령>` | 첫 토큰이 `help`면 `<명령…> --help`로 바꿔 파싱한다 |
| `--version` | 11.2절 모양(엔진 버전·구성·커밋·날짜)으로 쓴다. 값은 MSBuild가 넣는 `AssemblyInformationalVersion`과 `Build/Build.version`에서 온다 |
| 실행 실패 | action 안의 처리되지 않은 예외는 `error: ` 한 줄 + 종료 1(스택은 `--verbose`일 때만) |

- 크기: 900줄 이하(7.2절). 개정 1의 400줄은 System.CommandLine 기본 출력을 쓴다는 전제였다. 도움말 렌더러(약 300줄), 오류 문구 변환(약 150줄), 사용자 파서(약 150줄), 응답 파일·이름 제안(약 150줄), 설정·진입점(약 150줄)으로 다시 추정했다.

### 16.3 이전 대상

| 도구 | 지금 | 이전 뒤 |
|---|---|---|
| Rocks.Build.Cli | `build`·`test`·`sdk`·`game`·`bootstrap`, `--key value`만, 도움말 없음 | 하위 명령 다섯, `--offline`은 bool 플래그, 필수 옵션 표시 |
| Rocks.MetaCompiler | 단일 명령 + `ids` 하위 명령 7개, `RMC1000` 오류 | 기본 명령(빌드가 부르는 경로) + `ids …`. 빌드 도구가 내는 `RMC` 진단 코드는 그대로 두고 인자 오류만 공용 모양으로 바꾼다 |
| Rocks.ToolTests | 하위 명령 12개 + `scaffold plan`·`apply`·`validate` | 같은 구조, `--root`는 공용 옵션(Recursive) |
| 시험 실행 파일들(Rocks.Build.Tests 등) | `args.Contains` | 이전하지 않아도 된다. 바꾸면 같은 층을 쓴다 |

## 17. 시험 계획

무작위 사례는 고정 seed이고 타이밍에 기대는 시험은 없다. 스레드 시험의 순서는 명시 gate로 만든다.

- **AppInfo**
  - 만들기의 문자열 복사와 수명(입력 버퍼를 지운 뒤에도 값이 같다), 상한 초과 `CapacityExceeded`(자르지 않는다), 프로젝트 이름 문자 거부
  - `AppInfoScope` 중첩 Fatal과 설치 전 `Get()` assert. 둘 다 FoundationTests process-probe로 자식 프로세스에서 확인하고, 대화상자 없이 끝나야 한다
  - 설치 뒤 여러 스레드 읽기(TSan, macOS CI)
  - 시험용 교체 scope(이전 값 되돌림)
  - 부트 4단계 설정 등록 코드가 `AppInfo::Get().Traits()`를 읽을 수 있는지(8.3절 순서)
- **실행 성격(UE와 같은지)**
  - 10.2절 표의 모든 플래그와 조합. `--deterministic`이 고정 step(1/30)과 고정 시드를 켜는지, `--fps 60`만 주면 고정 step이 꺼진 채 `fixedDeltaSeconds`만 1/60인지, `--fps`와 `--use-fixed-time-step`을 함께 주면 1/60 고정 step인지, `--benchmark`가 `UsesFixedTimeStep()`과 고정 시드를 켜는지
  - Release 규칙(`CommandLineRules::release = true`, Debug에서 시험): `--benchmark`·`--fps`·`--instance-id`·`--session-id`·`--session-name`은 알 수 없는 옵션이고, `--deterministic`·`--fixed-seed`·`--use-fixed-time-step`·`--unattended`·`--headless`는 받는지
  - 시험 실행기 기본 `unattended`
  - unattended에서 Fatal이 대화상자 없이 끝나는지(process-probe). 대화상자 억제를 PlatformBase로 합친 뒤 TestSupport·ShaderToolSpike 경로도 같은 API를 쓰는지
  - Sandbox 자체 시험의 자식이 `--deterministic`으로 고정 step을 받는지
- **FrameTiming**
  - 고정 step에서 `currentSeconds`가 이전 + 고정 delta이고 `accumulatedSeconds`가 delta 합인지(비트 같음)
  - 고정 delta가 double 그대로인지(1/30 비트 비교)
  - 렌더 lane packet의 값이 packet을 만든 프레임 값과 같은지
  - worker가 제출한 프레임의 값을 보는지. 제출 뒤 게임 lane이 다음 프레임으로 넘어가도록 gate를 둔다. sleep을 쓰지 않는다
  - `LatestFrameIndex` 단조 증가
- **빌드 정보**
  - 생성 TU 값(구성·대상·커밋 길이·날짜 형식)
  - git이 없는 환경(빈 값, 빌드 성공), detached HEAD에서 `ROCKS_BUILD_BRANCH` 사용
  - 커밋만 바꾼 두 빌드의 `ToolBuildId` 같음
  - 같은 커밋 두 번 빌드의 BuildStamp 오브젝트 byte 같음(재현성)
  - 대상 종류 정의가 Game·Program 대상에 정확히 하나씩 들어가는지
  - `--version` 출력 golden
- **ProcessCommandLine**
  - Windows `wmain`(콘솔) 실행 파일과 `wWinMain`(GUI subsystem) 실행 파일 각각에 한글·이모지·전각 `－`·`＂`가 든 인자를 `Process::Start`로 넘겨 UTF-8 원문 왕복(best-fit 변환이 없는지). 구현됨: `Native.Launch.unicode-arguments`·`windowed-subsystem`(크래시 처리 LC3)
  - (개정 3에서 뺌) Windows 직접 분해 함수의 표 시험. 직접 분해를 하지 않는다(12절)
  - `ExecutablePath`가 argv[0]과 다르게 띄운 경우에도 실제 경로인지
  - 상한, 두 번 캡처, 잘못된 UTF-16(짝 없는 서로게이트)
  - 로그용 형태에서 `Secret` 값이 `<redacted>`, 파싱 실패 인자가 `<unparsed>`
- **파서(C++)**
  - 14.1절 문법 표의 모든 줄(각 규칙의 성공·실패). `--name:value`·`-abc`·`[directive]` 거부, `--flag false`의 `false`가 위치 인자가 되는지
  - 13.2절 형식 표의 모든 형식: 수 형식 범위 넘침, 꼬리 문자 거부, `nan`·무한대 거부, enum 대소문자 구분과 제안, bool `true`·`false`만, Guid 표기, `Array` 원소 추가(값 안 쉼표와 `\`로 끝나는 경로 보존)
  - 14.2절 검사 순서: 토큰 단계 오류가 있으면 `ValidateCommand`를 부르지 않는지, 필수 → 범위 → 검증 순서의 진단 순서, `ValidateCommand`의 이유 문자열이 그대로 나오는지
  - 14.4절 문구 표의 모든 종류, 이름 제안 문턱 경계(거리 = 문턱, 문턱 + 1)와 동점 규칙, 진단 여러 개의 순서
  - 공용 옵션 위치(명령 앞·뒤), `Range`(양 끝·한쪽·끝 제외 `<`), `Mandatory`
  - 알 수 없는 인자 정책(D29): 도구는 종료 2, `WarnAndIgnore`는 경고 진단과 `<unparsed>` 가림, `Remaining` 필드가 원문 순서로 받는지(`--` 뒤 포함)
  - `--set`·`--trace`·`--config`를 옮긴 뒤의 기존 Config·Trace 시험 통과와 `--set` 값의 출처 `--set` 유지
  - 링크 시험: Core만 링크하는 실행 파일(MathBench)이 `CommandLine` 모듈과 생성 바인더만으로 빌드되는지(ObjectRuntime·ReflectionText 기호 없음)
- **응답 파일**
  - 주석·따옴표·`""`·`\`로 끝나는 경로·빈 줄·닫히지 않은 따옴표, 중첩 깊이 4와 5, 순환, 크기 상한, 못 읽는 파일의 오류 코드 이름, 진단 위치 `파일:줄`
  - C#과 같은 입력 파일로 같은 토큰 열이 나오는지
- **MetaCompiler**
  - 13.3절 RMC 검사 각각의 실패 사례(`Alias`+`Argument`, 위치 인자 순서, `Array` 위치 인자 둘, `Mandatory`+`Optional`, `Range` 형식·순서·기본값이 범위 밖, 이름·별칭 겹침, `Command` 이름 문법, `Help` 누락, 받지 않는 필드 형식, `Remaining` 둘·형식, `Group`에 필드, Command 키를 Command 밖에서 사용, Command struct에 설정·직렬화 지정자, Command struct에 리플렉션 `Validate`)
  - identity 면제: Command만 있는 모듈이 `rocksids` 없이 빌드되는지, `rocksids`에 Command 항목이 있으면 오류인지, 필드를 더해도 `ids add` 없이 빌드되는지
  - 실행 파일 모듈(Programs 아래)의 리플렉션 헤더 탐색 경로
  - descriptor 왕복(다른 모듈의 공용 옵션 struct)
  - 생성 코드: `ROCKS_BUILD_RELEASE && ROCKS_TARGET_KIND_GAME`에서 도움말 문자열이 빈 값이고, Program 대상 Release에서는 남는지(생성 결과 golden)
- **출력 규약 golden(C++)**
  - 대상: 최상위, 명령 상세, 하위 명령 묶음(묶음 설명), `help <명령>`, 오류 출력(14.4절 표의 모든 종류)
  - 너비 60·80·120 각각, 그리고 [40, 200] 자르기
  - 긴 이름 칸 접기, 긴 설명 접기, 열보다 긴 단어, enum 목록, `(REQUIRED)`, `[default: …]`, 숨김 옵션 없음, Release 규칙에서 `NotInRelease` 옵션 없음
  - 줄 끝이 `\n`뿐이고 BOM이 없는지(Windows 포함), 제어 문자 `\xHH` 표기
  - golden 파일은 `Tests/Golden/`에 두고, 갱신은 명시 플래그로만 한다
- **양쪽 같음 golden(CL6)**
  - 같은 시험 명령 묶음(최상위 + 하위 명령 둘 + 공용 옵션 + enum + 필수 + 위치 인자 + `Remaining`)을 C++ 시험 fixture와 C# 시험 프로젝트에 각각 정의한다
  - 같은 입력에서 도움말·오류·`--version` 출력이 byte 단위로 같은지 본다. 프로그램 이름 자리는 시험이 같은 이름을 준다
  - 받는 값 집합이 같은지: 같은 수·bool·enum·Guid 표기 사례 표를 양쪽에 넣어 성공·실패가 같은지 본다
  - C# 시험은 `ko-KR` 문화권으로도 한 번 돌려 출력이 같은지 본다(현지화 리소스·수 형식이 새지 않는지)
  - System.CommandLine 판을 올리면 이 시험이 차이를 잡는다
- **C# 패키지 관리**
  - 오프라인(캐시만) restore에서 빌드가 감사 경고로 실패하지 않는지
  - `Directory.Packages.props`·`packages.lock.json`을 바꾸면 `SourceSnapshot` digest가 바뀌는지
  - 허용 목록 policy: 목록에 없는 PackageReference와 라이선스 기록 없는 패키지 거부, 게임 규칙 참조 어셈블리가 System.CommandLine을 참조하면 거부
- **크기**
  - 7.2절 표의 측정(Release Sandbox·Release MathBench 크기 비교, Game 대상 Release 링크 맵에 `CommandLineTools` 기호 없음)을 단계 완료 보고에 넣는다

## 18. 구현 단계

단계 기호는 AP(앱 정보)와 CL(명령줄)이다. 기존 문서의 기호(CV·CU·CF·CVR·TR·LG·RB 등)와 겹치지 않는다. 교차검증은 단계마다 리뷰어 1명이 한다(구현 → Debug·format·policy·바뀐 부분 시험 → 리뷰 → 반영). 스펙·문서 관점 리뷰는 묶음 끝 CL7에서 한 번 한다. 착수 시점은 머리말대로 사용자가 정한다.

| 단계 | 범위 | 선행 | 규모 |
|---|---|---|---|
| AP1 | Core `AppInfo`·`RunTraits`·`HostKind`·`AppInfoScope`. 개정 3: `ProcessCommandLine`(Windows `wmain`·`wWinMain`·`GetModuleFileNameW`), PlatformBase 대화상자 억제 API(`SuppressSystemErrorDialogs`), Sandbox·RocksCommandlet·FoundationTests 진입점 연결은 크래시 처리 설계 LC3·LC4가 Launch 진입점으로 먼저 했다. AP1에 남는 일: Launch `ApplicationKind`를 `HostKind`로 합치고, `ApplicationSettings::suppressSystemErrorDialogs`를 `RunTraits::unattended`에서 정한다. 파서가 아직 없으므로 `RunTraits`는 호스트 기본값(시험 실행기는 `unattended = true`, 나머지는 모두 기본값)으로 채우고, 지금 손 파싱하는 인자는 그대로 둔다 | — | M |
| CL1 | MetaCompiler: `Command(...)`·`Group`·PROPERTY 키(13.3절)·문자열 목록 값, Command 생성 방식(스키마·필드별 바인더·기본값 출력·범위 검사·`ValidateCommand` 호출), 명령줄 enum 이름 표, identity 면제, Command 키 사용 위치 검사, descriptor 왕복, 실행 파일 모듈 `Metadata` 경로, RMC 진단. Rocks.Build 대상 종류 정의 `ROCKS_TARGET_KIND_*` | — | M |
| CL2 | Core 런타임 `CommandLine` 모듈: `CommandLineSchema.h`, 토큰화·매칭·생성 바인더 호출·필수·범위·`ValidateCommand`·진단·이름 제안·알 수 없는 인자 정책·`Remaining`·로그용 가리기·짧은 사용법 줄, Core `EditDistance`, Base `Guid` 텍스트 변환, 크기 측정 | CL1, AP1 | M |
| AP2 | Rocks.Build `<exe>.BuildStamp.cpp`·`build::Description()`·`ROCKS_BUILD_BRANCH`, `ToolBuildId` 입력에서 제외, 시작 로그 묶음(D14 — 가린 명령줄은 CL2의 `RedactedArguments`) | AP1, CL2 | M |
| CL3 | Developer `CommandLineTools`: 출력 규약 렌더러(도움말·오류)·응답 파일·`CommandSet`(`AddGroup` 포함)·`--version`, PlatformBase `TerminalColumns`, C++ 출력 규약 golden | CL2, AP2 | M |
| CL4 | 이전: HostConfig에 리플렉션 헤더(표지)와 `HostCommandLineOptions`(`--set`·`--config`·`--trace`·`--project`·실행 성격), 부트 순서(8.3절 — AppInfo 3단계), `HostSettings::ApplyCommandLine`·`TraceController::ApplyCommandLine` 입력 변경. RocksCommandlet 명령 넷과 묶음 `log`, `CommandletRegistry`·`CommandletOutput` 삭제, Config의 `RegisterConfigDumpCommandlet`을 `Programs/Commandlet`으로 옮김(덤프 본체는 Config에 남김). 실행 파일 등록 변경: Sandbox·ShaderToolSpike·벤치 넷·FoundationTests의 Build.cs에 `ModuleDirectory`와 리플렉션 헤더 폴더의 include 디렉터리(identity 면제 — 헤더는 빌드가 표지로 찾는다, [리플렉션·빌드 보강](ReflectionFoundations.ko.md) 1.10절), FoundationTests `process-probe`는 `Remaining`. Sandbox 고정 delta 상수를 지우고 `--deterministic`으로, 자체 시험 자식에 `--deterministic`을 넘김 | CL3 | M |
| CL5 | C#: `Directory.Packages.props`·lock 파일·`NuGetAudit=false`·`Programs/ThirdPartyPackages.json`·허용 목록 policy·`BuildEvidence` 루트 목록, 새 어셈블리 `Rocks.Tooling.Cli`(`RocksCli` — 출력 규약 렌더러·파서 설정·사용자 파서), Rocks.Build.Cli·MetaCompiler(`ids` 포함)·ToolTests(`scaffold` 포함) 이전, 설치 SDK에 DLL 동봉 | — | M |
| CL6 | 양쪽 같음 golden 시험(17절) | CL3, CL5 | S |
| AP3 | `FrameTiming`(EngineTime 대체)·`LatestFrameIndex`, 렌더 packet 칸(RB1과 함께), worker 상속(CVar의 lane 묶음 장치에 `FrameTiming`을 넣는다), `Engine::IsForeground()` | AP1, RB1, CVR 묶음 장치 | M |
| AP4 | 실행 성격 소비처: 엔진 고정 step·시드, unattended 입력 장치 정책, `app.info` 콘솔 명령 | AP1, CL4 | S |
| CL7 | 문서 반영(설정 계층 5.4절 부트 순서, CVar 콘솔 영역 `app`과 12절 편집 거리 함수 이름, 로깅 시작 기록, 리플렉션 보강의 지정자 표와 identity 면제, [셰이더 시스템](ShaderSystem.ko.md) 96·147행과 [에셋 시스템](AssetSystem.ko.md) 37행의 `CommandletRegistry` → `CommandSet`, 구현 문서), 묶음 끝 리뷰 | 전부 | S |

## 19. 결정 목록

권장안으로 확정했다(2026-11-18, 개정 2). **[범위 큼]** 표시는 범위가 커서 착수 전에 사용자가 다시 볼 만한 항목이다.

- **D1 이름과 배치** — `AppInfo`·`RunTraits`·`HostKind`·`ProcessCommandLine`·`FrameTiming`은 Core 런타임 `Core`에 둔다. 빌드 정보는 Core `Build`에, 파서는 새 Core 런타임 모듈 `CommandLine`(의존은 Core만)에, 도움말·`CommandSet`·응답 파일은 새 Core Developer 모듈 `CommandLineTools`(`Core/Source/Developer/`)에 둔다. Engine에는 `FrameTiming` 갱신과 실행 성격 소비만 둔다.
- **D2 수명과 쓰기** — 호스트 부트가 한 번 채우는 불변 레코드다. setter가 없다. 실행 중 바뀌는 값(포커스·볼륨·그래픽 API 이름)은 두지 않는다.
- **D3 접근** — `AppInfoScope`로 프로세스에 설치하고 `AppInfo::Get()`으로 읽는다. 두 번 설치는 Fatal이다(중첩을 허용하는 `LogInstallScope`와 다르다). 설치는 부트 3단계(실행 성격을 정한 직후)다. 인자 전달 방식은 Core 깊은 곳(로그·trace·Fatal)까지 줄줄이 넘겨야 해서 택하지 않았다.
- **D4 프레임 시간 전달 [범위 큼]** — 게임 lane은 `Engine::Timing()`, 렌더 lane은 프레임 packet 사본, worker는 작업 문맥 상속, 그 밖은 `LatestFrameIndex()`와 단조 시계만 읽는다. worker 상속은 CVar 설계의 lane 묶음 장치(Task 모듈, 아직 없음)와 한 장치로 만든다. 프레임마다 게시하는 refcount 묶음에 `FrameTiming`을 넣고 작업에는 묶음 포인터만 넘긴다. 그래서 AP3은 RB1·CVR 작업에 묶인다.
- **D5 `FrameTiming` 칸** — UE `FAppTime` 칸 대응 + `frameIndex`·`averageFrameSeconds`. 고정 step은 `Optional<double>` 하나로 둔다. 단위는 double 초이고 기본 고정 길이는 1/30이다(UE와 같다). 고정 delta의 중간 float 반올림(`UEN` 3019, `LEL` 4730)은 따르지 않는다. timecode 칸은 provider가 생길 때 더한다. `EngineTime`을 대체한다.
- **D6 실행 성격 출처** — 명령줄 + 빌드·호스트 기본값만 쓴다. 설정 파일·CVar로는 정하지 않는다. 보기는 콘솔 `app.info`, 설정 기본값 조정은 등록 코드가 Code 층에 쓴다. BootCVar 방식은 검토하고 제외했다. 봉인 전에 필요하고 출시 빌드 세션 층 제한(Config D5)과 부딪히기 때문이다. 늘 고정 step이어야 하는 게임은 World `TimeStepPolicy`(설정)가 맡는다.
- **D7 실행 성격 목록** — `unattended`·`unattendedInput`·`benchmark`(비출시)·`fixedSeed`·`useFixedTimeStep`·`fixedDeltaSeconds`(기본 1/30)·`headless`. 옵션은 UE와 같은 뜻이다: `--use-fixed-time-step`(UE `-UseFixedTimeStep`), `--deterministic`(고정 step + 고정 시드), `--fixed-seed`, `--fps`(길이만 바꾸고 고정 step을 켜지 않는다, 비출시). 고정 step 계열은 출시 빌드에서도 받는다(UE `WITH_FIXED_TIME_STEP_SUPPORT` 기본 켜짐). 오디오·단일 스레드는 이름만 예약하고, 설치형 여부는 패키징 단계로 미룬다.
- **D8 benchmark 효과** — 고정 step(길이는 `fixedDeltaSeconds`, 기본 1/30), 고정 시드, 포커스 늘 true, 소리 끔(오디오 모듈이 생기면)이다. 출시 빌드에서는 옵션이 없다(UE와 같다).
- **D9 인스턴스·세션** — `instanceId`는 매 실행 무작위 Guid다. `--instance-id`·`--session-id`·`--session-name`은 비출시에서만 받는다(UE `InitializeSession`이 비출시만). `sessionId`가 없으면 단독 실행이다. 세션 소유자·사용자 허가 목록은 두지 않는다.
- **D10 빌드 정보 칸** — 구성·대상 종류·대상 이름·프로젝트 이름·브랜치·커밋·dirty·커밋 시각·빌드 URL·디버그 정보. Horde 실행 job URL·승격 빌드·제품 식별자는 두지 않는다. branch는 `ROCKS_BUILD_BRANCH`로 덮어쓸 수 있다(CI detached HEAD).
- **D11 빌드 날짜** — 빌드 시각(`__DATE__`) 대신 커밋 시각을 쓴다. 재현 빌드를 위해서다. UE와 뜻이 다르다(UE는 컴파일 날짜). 시간 단위·좌표 규칙과 무관한 진단 문자열이라 UE와 다르게 둔다.
- **D12 BuildStamp와 캐시 키** — 별도 생성 TU에 둔다. `ToolBuildId` 입력·쿠킹 byte·에셋 너겟 키에는 넣지 않는다. 커밋이 바뀌면 모든 실행 파일이 다시 링크되는 비용은 AP2에서 측정한다.
- **D13 그래픽 API 이름** — AppInfo에 두지 않는다. Graphics 장치 정보가 공개하고 시작 로그가 남긴다.
- **D14 시작 로그** — LogHost 설치 직후 `App` 카테고리 Info 기록 한 묶음을 남긴다. 내용은 실행 파일, 엔진 버전, 구성, 대상, 커밋, 브랜치, 플랫폼, PID, instance id, 가린 명령줄이다. 문구는 영어다.
- **D15 프로세스 명령줄** — 진입점에서 한 번 캡처하는 불변 UTF-8 배열이다. Windows는 Launch가 만드는 `wmain`·`wWinMain`(CRT `__wargv`)만 쓰고 `main(char**)`는 지원하지 않는다(개정 3, 크래시 처리 설계 D7. `GetCommandLineW` 직접 분해는 하지 않는다). 실행 파일 경로는 OS에서 얻는다(`GetModuleFileNameW`). `activeCodePage` UTF-8 manifest는 보조 수단으로만 검토한다. UE식 `Set`·`Append`가 없다. 하위 프로세스 상속(`Inherit`)은 자식을 띄우는 도구가 생길 때 더한다. 환경 변수 인자(UE `UE-CmdLineArgs`)는 두지 않는다.
- **D16 비밀 가리기** — `Secret` 지정자 옵션의 값을 `<redacted>`로, 파싱하지 못하거나 무시한 인자를 `<unparsed>`로 바꾼다. 로그·trace·크래시 보고에는 가린 형태만 쓴다.
- **D17 C++ 파서 — 자체 구현(확정)** — 외부 라이브러리는 검토하고 제외했다. 이유는 크기다(사용자 결정 2026-11-18).
- **D18 선언 방식** — `ROCKS_STRUCT(Command(...))`·`ROCKS_PROPERTY` 지정자 문법으로 선언하지만, Command struct는 리플렉션 등록 타입이 아니다. 생성물은 명령줄 스키마와 바인더뿐이다. 도움말 문자열은 `Help=` 지정자에서 온다. 문서 주석에서 가져오지 않는다(주석은 한국어, 도움말은 영어). 필수 표시는 기존 `Required`와 겹치지 않게 `Mandatory`로 한다. 출시 빌드 제외는 설정 지정자 `DevelopmentOnly`가 아니라 Command 전용 `NotInRelease`로 한다. 판정은 런타임 규칙(`CommandLineRules::release`)이라 Debug에서 시험한다. Command 키는 Command struct 밖에서 쓰면 RMC 오류다. 묶음 설명은 `Group` struct로 선언한다.
- **D19 값 변환** — MetaCompiler가 Command struct마다 필드별 바인더를 생성한다. 변환은 Core `NumberText`·Base `Guid` 텍스트 변환·생성 enum 표만 쓰고, ReflectionText·TypeRegistry·ObjectRegistry에 기대지 않는다. enum은 도구 이름 정확 일치(대소문자 구분), bool은 `true`·`false`만, 반복 필드는 원소마다 추가한다. 검사 순서는 토큰·변환 → 필수 → 범위 → `ValidateCommand`이고, 앞 단계에 오류가 있으면 `ValidateCommand`를 부르지 않는다. `ValidateCommand`는 `CommandValidation::Fail`로 영어 이유 문자열을 남긴다.
- **D20 공통 문법 규약** — `--kebab-case`, 한 글자 별칭, `--name value`·`--name=value`(`:` 구분자 없음), 값 없는 bool(다음 토큰을 먹지 않음), 반복 = 원소 추가(쉼표로 나누지 않음), 스칼라 중복 오류, `--`, 묶음·`/x`·지시자 없음, 대소문자 구분, 공용 옵션은 어디서나 받는다.
- **D21 도움말 출력 규약** — "Rocks 명령줄 출력 규약"을 이 문서가 정의한다(14.4·14.5절): UTF-8(BOM 없음), 줄 끝 `\n`, 영어, 너비(터미널 열 수 또는 80, [40, 200])와 열 규칙, Description·Usage·Arguments·Options·Commands·Examples 절, `[default: …]`, `(REQUIRED)`. C++과 C#이 각자 이 규약대로 렌더한다. System.CommandLine 기본 도움말은 쓰지 않는다. Game 대상 Release는 도움말 문자열이 없다. stdout, 종료 0.
- **D22 오류 출력과 종료 코드** — 오류 문구는 14.4절 표 하나에서만 만든다(System.CommandLine 문구를 쓰지 않는다). stderr에 `error: ` 접두로 쓰고, 짧은 사용법 한 줄과 `--help` 안내 한 줄을 붙인다. 종료 코드는 0 성공, 1 실행 실패, 2 사용법 오류다. 이름 제안은 Core `EditDistance`(CVar 12절과 같은 함수)를 쓰고, 명령줄 문턱은 이름 붙은 상수 `MinCommandLineSuggestionDistance`(1)·`CommandLineSuggestionLengthDivisor`(3)로 정한다. ShaderToolSpike의 64와 벤치의 1·stdout을 이 규약으로 고친다.
- **D23 응답 파일** — 도구·commandlet만 `@file`을 받는다. 문법은 Rocks가 정한다(14.7절 — escape는 `""`뿐이고 `\`는 글자 그대로). C#도 System.CommandLine 처리를 끄고 같은 문법으로 펼친다. 크기·깊이 상한을 둔다. 게임 호스트는 받지 않는다.
- **D24 commandlet 구조** — `CommandletRegistry`를 Developer `CommandSet`으로 대체한다(하위호환 없음). 명령은 옵션 struct + 실행 함수다. 도움말 목록과 개별 도움말은 등록만으로 생긴다. `--list`는 없앤다. Config의 `RegisterConfigDumpCommandlet`은 `Programs/Commandlet`으로 옮긴다. ShaderSystem·AssetSystem 문서는 CL7에서 고친다.
- **D25 `--set`·`--trace`·`--config`** — 호스트 공용 옵션 struct로 옮겨 같은 파서로 한 번만 읽는다. 값 의미(Config 대입, trace 목록)와 출시 빌드 제한은 그대로다. commandlet에서도 받는다.
- **D26 C# 파서 [범위 큼]** — System.CommandLine을 파싱에만 쓴다(저장소 첫 외부 NuGet 패키지). 출력은 새 어셈블리 `Rocks.Tooling.Cli`의 `RocksCli`가 그린다. 파서 설정은 16.2절 표(InvariantCulture, 위성 리소스 en, 묶음·`:`·bool 값 소비·지시자·자체 응답 파일 끔, 사용자 값 파서)다. 중앙 버전 관리·lock 파일·`NuGetAudit=false`(감사는 CI 별도 작업)·`BuildEvidence` 루트 목록 추가·허용 목록 policy·`Programs/ThirdPartyPackages.json` 라이선스 기록을 쓴다. 설치 SDK에는 두 DLL을 싣고, 게임 규칙이 참조하는 어셈블리는 패키지에 의존하지 않는다. 자체 C# 파서는 택하지 않았다.
- **D27 코드 크기** — 7.2절 상한을 둔다. 도움말 문자열과 도움말 writer를 빼는 기준은 Game 대상의 Release다(`ROCKS_TARGET_KIND_GAME`). Program 대상은 Release에서도 도움말을 싣는다.
- **D28 단계 기호** — AP(앱 정보)·CL(명령줄).
- **D29 알 수 없는 인자 [새 결정]** — 도구·commandlet·Debug·Development 게임 호스트는 오류(종료 2)다. Game 대상 Release 호스트는 경고로 남기고 무시한다. 경고에는 옵션 이름만 쓰고 값과 위치 토큰은 `<unparsed>`로 가린다. 명령은 `Remaining` 필드(`Array<String>`, 하나)로 남은 인자를 원문 순서대로 받을 수 있다.
- **D30 Command struct identity [새 결정]** — Command struct와 명령줄 enum은 직렬화하지 않으므로 identity 면제다. stable ID가 없고, `rocksids` 항목과 `ids add`가 필요 없다. 모듈 안 판정과 descriptor 왕복은 정규 이름에서 만든 키를 쓴다. Command만 있는 모듈은 `rocksids` 파일 없이 `Metadata`만 선언한다. 그래서 리플렉션을 쓰지 않던 낮은 계층 도구도 ObjectRuntime 의존 없이 옮길 수 있다.

## 20. 확인하지 못한 것

- **System.CommandLine 판과 API.** 3.0.0(2026-11-10)·2.0.15라는 판 번호와 날짜, 3.0의 API는 확인하지 않았다. C# 예시는 2.0 API(`Option<T>{Description}`·`SetAction`·`GetValue`)를 따랐다. 착수 때 고정할 판의 API로 고친다.
- **System.CommandLine 설정이 16.2절 표를 모두 지원하는지.** `:` 구분자 끄기, bool arity 0, 응답 파일 끄기, 지시자 빼기, `TreatUnmatchedTokensAsErrors`가 채택 판에서 설정으로 되는지 확인하지 않았다. 안 되는 항목은 `RocksCli`가 토큰을 넘기기 전에 거부하거나 고쳐 넣는다(16.2절). 현지화 리소스(ko 포함)를 패키지가 싣는다는 것도 원본에서 확인하지 않았다. `SatelliteResourceLanguages`와 InvariantCulture 설정은 어느 경우든 둔다.
- **Rocks.Build가 같은 구성에서 모듈 object를 Game·Program 대상 사이에 공유하는지.** 공유하면 `ROCKS_TARGET_KIND_*` 정의를 모듈 전체에 줄 수 없다. 그때는 생성 스키마 TU만 대상별로 컴파일한다(7.2절). CL1 착수 때 확인한다.
- **Core `TryParseDouble`이 `nan`·`inf` 표기를 받는지.** 받든 받지 않든 바인더가 유한값만 통과시킨다. C# 사용자 파서를 같은 문법으로 맞추려고 CL2에서 확인한다.
- **생성 바인더의 크기.** Command struct 하나에 2 KiB 안팎으로 추정했다. CL2에서 측정해 7.2절 상한을 고친다.
- **Windows 직접 분해 규칙의 세부.** MSVC CRT(2008 이후) argv 규칙(역슬래시와 따옴표 짝, 따옴표 안 `""`)을 그대로 옮긴다. 실제 CRT 동작과 같은지 AP1의 표 시험과 `Process::Start` 왕복 시험으로 확인한다.
- **`activeCodePage` UTF-8 manifest의 영향.** 남은 ANSI API(CRT 출력 등)에 주는 영향과 지원 OS 하한을 확인하지 않았다. 보조 수단으로만 검토한다.
- **비 ASCII 인자 깨짐의 실측.** 코드상 ANSI argv를 UTF-8로 취급하므로 깨진다고 판단했다. 실행해 보지는 않았다. AP1 시험이 확인한다.
- **엔진 호스트가 worker 없이(`InlineExecutor`만으로) 돌 수 있는지.** `--single-thread`를 열지 정하는 근거다.
- **Android 호스트가 명령줄 대신 받는 인자 경로.**
- **UE 온라인 서버 판정 delegate**(`SetIsServerForOnlineSubsystemsDelegate`)를 엔진 어디서 설정하는지. `Runtime/Engine/Private`에서 찾지 못했다(플러그인으로 보인다). 범위 밖이라 더 찾지 않았다.
- **UE `FApp` 조회 함수의 읽는 스레드 분포.** 대표 호출(3절 표의 파일:줄)과 Runtime 아래 호출 파일 수만 셌고, 호출마다 스레드를 추적하지는 않았다.

## 21. 변경 이력

- **개정 1 초안(2026-11-18)**: UE 5.8 `FApp`·`FAppTime`·`FCommandLine`·`FParse`·`UCommandlet` 조사, Rocks 앱 정보·명령줄 현황(C++·C#), `AppInfo`·`FrameTiming`·`ProcessCommandLine`·빌드 정보 주입, 리플렉션 기반 C++ 명령줄 파서와 System.CommandLine 기반 C# 도구의 공통 규약, 결정 D1–D28, 단계 AP1–AP4·CL1–CL7.
- **개정 2(2026-11-18)**: 교차검증 1차를 반영하고 결정 D1–D30을 확정했다.
  - 바인딩(D19): ReflectionText 대입 방식은 쓸 수 없었다. 원소 추가 표기가 없고, 같은 경로 두 번이 오류이며, `\`로 끝나는 원소가 깨지고, Validate 이유를 전할 수 없고, enum이 접두 일치였다. 그래서 MetaCompiler 생성 필드별 바인더(Core `NumberText`·생성 enum 표)로 바꿨다. 검사 순서와 `ValidateCommand` 이유 통로를 정했다.
  - 출력(D21·D22): "Rocks 명령줄 출력 규약"을 직접 정의했다(14.4·14.5절). System.CommandLine은 파싱에만 쓰고 출력은 `RocksCli`가 그린다. 14.4절(자체 문구)과 16.2절(System.CommandLine 문구)의 모순을 없앴다. 현지화·묶음·`:` 구분자·bool 값 소비 차단 설정을 적었고, `RocksCli` 크기를 900줄로 다시 추정했다.
  - 지정자(D18): `DevelopmentOnly`(설정 전용, `Cheat`로 바뀔 예정)를 쓰지 않고 `NotInRelease`·`Remaining`·`Group`을 더했다. Command 키의 사용 위치를 검사한다.
  - identity(D30): Command struct는 identity 면제로 했다. 낮은 계층 도구도 ObjectRuntime 없이 옮긴다. 15절 비교에 실제 비용을 넣었다.
  - 알 수 없는 인자(D29): 도구는 오류, Game 대상 Release 호스트는 경고와 `<unparsed>` 가림으로 했고 `Remaining` 필드를 두었다.
  - 시간·실행 성격(D5·D7·D8·D9): UE와 같게 고쳤다. `--deterministic` 기본 1/30, `--use-fixed-time-step` 추가, `--fps`는 길이만, 고정 step 계열은 출시 빌드에서도 받고, 세션 인자는 비출시만 받는다. 9.1절의 잘못된 절 참조를 고쳤다.
  - 부트 순서: AppInfo 설치를 3단계(실행 성격 직후)로 옮겨 설정 등록 코드가 읽을 수 있게 했다.
  - 크기(D27): 도움말을 빼는 기준을 Game 대상 Release(`ROCKS_TARGET_KIND_GAME`)로 바꿨다.
  - commandlet(D24): config-dump 등록 이전과 ShaderSystem·AssetSystem 문서 반영을 단계에 넣었다.
  - C# 패키지(D26): NuGet 감사·빌드 증거·허용 목록 policy·라이선스 기록·별도 어셈블리 `Rocks.Tooling.Cli`·SDK 동봉 조건을 적었다.
  - 이름 제안: Core `EditDistance`를 CVar와 함께 쓰고, 문턱은 이름 붙은 상수로 했다.
  - 단계 표: AP2 선행에 CL2를 넣었고, AP1이 `RunTraits`를 채우는 방법, CL4의 HostConfig·실행 파일 등록 변경, Sandbox 자식의 `--deterministic`을 적었다.
  - 그 밖: Windows 인코딩(비 ASCII 인자가 깨진다, best-fit 보안 문제, `wmain`·직접 분해·`GetModuleFileNameW`), 대화상자 억제 공용 API, `TryFillSecureRandom` 이름, `AppInfoScope`와 `LogInstallScope`의 차이, 스키마 칸 형식, 묶음 설명, 도움말 예의 `--config`, CI branch, UE `UE-CmdLineArgs`, Developer 판정 문구, 명령줄 enum의 kebab-case 이름을 반영했다.
- **개정 3(2026-11-19)**: [크래시 처리·실행 진입점](CrashHandlingAndLaunch.ko.md) LC3·LC4 구현을 반영했다.
  - 12절: `ProcessCommandLine`은 구현됐다(`Core/App/ProcessCommandLine.h`). 상한을 OS 한계에 맞춰 인자 32768개·텍스트 2 MiB·실행 파일 경로 96 KiB로 바꾸고 `IsCaptured`를 더했다. 캡처는 Launch 진입점이 한다.
  - 12절·D15: Windows `main(char**)`·`WinMain`의 `GetCommandLineW` 직접 분해를 지웠다(그 설계 D7). Launch 진입점은 늘 `wmain`·`wWinMain`(CRT `__wargv`)이다. 17절의 직접 분해 표 시험도 뺐다.
  - 7.1절 PlatformBase 행, 8.3절 0단계(Launch가 캡처, 실패 종료 코드는 `commandLineFailureExitCode`)를 맞췄다.
  - 18절 AP1: 크래시 처리 설계가 먼저 구현한 부분을 적고, 남은 일(`ApplicationKind` → `HostKind`, `suppressSystemErrorDialogs` ← `RunTraits::unattended`)을 적었다. 크래시 메타데이터에 가린 명령줄(CL2 뒤)·`--crash-*` 호스트 공용 옵션(CL4 뒤)·`BuildDescription`(AP2 뒤)을 더하는 일은 그 설계 21절에 있다.
- **개정 4(2026-11-22)**: [오디오 시스템](AudioSystem.ko.md) 요구로 오디오 실행 성격을 열었다. `RunTraits::audioOutput`(부트가 Server 대상·commandlet·benchmark·`--no-sound`로 정함)·`audioMuted`(`--mute-audio`), 비출시 `--allow-commandlet-audio`. UE `-enablesound`는 두지 않는다. 볼륨 배수는 오디오 CVar로 넘겼다(8.6절).
