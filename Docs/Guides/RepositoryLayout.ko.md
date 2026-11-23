# 저장소 구성

Rocks 저장소는 **Core(기반 라이브러리)**와 **Engine(게임 엔진 계층)**을 최상위에서 분리한다. Core는
엔진이 아니라 엔진·도구·게임이 함께 쓰는 바탕이다. 모듈 단위 구성과 규칙 파일 배치는 UE 5.8(5.8.4, commit `f45729c0b6f0`)
(`Engine/Source/<그룹>/<모듈>/<모듈>.Build.cs`, `Engine/Plugins/<분류>/<플러그인>`)을 따르되, UE에서
`Engine/Source/Runtime/Core`에 해당하는 기반 계층을 별도 최상위 `Core/`로 둔 점이 다르다.

```text
rocks/
  Core/                       기반 라이브러리(엔진 아님)
    Source/
      Runtime/                모듈: Base PlatformBase Core Compression TraceCapture Task AsyncCompression Serialization MetaSchema
                              Localization ObjectRuntime ObjectSerialization ReflectionText Config
      ThirdParty/             Core가 쓰는 고정 버전 외부 라이브러리(Lz4·Zstd·Zlib·Brotli·Sodium 원본, UnicodeData·
                              JsonTestSuite·Wycheproof 데이터). <Lib>/ThirdParty.json·<lib>-<version>/(릴리스 원본을
                              byte 그대로)·code 종류는 <Lib>.Build.cs와 필요 시 Rocks 글루 .c
        <Module>/
          <Module>.Build.cs   모듈 규칙(CoreModuleRules 상속)
          Public/             다른 모듈이 include하는 헤더
          Private/            구현과 모듈 내부 헤더(플랫폼 구현은 Private/<플랫폼>/)
          Tests/              모듈 테스트(테스트 실행 파일에 들어가고 모듈 라이브러리에는 안 들어간다)
      Developer/
        TestSupport/          테스트 지원 코드(Public/TestSupport/*.h, Private/<플랫폼>/)
  Engine/                     게임 엔진 계층(World·Renderer·Gameplay·Editor 등, 단계적으로 채운다)
    Source/
      Engine.Target.cs        게임이 링크하는 런타임 모듈 묶음(Installed SDK 기본 타깃)
      Runtime/                게임에 들어가는 엔진 모듈(EngineModuleRules 상속)
      Developer/              도구·에디터에서만 쓰는 엔진 모듈
      Editor/                 에디터 모듈
      ThirdParty/             엔진·도구 전용 외부 라이브러리(같은 manifest 규칙). 헤더 전용(kind: headers, Vulkan-Headers),
                              C 원본(SPIRV-Reflect), 미리 빌드한 바이너리(kind: binary, DXC·SPIRV-Cross·SPIRV-Tools는
                              공개 헤더와 <Lib>/Build/ 오프라인 빌드 스크립트만, FXC·Metal 도구는 manifest만)
    Plugins/<분류>/<플러그인>/   엔진 플러그인(<플러그인>.rocksplugin, Source/<모듈>/<모듈>.Build.cs)
  Programs/                   실행 프로그램 전부
    FoundationTests/          Core 모듈 테스트 실행 파일(C++, 규칙·타깃 포함)
    Commandlet/               headless 명령 실행기(C++)
    Rocks.Build …             빌드·메타 컴파일·검사·scaffold 도구(C#/.NET)
    Scaffold/Templates/       새 모듈 템플릿
  Build/                      규칙 어셈블리(Rules/)·C# 서식 검사 작업 영역(Format/)·공통 빌드 설정·CI 스크립트,
                              Build.version(엔진 버전 단일 출처 — ReflectionFoundations 8.2절)
  Templates/                  게임 프로젝트 템플릿
  Assets/Branding/            엔진 브랜드 자산(로고 SVG·PNG, 시각 아이덴티티 가이드라인)
  Docs/                       설계·구현·검토 문서(파일 크기·장별 폴더 규칙은 Guides/DocumentLayout.ko.md)
  Rocks.proj                  MSBuild 진입점(Build·Test·InstalledSdk·Game)
```

## 계층 규칙

- 의존 방향은 `Core ← Engine ← 게임·플러그인`이다. Core 모듈은 Engine 모듈을 참조하지 않는다.
  같은 계층 안에서도 아래 모듈이 위 모듈을 참조하지 않는다([모듈 구조](../Design/EngineImplementationPlan.ko.md) 3절).
- 공개 API는 `Public/`에만 둔다. 다른 모듈은 `#include <Core/...>`처럼 `Public/` 기준 경로로 include한다.
  폴더를 옮겨도 include 문은 바뀌지 않는다.
- 플랫폼 구현은 `Private/<플랫폼>/`(Win64·Mac·Posix·Apple·Android)에만 둔다. 공용 소스에 OS 헤더와
  플랫폼 조건부 전처리를 쓰지 않는다(policy 검사가 확인한다).
- 런타임 소스(Core·Engine·플러그인·프로그램)는 STL을 쓰지 않는다. 모듈 `Tests/`와 테스트 실행 파일은
  STL 정책 밖이다(STL 호환성 테스트가 STL을 직접 쓴다).
- 외부 라이브러리는 Core가 쓰는 것이면 `Core/Source/ThirdParty/<Lib>/`, 엔진·도구 전용이면 `Engine/Source/ThirdParty/<Lib>/`에
  둔다(Core는 Engine을 참조하지 않는다). 라이브러리 이름은 두 루트를 통틀어 하나다. 원본 디렉터리는 수정하지 않고
  `ThirdParty.json`(종류·버전·출처·라이선스·파일별 SHA-256·번역 단위·정의·컴파일러 지시문 목록)이 단일 출처다. 종류는
  `code`(C 번역 단위)·`data`(데이터 파일)·`headers`(헤더 전용)·`binary`(미리 빌드한 바이너리)이며, `binary`의 파일은
  저장소와 `out/`이 아니라 bootstrap 명령이 받는 사용자 단위 내용 주소 저장소에 둔다([셰이더 시스템](../Design/ShaderSystem/08-ThirdPartyToolBuild.ko.md) 8절). 정책 검사 `third-party`가 manifest와 원본의 일치를 확인하며, lexical 검사는 manifest가 선언한
  원본 디렉터리만 제외한다. 외부 헤더는 소비 모듈의 Private 구현에만 노출한다. 규칙과 절차는
  [압축 설계](../Design/Compression.ko.md) 3절을 따른다.

## 규칙 파일

- 모듈 규칙은 모듈 폴더의 `<Module>.Build.cs`, 타깃 규칙은 해당 폴더의 `*.Target.cs`다.
- 빌드 도구는 `Build/Rules/Rocks.Engine.Rules.csproj`가 다음 위치의 규칙을 모두 모아 컴파일한 어셈블리를
  읽는다(UE UBT의 rules 어셈블리와 같은 방식): `Core/Source/**`, `Engine/Source/**`, `Engine/Plugins/**`,
  `Programs/**`(C# 도구 프로젝트 `Programs/Rocks.*` 제외).
- 공통 기반 클래스는 `Build/Rules/SourceModuleRules.cs`의 `CoreModuleRules`·`EngineModuleRules`다.
  모듈 폴더 경로(`<Layer>/Source/<Group>/<Module>`)를 계산하므로 새 모듈 규칙은 이름과 의존성만 적는다.

```csharp
// Core/Source/Runtime/Core/Core.Build.cs
public sealed class Core : CoreModuleRules
{
    protected override string ModuleName => "Core";
    protected override string[] PublicDependencies => ["PlatformBase"];
}
```

## 새 모듈 추가

1. `Core/Source/Runtime/<Module>/`(또는 `Engine/Source/<그룹>/<Module>/`)에 `Public/`·`Private/`와
   `<Module>.Build.cs`를 만든다.
2. 테스트는 `<Module>/Tests/`에 두고 `Programs/FoundationTests/FoundationTests.Build.cs`의 테스트 모듈
   목록에 이름을 더한다.
3. 다른 모듈은 `PublicDependencies`(공개 헤더가 쓰는 모듈 — 소비자에게도 보인다)나 `PrivateDependencies`(구현만 쓰는 모듈)에 이름을 넣어 의존한다([빌드 타깃](../Design/BuildTargets.ko.md) 3.5절).

## 프로젝트·플러그인 기술자

게임 프로젝트는 폴더 최상위에 `<프로젝트>.rocksproject`, 플러그인은 `<플러그인>/<플러그인>.rocksplugin`을
둔다(UE `.uproject`·`.uplugin` 대응, UTF-8 JSON, 필드 이름은 camelCase). 읽기·검증·플러그인 해석은
`Programs/Rocks.Build/Descriptors.cs`(`DescriptorReader`·`PluginCatalog`)다.

```json
{
  "fileVersion": 1,
  "engineVersion": "0.1",
  "description": "예제 게임",
  "modules": [ { "name": "MyGame", "type": "Runtime", "loadingPhase": "Default" } ],
  "plugins": [ { "name": "Water", "enabled": true } ],
  "targetPlatforms": ["Windows", "MacOS", "Android"]
}
```

| 필드 | 프로젝트 | 플러그인 | 뜻 |
|---|---|---|---|
| fileVersion | 필수 | 필수 | 기술자 형식 버전(지금 1). 다른 값은 오류 |
| engineVersion | 필수 | — | 대상 엔진 버전 "주.부"(앞자리 0 없는 10진, 각 0–65535). 게임 빌드가 설치 SDK 엔진의 주·부와 같은지 본다 |
| engineAssociation | 선택 | — | 이 게임이 쓸 설치 SDK: 경로(`/`·`\`를 담으면 `.rocksproject` 디렉터리 기준) 또는 등록 ID `{GUID}`. 없으면 등록 SDK 중 `engineVersion`이 맞는 최고 패치를 고른다. 빈 문자열은 오류([Visual Studio 프로젝트 파일](../Design/ProjectFiles.ko.md) 10.1절) |
| version·versionName·friendlyName | — | 필수 | 플러그인 버전(정수 1 이상)·표시 버전·표시 이름 |
| description·category | 선택 | 선택 | 설명·분류 |
| enabledByDefault | — | 선택(false) | 프로젝트가 따로 적지 않을 때 켤지 |
| modules | 선택 | 선택 | `{name, type(Runtime·Developer·Editor), loadingPhase(기본 Default), platforms}` |
| plugins | 선택 | 선택 | 프로젝트: `{name, enabled}`로 켜고 끔. 플러그인: 의존 목록(`enabled`는 true만) |
| targetPlatforms | 선택(모두) | — | Windows·MacOS·Android |

- 모르는 필드·중복 필드·잘못된 타입·주석·끝 쉼표·1 MiB 초과는 오류다. 모듈·플러그인 이름은 영문자로
  시작하는 식별자(최대 64자)이며 대소문자만 다른 중복도 오류다.
- 플러그인 탐색 위치는 `Engine/Plugins/`와 `<프로젝트>/Plugins/`이며 같은 이름이 둘 이상이면 오류다.
- 켤 플러그인: 프로젝트가 `enabled: true`로 적은 것과, 적지 않은 것 중 `enabledByDefault`인 것. 그 의존은
  자동으로 켠다. 프로젝트가 끈 플러그인을 다른 켜진 플러그인이 요구하거나, 없는 플러그인을 참조하거나,
  의존이 순환하면 오류다. 결과는 의존이 앞에 오는 순서다.

### 게임 빌드 연동

설치 SDK로 게임을 빌드할 때(`Rocks.proj /t:Game`) `DescriptorModuleSet`이 기술자로 빌드할 모듈을 정한다.

1. 프로젝트 루트에 `.rocksproject`가 정확히 하나 있어야 한다. 이번 플랫폼이 `targetPlatforms`에 없으면 빌드하지
   않는다. `engineVersion`의 주·부가 설치 SDK manifest의 `EngineVersion`과 다르면(오래된 프로젝트·더 새 프로젝트
   모두) `<경로>: engineVersion X.Y does not match the installed SDK engine A.B.C; set engineVersion to A.B to upgrade
   the project.`로 멈춘다(패치는 보지 않는다 — [ReflectionFoundations](../Design/ReflectionFoundations.ko.md) 8.2절).
2. `<프로젝트>/Plugins/`의 플러그인을 찾고 위 규칙으로 켤 플러그인을 정한다.
3. 프로젝트와 모든 플러그인(켜지지 않은 것 포함)의 모듈을 C# 규칙 모듈과 이름으로 대조한다. 규칙은 있는데
   기술자가 선언하지 않은 모듈, 선언은 있는데 규칙이 없는 모듈, 대소문자만 다른 이름, 여러 기술자의 같은 모듈
   이름은 오류다.
4. 다음 모듈은 이번 빌드에서 뺀다. 타깃이나 남은 모듈이 빠진 모듈을 쓰면 이유와 함께 오류다.
5. 켠 플러그인의 남은 모듈은 타깃이 참조하지 않아도 빌드하고 타깃의 주 실행 파일(`Modules` 첫 항목)에 링크해
   모듈 표에 기술자 단계로 싣는다(UE처럼 — [빌드 타깃](../Design/BuildTargets.ko.md) 3.9절). Program 타깃은
   `enabledByDefault`를 받지 않고 명시한 플러그인만 켠다. 타깃 규칙 `Plugins`가 같은 이름의 프로젝트 참조를 이긴다.
   엔진 저장소 빌드도 `Engine/Plugins`에 같은 규칙을 적용한다(플러그인 모듈 규칙은 `PluginModuleRules`). 설치 SDK는 엔진
   플러그인을 내보내지 않는다.

| 조건 | 이유 |
|---|---|
| 켜지지 않은 플러그인의 모듈 | 프로젝트가 끄거나 `enabledByDefault`가 false이고 아무도 요구하지 않음 |
| `type: Editor` | 에디터 타깃이 아직 없음 |
| `type: Developer`이고 Game 타깃의 Release | 배포 빌드에 개발 도구를 넣지 않음. UE 5.8의 `DeveloperTool`과 같은 의미다(`bBuildDeveloperTools` 기본값 — 에디터·프로그램 타깃이거나 Test·Shipping이 아닌 구성. UE `Developer`는 에디터·프로그램 타깃 전용. UnrealBuildTool `Configuration/Descriptors/ModuleDescriptor.cs` 794–797행, `Configuration/Rules/TargetRules.cs` 1212행) |
| `platforms`에 이번 플랫폼이 없음 | 플랫폼 전용 모듈 |

규칙 계약: 규칙 로더는 모듈 이름을 알기 위해 모든 모듈 규칙의 `Configure`를 먼저 실행한 뒤 위 필터를 적용한다.
따라서 빠질 모듈(켜지지 않은 플러그인, 다른 플랫폼 전용)의 규칙도 예외 없이 결과를 돌려줘야 한다.

## 후속

- 타깃 종류 확장(Editor·Client·Server와 `ROCKS_WITH_EDITOR` 계열 매크로), 모듈 의존의 Public·Private 구분, 프로젝트 플러그인 참조의
  활성 조건(타깃·플랫폼·구성), 모듈 type ServerOnly·ClientOnly: [빌드 타깃 설계](../Design/BuildTargets.ko.md). 구현하면 위 기술자 표·모듈
  type 표·게임 빌드 연동 절을 함께 고친다.
- 규칙 이름을 `Configure` 전에 알 수 있게 해(UBT처럼 파일·클래스 이름 규약) 빠질 모듈의 규칙을 실행하지 않는다.
- `loadingPhase`의 런타임 반영: [모듈 로더](../Design/ModuleLoader.ko.md) 3.3·3.11절(ML1)이 모듈 표 행의 단계로 반영한다(`PostEngineInit`·`None` 포함). 기술자 모듈의 플러그인 활성 반영은 ML5다.
- 소스 트리 프로젝트 빌드가 생기면 그 `.rocksproject`의 engineVersion을 `Build/Build.version`과 대조한다(지금 기술자는
  설치 SDK 게임 빌드에서만 읽고, 그때 SDK manifest의 엔진 버전과 대조한다).
- 2026-11-12 이전 구조(`Engine/<Module>`, 최상위 `Tests/`·`Tools/`)를 가리키는 과거 검토·배포 문서
  (`Docs/Reviews`, `Docs/Releases`)는 기록이므로 경로를 고치지 않는다. 계획 데이터
  (`CoreObjectInputData.json`, `CoreObjectInputFiles.ko.csv`, `CoreObjectInputTasks.ko.csv`)의 예정 파일
  경로도 개편 전 표기이며, 해당 항목을 구현할 때 이 문서의 구성으로 옮긴다.
