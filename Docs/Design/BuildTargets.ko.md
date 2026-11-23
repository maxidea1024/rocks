# 빌드 타깃·모듈 의존·플러그인 조건 설계

기준일: 2026-11-23, 개정 25. Rocks.Build에 없는 UE 5.8 빌드 기능 네 가지를 정한다.
- **타깃 종류 확장**: Editor·Client·Server 타깃과, 타깃 종류에 따라 코드·데이터를 컴파일에서 빼는 매크로.
- **모듈 의존 공개 범위**: 의존마다 Public(소비자에게 include·정의를 넘김)과 Private(이 모듈 안에서만 씀)을 나눈다.
- **플러그인 활성 조건**: 프로젝트가 플러그인을 특정 타깃·플랫폼·구성에서만 켠다.
- **링크 방식**: Editor 타깃은 UE와 같이 모듈마다 공유 라이브러리로 링크한다(모듈 API 매크로, 모듈 사이 공유 상태 규칙).

**전용 서버는 필수 요구다**(사용자 요구 2026-11-19). 네트워크가 구성되면 Rocks 게임은 렌더링 없는 전용 서버(dedicated server)로 돌 수 있어야 한다. 서버 플랫폼은 Windows·macOS·Linux다. 구현은 네트워크 단계에서 하지만 빌드 구조가 서버를 받을 수 있도록 스펙은 지금 잡는다. [전체 계획](EngineImplementationPlan.ko.md)도 같은 요구를 담고 있다: 130행 플랫폼 표의 "Linux x64 서버", 103행 "M5 Linux 서버 통합", 2498행 "M4 server service boot/shutdown", 1610행 PC08 "Linux 서버와 각 플랫폼 client". 그래서 이 문서에서 Server 타깃과 Linux 빌드 플랫폼은 선택이 아니라 필수 항목이다(3.7절).

출발점은 UE 5.8 `Templates`(C++ 템플릿 8종의 `*.Target.cs`·`*.Build.cs`, `.uproject` 36개)와 Rocks.Build를 대조한 결과다. C++ 템플릿은 모두 `*Editor.Target.cs`를 가진다. 모듈 규칙은 `PublicDependencyModuleNames`·`PrivateDependencyModuleNames`를 나눠 쓴다. `.uproject` 21개가 `ModelingToolsEditorMode` 플러그인을 참조하고 그중 17개가 `TargetAllowList: ["Editor"]`로 에디터에서만 켠다. `TargetDenyList: ["Server"]`(TP_VehicleAdv·TP_VehicleAdvBP)와 `PlatformAllowList`(TP_UEIntro_BP)도 쓴다.

대상 코드는 `Programs/Rocks.Build`(ActionGraph·Descriptors·DescriptorModules·BuildInfo·NativeCompileProfile·InputValidation), `Programs/Rocks.Build.Abstractions/BuildRules.cs`, `Programs/Rocks.Build.Cli`(Program·MetadataRunner·MetadataLayout·InstalledSdk), `Programs/Rocks.Tooling.Checks`(ReleaseExclusion·PolicyChecks.TargetMacros), `Build/Rules/`(SourceModuleRules·ThirdPartyModuleRules), `Programs/FoundationTests/FoundationTests.Build.cs`다.

UE 근거는 UE 5.8.4(`release`, 로컬 클론 `C:\github\UnrealEngine`, commit `f45729c0b6f0`)다. UBT 경로는 `Engine/Source/Programs/UnrealBuildTool/` 기준이다(약어 `UBT/`). UE 이름은 대응표와 근거에만 적는다. 레거시·deprecated 경로(`[Obsolete]` `bBuildEditor`, `BuildSettingsVersion`, `IncludeOrderVersion`)는 따르지 않는다.

상태: **설계 초안**(결정 BD1–BD31, BD17은 확정(사용자 결정 — UE 방식), BD18은 확정(요구), 나머지는 권장안). 구현 단계는 BS0–BS8(6절)이며 착수 시점은 사용자가 정한다. 초안 작성 뒤 서브에이전트 3개로 교차 검증하고, 반영본을 서브에이전트 1개로 다시 검증했다(10절). PCH·유니티 빌드는 [유니티 빌드·PCH 설계](UnityBuildPch.ko.md)가 맡으며 이 문서의 범위가 아니다.

## 1. 현재 상태

| 항목 | 현재 Rocks.Build | 문제 |
|---|---|---|
| 타깃 종류 | `TargetType { Game, Program }`(`BuildRules.cs:43`) | 에디터(Rocks Studio — [UI 툴킷](UiToolkit.ko.md) 6절)와 전용 서버를 전제한 설계가 있는데 받을 타깃이 없다 |
| 에디터 전용 코드·데이터 | `EditorOnly`는 쿠킹 저장·로드 정책(`FieldEncodePolicy::excludedFlags`)의 데이터 플래그이고 "항상 컴파일된다"([리플렉션 기반](ReflectionFoundations.ko.md) 134행) | 게임 빌드에서도 필드가 메모리 배치에 남는다. 에디터 전용 함수·코드를 컴파일에서 뺄 수단이 없다 |
| 모듈 type | 기술자 `type`(Runtime·Developer·Editor)은 게임·플러그인 모듈에만 있고 `Editor`는 "에디터 타깃이 아직 없음"으로 항상 뺀다([저장소 구성](../Guides/RepositoryLayout.ko.md) 게임 빌드 연동). 엔진 모듈은 `SourceModuleRules.Group` 폴더뿐이다 | 엔진 모듈을 서버·클라이언트 전용으로 표시할 수 없다 |
| Release 배제 정책 | `ReleaseExclusion.cs:90`이 `Type == Game` 타깃만 검사한다 | Client·Server 타깃이 생기면 빠진다 |
| 의존 | (BS0에서 해소 — 3.5절 두 목록) `NativeModuleInput.Dependencies` 하나(`NativeBuildModels.cs:216`), 규칙 기반 클래스는 `RequiredModules` 하나 | `ActionGraph.cs:138-143`이 도달 집합 **전체**의 public include·public define을 넘긴다. 모든 의존이 사실상 Public이라 include가 새어 나간다. [전체 계획](EngineImplementationPlan.ko.md) 282행은 "공개/비공개 의존성" 기록을 요구한다 |
| 서드파티 우회 | `ThirdPartyModuleRules`는 public include를 비우고, 소비자가 `ConsumerIncludeDirectories`·`ConsumerDefines`를 private으로 받는다(`Build/Rules/ThirdPartyModuleRules.cs:18-21, 76-90`, `FoundationTests.Build.cs:85-88`) | private 의존이 없어서 생긴 우회다 |
| 공개 헤더 검사 | `CheckPublicHeaders`가 타깃 도달 모듈 **전체**의 public include를 합친 환경에서 각 공개 헤더를 컴파일한다(`Rocks.Build.Cli/Program.cs:170-213`) | 공개 헤더가 의존을 선언하지 않은 모듈의 헤더를 include해도 잡지 못한다 |
| 리플렉션 메타데이터 | `MetadataRunner.TransitiveDependencies`(`MetadataRunner.cs:262`)·`MetadataLayout`의 도달 집합·include 순서·`DependsOn`(`MetadataLayout.cs:138, 156-183, 395-406`)이 도달 집합 전체를 쓴다 | 의존 범위와 맞물려야 한다(3.5절) |
| 프로젝트의 플러그인 참조 | `PluginReference(Name, Enabled)`(`Descriptors.cs:49`) | 타깃·플랫폼·구성 조건을 적을 자리가 없다 |
| 링크 | 정적 라이브러리 + 실행 파일, DLL 없음. 모듈 API export 매크로·모듈 수명 객체·로더가 없다 | Editor 타깃은 UE와 같이 모듈식으로 링크한다(3.8절, BD17) |
| 빌드 플랫폼 | `NativePlatform { Windows, MacOS, Android }`(`NativeBuildModels.cs:4`), `BuildContext.PlatformDirectory`는 셋 외에는 예외 | 전용 서버의 운영 플랫폼인 Linux x64를 빌드할 수 없다 |
| 서버 실행 | 실행 옵션 `--headless`(창·그래픽 장치를 만들지 않음, [AppInfo·CommandLine](AppInfoCommandLine.ko.md) 379·548행)만 있다 | 서버 실행 파일을 만들 타깃이 없다 |

## 2. 용어 대응

| Rocks | UE 5.8 대응 | 뜻 |
|---|---|---|
| 타깃 종류 `TargetType` | `TargetType`(`UBT/Configuration/Rules/TargetRules.cs:21-47`) | Game·Editor·Client·Server·Program |
| 에디터 코드 매크로 `ROCKS_WITH_EDITOR` | `WITH_EDITOR` | 에디터 기능 코드(편집 hook·편집기 전용 함수)를 컴파일하는지 |
| 에디터 데이터 매크로 `ROCKS_WITH_EDITOR_DATA` | `WITH_EDITORONLY_DATA` | 에디터 전용 필드를 메모리 배치에 두는지 |
| 서버·클라이언트 코드 매크로 `ROCKS_WITH_SERVER_CODE`·`ROCKS_WITH_CLIENT_CODE` | `WITH_SERVER_CODE`·`WITH_CLIENT_CODE` | 서버 전용·클라이언트 전용 코드를 컴파일하는지 |
| 모듈 type | `ModuleHostType`(`UBT/Configuration/Descriptors/ModuleDescriptor.cs:785-810`) | 어느 타깃에 들어갈 수 있는 모듈인지 |
| 공개 의존 `PublicDependencies` | `PublicDependencyModuleNames` | 이 모듈의 공개 헤더가 쓰는 모듈. 소비자에게 include·정의가 넘어간다 |
| 비공개 의존 `PrivateDependencies` | `PrivateDependencyModuleNames` | 이 모듈의 구현만 쓰는 모듈 |
| 공개 환경 | `FindModulesInPublicCompileEnvironment`(`UBT/Configuration/UEBuildModule.cs:627-647`) | 모듈 자신 + 공개 의존을 따라 닿는 모듈의 public include·define |
| 컴파일 환경 | `FindModulesInPrivateCompileEnvironment`(같은 파일 607–621행) | 공개 환경 + private include·define + 비공개 의존의 공개 환경 |
| 플러그인 활성 조건 | `PluginReferenceDescriptor`의 `*AllowList`(`UBT/Configuration/Descriptors/PluginReferenceDescriptor.cs:50-75`) | 프로젝트 기술자의 플러그인 참조에 붙는 타깃·플랫폼·구성 목록 |

## 3. 설계

### 3.1 타깃 종류

`TargetType`을 `{ Game, Editor, Client, Server, Program }`으로 넓힌다(BD1). UE와 같은 다섯 종류다.

| 종류 | 용도 | 들어갈 수 있는 모듈 type(3.3절) | 플랫폼 |
|---|---|---|---|
| Game | 출시 실행 파일(클라이언트·서버 겸용, listen server 포함) | Runtime·ServerOnly·ClientOnly, Release가 아니면 Developer | Windows·macOS·Android |
| Client | 서버 코드를 뺀 출시 실행 파일 | Runtime·ClientOnly, Release가 아니면 Developer | Windows·macOS·Android |
| Server | 클라이언트 코드를 뺀 전용 서버 실행 파일 | Runtime·ServerOnly, Release가 아니면 Developer | Windows·macOS·Linux |
| Editor | Rocks Studio(프로세스 안 에디터) | 모든 type(구성과 무관) | Windows·macOS |
| Program | 도구·시험·벤치마크·commandlet | Runtime·ServerOnly·ClientOnly·Developer(구성과 무관). `CompileWithEditor`면 Editor도 | 모든 플랫폼 |

- UE 근거: Game은 "Cooked monolithic game executable", Client는 "no server code", Server는 "no client code", Editor는 "Uncooked modular editor"다(`TargetRules.cs:24-45`). 모듈 포함 규칙은 `ModuleDescriptor.cs:785-810`이다. `ServerOnly`는 Program·Client가 아니면, `ClientOnly`는 Program·Server가 아니면 포함한다. `Editor`는 Editor 타깃만, `Developer`는 Editor·Program만, `DeveloperTool`은 `bBuildDeveloperTools`일 때 포함한다.
- 플랫폼 열은 3.7절과 [전체 계획](EngineImplementationPlan.ko.md) 130행(Windows·macOS 에디터, Android·iOS 클라이언트, Linux 서버·기반 시험)을 따른다. 표 밖의 조합은 빌드 규칙이 이유와 함께 거부한다.
- **`CompileWithEditor`**: Program 타깃 속성이다(UE `bCompileAgainstEditor`, `TargetRules.cs:1395-1400`). 켜면 `ROCKS_WITH_EDITOR=1`·`ROCKS_WITH_EDITOR_DATA=1`이 되고 Editor 모듈을 링크할 수 있다. 에디터 모듈을 쓰는 commandlet용이다. 기본은 꺼짐이다.
- **`CompanionTargets`**: 모든 타깃 속성이다(개정 24, [셰이더 시스템](ShaderSystem.ko.md) 11.1절 SC14 — UE 에디터가 ShaderCompileWorker를 함께 빌드하는 `bEditorDependsOnShaderCompileWorker` 대응, `VCProject.cs:2254`). 함께 빌드해 같은 출력 폴더에 두는 프로그램 타깃 이름 목록이다. 빌드는 동반 타깃의 모듈을 이 타깃의 모듈 뒤에 더해 같은 구성으로 한 번에 빌드한다(`RuleLoader.WithCompanions`). 동반 타깃은 Program·Monolithic이고 플러그인·동반 타깃이 없어야 하며, 이 타깃도 Monolithic이어야 한다(Modular 동반은 모듈 manifest와 함께 후속). 지금은 EngineTests가 `ShaderCompileWorker`를 동반한다.
- **`WithEditorData`**: Program 타깃 속성이다. 켜면 `ROCKS_WITH_EDITOR_DATA=1`이다. 에디터 데이터를 읽는 에셋 도구용이다. 기본은 꺼짐이다(BD19). UE는 Program 기본값이 1이지만(`TargetRules.cs:1201-1203`) 시험 타깃은 0이다(`UBT/Configuration/Rules/TestTargetRules.cs:316-317`). Rocks Program의 대부분이 시험·벤치마크라 0을 기본으로 한다. 이렇게 하면 Program의 기본 매크로 조합이 Game과 같아 설치 SDK 라이브러리를 공유한다(3.2절).
- **Server 타깃의 기능 축소**(렌더·오디오·입력 backend 제외, UE `USE_NULL_RHI=1` — `TargetRules.cs:3492-3493`)의 범위는 네트워크 단계 스펙에서 정한다. 이 문서는 그 결정이 기댈 type·매크로·검사를 준비한다(3.7절). 오디오 모듈의 범위는 [오디오 시스템](AudioSystem.ko.md) AD24가 정했다(개정 20): 장치 backend `AudioMiniaudio`와 디코더 `AudioOpus`는 `ClientOnly`, `Audio`·`EngineAudio`는 `Runtime`(서버에서는 재생 요청이 `Ignored`)이고, Server 쿠킹은 오디오 payload를 넣지 않는다.
- **빌드 주입 상수**: [AppInfo·CommandLine](AppInfoCommandLine.ko.md)이 정한 `build::TargetType`(565행, 아직 미구현)을 같은 다섯 값으로 넓힌다. 실행 중 분기는 `build::Description()` 상수로 한다. UE의 `UE_GAME`·`UE_EDITOR`·`UE_SERVER`·`UE_CLIENT`·`IS_PROGRAM` 같은 타깃 종류 매크로는 두지 않는다(BD3). 실행 종류 `HostKind`(같은 문서 304행 — "에디터·서버는 생길 때 더한다")는 타깃 종류와 다른 축이다. Editor·Server 타깃이 생길 때 `HostKind`에 `Editor`·`Server`를 더한다.

### 3.2 타깃 종류 매크로

모든 타깃의 모든 모듈에 네 매크로를 0 또는 1로 정의한다(BD2).

| 매크로 | Game | Client | Server | Editor | Program | UE 근거 |
|---|---|---|---|---|---|---|
| `ROCKS_WITH_EDITOR` | 0 | 0 | 0 | 1 | `CompileWithEditor`면 1 | `UBT/Configuration/UEBuildTarget.cs:6549-6557` |
| `ROCKS_WITH_EDITOR_DATA` | 0 | 0 | 0 | 1 | `CompileWithEditor`·`WithEditorData`면 1 | `TargetRules.cs:1201-1203`, `UEBuildTarget.cs:6559-6562`(=0만 정의하고 1은 `Runtime/Core/Public/Misc/CoreMiscDefines.h:21-26`이 기본으로 둔다) |
| `ROCKS_WITH_SERVER_CODE` | 1 | 0 | 1 | 1 | 1 | `TargetRules.cs:1496-1498`, `UEBuildTarget.cs:6567-6568` |
| `ROCKS_WITH_CLIENT_CODE` | 1 | 1 | 0 | 1 | 1 | `TargetRules.cs:1485-1487`, `UEBuildTarget.cs:6564-6565` |

- **타깃 전체에 같은 값이다.** 모듈별로 다르게 정의하면 같은 클래스의 배치가 TU마다 달라지는 ODR 위반이므로 빌드 검사가 거부한다(로그 하한 `ROCKS_LOG_COMPILED_MINIMUM`과 같은 규칙).
- **값은 타깃 종류로만 정한다.** UE는 `bWithServerCode`·`bWithClientCode`를 타깃이 재정의할 수 있지만(같은 행의 `…Override`) Rocks는 두지 않는다. 조합 수가 늘면 SDK 변형과 시험 행렬이 늘어나고, 서버·클라이언트 코드를 함께 쓰는 실행 파일은 Game이다.
- `ROCKS_WITH_EDITOR=1`이면 `ROCKS_WITH_EDITOR_DATA=1`이어야 한다(에디터 코드는 에디터 데이터를 읽는다). 반대는 허용한다.
- **검사**: `Base/Compiler.h`가 네 매크로의 정의 여부와 값(0 또는 1)을 검사하고, 정의가 없으면 `#error`다(메시지는 영어). 플랫폼 매크로는 없으면 0으로 두고 합만 검사하지만, 이 네 매크로는 기본값을 두지 않는다. 기본값이 있으면 매크로를 넘기지 않은 컴파일 경로(도구 시험 등)가 조용히 다른 조합으로 컴파일되기 때문이다.
- **주입 위치**: 구성·플랫폼 매크로와 같이 명령줄로 넘긴다([유니티 빌드·PCH](UnityBuildPch.ko.md) UD9 뒤에도 명령줄). 지금 구성·플랫폼 매크로는 `NativeCompileProfile`·`ActionGraph.CompileFlags`(인자에 타깃 종류가 없다)가 만들고 AST 생성(MetadataRunner)과 공유한다. 바꿀 곳은 다음과 같다(BS2).
  - `NativeBuildRequest`에 타깃 종류와 Program 속성을 더하고, `CompileFlags`가 받는다.
  - MetadataRunner의 AST 생성이 같은 매크로를 받는다.
  - `Rocks.Build.Cli/Program.cs:308-323`의 도구 시험(`native`·`meta` 검사) 호출이 구성·플랫폼 매크로와 함께 넘긴다.
  - `InputValidation.cs:163`의 예약 매크로 접두에 `ROCKS_WITH_`를 더한다(모듈 정의로 덮어쓰기 금지).
  - 정책 `PolicyChecks.TargetMacros.cs`의 `target-macro-defined`·`target-macro-order` 검사를 새 매크로로 넓힌다.
- **설치 SDK(BD5)**: 엔진 라이브러리는 매크로 조합마다 미리 빌드해야 한다. 변형 key는 타깃 종류가 아니라 **매크로 조합**이다. 조합은 다음과 같다.
  - Game과 Program 기본은 같은 조합이다.
  - Program+`CompileWithEditor`는 Editor와 같은 매크로 조합이지만 링크 방식은 기본 `Monolithic`이다(UE도 `LinkType` 기본값은 Editor 타깃만 `Modular`다). 그래서 변형 key는 매크로 조합과 링크 방식(3.8절)의 쌍이고, 이 Program은 Editor 변형(`Modular`)을 쓰지 못한다. 1차 SDK는 이 조합의 `Monolithic` 변형을 싣지 않으며, 게임 프로젝트의 이런 Program은 `LinkType = Modular`로 Editor 변형을 쓴다.
  - 나머지는 Client, Server, Program+`WithEditorData`다.

  1차 SDK는 Game 조합만 싣는다. 그래서 게임 프로젝트의 Game 타깃과, 기본 Program 타깃(도구·시험)을 빌드할 수 있다. SDK에 없는 조합의 타깃을 빌드하면 이유와 함께 오류다. 변형을 더하면 SDK manifest `ManifestVersion`을 올린다. 지금 IntegrationTests가 SDK로 내보내 게임이 쓰는 `ReflectionFixturesTarget`(Program, `Foundation.Target.cs`)은 BD19 덕분에 Game 조합과 같다.

### 3.3 모듈 type

type 값은 `{ Runtime, Developer, Editor, ServerOnly, ClientOnly }`다(BD4). 들어갈 수 있는 타깃은 3.1절 표다.

| type | UE 5.8 대응 | 비고 |
|---|---|---|
| Runtime | `RuntimeAndProgram` | Rocks는 Program에도 넣는다(UE `Runtime`은 Program에서 빠진다) |
| Developer | `DeveloperTool` | 지금과 같다. UE `Developer`(Editor·Program 전용)는 두지 않는다 |
| Editor | `Editor` + Program의 `CompileWithEditor` | UE `EditorAndProgram`은 모든 Program에 들어가지만 Rocks는 `CompileWithEditor` Program에만 들어간다 |
| ServerOnly | `ServerOnly` | Client에서 뺀다. Program에는 들어간다(서버 시험) |
| ClientOnly | `ClientOnly` | Server에서 뺀다. Program에는 들어간다 |

**type을 정하는 곳**:
- 게임·플러그인 모듈: 기술자 `modules[].type`(지금과 같다). 이번 타깃에 들어갈 수 없는 모듈은 빌드에서 뺀다(지금 게임 빌드 연동 4단계).
- 엔진 모듈(Core·Engine): 규칙 기반 클래스 `SourceModuleRules`에 `ModuleType` 속성을 더한다. 기본값은 `Group` 폴더에서 온다(`Runtime`→Runtime, `Developer`→Developer, `Editor`→Editor). `ServerOnly`·`ClientOnly`는 규칙이 명시한다. 엔진 모듈은 **걸러 내지 않고 검사만 한다.** 타깃 도달 집합에 들어갈 수 없는 type의 모듈이 있으면 빌드 오류다. UE도 엔진 모듈은 기술자 type으로 걸러 내지 않고 타깃 참조로 정해진다. Rocks는 거기에 검사를 더한다.
- 설치 SDK manifest의 `SdkModule`에 type을 싣는다. 게임 빌드가 같은 검사를 한다.

**type 사이 의존 규칙(BD25)**: 모듈 A가 모듈 B에 의존하려면, A가 들어갈 수 있는 (타깃 종류, 구성) 집합이 B의 집합에 포함되어야 한다. 그렇지 않으면 A를 넣는 타깃에서 B가 빠져 빌드가 깨진다(예: Runtime 모듈이 ClientOnly 모듈에 의존하면 모든 Server 타깃이 실패한다). 3.1절 표에서 나오는 결과는 다음과 같다. 위반은 타깃과 무관하게 규칙 로드 때 오류다. 의존에는 실행 중 적재 관계(`DynamicModules` — [모듈 로더](ModuleLoader.ko.md) 3.8절)도 든다. Monolithic이 그 모듈을 링크하기 때문이다(UE도 동적 적재 모듈을 같은 검사에 넣는다).

| 모듈 type | 의존할 수 있는 type |
|---|---|
| Runtime | Runtime |
| Developer | Runtime·Developer |
| ClientOnly | Runtime·ClientOnly |
| ServerOnly | Runtime·ServerOnly |
| Editor | 모든 type |

**Release 배제 정책(BD20)**: `ReleaseExclusion`의 검사 대상을 `Type == Game`에서 Game·Client·Server로 넓힌다. UE도 Editor·Program이 아닌 모든 타깃의 Shipping을 같은 대상 범위로 본다(`UEBuildTarget.cs:3918` `CheckForEULAViolation` — 검사 내용은 NonRedist 라이선스로 다르다). 평가 플랫폼은 타깃 종류별이다: Game·Client는 Windows x64·macOS arm64·Android arm64, Server는 Windows x64·macOS arm64·Linux x64(BS6 뒤) Release 규칙이다. 그리고 `<계층>/Source/Editor/` 모듈(type Editor)이 Game·Client·Server 타깃에 닿는 것은 구성과 무관하게 정책 오류로 막는다. [셰이더 시스템](ShaderSystem.ko.md) 9절의 Release 배제 대상("Type == Game")도 Game·Client·Server로 읽는다.

채택하지 않은 UE type은 9.1절이다.

### 3.4 에디터 전용 데이터와 메타 컴파일

**선언 규칙(BD6)**: UHT처럼 어휘 규칙을 둔다.
- 리플렉션 선언(`ROCKS_CLASS`·`ROCKS_STRUCT`·`ROCKS_ENUM`·`ROCKS_ENUMERATOR`·`ROCKS_PROPERTY`·`ROCKS_FUNCTION` 등)은 원칙적으로 전처리 조건 블록 밖에만 쓴다. 예외는 둘이다.
  - `ROCKS_PROPERTY`는 `#if ROCKS_WITH_EDITOR_DATA` 블록 안에 쓸 수 있고, 이때 `EditorOnly`를 명시해야 한다.
  - `ROCKS_FUNCTION`은 `#if ROCKS_WITH_EDITOR` 블록 안에 쓸 수 있고, 이때 함수 지정자 `EditorOnly`를 명시해야 한다. UE도 `WITH_EDITOR` 안의 UFUNCTION을 허용하고 EditorOnly 플래그를 붙인다(`EpicGames.UHT/Parsers/UhtHeaderFileParser.cs:123`, `EpicGames.UHT/Parsers/UhtFunctionParser.cs:150`).
- 타입 선언(`ROCKS_CLASS`·`ROCKS_STRUCT`·`ROCKS_ENUM`)과 열거자는 어떤 조건 블록 안에도 둘 수 없다. 그래서 타입 집합은 모든 조합에서 같다.
- 다른 조건 블록(`ROCKS_WITH_SERVER_CODE`·`ROCKS_WITH_CLIENT_CODE`·플랫폼·구성·간접 매크로 등) 안의 리플렉션 선언은 메타 컴파일러 오류다. Client와 Server, 플랫폼 사이에 스키마가 갈라지지 않게 하기 위해서다.
- 양방향 일치: 블록 안의 필드·함수는 `EditorOnly`를 명시해야 하고, `EditorOnly` 필드·함수는 그 블록 안에 있어야 한다. UHT는 블록 밖 리플렉션 선언을 거부하고(`UhtHeaderFileParser.cs:731-736`), `WITH_EDITORONLY_DATA` 안의 필드에 EditorOnly를 자동으로 붙인다(`EpicGames.UHT/Parsers/UhtPropertyParser.cs:649-669`). Rocks는 자동으로 붙이지 않고 명시와 위치를 맞춘다.
- 이에 따라 "항상 컴파일되고 플래그만 다는" 지금의 `EditorOnly`([리플렉션 기반](ReflectionFoundations.ko.md) 134행)는 없어진다. 기존 fixture의 블록 밖 `EditorOnly` 필드(`ReflectionFixture.h`, `CombinationAssets.h` 등)는 구현 때 블록 안으로 옮긴다(하위 호환은 두지 않는다). 중첩 struct·배열 안 값에 대한 유효 플래그 전파([리플렉션 기반](ReflectionFoundations.ko.md) 6.1절)는 그대로다. 전파는 저장 정책이고, 이 규칙은 선언 위치 규칙이다.

**판정 수단**:
- 지금 메타 컴파일러는 Clang `-ast-dump=json`만 쓰고(`Programs/Rocks.MetaCompiler/CompilerDriver.cs:36-37`), 이 출력에는 조건 블록 정보가 없다. 그래서 UHT처럼 메타 컴파일러가 리플렉션 헤더를 **어휘 스캔**한다.
- 스캔은 `#if`·`#ifdef`·`#ifndef`·`#elif`·`#else`·`#endif` 중첩을 따라가며, 각 리플렉션 매크로를 감싼 조건 블록의 조건식 철자를 본다. 허용되는 조건은 정확히 `#if ROCKS_WITH_EDITOR_DATA`(필드)와 `#if ROCKS_WITH_EDITOR`(함수) 두 철자다. 그 밖의 조건식(간접 매크로 `#if X` 포함)과 `#else`·`#elif` 가지 안의 리플렉션 선언은 오류다.
- 리플렉션 선언은 빌드가 찾는 리플렉션 헤더(표지 헤더 — [리플렉션·빌드 보강](ReflectionFoundations.ko.md) 1.10절)에만 쓸 수 있으므로(정책 `reflection-annotation-unscanned` 등), include를 거쳐 다른 파일에 숨은 선언은 이미 거부된다.

**두 변형 비교(보강 검사)**: 메타 컴파일러는 모든 빌드에서 리플렉션 헤더의 AST를 에디터 조합(`ROCKS_WITH_EDITOR=1`, `ROCKS_WITH_EDITOR_DATA=1`)과 게임 조합(둘 다 0)으로 만든다. 두 결과의 차이는 정확히 `EditorOnly` 필드·함수여야 한다. 차이가 없어야 하는 것은 타입 집합, 기반 타입, 나머지 필드의 순서·형식·지정자, enum 값이다. 어휘 스캔을 빠져나가는 경우를 잡는 방어선이다. 서버·클라이언트·플랫폼 조건은 어휘 스캔이 막으므로 비교 축에 넣지 않는다.

**생성 코드**: 생성 헤더·등록 표·offset 표의 `EditorOnly` 필드·함수 항목은 같은 조건(`ROCKS_WITH_EDITOR_DATA`·`ROCKS_WITH_EDITOR`)으로 감싼다(UE `IF_WITH_EDITORONLY_DATA` — `EpicGames.UHT/Exporters/CodeGen/UhtUsedDefineScopes.cs:611-614`). [UI 툴킷](UiToolkit.ko.md) 7.3절의 편집 메타데이터 블록도 `ROCKS_WITH_EDITOR_DATA`로 생성한다.

**descriptor와 안정 ID**:
- 메타 컴파일러가 내는 descriptor(`.rocksmeta.json`)와 `.rocksids.json`은 두 변형의 합집합이다. `EditorOnly` 필드·함수도 담고 표시한다.
- `ids verify`는 합집합으로 검사하므로, 게임 조합에 없는 필드·함수를 "사라진 것"으로 보지 않는다. 설치 SDK도 합집합 descriptor를 싣는다.

**로드와 저장(BD26)**:
- `ROCKS_WITH_EDITOR_DATA=0` 빌드의 레지스트리는 타입마다 `EditorOnly` 필드의 FieldId 목록(ID만, 배치 정보 없음)을 가진다.
- 이 빌드가 에디터가 저장한 패키지를 읽으면, 목록에 있는 FieldId의 값은 건너뛴다. 목록에 없는 모르는 FieldId는 지금 규칙을 따른다. 쿠킹 데이터 로드(`excludedFlags`)는 지금과 같다.
- **`ROCKS_WITH_EDITOR_DATA=0` 빌드는 에디터 패키지(쿠킹하지 않은 패키지)를 저장하지 않는다.** 저장 API가 Unsupported로 거부한다. 건너뛴 `EditorOnly` 값을 잃기 때문이다. 에디터 패키지를 쓰는 에셋 도구는 `WithEditorData` Program이어야 한다.
- UE는 Game·Client·Server가 쿠킹 데이터만 읽는다(`bBuildRequiresCookedData`, `TargetRules.cs:1190-1195`). Rocks는 이 축을 채택하지 않는다(9.1절). Development Game이 에디터 패키지를 바로 읽는 개발 흐름을 유지하기 위해서다.

**시험**: `ROCKS_WITH_EDITOR_DATA=0` 조합(Game·기본 Program)과 1 조합(`WithEditorData` Program) 모두 시험 빌드를 둔다(BS2).

### 3.5 모듈 의존 공개 범위

`NativeModuleInput.Dependencies`를 `PublicDependencies`·`PrivateDependencies` 두 목록으로 바꾼다(BD7). 하위 호환 이름은 두지 않는다. 규칙 기반 클래스의 `RequiredModules`도 같은 두 속성으로 바꾼다.

**컴파일 환경**(UE `UEBuildModule.cs:607-647`과 같은 규칙):
- 모듈 M의 **공개 환경** = M의 public include·public define + M의 각 공개 의존 D의 공개 환경(재귀).
- 모듈 M의 **컴파일 환경** = M의 공개 환경 + M의 private include·private define + M의 각 비공개 의존 D의 공개 환경.
- 비공개 의존의 공개 의존은 M에게 보이지만, M의 소비자에게는 보이지 않는다.
- `AdditionalInputs`(compile key 입력)도 같은 규칙으로 전파한다.

**링크**: 정적 링크이므로 실행 파일은 두 목록을 모두 따라간 도달 집합 전체를 링크한다. `ActionGraph`의 `Closures`(BuildInfo 도구 빌드 ID 입력)와 순환 검사(`ActionGraph.cs:80`)도 두 목록을 모두 따라간다. UE도 비공개 의존을 링크한다(`UEBuildModule.cs:999-1020` `SetupPrivateLinkEnvironment`).

**리플렉션 메타데이터(BD8)**: 메타 컴파일러가 모듈 M을 처리할 때 쓰는 모듈 범위는 M의 컴파일 환경에 보이는 모듈과 같다. 바꿀 곳은 다음과 같다.
- import 범위(`MetadataRunner.cs:46, 262`)
- AST include 순서(`MetadataLayout.cs:156-183`)
- 생성 헤더 해석 `DependsOn`(`MetadataLayout.cs:395-406` — "도달"이 아니라 "보임")
- 전파 define(`MetadataRunner.cs:131`)

공개 헤더의 리플렉션 타입이 다른 모듈의 리플렉션 타입을 기반·필드로 쓰면, 그 모듈은 공개 의존이어야 한다. 이 조건은 공개 헤더 검사가 함께 잡는다.

**공개 헤더 검사(BD9)**: `CheckPublicHeaders`는 모듈마다 **그 모듈의 공개 환경만으로** 각 공개 헤더를 컴파일한다.

**include-only 의존은 두지 않는다**(BD10, UE `PublicIncludePathModuleNames`·`PrivateIncludePathModuleNames`). UE에서는 순환 의존을 끊는 수단이다(`UEBuildModule.cs:1382` 안내 문구). Rocks는 계층 규칙으로 순환을 금지하고, header-only 모듈은 일반 의존으로 쓴다.

**정책 검사(BD11)**:
- 모듈의 소스·헤더가 include한 Rocks 헤더는 컴파일 환경에 보이는 모듈의 것이어야 한다. 판정은 컴파일러가 낸 의존 기록(depfile)의 실제 경로로 소유 모듈을 찾아서 한다. include 디렉터리 이름만으로는 같은 상대 경로가 여러 모듈에서 보일 수 있기 때문이다. depfile 파서는 [유니티 빌드·PCH](UnityBuildPch.ko.md) UB0의 것을 쓴다. 그래서 BS1은 UB0의 depfile 파서 부분 뒤다(유니티·PCH 본체와는 무관하다).
- 한 모듈을 두 목록에 같이 적으면 오류다.
- 공개 헤더가 쓰지 않는 공개 의존은 진단한다. 판정은 공개 헤더 검사의 depfile로 한다.

**서드파티 모듈(BD21)**: private 의존이 생기면 UE 방식으로 바꾼다. 서드파티 모듈은 자기 include·define을 public으로 내고, 소비 모듈은 `PrivateDependencies`로 쓴다. `ConsumerIncludeDirectories`·`ConsumerDefines` 우회는 없앤다(BS3). "엔진 공개 헤더는 서드파티 헤더를 include하지 않는다"([저장소 구성](../Guides/RepositoryLayout.ko.md) "외부 헤더는 소비 모듈의 Private 구현에만 노출") 규칙은 공개 헤더 검사(BD9)가 자동으로 잡는다.

**시험 실행 파일(BD22)**: 시험 모듈(`FoundationTests`·`EngineTests`)의 각 `<Module>/Tests/` 소스는 시험 대상 모듈의 컴파일 환경 전체(비공개 의존 포함)와 그 모듈의 `Private/` 폴더를 받는다. 지금 `NativeModuleInput.Sources`는 문자열 목록이라(`NativeBuildModels.cs:206`) 소스마다 환경을 줄 수 없으므로 `NativeModuleInput`에 **소스 묶음** `SourceGroups`를 더한다. 묶음은 (소스 목록, 추가 비공개 의존, 추가 private include 디렉터리)이고, 묶음의 소스는 모듈 컴파일 환경에 묶음의 추가분을 더한 환경으로 컴파일한다. 시험 규칙은 대상 모듈마다 묶음 하나를 만든다(추가 비공개 의존 = 대상 모듈과 그 두 의존 목록, 추가 include = 대상 모듈 `Private/`). 지금 시험 실행 파일이 상위 모듈만 나열하고 하위 의존 전파에 기대며(`FoundationTests.Build.cs`의 "lower dependencies propagate"), `Core/Source/Runtime` 루트 include로 비공개 헤더를 여는 것(83–85행)을 이 규칙으로 바꾼다. BD11의 소유 모듈 판정은 묶음 환경을 쓴다. 정적 라이브러리로 나누지 않으므로 시험 등록(정적 초기화)이 링커에 의해 빠지는 문제가 없다. [유니티 빌드·PCH](UnityBuildPch.ko.md)의 묶음 TU는 소스 묶음 경계를 넘지 않는다(환경이 다른 소스를 한 TU로 묶을 수 없다).

**설치 SDK**: `SdkModule`(`InstalledSdk.cs:14`)의 `Dependencies`를 두 목록으로 바꾼다(`Reachable` `:49`, 검증 `:455-469`도 같다). SDK 게임 빌드는 같은 규칙으로 컴파일 환경을 만든다.

**그 밖의 소비처**: `DescriptorModules.cs:170`(빠진 모듈 참조 검사)과 `ReleaseExclusion.cs:124`(도달 검사)는 두 목록을 모두 따라간다.

**전환**: 기존 모듈은 먼저 모두 공개 의존으로 옮긴다(동작 불변, BS0). 그 뒤 공개 헤더 검사와 진단으로 공개 헤더가 쓰지 않는 의존을 비공개로 내린다(BS3).

**BS0 구현 세부(개정 5).**
- `NativeModuleInput.PublicDependencies`·`PrivateDependencies`(문자열 목록)와 둘을 차례로 잇는 읽기 전용 `AllDependencies`(공개 다음 비공개)를 둔다. `Dependencies`는 없앤다. 규칙 기반 클래스(`SourceModuleRules`)는 추상 `PublicDependencies`와 기본이 빈 `PrivateDependencies`를 둔다(`RequiredModules`는 없앤다). 다른 규칙(Program·서드파티·게임 템플릿)은 `NativeModuleInput`을 직접 채운다.
- 보임(컴파일 환경)은 Rocks.Build `ModuleVisibility.Of`(대상 모듈은 두 목록, 그 밖은 공개 목록만 따라가는 의존 우선 후위 순서)로 한 곳에서 계산한다. 쓰는 곳: 액션 그래프의 include·정의·`AdditionalInputs`·공유 PCH 후보, 메타데이터의 AST include·전파 정의·생성 헤더 `DependsOn`. 공유 PCH 제공 모듈의 closure(포함 검사·공유 정의·후보 순위)는 그 제공 모듈의 공개 환경(`ModuleVisibility.PublicClosure` — 자기와 공개 목록만)이다(리뷰 반영).
- 메타데이터 import 범위(BD8 개정)는 보임이 아니라 도달(두 목록)이다. 메타 컴파일러는 import한 descriptor의 타입을 모두 잇는데(그 모듈의 private 타입 포함), 그 타입이 비공개 의존 모듈의 타입을 쓰면 그 descriptor도 있어야 집합이 닫힌다. import는 타입 정보이지 헤더 노출이 아니므로 공개 범위를 넓히지 않는다. 버린 안: 메타 컴파일러가 다른 모듈의 private 타입을 잇지 않고 건너뛰게(다른 모듈 private 타입의 해석 규칙이 새로 필요하다).
- 공개 헤더 검사(`PublicHeaderChecks`)는 아직 도달한 라이브러리를 모두 공개 의존으로 두는 검사다(BD9 아님). 그래서 BS1 전에는 의존을 비공개로 내리지 않는다(BS0 → BS1 → BS3 순서). 시험 실행 파일의 하위 의존 전파(BD22)도 같다. 순서는 공개 목록을 먼저 따라가므로, 모든 의존이 공개인 지금은 전환 전 도달 순서와 같다.
- 도달(링크·빌드 대상·순환 검사·`Closures`·모듈 표 행 의존·진입점 판정·SDK·기술자·Release 배제·`DynamicModules` 겹침)은 `AllDependencies`를 따른다.
- 한 모듈을 두 목록에 함께 적으면 입력 오류다(BD11의 첫 항목 — 규칙 값만 보므로 BS1의 depfile 없이 지금 한다).
- 설치 SDK manifest 12: `SdkModule.Dependencies`를 `PublicDependencies`·`PrivateDependencies`로 나눈다. SDK 게임 빌드는 같은 보임 규칙을 쓴다.
- 전환 검증(완료 기준): 모든 타깃(Foundation·EngineTests·Engine·Sandbox·Commandlet·벤치 여섯·ShaderToolSpike·WorldBench)의 Debug `compile_commands.json`이 전환 전과 byte 단위로 같다.

**[유니티 빌드·PCH](UnityBuildPch.ko.md)와의 관계**: 8.4절 공유 PCH의 "제공 모듈의 public include closure"는 이 문서의 공개 환경이다. 소비 모듈이 고르는 제공 모듈의 후보(같은 절 248행 "도달 집합에 있는 제공 모듈")는 "컴파일 환경에 보이는 제공 모듈"로 바꾼다. UE도 공개 closure와 직접 비공개 의존만 본다(`UEBuildModule.cs:1169-1206` `InternalGetAllDependencyModulesForPCH`).

### 3.6 플러그인 활성 조건

프로젝트 기술자의 `plugins[]` 항목에 선택 필드 세 개를 더한다(BD12).

```json
{ "name": "ModelingTools", "enabled": true, "targets": ["Editor"] }
{ "name": "OnlineSteam", "enabled": true, "platforms": ["Windows"], "configurations": ["Development", "Release"] }
```

| 필드 | 값 | 생략 | UE 5.8 대응 |
|---|---|---|---|
| `targets` | `Game`·`Editor`·`Client`·`Server`·`Program` | 모든 타깃 | `TargetAllowList` |
| `platforms` | `Windows`·`MacOS`·`Android`·`Linux`(BD18) | 모든 플랫폼 | `PlatformAllowList` |
| `configurations` | `Debug`·`Development`·`Release` | 모든 구성 | `TargetConfigurationAllowList` |

**값 검사**: 문맥과 무관하게 항상 검사한다.
- 조건은 `enabled: true`에만 붙일 수 있다.
- 빈 목록, 중복 값, 모르는 값은 오류다.
- 프로젝트 `targetPlatforms` 밖의 플랫폼을 적어도 오류다.

**첫 엔진 플러그인**(Unreal 교환 UXR7, 2026-11-20 — 설계. 플러그인 골격은 BS9d(3.9절), 내용은 UnrealExchange UX1a·UX1b). `Engine/Plugins/Importers/UnrealExchange/`([Unreal 교환 형식](UnrealExchange.ko.md) 10.1절)가 엔진 저장소의 첫 플러그인이 된다. 엔진 규칙 프로젝트가 이미 `Engine/Plugins/**`의 규칙을 모으고(Build/Rules/Rocks.Engine.Rules.csproj:15) PluginCatalog가 그 경로를 찾는다.
- 모듈은 `UnrealExchange` 하나이고 종류는 Developer다. 그래서 3.1절 표대로 Release 구성의 Game·Client·Server에 들어가지 않고 Program(commandlet)·Editor에만 들어간다. 이 문단의 활성 조건 필드를 쓰지 않아도 Shipping에서 빠진다.
- 기본 비활성이다. 명령행 도구(`Programs/Commandlet`)가 이 플러그인을 켜서 `asset-import-unreal` 명령을 등록한다. 플러그인 모듈이 Commandlet 명령을 더하는 확장점은 UX1에서 Commandlet 쪽과 함께 정한다.
- 플러그인 콘텐츠(엔진 `/Engine` 마운트가 생기기 전까지 둘 "구운 표면" 머티리얼 — MaterialSystem 11.3절)의 마운트 규칙도 UX1에서 정한다.
- 없는 플러그인을 참조하면 문맥과 무관하게 오류다(지금과 같다).
- Asan·Coverage 구성은 Debug로 본다(`ROCKS_BUILD_DEBUG`와 같다).

**해석(UE 의미론)**: 조건이 있는 참조는 그 플러그인의 상태를 정한다. 이번 빌드 문맥(타깃 종류, 플랫폼, 구성)이 조건에 맞으면 `enabled: true`, 맞지 않으면 `enabled: false`로 적은 것과 같다. 그래서 `enabledByDefault` 플러그인을 `targets: ["Editor"]`로 적으면 Editor에서만 켜진다. UE 근거는 다음과 같다.
- 프로젝트 참조가 플러그인 이름을 먼저 차지하면 기본 활성 처리가 그 이름을 다시 켜지 않는다(`UBT/Configuration/UEBuildTarget.cs:5661-5668, 5680`).
- 참조의 활성 여부는 `IsEnabledForPlatform && IsEnabledForTargetConfiguration && IsEnabledForTarget`로 정해진다(`UBT/System/Plugins.cs:689-706`).

그 뒤는 지금 규칙(의존 자동 활성, 끈 플러그인을 다른 켜진 플러그인이 요구하면 오류)을 그대로 적용한다. 문맥이 맞지 않아 꺼진 플러그인을 다른 켜진 플러그인이 요구하면 오류다. 메시지는 요구하는 쪽 참조에도 같은 조건을 적으라고 안내한다.

**플러그인의 지원 플랫폼(BD13)**: `.rocksplugin`에 선택 필드 `platforms`(생략은 모든 플랫폼)를 더한다.
- 이번 플랫폼이 플러그인 `platforms`에 없을 때:
  - 기본 활성(`enabledByDefault`, 프로젝트 참조 없음)으로만 켜질 플러그인은 조용히 뺀다. UE도 기본 활성 플러그인에 지원 플랫폼을 복사해 미지원 플랫폼에서 뺀다(`UEBuildTarget.cs:5685-5692, 5846-5851`).
  - 프로젝트가 이 문맥에서 명시해 켰거나, 켜진 플러그인이 의존으로 요구하면 오류다.
- UE는 같은 정보(`SupportedTargetPlatforms`)를 `.uproject` 참조에도 복사해 두지만(`PluginReferenceDescriptor.cs:85`), Rocks는 플러그인 기술자 한 곳에만 둔다.

**플러그인 의존 참조에는 조건을 두지 않는다**(BD23). 의존 플러그인(`.rocksplugin`의 `plugins`)은 의존하는 플러그인이 켜진 문맥에서 항상 켜진다. UE는 `.uplugin`의 `Plugins[]`에도 같은 조건을 평가한다(조건 평가 `UEBuildTarget.cs:5828-5844`, 새 의존 재귀 5966–5975행). Rocks는 문맥별 조건을 프로젝트 한 곳에 모아 해석 순서를 단순하게 한다.

**처리 순서**: 지금 게임 빌드는 규칙을 읽기 전에 `DescriptorModuleSet.Resolve`를 불러(`InstalledSdk.cs:281`, 규칙 로드는 `:315`) 타깃 종류를 모른다. 이를 두 단계로 나눈다.
- 문맥과 무관한 검증(형식·값·참조 존재)은 지금 자리에 둔다.
- 문맥별 해석은 규칙을 읽은 뒤 타깃 종류를 받아 한다. `Resolve` 시그니처에 타깃 종류를 더한다(BS4).

### 3.7 전용 서버 실행

Rocks 게임은 네트워크가 구성되면 전용 서버로 돌 수 있어야 한다(필수 — 머리말). 서버 플랫폼은 Windows·macOS·Linux다. 빌드 쪽에서 필요한 것은 아래 네 가지다. 구현 시점은 네트워크 단계(BS7)다. 그 전 단계(BS2 타깃 종류·매크로, BS6 Linux 플랫폼)는 서버를 받을 자리를 미리 만든다.

1. **Server 타깃(3.1–3.3절).** 클라이언트 코드(`ROCKS_WITH_CLIENT_CODE=0`)와 `ClientOnly` 모듈을 뺀 실행 파일이다. 게임 프로젝트는 `<Game>Server.Target.cs`를 두며, 템플릿(`Templates/Game`)도 Server 타깃 규칙을 함께 싣는다. UE 템플릿에는 Server 타깃 파일이 없고 사용자가 더하지만, Rocks는 필수 요구라 처음부터 싣는다.
2. **Linux x64 빌드 플랫폼(BD18).** `NativePlatform`에 `Linux`를 더한다.
   - `BuildContext.PlatformDirectory`는 `Linux`, 플랫폼 매크로는 `ROCKS_PLATFORM_LINUX`다.
   - 툴체인은 clang++ + lld다(Windows clang-cl·macOS clang++·Android NDK clang과 같은 clang 계열).
   - 플랫폼 구현은 `Private/Posix/`(공용)와 `Private/Linux/`(Linux 전용)에 둔다([저장소 구성](../Guides/RepositoryLayout.ko.md) 계층 규칙에 Linux를 더한다).
   - 지원 범위는 [전체 계획](EngineImplementationPlan.ko.md) 130행대로 서버·기반 시험이다. Linux에서는 Server·Program 타깃만 빌드하고, 다른 타깃은 빌드 규칙이 이유와 함께 거부한다.
3. **렌더 없는 실행.** Server 타깃 실행 파일은 항상 `--headless`와 같은 실행 성격이다(창·그래픽 장치를 만들지 않는다). 빌드 쪽 계약은 "Server 타깃은 그래픽 backend 모듈을 링크하지 않는다"이다. 그래픽 backend 모듈을 type `ClientOnly`로 두어 3.3절 검사가 막는다. 오디오·입력 backend를 어떻게 할지, 외형 에셋을 stub으로 로드하는 규칙([Sparks](SparksLanguage.ko.md) 851행 준비 mask)과 어떻게 맞출지는 네트워크 단계 스펙에서 정한다.
4. **서버 플랫폼은 Windows·macOS·Linux다.** 세 플랫폼 모두 Server 타깃을 빌드·실행한다(UE도 Win64·Mac·Linux 서버를 빌드한다). Android·iOS는 클라이언트 전용이라 Server 타깃을 거부한다. 운영 서버의 주 플랫폼은 Linux x64다.

[전체 계획](EngineImplementationPlan.ko.md) 103행 "M3 Linux에서 Core CPU/headless suite"의 선행 조건은 BS6이다.

### 3.8 링크 방식

Editor 타깃은 UE와 같이 모듈식으로 링크한다(BD17 — 사용자 결정 2026-11-19 "언리얼 방식대로"). 이 절은 UE 구조를 그대로 따르고, Rocks에 아직 없는 부분(모듈 수명·로더)은 요구 계약만 정해 별도 스펙으로 넘긴다.

**링크 방식 두 가지(BD17).** 타깃 규칙 속성 `LinkType { Monolithic, Modular }`를 둔다.
- `Monolithic`: 지금 구조다. 모든 모듈을 정적 라이브러리로 만들어 실행 파일 하나에 링크한다.
- `Modular`: 모듈마다 공유 라이브러리 하나(Windows `.dll`과 import `.lib`, macOS `.dylib`, Linux `.so`)를 만든다. 실행 파일은 부트 모듈(Launch 의존 폐포)만 import하고, 나머지는 모듈 로더가 의존 순서로 적재한다([모듈 로더](ModuleLoader.ko.md) MD21).
- 기본값은 Editor 타깃이 `Modular`, 나머지가 `Monolithic`이다(UE `TargetRules.cs:2686-2689`). 타깃 규칙이 바꿀 수 있다(UE와 같다). Modular Program 타깃은 export 누락을 Editor 전에 잡는 시험 빌드에 쓴다(BD31).
- Android는 `Modular`를 거부한다(UE `Platform/Android/UEBuildAndroid.cs:347-351`, iOS `Platform/IOS/UEBuildIOS.cs:993-997`, 검사 `Configuration/Rules/RulesAssembly.cs:856`). iOS가 생기면 같은 규칙이다.
- 링크 방식은 매크로 `ROCKS_MONOLITHIC`(0·1)로 컴파일에 알린다(UE `IS_MONOLITHIC` — `UEBuildTarget.cs:6345`). BD2 매크로와 같이 타깃 안에서 같은 값을 강제하고, 기본값 없이 `Base/Compiler.h`가 검사한다. 쓰는 곳은 모듈 진입점 매크로(BD30)뿐이다. export 값은 빌드가 모듈 API 매크로로 직접 넣는다(BD27). 타깃 종류 매크로를 두지 않는 규칙(BD3)과는 다른 축이다.
- 공유 라이브러리 파일 이름은 `<타깃 이름>-<모듈 이름>`이다. 구성이 Development가 아니면 `-<플랫폼>-<구성>`을 붙이고, macOS·Linux는 `lib` 접두를 붙인다(UE `UEBuildTarget.cs:5400, 5473-5508`).
- **산출물 배치와 링크 옵션**: 엔진 모듈 공유 라이브러리는 실행 파일과 같은 출력 폴더에 둔다. 게임·플러그인 모듈의 위치와 실행 중 탐색은 모듈 로더 스펙(BD30)이 정한다. 빌드 쪽 옵션은 UE와 같다.
  - macOS: 공유 라이브러리의 install name은 `@rpath/<파일 이름>`이고, 실행 파일·라이브러리는 `-rpath @loader_path/`·`-rpath @executable_path/`를 받는다(UE `Platform/Mac/MacToolChain.cs:578-584`).
  - Linux: `-Wl,-rpath=$ORIGIN`(UE `Platform/Linux/LinuxToolChain.cs:791-793`), 공유 라이브러리마다 `-soname`(파일 이름), 컴파일은 `-fPIC`다(지금은 Android만 붙인다 — `ActionGraph.cs:515-518`). Linux에는 Editor 타깃이 없으므로 `Modular`는 Program 시험 빌드에만 쓴다.
  - Windows: import 라이브러리를 링크하고, 실행 파일 폴더의 DLL은 기본 탐색으로 적재된다.
- `Modular`는 Debug·Development 구성에서만 허용한다. Asan·Coverage 구성과 Release는 `Monolithic`만이다(Editor는 Release로 빌드하지 않는다 — BD20). 필요해지면 다시 본다.

**BS8 하위 단계(개정 6).** BS8은 범위가 넓어(모든 모듈의 공개 헤더·생성 코드·정책·검사 체인) 넷으로 나눠 차례로 한다. 각 단계는 Monolithic 동작을 바꾸지 않는다(매크로 값만 더해진다).
- **BS8a 빌드 기반**: 타깃 규칙 `LinkType`(Editor 기본 `Modular`, 나머지 `Monolithic`)과 요청 `NativeBuildRequest.LinkType`, `Modular` 거부(Android, Asan·Coverage·Release 구성), `ROCKS_MONOLITHIC`(구성 매크로처럼 모든 compile 인자 — 타깃 하나에 한 값, `Base/Compiler.h`가 기본값 없이 검사), `ROCKS_DLLEXPORT`·`ROCKS_DLLIMPORT`, 모듈 API 매크로 `ROCKS_<MODULE>_API`(대문자 모듈 이름) 주입, 예약 매크로 거부, `Modular` 공유 라이브러리 링크(파일 이름·import 라이브러리·install name·rpath·`-soname`·`-fPIC`·가시성 옵션). 완료 기준: Monolithic 모든 타깃의 `compile_commands.json`이 전환 전과 `ROCKS_MONOLITHIC=1` 인자와 정의 헤더 강제 include만 다르고(모든 C++ 모듈이 빈 값 API 매크로를 받아 정의가 없던 모듈도 정의 헤더가 생긴다 — 13개 타깃 파일별 대조로 확인), 시험 fixture(공유 라이브러리 둘과 실행 파일 — export·import 함수 호출)가 `Modular`로 Windows에서 링크·실행된다.
- **BS8b 생성 코드 키**: `NativeTypeKey<T>()`·모듈 키·`Schema()`의 저장소를 생성 `.cpp`로 옮기고 모듈 API 매크로로 export(BD28).
- **BS8c 공유 상태**: 정책 `public-header-static-storage`와 지금 위반 정리, 서드파티 `HasGlobalState` 표시와 단일 소비 검사(BD29).
- **BS8d export와 검사 체인**: 기존 모듈 사이 API에 매크로 붙이기, 상속 export 진단·모듈별 export 수 진단, 빌드 정보(`ToolchainFingerprint`·`ToolBuildId`)의 Modular 정의 위치, 시험이 부르는 내부 선언 export(BS8d5 — BD31 개정, 처음 안 "`Private/` 시험 묶음의 모듈 내 링크"를 대신한다), `Modular` FoundationTests 검사 체인(BD31 — Windows 로컬, macOS는 CI). 이 단계가 끝나면 BS8 완료 기준을 채운다. 모듈 로더 쪽 Modular 적재는 [모듈 로더](ModuleLoader.ko.md) ML4다.

**BS8a 구현 세부(개정 6).**
- 모듈 API 매크로는 모듈 정의 헤더(`<출력>/Definitions/<모듈>.Definitions.h`)에 들어간다: 자기 모듈과 컴파일 환경에 보이는 C++ 모듈(서드파티·header-only 제외 — 바이너리가 없다)마다 하나씩. 값은 `Monolithic`이면 빈 값, `Modular`면 자기 모듈은 `ROCKS_DLLEXPORT`, 다른 모듈은 `ROCKS_DLLIMPORT`와 같은 철자다(정의 헤더는 Base 헤더 없이도 쓰이도록 플랫폼 철자 `__declspec(dllexport)`·`__attribute__((visibility("default")))`를 직접 쓴다). 코드가 없는 모듈(header-only)은 `Modular`에서도 빈 값이다. 공유 PCH의 공유 정의는 제공 모듈 공개 환경의 API 매크로를 import 값으로 담는다(PCH 헤더가 그 매크로를 쓰므로). 그래서 `Modular`에서 제공 모듈 자신은 값이 달라 공유 PCH를 쓰지 않는다(기존 "같은 이름 다른 값이면 공유 PCH를 쓰지 않는다" 규칙).
- 예약: 규칙·명령줄은 `ROCKS_MONOLITHIC`과 `ROCKS_*_API`를 정의할 수 없다(`ROCKS_MODULE_NAME`과 같은 `InputValidation` 검사).
- `Modular` 링크: 라이브러리 모듈은 공유 라이브러리가 된다. Windows는 `<이름>.dll`과 import `<이름>.lib`(링크 `/DLL`), macOS는 `lib<이름>.dylib`(`-dynamiclib`, install name `@rpath/<파일>`), Linux는 `lib<이름>.so`(`-shared`, `-soname`). 이름은 `<타깃>-<모듈>`이고 Development가 아니면 `-<플랫폼>-<구성>`을 붙인다(3.8절). 공유 라이브러리와 실행 파일은 링크 폐포(두 의존 목록, 실행 파일은 `DynamicModules` 쪽 포함)의 공유 라이브러리(Windows는 import 라이브러리)를 링크한다. inline 헤더가 간접 의존 모듈의 함수를 부를 수 있기 때문이다. 서드파티·미리 빌드한 라이브러리는 정적으로 그것을 링크하는 공유 라이브러리마다 들어간다(사본마다 전역 상태가 따로 생길 수 있다 — BS8c의 `HasGlobalState` 단일 소비 검사가 맡는다). 실행 파일이 `DynamicModules` 쪽 import 라이브러리까지 링크하므로 요청 시 적재 모듈도 load-time 의존이 된다(한계 — ML4에서 manifest 적재로 바꾼다). BS8d·ML4 전에는 모듈 표·빌드 정보 제공 모듈(Core)에 닿는 타깃과 미리 빌드한 SDK 모듈, SDK 내보내기의 `Modular`를 거부한다. Modular 가시성 옵션은 서드파티 C 소스에도 붙는다(macOS C 모드의 `-fvisibility-inlines-hidden` 수용은 macOS 빌드에서 확인할 것). 실행 파일과 공유 라이브러리는 같은 출력 폴더(`bin`)에 두고 macOS·Linux는 `@loader_path`·`$ORIGIN` rpath를 받는다. 가시성: macOS `-fvisibility-ms-compat`·`-fvisibility-inlines-hidden`, Linux `-fvisibility-ms-compat`와 `-fPIC`.
- 지금 실제 타깃은 모두 `Monolithic`이다(Editor 타깃이 없다). `Modular` 실제 타깃 빌드(모듈 표의 export 진입점·빌드 정보 정의 위치)는 BS8d·ML4 뒤에 된다.

**BS8b 구현 세부(개정 7 — 사용자 결정 2026-11-21, UE 방식).**
- 문제: 손으로 쓴 헤더는 자기 생성 헤더를 include하지 않는다(리플렉션 생성 소스 GSD6). 그래서 생성 `.cpp`의 키 저장소를 쓰는 `NativeTypeKey<T>()`는 생성 헤더를 include하지 않은 번역 단위에서 그 선언을 보지 못하고, 그 번역 단위만 다른 키(inline 변수 주소)를 써 Monolithic에서도 ODR 위반이 된다.
- 결정: UE처럼 모든 리플렉션 class·struct(`ROCKS_CLASS`·`ROCKS_STRUCT`)에 `ROCKS_GENERATED_BODY()`를 의무로 한다(UE `GENERATED_BODY`). 의무화·기존 타입 삽입·판정 표지는 리플렉션 쪽 작업이 먼저 넣었다(2cdc188f·90972993 — 빌드 메타 단계 RMC2211, 표지 `rocksReflectedSelf`와 `ReflectedDeclarationV<T>`, [리플렉션 생성 소스](ReflectionGeneratedSources.ko.md) 8.1절). BS8b는 그 판정을 키에 쓴다: `ReflectedDeclarationV`는 선언만으로 정해져 생성 헤더 include 여부와 무관하게 번역 단위마다 같고, 리플렉션 base를 물려받은 native 파생은 거짓이다.
- 키: 생성 헤더의 `GeneratedType<X>`가 `static ROCKS_<M>_API const void* NativeKey() noexcept`를 선언하고 생성 `.cpp`가 const가 아닌 정적 지역 저장소의 주소로 정의한다(공유 라이브러리마다 하나 — 소유 모듈). 생성 키는 리플렉션 클래스(Object 파생)만 낸다. `NativeTypeKey<T>()`는 `Object`면 null, 리플렉션 클래스(`ReflectedDeclarationV<T>`이고 Object 파생)면 `GeneratedType<T>::NativeKey()`이고 생성 헤더가 보이지 않으면 `static_assert` 오류("NativeTypeKey of a reflected type needs its generated header: include <Module/Header.generated.h> …"), 그 밖(리플렉션 struct·enum·native 등록 타입)은 지금처럼 inline 변수 주소다. 클래스 키를 담는 연산표(`ReferenceArrayStage`·`ReferenceMapStage`의 `ReferenceStageOps`)는 상수식이 될 수 없어 처음 쓸 때 만드는 함수 안 정적 표(`Ops()`)로 바꿨다 — 생성 코드에서만 인스턴스화된다. class 타입은 완전한 타입이어야 한다(`static_assert` — 전방 선언만 보이면 `ReflectedDeclarationV`가 거짓이 되어 조용히 inline 키로 가는 것을 막는다).
- 사용처: 생성 소스는 클래스 대상(참조의 기대 타입·ClassRef의 Base·클래스 base)의 생성 헤더를 include한다(말단 번역 단위라 순환이 없다 — 생성 헤더의 include는 지금처럼 값 타입만). 손으로 쓴 코드는 리플렉션 타입의 키를 인스턴스화하는 번역 단위(`World::With<Actor>`·`ObjectRegistry::Resolve<Package>` 등 템플릿 호출 포함)가 그 생성 헤더를 include한다. 탐침이 리플렉션 타입 판정을 생성 헤더 없이 하므로 빠뜨리면 조용히 다른 키가 되지 않고 컴파일 오류다.
- 버린 안: 리플렉션 헤더가 자기 생성 헤더를 include(UE 구조 — GSD6을 뒤집어 리플렉션 생성 소스 설계와 겹친다), 타입 이름 해시 키(생성 코드가 필요 없지만 익명 namespace의 같은 이름 타입이 충돌한다).
- 한계: 리플렉션 struct는 inline 키를 유지한다. 헤더 템플릿의 상수 연산표(`StructElementOpsOf<S>::Value` — `StructRefArray<S>` 생성자가 손으로 쓴 헤더 안에서 인스턴스화한다)가 키를 상수 주소로 담는데, 생성 키를 쓰면 `StructRefArray` 필드를 가진 클래스를 만드는 모든 번역 단위에 원소 struct의 생성 헤더가 필요해진다(GSD6과 충돌). Modular에서 모듈 경계를 넘는 struct 키 통합은 BS8d에서 TypeId 기반 조회 등으로 다룬다. 리플렉션 enum은 본문 매크로를 둘 수 없어 지금 키(inline 변수)를 유지한다. Binds 미러의 실제 타입(`Vec3`·`Transform`·`Color`·`InterpCurve` 등 Core 값 타입)도 표지가 없어 inline 키다 — Modular에서 모듈 경계를 가장 많이 넘는 struct 키다. 리플렉션하지 않는 native 등록 타입도 inline 키라 Modular에서 모듈 경계를 넘는 그런 타입의 키는 모듈마다 다르다. 둘 다 BS8d의 진단 대상이다. 모듈 키는 이미 모듈 생성 소스의 정적 지역 객체(소유 모듈 하나)이고, `Schema()`는 리플렉션 생성 소스(GS2) 뒤 생성 `.cpp`의 비inline 함수라 export(BS8d)만 남는다. 완전한 타입 검사는 `NativeTypeKey` 안에서만 한다: 같은 번역 단위의 다른 곳이 불완전한 T로 `ReflectedDeclarationV<T>`를 먼저 평가하면 그 값이 고정된다([리플렉션 생성 소스](ReflectionGeneratedSources.ko.md) 8.1절 한계와 같다).

**BS8c 구현 세부(개정 13).**
- 판정 1(컴파일): Windows 대상의 공개 헤더 검사 모듈(`PublicHeaderChecks`) 컴파일에 `-Wunique-object-duplication`을 더한다. LLVM Clang 21부터 있는 진단으로, 공유 라이브러리마다 복제되는 가변 객체를 경고한다(빌드는 경고를 오류로 다룬다). 잡는 것: const가 아닌 `inline` 변수, inline 함수 안의 const가 아닌 `static` 지역 변수, `static inline` 데이터 멤버, `inline thread_local`. 메시지는 "no import/export annotation"이다. 진단은 헤더의 선언에만 걸린다(주 소스 파일·시스템 헤더는 보지 않는다). 공개 헤더는 서드파티 헤더를 include하지 않는다(서드파티 규칙 계약).
- 판정 2(어휘): 정책 `public-header-static-storage`가 엔진 소유 모듈의 `Public/` 헤더(엔진 플러그인 모듈 포함, 서드파티 제외)에서 주석·리터럴을 뺀 `thread_local` 토큰을 거부한다. 진단 1이 보지 못하는 `extern thread_local` 선언과 `static thread_local` 데이터 멤버 선언까지 잡는다(DLL 경계로 import할 수 없으므로 선언도 공개 헤더에 두지 않는다).
- 대상: 판정 1은 Windows 대상만 켠다(`ToolchainCheck.SupportsUniqueObjectDuplication` — 대상과 `CompilerVersion`의 제조사·주 버전으로 정한다). ELF·Mach-O에서는 진단이 숨김 가시성 객체만 알리는데 공개 헤더 검사는 Monolithic 기본 가시성으로 컴파일하므로 걸릴 것이 없고, Apple clang에는 진단이 없으며, Android NDK clang 20에는 진단이 없다(Windows 판은 Clang 21부터). Windows 최소가 Clang 21이고 Windows CI가 판정 1을 늘 돌리므로 공통 헤더는 빠짐없이 검사된다. 다른 대상에서는 판정 2만 돈다.
- 고친 위반: 할당기(`DebugAllocator`·`ExternalAllocator`·`PoolAllocator`)의 `static thread_local activeEntry_` → `.cpp`가 정의한 private 접근 함수(함수 안 `thread_local`). `Correlation.h`의 `extern thread_local correlationDepth`·`TraceLogSink.h`의 `inTraceLogWrite`·`ConfigSnapshot.h`의 `inline thread_local CurrentBinding`·`ConfigContext.h`의 `inline ActiveDefaultWriteProbe` → `.cpp`가 정의한 접근 함수(inline으로 쓰던 곳은 그 함수를 부른다 — 호출 하나가 는다). 시험 fixture의 쓰기 가능한 `static inline` 멤버 → 선언만 두고 fixture 모듈 `.cpp`가 정의한다.
- 서드파티 전역 상태: `ThirdParty.json`(서드파티의 단일 원천)의 code 라이브러리에 `globalState`(true·false) 키를 필수로 둔다(다른 종류는 거부). 규칙이 `NativeModuleInput.HasGlobalState`로 옮기고(서드파티 모듈만 가질 수 있다 — 아니면 입력 오류), 설치 SDK 매니페스트 13이 함께 나르며, 공개 헤더 검사 모듈(`PublicHeaderChecks`)은 서드파티를 의존하지 않으며(공개 include가 없다 — 소비 모듈로 세지 않게), 빌드 입력 검사가 그 모듈을 직접 의존(두 의존 목록)하는 Rocks 모듈이 둘 이상이면 오류다(소비 모듈 이름을 모두 알린다). 지금 표시: Sodium만 true(`sodium_init` 상태·난수 구현 교체·misuse handler — Crypto 하나가 소비), Zstd·Zlib·Lz4·Brotli·MikkTSpace·SpirvReflect는 false(정적 const 표뿐).
- 버린 안: 헤더마다 Clang JSON AST를 내 걸러 보기(World.h 하나에 92 MB — 헤더 수백 개면 검사 시간이 빌드를 넘는다), 어휘만으로 정적 지역 변수·inline 변수 판정(매크로가 펼치는 정적 지역·템플릿 문맥을 놓치고 오탐이 많다), `globalState`를 규칙 클래스 속성으로(서드파티 사실은 매니페스트가 단일 원천이다).
- 한계: Clang 진단은 템플릿 문맥(변수 템플릿·템플릿 함수 안 정적 지역·템플릿 클래스 정적 멤버)을 보지 않고 어휘 검사도 잡지 않는다. 쓰기 가능한 것은 `NativeTypeIdentity<T>`(native 등록 타입 키 — struct·enum·Binds 대상은 BS8d4가 생성 키로 옮겼다)이고, 그 밖에 템플릿 안 동적 초기화 const 정적 지역(`GeneratedBitfieldAccess.h`·`GeneratedStagedReferenceAccess.h`의 연산표, `ValueCopy.h`)이 있다 — 주소를 비교하지 않아 복제돼도 동작은 같다. 주소를 식별자로 쓰는 const 정적 객체도 판정 밖이다(리뷰로 막는다). `ROCKS_GRAPH_SHARED_DATA`(`GraphSharedData.h`)는 inline 함수 안 정적 색인이라 공개 헤더에서 쓰면 판정 1이 잡지만, 모듈 사이에 공유할 타입을 등록할 길이 없다 — 호스트 서비스 키처럼 선언·정의로 나누는 것은 BS8d4가 했다(`ROCKS_DECLARE·DEFINE_GRAPH_SHARED_DATA`). 게임 모듈(`Templates/<게임>/Source/<모듈>/Public/`)은 판정 2의 경로와 엔진 타깃의 공개 헤더 검사 범위 밖이다(게임 빌드의 공개 헤더 검사가 생길 때 다룬다).

**BS8d와 ML4 묶음(개정 14 — 사용자 결정 2026-11-21).**
- 문제: Modular에서는 실행 파일 생성 TU가 정의하던 두 기호를 공유 라이브러리가 풀지 못한다. 모듈 표 `GeneratedModuleTable()`(HostConfig·Launch·시험이 부른다)과 빌드 정보 `ToolchainFingerprint()`·`ToolBuildId()`(Launch·TraceCapture가 부른다)다. 빌드 정보는 Core 공유 라이브러리에 둘 수 있다(공유 라이브러리 이름이 타깃마다 따로다 — `<Target>-Core`). 모듈 표는 모든 모듈의 진입점을 참조하므로 Core에 두면 공유 라이브러리 사이 순환 의존이 된다. UE는 Modular에서 정적 표 없이 바이너리 옆 manifest(`.modules`)로 모듈을 찾는다([모듈 로더](ModuleLoader.ko.md) 3.5절 — ML4).
- 결정: BS8 완료 기준(`Modular` FoundationTests가 Windows·macOS에서 빌드·통과)은 ML4(manifest 적재)와 함께 채운다. 묶음 순서는 다음과 같다. 단계마다 스펙 세부를 먼저 적고 리뷰·병합한다.
  1. **BS8d1 Modular 빌드 기반**: Modular에서 빌드 정보 TU를 빌드 정보 제공 모듈(Core)의 공유 라이브러리에 넣는다. 모듈 표 TU를 만들지 않는다. 진입점 매크로가 공유 라이브러리마다 export 진입점 `RocksModuleEntry()`를 정의하고 빌드가 강제 참조를 넣는다. 빌드가 링크 뒤 manifest(`<Target>[-<Platform>-<Configuration>].modules.json`, `buildId` 재사용 MD13·크기·의존·단계)를 원자적으로 쓴다. 검증은 빌드 시험의 Modular fixture다.
  2. **ML4a 로더의 manifest 적재**: manifest 읽기(모르는 키 거부), `Prepare`(의존 순서 OS 적재·진입 기술 읽기·`buildId`·크기 대조·report), 그 결과로 만든 표로 지금 로더 동작(단계·묶음·동적 모듈)을 그대로 쓴다.
  3. **ML4b 실행 파일 연결**: `GeneratedModuleTable()`은 Monolithic에만 있다. Launch·HostSettings·시험 runner는 Modular에서 로더가 만든 표를 쓴다. 실행 파일은 부트 모듈만 import한다(MD21). 시험 등록 목록의 머리는 TestSupport가 한 벌 가진다(MD20). 이 단계 끝에 Core에 닿는 타깃의 Modular 거부를 푼다.
  4. **BS8d2 export 매크로**: Modular FoundationTests 링크 오류를 따라 그 폐포 모듈의 모듈 사이 API에 `ROCKS_<M>_API`를 붙인다(클래스 단위 우선 — BD27). `Monolithic` object 인자는 매크로 값 외에 바뀌지 않는다.
  5. **BS8d3 진단**: 상속 export 진단(메타 컴파일러)·모듈별 export 수 진단.
  6. **BS8d4 남은 키**: struct·enum·native 등록 타입 키의 모듈 사이 통합(BS8b 한계), `ROCKS_GRAPH_SHARED_DATA` 선언·정의 나누기(BS8c 한계).
  7. **BS8d5 검사 체인**: `Modular` FoundationTests를 로더로 실행해 통과(Windows 로컬, macOS CI) — BS8·ML4 완료 기준.
  8. **BS8d6 Modular 크래시·Launch 하니스**(개정 22에서 더함, 세부는 개정 25): native 하니스의 크래시·Launch 시험을 Modular에서 돌린다(Launch 시험 프로그램의 Modular 타깃, cdb 스택 검사의 모듈 이름, 도우미 프로세스의 Modular 진입). Editor가 Modular라 크래시 수집 경로를 확인한다.
  9. **BS8e Engine 층 Modular export**(개정 23에서 더함): Engine 층 모듈과 Engine 시험이 부르는 선언의 export, 시험 타깃 `EngineModular`(엔진 플러그인 시험 제외 — 플러그인 Modular는 ML5). 모듈을 맡은 세션은 목록을 받아 붙인다.
- 버린 안: 실행 파일이 모듈 표를 export하고 Core가 OS 기호 조회로 찾는 임시 방식(UE에 없고 ML4가 대체하면 버릴 코드다).

**BS8d1 구현 세부(개정 15).**
- 빌드 정보: Modular에서 빌드 정보 TU(`<Target>.BuildInfo.cpp` — `ToolchainFingerprint()`·`ToolBuildId()`·`CurrentEngineVersion()`)를 실행 파일이 아니라 그 타깃이 닿는 빌드 정보 제공 모듈(`ProvidesBuildInfo`, Core)의 공유 라이브러리 소스에 넣는다. 공유 라이브러리 이름이 타깃마다 따로라(`<Target>-Core`) 타깃마다 한 벌이다. 세 함수의 선언(`Core/Build/BuildInfo.h`)은 `ROCKS_CORE_API`를 단다. Monolithic은 지금처럼 실행 파일에 넣는다(매크로는 빈 값).
- 모듈 표: Modular에서는 실행 파일 모듈 표 TU를 만들지 않는다. 같은 행(진입점 모듈의 의존 순서, 의존 이름, 단계, 요청 시 적재 표지, 실행 파일 행)을 manifest에 쓴다.
- 진입점: 빌드가 공유 라이브러리로 컴파일하는 진입점 모듈의 소스에 `ROCKS_MODULE_SHARED_LIBRARY=1`을 정의한다(다른 모듈·실행 파일·Monolithic은 0, 규칙·명령줄은 정의할 수 없다). 그 값이 1이면 `ROCKS_IMPLEMENT_MODULE`이 공유 라이브러리마다 하나뿐인 export 진입점 `extern "C" ROCKS_DLLEXPORT const ModuleEntry* RocksModuleEntry() noexcept`를 더 정의한다([모듈 로더](ModuleLoader.ko.md) 3.2절). 같은 자리에서 export하지 않는 모듈별 C 기호 `RocksModuleEntryOf_<Name>`도 정의하고, 빌드는 그 링크에 이 기호를 강제 참조로 넣는다: Windows `/INCLUDE:RocksModuleEntryOf_<Name>`, macOS `-Wl,-u,_RocksModuleEntryOf_<Name>`, Linux `-Wl,-u,RocksModuleEntryOf_<Name>`. 매크로가 빠진 진입점 모듈은 링크 오류다. export 진입점 이름(`RocksModuleEntry`)으로 강제 참조하면 의존 모듈의 import 라이브러리가 같은 이름을 내서 빠짐을 잡지 못한다(구현 중 시험으로 확인). 실행 파일은 export 진입점을 두지 않는다. ML4b부터 Modular 실행 파일 모듈은 호스트 공유 라이브러리가 되어 이 규칙을 그대로 받는다(값 1 — 아래 ML4b 세부).
- manifest: 링크가 모두 성공하고 입력이 바뀌지 않은 빌드 끝에 `<out>/bin/<Target>[-<Platform>-<Configuration>].modules.json`(접미사는 공유 라이브러리 이름 규칙과 같다)을 원자적으로 쓴다. 모듈 표 제공 모듈에 닿는 Modular 타깃만 쓴다. 키는 영어이고 모르는 키는 읽는 쪽이 거부한다(ML4a).
  - `schema`(3 — ML4a가 행 키 `executable`을 더하며 2로, ML4b가 그 키를 `host`로 바꾸며 3으로 올렸다), `buildId`, `engineVersion`(`"M.m.p"`), `toolchainFingerprint`(64자 소문자 16진), `modules`(배열 — JSON 객체의 키 순서는 의미가 없으므로 의존 순서를 배열로 싣는다).
  - `modules[]` 항목: `name`, `file`(bin 폴더 기준 파일 이름), `size`(링크가 끝난 파일의 바이트 수), `dependencies`(행 이름 배열), `loadingPhase`(단계 이름), `onRequest`(true·false), `host`(true·false — 호스트 모듈 행, ML4b. [모듈 로더](ModuleLoader.ko.md) 3.14절).
  - `buildId`(MD13): 같은 경로의 이전 manifest를 읽어 `schema`·`engineVersion`·`toolchainFingerprint`가 같으면 그 `buildId`를 다시 쓰고, 없거나 다르거나 읽을 수 없으면 새 GUID(32자 소문자 16진)다. 값은 manifest에만 있어 증분 빌드가 다시 컴파일하지 않는다(UE `CanRecycleBuildId`와 같다 — 실행할 때마다 달라지는 값이라 시험은 재사용·갱신 조건만 확인한다).
  - 엔진 버전·툴체인 신원은 빌드 정보 TU와 같은 값이다.
- 입력 검사(ML4b가 첫 항목을 바꿨다 — 아래 ML4b 세부): Core에 닿는 타깃의 Modular 거부를 모듈 표·빌드 정보 대신 실행 파일 연결 기준으로 좁힌다 — 타깃이 Launch(`ProvidesLaunch`)에 닿으면 ML4b 전까지 거부한다(Launch가 Monolithic 모듈 표 `GeneratedModuleTable()`을 부르므로 Modular 링크가 미해결 기호로 실패한다. HostConfig의 HostSettings도 부르지만 그 소비 타깃은 모두 Launch에 닿는다 — ML4b가 둘 다 바꾼다). 모듈 표·빌드 정보 제공 모듈에 닿는 대상 실행 파일(대상이 비면 전부에서 닿는 것)이 둘 이상이면 거부하고, 하나면 엔진 버전이 있어야 한다(manifest·빌드 정보는 타깃 하나 몫이다 — 링크가 끝난 뒤 실패하지 않게). manifest의 실행 파일 선택은 모듈 표 TU와 같은 규칙이다(`ModuleTables.TableExecutables`).
- 한계:
  - macOS Modular는 소비 모듈이 `.dylib` 자체를 링크 입력으로 가진다. 도구 빌드 ID는 도달 집합의 입력이 한 바이트만 바뀌어도 달라지고 그 정의 TU가 Core 공유 라이브러리에 있으므로, 소스 하나를 고쳐도 Core가 다시 링크되고 Core에 닿는 공유 라이브러리·실행 파일이 줄줄이 다시 링크된다(Windows는 import 라이브러리 내용이 같으면 멈춘다). Editor 반복 속도에 걸리므로 BS8d5 전에 다시 본다 — 후보: 소비 링크 key에 export 인터페이스만 쓰기(`.tbd`), Modular에서 도구 빌드 ID를 manifest나 작은 별도 라이브러리로 옮기기.
  - ELF `-shared` 링크는 미해결 기호를 허용하므로 Linux(BS6)는 `-Wl,-z,defs`를 함께 넣어야 강제 참조가 링크 오류가 된다.
- 시험: 빌드 시험 `modular` 묶음에 가짜 Core(모듈 표·빌드 정보 제공, 최소 진입 헤더)를 둔 Modular fixture를 더한다. 확인: 빌드 정보 기호가 Core 공유 라이브러리에서 export되고 실행 파일이 부른다, 공유 라이브러리마다 `RocksModuleEntry` export(매크로가 빠지면 링크 오류), 모듈 표 TU 없음, manifest 내용(행 순서·크기·의존·단계)과 `buildId` 재사용·갱신, 입력 거부(Launch·실행 파일 둘·엔진 버전 없음), Monolithic 무변화. 실제 `ModuleEntry.h`는 도구 시험(`ModuleEntryCompilerTests`)이 `ROCKS_MODULE_SHARED_LIBRARY=1`로 PCH 없이 컴파일한다.

**ML4b 빌드 쪽 세부(개정 16 — [모듈 로더](ModuleLoader.ko.md) 3.14절).**
- 대상: 타깃이 Launch(`ProvidesLaunch`)에 닿는 Modular 실행 파일. 그 실행 파일 모듈은 **호스트 모듈**이 된다 — 라이브러리 모듈과 같이 공유 라이브러리 `<Target>-<모듈>[-<Platform>-<Configuration>]`로 링크하고(진입점 모듈 규칙·`ROCKS_MODULE_SHARED_LIBRARY=1`·강제 참조), manifest 행에 `host: true`를 단다.
- 실행 파일은 빌드가 만든 진입 TU `<out>/GeneratedSources/<모듈>.Launcher.cpp` 하나를 그 모듈의 공통 compile 인자로(PCH·유니티·정의 헤더 없이) 컴파일하고, 링크 입력은 그 object와 Launch 공유 라이브러리(Windows는 import 라이브러리)뿐이다. subsystem·PDB 같은 실행 파일 링크 인자는 지금과 같다. 호스트 공유 라이브러리는 동적 모듈을 링크하지 않는다(로더가 싣는다). 진입 TU는 헤더 없이 Launch의 C 함수(`RocksLaunchModularApplication`·`RocksLaunchModularWindowedApplication`)를 선언하고 manifest 파일 이름을 넘긴다.
- manifest: `schema` 3, 행 키 `executable` 대신 `host`(호스트 행도 OS로 싣는다 — `file`은 호스트 공유 라이브러리).
- 입력 검사: 위 "Launch에 닿으면 거부"를 푼다. 모듈 표 제공 모듈에 닿는 Modular 실행 파일이 Launch에 닿지 않으면 거부한다(모듈을 시작할 진입이 없다). 실행 파일 둘 이상 거부는 그대로다.
- 시험: `modular` 묶음 fixture에 가짜 Launch를 더해 실행 파일이 import하는 Rocks 공유 라이브러리가 Launch뿐인지, 호스트 공유 라이브러리·export·`host` 행, 창 subsystem 진입 함수, Launch 없는 거부를 본다. 실제 Launch로 도는 성공 경로는 BS8d5다.
- Modular FoundationTests(BD31, BS8d5)는 시험 실행 파일 하나만 가진 타깃으로 둔다(실행 파일 둘 이상 거부 — Launch 시험 프로그램과 내장 진입 프로그램은 Monolithic 시험 타깃에만 있다).

**BS8d2 export 매크로 세부(개정 18).**
- 측정(2026-11-22, Windows Debug): FoundationTests가 링크하는 모듈 폐포를 Modular로 빌드하면 공유 라이브러리 링크마다 다른 모듈의 기호가 풀리지 않는다. 측정에서만 링크에 `/FORCE:UNRESOLVED`를 넣어 모든 라이브러리를 끝까지 링크해 보니 서로 다른 미해결 기호가 735개였다: 클래스 93개의 멤버 524개, 자유 함수 162개, 전역 변수 2개. 대부분 Core가 PlatformBase를, 그 위 모듈이 Core·ObjectRuntime을 부르는 것이다. 시험 소스(호스트 모듈)가 부르는 기호는 호스트가 컴파일되지 않아(정적 모듈 표 시험 — ML4b가 BS8d5로 넘김) 이 수에 없다.
- 붙이는 규칙(BD27, UE `<MODULE>_API`와 같다):
  1. 다른 모듈이 멤버를 링크하는 클래스는 클래스 전체에 붙인다(`class ROCKS_CORE_API String`). 클래스 단위가 BD27의 기본이고, 멤버 하나씩 붙이면 다음 사용처가 생길 때마다 링크 오류가 다시 난다.
  2. 자유 함수·전역 변수는 선언마다 붙인다(`[[noreturn]] ROCKS_PLATFORMBASE_API void Fatal(...)`, `ROCKS_CORE_API extern uint8 logFastMinimum` — 앞의 `[[...]]` 속성 뒤에 둔다). 정의(`.cpp`)에는 붙이지 않는다 — 선언의 속성이 정의에 이어진다.
  3. 템플릿·inline·`constexpr`에는 붙이지 않는다(BD27). 템플릿 클래스가 out-of-line 비템플릿 함수를 부르면 그 함수에 붙인다.
  4. 클래스 전체 export가 안 되는 클래스(Windows `dllexport` 클래스는 정의할 수 없는 멤버까지 모두 정의하려 한다 — 예: 복사할 수 없는 멤버를 가진 암시 복사)는 그 이유를 주석에 적고 멤버 단위로 붙인다.
  5. 공개 헤더(`Public/`)의 선언에만 붙인다. 다른 모듈이 부르는 기호가 비공개 헤더에만 선언돼 있으면 공개 헤더의 inline·템플릿 코드가 비공개 기호를 부르는 것이므로, 그 선언을 공개 헤더로 옮겨 붙인다(비공개 헤더에 API 매크로를 두지 않는다 — 공개 헤더 검사(BD9)가 보는 범위가 공개 API다). 예외는 규칙 파일이 비공개 include 디렉터리로 다른 모듈에 일부러 보여 준 내부 헤더다 — 지금은 Compression의 `CompressionInternal.h`·`CompressedBlocksInternal.h`(AsyncCompression만 쓴다 — 같은 코덱·블록 인코딩을 쓰려고). 그 선언에는 붙인다(UE 5.x 모듈 `Internal` 헤더와 같은 자리).
  6. 매크로는 그 선언이 있는 모듈의 것만 쓴다. 정책 `module-api-macro`가 막는다: 다른 모듈 이름의 매크로, 비공개 헤더의 매크로(규칙 5 예외 목록 `PolicyChecks.ExportedPrivateHeaders` 밖), 소스(`.cpp`)의 매크로는 오류다.
  7. 생성 코드: 메타 컴파일러가 모듈 생성 헤더의 public 등록 함수(`RegisterModuleTypes`) 선언과, 클래스·struct `GeneratedType<X>` 특수화의 out-of-line 정적 멤버 선언 전부(본문이 생성 소스에 있는 `static ... ;` — `Schema`·`Ops`·`Register`·`ValidateSelf`·`ValidateTree`·`CopyValue`·`ReferenceLayout`·비트 필드 접근·Binds 검사·`VerifyGenerated` 등, private 리플렉션 타입 포함)에 그 모듈의 매크로를 붙인다(BD28). 멤버 템플릿(`CopyValueImpl` 등)에는 붙이지 않는다. 특수화 전체가 아니라 멤버마다 붙이는 것은 이미 멤버에 붙인 `NativeKey`(BS8b)와 겹치지 않게 하려는 것이다. 매크로를 쓰는 생성 헤더(타입 생성 헤더·모듈 생성 헤더)는 빌드 밖 컴파일을 위해 `#ifndef` 대체 정의(빈 값)를 둔다.
  8. 같은 헤더의 같은 이름 오버로드와 짝 API(`TryCompressAppend`·`TryDecompressAppend`, `Begin`·`EndTraceTaskExecution` 등)는 하나가 export되면 함께 붙인다 — 다음 사용처가 오버로드 하나만 바꿔 다시 링크 오류가 나지 않게.
  9. `friend` 재선언: export한 자유 함수를 클래스가 `friend`로 선언하면 그 선언에도 같은 매크로를 붙인다(Windows는 속성이 다른 재선언을 오류로 본다 — `-Wdll-attribute-on-redeclaration`).
- 빌드 밖 컴파일: 정의 헤더 없이 헤더를 컴파일하는 검사(Rocks.ToolTests의 native 컴파일 시험·생성 헤더 검증)는 `Rocks.Build.Cli`가 타깃의 모든 비서드파티 모듈 `-DROCKS_<M>_API=`(빈 값 — Monolithic)를 넘긴다.
- 링크 오류를 따라 붙이므로 지금 다른 모듈이 부르지 않는 공개 함수는 export하지 않는다. 다만 모듈 사이 API로 설계된 클래스는 지금 쓰는 곳이 없어도 붙인다(`HostServices` — 엔진 모듈 `Startup`이 서비스를 찾는다). 다음 사용처가 생기면 그 링크가 알린다(Modular 시험 타깃 `FoundationModular`이 체인에 있다). 클래스 단위 export가 이 재작업을 줄인다.
- 확인 타깃 `ModularCheck`(`Programs/ModularCheck`, Modular Program 타깃): FoundationTests와 같은 런타임·개발 도구·시험 fixture 모듈에 의존하는 작은 실행 파일 하나다(ML4b — 타깃당 실행 파일 하나). 빌드가 모든 모듈 공유 라이브러리를 export 누락 없이 링크하는지 보고, 실행은 ML4b Modular 진입(진입 스텁 → Launch → manifest 로더 → 호스트 앱 기술 → 부트 모듈 시작)으로 끝까지 돌아 0을 돌려준다 — ML4b가 BS8d5로 넘긴 실제 Launch 성공 경로를 여기서 먼저 본다. 본문은 로더 표의 모듈 수와 부트 모듈 시작 여부를 찍는다. CI는 Windows·macOS 두 줄기에서 Debug 빌드·실행한다(`foundation.yml` "Build and run the Modular link check"). Windows Debug에서 dllimport 클래스의 inline 멤버는 DLL export를 부르므로, inline 본문이 숨은 기호를 직접 부르는 누락(macOS `-fvisibility-inlines-hidden`)은 macOS 줄기가 처음 드러낸다. 첫 실행(2026-11-22, Windows): 27개 모듈 공유 라이브러리를 싣고 부트 모듈을 시작해 0으로 끝났다. BS8d5(개정 22)에서 지웠다 — `FoundationModular`이 같은 모듈 폐포를 링크하고 같은 Modular 진입으로 시험까지 돌리므로 겹친다(CI 단계도 바꿨다).
- 프로젝트 파일: IDE 계획은 타깃의 링크 방식이 빌드할 수 있는 구성만 만든다(`LinkTypes.SupportsConfiguration` — 입력 검사와 같은 판정). Modular 타깃은 Debug·Development만 있고 다른 솔루션 구성에서는 빌드하지 않는다.
- Monolithic: 매크로가 빈 값이라 object가 바뀌지 않는다(기존 Monolithic 체인이 그대로 통과).
- 한계: 시험 소스가 부르는 모듈 내부 기호는 BS8d5다(시험용 내부 export — `Private/` 시험의 모듈 내 링크는 BD31 개정으로 버렸다). 모듈별 export 수 진단은 BS8d3이다.
- 버린 안: 모든 기호 export(Windows `.def` 자동 생성·CMake `WINDOWS_EXPORT_ALL_SYMBOLS` 식 — UE 방식이 아니고 export 수 상한에 닿는다), 공개 헤더의 모든 클래스·함수에 일괄로 붙이기(쓰지 않는 API까지 ABI로 굳힌다), 측정용 `/FORCE:UNRESOLVED`를 빌드에 남기기(실행 중에야 터진다).

**BS8d3 진단 세부(개정 19).**
- **상속 export 진단(메타 컴파일러, 링크 방식과 무관하게 늘)**: 다른 모듈이 상속하는 리플렉션 클래스는 클래스 전체를 export해야 한다(BD27 — 파생 클래스가 기반의 생성자·소멸자·out-of-line 가상 함수를 링크한다).
  - 판정: 메타 컴파일러가 리플렉션 클래스의 선언부 원문(Clang AST의 선언 범위 시작 — `class`·`struct` 키워드 — 부터 클래스 이름까지, 주석·문자열을 뺀 토큰)에 모듈 API 매크로 토큰(`ROCKS_<M>_API`)이 있는지 본다. Monolithic에서는 매크로가 빈 값이라 Clang AST에 export 속성이 없으므로 원문을 본다. 매크로는 그 클래스가 속한 모듈의 것이어야 한다 — 다른 모듈 이름의 매크로면 오류 RMC2007이다(Modular에서 import가 되어 정의가 export되지 않는다. 엔진 저장소는 정책 `module-api-macro`도 막지만 게임·SDK 사용자 모듈은 이 진단만 본다).
  - 한계: 매크로가 펼친 클래스 선언(선언 자체가 매크로 안)은 원문이 매크로 호출 자리라 판정하지 못한다(Exported 아님으로 본다). 리플렉션 클래스는 `ROCKS_CLASS()` 표지 뒤에 손으로 선언하므로 지금은 없다.
  - 기록: 모듈 descriptor(`<M>.rocksmeta.json`)의 클래스 `flags`에 `Exported`를 싣는다. 새 표지라 descriptor `schema`를 2로 올린다(옛 설치 SDK descriptor는 다시 만들라는 오류 — 하위호환을 두지 않는다).
  - 검사: 파생 모듈의 리플렉션 클래스의 base가 import한(다른 모듈의) 리플렉션 클래스인데 그 기반이나 그 위 import 조상(`rocks::Object` 아래까지 — 파생 클래스가 기반 사슬 전체의 특수 멤버를 링크한다)에 `Exported`가 없으면 오류 RMC2002다(파생 클래스 위치, 기반 이름과 모듈, 고칠 방법 — 기반 선언에 `class ROCKS_<기반 모듈>_API`).
  - 루트 `rocks::Object`는 리플렉션 타입이 아니어서 descriptor에 없다. 클래스 전체를 export하고(`class ROCKS_OBJECTRUNTIME_API Object`), 정책 `module-api-macro`가 그 선언을 필수로 본다.
  - struct 상속(B9k)은 대상이 아니다: 리플렉션 struct는 가상 함수가 없고 기반의 특수 멤버는 보통 inline이다(out-of-line이 필요하면 BS8d2 규칙대로 링크 오류가 알린다).
  - 지금 다른 모듈이 상속하는 리플렉션 클래스(저장소 전체 — EngineTests 폐포·게임 템플릿·플러그인·SDK 통합 시험): `Object`(ObjectRuntime ← AssetRuntime·World·시험 fixture), `Actor`·`SceneComponent`(World ← WorldTestFixtures, `SceneComponent`의 기반 `Component` 포함), `Asset`(AssetRuntime ← Animation·AssetTestFixtures), `reflection_fixture::Base`(ReflectionTestFixtures ← SDK 통합 시험의 게임 모듈 GameCore), 통합 시험의 게임 모듈 사이 `CoreThing`(GameCore ← SampleGame — 시험 문자열이 `ROCKS_GAMECORE_API`를 붙인다). 여기에 클래스 단위 매크로를 붙인다 — Engine 층의 첫 매크로다(Engine 층 나머지는 BS8d 뒤 Engine 층 Modular 확인 단계).
  - `ScriptExtensible` 클래스(UI 툴킷 7.3절 (f))의 자기 모듈 검사는 그 지정자를 구현할 때 더한다.
- **export 수 진단(빌드)**: Windows DLL 하나의 export 상한은 65535다(넘으면 링커가 오류). Modular Windows 빌드가 끝나면 공유 라이브러리마다 PE export 디렉터리의 함수 수를 읽어 빌드 출력에 모듈별 수를 한 줄로 남기고(`note: Modular exports: Core 4123, ...` — 큰 순서), 상한의 90%(올림 58982) 이상인 모듈은 따로 한 줄로 알린다(`ModularExports`). 세는 대상은 이번 계획이 공유 라이브러리로 링크한 모듈(`LinkTypes.CompilesAsSharedLibrary`)이고, 읽을 수 없는 파일은 그 모듈만 "읽지 못했다"는 줄로 남긴다(링크가 끝난 빌드를 진단 때문에 실패시키지 않는다)(상한에 닿기 전에 멤버 단위 export로 줄이라는 안내 — BD27 한계). macOS·Linux는 상한이 없어 세지 않는다.
- 시험: 메타 컴파일러 시험 — 같은 모듈 base는 매크로 없이 통과, import base에 `Exported`가 없으면 RMC2002(키워드와 이름 사이 주석 속 매크로는 세지 않는다), 있으면 통과, 다른 모듈 매크로는 RMC2007, 매크로가 실제 export 철자로 펼쳐지는 경우(Modular 메타 단계와 같다)도 `Exported`, descriptor 왕복에 `Exported` 표지, schema 1 descriptor 거부. 빌드 시험 Modular fixture — export 수 줄과 경고 문턱(문턱 값을 낮춘 판정 함수로).

**BS8d4 남은 키 세부(개정 21 — 사용자 결정 2026-11-22: UE 방식 생성 키 확장).**
- 문제: 리플렉션 struct·enum, Binds 미러의 실제 타입(`Color`·`Vec3`·`InterpCurve<…>` 등 Core 값 타입), 리플렉션하지 않는 native 등록 타입의 키(`NativeTypeKey<T>()`)는 inline 변수 주소(`NativeTypeIdentity<T>`)라 Modular에서 공유 라이브러리마다 따로 생긴다. 등록 모듈과 다른 모듈의 조회(`AcquireStructByNative`·`InstancedStruct` 타입 비교·경로 검사 등)가 실행 중에 엇갈린다. 컴파일·링크는 통과하므로 빌드가 잡지 못한다.
- 결정(UE 방식): UE가 리플렉션 struct·enum 정보를 생성 `.gen.cpp`의 export 함수(`StaticStruct`·`StaticEnum`)로, 엔진 기본 struct를 export 선언 목록(`TBaseStructure<FVector>` 등 — `CoreUObject/Public/UObject/Class.h`)으로 주는 것과 같이 한다.
  1. **리플렉션 struct·enum**: 클래스처럼(BS8b) 생성 헤더의 `GeneratedType<X>`가 `static ROCKS_<M>_API const void* NativeKey() noexcept`를 선언하고 생성 `.cpp`가 const가 아닌 정적 지역 저장소의 주소로 정의한다. `NativeTypeKey<X>()`는 그것을 쓰고, 리플렉션 struct(`ReflectedDeclarationV`)인데 생성 헤더가 보이지 않으면 `static_assert`다. enum은 본문 표지를 둘 수 없어(UE도 `UENUM`은 생성 헤더 include로 푼다) 생성 헤더 없는 쓰임을 판정하지 못한다 — 그때는 native 키가 되어 조회가 `NotFound`로 실패한다(조용한 손상은 아니고 시험이 잡는다). 리플렉션하지 않는 enum도 native 등록 타입으로 쓸 수 있어(시험의 지역 enum) 모든 enum을 막을 수는 없다.
  2. **Binds 미러의 실제 타입**: 실제 타입은 Core에 있어 표지를 둘 수 없다. ObjectRuntime이 그 목록을 변수 템플릿 `BoundCoreValueV<T>`로 둔다(UE `TBaseStructure` 목록 자리 — `Object/BoundCoreValues.h`, `Object.h`가 include). CoreTypeBindings 생성 코드가 미러의 실제 타입마다 `GeneratedType<실제 타입>::NativeKey()`를 낸다(지금도 실제 타입 이름으로 특수화한다). `NativeTypeKey<T>()`는 목록에 있는데 생성 헤더가 보이지 않으면 `static_assert`("include <CoreTypeBindings/…generated.h>"). 생성 소스가 미러마다 `static_assert(BoundCoreValueV<실제 타입>)`를 내 목록 빠짐을 컴파일 오류로 잡는다.
  3. **native 등록 타입(리플렉션하지 않는 클래스·struct — `RegisterNativeClass<T>` 등)**: 선언·정의 매크로 쌍 `ROCKS_DECLARE_NATIVE_TYPE_KEY(Type, Api)`(타입의 공개 헤더, 전역 namespace)·`ROCKS_DEFINE_NATIVE_TYPE_KEY(Type)`(그 모듈의 `.cpp` 하나)가 `NativeTypeKeyTraits<Type>::Key()`를 선언·정의하고 `NativeTypeKey<T>()`는 그것이 있으면 쓴다. 매크로가 없으면 inline 키다 — 그 타입을 한 모듈 안에서만 쓰면 충분하고(게임 모듈·시험의 native 파생이 대부분), 다른 모듈도 쓰면 매크로 쌍이 필요하다(없으면 조회가 `NotFound`로 실패한다 — 조용한 손상은 아니다).
  - 판정 위치: 생성 키·키 매크로가 보이는지는 `NativeTypeKey`의 템플릿 인자 기본값(`Generated`·`Keyed`)으로 정한다 — 본문 안에서 판정하면 보이는 번역 단위와 보이지 않는 번역 단위가 같은 이름의 다른 inline 정의를 가져(ODR 위반) 링커가 고른 정의에 따라 등록 키까지 바뀔 수 있다. 기본값으로 올리면 서로 다른 특수화라 실패가 결정적이다(enum은 NotFound). Binds 목록 판정은 cv를 뗀 타입으로 본다.
  4. **`StructRefArray` 원소 표**: `StructElementOps`에서 키(`nativeType`)를 뺀다. 원소 표는 `StructRefArray<S>` 필드를 가진 클래스의 inline 생성자 — 손으로 쓴 헤더 — 가 인스턴스화하므로 키를 담으면 그 헤더가 원소 struct의 생성 헤더를 include해야 한다(GSD6과 충돌 — 처음 시도에서 메타 컴파일러 단계가 깨졌다). 키를 쓰는 곳은 원소를 지울 때 그 원소의 간선을 등록 struct trace로 훑는 관찰자 경로뿐이라, 원소를 지우는 호출(`RemoveElementAt<S>`·`ClearElements<S>` → `RemoveStructElement`·`ClearStructElements`의 새 인자 `elementType`)이 `NativeTypeKey<S>()`를 넘긴다. 그 호출을 하는 번역 단위가 S의 생성 헤더를 include한다(클래스 키의 `World::With<Actor>`와 같은 규칙).
- **`ROCKS_GRAPH_SHARED_DATA`(BS8c 한계)**: 선언·정의로 나눈다 — `ROCKS_DECLARE_GRAPH_SHARED_DATA(Type, Api)`(공개 헤더 — `Index()`를 export 선언)·`ROCKS_DEFINE_GRAPH_SHARED_DATA(Type)`(`.cpp` 하나 — 정적 지역 색인). 공개 헤더에 정적 지역이 남지 않아 BS8c 판정 1과도 맞다. 같은 모양의 호스트 서비스 키(`ROCKS_DECLARE_HOST_SERVICE`)도 Modular에서 `Key()`를 export해야 하므로 `Api` 인자를 더한다(ModuleLoader 3.12절).
- 버린 안: 이름 기반 정규 키(사용자가 UE 방식을 골랐다 — BS8b 버린 안 그대로), native 등록 타입 매크로 의무화(한 모듈 안에서만 쓰는 대부분의 native 파생에 군더더기다).
- 시험: 메타 컴파일러 시험 — struct·enum 생성 헤더의 `NativeKey` 선언(API 매크로)과 생성 소스 정의, Binds 미러의 `BoundCoreValueV` 확인. 런타임 시험 — `NativeTypeKey`가 struct·enum·Binds 실제 타입에서 생성 키와 같은 주소, native 키 매크로 쌍. 컴파일 음성 시험(도구 시험) — 생성 헤더 없이 리플렉션 struct·Binds 실제 타입의 키를 쓰면 `static_assert`. Modular 왕복은 BS8d5의 Modular FoundationTests가 본다.

**BS8d5 Modular FoundationTests 세부(개정 22 — 사용자 결정 2026-11-22: 시험용 내부 export).**
- 측정(Windows Debug, `/FORCE:UNRESOLVED`는 측정에서만): FoundationTests를 Modular로 빌드하면 시험 소스(호스트 모듈 — ML4b)가 부르는 서로 다른 미해결 기호가 1,174개다. 공개 헤더 선언 359곳(BS8d2 규칙으로 export), 비공개 헤더·내부 기호 85곳(Crypto 내부, platform 내부, `detail`, 시험 훅 `BinnedAllocator::TestHooks` 등).
- BD31 개정(사용자 결정): BD31은 모듈 시험 묶음을 Modular에서 그 모듈 공유 라이브러리에 링크한다고 정했지만, 6개 모듈(ObjectRuntime·Compression·Curves·TraceCapture·PlatformBase·Base)은 시험이 그 모듈에 의존하는 모듈(시험 fixture·AsyncCompression·TraceAnalysis·Core)을 써서 공유 라이브러리 사이 순환이 생긴다. 그래서 시험은 지금처럼 시험 실행 파일 모듈(호스트 공유 라이브러리) 하나에 두고, 시험이 부르는 내부 선언에도 그 모듈의 API 매크로를 붙인다(시험용 내부 export). Modular는 개발 구성(Debug·Development)만이라 내부 export가 출시 게임(Release·Monolithic)에는 나가지 않는다. 다만 개발용 Modular 라이브러리(Editor — BD17, 설치 SDK의 엔진 공유 라이브러리 — BD30)에는 실린다(시험 훅 `SetCrashHelperOverride`·`BinnedAllocator::TestHooks`, Crypto 내부 함수 등) — 한계로 둔다. 막으려면 시험 훅 전용 조건 매크로(시험 빌드에서만 export)가 필요하고, Editor·SDK 배포를 다룰 때 정한다.
  - 비공개 헤더의 매크로: 정책 `module-api-macro`의 허용 목록(`PolicyChecks.ExportedPrivateHeaders`)에 그 헤더를 더한다(BS8d2 규칙 5의 예외 — 다른 모듈이 비공개 include로 보는 내부 헤더와 시험이 보는 내부 헤더). 목록에 없는 비공개 헤더의 매크로는 여전히 오류다.
  - 명시적 인스턴스(BS8d2 규칙 3 "템플릿에는 붙이지 않는다"의 예외): 모듈 .cpp가 명시적으로 인스턴스화하고 헤더가 `extern template`으로 다른 TU의 인스턴스화를 막는 템플릿은 그 선언에 매크로를 붙인다(`extern template ROCKS_CRYPTO_API ...` — Crypto `KeyBlob.h`). 정의 TU의 명시적 인스턴스 정의가 그 속성을 이어받아 export하고, 쓰는 모듈은 import한다.
  - 시험 등록 머리(MD20): 시험이 호스트 하나에 있으므로 등록 목록 머리(`first`)는 시험 runner(호스트)에 한 벌이다. Core로 옮기지 않는다.
- 시험 타깃 `FoundationModular`: Modular Program 타깃, 실행 파일은 FoundationTests 하나(ML4b — Launch 시험 프로그램·내장 진입 프로그램 제외), `TestExecutable`은 FoundationTests. 실행은 ML4b Modular 진입(진입 스텁 → Launch → manifest 로더 → 호스트 앱 기술 → 시험 runner)이다. 로컬 체인과 CI(Windows·macOS — `foundation.yml` "Test FoundationTests as Modular")가 `/t:Test`로 돈다 — BS8·ML4 완료 기준. Modular `/t:Test`는 native 시험 실행 파일과 native 하니스(크래시·Launch — BS8d6, 아래 세부)를 돈다. host self·meta 검사는 링크 방식과 무관한 도구 검사라 Monolithic 시험 타깃(`Foundation`)이 돈다. BS8d5 때는 native 시험만 돌았다. 확인 타깃 `ModularCheck`를 대신한다(겹치므로 지웠다). 첫 실행(2026-11-22, Windows Debug): 2,698개 시험 통과.
- 정적 모듈 표를 쓰는 시험(DynamicModuleTests·ModuleLoaderTests의 `GeneratedModuleTable()`): Modular에는 그 표가 없으므로 그 시험은 로더 표(`test::ProcessModules().Table()`)를 쓰거나 Monolithic에서만 돈다(`ROCKS_MONOLITHIC`) — 시험이 보는 것이 무엇인지에 따라 하나를 고른다.
- `constinit`과 dllimport: Windows에서 다른 공유 라이브러리가 export한 함수·변수의 주소는 상수식이 아니다(dllimport — `__imp_` 포인터를 읽는다). 그래서 상수 초기화(`constinit` CVar·상수 표)가 다른 모듈의 함수 주소를 담으면 Modular에서 컴파일 오류다. 그 자리는 이 모듈 안의 inline 함수(템플릿)로 감싸 그 주소를 담는다 — 예: CVar의 리플렉션 enum 스키마 함수(`cvar::detail::ReflectedEnumSchema<E>`).
- 한계:
  - 헤더 inline 함수의 주소: inline 정의는 그 함수를 쓰는 공유 라이브러리마다 사본이 생겨(Windows — 이미지마다, POSIX — `RTLD_LOCAL`) 주소가 모듈마다 다르다. 함수 주소를 비교하거나 표 key로 쓰는 코드(생성 ops의 `migrate`처럼 사용자 함수 주소를 담는 자리와 그 주소를 비교하는 시험)는 그 함수를 out-of-line으로 정의하고 export한다 — 예: fixture `StructLeaf::Migrate`. 시험 실행으로 찾았다(FoundationModular 2,698개 중 이 2개만 실패).
  - thread_local 변수는 export할 수 없다(dllexport·dllimport 불가). 시험이 다른 모듈의 thread_local 상태를 바꾸면 그 모듈 안의 export 함수로 감싼다 — 예: `ThisThreadVirtualMemoryFaults()` — header-only 시험 접근점 `VirtualMemoryTestAccess`가 이 함수로 주입 상태에 닿는다(접근점은 제품 소스가 include하지 않는다 — 정책 `test-access-include`).
  - 클래스 단위 dllimport는 그 클래스 멤버 함수의 주소를 상수식에서 쓰지 못하게 한다(dllimport 함수의 주소는 상수식이 아니다 — constexpr 멤버 호출은 된다). consteval 검사가 멤버 함수 주소를 쓰는 클래스(`Formatter<DateTime>`·`Formatter<TimeSpan>`·`Formatter<Guid>` — 로그 spec 검사가 `&Formatter<U>::IsValidSpec`를 쓴다)는 클래스 단위가 아니라 그 밖의 멤버(`Format`)에만 매크로를 붙인다.
  - 무할당 시험(`OperatorNewCounter.cpp`의 전역 `operator new` 대체)은 Modular에서 호스트 이미지 안의 할당만 센다(Windows는 이미지마다, POSIX는 `RTLD_LOCAL`). 다른 모듈 안의 `operator new`는 세지 않으므로 그 시험은 Modular에서 약해진다 — Monolithic 체인이 원래 강도로 본다.
  - 부트 모듈 두 번 적재(ML4b 한계 — 실행 파일 import와 로더의 절대 경로 적재가 다른 철자로 같은 DLL을 두 번 싣는 경우)는 실행 중에 확인하지 않는다. manifest 로더의 경로는 실행 파일 이미지 경로(`GetExecutablePath` — Windows `GetModuleFileNameW`)의 디렉터리이고, 이는 OS가 실행 파일 import를 찾는 응용 프로그램 디렉터리와 같은 철자라 보통은 같은 모듈이다. 확인하려면 주소가 속한 이미지를 찾는 플랫폼 API가 필요해 요구가 생길 때 더한다(FoundationModular 시험이 두 번 적재된 Core의 공유 상태 — 로그·할당자 — 를 쓰면 실패하므로 같은 철자 경로의 회귀는 시험이 드러낸다).

**BS8e Engine 층 Modular export 세부(개정 23).**
- 범위: Engine 층 모듈(Engine/Source)과 Engine 시험이 부르는 Core 층 선언을 Modular로 링크되게 한다. Editor(BD17 — Modular)가 엔진 모듈 공유 라이브러리를 싣기 전에 export 누락을 시험 빌드로 먼저 잡는다(BD31과 같은 이유). 규칙은 BS8d2(붙이는 자리)와 BS8d5(시험용 내부 export·비공개 헤더 허용 목록·inline 함수 주소·thread_local)를 그대로 쓴다.
- 측정(2026-11-22, Windows Debug, `/FORCE:UNRESOLVED`는 측정에서만): Engine 시험을 Modular로 빌드하면 매크로를 붙일 선언이 364곳이다(빌드를 세 번 돌려 링크가 진행되며 드러난 것까지). 많은 순으로 AssetRuntime 68·Graphics 40·GraphicsTypes 32·AssetDatabase 29·Animation 25·SparksCompiler 23·AssetNugget 19·SparksVm 17·Mesh 16·RenderBase 15이고, Core 층 6곳(`Log`·`ModuleTypeRegistrations`·Trace 함수 둘·`LogicalProcessorCount`·HostConfig `FindEngineDirectory`)도 있다. 비공개 헤더는 AssetRuntime `PackageInternal.h`·EngineAssets `EngineAssetsInternal.h`·MeshTools `MeshBuildInternal.h` 셋이다. friend 재선언(`InstantiatePackages`·`ParseLogicalPath`·`LinkModule`·`LinkImports`·`TryParseExpectation`)에도 붙인다.
- 분담: 모듈을 맡은 세션이 있으면 그 세션이 목록을 받아 붙인다(Animation·Mesh·MeshSource·MeshTools — rocks-ff, 55곳). 나머지는 이 단계가 붙이고 그 세션들에 알린다.
- 시험 타깃 `EngineModular`: Modular Program 타깃, 실행 파일은 시험 모듈 `EngineModularTests` 하나(`EngineTests`와 같은 소스, 엔진 플러그인 시험 제외). 로컬 체인과 CI(Windows·macOS)가 `/t:Test`로 돈다(native 시험과 native 하니스 — BS8d6).
- 엔진 플러그인 제외: `EngineTests`는 켠 엔진 플러그인의 시험 모듈(`PluginTestModules` — UnrealExchange)을 링크하는데, `Modular` 타깃의 플러그인은 플러그인 binary manifest(플러그인마다 `<Plugin>/Binaries/<Platform>/` — [모듈 로더](ModuleLoader.ko.md) MD12, ML5) 전까지 거부한다(BD41). 그래서 Modular 시험 모듈은 플러그인 시험을 빼고(`TestsModule.PluginTests`), 플러그인 모듈의 export는 ML5에서 한다. 플러그인 manifest를 지금 앞당기지 않은 것은 플러그인 활성 조건 해석·빌드 목록 대조와 한 묶음(ML5 — Rocks Studio)이기 때문이다.
- 정적 모듈 표를 쓰는 Engine 시험(ModuleLoader 3.14절): `EngineAssets_ModuleEntryRegistersSettings`는 프로세스 로더 표(`test::ProcessModules()`·`EntryAt`)로 옮겼고, 정적 표로 새 로더를 만드는 `GpuTrace_RenderBaseModuleRegistersThroughTheTraceService`는 `ROCKS_MONOLITHIC`으로 감쌌다.
- 타입 키(Core `Core/TypeKey.h`): 헤더의 변수 템플릿 주소를 타입 표지로 쓰던 세 곳은 Modular에서 이미지마다 주소가 달라 모듈 사이 조회가 갈라진다 — BD29가 금지한 주소 식별 정적 객체인데 템플릿 문맥이라 BS8c 검사가 보지 못했다. 그래서 BS8d4의 native 등록 타입 키 기구(`NativeTypeKeyTraits`·`ROCKS_DECLARE_NATIVE_TYPE_KEY`·`ROCKS_DEFINE_NATIVE_TYPE_KEY`·이미지 지역 표지 `NativeTypeIdentity`)를 ObjectRuntime에서 Core로 내리고 `TypeKeyOf<T>()`(선언한 export 키, 없으면 이미지 지역 표지 — 선언이 보이는지는 템플릿 기본 인자로 판정, BS8d4와 같은 ODR 이유)를 두었다. `NativeTypeKey`(리플렉션 생성 키 우선)는 그 위에 있다. 다른 모듈이 같은 키를 써야 하는 타입은 export 키를 선언한다.
  - Subsystem(`World/Subsystem.h` `SubsystemKeyOf`): 다른 모듈(시험·게임)의 `GetSubsystem<EngineAssetsSubsystem>()`이 nullptr였다(EngineModular 첫 실행에서 찾음 — UE는 `UClass*`). `EngineAssetsSubsystem`이 export 키를 선언한다.
  - 렌더 설정 참조(`RenderBase/RenderSettings.h` `RenderSettingsRef`): 게임 lane이 담고 렌더러 모듈이 꺼낸다. RenderBase는 ObjectRuntime에 의존하지 않으므로(렌더 층 "Object 없음") Core 기구를 쓴다. `ConfigSnapshot`(Config)이 export 키를 선언한다. 옛 표지는 `const`라 Monolithic에서도 ICF가 서로 다른 타입의 주소를 합칠 수 있었다.
  - 콘솔 명령 대상(`Config/CVarCommand.h` `CommandContext::Target<T>`·`ConfigContext::BindCommand`): 대상을 묶는 모듈과 명령 함수의 모듈이 다르면 Modular에서 Fatal이었다. 대상 타입(`int32` 같은 기본 타입 포함)은 보통 한 모듈 안이라 이미지 지역 표지로 충분하고, 모듈을 넘으면 그 타입이 키를 선언한다. 옛 표지도 `const`였다.
  - 정책 `public-header-type-tag`: 엔진 소유 모듈 공개 헤더에서 같은 헤더의 변수 템플릿 선언과 그 주소(`&Name<`)가 함께 있으면 거부한다(`Core/TypeKey.h`만 예외). 어휘 검사라 다른 헤더에 선언한 변수 템플릿의 주소는 보지 못한다.
- 이름 상수의 주소: 헤더의 `inline constexpr char[]`(예 `EngineStageReady`)도 이미지마다 사본이다(inline 함수 주소와 같은 이유 — BS8d5). 계약이 문자열 내용인 자리(크래시 기록 단계 이름)는 내용으로 비교한다(시험 `Engine_MarksRunStagesForCrashRecords`).
- 한계: Engine 층 크래시·Launch 하니스는 BS8d6이 Modular에서도 돌린다(아래). 엔진 플러그인 모듈의 Modular 링크는 ML5다.

**BS8d6 Modular 크래시·Launch 하니스 세부(개정 25).**
- 범위: Modular 시험 타깃(`FoundationModular`·`EngineModular`)의 `/t:Test`가 native 시험 실행 파일 뒤에 native 하니스(크래시 수집·Launch·로그 크래시·컴파일 계약 시험 — `Rocks.ToolTests native`)도 돈다. Editor(BD17)가 Modular라 크래시 수집(도우미·덤프·cdb 스택)과 Launch 흐름(Modular 진입 — [모듈 로더](ModuleLoader.ko.md) 3.14절)을 Modular 실행 파일로 확인한다. host self·meta 검사는 링크 방식과 무관한 도구 검사라 지금처럼 Monolithic 시험 타깃만 돈다.
- 첫 실행(2026-11-23, Windows Debug, FoundationModular): 192건 중 44건 실패 — cdb 스택 검사 29건, Launch 시험 프로그램 없음 15건(그중 내장 진입 1건), 도우미 실패 주입 4건(실패 줄로는 cdb 검사와 겹친 것이 있다).
- **Launch 시험 프로그램은 Modular 타깃을 따로 둔다**(결정, UE 방식): Modular 링크는 실행 파일 하나마다 manifest(`<Target>.modules.json`)와 빌드 정보(Core 공유 라이브러리의 정의 TU 하나 — BS8d1)를 만들므로 Modular 타깃의 실행 파일은 하나다(입력 검사가 거부한다). UE도 Modular 프로그램마다 타깃이 따로다(`ShaderCompileWorker`처럼 target 하나 = 실행 파일 하나 = `.modules` 하나). 그래서 `LaunchProbeModular`·`LaunchWindowedProbeModular`(Program, Modular, 실행 파일 `LaunchProbe`·`LaunchWindowedProbe`)를 더한다. 내장 진입 프로그램(`LaunchEmbeddedHost`)은 Modular를 지원하지 않으므로(ModuleLoader 3.14절) 타깃이 없다.
  - 버린 안: Modular 타깃 하나에 실행 파일 여럿(manifest를 실행 파일마다 두기). 빌드 정보 ID가 실행 파일의 링크 폐포로 계산되는데(ReflectionFoundations 8.1절) Modular에서는 Core 공유 라이브러리 하나에 정의 TU가 하나뿐이라, 실행 파일이 여럿이면 빌드 정보의 뜻(타깃 단위 ID)을 새로 정해야 한다. UE에도 없는 구조라 버린다.
- **시험 동반 타깃 `TargetRules.TestPrograms`**: 시험 타깃이 하니스에 필요한 다른 타깃 이름을 적는다(기본 빈 목록). `test`는 native 시험 실행 파일 뒤, 하니스 전에 그 타깃들을 같은 구성·도구 체인·빌드 방식으로 빌드하고(출력은 각 타깃의 기본 출력 디렉터리이고, `--output <경로>`를 준 `test`는 `<경로>-<타깃 소문자>` 형제 디렉터리다 — 시험 타깃 출력과 겹치지 않고 같은 `out` 아래다. CI가 `/p:NativeOutput`으로 출력을 정한다. 실행 파일 경로는 동반 빌드 보고서 `action-report.json`의 모듈 출력에서 읽는다) 실행 파일 경로를 하니스에 `--program <이름>=<경로>`로 넘긴다. 하니스는 넘겨받은 프로그램을 쓰고, 없는 이름은 지금처럼 시험 실행 파일 옆에서 찾는다(Monolithic 타깃은 Launch 시험 프로그램을 같은 타깃에 둔다 — 바뀌지 않는다). `FoundationModular`·`EngineModular`가 `LaunchProbeModular`·`LaunchWindowedProbeModular`를 적는다. `CompanionTargets`(개정 24 — 동반 타깃의 모듈을 이 타깃 빌드에 합쳐 같은 출력 폴더에 두는 Monolithic 전용 속성)와 다르다: 시험 동반 타깃은 Modular 시험 타깃이 쓰고, 타깃마다 따로 빌드해 자기 출력(manifest·빌드 정보)을 가진다. 동반 타깃은 `Program`이고 실행 파일 모듈 하나만 고르며 시험 실행 파일·동반 타깃이 없어야 한다(재귀를 막는다). 이름은 대소문자까지 구별하고 중복은 오류다(모두 `ArgumentException`).
- **cdb 스택의 모듈 이름**: Modular에서는 크래시 probe 함수가 실행 파일이 아니라 호스트 공유 라이브러리(`<Target>-<Module>[-<Platform>-<Configuration>].dll`) 안에 있고, cdb는 이미지 이름의 식별자가 아닌 글자를 `_`로 바꿔 모듈 이름으로 쓴다(`FoundationModular_FoundationTests_Windows_Debug!rocks::test::RunCrashProbe`). 그래서 `test`가 Modular면 하니스에 호스트 공유 라이브러리 경로를 `--host-module <경로>`로 넘기고, 하니스는 스택 프레임·`lm v m`·private PDB 검사를 그 이름으로 한다. 실행 파일 이름(크래시 폴더 이름·`program`·PDB 이름 칸)은 그대로 실행 파일이다.
- **도우미 실패 주입 시험은 Monolithic만**: 실패 주입(`ROCKS_CRASH_PROBE_HELPER` — 시험 도우미 본문 대체·캡처 gate, `InstallCrashHelperTestMode`)은 시험 호스트 모듈의 정적 초기화가 건다. Modular 도우미 프로세스는 진입 스텁의 도우미 분기에서 갈라져 호스트 공유 라이브러리를 싣지 않으므로(ModuleLoader 3.14절 1단계) 주입이 걸리지 않는다. 시험 대상(부모의 도우미 실패 처리 — PlatformBase)은 링크 방식과 무관한 코드라 Monolithic 하니스가 보고, Modular 하니스는 이 네 시험(`helper-capture-timeout`·`helper-exit-during-capture`·`helper-exited-early`·`helper-gate-parent-first`)을 등록하지 않는다. 정상 도우미 경로(도우미가 덤프를 씀)는 Modular의 다른 크래시 시험이 본다.
  - 버린 안: 진입 스텁에 시험 주입 고리를 두기(시험 전용 코드가 모든 Modular 실행 파일 진입에 들어간다), 도우미 프로세스가 호스트 모듈을 싣기(도우미는 시동 비용과 실패 면을 줄이려고 Launch 상태·할당기·로그 없이 갈라진다 — CrashHandlingAndLaunch).
- 내장 진입 시험(`Native.Launch.embedded-host`)도 Modular에서는 등록하지 않는다(프로그램이 없다).
- **시험 실행기 이름**: 크래시 폴더 이름·`program`은 앱 기술 이름이고 하니스는 실행 파일 이름으로 찾는다. 실행기 이름을 층 번호(`FOUNDATION_TEST_RUNNER_LAYER`)로 고르던 것을 빌드가 정의하는 모듈 이름(`ROCKS_MODULE_NAME_TEXT` — 시험 실행 파일 모듈 이름이 실행 파일 이름이다)으로 바꿨다 — `EngineModularTests`는 Engine 층이지만 이름이 다르다(EngineModular 하니스 첫 실행 37건).
- **정적 소멸 없는 종료(하니스가 찾은 제품 결함)**: `RequestExitWithoutStaticTeardown`은 `_Exit`로 끝냈는데, Windows `_Exit`는 `ExitProcess`를 불러 DLL마다 `DLL_PROCESS_DETACH`가 오고 DLL의 CRT가 그 DLL의 정적 소멸자를 돌린다. 그래서 Modular에서는 호스트 공유 라이브러리의 정적 소멸이 돌았다(`Native.Launch.static-teardown`·`off-no-teardown`). PlatformBase `ExitProcessWithoutTeardown`(Windows `TerminateProcess(GetCurrentProcess(), code)` — UE `FPlatformMisc::RequestExit(true)`와 같다, POSIX `_Exit`)으로 바꿨다([크래시 처리와 실행](CrashHandlingAndLaunch.ko.md) 8.2절 개정 14).
- 시험: 하니스 자체가 시험이다. 로컬 체인과 CI(Windows "Test FoundationTests as Modular"·"Test EngineTests as Modular", macOS)가 Modular `/t:Test`로 돈다. CLI 시험 — 동반 타깃의 검사(시험 실행 파일이 있는 동반 타깃·없는 타깃 이름·중복 거부, 기본 출력과 `--output` 형제 출력).
- 한계: 동반 타깃은 따로 빌드하므로 Core 등 공유 모듈을 타깃마다 다시 컴파일한다(출력 디렉터리가 타깃마다다 — 시험 시간이 는다). macOS의 cdb 검사는 원래 없다(lldb 검사가 생기면 같은 이름 규칙을 쓴다).

**모듈 API 매크로(BD27).**
- 빌드가 C++ 모듈마다 `ROCKS_<MODULE>_API`(모듈 이름 대문자)를 정의한다. UE `<MODULE>_API`(`UEBuildModule.cs:223, 239`)에 해당하고, 이름은 Rocks 매크로 접두 규칙을 따른다.
- 값은 UE와 같다(`UEBuildModule.cs:750-770`). `Monolithic`이면 빈 값이다. `Modular`이면 그 모듈 자신의 컴파일에서는 export, 다른 모듈의 컴파일에서는 import다.
- export·import 철자는 `Base/Compiler.h`의 `ROCKS_DLLEXPORT`·`ROCKS_DLLIMPORT`다. Windows는 `__declspec(dllexport)`·`__declspec(dllimport)`, macOS·Linux는 둘 다 `__attribute__((visibility("default")))`다.
- `Modular` 빌드의 가시성 옵션은 UE와 같다(`bHideSymbolsByDefault`). Linux는 공유 라이브러리 컴파일에 `-fvisibility-ms-compat`(`Platform/Linux/LinuxToolChain.cs:432-446` — 공유 라이브러리가 아닐 때만 `-fvisibility=hidden`), macOS는 `-fvisibility-ms-compat`·`-fvisibility-inlines-hidden`(`Platform/Mac/MacToolChain.cs:158-162`)이다. 함수·변수는 기본으로 숨겨지고 타입 정보는 보이므로, 함수·변수의 export 누락이 세 플랫폼 모두에서 링크 오류로 드러난다. 예외: 클래스 단위로 export한 클래스의 inline 멤버가 같은 모듈의 export하지 않은 내부 기호를 부르는 누락은 Windows에서 드러나지 않는다(dllimport 클래스의 inline 멤버는 DLL 쪽 사본을 부른다). macOS는 그 inline 멤버가 쓰는 쪽에 펼쳐져(`-fvisibility-inlines-hidden`) 링크 오류가 나므로 macOS CI가 잡는다(BS8e 리뷰).
- 붙이는 곳: 다른 모듈이 부르는 inline이 아닌 함수·전역 변수, 클래스 전체(`class ROCKS_CORE_API Foo`) 또는 멤버 함수 하나씩. inline·`constexpr`·템플릿 정의에는 붙이지 않는다(UE와 같다).
- 규칙 파일·명령줄은 `ROCKS_*_API`와 `ROCKS_MONOLITHIC`을 정의할 수 없다(빌드 예약 — `InputValidation.cs:163`의 예약 접두 목록에 BS2가 `ROCKS_WITH_`를 더할 때 같이 더한다). 메타 컴파일러의 Clang 실행(`MetadataRunner`)과 공개 헤더 검사(BD9)도 같은 정의를 받는다.
- 정책 검사: 모듈 API 매크로는 자기 모듈의 매크로만 쓴다(다른 모듈 이름의 `_API`를 붙이면 오류).
- **다른 모듈이 상속하는 클래스**: 파생 클래스는 기반의 생성자·소멸자·out-of-line 가상 함수를 링크해야 한다. 그래서 다른 모듈에서 상속하는 리플렉션 클래스와 `ScriptExtensible` 클래스(UI 툴킷 7.3절 (f)의 `ScriptProxy<X>`가 X를 상속한다)는 클래스 전체에 API 매크로를 붙여야 한다. 메타 컴파일러가 파생 모듈을 처리할 때 기반이 다른 모듈에 있고 클래스 단위 export가 없으면 오류로 알린다(`ScriptExtensible`은 자기 모듈에서 바로 검사한다).
- 한계: Windows DLL 하나의 export 수 상한은 65535다. 클래스 전체 export와 BD28이 겹치면 큰 모듈이 이 상한에 닿을 수 있다. BS8은 모듈별 export 수를 진단하고, 상한에 가까우면 멤버별 export로 줄인다.

**리플렉션 생성 코드(BD28).**
- 지금 메타 컴파일러는 생성 헤더(`.generated.h`)만 낸다(`Programs/Rocks.MetaCompiler/CompilerOptions.cs:33`). 타입 정보는 `GeneratedType<X>` inline 특수화이고, 등록 함수 `RegisterModuleTypes`도 inline이다(`CppEmitter.cs:91`). 타입 식별은 주소를 쓴다.
  - 네이티브 타입 키 `NativeTypeKey<T>()`는 `inline uint8 NativeTypeIdentity<T>`의 주소다(`Core/Source/Runtime/ObjectRuntime/Public/Object/Object.h:78-92`, 주석이 "동적 모듈 사이 키 통합은 보장하지 않는다"고 적는다).
  - 모듈 키와 `Schema()`도 inline 함수 안 정적 객체의 주소다.
  - `Modular`에서는 이 객체들이 공유 라이브러리마다 따로 생겨 같은 타입이 모듈마다 다른 키를 갖는다. `ObjectCast`·`IsNativeA`·등록 중복 판정이 깨진다.
- UE는 모듈마다 생성 `.gen.cpp`를 두고, 타입 정보(`StaticClass` 등)를 그 안의 inline이 아닌 함수로 정의한다. 다른 모듈은 export된 그 함수를 부른다. Rocks도 같이 한다.
  - 메타 컴파일러는 모듈마다 생성 `.cpp`를 더 낸다. 타입마다 타입 키 저장소, 타입 정보·스키마 객체, 모듈 등록 함수를 여기에 정의하고 모듈 API 매크로로 export한다. 생성 헤더에는 선언과 inline 접근 함수만 남는다.
  - 생성 코드를 헤더에서 소스로 옮기는 부분의 구현 스펙은 [리플렉션 생성 소스](ReflectionGeneratedSources.ko.md)다. 컴파일 시간 때문에 BS8보다 먼저 한다. 생성 소스는 리플렉션 헤더마다 하나, 모듈마다 등록용 하나다(GSD2). 모듈 API 매크로 주입과 `NativeTypeKey` 교체는 BS8에 남는다.
  - `NativeTypeKey<T>()`는 생성 `.cpp`가 정의한 그 타입의 키 저장소 주소를 돌려준다. 리플렉션하지 않는 타입에는 키가 없다(지금도 키는 등록 타입 조회에만 쓴다). 키 저장소는 ICF로 합쳐지지 않게 const가 아닌 객체로 둔다(지금 주석과 같은 이유).
  - 이 구조는 `Monolithic`에서도 같다. 링크 방식에 따라 생성 코드가 달라지지 않는다.
- 그래서 모든 리플렉션 타입을 다른 모듈에서 조회·생성·`ObjectCast`([UI 툴킷](UiToolkit.ko.md) 7.3절 (f))할 수 있다. UE는 클래스를 export하지 않을 때 `UCLASS(MinimalAPI)`로 타입 정보만 export한다(지정자가 없으면 다른 모듈에서 `StaticClass`를 참조할 수 없다). Rocks는 타입 정보 export가 기본이라 이 지정자를 두지 않는다(7절).
- 클래스 멤버 함수의 export는 작성자가 API 매크로로 정한다(UE와 같다, 상속 규칙은 BD27). 생성 대리 클래스 `ScriptProxy<X>`(UI 툴킷 7.3절 (f))는 X의 모듈의 생성 `.cpp`에 정의되고, 레지스트리는 생성 함수 포인터로 부르므로 따로 export하지 않는다.

**모듈 사이 공유 상태(BD29).**
- 전역 상태는 소유 모듈에 한 벌만 둔다. 공유 라이브러리는 inline 변수, inline·템플릿 함수 안의 정적 지역 변수, 헤더에서 정의한 템플릿 정적 데이터 멤버를 라이브러리마다 따로 가진다(Windows는 언제나, ELF·Mach-O도 숨김 가시성에서). 그래서 공개 헤더에 다음을 두지 않는다.
  - 쓰기 가능한 정적 저장소: const가 아닌 `inline` 변수, inline·템플릿 함수 안의 const가 아닌 `static` 지역 변수, 헤더에서 정의한 const가 아닌 정적 데이터 멤버.
  - `thread_local` 변수(선언 포함). DLL 경계로 import할 수 없다.
  - 주소를 식별자로 쓰는 정적 객체. const여도 공유 라이브러리마다 주소가 다르다(BD28의 타입 키·모듈 키).
  - 대신 소유 모듈의 cpp에 두고 export한 접근 함수로 노출한다. `constexpr`·const 상수는 괜찮다.
- 판정은 공개 헤더 검사(BD9)가 헤더마다 하는 컴파일에 Clang 진단 `-Wunique-object-duplication`을 켜서 한다(개정 13 — 처음에는 Clang AST로 정했으나 비용 때문에 바꿨다, BS8c 구현 세부). 그 진단이 보지 못하는 `thread_local` 선언은 정책 `public-header-static-storage`가 어휘로 본다. Monolithic·Modular와 무관하게 늘 검사해, Editor 빌드에서만 드러나는 상태 분열을 막는다.
- 설계 당시 위반의 예(개정 2 기준 줄 번호 — `thread_local`·fixture는 BS8c에서 모두 고쳤고, `NativeTypeIdentity`는 클래스만 BS8b에서 바꿨다):
  - `thread_local`: `Config/ConfigSnapshot.h:215`(`inline thread_local`), `Core/Diagnostics/Correlation.h:139`(`extern thread_local`), `Core/Memory/BinnedAllocator.h:60`·`DebugAllocator.h:84`·`ExternalAllocator.h:71`·`PoolAllocator.h:68`(`static thread_local` 멤버), `TraceLogSink.h:46`.
  - 주소 식별: `Object/Object.h:78-92` `NativeTypeIdentity`(BD28로 바꾼다).
  - 매크로가 펼치는 정적 지역 변수: `TraceMark.h:177`(inline 함수 안에서 쓰면 위반이다).
  - 시험 fixture: `ValidateFixture.h:63`, `ReflectionFixture.h:24-25, 130, 132`(쓰기 가능한 `static inline`).
- 할당·해제: 모든 모듈은 같은 공유 CRT(Windows `/MD`·`/MDd` — `ActionGraph.cs:491`)를 쓰므로 한 모듈이 할당한 메모리를 다른 모듈이 해제해도 된다. 엔진 할당기 상태는 Core가 소유한다. UE는 모듈마다 전역 `operator new`·`delete`를 바꾼다(정의 `Runtime/Core/Public/Modules/Boilerplate/ModuleBoilerplate.h:44-73`, 모듈마다 넣는 `PER_MODULE_BOILERPLATE` 115-117행). Rocks는 전역 `operator new`를 바꾸지 않으므로 해당하지 않는다. 바꾸게 되면 같은 모듈 boilerplate로 모든 모듈에 넣는다.
- 서드파티 모듈(소스로 빌드하는 정적 라이브러리)은 UE와 같이 `Modular`에서 그것을 직접 소비하는 모듈의 공유 라이브러리마다 링크된다. 두 모듈이 직접 소비하면 코드가 두 벌이 된다. 상태가 없는 라이브러리는 이것으로 충분하다(지금 Zstd는 Compression·CompressionTraining, SpirvReflect는 ShaderFormatVulkan·시험·ShaderToolSpike가 직접 소비한다). header-only 서드파티는 해당하지 않는다.
  - 프로세스 전역 상태가 있는 서드파티(초기화 상태·전역 할당기 훅 등)는 규칙에 `HasGlobalState`를 표시한다. 그 모듈을 직접 소비하는 Rocks 모듈은 하나여야 하고(규칙 로드 때 오류), 다른 모듈은 그 모듈의 Rocks API를 거친다. 지금 서드파티 규칙마다 표시 여부를 BS8에서 정한다.

**모듈 수명과 로더(BD30).**
- UE는 모듈마다 `IMPLEMENT_MODULE`로 진입점을 둔다(`Runtime/Core/Public/Modules/ModuleManager.h:943-971`). `Monolithic`에서는 정적 초기화로 등록하고, `Modular`에서는 `FModuleManager`가 DLL을 적재한 뒤 진입점을 부른다. 플러그인 모듈은 플러그인 관리자가 로딩 단계별로 적재한다.
- 모듈 수명 객체·진입점 매크로(Core)와 Monolithic 로더(`ModuleLoader` 모듈)는 [모듈 로더](ModuleLoader.ko.md) ML0으로 구현했다. 리플렉션 등록은 아직 생성 함수 `RegisterModuleTypes`를 명시 호출하고(로더 연결은 ML2), Modular 적재는 ML4다. 별도 스펙 [모듈 로더](ModuleLoader.ko.md)로 정했다. 그 단계 ML0–ML4는 BS5 전에, ML5는 BS5와 함께 구현한다. 이 문서가 그 스펙에 요구하는 계약은 다음과 같다.
  1. 같은 소스가 `Monolithic`과 `Modular`에서 같은 순서·결과로 동작한다. 모듈 진입점 매크로가 `ROCKS_MONOLITHIC`으로 두 방식을 가린다.
  2. 모듈 적재·시작은 의존 순서, 종료·해제는 그 역순이다. 리플렉션 타입 등록은 모듈 시작에 묶는다.
  3. Rocks Studio는 프로젝트 기술자(`.rocksproject`)와 플러그인 활성 조건(3.6절)을 읽어 게임·플러그인 모듈의 공유 라이브러리를 실행 중에 적재한다.
  4. 적재 실패(파일 없음, 진입점 없음, 기대한 SDK·빌드 식별자와 다름)는 원래 OS 오류 코드를 보존한 진단으로 끝낸다.
- 코드 다시 불러오기(UE Live Coding — `TargetRules.cs:1587-1590`)는 이 문서와 모듈 로더 스펙의 범위 밖이다(9.2절).

**공유 빌드 환경.**
- UE는 `Modular` 타깃의 엔진 모듈을 여러 프로젝트가 공유한다(`TargetBuildEnvironment.Shared` — `TargetRules.cs:2859-2862`). Rocks도 같다. 설치 SDK의 Editor 변형(BD5)이 Rocks Studio 실행 파일과 엔진 모듈 공유 라이브러리를 한 벌 싣는다. 게임의 Editor 타깃은 게임 모듈과 프로젝트 플러그인 모듈의 공유 라이브러리만 만든다.
- 그래서 게임의 Editor 타깃은 엔진 모듈의 컴파일 환경(define·컴파일 옵션)을 바꿀 수 없다. 바꾸는 타깃 설정은 규칙 로드 때 오류다. 설치 엔진의 UE도 같다(UE 소스 빌드는 그때 고유 환경으로 바꾸지만, Rocks 게임은 설치 SDK만 쓴다).
- SDK 변형 key(BD5)에 링크 방식을 더한다. Editor 변형은 `Modular`이고, 다른 변형은 `Monolithic`이다.

### 3.9 엔진 플러그인 빌드

엔진 저장소의 타깃(EngineTests·Commandlet·Engine)이 `Engine/Plugins` 아래 플러그인을 찾고, 타깃이 켠 플러그인의 모듈만 빌드·링크한다(2026-11-21, 사용자 지시 "진행 가능한거부터" — Unreal 교환 리더 UX1a의 선행 조건). 3.6절의 프로젝트 쪽 활성 조건(BS4)과 별개로, 프로젝트가 없는 엔진 타깃의 경로를 정한다. 지금은 규칙 어셈블리가 `Engine/Plugins/**/*.Build.cs`를 이미 모으지만(Rocks.Engine.Rules.csproj) 엔진 빌드 경로 어디에도 `PluginCatalog`·`DescriptorModuleSet`이 없다. 그래서 플러그인 모듈이 이름 참조만으로 빌드되고, 활성·모듈 종류·Release 배제가 적용되지 않는다.

**탐색(BD32).** 엔진 저장소의 모든 규칙 평가(빌드 CLI, Release 배제·유니티·PCH 정책 검사)는 `Engine/Plugins`를 `PluginCatalog.Discover`로 찾는다. 켜지 않은 플러그인도 기술자 검증과 규칙 대조(선언한 모듈마다 규칙이 하나, 규칙마다 선언이 하나)를 한다. 끈 플러그인의 규칙도 Configure가 성공해야 한다. UE도 엔진 플러그인을 늘 찾는다(`UBT/System/Plugins.cs` `ReadEnginePlugins`, 탐색 규칙 `EpicGames.Build/System/EnumeratePlugins.cs` — `.uplugin`이 있는 폴더 아래는 더 찾지 않는다. Rocks `PluginCatalog`와 같다).

**타깃이 켜기(BD33·BD34).** `TargetRules.Plugins : IReadOnlyList<PluginReference>`를 더한다. 형식과 의미는 `.rocksproject`의 `plugins[]`와 같다(`enabled` true·false, 3.6절 조건 필드는 BS4가 넓힌다). 같은 이름을 프로젝트도 적으면 타깃 참조가 이긴다(UE `UEBuildTarget.SetupPlugins`가 타깃 `EnablePlugins`·`DisablePlugins`를 먼저 처리하고, 프로젝트 참조는 이미 적힌 이름을 건너뛴다). `enabledByDefault`는 Program 타깃에 적용하지 않는다. Program은 명시한 것만 켠다(UE는 `bCompileAgainstEngine || bCompileWithPluginSupport`일 때만 기본 활성을 더하고, Program은 보통 둘 다 꺼져 있다). Game에는 적용한다. 의존 플러그인 자동 활성과 "끈 플러그인을 켠 플러그인이 요구하면 오류"는 3.6절 규칙 그대로다.

**모듈 포함(BD35).** 켠 플러그인 모듈은 기술자 모듈 종류로 거른다.

| 종류 | 들어가는 문맥 | UE 5.8 대응(`ModuleDescriptor.IsCompiledInConfiguration`) |
|---|---|---|
| Runtime | 모든 타깃 | `Runtime`·`RuntimeAndProgram`(Rocks Runtime은 Program에도 들어간다 — 7절) |
| Developer | Game의 Release를 뺀 모든 문맥 | `DeveloperTool`(`bBuildDeveloperTools` = Editor·Program, 또는 Test·Shipping이 아닌 구성) |
| Editor | BS2가 Editor 타깃을 만들기 전까지 빠짐 | `Editor` |

지금 게임 빌드 규칙(Developer는 Release면 무조건 뺌)은 Program Release(명령행 도구)에서 Developer 플러그인을 빼므로 이 표로 고친다. 플랫폼 규칙은 지금과 같다.

**링크와 모듈 표(BD36).** 켠 플러그인의 모듈은 타깃의 주 실행 파일(`Modules[0]`)에 링크하고, 모듈 표에 기술자 `loadingPhase`의 보통 행(단계 실행 대상)으로 싣는다. 실행 파일이 직접 의존하지 않아도 링크된다. UE도 켠 플러그인 모듈을 주 binary에 넣는다(`UEBuildTarget.PreBuildSetup`의 `AddModuleToBinary`, Monolithic이면 `LaunchModuleName`으로 만든 `Binaries[0]`). 부 실행 파일(시험 실행기의 probe 등)에는 링크하지 않는다. 실행 파일이 없는 타깃(Engine)은 빌드만 한다.

게임 빌드에도 같은 규칙을 적용한다. 지금 게임 빌드는 켠 플러그인 모듈을 빌드만 하고, 실행 파일이 참조한 모듈만 링크한다([저장소 배치](../Guides/RepositoryLayout.ko.md)). 그래서 플러그인이 명령을 등록하려면 실행 파일이 정적으로 의존해야 했고, 그러면 플러그인을 끈 타깃이 깨졌다. 이 결정은 [모듈 로더](ModuleLoader.ko.md) ML5 "기술자 모듈 행" 중 Monolithic 부분을 당겨 온다.

**규칙 기반 클래스(BD37).** 플러그인 모듈 규칙은 `PluginModuleRules`를 상속한다. `ModuleDirectory`는 `<플러그인>/Source/<모듈>`이다(UE 플러그인 규약 — `RulesCompiler`가 `<플러그인>/Source` 아래 규칙을 그 플러그인 소유로 등록한다). 위치·종류·`loadingPhase`·`platforms`의 단일 출처는 기술자이고, 기반 클래스가 `BuildContext.Plugins`에서 모듈 이름으로 찾는다. 기술자에 선언 없는 모듈의 규칙은 오류다. 정책 검사 `plugin-module-rules-base`가 `Engine/Plugins/**/*.Build.cs`의 기반 클래스를 확인한다.

**모듈 종류 칸(BD38).** `NativeModuleInput.Type`(Runtime·Developer·Editor)을 더한다. 엔진 모듈은 `SourceModuleRules`의 Group 폴더에서 오고(BS2가 생기면 그 규칙 값이 덮어쓴다), 플러그인 모듈은 기술자에서 온다. Release 배제 정책 검사는 경로(`Source/Developer`) 대신 이 칸으로 판정하고, Game 타깃의 도달 집합에 BD36 링크 모듈을 넣는다.

**계층 의존(BD39).** Core·`Engine/Source` 모듈은 플러그인 모듈에 의존할 수 없다. 링크 의존과 `DynamicModules` 모두 해당한다(UE도 엔진 모듈이 플러그인 모듈을 참조하면 동적 적재까지 오류다 — `UEBuildTarget`의 scope 검사). Programs·플러그인 모듈은 이번 타깃에서 켜진 플러그인의 모듈에만 의존할 수 있다(위반은 오류, UE는 경고). 플러그인이 다른 플러그인 모듈에 의존하려면 `.rocksplugin` `plugins`에 적어야 한다. [모듈 로더](ModuleLoader.ko.md)의 "Engine이 `DynamicModules`로 둔 렌더러·플러그인 모듈" 문장은 이 규칙에 맞게 고친다(BS9b에서 함께).

**플러그인 시험(BD40).** 플러그인 모듈 시험은 `<플러그인>/Source/<모듈>/Tests/`에 둔다. EngineTests 규칙이 `PluginTestModules` 목록(`플러그인/모듈`)으로 모으고, EngineTests 타깃이 그 플러그인을 켠다. `Private/` include 루트는 `<플러그인>/Source`다. UE는 플러그인마다 `<플러그인>/Tests` 시험 타깃을 둘 수 있지만 Rocks는 시험 실행기를 하나로 유지한다.

**거부(BD41).** SDK export 타깃은 엔진 플러그인을 켤 수 없다(설치 SDK는 엔진 플러그인을 싣지 않는다). `Modular` 타깃이 플러그인을 켜면 ML4(플러그인 binary manifest) 전까지 거부한다.

**정책 경로(BD42).** `crt-number-conversion` 검사 루트에 `Engine/Plugins`를 더한다(교환 리더가 JSON 숫자를 읽는다). 시험 소스·시험 fixture 판정에 플러그인 경로 모양(`Engine/Plugins/**/Source/<모듈>/Tests`)을 더한다.

**바뀌는 계약.**
- `PluginCatalog.ResolveEnabled(참조 목록, 참조 출처, available, applyDefaults)` — `ProjectDescriptor` 필수를 없앤다.
- `DescriptorModuleSet.Resolve(소유자, available, platform, configuration, targetType)`. `Apply`는 플러그인 소유 모듈만 대조하고 엔진 모듈은 통과시킨다.
- `TargetRules.Plugins`, `BuildContext.Plugins`(RuleLoader가 Configure 전에 채운다).
- `NativeModuleInput.Type`, 실행 파일의 플러그인 링크 칸(빌드가 채우는 칸이며, 규칙이 채우면 입력 오류).
- 모듈 표: 플러그인 모듈 행은 실행 파일 링크 폐포 행 뒤, 동적 행 앞의 보통 행이다.

**BS9a 구현 세부(개정 9).**
- 참조 주체는 `PluginReferrer(Label, FilePath)`다(`project`·`target '<이름>'`). 오류 문구의 주어와 가리킬 기술자를 정한다. 같은 이름을 두 번 참조하면 오류다.
- 빌드 주인은 `DescriptorOwner`다. `ForProject(project, 타깃 이름, 타깃 참조)`는 타깃 참조를 앞에 두고 같은 이름의 프로젝트 참조를 뺀다(`TargetPluginReferences.Combine`). `ForEngine(타깃 이름, 타깃 참조)`는 프로젝트가 없다. 그래서 `targetPlatforms` 검사가 없고, `Apply`는 기술자가 선언하지 않은 규칙 모듈(Core·Engine)을 그대로 통과시킨다.
- 타깃 참조 검사(`TargetPluginReferences.Validate` — 식별자, 대소문자 무시 중복)는 `RuleLoader.LoadAll`이 모든 타깃에 한다. 플러그인이 있는지는 해석이 본다.
- 탐색 위치는 `BuildContext.PluginRoot`다. 엔진 저장소는 `Engine/Plugins`이고, 설치 SDK 게임 빌드(`SdkDirectory`가 있음)는 프로젝트 `Plugins`다. `RuleLoader.LoadAll`이 Configure 전에 `BuildContext.Plugins`를 채우고, `LoadedRules`·`LoadedRuleSet`의 `Plugins`로도 돌려준다.
- `TargetType`은 기술자 해석이 쓰므로 규칙 기반 어셈블리에서 `Rocks.Build` 어셈블리로 옮겼다(이름공간 `Rocks.Build.Rules`는 그대로).
- 게임 빌드는 기술자 검증(`Discover`)을 규칙 컴파일 전에 하고, 해석(`Resolve`)은 규칙 로드 뒤 타깃 종류·타깃 참조로 한다(BD24). 결과로 게임 프로젝트의 Program 타깃도 `enabledByDefault`를 받지 않는다(BD34).

**BS9b 구현 세부(개정 10).**
- `PluginModuleRules`는 `SourceModuleRules`를 상속하고 `Configure`를 봉인한다. `Configure`가 `BuildContext.Plugins`에서 모듈을 선언한 플러그인을 하나 찾고(없거나 둘 이상이면 오류), `ModuleDirectory`(`<플러그인>/Source/<모듈>`)·`ModuleType`·`LoadingPhase`를 기술자 값으로 정한다. 플러그인 규칙은 이 값을 바꿀 수 없다.
- `SourceModuleRules.ModuleType`은 Group 폴더에서 온다. `Configure`를 재정의한 엔진 규칙은 `base.Configure`에서 시작해야 종류가 남는다. 이를 정책 `module-type-group`(Release 배제 검사의 일부)이 확인한다. Group 폴더(`<계층>/Source/Developer|Editor/`)와 종류 칸이 다르면 실패이고, 플러그인 경로는 보지 않는다.
- 위치·계층 검사(BD37·BD39)는 `DescriptorModuleSet.Apply`가 한다. 엔진 타깃 주인은 저장소 루트(`DescriptorOwner.EngineRoot`)를 갖고, 기술자 모듈 규칙의 `ModuleDirectory`를 기대 경로와 대조한다. 대상은 타깃과 무관하게 모든 규칙 모듈(빠진 모듈 포함)이고, 의존은 링크 의존과 `DynamicModules`다. 엔진 계층은 `ModuleDirectory`가 `Core/` 또는 `Engine/Source/`로 시작하는 모듈이다.
- Release 배제는 Game 타깃마다 엔진 주인으로 해석해 `IncludedPluginModules`를 도달 근에 더한다. 해석이 실패하면(끈 플러그인 모듈 사용 등) `release-plugin-resolution` 실패다.
- `plugin-module-rules-base` 검사 대상은 두 가지다. 규칙 파일이 `<플러그인>/Source/<모듈>/<모듈>.Build.cs`이고 그 플러그인 폴더에 `<플러그인>.rocksplugin`이 있는지, 그리고 `class <모듈> : PluginModuleRules` 선언인지(다른 규칙 기반 클래스 금지)다.

**BS9c 구현 세부(개정 11).**
- 링크 표시는 `NativeModuleInput.PluginModules`다(실행 파일만 가진다). `DescriptorModuleSet.LinkIntoPrimary(modules, 타깃 Modules)`가 `Modules[0]`이 실행 파일일 때만 `IncludedPluginModules`를 넣는다. 규칙이 이 칸을 채우면 `RuleLoader`가 거부하고, 입력 검사는 실행 파일만·아는 이름·한 번·`Modular` 거부를 본다.
- `LinkClosure`는 루트의 링크 폐포 다음에 `PluginModules`와 그 링크 폐포를 링크 쪽(요청 시 적재 아님)에 넣고, 그 뒤에 동적 모듈을 넣는다. 그래서 모듈 표 생성기와 링크 명령은 고치지 않고 BD36 순서를 얻는다. `Reachable`, 액션 그래프의 계획 순서(실행 파일을 플러그인 archive 뒤로), 메타데이터 생성 순서도 `PluginModules`를 따른다. 플러그인 모듈은 실행 파일의 의존이 아니므로 실행 파일 컴파일에 include·정의가 들어가지 않는다.
- 빌드 CLI는 모든 엔진 타깃에서 `DescriptorOwner.ForEngine`으로 해석한다. 메타데이터·공개 헤더 검사·빌드의 근은 `Modules`에 `IncludedPluginModules`를 더한 것이다. SDK export 타깃은 켠 플러그인이 하나라도 있으면(Game 타깃의 `enabledByDefault` 포함) 거부한다. 게임 빌드도 `LinkIntoPrimary`를 쓴다.

**BS9d 구현 세부(개정 12).**
- 첫 엔진 플러그인 `Engine/Plugins/Importers/UnrealExchange`에는 기술자(`enabledByDefault: false`, 모듈 `UnrealExchange` Developer, 단계 Default), `PluginModuleRules` 규칙(공개 의존 Core), 진입점 `Private/UnrealExchangeModule.cpp`, 공개 헤더 `UnrealExchange/UnrealExchangeFormat.h`(묶음 `formatVersion` 상수), 시험 하나가 있다. 리더·명령 등록은 UX1a·UX1b가 채운다.
- Commandlet·EngineTests 타깃이 `Plugins`로 켠다. EngineTests 규칙의 `PluginTestModules`(`Importers/UnrealExchange/UnrealExchange`)는 시험 폴더, `<플러그인>/Source` include 루트, 그 모듈 공개 의존을 더한다.
- 엔진 저장소 규칙 어셈블리는 모든 타깃의 모듈을 담는다. 그래서 엔진 주인의 `Apply`는 결과와 빠진 모듈 사용 검사를 타깃 모듈과 켠 플러그인 모듈에서 닿는 모듈로 줄인다. 그렇지 않으면 플러그인을 켜지 않은 타깃(Foundation 등)이 EngineTests 모듈의 플러그인 의존 때문에 실패한다.

## 4. 결정 요약

1. 타깃 종류는 UE와 같은 다섯 가지다(Game·Editor·Client·Server·Program).
2. 타깃 종류 차이는 네 매크로(`ROCKS_WITH_EDITOR`·`_EDITOR_DATA`·`_SERVER_CODE`·`_CLIENT_CODE`)와 빌드 주입 상수로 나타낸다. 타깃 종류 매크로는 두지 않는다.
3. Program의 기본 매크로 조합은 Game과 같다. 에디터 데이터·에디터 코드는 Program 속성으로 켠다.
4. 리플렉션 선언은 조건 블록 밖이나 `ROCKS_WITH_EDITOR_DATA` 블록 안에만 쓰고, 두 변형 비교로 보강한다.
5. 모듈 type은 게임·플러그인에서는 거르고, 엔진 모듈에서는 검사한다.
6. 의존은 Public·Private 둘로 나누고 UE와 같은 전파 규칙을 쓴다. include-only 의존은 두지 않는다. 서드파티 우회를 없앤다.
7. 프로젝트의 플러그인 참조에 허용 목록(타깃·플랫폼·구성)을 두고, 조건 참조가 그 플러그인의 상태를 정한다(UE 의미론). 거부 목록과 optional은 두지 않는다.
8. 전용 서버는 필수다(구현은 네트워크 단계). Server 타깃은 Windows·macOS·Linux에서 빌드하고, Linux x64를 빌드 플랫폼(서버·기반 시험 전용)으로 더한다.
9. Editor 타깃은 UE와 같이 모듈마다 공유 라이브러리로 링크하고(사용자 결정), 나머지 타깃은 단일 실행 파일이다. 모듈 API 매크로·리플렉션 타입 정보 export·공개 헤더 정적 저장소 금지로 두 방식이 같은 소스로 동작하게 하고, 모듈 수명·로더는 별도 스펙으로 정한다.
10. 엔진 타깃은 `Engine/Plugins`를 늘 찾아 검증하고, 타깃 규칙 `Plugins`로 켠 플러그인의 모듈만 기술자 종류대로 걸러 주 실행 파일에 링크한다(3.9절). 엔진 모듈은 플러그인 모듈에 의존하지 않는다.

## 5. 결정 사항

| 번호 | 결정 | 상태 |
|---|---|---|
| BD1 | `TargetType`을 Game·Editor·Client·Server·Program 다섯 값으로 넓힌다. 타깃 종류별 플랫폼은 3.1절 표다 | 권장안 |
| BD2 | 매크로 `ROCKS_WITH_EDITOR`·`ROCKS_WITH_EDITOR_DATA`·`ROCKS_WITH_SERVER_CODE`·`ROCKS_WITH_CLIENT_CODE`를 모든 모듈에 0·1로 정의한다. 타깃 안에서 같은 값을 강제하고, 기본값 없이 `Base/Compiler.h`가 검사한다. `ROCKS_WITH_EDITOR=1`이면 `ROCKS_WITH_EDITOR_DATA=1`이다. 서버·클라이언트 코드 값은 타깃 종류로만 정한다 | 권장안 |
| BD3 | 타깃 종류 매크로(UE `UE_GAME`·`UE_EDITOR`·`IS_PROGRAM` 등)는 두지 않는다. 실행 중 분기는 `build::Description().targetType`, 컴파일 분기는 BD2 매크로로 한다 | 권장안 |
| BD4 | 모듈 type은 Runtime·Developer·Editor·ServerOnly·ClientOnly다. 게임·플러그인 모듈은 기술자 type으로 거르고, 엔진 모듈은 `SourceModuleRules.ModuleType`(기본은 Group 폴더)으로 도달 검사만 한다. SDK manifest에 type을 싣는다 | 권장안 |
| BD5 | 설치 SDK는 매크로 조합마다 엔진 라이브러리 변형을 싣는다(key는 조합). 1차는 Game 조합만이며 게임의 Game 타깃과 기본 Program 타깃을 빌드할 수 있다. 없는 조합은 오류이고, 변형을 더하면 `ManifestVersion`을 올린다. Editor 변형은 key에 링크 방식(`Modular`)을 더하고 Rocks Studio 실행 파일과 엔진 모듈 공유 라이브러리를 싣는다(3.8절). 빌드 구성(Debug·Development·Release)은 이 변형 축과 직교하는 별도 축이다: SDK 하나가 여러 구성을 `Configurations/<구성>/`에 싣고(manifest 14, [Visual Studio 프로젝트 파일](ProjectFiles.ko.md) 10.3절), 변형이 생기면 `Configurations/<구성>/<변형>/`으로 넓힌다 | 권장안 |
| BD6 | 리플렉션 선언은 전처리 조건 블록 밖에만 쓴다. 예외는 `#if ROCKS_WITH_EDITOR_DATA` 안의 `ROCKS_PROPERTY`와 `#if ROCKS_WITH_EDITOR` 안의 `ROCKS_FUNCTION`뿐이고, 둘 다 `EditorOnly`와 양방향으로 일치해야 한다. 타입 선언은 블록 안에 둘 수 없다. 판정은 메타 컴파일러의 어휘 스캔(정확한 조건 철자 두 개만 허용)이다. 에디터·게임 두 조합의 AST 비교를 보강 검사로 두고, descriptor·안정 ID는 합집합, 생성 코드는 조건부다. `ROCKS_WITH_EDITOR_DATA=0` 빌드는 `EditorOnly` FieldId 목록으로 에디터 패키지의 그 값을 건너뛴다 | 권장안 |
| BD7 | `Dependencies`·`RequiredModules`를 `PublicDependencies`·`PrivateDependencies`로 바꾼다(호환 이름 없음) | 권장안 |
| BD8 | 메타 컴파일러의 모듈 범위(import·include 순서·생성 헤더 해석·전파 define)는 컴파일 환경에 보이는 모듈과 같다 | 권장안 |
| BD9 | 공개 헤더 검사는 모듈마다 그 모듈의 공개 환경만으로 컴파일한다 | 권장안 |
| BD10 | include-only 의존(UE `*IncludePathModuleNames`)은 두지 않는다 | 권장안 |
| BD11 | include 소유 모듈 정책 검사(depfile 기반, UB0 파서 공유), 두 목록 중복 금지, 쓰지 않는 공개 의존 진단 | 권장안 |
| BD12 | 프로젝트 플러그인 참조에 `targets`·`platforms`·`configurations` 허용 목록을 둔다. 조건 참조는 그 플러그인의 상태를 정한다(문맥이 맞으면 켬, 아니면 끔 — UE 의미론). 값 검사는 문맥과 무관하게 한다 | 권장안 |
| BD13 | `.rocksplugin`에 지원 플랫폼 `platforms`를 둔다. 기본 활성만으로 켜질 플러그인은 미지원 플랫폼에서 조용히 빼고, 명시·의존으로 켜지면 오류다 | 권장안 |
| BD14 | 거부 목록(UE `*DenyList`)은 두지 않는다. 같은 축에 허용·거부 두 목록이 있으면 충돌 규칙이 필요하다. 템플릿의 `TargetDenyList: ["Server"]`는 `targets: ["Game", "Client", "Editor", "Program"]`로 적는다. 대가로 타깃 종류가 늘면 기존 허용 목록에 자동으로 들어가지 않는다. 타깃 종류 추가는 드물고, 그때 기존 목록을 검토하는 편이 명시적이다 | 권장안 |
| BD15 | 플러그인 참조 `optional`(없으면 조용히 무시)은 두지 않는다. 저장소에 플러그인이 있는지에 따라 빌드 결과가 조용히 달라지기 때문이다 | 권장안 |
| BD16 | 모듈 기술자의 `AdditionalDependencies`(UE 에디터 클래스 추가 마법사가 Build.cs 대신 기술자에 의존을 덧붙이는 칸 — `UEBuildTarget.cs:6784-6790`)는 두지 않는다. 의존의 단일 출처는 모듈 규칙(Build.cs)이고, 에디터 마법사가 생기면 Build.cs를 고친다 | 권장안 |
| BD17 | 타깃 규칙 속성 `LinkType { Monolithic, Modular }`. 기본값은 Editor가 `Modular`, 나머지가 `Monolithic`이다(UE `TargetRules.cs:2686-2689`). 타깃이 바꿀 수 있고 Android는 `Modular`를 거부한다. 매크로 `ROCKS_MONOLITHIC`, 파일 이름은 UE 규칙(3.8절) | **확정 — 사용자 결정(2026-11-19, UE 방식)** |
| BD18 | 전용 서버는 필수다(사용자 요구 2026-11-19 — 네트워크가 구성되면 서버로 돌 수 있어야 하며, 스펙은 지금 잡고 구현은 네트워크 단계). Server 타깃은 Windows·macOS·Linux에서 빌드하고 Android에서는 거부하며, 그래픽 backend 모듈(type ClientOnly)을 링크하지 않는다. `NativePlatform`에 Linux(x64)를 더하고 Linux에서는 Server·Program 타깃만 빌드한다. 게임 템플릿은 Server 타깃 규칙을 싣는다 | **확정(요구)** |
| BD19 | Program 타깃의 기본 매크로 조합은 Game과 같다(`ROCKS_WITH_EDITOR_DATA=0`). `WithEditorData`·`CompileWithEditor` 속성으로 켠다 | 권장안 |
| BD20 | Release 배제 정책을 Game·Client·Server로 넓히고, type Editor 모듈이 Game·Client·Server에 닿는 것을 구성과 무관하게 막는다 | 권장안 |
| BD21 | 서드파티 모듈은 include·define을 public으로 내고, 소비자는 `PrivateDependencies`로 쓴다. `Consumer*` 우회를 없앤다 | 권장안 |
| BD22 | 시험 소스는 시험 대상 모듈의 컴파일 환경 전체와 그 모듈의 `Private/`를 받는다. 수단은 `NativeModuleInput.SourceGroups`(묶음별 추가 비공개 의존·include)이고, 유니티 묶음 TU는 소스 묶음 경계를 넘지 않는다 | 권장안 |
| BD23 | 플러그인 의존 참조(`.rocksplugin` `plugins`)에는 조건을 두지 않는다 | 권장안 |
| BD24 | 게임 빌드의 플러그인 처리는 문맥 무관 검증(규칙 로드 전)과 문맥별 해석(규칙 로드 뒤, 타깃 종류 사용)으로 나눈다 | 권장안 |
| BD25 | 모듈 A는 들어갈 수 있는 (타깃 종류, 구성) 집합이 자기 집합을 포함하는 모듈에만 의존한다(3.3절 표). 위반은 규칙 로드 때 오류다 | 권장안 |
| BD26 | `ROCKS_WITH_EDITOR_DATA=0` 빌드는 에디터 패키지(쿠킹하지 않은 패키지)를 저장하지 않는다(Unsupported). 건너뛴 `EditorOnly` 값을 잃지 않기 위해서다 | 권장안 |
| BD27 | 빌드가 모듈마다 `ROCKS_<MODULE>_API`를 정의한다. `Monolithic`이면 빈 값, `Modular`이면 자기 모듈은 export·다른 모듈은 import다(UE `<MODULE>_API`). 가시성·rpath 옵션은 UE와 같다. 규칙 파일은 이 매크로를 정의할 수 없고, 다른 모듈의 API 매크로를 붙이면 정책 오류다. 다른 모듈이 상속하는 클래스는 클래스 단위 export가 필요하다(메타 컴파일러 진단) | 권장안 |
| BD28 | 메타 컴파일러는 생성 `.cpp`(UE `.gen.cpp` — 리플렉션 헤더마다 하나, 모듈마다 등록용 하나: [리플렉션 생성 소스](ReflectionGeneratedSources.ko.md) GSD2)를 내고, 타입 키·타입 정보·등록 함수를 거기 정의해 항상 export한다. `NativeTypeKey`는 그 키 저장소 주소다. UE `MinimalAPI` 지정자는 두지 않는다 | 권장안 |
| BD29 | 공개 헤더에 쓰기 가능한 정적 저장소(const 아닌 inline 변수·inline/템플릿 함수의 static 지역 변수·헤더 정의 정적 데이터 멤버), `thread_local` 변수, 주소를 식별자로 쓰는 정적 객체를 두지 않는다(정책 `public-header-static-storage`, 링크 방식과 무관하게 검사). 서드파티는 UE와 같이 소비 모듈마다 링크하고, `HasGlobalState` 서드파티만 직접 소비 모듈을 하나로 제한한다 | 권장안 |
| BD30 | 모듈 수명 객체·로더는 별도 스펙 [모듈 로더](ModuleLoader.ko.md)로 정한다(ML0–ML4는 BS5 전, ML5는 BS5와 함께). Modular 실행 파일은 부트 모듈만 import한다(MD21). 요구 계약은 3.8절 네 항목이다. 게임의 Editor 타깃은 설치 SDK의 엔진 모듈 공유 라이브러리를 공유하며 엔진 컴파일 환경을 바꿀 수 없다 | 권장안 |
| BD31 | Editor보다 먼저 `Modular` Program 시험 빌드(FoundationTests — 타깃 `FoundationModular`)를 검사 체인에 넣어 export 누락과 공유 상태 위반을 잡는다(Windows·macOS). 개정 22(사용자 결정): 시험은 시험 실행 파일 모듈(호스트) 하나에 두고 시험이 부르는 모듈 내부 선언에 그 모듈의 API 매크로를 붙인다(비공개 헤더는 정책 허용 목록). 처음 안(모듈 `Private/` 시험 묶음을 그 모듈 공유 라이브러리에 링크 — UE `Private/Tests`)은 6개 모듈의 시험이 그 모듈에 의존하는 fixture·모듈을 써서 순환이 생겨 버렸다. 시험 등록 표는 호스트에 한 벌이다 | 확정(사용자 결정) |
| BD32 | 엔진 저장소의 규칙 평가는 `Engine/Plugins`를 늘 찾고, 끈 플러그인까지 기술자 검증·규칙 대조를 한다(3.9절) | 권장안 |
| BD33 | 타깃 규칙 `Plugins`(프로젝트 `plugins[]`와 같은 형식), 타깃 참조가 프로젝트 참조를 이긴다. 버린 안: 문자열 목록 둘(형식이 둘이 됨), 플러그인 쪽 지원 프로그램 목록(UE `SupportedPrograms` — 이중 등록) | 권장안 |
| BD34 | `enabledByDefault`는 Program 타깃에 적용하지 않는다(명시만). Game에는 적용한다 | 권장안 |
| BD35 | 플러그인 모듈 포함: Runtime은 모두, Developer는 Game Release만 뺌, Editor는 BS2 전까지 뺌(3.9절 표). 버린 안: 지금 규칙(Program Release에서 Developer가 빠짐) | 권장안 |
| BD36 | 켠 플러그인 모듈은 주 실행 파일에 링크하고 기술자 단계의 보통 모듈 행으로 싣는다. 게임 빌드도 같다. 버린 안: 참조된 것만 링크(정적 의존이 필요해 끄고 켜는 의미가 없어짐), `DynamicModules` 재사용(요청 시 적재 행이라 단계로 시작하지 않음), 모든 실행 파일에 링크 | 권장안 |
| BD37 | 플러그인 모듈 규칙 기반 클래스 `PluginModuleRules`, 위치·종류·단계의 단일 출처는 기술자. 버린 안: 규칙에 경로 직접 기입(중복), `Source/Developer` 흉내 | 권장안 |
| BD38 | `NativeModuleInput.Type`을 두고, Release 배제는 경로 대신 이 칸으로 판정한다 | 권장안 |
| BD39 | 엔진 모듈 → 플러그인 모듈 의존 금지(동적 포함), 끈 플러그인 모듈 의존은 오류(UE는 경고), 플러그인 사이는 `plugins` 선언 필요 | 권장안 |
| BD40 | 플러그인 시험은 `<플러그인>/Source/<모듈>/Tests/`에 두고 EngineTests가 `PluginTestModules`로 모은다. 버린 안: 플러그인마다 시험 실행 파일 | 권장안 |
| BD41 | SDK export 타깃의 엔진 플러그인 켜기 거부, `Modular` 타깃의 플러그인은 ML4 전까지 거부 | 권장안 |
| BD42 | 정책 경로에 `Engine/Plugins` 모양 추가(`crt-number-conversion`·시험 소스·fixture 판정), 새 검사 `plugin-module-rules-base` | 권장안 |

## 6. 단계

각 단계는 Debug 빌드, format, policy, 바뀐 부분의 시험(`Rocks.Build.Tests`·`Rocks.Build.IntegrationTests`·`Rocks.ToolTests`)으로 검증한다.

| 단계 | 내용 | 완료 기준 |
|---|---|---|
| BS0 | 의존 두 목록(3.5절): `NativeModuleInput`·규칙 기반 클래스·ActionGraph 전파·순환 검사·Closures, MetadataRunner·MetadataLayout 범위, SDK manifest(`SdkModule`·`Reachable`·검증), DescriptorModules·ReleaseExclusion 도달 검사. 기존 모듈은 모두 공개 의존으로 옮긴다 | 모든 타깃의 빌드 결과(object 인자)가 전환 전과 같다 |
| BS1 | 공개 헤더 검사의 모듈별 공개 환경(BD9), include 소유 모듈 정책 검사(BD11), `SourceGroups`와 시험 소스 환경(BD22). 선행: UB0의 depfile 파서 | 공개 헤더가 비공개 의존 헤더를 include한 시험 모듈을 검사가 거부한다 |
| BS2 | 타깃 종류 다섯 값·플랫폼 표·`build::TargetType`·네 매크로와 주입 위치(3.2절 목록)·`Base/Compiler.h` 검사·`CompileWithEditor`·`WithEditorData`·모듈 type ServerOnly·ClientOnly와 `SourceModuleRules.ModuleType`·SDK manifest type, Release 배제 정책 확장과 타깃 종류별 평가 플랫폼(BD20), type 사이 의존 규칙(BD25), 메타 컴파일러 어휘 스캔·두 조합 비교·조건부 생성·합집합 descriptor·`EditorOnly` FieldId 목록(BD6), 에디터 패키지 저장 거부(BD26), 기존 fixture의 `EditorOnly` 필드 이동 | 타깃 종류별 모듈 포함 표와 매크로 값을 시험이 확인한다. 허용되지 않은 조건 블록 안 리플렉션 선언, 블록 안 타입 선언, 블록 밖 `EditorOnly`를 거부한다. `ROCKS_WITH_EDITOR_DATA=0` 빌드의 에디터 패키지 저장이 Unsupported다. `ROCKS_WITH_EDITOR_DATA` 0·1 조합 시험 빌드가 통과한다 |
| BS3 | 서드파티 모듈 전환(BD21), 기존 모듈의 공개 의존을 비공개로 내리기 | 쓰지 않는 공개 의존 진단 0, `Consumer*` 우회 0, 빌드 통과 |
| BS4 | 플러그인 활성 조건(3.6절): 기술자 읽기·값 검사, 처리 순서 분리(BD24), 문맥별 해석, 플러그인 `platforms` | 조건 조합 시험(타깃·플랫폼·구성), `enabledByDefault` 플러그인의 Editor 전용 켜기, 미지원 플랫폼 조용히 빼기와 명시 오류 시험 |
| BS5 | Editor 타깃 첫 실행 파일 — Rocks Studio 착수 시점. 선행: BS8, 모듈 로더 스펙과 구현(BD30). 설치 SDK Editor 변형(`Modular`), 게임 Editor 타깃의 공유 빌드 환경 검사 | Rocks Studio 실행 파일이 엔진 모듈 공유 라이브러리와 게임·플러그인 모듈을 적재해 부트·종료한다(Windows·macOS) |
| BS6 | Linux x64 빌드 플랫폼(3.7절 2): `NativePlatform.Linux`, clang++·lld 툴체인 탐색과 fingerprint, 플랫폼 매크로, `Private/Linux/` 규칙, 정책 검사의 플랫폼 폴더 목록, Linux에서 허용 타깃 제한 | Core 모듈과 FoundationTests가 Linux x64에서 빌드·통과(전체 계획 M3 "Core CPU/headless suite") |
| BS7 | Server 타깃 첫 실행 파일(3.7절 1·3·4) — 네트워크 단계: 게임 템플릿의 Server 타깃 규칙, 그래픽 backend ClientOnly 표시와 비링크 검사, Android Server 타깃 거부, headless 부트·종료 | Windows·macOS·Linux에서 Server 실행 파일이 부트·종료한다(전체 계획 M4 "server service boot/shutdown") |
| BS8 | 모듈식 링크 기반(3.8절) — 선행: BS0. `LinkType`·`ROCKS_MONOLITHIC`·Android 거부·파일 이름, 모듈 API 매크로 주입과 `ROCKS_DLLEXPORT`·`ROCKS_DLLIMPORT`, 숨김 가시성, 예약 매크로 검사, 가시성·`-fPIC`·rpath·install name, 생성 `.cpp`와 `NativeTypeKey` 교체(BD28), 상속 export 진단과 모듈별 export 수 진단, 기존 모듈의 모듈 사이 API에 매크로 붙이기, 정책 `public-header-static-storage`와 지금 위반 정리, `HasGlobalState` 표시와 단일 소비 검사, 시험용 내부 export와 `Modular` FoundationTests 검사 체인(BD31 개정 22), 실행 파일 생성 TU가 정의하던 빌드 정보(`ToolchainFingerprint`·`ToolBuildId`)의 Modular 정의 위치(Core 공유 라이브러리에 미해결 기호가 남지 않게) | `Modular` FoundationTests가 Windows·macOS에서 빌드·통과한다. `Monolithic` 빌드 결과(object 인자)는 매크로 값 외에 바뀌지 않는다. 공개 헤더의 `thread_local`·쓰기 가능한 inline 정적 저장소를 정책이 거부한다 |
| BS9a | 엔진 플러그인 해석(3.9절): `ResolveEnabled`·`Resolve` 소유자 일반화, `targetType` 인자, BD34·BD35, `TargetRules.Plugins`, `BuildContext.Plugins`, RuleLoader 탐색 | 기술자 시험(엔진 소유자 해석, 타깃 참조 우선, Program 기본 활성 미적용, Developer의 Program Release 포함·Game Release 제외, 없는 참조·끈 플러그인 요구·순환 오류), 게임 빌드 동작 불변(Program Release의 Developer만 바뀜) |
| BS9b | `PluginModuleRules`, `NativeModuleInput.Type`, Release 배제 정책 전환, BD39 계층 검사, BD42 정책 | 규칙 시험(디렉터리 일치, 선언 없는 규칙·규칙 없는 선언·대소문자 이름 오류, 끈 플러그인 규칙 Configure 성공), 계층 위반 시험, 플러그인 Developer 모듈을 Release 정책이 잡는 음성 대조 |
| BS9c | 엔진 타깃 연결(CLI), BD36 링크·모듈 표 행(게임 빌드 포함), BD41 거부 | 의존 없는 주 실행 파일에 켠 플러그인 모듈이 링크되고 기술자 단계로 시작, probe에는 없음, 끈 플러그인 모듈은 빌드되지 않음, 실행 파일 없는 타깃은 빌드만, SDK export·Modular 거부 시험 |
| BS9d | 첫 엔진 플러그인 `Engine/Plugins/Importers/UnrealExchange`(기술자와 진입점만 있는 모듈), Commandlet·EngineTests가 켬, 플러그인 시험 수집(BD40) | EngineTests가 플러그인 Tests를 실행, 다른 타깃에는 모듈 없음 — Unreal 교환 UX1a 선행 조건 충족 |

순서: BS0 → BS1(UB0 depfile 파서 뒤) → BS3. BS2는 BS0 뒤가 쉽다(규칙 기반 클래스를 한 번에 고친다). BS4는 BS2 뒤다(`targets` 값 검증과 문맥별 해석에 다섯 값 `TargetType`이 필요하다). BS6은 다른 단계와 독립이다. BS7은 BS2·BS6 뒤이고, 렌더·오디오·입력 backend를 서버에서 빼는 범위는 BS7 전에 네트워크 단계 스펙이 정한다. BS9a → BS9b → BS9c → BS9d는 BS2·BS4와 독립이다(타깃 종류 두 값, 프로젝트 없는 엔진 타깃만으로 동작). BS2·BS4가 나중에 종류 표와 조건 필드를 넓힌다.

## 7. UE와 다르게 정한 점

| 항목 | UE 5.8 | Rocks | 이유 |
|---|---|---|---|
| 타깃 종류 매크로 | `UE_GAME`·`UE_EDITOR`·`UE_SERVER`·`UE_CLIENT`·`IS_PROGRAM` | 없음. 빌드 주입 상수 | 기능 매크로(BD2)로 컴파일 분기가 충분하고, 실행 중 분기는 상수가 더 명확하다 |
| 서버·클라이언트 코드 값 | 타깃이 재정의 가능 | 타깃 종류로 고정 | SDK 변형·시험 행렬을 줄인다 |
| 매크로 기본값 | `WITH_EDITORONLY_DATA`는 정의가 없으면 1 | 기본값 없음, 미정의는 `#error` | 매크로를 넘기지 않은 컴파일 경로가 조용히 다른 조합이 되지 않게 |
| Program의 에디터 데이터 | 기본 1(시험 타깃만 0) | 기본 0, `WithEditorData`로 켬 | Rocks Program 대부분이 시험·벤치마크이고 SDK 조합을 Game과 공유한다 |
| Runtime 모듈과 Program | `Runtime`은 Program에서 빠지고 `RuntimeAndProgram`이 따로 있다 | Runtime은 Program에도 들어간다 | Rocks 시험·도구는 런타임 모듈을 그대로 링크한다 |
| Editor 모듈과 Program | `EditorAndProgram`은 모든 Program에 들어간다 | `CompileWithEditor` Program에만 들어간다 | Program이 에디터 코드를 쓰는지를 타깃이 명시한다 |
| 엔진 모듈 type | 엔진 모듈은 기술자 type으로 거르지 않는다 | 엔진 모듈도 type을 갖고 도달 검사한다 | 서버·클라이언트 전용 엔진 모듈을 검사로 막는다 |
| EditorOnly 표시 | `WITH_EDITORONLY_DATA` 블록 안 필드에 자동으로 붙는다 | 명시해야 하고 블록과 양방향 일치를 검사한다. 두 조합 비교를 더한다 | 지정자 하나를 보고 저장 의미를 알 수 있게 |
| 모듈 type 사이 의존 | 규칙 없음(타깃 빌드 때 깨진다) | 규칙 로드 때 포함 관계 검사(BD25) | 특정 타깃에서만 드러나는 실패를 미리 막는다 |
| 명시 참조된 플러그인의 미지원 플랫폼 | 참조에 복사된 지원 플랫폼으로 조용히 뺀다(`UEBuildTarget.cs:5847`) | 오류(BD13) | 명시한 의도와 다른 결과를 조용히 만들지 않는다 |
| 프로젝트 조건으로 꺼진 플러그인을 다른 플러그인이 의존 | 의존 처리가 켠다(`UEBuildTarget.cs:5966-5975`) | 오류(3.6절) | 프로젝트가 끈 것을 다른 경로가 조용히 켜지 않게 |
| 쿠킹 데이터만 읽기 | Game·Client·Server는 쿠킹 데이터만(`bBuildRequiresCookedData`) | 채택하지 않음. 에디터 패키지의 `EditorOnly` 값을 건너뛴다 | Development Game의 개발 흐름 |
| include-only 의존 | 있음 | 없음 | 순환 금지 규칙이 있다 |
| 플러그인 조건 | 허용·거부 목록, optional, 지원 플랫폼 복사, 플러그인 의존 참조에도 조건 | 허용 목록만, optional 없음, 지원 플랫폼은 플러그인 한 곳, 의존 참조에는 조건 없음 | 충돌 규칙 제거, 빌드 결과가 저장소 상태에 조용히 의존하지 않게, 조건을 프로젝트 한 곳에 |
| 기술자 `AdditionalDependencies` | 있음 | 없음 | 의존의 단일 출처 |
| 리플렉션 타입 정보 export | 클래스를 export하지 않으면 `MinimalAPI`가 있어야 타입 정보가 export된다 | 생성 코드가 항상 export한다(BD28) | 지정자를 빠뜨려 Editor 빌드에서만 다른 모듈 참조가 깨지는 일을 없앤다 |
| 공개 헤더의 정적 저장소 | 관례에 맡긴다 | 정책으로 막는다(BD29) | 모듈마다 상태가 갈라지는 오류는 Editor(`Modular`)에서만, 그것도 실행 중에만 드러난다 |
| 서드파티 정적 라이브러리 소비 | 소비 모듈의 바이너리마다 링크된다 | 같다. 단 `HasGlobalState` 서드파티는 직접 소비 모듈을 하나로 제한한다(BD29) | 전역 상태가 두 벌이 되지 않게 |
| 플러그인을 켜는 타깃 목록 | `EnablePlugins`·`DisablePlugins`·`OptionalPlugins`·`BuildPlugins` 목록 넷 | `Plugins` 하나(`enabled` 값) | 프로젝트 `plugins[]`와 형식을 하나로 맞춘다 |
| 플러그인의 지원 프로그램 | `.uplugin` `SupportedPrograms`에 없으면 Program이 무시 | 없음. 타깃이 켜면 들어간다 | 켜는 쪽 한 곳에서 정한다(이중 등록 없음) |
| 끈 플러그인 모듈 의존 | 경고 | 오류 | 조용히 빌드되는 모듈을 막는다 |

## 8. 다른 문서 개정

이번 개정에서 함께 고친 문서:
- [저장소 구성](../Guides/RepositoryLayout.ko.md): "후속"에 이 문서 링크를 더했다. 기술자 표·모듈 type 표·게임 빌드 연동·플랫폼 폴더 목록(Linux)은 BS2·BS4·BS6 구현 때 고친다.
- [리플렉션 기반](ReflectionFoundations.ko.md) 134행 `EditorOnly`: BD6의 선언 규칙으로 바뀐다는 주석을 달았다.
- [유니티 빌드·PCH](UnityBuildPch.ko.md) 8.4절 248행: 제공 모듈 후보를 "컴파일 환경에 보이는 제공 모듈"로 고쳤다.
- [셰이더 시스템](ShaderSystem.ko.md) 9절: Release 배제 대상을 Game·Client·Server로 고쳤다.
- [AppInfo·CommandLine](AppInfoCommandLine.ko.md) 307·565행 `build::TargetType`: 다섯 값으로 고쳤다. `HostKind`(304행)는 Editor·Server 타깃이 생길 때 더한다.
- [UI 툴킷](UiToolkit.ko.md) 7.3절 (g) `MinimalAPI`·`*_API` 행(개정 2): BD27·BD28로 정했다고 고쳤다.

구현 때 고칠 문서:
- [전체 계획](EngineImplementationPlan.ko.md) 282행 "공개/비공개 의존성": 이 문서 3.5절이 구현 스펙이다(링크만 더한다).
- `Programs/Rocks.Build/README.ko.md`: 단계마다 지원 범위를 갱신한다.

## 9. 범위 밖과 채택하지 않은 UE 요소

### 9.1 채택하지 않은 UE 요소

| UE 5.8 요소 | 판단 |
|---|---|
| `BuildSettingsVersion`·`IncludeOrderVersion`(템플릿 `DefaultBuildSettings = V7`, `IncludeOrderVersion = Unreal5_8`) | 하위 호환용 버전 스위치다. Rocks는 하위 호환을 두지 않는다 |
| 모듈 type `RuntimeNoCommandlet`·`EditorNoCommandlet`·`ClientOnlyNoCommandlet` | commandlet이 에디터 실행 파일 안에서 도는 UE 구조용이다. Rocks commandlet은 별도 Program이다 |
| 모듈 type `CookedOnly`·`UncookedOnly`, 타깃의 `bBuildRequiresCookedData` | 쿠킹 데이터 요구를 타깃 축으로 두지 않는다(3.4절 로드). 필요해지면 다시 본다 |
| 모듈 type `Program`·UE `Developer`(Editor·Program 전용)·`RuntimeAndProgram`·`EditorAndProgram` | Program 전용 모듈은 Program 타깃만 참조하면 된다. 나머지는 3.3절 대응표로 합쳤다 |
| 모듈 기술자의 `TargetAllowList`·`ProgramAllowList`·`GameTargetAllowList`·`PlatformArchitectureAllowList` 등 | 모듈 type과 `platforms`로 나타낸다. 필요가 생기면 BD12와 같은 허용 목록으로 더한다 |
| 플러그인 기술자의 `SupportedPrograms`(Program 타깃은 목록에 없는 플러그인을 뺀다 — `UEBuildTarget.cs:5904-5909`) | 켠 플러그인의 모듈은 Program 타깃에도 지금처럼 빌드된다(타깃이 참조한 모듈만 링크된다). Program이 플러그인 모듈을 쓰는지는 타깃의 모듈 참조가 정한다 |
| 타깃 규칙의 `EnablePlugins`·`DisablePlugins`·`OptionalPlugins`·`BuildPlugins`(`UEBuildTarget.cs:5609-5636, 5721-5729`), `bAllowEnginePluginsEnabledByDefault`, 프로젝트 `DisableEnginePluginsByDefault`(`:5645-5649`) | 플러그인 활성의 단일 출처는 프로젝트 기술자다(조건은 BD12). 엔진 플러그인 기본 활성은 플러그인마다 `enabledByDefault`와 프로젝트의 명시적 끄기로 정한다 |
| 플러그인 참조 `Version`(`RequestedVersion`)·`HasExplicitPlatforms`·`Description`·`MarketplaceURL`(`PluginReferenceDescriptor.cs:40-45, 90, 95`) | 플러그인 버전 선택은 저장소에 한 벌만 있는 Rocks 구조에서 필요 없다. 명시 플랫폼은 플러그인 `platforms`(BD13)가 맡는다. 설명·스토어 URL은 설치되지 않은 플러그인 안내용이다 |
| 플러그인 참조 `bActivate`·`TargetsToExplicitlyLoad`, `bRequiresBuildPlatform`·`DisallowedPlugins`·`bExplicitlyLoaded` | Game Feature 플러그인·로딩 단계 재정의·플랫폼 SDK 요구용이다. Rocks에 대응 기능이 아직 없다 |
| 의존 축 `InternalIncludePaths`(같은 scope에만 보이는 제3의 공개 범위 — `ModuleRules.cs:1291`, `UEBuildModule.cs:736-740`) | 1차는 두지 않는다. 설치 SDK는 엔진 모듈의 public include를 그대로 내보낸다. 엔진 안에서만 쓰는 공개 API를 게임에 숨길 필요가 생기면 다시 본다 |
| `DynamicallyLoadedModuleNames`·`PublicDelayLoadDLLs`·`*RuntimeLibraryPaths` | 실행 중 적재 모듈 선언과 적재 경로는 모듈 로더 스펙(BD30)에서 정한다. 엔진 런타임 서드파티는 소스로 빌드하는 정적 라이브러리다. 셰이더 도구가 실행 중에 여는 DXC·SpirvCross·SpirvTools·D3DCompiler DLL(`ShaderToolSpike.Build.cs:27`)은 도구 쪽 적재이고, 배치는 패키징·스테이징(9.2절)과 모듈 로더 스펙에서 정한다 |
| `RuntimeDependencies`(실행 파일 옆에 둘 런타임 파일) | 실행 파일 배치·스테이징은 이 문서 범위 밖이다(9.2절) |
| `PublicSystemIncludePaths` | 서드파티 include를 system include로 넘길지는 경고 정책과 함께 BS3에서 정한다 |
| `CircularlyReferencedDependentModules` | 순환을 금지한다 |
| `WITH_UNREAL_DEVELOPER_TOOLS`(`UEBuildTarget.cs:6349`) | 두지 않는다. 런타임 모듈은 계층 규칙상 Developer 모듈에 의존하지 않으므로, Developer 모듈이 있는지를 코드가 알 필요가 없다 |
| `.uproject` `Enterprise`·`MarketplaceURL`·`EngineAssociation` | Enterprise는 템플릿 분류용 표지다. MarketplaceURL은 스토어 연동이다. 엔진 연결은 `engineVersion`과 설치 SDK 경로로 한다 |

### 9.2 범위 밖

- PCH·유니티 빌드: [유니티 빌드·PCH 설계](UnityBuildPch.ko.md).
- 템플릿에서 새 프로젝트 만들기(UE `TemplateDefs.ini`의 폴더·파일 이름 바꾸기·내용 치환, 분류, 현지화된 표시 이름): [전체 계획](EngineImplementationPlan.ko.md) 1945행의 `rocks template` 계획이며 2026-11-14에 보류했다.
- 플랫폼별 설정 파일(`Config/<Platform>/`): [설정 시스템](ConfigSystem.ko.md)의 플랫폼 층이다.
- 패키징·스테이징(실행 파일 옆 런타임 파일, UE `RuntimeDependencies`·`AllowedConfigFiles`·`DirectoriesToAlwaysStageAsNonUFS`, Android 패키징 설정): [에셋 시스템](AssetSystem.ko.md) 18절 "범위 밖과 후속"의 Android 패키징 후속과 전체 계획 2430행 부근의 패키징 계획에서 정한다.
- Server 타깃의 기능 축소 범위: 네트워크 단계 스펙(3.7절).
- 모듈 수명 객체·로더(UE `IModuleInterface`·`FModuleManager`·플러그인 로딩 단계): [모듈 로더](ModuleLoader.ko.md)(BD30). 이 문서는 요구 계약만 정한다.
- 코드 다시 불러오기(UE Live Coding — Win64 비출시 구성, `TargetRules.cs:1587-1590`): `Modular` 링크가 생긴 뒤 별도 스펙으로 정한다.

## 10. 개정 이력

| 개정 | 날짜 | 내용 |
|---|---|---|
| 1 | 2026-11-19 | 첫 초안. UE 5.8 Templates 대조에서 나온 빌드 공백(타깃 종류, 의존 공개 범위, 플러그인 활성 조건)을 정리했다. 전용 서버 필수 요구(사용자 지시 — 네트워크 구성 뒤, 서버 플랫폼 Windows·macOS·Linux)로 3.7절·BD18·BS6·BS7을 더했다. 서브에이전트 3개(이 문서 UE·Rocks 코드 대조, UI 툴킷 7.3절 대조, Templates 독립 재대조)로 교차 검증해 반영했다: 플러그인 조건 해석을 UE 의미론으로 바꿈(치명), `bCompileAgainstEditor` 인용, 템플릿 사실, 에디터 데이터의 생성 코드·descriptor·로드 계약, UHT식 선언 규칙, 엔진 모듈 type, Release 배제 확장, 매크로 주입 위치, SDK 변형 key와 Program 기본 조합, 플러그인 처리 순서, 의존 소비처 전부, 서드파티 우회 제거, 시험 실행 파일 규칙, UnityBuildPch 제공 모듈 선택, depfile 선행, 9.1 표 보강. 반영본을 서브에이전트 1개로 다시 검증해 반영했다: 대리 클래스·interface 다중 상속(UI 툴킷 7.3절 (f)), 블록 안 허용 선언의 범위(BD6), 어휘 스캔 판정 수단, 에디터 패키지 저장 거부(BD26), `SourceGroups`(BD22), 타깃 종류별 Release 평가 플랫폼, type 사이 의존 규칙(BD25), 7절 표 보강, 인용 행 정정 |
| 2 | 2026-11-19 | BD17을 사용자 결정("언리얼 방식대로")으로 확정했다. 3.8절 링크 방식을 더했다: `LinkType`(Editor 기본 `Modular`), `ROCKS_MONOLITHIC`, 모듈 API 매크로(BD27), 리플렉션 타입 정보 항상 export(BD28), 공개 헤더 정적 저장소·`thread_local` 금지와 서드파티 단일 소비(BD29), 모듈 로더 별도 스펙과 공유 빌드 환경(BD30), `Modular` 시험 빌드(BD31), 단계 BS8. BD5에 Editor 변형을 더하고 BS5·7절·9절을 고쳤다. 서브에이전트 1개로 교차 검증해 반영했다: 생성 코드가 모두 inline이고 타입 키가 주소라 `Modular`에서 갈라짐(BD28을 생성 `.cpp`로), 비공개 심볼을 쓰는 시험의 `Modular` 링크(BD31), `CompileWithEditor` Program의 SDK 변형 key, 서드파티 다중 소비 현황(UE와 같이 허용하고 전역 상태만 제한), 가시성 옵션을 UE와 같게(`-fvisibility-ms-compat`), rpath·install name·`-fPIC`, 상속 export, export 수 상한, 위반 목록 보강, 인용 행 정정. 같은 날 [모듈 로더](ModuleLoader.ko.md) 개정 1을 쓰며 BD30(단계 관계·실행 파일 import 범위), 3.8절 Linux `-soname`, BS8 빌드 정보 정의 위치를 고쳤다 |
| 3 | 2026-11-20 | 3.6절에 첫 엔진 플러그인 `Engine/Plugins/Importers/UnrealExchange`(Unreal 교환 스펙 UXR7)를 적었다: Developer 모듈 하나라 Release 게임에서 빠짐, 기본 비활성·Commandlet이 켬, 명령 확장점과 플러그인 콘텐츠 마운트는 UX1에서 정함 |
| 4 | 2026-11-20 | BD25의 의존에 실행 중 적재 관계(`DynamicModules` — [모듈 로더](ModuleLoader.ko.md) 개정 4, 3.8절)를 넣었다. Monolithic이 그 모듈을 링크하므로 같은 type 검사를 받는다(UE도 동적 적재 모듈을 범위 검사에 넣는다) |
| 5 | 2026-11-20 | BS0 구현 세부(3.5절): 두 목록과 `AllDependencies`, 규칙 `PublicDependencies`·`PrivateDependencies`, 보임 계산 `ModuleVisibility` 한 곳, 도달은 두 목록, 두 목록 중복 거부, SDK manifest 12, 완료 기준을 모든 타깃 `compile_commands.json` 동일로 확인 |
| 6 | 2026-11-20 | BS8을 BS8a(빌드 기반)·BS8b(생성 코드 키)·BS8c(공유 상태)·BS8d(export와 검사 체인)로 나눴다. BS8a 구현 세부: API 매크로는 모듈 정의 헤더(보이는 C++ 모듈마다), 공유 PCH 공유 정의에 import 값, 예약 매크로 거부, Modular 공유 라이브러리 파일·링크·rpath·가시성, 완료 기준(Monolithic 인자는 `ROCKS_MONOLITHIC=1`만 더해짐, Modular fixture 링크·실행) |
| 7 | 2026-11-21 | BS8b 구현 세부(사용자 결정 — UE 방식): `ROCKS_GENERATED_BODY()` 의무(리플렉션 쪽 RMC2211·`ReflectedDeclarationV`를 씀), `GeneratedType<X>::NativeKey()`(생성 `.cpp` 저장소, API 매크로), `NativeTypeKey<T>()`는 리플렉션 클래스면 그것을 쓰고(클래스 키 연산표는 함수 안 정적 표) 생성 헤더가 없으면 `static_assert`, 생성 소스는 클래스 대상 생성 헤더 include, struct·enum·native 등록 타입은 inline 키 유지(한계 — struct는 헤더 템플릿 상수 연산표). 버린 안: 리플렉션 헤더의 생성 헤더 include, 이름 해시 키 |
| 8 | 2026-11-21 | 3.9절 엔진 플러그인 빌드를 더했다(BD32–BD42, BS9a–BS9d, 4절 10항, 7절 세 행). 사용자 지시 "진행 가능한거부터"에 따라 Unreal 교환 리더(UX1a)를 막는 빈 곳을 메운다. rocks-07과 조율해 rocks-ff가 맡는다. UE 근거는 5.8.4 UBT(`Plugins.cs`·`EnumeratePlugins.cs`·`UEBuildTarget.SetupPlugins`·`PreBuildSetup`·`ModuleDescriptor.IsCompiledInConfiguration`·`TargetRules`의 플러그인 목록과 `bBuildDeveloperTools`·`RulesCompiler`) |
| 9 | 2026-11-21 | BS9a 구현 세부(3.9절): `PluginReferrer`·`DescriptorOwner`·`TargetPluginReferences`, `BuildContext.PluginRoot`·`Plugins`, `TargetType`을 `Rocks.Build` 어셈블리로 옮김, 게임 빌드 해석을 규칙 로드 뒤로 |
| 10 | 2026-11-21 | BS9b 구현 세부(3.9절): `PluginModuleRules`(봉인 Configure, 기술자 값), `SourceModuleRules.ModuleType`과 정책 `module-type-group`, `Apply`의 위치·계층 검사(`EngineRoot`), Release 배제의 플러그인 해석과 `release-plugin-resolution`, `plugin-module-rules-base` 범위. [모듈 로더](ModuleLoader.ko.md) 개정 7 |
| 11 | 2026-11-21 | BS9c 구현 세부(3.9절): `PluginModules`와 `LinkIntoPrimary`, `LinkClosure` 순서(링크 폐포 → 플러그인 모듈 → 동적 모듈), CLI 엔진 해석과 빌드 근, SDK export·`Modular` 거부 |
| 12 | 2026-11-21 | BS9d 구현 세부(3.9절): UnrealExchange 플러그인 골격, Commandlet·EngineTests의 `Plugins`, `PluginTestModules`, 엔진 주인 `Apply`의 도달 범위 축소 |
| 13 | 2026-11-21 | BS8c 구현 세부: BD29 판정을 Clang AST 대신 공개 헤더 검사의 `-Wunique-object-duplication`(LLVM Clang 20+, Apple clang 제외)과 `thread_local` 어휘 정책 `public-header-static-storage`로, 지금 위반 정리(할당기·Correlation·TraceLogSink·CVar 바인딩·기본값 쓰기 탐침·fixture), 서드파티 `ThirdParty.json` `globalState` 필수 키와 단일 직접 소비 검사(Sodium만 true). 한계: 템플릿 문맥 저장소 |
| 14 | 2026-11-21 | BS8d와 ML4를 한 묶음으로(사용자 결정): Modular에서 모듈 표·빌드 정보가 풀리지 않는 문제, 순서 BS8d1(빌드 기반·manifest 쓰기) → ML4a(manifest 적재) → ML4b(실행 파일 연결) → BS8d2(export) → BS8d3(진단) → BS8d4(남은 키) → BS8d5(검사 체인). BS8 완료 기준은 ML4와 함께 채운다. 버린 안: 실행 파일 표 export·OS 기호 조회 |
| 15 | 2026-11-21 | BS8d1 구현 세부: Modular 빌드 정보 TU를 Core 공유 라이브러리에(`ROCKS_CORE_API`), 모듈 표 TU 대신 manifest(`.modules.json` — 행 배열·크기·`buildId` 재사용), `ROCKS_MODULE_SHARED_LIBRARY`와 export 진입점 `RocksModuleEntry`·강제 참조, Modular 거부를 Launch 기준으로 좁힘 |
| 16 | 2026-11-22 | ML4b 빌드 쪽 세부: Launch에 닿는 Modular 실행 파일의 모듈은 호스트 공유 라이브러리, 실행 파일은 생성 진입 TU + Launch import만, manifest schema 3·행 키 `host`, Launch 거부를 풀고 Launch 없는 Modular 실행 파일을 거부([모듈 로더](ModuleLoader.ko.md) 3.14절) |
| 17 | 2026-11-22 | BD5에 빌드 구성 축을 적었다: SDK 변형(매크로 조합 × 링크 방식)과 직교하며, SDK 하나가 여러 구성을 `Configurations/<구성>/`에 싣는다(manifest 14, [Visual Studio 프로젝트 파일](ProjectFiles.ko.md) 10.3절 PJ4) |
| 18 | 2026-11-22 | BS8d2 세부: Foundation 폐포 Modular 측정(미해결 기호 735 — 클래스 93·자유 함수 162·변수 2), 붙이는 규칙(클래스 단위 우선·선언마다·공개 헤더만·비공개 선언은 공개로 옮김), 링크 오류를 따라 붙임, 확인 타깃 `ModularCheck`(Modular 빌드·실행으로 ML4b Launch 성공 경로 확인) |
| 19 | 2026-11-22 | BS8d3 세부: 상속 export 진단(선언부 원문의 API 매크로 → descriptor `Exported` 표지·schema 2, import base에 없으면 RMC2002, 루트 Object는 정책 필수), 다른 모듈이 상속하는 Object·Actor·SceneComponent·Asset 클래스 단위 export, Windows Modular export 수 진단(모듈별 줄·상한 90% 경고) |
| 20 | 2026-11-22 | 오디오 모듈 유형([오디오 시스템](AudioSystem.ko.md) AD24): `AudioMiniaudio`·`AudioOpus`는 ClientOnly, `Audio`·`EngineAudio`는 Runtime, Server 쿠킹은 오디오 payload 제외 — 3.1절 Server 기능 축소 문단 |
| 21 | 2026-11-22 | BS8d4 세부(사용자 결정 — UE 방식 생성 키 확장): 리플렉션 struct·enum 생성 키(NativeKey), Binds 실제 타입 목록 `BoundCoreValueV`와 CoreTypeBindings 생성 키, native 등록 타입 키 매크로 쌍, `StructElementOps`에서 키를 빼고 원소를 지우는 호출이 키를 넘김, `ROCKS_DECLARE·DEFINE_GRAPH_SHARED_DATA`와 호스트 서비스 키의 `Api` 인자 |
| 22 | 2026-11-22 | BS8d5 세부(사용자 결정 — 시험용 내부 export): Modular FoundationTests 측정(1,174 — 공개 359·내부 85), BD31 개정(시험은 호스트 하나, 순환 6개 모듈 때문에 모듈 공유 라이브러리 링크를 버림, 내부 선언 export·비공개 헤더 허용 목록), 시험 타깃 `FoundationModular`, 정적 모듈 표 시험, `constinit`과 dllimport 주소, inline 함수 주소·thread_local·constexpr 멤버 클래스 export 한계, 무할당 시험·부트 모듈 두 번 적재 한계, `ModularCheck`를 `FoundationModular`로 대체 |
| 23 | 2026-11-22 | BS8e Engine 층 Modular export 세부: 측정 364곳(비공개 헤더 셋, friend 재선언), 세션 분담, 시험 타깃 `EngineModular`(시험 모듈 `EngineModularTests`, 엔진 플러그인 시험 제외 — 플러그인 Modular는 ML5), 정적 모듈 표 Engine 시험 둘, 타입 키 기구를 Core `TypeKeyOf`로(Subsystem·렌더 설정 참조·콘솔 명령 대상), 정책 `public-header-type-tag`, 이름 상수 주소, BD27 inline 멤버 예외 |
| 24 | 2026-11-23 | 3.1절에 타깃 속성 `CompanionTargets`를 더했다(셰이더 시스템 SC14 — 동반 프로그램 타깃을 같은 빌드·출력 폴더에, Monolithic만). |
