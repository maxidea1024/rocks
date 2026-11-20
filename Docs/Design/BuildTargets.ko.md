# 빌드 타깃·모듈 의존·플러그인 조건 설계

기준일: 2026-11-20, 개정 5. Rocks.Build에 없는 UE 5.8 빌드 기능 네 가지를 정한다.
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
- **`WithEditorData`**: Program 타깃 속성이다. 켜면 `ROCKS_WITH_EDITOR_DATA=1`이다. 에디터 데이터를 읽는 에셋 도구용이다. 기본은 꺼짐이다(BD19). UE는 Program 기본값이 1이지만(`TargetRules.cs:1201-1203`) 시험 타깃은 0이다(`UBT/Configuration/Rules/TestTargetRules.cs:316-317`). Rocks Program의 대부분이 시험·벤치마크라 0을 기본으로 한다. 이렇게 하면 Program의 기본 매크로 조합이 Game과 같아 설치 SDK 라이브러리를 공유한다(3.2절).
- **Server 타깃의 기능 축소**(렌더·오디오·입력 backend 제외, UE `USE_NULL_RHI=1` — `TargetRules.cs:3492-3493`)의 범위는 네트워크 단계 스펙에서 정한다. 이 문서는 그 결정이 기댈 type·매크로·검사를 준비한다(3.7절).
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
- 리플렉션 선언은 모듈 규칙 `Metadata` 목록의 헤더에만 쓸 수 있으므로(기존 정책 `reflection-metadata-not-literal` 등), include를 거쳐 다른 파일에 숨은 선언은 이미 거부된다.

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

**첫 엔진 플러그인**(Unreal 교환 UXR7, 2026-11-20 — 설계, 구현은 UnrealExchange UX1). `Engine/Plugins/Importers/UnrealExchange/`([Unreal 교환 형식](UnrealExchange.ko.md) 10.1절)가 엔진 저장소의 첫 플러그인이 된다. 엔진 규칙 프로젝트가 이미 `Engine/Plugins/**`의 규칙을 모으고(Build/Rules/Rocks.Engine.Rules.csproj:15) PluginCatalog가 그 경로를 찾는다.
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

**모듈 API 매크로(BD27).**
- 빌드가 C++ 모듈마다 `ROCKS_<MODULE>_API`(모듈 이름 대문자)를 정의한다. UE `<MODULE>_API`(`UEBuildModule.cs:223, 239`)에 해당하고, 이름은 Rocks 매크로 접두 규칙을 따른다.
- 값은 UE와 같다(`UEBuildModule.cs:750-770`). `Monolithic`이면 빈 값이다. `Modular`이면 그 모듈 자신의 컴파일에서는 export, 다른 모듈의 컴파일에서는 import다.
- export·import 철자는 `Base/Compiler.h`의 `ROCKS_DLLEXPORT`·`ROCKS_DLLIMPORT`다. Windows는 `__declspec(dllexport)`·`__declspec(dllimport)`, macOS·Linux는 둘 다 `__attribute__((visibility("default")))`다.
- `Modular` 빌드의 가시성 옵션은 UE와 같다(`bHideSymbolsByDefault`). Linux는 공유 라이브러리 컴파일에 `-fvisibility-ms-compat`(`Platform/Linux/LinuxToolChain.cs:432-446` — 공유 라이브러리가 아닐 때만 `-fvisibility=hidden`), macOS는 `-fvisibility-ms-compat`·`-fvisibility-inlines-hidden`(`Platform/Mac/MacToolChain.cs:158-162`)이다. 함수·변수는 기본으로 숨겨지고 타입 정보는 보이므로, 함수·변수의 export 누락이 세 플랫폼 모두에서 링크 오류로 드러난다.
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
- 판정은 공개 헤더 검사(BD9)가 헤더마다 하는 컴파일에서 Clang AST로 한다(정책 이름 `public-header-static-storage`). Monolithic·Modular와 무관하게 늘 검사해, Editor 빌드에서만 드러나는 상태 분열을 막는다.
- 지금 위반의 예(BS8에서 고친다, 전체 목록은 BS8의 정책 검사가 찾는다):
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

## 5. 결정 사항

| 번호 | 결정 | 상태 |
|---|---|---|
| BD1 | `TargetType`을 Game·Editor·Client·Server·Program 다섯 값으로 넓힌다. 타깃 종류별 플랫폼은 3.1절 표다 | 권장안 |
| BD2 | 매크로 `ROCKS_WITH_EDITOR`·`ROCKS_WITH_EDITOR_DATA`·`ROCKS_WITH_SERVER_CODE`·`ROCKS_WITH_CLIENT_CODE`를 모든 모듈에 0·1로 정의한다. 타깃 안에서 같은 값을 강제하고, 기본값 없이 `Base/Compiler.h`가 검사한다. `ROCKS_WITH_EDITOR=1`이면 `ROCKS_WITH_EDITOR_DATA=1`이다. 서버·클라이언트 코드 값은 타깃 종류로만 정한다 | 권장안 |
| BD3 | 타깃 종류 매크로(UE `UE_GAME`·`UE_EDITOR`·`IS_PROGRAM` 등)는 두지 않는다. 실행 중 분기는 `build::Description().targetType`, 컴파일 분기는 BD2 매크로로 한다 | 권장안 |
| BD4 | 모듈 type은 Runtime·Developer·Editor·ServerOnly·ClientOnly다. 게임·플러그인 모듈은 기술자 type으로 거르고, 엔진 모듈은 `SourceModuleRules.ModuleType`(기본은 Group 폴더)으로 도달 검사만 한다. SDK manifest에 type을 싣는다 | 권장안 |
| BD5 | 설치 SDK는 매크로 조합마다 엔진 라이브러리 변형을 싣는다(key는 조합). 1차는 Game 조합만이며 게임의 Game 타깃과 기본 Program 타깃을 빌드할 수 있다. 없는 조합은 오류이고, 변형을 더하면 `ManifestVersion`을 올린다. Editor 변형은 key에 링크 방식(`Modular`)을 더하고 Rocks Studio 실행 파일과 엔진 모듈 공유 라이브러리를 싣는다(3.8절) | 권장안 |
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
| BD31 | Editor보다 먼저 `Modular` Program 시험 빌드(FoundationTests)를 검사 체인에 넣어 export 누락과 공유 상태 위반을 잡는다(Windows·macOS). 대상 모듈의 `Private/`를 받는 시험 묶음(BD22)은 `Modular`에서 그 모듈의 공유 라이브러리에 함께 링크한다(UE도 모듈의 `Private/Tests` 시험을 모듈 바이너리에 컴파일한다 — 예 `Runtime/Core/Private/Tests`). 시험 등록 표는 Core가 한 벌 가진다 | 권장안 |

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
| BS8 | 모듈식 링크 기반(3.8절) — 선행: BS0. `LinkType`·`ROCKS_MONOLITHIC`·Android 거부·파일 이름, 모듈 API 매크로 주입과 `ROCKS_DLLEXPORT`·`ROCKS_DLLIMPORT`, 숨김 가시성, 예약 매크로 검사, 가시성·`-fPIC`·rpath·install name, 생성 `.cpp`와 `NativeTypeKey` 교체(BD28), 상속 export 진단과 모듈별 export 수 진단, 기존 모듈의 모듈 사이 API에 매크로 붙이기, 정책 `public-header-static-storage`와 지금 위반 정리, `HasGlobalState` 표시와 단일 소비 검사, `Private/` 시험 묶음의 모듈 내 링크와 `Modular` FoundationTests 검사 체인(BD31), 실행 파일 생성 TU가 정의하던 빌드 정보(`ToolchainFingerprint`·`ToolBuildId`)의 Modular 정의 위치(Core 공유 라이브러리에 미해결 기호가 남지 않게) | `Modular` FoundationTests가 Windows·macOS에서 빌드·통과한다. `Monolithic` 빌드 결과(object 인자)는 매크로 값 외에 바뀌지 않는다. 공개 헤더의 `thread_local`·쓰기 가능한 inline 정적 저장소를 정책이 거부한다 |

순서: BS0 → BS1(UB0 depfile 파서 뒤) → BS3. BS2는 BS0 뒤가 쉽다(규칙 기반 클래스를 한 번에 고친다). BS4는 BS2 뒤다(`targets` 값 검증과 문맥별 해석에 다섯 값 `TargetType`이 필요하다). BS6은 다른 단계와 독립이다. BS7은 BS2·BS6 뒤이고, 렌더·오디오·입력 backend를 서버에서 빼는 범위는 BS7 전에 네트워크 단계 스펙이 정한다.

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
