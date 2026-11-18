# 앱 정보와 명령줄 설계

기준일: 2026-11-18, 개정 1 초안. 사용자 지시("설계문서만 작성해놔")로 쓴 첫 초안이고 구현하지 않는다. 결정은 권장안으로 확정하는 것을 전제로 썼다(19절). 원칙 충돌이나 범위가 큰 항목은 19절에 따로 표시했다.

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
- **빌드 정보는 Rocks.Build가 생성 TU로 주입한다.** 이미 있는 `<exe>.BuildInfo.cpp` 옆에 커밋·브랜치·구성·대상 종류를 담은 별도 생성 TU를 만든다. 날짜는 빌드 시각이 아니라 커밋 시각을 쓴다(재현성 — 11절).
- **프로세스 명령줄 보관.** `ProcessCommandLine`이 진입점에서 한 번 UTF-8로 캡처하고 불변으로 둔다. Windows는 넓은 문자 원문에서 바꾼다. 로그에는 `Secret` 옵션 값을 가린 형태만 남긴다(12절).
- **C++ 명령줄 파서는 자체 구현이다(확정).** 외부 라이브러리는 검토하고 제외했다. 이유는 크기다(사용자 결정 2026-11-18).
  - 중심 설계는 리플렉션이다. 옵션 struct 하나를 `ROCKS_STRUCT(Command(...))`·`ROCKS_PROPERTY(Help=..., Alias=...)`로 선언하면 MetaCompiler가 명령줄 스키마를 만든다. 런타임은 그 스키마 하나로 파싱·검증·도움말을 모두 만든다.
  - 값 변환은 기존 ReflectionText 대입 경로를 그대로 쓴다. 옵션 하나가 옵션 struct에 대한 대입 하나다.
  - 실행 함수는 결과 struct를 `const&`로 받는다. 옵션마다 `GetValue`를 부르는 C#보다 꺼내 쓰기가 짧다(15절).
- **C# 도구는 System.CommandLine을 쓴다.** 외부 패키지(MIT)를 처음 들여온다. Rocks.Tooling.Common에 얇은 공용 층 `RocksCli`를 두어 오류 문구·종료 코드·예시 절·`help <명령>`·이름 제안을 C++과 같게 맞춘다.
- **양쪽 사용 경험이 같다.** 도움말 모양(System.CommandLine 기본 모양 + Examples 절), 오류 문구, 종료 코드(0·1·2), 옵션 철자(`--kebab-case`), 응답 파일(`@file`), `help <명령>`이 같다. 같은 시험 명령을 양쪽으로 정의해 도움말·오류 출력이 byte 단위로 같은지 golden 시험으로 확인한다(17절).

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
- 오디오 볼륨 배수와 VR 포커스. 오디오 모듈·XR 설계 때 정한다(8.6절).

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
| `GetInstanceId` (`APP` 267), `InstanceId` (`APP` 848) | 실행 인스턴스 GUID | 정적 초기화에서 `FGuid::NewGuid()`(`APPC` 31). 비출시 빌드는 `InitializeSession`이 `-InstanceId=`로 덮어쓴다(`APPC` 182–188) | 아무 스레드(Runtime 18개 파일) | 없음 | `AppInfo::instanceId`(Guid). 매 실행 `platform::SecureRandom`으로 만들고, 비출시 빌드만 `--instance-id`로 덮어쓴다 |
| `GetInstanceName` (`APP` 276) | `컴퓨터이름-PID` | 매번 계산 | 아무 스레드 | 없음 | 두지 않는다(시작 로그에 PID를 남긴다) |
| `IsThisInstance` (`APP` 366) | ID 비교 | — | — | 없음 | 두지 않는다(Guid 비교로 충분) |
| `GetSessionId` (`APP` 292), `SessionId` (`APP` 851) | 같이 실행한 묶음의 ID | 정적 초기화(`APPC` 32), `-SessionId=`(`APPC` 191–201) | 아무 스레드 | 없음 | `AppInfo::sessionId`(`Optional<Guid>`, `--session-id`로만) |
| `GetSessionObjectId` (`APP` 303, `APPC` 490–494) | 세션 ID의 Compact Binary 형태 | 첫 호출 | 아무 스레드 | 없음 | 두지 않는다 |
| `GetSessionName`/`SetSessionName` (`APP` 310·377), `SessionName` (`APP` 854) | 세션 이름 | `-SessionName=`(`APPC` 203), Insights 시험(`Developer/TraceInsightsFrontend/Private/InsightsFrontend/Common/InsightsAutomationController.cpp:64`) | 아무 스레드 | 없음 | `AppInfo::sessionName`(`--session-name`). setter 없음 |
| `GetSessionOwner`/`SetSessionOwner` (`APP` 325·388), `SessionOwner` (`APP` 857) | 세션을 띄운 사용자 | `-SessionOwner=` 또는 OS 사용자 이름(`APPC` 205–208), 세션 서비스(`Runtime/SessionServices/Private/SessionServicesModule.cpp:72`) | 아무 스레드 | 없음 | 두지 않는다(원격 세션 허가 없음) |
| `InitializeSession` (`APP` 333, `APPC` 177–209) | 명령줄의 세션 인자 읽기 | `AppInit`, 비출시만(`LEL` 6880–6886) | 게임 스레드 | 없음 | 호스트 부트가 공용 옵션 struct에서 채운다(8.3절) |
| `IsStandalone` (`APP` 355), `Standalone` (`APP` 866) | 세션 ID가 명령줄에 없음 | `InitializeSession` | 아무 스레드 | 없음 | `!sessionId.HasValue()` |
| `AuthorizeUser`·`DenyUser`·`DenyAllUsers`·`IsAuthorizedUser` (`APP` 214–342), `SessionUsers` (`APP` 863) | 원격 조작 허가 목록 | 엔진 서비스 메시지(`Runtime/Engine/Private/EngineService.cpp:127,142`) | 메시지 처리 스레드 | 없음 | 범위 밖 |
| `GetZenStoreProjectId`·`GetZenStoreProjectIdForProject` (`APP` 246·255, `APPC` 437–481) | Zen 저장소 프로젝트 ID | 계산 | 아무 스레드 | 없음 | 범위 밖 |

### 3.5 실행 성격

| 멤버 | 의미 | 쓰는 곳·시점 | 읽는 스레드 | Rocks 현황 | Rocks 처리 |
|---|---|---|---|---|---|
| `IsGame` (`APP` 176) | 게임처럼 동작 중(에디터의 PIE 포함) | `GIsEditor`·`GIsPlayInEditorWorld`. PIE 전환 때 게임 스레드가 바꾼다. 주석이 "worker에서 믿을 수 없다"고 적는다(`APP` 169) | 게임 스레드 | 없음 | AppInfo에 두지 않는다. 에디터·PIE 구분은 World 종류가 맡는다 |
| `CanEverRender` (`APP` 400) | 렌더링할 수 있는 프로세스 | `UE_SERVER`, commandlet, `-nullrhi`를 처음 한 번 읽는다 | 아무 스레드(Runtime 91개 파일) | 없음. 헤드리스 실행은 Sandbox `--engine`의 고정 delta(`Programs/Sandbox/Private/EngineHost.cpp:27,42`)뿐이고 Null graphics backend가 없다(`Engine/Source/Runtime/Graphics/Public/Graphics/DeviceTypes.h:19–25`) | `RunTraits::headless`(10절) |
| `CanEverRenderOrProduceRenderData` (`APP` 419) | 렌더하거나 렌더 데이터를 만든다(쿠커) | 계산 | 아무 스레드 | 없음 | 두지 않는다. 쿠커는 Program 대상이고 렌더 데이터 생성 여부는 쿠커가 안다 |
| `CanEverRenderAudio` (`APP` 431), `IsAudioMuted` (`APP` 453) | 소리 장치를 쓸지 / 마지막 출력만 끌지 | `-nosound`·`-enablesound`·`-muteaudio`, benchmark·서버·commandlet | 오디오 스레드·게임 스레드 | 오디오 모듈 없음 | 이름만 예약한다(`--no-sound`·`--mute-audio`). 오디오 모듈 설계 때 연다 |
| `IsInstalled` (`APP` 473, `APPC` 212–242), `IsEngineInstalled` (`APP` 485, `APPC` 245–272) | 설치형 실행(설치 폴더에 쓰지 않음) | 출시 데스크톱 기본 + 표지 파일 + `-Installed`·`-NotInstalled` | 아무 스레드 | Config가 "설치 경로 규칙은 패키징 단계에서 정한다"고 적었다([설정 계층](ConfigSystem.ko.md) 5.4절) | 패키징 단계로 미룬다 |
| `IsUnattended` (`APP` 495, `APPC` 274–280) | 사람이 지켜보지 않음(대화상자 금지) | `-UNATTENDED`를 처음 한 번 읽고, `GIsAutomationTesting`과 OR | 아무 스레드(48개 파일) | 없음. PlatformBase에 대화상자 정책 API도 없다 | `RunTraits::unattended` |
| `AllowUnattendedInput` (`APP` 505) | 무인 실행이어도 입력 모듈을 켠다 | `-UnattendedInput` | 입력 초기화 | 없음 | `RunTraits::unattendedInput` |
| `ShouldUseThreadingForPerformance` (`APP` 515, `APPC` 290–302) | 렌더·태스크 성능 스레드를 쓸지 | `-onethread`·`-noperfthreads`·`-useperfthreads`, 코어 수, 서버 | 아무 스레드(47개 파일) | Task 모듈에 `InlineExecutor`가 있다(`Core/Source/Runtime/Task/Private/InlineExecutor.cpp`). 호스트 수준 선택은 없다 | 이름만 예약한다(`--single-thread`). 엔진 호스트가 모두 inline으로 돌 수 있는지 확인한 뒤 연다(20절) |
| `IsMultithreadServer` (`APP` 520) | 다중 스레드 전용 서버 | `UE_DEFAULT_MULTITHREAD_SERVER` | 아무 스레드 | 서버 대상 없음 | 두지 않는다 |
| `IsBenchmarking`/`SetBenchmarking` (`APP` 527·537), `bIsBenchmarking` (`APP` 869) | 벤치마크 모드(고정 step·고정 시드·포커스 무시·소리 끔) | PreInit `-BENCHMARK`(비출시), 출시는 false(`LEL` 2449–2454) | 아무 스레드: 게임 스레드 시간 갱신(`UEN` 3005), 렌더 통계(`Runtime/Engine/Private/UnrealClient.cpp:364`), 포커스(`APPC` 347) | 없음 | `RunTraits::benchmark`(비출시만) |
| `bUseFixedSeed` (`APP` 835) | 무작위 시드를 상수로 | PreInit `-Deterministic`·`-FixedSeed`·benchmark(`LEL` 2456–2463), 시드 초기화(`LEL` 2465–2480) | 아무 스레드(파티클·이동·네트워크·스레드 생성 `Runtime/Core/Private/Windows/WindowsRunnableThread.cpp:66`) | 시험 도구의 `--seed`만 있다(FoundationTests, ToolTests) | `RunTraits::fixedSeed` |
| `GetGraphicsRHI`/`SetGraphicsRHI` (`APP` 104·109), `GraphicsRHI` (`APP` 860) | 실제로 쓰는 그래픽 API 이름 | RHI 초기화(`Runtime/RHI/Private/Windows/WindowsDynamicRHI.cpp:1366` 등 플랫폼별 4곳) | 자동화 보고·HLOD(`Runtime/AutomationWorker/Private/AutomationWorkerModule.cpp:332`) | Graphics `Backend`와 `BackendName`(`DeviceTypes.h:19–28`), 설정 `render.backend`(CVar 예시) | AppInfo에 두지 않는다. Graphics 장치 정보가 공개하고 시작 로그에 남긴다(D13) |

### 3.6 시간

UE 5.8은 시간 값을 `FAppTime`(`AT` 22)이라는 값 타입 하나에 모으고, 게임 스레드가 프레임마다 통째로 게시한다.

| 칸(`AT` 66–95) | 형식 | 의미 | 쓰는 곳 |
|---|---|---|---|
| `bUseFixedTimeStep` | bool(`WITH_FIXED_TIME_STEP_SUPPORT`일 때만 칸) | 고정 step 사용 | PreInit `-UseFixedTimeStep`·`-Deterministic`(`LEL` 2460), DumpGPU(`Runtime/RenderCore/Private/DumpGPU.cpp:2284`), 영상 캡처(`Runtime/MovieSceneCapture/Private/MovieSceneCapture.cpp:835`) |
| `FixedDeltaTime` | double 초, 기본 1/30 | 고정 step 길이 | `InitTime`의 `-FPS=`(`LEL` 4698–4738, 4730행) |
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
- 리플렉션으로 프로퍼티에 대입하는 발상은 UE `ApplyCommandLineSwitches`와 같다. Rocks는 이것을 에디터 전용 보조가 아니라 기본 경로로 삼는다.

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
| FoundationTests | `--process-probe <mode>` | 값 다섯 개(`strcmp`) | 없음 | `ParseUnsigned` | 없음 | stderr, 종료 2 | 없음 | `Programs/FoundationTests/Private/Main.cpp:115–385` |

엔진 런타임에서 명령줄을 읽는 곳은 셋이다. Config `AddCommandLineAssignments`(`Core/Source/Runtime/Config/Private/ConfigLoader.cpp:222–252`), HostSettings `ApplyCommandLine`(`Core/Source/Runtime/HostConfig/Private/HostSettings.cpp:216–241` — `--set=`를 **한 번 더 훑는다**), TraceController `ApplyCommandLine`(`Core/Source/Runtime/TraceControl/Private/TraceController.cpp:248–272`)이다. Commandlet 호스트는 `ApplyCommandLine`을 부르지 않아 `--set`·`--trace`를 쓸 수 없다(`CommandletHost.cpp:166–175`).

**프로세스 명령줄 보관이 없다.** PlatformBase에 `GetCommandLine` 류가 없다. 진입점마다 처리가 다르다.
- RocksCommandlet은 `wmain`에서 UTF-16을 UTF-8로 바꾼다.
- Sandbox·ShaderToolSpike·벤치는 `main(int, char**)`이다. Windows에서 이 argv는 시스템 ANSI 코드 페이지 문자열이다. Rocks.Build는 컴파일 인자 `/utf-8`만 주고(`ActionGraph.cs:429`) 실행 파일 manifest의 UTF-8 코드 페이지 설정은 찾지 못했다. 그래서 한글 경로 인자가 깨질 수 있다(20절).

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

옵션 struct 선언에 쓸 기반을 확인했다. 지정자 파서는 `Programs/Rocks.MetaCompiler/Specifiers.cs`이고 표에 없는 키는 빌드 오류다.

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
| `EBuildTargetType` | `build::TargetType` | `Game`·`Program`(빌드 규칙의 `TargetType`과 같은 이름·같은 값) |
| `FAppTime` | `FrameTiming` | 한 프레임의 시간 값(double 초). 지금의 `EngineTime`을 대체한다 |
| `FAppTime::GetApproximateCurrentTime` | `LatestFrameIndex()` + `platform::MonotonicNanoseconds()` | 아무 스레드가 읽는 대략값 |
| `FCommandLine` | `ProcessCommandLine` | 진입점에서 캡처한 불변 UTF-8 인자 배열 |
| `FParse`·`UCommandlet::ParseCommandLine` | `ParseCommandLine<T>()` | 스키마 기반 파서(옵션 struct로 결과를 낸다) |
| `UCommandlet`의 `Help*` 칸 | `CommandLineSchema` | MetaCompiler가 만드는 명령줄 스키마(이름·별칭·도움말·예시) |
| `UCommandlet`·commandlet registry | `CommandSet` | 명령 묶음(하위 명령·공용 옵션·`help`·`--version`) |
| `-CmdLineFile=` | 응답 파일 `@file` | 도구 전용 |
| `FILTER_COMMANDLINE_LOGGING` | `Secret` 지정자 | 로그에서 값을 가릴 옵션 |

## 7. 모듈 배치와 코드 크기

### 7.1 배치

Engine/Source/Runtime에는 핵심만 둔다. 게임 실행 파일이 쓰는 최소 부분과 도구·commandlet 전용 부분을 나눈다.

| 위치 | 담는 것 | 누가 링크하나 |
|---|---|---|
| Core 런타임 `Core`: `Core/App/AppInfo.h`, `Core/App/ProcessCommandLine.h`, `Core/Time/FrameTiming.h` | `AppInfo`·`RunTraits`·`HostKind`·`AppInfoScope`, `ProcessCommandLine`, `FrameTiming`·`LatestFrameIndex` | 모든 실행 파일 |
| Core 런타임 `Core`: `Core/Build/BuildInfo.h`(기존 헤더 확장) | `build::Description()` 선언. 정의는 Rocks.Build 생성 TU | Rocks.Build로 링크한 모든 실행 파일 |
| Core `PlatformBase` | Windows 넓은 문자 명령줄 캡처, 터미널 너비 조회 | 모든 실행 파일 |
| Core `MetaSchema`: `Meta/CommandLineSchema.h` | 생성 스키마 형식(`SettingsSchema`와 같은 자리) | 옵션 struct를 선언한 모듈 |
| 새 Core 런타임 모듈 `CommandLine` | 토큰화·옵션 매칭·ReflectionText 대입 바인딩·필수 검사·진단·이름 제안·비밀 가리기 | 게임 호스트, Sandbox, 도구 |
| `HostConfig`(기존) | 호스트 공용 옵션 struct `HostCommandLineOptions`(`--set`·`--config`·`--trace`·`--project`·실행 성격)와 부트 도우미 | 게임 호스트, Sandbox, RocksCommandlet |
| 새 Core Developer 모듈 `CommandLineTools`(`type: Developer` — Release 빌드에서 빠진다) | 도움말 writer(너비·열·예시)·응답 파일·`CommandSet`·`--version` 출력 | 도구·commandlet, 개발 빌드 호스트 |
| Engine 런타임 `Engine` | `FrameTiming` 갱신과 게시, `RunTraits` 소비(고정 step) | 엔진 호스트 |
| `Programs/Rocks.Build` | 생성 TU `<exe>.BuildStamp.cpp` | — |
| `Programs/Rocks.Tooling.Common` | `RocksCli`(System.CommandLine 위의 얇은 공용 층) | C# 도구 |

지금 Core 런타임에 있는 `CommandletRegistry`(`Core/Source/Runtime/Core/Public/Core/Commandlet/Commandlet.h`)는 `CommandLineTools`의 `CommandSet`으로 대체하고 지운다(하위호환 없음, D24). 게임 실행 파일은 commandlet을 쓰지 않는다.

### 7.2 코드 크기 목표

외부 라이브러리를 쓰지 않은 이유가 크기이므로, 자체 구현에도 상한을 둔다. 단계 완료 때 측정해 보고한다.

| 부분 | 소스 상한(시험 제외) | Release x64 실행 파일 증가 상한 | 측정 |
|---|---|---|---|
| Core `App`·`Time`·`ProcessCommandLine` | 600줄 | 8 KiB | Release Sandbox 크기 비교 |
| `CommandLine`(런타임 파서) | 1,500줄 | 32 KiB | Release Sandbox 크기 비교. ReflectionText·ObjectRuntime은 게임 호스트가 이미 Config를 거쳐 링크하므로 새 의존이 아니다 |
| `CommandLineTools`(Developer) | 2,000줄 | Release 0(링크하지 않는다) | Release 링크 맵에 기호가 없음을 확인 |
| MetaCompiler 추가 | 700줄(C#) | — | — |
| 생성 스키마 문자열 | — | Release는 도움말·예시·값 이름 문자열 0 B | 생성 코드가 `ROCKS_BUILD_RELEASE`이면 그 칸을 빈 값으로 낸다(CVar의 출시 빌드 치트 문자열 제거와 같은 방향 — [CVar](CVarSystem.ko.md) 13.1절) |
| `RocksCli`(C#) | 400줄 | — | — |

## 8. 앱 정보 `AppInfo`

### 8.1 레코드

```cpp
namespace rocks {

// 실행 종류다. 빌드 대상 종류(build::TargetType)와 다르다. Program 대상 하나가 commandlet으로도 도구로도 돈다.
enum class HostKind : uint8 { Game, Commandlet, Tool, TestRunner };

// 실행 성격이다. 명령줄과 빌드·호스트 기본값에서만 정하고(10절) 실행 중 바뀌지 않는다.
struct RunTraits {
    bool unattended = false;        // 사람이 보지 않는다: 대화상자·입력 대기 금지
    bool unattendedInput = false;   // 무인이어도 입력 장치를 연다
    bool benchmark = false;         // 비출시 빌드만. fixedSeed·고정 step을 함께 켠다
    bool fixedSeed = false;         // 무작위 시드를 상수로
    Optional<double> fixedFrameSeconds; // 값이 있으면 엔진 프레임 delta를 이 값으로(초)
    bool headless = false;          // 창·그래픽 장치를 만들지 않는다
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
  - 이미 설치돼 있으면 생성자가 Fatal이다(두 번 설치는 부트 순서 버그다).
  - 시험은 시험마다 scope를 만든다. 시험 실행기(`HostKind::TestRunner`)는 기본 AppInfo 하나를 설치하고, 다른 값이 필요한 시험은 TestSupport의 `ReplaceAppInfoForTest` scope로 잠시 바꾼다. 그동안 다른 스레드가 읽지 않는 시험만 쓴다(문서화한 전제).
- **왜 전역인가(D3).** 로그·trace·크래시 처리·Fatal 대화상자 정책은 Core 깊은 곳에서 프로젝트 이름·instance id·unattended를 읽어야 한다. 이것을 모두 인자로 전하면 Core API가 앱 정보를 줄줄이 받아야 한다. 값이 불변이고 설치·해제가 범위로 묶이므로 UE 전역의 스레드·수명 문제(동기화 없는 쓰기, 첫 호출 시점에 굳는 static)가 없다. 이미 쓰는 `LogInstallScope`와 같은 방식이다.

### 8.3 부트 순서

Config 5.4절 부트 순서 앞뒤에 다음을 끼운다.

| 순서 | 일 | 실패 처리 |
|---|---|---|
| 0 | 진입점이 `ProcessCommandLine::Capture`를 부른다(Windows는 넓은 문자 원문에서 — 12절) | 종료 코드 1, stderr 한 줄 |
| 1 | 호스트가 공용 옵션 struct(`HostCommandLineOptions`)와 자기 옵션 struct를 파싱한다(14절) | 사용법 오류: 진단 + 짧은 사용법, 종료 2. 로그가 없으므로 stderr에 직접 쓴다 |
| 2 | `RunTraits`를 정하고 Fatal 대화상자 정책을 바로 적용한다(unattended면 대화상자 없음) | — |
| 3 | `HostSettings::Initialize` — `projectName`·`projectDir`를 1단계 결과로 채운다 | 기존과 같다 |
| 4–7 | 층 파일 → `--config` 파일 → `--set` 값 → `CompleteStartup`(기존 2–5단계). `--set` 값은 파싱한 배열을 그대로 받는다(14.3절) | 기존과 같다 |
| 8 | `AppInfo::Create` → `AppInfoScope` 설치 | 종료 코드 1 |
| 9 | LogHost 설치 → 시작 로그 묶음(D14) | 기존과 같다 |
| 10 | trace 세션 → `--trace` 목록 적용 → 엔진 루프 | 기존과 같다 |

- 2단계부터 8단계 사이의 Fatal은 AppInfo가 아직 없다. 대화상자 정책은 2단계에서 PlatformBase에 직접 넣은 값을 본다(AppInfo를 보지 않는다).
- Commandlet 호스트도 같은 순서다. 그래서 commandlet에서도 `--set`·`--trace`가 동작한다. 지금은 쓸 수 없다(5.2절).

### 8.4 스레드

| 대상 | 쓰는 쪽 | 읽는 쪽 | 동기화 |
|---|---|---|---|
| AppInfo 내용 | 만들 때 한 번(게임 lane, 부트) | 모든 스레드 | 설치 포인터의 release·acquire. 내용은 불변 |
| 설치 포인터 | `AppInfoScope` 생성·소멸(부트·종료) | `Get()` | 원자 포인터. worker·렌더 lane은 설치 뒤에 시작하고 해제 전에 끝난다(Config 5.4 순서와 같다) |
| `RunTraits`가 PlatformBase에 넣은 대화상자 정책 | 부트 2단계 한 번 | Fatal 경로(아무 스레드) | relaxed 원자 값 하나(정책 하나라 순서 문제가 없다) |

### 8.5 빌드 정보 조회

`AppInfo::Build()`는 `build::Description()`(11절)과 같은 레코드를 가리킨다. 상수라 스레드 안전하다. AppInfo를 설치하기 전에도 `build::Description()`을 직접 부를 수 있다.

### 8.6 AppInfo에 두지 않는 것

| UE 값 | Rocks에서 맡는 곳 | 이유 |
|---|---|---|
| 포커스(`HasFocus`) | PlatformWindow 이벤트를 받은 엔진이 프레임 상태로 공개한다: `Engine::IsForeground()`(게임 lane). 다른 lane이 필요하면 프레임 packet에 넣는다. benchmark면 늘 true(UE `APPC` 347–350과 같다) | 실행 중 바뀐다. 원천은 `ui::Platform`·PlatformWindow다 |
| 볼륨 배수·포커스 잃었을 때 배수 | 오디오 모듈의 `audio.*` CVar | 설정 값이다. UE도 config에서 읽는다(`APPC` 316) |
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
    Optional<double> fixedDeltaSeconds; // 고정 step이면 그 길이
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
- **고정 step delta는 double 그대로다.** UE는 고정 delta를 float 지역 변수로 거쳐 넣는다(`UEN` 3019). 그래서 1/30이 float로 반올림된다. 단위·형식(double 초)은 UE와 같다. Rocks는 이 중간 float 반올림을 따르지 않는다. UE 결함으로 보고 15절이 아니라 이 줄에만 적는다. 결정적 실행의 비트 결과가 UE와 같아야 하는 기능(UE 데이터와 비트 대조)이 생기면 다시 본다.
- **음수 delta 처리 없음.** 단조 시계(`uint64` 차)라 음수가 없다. UE의 "음수면 0.01"(`UEN` 3160–3164)은 필요 없다.
- **엔진 delta에 상한이 없다.** UE 단독 게임의 `MaxDeltaTime` 상한(`UEN` 3184–3196)은 Rocks에서 World `GameClock`의 `maxFrameDeltaSeconds`가 맡는다([엔진 런타임](EngineRuntime.ko.md) 5절). 엔진 프레임 값은 측정값 그대로다.

### 9.2 스레드별 읽기

| 읽는 스레드 | 읽는 법 | 보는 값 |
|---|---|---|
| 게임 lane | `Engine::Timing()` | 이번 프레임 값 |
| 렌더 lane | 프레임 packet(RenderBase `FramePacket`) 안 `FrameTiming` 사본 | 그 packet을 만든 프레임의 값 |
| worker 작업 | 작업 문맥이 물려받은 값. CVar 설계가 계획한 "Task 생성 때 만든 lane의 묶음을 작업에 넘긴다" 장치에 `FrameTiming` 사본을 함께 싣는다 | 작업을 만든 프레임의 값 |
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
- **값 기본값.** 엔진 설정이 실행 성격에 따라 기본값이 달라야 하면 그 설정의 등록 코드가 `AppInfo::Get().Traits()`를 보고 Code 층에 쓴다. 예: benchmark면 프레임 상한을 끈다. 출처 스택에 `app traits`로 남아 Explain에서 보인다.
- **엔진 고정 step.** `EngineConfig::fixedFrameDeltaSeconds`는 호스트가 `Traits().fixedFrameSeconds`로 채운다. 지금 Sandbox가 상수 `HeadlessFrameSeconds`(1/60)를 넣는 것은 `--deterministic`의 기본값으로 옮긴다.

### 10.2 명령줄 플래그

공용 옵션 struct `HostCommandLineOptions`(14.3절)의 필드다.

| 옵션 | 효과 | 출시 빌드 | UE 대응 |
|---|---|---|---|
| `--unattended` | `unattended = true` | 받는다 | `-UNATTENDED` |
| `--unattended-input` | `unattendedInput = true` | 받는다 | `-UnattendedInput` |
| `--benchmark` | `benchmark = true`. `fixedSeed = true`이고 `fixedFrameSeconds`가 없으면 1/30. 포커스는 늘 true | **받지 않는다**(알 수 없는 옵션) | `-BENCHMARK`(`LEL` 2449–2454), 고정 step(`UEN` 3005), 시드(`LEL` 2462) |
| `--deterministic` | `fixedSeed = true`, `fixedFrameSeconds`가 없으면 1/60 | 받지 않는다 | `-Deterministic`(`LEL` 2456–2460) |
| `--fixed-seed` | `fixedSeed = true` | 받지 않는다 | `-FixedSeed` |
| `--fps <rate>` | `fixedFrameSeconds = 1 / rate`(rate는 유한한 양수) | 받지 않는다 | `-FPS=`(`LEL` 4724–4731) |
| `--headless` | `headless = true` | 받는다 | `-nullrhi`(뜻이 넓다 — 창·장치를 아예 만들지 않는다) |
| `--instance-id <guid>`·`--session-id <guid>`·`--session-name <text>` | AppInfo 칸 | instance-id만 받지 않는다 | `-InstanceId=`·`-SessionId=`·`-SessionName=` |

- 시험 실행기(`HostKind::TestRunner`)는 `unattended`가 기본 true다(UE `GIsAutomationTesting` OR와 같은 뜻).
- 이름만 예약하고 지금 받지 않는 것: `--no-sound`·`--mute-audio`(오디오 모듈), `--single-thread`(20절).
- 출시 빌드에서 받지 않는 옵션은 출시 빌드 스키마에서 빠진다. 그래서 "알 수 없는 옵션"이다. 지정자는 `DevelopmentOnly`(이미 있는 PROPERTY 플래그 — `Specifiers.cs:204–259`)를 쓴다.

## 11. 빌드 정보 주입

### 11.1 레코드

```cpp
namespace rocks::build {

enum class BuildConfiguration : uint8 { Debug, Development, Release };
enum class TargetType : uint8 { Game, Program };

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
- **날짜는 커밋 시각이다(D11).** UE는 `__DATE__`(컴파일 시각)를 쓴다. 이것은 같은 소스에서 빌드할 때마다 byte가 달라진다. Rocks는 재현 빌드를 위해 커밋 시각을 쓴다. dirty 빌드는 `commitDirty`로 구분한다.
- **캐시 무효화와 분리(D12).** `ToolBuildId()`는 입력 내용으로 정해지는 진단·캐시 무효화 키다(`BuildInfo.h:20–24`). BuildStamp TU는 그 입력에서 뺀다. 커밋만 바뀌고 코드가 같으면 `ToolBuildId`가 같아야 한다. 쿠킹 byte·에셋 너겟 키에도 넣지 않는다(엔진 버전과 같은 규칙).
- **구성·대상 종류.** 이미 있는 매크로(`ROCKS_BUILD_*`)와 빌드 규칙의 `TargetType`에서 생성 TU에 상수로 쓴다. C++ 코드의 `if constexpr` 분기는 계속 `IsReleaseBuild` 상수를 쓴다. `Description()`은 진단·출력용이다.
- **사용처.** `--version` 출력(14.6절), 시작 로그(D14), 크래시 보고·trace 파일 머리(나중).

`--version` 출력 예(C++·C# 같은 모양):

```
RocksCommandlet 0.1.0 (Development, Program, commit 4a28102d, 2026-11-18)
```

## 12. 프로세스 명령줄 `ProcessCommandLine`

```cpp
namespace rocks {

// 진입점이 한 번 캡처하는 프로세스 명령줄이다. 캡처 뒤 불변이고 모든 스레드가 읽을 수 있다.
class ProcessCommandLine {
public:
    static constexpr usize MaxArguments = 4096;
    static constexpr usize MaxTotalBytes = 128 * 1024;

    // 진입점에서 한 번 부른다. Windows는 argv를 무시하고 넓은 문자 원문을 UTF-8로 바꾼다.
    // 오류: 두 번째 호출 AlreadyExists, 잘못된 UTF-16·UTF-8 InvalidArgument, 상한 초과 CapacityExceeded.
    static Result<void> Capture(int argc, char** argv) noexcept;

    // 전제: Capture 성공(Debug·Development assert).
    static const ProcessCommandLine& Get() noexcept;

    StringView ExecutablePath() const noexcept;
    Span<const StringView> Arguments() const noexcept;  // 실행 파일 제외, 원문 순서
};

} // namespace rocks
```

- **원문 보존.** 캡처한 인자는 바꾸지 않는다. UE `Set`·`Append`(`CLH` 88–94)에 대응하는 것이 없다. 응답 파일을 펼친 결과는 파서가 따로 만드는 값이고, 보관된 원문은 그대로다.
- **정규화.** UTF-8 검증만 한다. 대소문자·경로 구분자·따옴표는 건드리지 않는다. 따옴표 처리는 OS가 이미 했다(Windows는 `CommandLineToArgvW` 규칙). PlatformBase `Process::Start`가 자식 명령줄을 만들 때 쓰는 규칙(MSVC CRT 인용)과 같은 쌍이다([플랫폼 층](PlatformLayer.ko.md) 3절).
- **Windows.** `GetCommandLineW` + `CommandLineToArgvW`로 넓은 문자 원문을 얻어 UTF-8로 바꾼다. 진입점이 `main(char**)`이어도 한글 경로가 깨지지 않는다(5.2절 문제). 이 일은 PlatformBase `Private/Win64`가 한다.
- **Android.** argv가 없다. 런처 intent의 추가 인자 문자열을 같은 형태로 넣는 경로는 Android 호스트를 만들 때 정한다(20절).
- **로그용 형태와 비밀 가리기(D16).** 원문은 로그·trace·크래시 보고에 직접 쓰지 않는다. 파서가 스키마를 알고 난 뒤 `RedactedArguments(schema, out)`로 `Secret` 옵션의 값을 `<redacted>`로 바꾼 형태를 만든다. 시작 로그는 이 형태만 쓴다.
  - 파싱 실패로 스키마 판정을 못 한 인자도 원문 대신 `<unparsed>`로 남긴다. 비밀 값이 오타 때문에 그대로 새는 것을 막는다.
  - `--set` 값은 설정 쪽 비밀 표시가 생기면 그 대상의 값을 가린다. 지금 설정에는 비밀 표시가 없다.
- **AppInfo와의 관계.** 둘은 따로 설치되는 불변 값이다. `ProcessCommandLine`은 0단계, AppInfo는 8단계에서 설치된다. AppInfo는 명령줄을 소유하지 않는다. AppInfo의 칸 일부(`sessionId`·`instanceId`·실행 성격)가 파싱 결과에서 온다.
- **하위 프로세스 상속.** UE `RegisterArgument(…, Inherit)`처럼 옵션에 "자식에게 물려준다" 표시를 두는 것은 이번 범위에서 뺀다. 자식을 띄우는 도구(셰이더 컴파일 worker 등)가 생길 때 `Inherit` 지정자로 더한다(20절).

## 13. 명령줄 파서(C++) — 선언

### 13.1 원칙

- 편의성의 기준점은 C#의 System.CommandLine이다. 명령 하나를 만들 때의 코드 양, 옵션 선언 한 줄로 파싱·검증·도움말이 나오는지, 형식 변환이 자동인지, 결과를 꺼내 쓰기가 편한지를 C#과 같은 수준 이상으로 맞춘다(15절 비교).
- **정의 한 곳.** 명령·하위 명령·옵션·위치 인자마다 이름·별칭·형식·기본값·필수 여부·설명·예시를 옵션 struct에 한 번 선언한다. 그 정의 하나로 파싱·검증·도움말을 모두 만든다. 설명을 두 번 쓰지 않는다.
- **외부 라이브러리 없음.** CLI11·cxxopts·argparse·Lyra·Boost.Program_options를 검토하고 제외했다. 이유는 크기다(사용자 결정 2026-11-18). 형식은 리플렉션 struct가, 값 변환은 ReflectionText가 이미 갖고 있어 새로 들여올 것이 없다.

### 13.2 선언 예

```cpp
ROCKS_ENUM()
enum class TraceReportFormat : uint8 {
    Text ROCKS_ENUMERATOR(Name = "text"),
    Json ROCKS_ENUMERATOR(Name = "json")
};

// 명령 하나 = 옵션 struct 하나. 도움말 문자열은 실행 중 출력이라 영어다.
ROCKS_STRUCT(Command(Name = "trace-report", Help = "Convert a trace file into a text report.",
                     Examples = ("trace-report run.rtrace",
                                 "trace-report run.rtrace report.json --format json")))
struct TraceReportOptions {
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
- 형식은 필드 형식에서 나온다.
  - `bool`은 값 없는 플래그다.
  - `Optional<T>`·`Array<T>`는 선택이다.
  - `Array<T>`는 옵션을 반복해 받는다.
  - `ROCKS_ENUM`은 이름 목록으로 받는다.
- 기본값은 생성자를 실행한 결과다(멤버 초기화식). 도움말의 `[default: …]`는 기본 인스턴스를 `FormatTextValue`로 출력해 만든다.
- 필드 사이 검증은 이미 있는 struct `Validate` hook(CV3)을 쓴다. 예: "`--fps`와 `--benchmark`는 함께 쓸 수 없다".

### 13.3 지정자(MetaCompiler에 더할 것)

| 대상 | 지정자 | 값 | 뜻 | 검사(RMC) |
|---|---|---|---|---|
| STRUCT | `Command(...)` 묶음 | 아래 키 | 이 struct는 명령줄 정의다. `Settings(...)`와 같은 묶음 지정자다 | 같은 struct에 `Settings`와 함께 쓰지 않는다 |
| `Command` 안 | `Name` | 문자열 | 명령 경로. 하위 명령은 공백으로 잇는다(`"log keygen"`). 각 마디는 commandlet 이름 문법(소문자·숫자·`-`) | 문법, 예약어 `help` |
| `Command` 안 | `Help` | 문자열 | 한 줄 설명(명령 목록과 Description 절) | 비어 있지 않다(Release 생성은 빈 값) |
| `Command` 안 | `Examples` | 문자열 목록 `("…", "…")` | 예시 줄. 프로그램 이름을 빼고 쓴다 | **새 값 형태**: 지정자 문법에 문자열 목록을 더한다(`Specifiers.cs` 86–92·337행의 값 분류) |
| `Command` 안 | `Global` | 플래그 | 공용 옵션 struct(모든 명령에서 받는다 — System.CommandLine의 Recursive) | `Name`과 함께 쓰지 않는다 |
| PROPERTY | `Help` | 문자열 | 옵션·인자 설명. 계획된 함수 리플렉션의 `ROCKS_FUNCTION(…, Help = "…")`([CVar](CVarSystem.ko.md) 14.3절)과 같은 키 이름이다 | `Command` struct 안에서는 필수(`Hidden` 제외) |
| PROPERTY | `Alias` | 문자열(쉼표 목록) | 다른 이름. `-f`(한 글자) 또는 `--other-name` | 문법, 같은 명령·공용 옵션과 겹침 |
| PROPERTY | `Argument` | 플래그 | 위치 인자. 선언 순서대로 받는다 | `Alias`와 함께 못 쓴다. 필수 인자는 선택 인자 앞, `Array` 위치 인자는 마지막 하나만 |
| PROPERTY | `Mandatory` | 플래그 | 반드시 줘야 하는 옵션. 기존 `Required`(참조 non-null)와 뜻이 달라 다른 이름을 쓴다 | `Optional`·`Array`·`bool`에는 못 쓴다 |
| PROPERTY | `ValueName` | 문자열 | 도움말의 `<값>` 자리 이름. 없으면 옵션 이름(대시 뺀 것) | 문법 |
| PROPERTY | `Range` | 문자열 `"1..1000000"` | 수 범위. MetaCompiler가 필드 형식으로 양 끝을 해석해 검사한다 | 수 형식만, 최소 ≤ 최대 |
| PROPERTY | `Hidden` | 플래그 | 도움말에 보이지 않는다(받기는 한다) | — |
| PROPERTY | `Secret` | 플래그 | 로그용 명령줄에서 값을 가린다(12절) | `bool`에는 못 쓴다 |
| PROPERTY | `DevelopmentOnly`(기존 키) | 플래그 | Release 스키마에서 빠진다 | 기존 검사 |

- **위치 인자 필수 규칙.** `Optional`·`Array`가 아닌 위치 인자는 필수다. 따로 표시하지 않는다.
- **도움말은 문서 주석에서 가져오지 않는다(D18).** 손으로 쓴 주석은 한국어이고 도움말은 실행 중 출력이라 영어다. 문서 주석을 가져오면 이 규칙이 깨진다. 주석 추출은 MetaCompiler에 Clang 주석 노드 수집을 새로 넣는 일이기도 하다.
- **생성 코드.** `GeneratedType<S>::CommandLine()` → `CommandLineSchema`를 낸다(`CppEmitter.cs` 796–815의 `Settings()` 선례).
  - FieldSchema에 칸을 늘리지 않는다. 늘리면 TypeRegistry 등록이 문자열을 깊게 복사하는 경로(`TypeSchema.h:502–503`)까지 바뀐다. 별도 스키마는 영향 범위가 작다.
  - 모듈 사이 import descriptor 왕복(`Programs/Rocks.MetaCompiler/ModuleDescriptor.cs:31,243–244,676–677`)에 새 키를 더한다.
  - Release 생성에서는 `Help`·`Examples`·`ValueName` 칸을 빈 값으로 낸다(7.2절).

```cpp
// Meta/CommandLineSchema.h (생성 코드가 채운다)
struct CommandLineFieldSchema {
    uint32 field;              // FieldSchema 순번
    StringView option;         // "--max-frames", 위치 인자는 빈 값
    StringView aliases;        // "-n" 또는 "-n,--count"
    StringView valueName;
    StringView help;
    StringView range;          // "1..1000000" 또는 빈 값
    CommandLineFieldFlags flags; // Argument·Mandatory·Hidden·Secret
};

struct CommandLineSchema {
    StringView name;           // "trace-report", 공용 옵션은 빈 값
    StringView help;
    Span<const StringView> examples;
    bool global;
    Span<const CommandLineFieldSchema> fields;
};
```

### 13.4 콘솔 명령과의 관계

콘솔 명령 인자 struct(`trace.enable channels=cpu,gpu`)도 리플렉션 struct다(5.5절). 문법이 다르다. 콘솔은 `name=value`, 명령줄은 `--name value`다. Config D9(콘솔 명령과 commandlet registry는 따로)를 유지한다. 다만 `Help` 지정자는 같은 키이므로, 콘솔 `help <명령>`이 나중에 같은 문자열을 쓸 수 있다.

## 14. 명령줄 파서(C++) — 동작

### 14.1 문법 규칙(양쪽 공통 — D20)

| 규칙 | 내용 |
|---|---|
| 옵션 철자 | `--kebab-case`. 한 글자 별칭은 `-x`. 대소문자를 구분한다 |
| 값 | `--name value`와 `--name=value` 둘 다. 값이 `-`로 시작하면 `=` 형태만 된다(음수 등) |
| bool | `--flag`는 true. 명시하려면 `--flag=false`. 다음 토큰을 값으로 먹지 않는다 |
| 반복 | `Array<T>` 필드는 옵션을 반복해 원소를 하나씩 더한다. 쉼표로 나누지 않는다(값 안의 쉼표는 그 값의 일부 — `--set=log.levels=a:debug,b:info`). 스칼라 옵션을 두 번 주면 오류다 |
| 위치 인자 | 옵션이 아닌 토큰을 선언 순서대로 채운다. 옵션과 섞어 써도 된다 |
| `--` | 그 뒤는 모두 위치 인자다 |
| 하위 명령 | 첫 위치 토큰부터 명령 경로를 찾는다(`log keygen`). 공용 옵션은 명령 앞뒤 어디서나 받는다 |
| 묶음 `-abc`, `/x` | 받지 않는다 |
| 응답 파일 | `@path`(도구·commandlet만 — 14.7절) |
| 도움말 | `--help`·`-h`·`-?`, `help [명령…]` |
| 버전 | `--version`(최상위만) |

### 14.2 값 변환과 바인딩(D19)

- 옵션 하나는 옵션 struct에 대한 **ReflectionText 대입 하나**다. 예: `--format json`은 대입 `format=json`, `--channel cpu`는 `channel=$append:cpu` 형태의 원소 추가다. 표기는 ReflectionText 명령행 단일 값 표기를 그대로 쓴다. 수·enum·Optional·Guid·Name 변환을 새로 짜지 않는다.
- 바인딩은 `PrepareAssignments(AssignmentTarget::Value(...), …)`로 한다.
  - 파서는 호출마다 작은 `TypeRegistry`·`ObjectRegistry`를 만들고 옵션 struct의 모듈 타입을 생성 집계 함수로 등록한다. 부트 1단계는 `HostSettings`보다 앞서기 때문이다(`--project`가 HostSettings 초기화의 입력이다).
  - 비용(등록 시간·코드 크기)은 CL2에서 측정한다(20절).
- 대입이 끝나면 다음 순서로 검사한다.
  1. `Mandatory`·필수 위치 인자가 채워졌는지
  2. `Range`
  3. struct `Validate` hook
- 결과는 옵션 struct 값 하나다. 실행 함수는 `const Options&`로 받는다.

```cpp
// 런타임 API(CommandLine 모듈). 도움말은 그리지 않는다.
template <class Options>
Result<CommandLineOutcome> ParseCommandLine(Span<const StringView> arguments, Options& out,
                                            CommandLineDiagnostics& diagnostics) noexcept;

enum class CommandLineOutcome : uint8 {
    Parsed,       // out이 채워졌다
    HelpRequested,
    VersionRequested,
    UsageError    // diagnostics에 오류가 있다. 종료 코드 2
};
```

### 14.3 호스트 공용 옵션과 `--set`·`--trace`·`--config`(D25)

```cpp
// HostConfig 모듈. 모든 호스트(게임·Sandbox·RocksCommandlet)가 받는 공용 옵션이다.
ROCKS_STRUCT(Command(Global))
struct HostCommandLineOptions {
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

    ROCKS_PROPERTY(DevelopmentOnly, Help = "Run with a fixed time step and a fixed random seed")
    bool deterministic = false;

    // … 10.2절 나머지
};
```

- `--set`·`--trace`·`--config`를 같은 파서로 옮긴다. 인자 배열은 한 번만 읽는다.
  - `HostSettings::ApplyCommandLine`은 원시 인자 대신 `Span<const String>`(파싱한 `set` 배열)을 받는다. `--set=`를 두 번 훑는 일(5.4절)이 없어진다.
  - `TraceController::ApplyCommandLine`도 `trace` 배열을 받는다.
- **값의 의미는 그대로다.** `--set` 값은 지금처럼 Config가 ReflectionText 대입으로 해석하고 출처는 `--set`이다. `--trace` 값은 지금처럼 쉼표 목록을 TraceController가 나눈다. 바뀌는 것은 인자를 찾는 곳뿐이다.
- `--set=a=b`와 `--set a=b`를 둘 다 받는다(첫 `=`까지가 옵션 이름).
- 출시 빌드의 `--set` 제한(Config D5)은 파서가 아니라 Config가 그대로 판정한다.
- 도움말에서 공용 옵션은 각 명령의 Options 절에 함께 나온다(System.CommandLine의 Recursive 옵션과 같은 모양).

### 14.4 진단(D22)

| 종류 | 문구(영어, 양쪽 같음) |
|---|---|
| 알 수 없는 옵션 | `Unrecognized option '--fromat'. Did you mean '--format'?` |
| 알 수 없는 명령 | `Unrecognized command 'trace-reprot'. Did you mean 'trace-report'?` |
| 남는 위치 인자 | `Unrecognized argument 'extra'.` |
| 값 없음 | `Option '--format' requires a value.` |
| 형식 오류 | `Cannot parse 'forty-two' for option '--frames' as uint64.` |
| 범위 밖 | `Value 0 for option '--frames' is out of range 1..1000000000.` |
| enum 값 오류 | `'xml' is not a valid value for '--format'. Allowed values: json, text.` |
| 필수 옵션 없음 | `Option '--out' is required.` |
| 필수 인자 없음 | `Required argument 'input' was not provided.` |
| 명령 없음 | `Required command was not provided.` |
| 스칼라 중복 | `Option '--format' was specified more than once.` |
| struct 검증 | `Validate`가 돌려준 이유 문자열 |

- 오류는 stderr에 `error: ` 접두로 모두 쓴다. 그다음 그 명령의 짧은 사용법 한 줄과 안내 한 줄을 쓴다. 종료 코드는 2다.

```
error: Unrecognized option '--fromat'. Did you mean '--format'?
Usage: RocksCommandlet trace-report <input> [<output>] [options]
Run 'RocksCommandlet trace-report --help' for more information.
```

- **이름 제안.** 후보는 그 명령의 옵션·별칭과 공용 옵션(명령 오류면 형제 명령)이다. 거리는 Damerau-Levenshtein이고, 대상은 대시를 뗀 이름, 대소문자 무시다. 거리가 `max(1, 길이 / 3)` 이하인 것 중 가장 가까운 것을 고른다. 같은 거리면 사전순 첫째다. 후보가 없으면 제안 문장을 붙이지 않는다. C# 쪽도 같은 알고리즘·같은 문턱을 쓴다(16절).
- 진단은 `CommandLineDiagnostics`에 모두 모은다(첫 오류에서 멈추지 않는다). 출력 순서는 인자 순서다.
- ReflectionText 형식 오류(`TextDiagnostics`)는 위 표의 "형식 오류" 문구로 옮긴다. 원래 진단 코드는 Debug 로그에 남긴다(원본 오류 코드 보존).

### 14.5 도움말(D21)

모양은 System.CommandLine 기본 도움말과 같다(Description·Usage·Arguments·Options·Commands 절, 두 칸 들여쓰기, 열 맞춤, `[default: …]`, 필수 표시 `(REQUIRED)`). 여기에 Examples 절을 더한다.

**최상위**(`RocksCommandlet --help`, `RocksCommandlet help`):

```
Description:
  Rocks command-line tools.

Usage:
  RocksCommandlet [command] [options]

Options:
  --set <target=value>  Override a setting for this run
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
  --trace <channels>        Enable trace channels or presets
  --project <path>          Project directory
  --unattended              Never show dialogs or wait for user input
  -?, -h, --help            Show help and usage information

Examples:
  RocksCommandlet trace-report run.rtrace
  RocksCommandlet trace-report run.rtrace report.json --format json
```

**하위 명령 묶음**(`RocksCommandlet log --help`): Description·Usage(`RocksCommandlet log [command] [options]`)·Options·Commands(`keygen`·`decode`·`seal`) 절이 나온다.

배치 규칙:
- **너비.** 터미널이면 `platform::TerminalColumns(stream)`이고, 터미널이 아니면(파일·파이프) 80이다. 범위는 [40, 200]으로 자른다. 시험은 너비를 인자로 준다.
- **왼쪽 열.** 너비는 `min(가장 긴 이름 칸 + 2, 전체 너비의 40%)`다. 절마다 따로 정한다. 이름 칸 + 두 칸 간격이 열보다 길면 이름 칸을 공백 경계에서 접고, 이어지는 줄은 두 칸 더 들여 쓴다(위 `trace-report <input> [<output>]`). System.CommandLine도 이름 칸을 열 안에서 접는다.
- **오른쪽 열.** 설명은 단어 경계에서 접는다. 길이는 UTF-8 코드 포인트로 센다. 도움말 문자열은 영어 ASCII이고, 기본값 표시만 비 ASCII일 수 있다.
- **정렬.** enum 값 목록은 이름순이다(System.CommandLine과 같다). 옵션은 선언 순서이고, 공용 옵션이 그 뒤에, 그다음 `--version`(최상위만)·`--help`가 온다. 명령 목록은 이름순이다.
- **필수 표시.** 필수 옵션은 설명 끝에 ` (REQUIRED)`를 붙인다.
- **기본값 표시.** `[default: …]`는 기본값이 빈 문자열·빈 배열·빈 Optional·false가 아닐 때만 붙인다.
- **출력 대상.** 도움말은 stdout이고 종료 코드 0이다.
- **Release 빌드.** 게임 호스트는 `CommandLineTools`를 링크하지 않는다. `--help`는 `Help is not available in Release builds.` 한 줄을 stdout에 쓰고 종료 0이다.

### 14.6 commandlet 구조(D24)

```cpp
// Programs/Commandlet의 등록 함수. 명령 하나에 한 줄이다.
Result<void> RegisterCommands(CommandSet& commands) noexcept {
    if (Result<void> added = commands.Add<TraceReportOptions>(&RunTraceReport); !added) {
        return added;
    }

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
  - 하위 명령 묶음(`log keygen`의 `log`)을 자동으로 만든다.
  - 공용 옵션 struct를 연결한다.
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
- 토큰 문법은 System.CommandLine 응답 파일과 같게 한다(줄마다 공백으로 나누고 큰따옴표로 묶으며 `#`로 시작하는 줄은 주석). 채택한 System.CommandLine 버전에서 실제 문법을 확인하고 양쪽 golden 시험으로 맞춘다(20절).
- 상한(이름 붙은 상수):
  - 파일 크기 1 MiB
  - 중첩 깊이 4
  - 펼친 뒤 인자 수 `ProcessCommandLine::MaxArguments`
- 순환(같은 파일을 다시 펼침)은 오류다.
- 파일을 못 읽으면 `Cannot read response file 'x.rsp': NotFound.`이고 종료 2다. 원래 오류 코드 이름을 붙인다.
- 펼친 위치는 진단에 `x.rsp:3`처럼 파일·줄로 나온다.

## 15. C++과 C# 나란히 보기

같은 명령(`trace-report` — 예시라 지금 명령에 없는 `--format`·`--channel`을 더했다)을 양쪽으로 쓴다.

**C# (System.CommandLine + `RocksCli`)**

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
Option<ReportFormat> format = new("--format", "-f")
{
    Description = "Report format",
    DefaultValueFactory = _ => ReportFormat.Text,
};
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
command.SetAction(result => TraceReport.Run(
    result.GetValue(input)!,
    result.GetValue(output),
    result.GetValue(format),
    result.GetValue(channel) ?? []));
```

**C++ (리플렉션 옵션 struct)** — 13.2절 선언(약 20줄) + 등록 한 줄 + 실행 함수:

```cpp
commands.Add<TraceReportOptions>(&RunTraceReport);

Result<int32> RunTraceReport(CommandContext& context, const TraceReportOptions& options) noexcept {
    // options.input, options.output, options.format, options.channel을 바로 쓴다.
}
```

| 기준 | C# (System.CommandLine) | C++ (Rocks) |
|---|---|---|
| 명령 하나의 코드 양 | 선언 약 25줄 + action | 선언 약 20줄 + 등록 1줄 |
| 옵션 한 줄로 파싱·검증·도움말 | 된다(`Option<T>` + `Description`) | 된다(필드 + `ROCKS_PROPERTY(Help=…)`) |
| 형식 변환 | 자동(`T`에서) | 자동(필드 형식 + ReflectionText) |
| 결과 꺼내기 | 옵션마다 `GetValue(option)` | struct 필드 그대로(`options.format`) |
| 필드 사이 검증 | `Validators.Add` | struct `Validate` hook |
| 예시 절 | `RocksCli.AddExamples`(System.CommandLine에 없음) | `Command(Examples=…)` |

두 쪽의 `--help` 출력은 14.5절 "명령 상세" 예와 byte 단위로 같아야 한다. 프로그램 이름만 다르다(C# 시험 실행 파일 이름). 17절 golden 시험이 이를 확인한다.

## 16. C# 도구

### 16.1 파서 선택(D26)

| 항목 | System.CommandLine | 자체 작은 라이브러리 |
|---|---|---|
| 기능 | 선언형 옵션·인자·하위 명령, 자동 도움말, 형식 변환, 응답 파일, 이름 제안, 탭 완성 지시자 | 모두 새로 만든다 |
| 안정성 | 안정판 있다. NuGet 기준 3.0.0(2026-11-10), 2.0.x 줄(2.0.15)도 유지된다 | 우리 책임 |
| 라이선스 | MIT | — |
| AOT·트리밍 | NuGet 페이지가 "trimming과 AOT 완전 지원"으로 적는다. 2.0부터 리플렉션 바인딩을 뺐다 | — |
| 의존 | net10.0 대상은 의존 없음 | 없음 |
| 이전 비용 | 도구 셋(Build.Cli·MetaCompiler·ToolTests)과 그 안의 하위 명령 묶음 둘(MetaCompiler `ids`·ToolTests `scaffold`)의 손 루프를 명령 정의로 바꾼다. 손 루프 코드가 줄어든다 | 같은 이전 + 라이브러리 작성 |
| 단점 | 저장소 첫 외부 NuGet 패키지다. restore가 필요하다(오프라인 빌드는 NuGet 캐시·사내 feed). 공급망 관리(버전 고정·lock 파일)가 생긴다. 메이저 판 사이 API 변화가 있었다(2.0 beta 동안 큰 변경) | 기능 차이를 계속 메워야 한다 |

**권장: System.CommandLine을 쓴다.** 사용자가 외부 패키지를 허용했다(2026-11-18). 편의성 기준점이 이 패키지이므로 C# 쪽은 기준 그 자체를 쓰는 것이 가장 정확하다.

들여올 때 지킬 것:
- **중앙 버전 관리.** 저장소 루트 `Directory.Packages.props`(Central Package Management)에 버전 하나를 고정한다. 착수 때의 최신 안정판을 쓴다(지금 3.0.0).
- **lock 파일.** `packages.lock.json`과 `RestoreLockedMode`(CI)를 켜 공급망을 고정한다.
- **policy 정리.** 지금 policy 검사 가운데 "PackageReference 0건"을 전제한 것이 있는지 착수 때 확인하고 허용 목록으로 바꾼다(20절).
- **Rocks.Build의 offline bootstrap**(`ThirdPartyBootstrapCommand.cs`)과 NuGet 캐시 관계를 정한다. 빌드 도구 자신(Rocks.Build.Cli)이 패키지를 쓰므로 첫 restore는 네트워크가 필요하다.

### 16.2 `RocksCli`(Rocks.Tooling.Common)

System.CommandLine 위에 얇은 층 하나를 두어 C++과 같은 경험을 만든다. 400줄 이하.

| 일 | 방법 |
|---|---|
| 오류 모양·종료 코드 | `Invoke` 전에 `ParseResult.Errors`를 본다. 알 수 없는 토큰은 `UnmatchedTokens`로 14.4절 문구를 직접 만든다. 나머지 오류 문구는 System.CommandLine 문구를 쓴다(C++이 그 문구를 따라간다). `error: ` 접두 + 짧은 사용법 + `--help` 안내, 종료 2 |
| 이름 제안 | 14.4절과 같은 거리·문턱·동점 규칙. System.CommandLine 자체 제안 대신 이것을 쓴다(양쪽이 같은 후보를 내도록) |
| 예시 절 | `AddExamples(command, …)`가 도움말 action을 감싸 기본 도움말 뒤에 Examples 절을 쓴다 |
| `help <명령>` | 첫 토큰이 `help`면 `<명령…> --help`로 바꿔 파싱한다 |
| `--version` | 11.2절 모양(엔진 버전·구성·커밋·날짜)으로 바꾼다. 값은 MSBuild가 넣는 `AssemblyInformationalVersion`과 `Build/Build.version`에서 온다 |
| 실행 실패 | action 안의 처리되지 않은 예외는 `error: ` 한 줄 + 종료 1(스택은 `--verbose`일 때만) |

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
  - 시험용 교체 scope
- **실행 성격**
  - 10.2절 표의 모든 플래그 조합, `--benchmark`가 켜는 값
  - Release 스키마에서 `DevelopmentOnly` 옵션이 "알 수 없는 옵션"인지(`releaseRules` 같은 시험 옵션으로 Debug에서 확인)
  - 시험 실행기 기본 `unattended`
  - unattended에서 Fatal이 대화상자 없이 끝나는지(process-probe)
- **FrameTiming**
  - 고정 step에서 `currentSeconds`가 이전 + 고정 delta이고 `accumulatedSeconds`가 delta 합인지(비트 같음)
  - 고정 delta가 double 그대로인지(1/30 비트 비교)
  - 렌더 lane packet의 값이 packet을 만든 프레임 값과 같은지
  - worker가 제출한 프레임의 값을 보는지. 제출 뒤 게임 lane이 다음 프레임으로 넘어가도록 gate를 둔다. sleep을 쓰지 않는다
  - `LatestFrameIndex` 단조 증가
- **빌드 정보**
  - 생성 TU 값(구성·대상·커밋 길이·날짜 형식)
  - git이 없는 환경(빈 값, 빌드 성공)
  - 커밋만 바꾼 두 빌드의 `ToolBuildId` 같음
  - 같은 커밋 두 번 빌드의 BuildStamp 오브젝트 byte 같음(재현성)
  - `--version` 출력 golden
- **ProcessCommandLine**
  - Windows `main(char**)` 실행 파일에 한글·이모지 경로 인자를 `Process::Start`로 넘겨 UTF-8 원문 왕복
  - 상한, 두 번 캡처, 잘못된 UTF-16(짝 없는 서로게이트)
  - 로그용 형태에서 `Secret` 값이 `<redacted>`, 파싱 실패 인자가 `<unparsed>`
- **파서(C++)**
  - 14.1절 문법 표의 모든 줄(각 규칙의 성공·실패)
  - 진단 표의 모든 종류와 문구, 이름 제안 문턱 경계(거리 = 문턱, 문턱 + 1)와 동점 규칙, 진단 여러 개의 순서
  - `--`, bool 형태, 반복, 스칼라 중복, enum, Optional, Guid, 음수 값의 `=` 형태
  - 공용 옵션 위치(명령 앞·뒤), `Validate` hook, `Range`, `Mandatory`
  - `--set`·`--trace`·`--config`를 옮긴 뒤의 기존 Config·Trace 시험 통과와 `--set` 값의 출처 `--set` 유지
- **응답 파일**
  - 주석·따옴표·빈 줄, 중첩 깊이 4와 5, 순환, 크기 상한, 못 읽는 파일의 오류 코드 이름, 진단 위치 `파일:줄`
  - C#과 같은 입력 파일로 같은 토큰 열이 나오는지
- **MetaCompiler**
  - 13.3절 RMC 검사 각각의 실패 사례(`Alias`+`Argument`, 위치 인자 순서, `Array` 위치 인자 둘, `Mandatory`+`Optional`, `Range` 형식·순서, 이름·별칭 겹침, `Command` 이름 문법, `Help` 누락)
  - descriptor 왕복(다른 모듈의 옵션 struct)
  - Release 생성의 도움말 문자열 빈 값
- **도움말 golden(C++)**
  - 대상: 최상위, 명령 상세, 하위 명령 묶음, `help <명령>`, 오류 출력(알 수 없는 옵션·명령, 필수 누락, 형식 오류)
  - 너비 60·80·120 각각
  - 긴 이름 칸 접기, 긴 설명 접기, enum 목록, `(REQUIRED)`, `[default: …]`, 숨김 옵션 없음
  - golden 파일은 `Tests/Golden/`에 두고, 갱신은 명시 플래그로만 한다
- **양쪽 같음 golden**
  - 같은 시험 명령 묶음(최상위 + 하위 명령 둘 + 공용 옵션 + enum + 필수 + 위치 인자)을 C++ 시험 fixture와 C# 시험 프로젝트에 각각 정의한다
  - 같은 입력에서 도움말·오류·`--version` 출력이 byte 단위로 같은지 본다. 프로그램 이름 자리는 시험이 같은 이름을 준다
  - System.CommandLine 판을 올리면 이 시험이 차이를 잡는다
- **크기**
  - 7.2절 표의 측정(Release Sandbox 크기 비교, Release 링크 맵에 `CommandLineTools` 기호 없음)을 단계 완료 보고에 넣는다

## 18. 구현 단계

단계 기호는 AP(앱 정보)와 CL(명령줄)이다. 기존 문서의 기호(CV·CU·CF·CVR·TR·LG·RB 등)와 겹치지 않는다. 교차검증은 단계마다 리뷰어 1명이 한다(구현 → Debug·format·policy·바뀐 부분 시험 → 리뷰 → 반영). 스펙·문서 관점 리뷰는 묶음 끝 CL7에서 한 번 한다. 착수 시점은 머리말대로 사용자가 정한다.

| 단계 | 범위 | 선행 | 규모 |
|---|---|---|---|
| AP1 | Core `AppInfo`·`RunTraits`·`HostKind`·`AppInfoScope`, `ProcessCommandLine`(Windows 넓은 문자 캡처 — PlatformBase), Sandbox·RocksCommandlet·FoundationTests 진입점 연결, 대화상자 정책 값 | — | M |
| AP2 | Rocks.Build `<exe>.BuildStamp.cpp`·`build::Description()`, `ToolBuildId` 입력에서 제외, 시작 로그 묶음(D14) | AP1 | M |
| CL1 | MetaCompiler `Command(...)`·PROPERTY 키(13.3절)·문자열 목록 값·`CommandLineSchema` 생성·descriptor 왕복·RMC 진단, MetaSchema `CommandLineSchema.h` | — | M |
| CL2 | Core 런타임 `CommandLine` 모듈: 토큰화·매칭·ReflectionText 대입 바인딩·필수·범위·`Validate`·진단·이름 제안·로그용 가리기, 크기 측정 | CL1, AP1 | M |
| CL3 | Developer `CommandLineTools`: 도움말 writer·응답 파일·`CommandSet`·`--version`, PlatformBase `TerminalColumns`, C++ 도움말 golden | CL2, AP2 | M |
| CL4 | 이전: HostConfig `HostCommandLineOptions`(`--set`·`--config`·`--trace`·`--project`·실행 성격), 부트 순서(8.3절), RocksCommandlet 명령 넷과 `CommandletRegistry` 삭제, Sandbox·ShaderToolSpike·벤치 넷·FoundationTests, Sandbox 고정 delta를 `--deterministic`으로 | CL3 | M |
| CL5 | C#: `Directory.Packages.props`·lock 파일·System.CommandLine, `RocksCli`, Rocks.Build.Cli·MetaCompiler(`ids` 포함)·ToolTests(`scaffold` 포함) 이전 | — | M |
| CL6 | 양쪽 같음 golden 시험(17절) | CL3, CL5 | S |
| AP3 | `FrameTiming`(EngineTime 대체)·`LatestFrameIndex`, 렌더 packet 칸(RB1과 함께), worker 상속(CVar의 lane 묶음 장치와 함께), `Engine::IsForeground()` | AP1, RB1, CVR 묶음 장치 | M |
| AP4 | 실행 성격 소비처: 엔진 고정 step·시드, unattended 입력 장치 정책, `app.info` 콘솔 명령 | AP1, CL4 | S |
| CL7 | 문서 반영(설정 계층 5.4절 부트 순서, CVar 콘솔 영역 `app`, 로깅 시작 기록, 리플렉션 보강의 지정자 표, 구현 문서), 묶음 끝 리뷰 | 전부 | S |

## 19. 결정 목록

권장안으로 확정하는 것을 전제로 썼다(2026-11-18, 개정 1 초안). **[범위 큼]**과 **[원칙 확인]** 표시는 사용자 확인을 권하는 항목이다.

- **D1 이름과 배치** — `AppInfo`·`RunTraits`·`HostKind`·`ProcessCommandLine`·`FrameTiming`은 Core 런타임 `Core`에 둔다. 빌드 정보는 Core `Build`에, 파서는 새 Core 런타임 모듈 `CommandLine`에, 도움말·`CommandSet`·응답 파일은 새 Core Developer 모듈 `CommandLineTools`에 둔다. Engine에는 `FrameTiming` 갱신과 실행 성격 소비만 둔다.
- **D2 수명과 쓰기** — 호스트 부트가 한 번 채우는 불변 레코드다. setter가 없다. 실행 중 바뀌는 값(포커스·볼륨·그래픽 API 이름)은 두지 않는다.
- **D3 접근** — `AppInfoScope`로 프로세스에 설치하고 `AppInfo::Get()`으로 읽는다(`LogInstallScope` 방식). 인자 전달 방식은 Core 깊은 곳(로그·trace·Fatal)까지 줄줄이 넘겨야 해서 택하지 않았다.
- **D4 프레임 시간 전달 [범위 큼]** — 게임 lane은 `Engine::Timing()`, 렌더 lane은 프레임 packet 사본, worker는 작업 문맥 상속, 그 밖은 `LatestFrameIndex()`와 단조 시계만 읽는다. worker 상속은 CVar 설계의 lane 묶음 장치(Task 모듈, 아직 없음)와 한 장치로 만든다. 그래서 AP3은 RB1·CVR 작업에 묶인다.
- **D5 `FrameTiming` 칸** — UE `FAppTime` 칸 대응 + `frameIndex`·`averageFrameSeconds`. 고정 step은 `Optional<double>` 하나로 둔다. 단위는 double 초(UE와 같다)이고, 고정 delta의 중간 float 반올림(`UEN` 3019)은 따르지 않는다. timecode 칸은 provider가 생길 때 더한다. `EngineTime`을 대체한다.
- **D6 실행 성격 출처** — 명령줄 + 빌드·호스트 기본값만 쓴다. 설정 파일·CVar로는 정하지 않는다. 보기는 콘솔 `app.info`, 설정 기본값 조정은 등록 코드가 Code 층에 쓴다. BootCVar 방식은 검토하고 제외했다. 봉인 전에 필요하고 출시 빌드 세션 층 제한(Config D5)과 부딪히기 때문이다.
- **D7 실행 성격 목록** — `unattended`·`unattendedInput`·`benchmark`(비출시)·`fixedSeed`·`fixedFrameSeconds`·`headless`. `--deterministic`은 고정 step + 고정 시드다. 오디오·단일 스레드는 이름만 예약하고, 설치형 여부는 패키징 단계로 미룬다.
- **D8 benchmark 효과** — 고정 시드, 고정 step(따로 정하지 않으면 1/30), 포커스 늘 true다. 출시 빌드에서는 옵션이 없다(UE와 같다).
- **D9 인스턴스·세션** — `instanceId`는 매 실행 무작위 Guid다(비출시만 `--instance-id`). `sessionId`·`sessionName`은 명령줄로만 정하고, 없으면 단독 실행이다. 세션 소유자·사용자 허가 목록은 두지 않는다.
- **D10 빌드 정보 칸** — 구성·대상 종류·대상 이름·프로젝트 이름·브랜치·커밋·dirty·커밋 시각·빌드 URL·디버그 정보. Horde 실행 job URL·승격 빌드·제품 식별자는 두지 않는다.
- **D11 빌드 날짜 [원칙 확인]** — 빌드 시각(`__DATE__`) 대신 커밋 시각을 쓴다. 재현 빌드를 위해서다. UE와 뜻이 다르다(UE는 컴파일 날짜).
- **D12 BuildStamp와 캐시 키** — 별도 생성 TU에 둔다. `ToolBuildId` 입력·쿠킹 byte·에셋 너겟 키에는 넣지 않는다.
- **D13 그래픽 API 이름** — AppInfo에 두지 않는다. Graphics 장치 정보가 공개하고 시작 로그가 남긴다.
- **D14 시작 로그** — LogHost 설치 직후 `App` 카테고리 Info 기록 한 묶음을 남긴다. 내용은 실행 파일, 엔진 버전, 구성, 대상, 커밋, 브랜치, 플랫폼, PID, instance id, 가린 명령줄이다. 문구는 영어다.
- **D15 프로세스 명령줄** — 진입점에서 한 번 캡처하는 불변 UTF-8 배열이다. Windows는 넓은 문자 원문에서 바꾼다. UE식 `Set`·`Append`가 없다. 하위 프로세스 상속(`Inherit`)은 자식을 띄우는 도구가 생길 때 더한다.
- **D16 비밀 가리기** — `Secret` 지정자 옵션의 값을 `<redacted>`로, 파싱하지 못한 인자를 `<unparsed>`로 바꾼다. 로그·trace·크래시 보고에는 가린 형태만 쓴다.
- **D17 C++ 파서 — 자체 구현(확정)** — 외부 라이브러리는 검토하고 제외했다. 이유는 크기다(사용자 결정 2026-11-18).
- **D18 선언 방식** — 리플렉션 옵션 struct와 MetaCompiler 지정자(13.3절)를 쓰고, 생성 스키마는 FieldSchema와 따로 둔다. 도움말 문자열은 `Help=` 지정자에서 온다. 문서 주석에서 가져오지 않는다(주석은 한국어, 도움말은 영어). 필수 표시는 기존 `Required`와 겹치지 않게 `Mandatory`로 한다.
- **D19 값 변환** — ReflectionText 대입 경로를 다시 쓴다(옵션 = 대입 하나). 전용 수 파서를 두지 않는다. 파서가 작은 TypeRegistry·ObjectRegistry를 호출마다 만든다(비용은 CL2에서 측정).
- **D20 공통 문법 규약** — `--kebab-case`, 한 글자 별칭, `--name value`·`--name=value`, 값 없는 bool, 반복 = 원소 추가(쉼표로 나누지 않음), 스칼라 중복 오류, `--`, 묶음·`/x` 없음, 대소문자 구분, 공용 옵션은 어디서나 받는다.
- **D21 도움말 모양** — System.CommandLine 기본 모양(Description·Usage·Arguments·Options·Commands, `[default: …]`, `(REQUIRED)`)에 Examples 절을 더한다. 터미널 너비에 맞추고, 터미널이 아니면 80이다. stdout, 종료 0.
- **D22 오류 모양과 종료 코드** — 오류는 stderr에 `error: ` 접두로 쓰고, 짧은 사용법 한 줄과 `--help` 안내 한 줄을 붙인다. 종료 코드는 0 성공, 1 실행 실패, 2 사용법 오류다. 이름 제안은 Damerau-Levenshtein ≤ max(1, 길이/3)에 동점이면 사전순이고, 양쪽이 같은 알고리즘을 쓴다. ShaderToolSpike의 64와 벤치의 1·stdout을 이 규약으로 고친다.
- **D23 응답 파일** — 도구·commandlet만 `@file`을 받는다. 문법은 System.CommandLine과 같게 하고, 크기·깊이 상한을 둔다. 게임 호스트는 받지 않는다.
- **D24 commandlet 구조** — `CommandletRegistry`를 Developer `CommandSet`으로 대체한다(하위호환 없음). 명령은 옵션 struct + 실행 함수다. 도움말 목록과 개별 도움말은 등록만으로 생긴다. `--list`는 없앤다.
- **D25 `--set`·`--trace`·`--config`** — 호스트 공용 옵션 struct로 옮겨 같은 파서로 한 번만 읽는다. 값 의미(Config 대입, trace 목록)와 출시 빌드 제한은 그대로다. commandlet에서도 받는다.
- **D26 C# 파서 [범위 큼]** — System.CommandLine을 채택한다(저장소 첫 외부 NuGet 패키지). 중앙 버전 관리와 lock 파일을 쓰고, `RocksCli` 얇은 층으로 오류·제안·예시·`help <명령>`·`--version`을 C++과 맞춘다. 자체 C# 파서는 택하지 않았다.
- **D27 코드 크기** — 7.2절 상한을 둔다. Release 게임 실행 파일에는 도움말 writer와 도움말 문자열을 넣지 않는다.
- **D28 단계 기호** — AP(앱 정보)·CL(명령줄).

## 20. 확인하지 못한 것

- **System.CommandLine 3.0.0 API.** 이 문서의 C# 예시는 Microsoft Learn 시작 문서(2.0 API — `Option<T>{Description, DefaultValueFactory, Required}`·`SetAction`·`GetValue`·`Parse(args).Invoke()`)를 따랐다. 3.0.0에서 바뀐 점은 확인하지 않았다. 착수 때 고정할 판의 API로 고친다.
- **System.CommandLine 도움말 세부.** 필수 표시 문구(`(REQUIRED)`로 보이나 원본에서 확인하지 않았다), Arguments 절 모양, 옵션 순서(문서 예에서는 상위 명령에서 온 옵션과 `--help`의 자리가 추가 순서를 따르는 것으로 보인다), 열 너비 규칙, 출력이 터미널이 아닐 때의 너비, 기본 응답 파일 문법과 기본 켜짐 여부, 기본 오류 문구 전체. C++은 이 문구를 따라가므로 CL3·CL6 착수 때 채택 판에서 확인하고 golden으로 고정한다.
- **ReflectionText enum 입력의 대소문자 규칙과, 파서가 호출마다 TypeRegistry·ObjectRegistry를 만드는 비용.** CL2에서 측정한다. 크기 목표를 넘으면 생성 코드가 필드별 대입 함수를 직접 내는 방식(source generator와 같은 방식)으로 바꾼다.
- **policy가 "외부 NuGet 패키지 0건"을 전제한 검사를 갖는지.** CL5 착수 때 확인한다.
- **Sandbox 같은 `main(char**)` 실행 파일이 Windows에서 한글 인자를 실제로 잃는지.** 코드상 ANSI 코드 페이지 argv이고 UTF-8 manifest를 찾지 못했다. 실행해 보지는 않았다. AP1 시험이 확인한다.
- **엔진 호스트가 worker 없이(`InlineExecutor`만으로) 돌 수 있는지.** `--single-thread`를 열지 정하는 근거다.
- **Android 호스트가 명령줄 대신 받는 인자 경로.**
- **UE 온라인 서버 판정 delegate**(`SetIsServerForOnlineSubsystemsDelegate`)를 엔진 어디서 설정하는지. `Runtime/Engine/Private`에서 찾지 못했다(플러그인으로 보인다). 범위 밖이라 더 찾지 않았다.
- **UE `FApp` 조회 함수의 읽는 스레드 분포.** 대표 호출(3절 표의 파일:줄)과 Runtime 아래 호출 파일 수만 셌고, 호출마다 스레드를 추적하지는 않았다.

## 21. 변경 이력

- **개정 1 초안(2026-11-18)**: UE 5.8 `FApp`·`FAppTime`·`FCommandLine`·`FParse`·`UCommandlet` 조사, Rocks 앱 정보·명령줄 현황(C++·C#), `AppInfo`·`FrameTiming`·`ProcessCommandLine`·빌드 정보 주입, 리플렉션 기반 C++ 명령줄 파서와 System.CommandLine 기반 C# 도구의 공통 규약, 결정 D1–D28, 단계 AP1–AP4·CL1–CL7.
