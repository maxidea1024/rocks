# Visual Studio 프로젝트 파일 생성 설계

기준일: 2026-11-21, 개정 1. Rocks 저장소와 게임 프로젝트에서 Visual Studio 솔루션·프로젝트 파일을 만드는 방법을 정한다(UE 5.8 GenerateProjectFiles 대응).
- **엔진 모드**: 저장소 루트에서 `projectfiles --root`로 엔진·Programs·C# 도구 솔루션을 만든다. 엔진에는 `.rocksproject`가 없다.
- **IntelliSense**: 빌드 계획의 논리 compile 명령(`compile_commands.json`과 같은 원천)에서 파일별 설정을 만든다.
- **NMake 빌드**: 프로젝트의 빌드·다시 빌드·정리는 Rocks.Build를 부른다. Visual Studio가 직접 컴파일하지 않는다.
- **게임 모드**: `.rocksproject`의 SDK 연결, 다중 구성 SDK, 입력 snapshot 제외, 정리 명령, 출력 경로를 먼저 정하고 같은 생성기를 쓴다.
- **템플릿 생성**: 빌드되는 실제 프로젝트인 템플릿을 이름 치환과 ID 재발급으로 새 게임 프로젝트로 만든다.

2026-11-14 검토 결과(엔진 먼저, 계획 기반 IntelliSense, NMake, 게임 쪽 결정 네 가지, UE식 템플릿 생성)를 출발점으로 삼는다.

대상 코드는 `Programs/Rocks.Build`(ActionGraph·CompilationDatabase·Pch·Descriptors), `Programs/Rocks.Build.Cli`(Program·InstalledSdk·GameInputSnapshot), `Programs/Rocks.Build.Abstractions/RuleLoader.cs`, `Programs/Rocks.MetaCompiler`(identity 명세 명령), `Rocks.proj`, `Templates/Game/Foundation`이다.

UE 근거는 UE 5.8.4(`release`, 로컬 클론 `C:\github\UnrealEngine`, f45729c0b6f0)다. UBT 경로는 `Engine/Source/Programs/UnrealBuildTool/` 기준으로 적는다(`ProjectFiles/VisualStudio/VCProject.cs`·`VCProjectFileGenerator.cs`·`VisualStudioSolutionBuilder.cs`, `ProjectFiles/ProjectFileGenerator.cs`, `Modes/GenerateProjectFilesMode.cs`·`Modes/CleanMode.cs`). 그 밖에 `GenerateProjectFiles.bat`, `Engine/Build/BatchFiles/{Build,Rebuild,Clean,GenerateProjectFiles}.bat`, `Engine/Source/Programs/UnrealVersionSelector`, `Engine/Source/Developer/DesktopPlatform`, `Engine/Source/Editor/GameProjectGeneration`, `Templates/*/Config/TemplateDefs.ini`을 읽었다. UE 이름은 대응표와 근거에만 적는다. 레거시와 deprecated 경로는 따르지 않는다(`.sln` 병행 출력, VS 2022 이하 형식 선택, `.suo` 쓰기, UE4 시절 `EngineAssociation` 추정).

상태: **설계(개정 1)**. 결정 PJD1–PJD31은 권장안으로 확정했다(사용자 지시: 권장안으로 정하고 묻지 않는다). 구현 단계는 PJ0–PJ8(15절)이다. 리뷰어 교차 검증 뒤 착수한다.

## 1. 현재 상태와 문제

| 항목 | 현재 | 이 스펙에 주는 영향 |
|---|---|---|
| 솔루션 | C# 서식 검사용 `Build/Format/Rocks.Format.slnx`뿐이다. C++ 프로젝트 파일은 없다 | Visual Studio에서 엔진 C++을 열 방법이 없다. C# 프로젝트 목록은 이 파일이 이미 전부 가진다 |
| 빌드 진입 | `dotnet msbuild Rocks.proj /t:Build·Test·InstalledSdk·Game`(`Rocks.proj:45-62`). `BootstrapTools`가 매번 `dotnet build Programs/Rocks.Tools.csproj`를 돈다(`Rocks.proj:42-44`). CLI 경로는 Debug 고정이다(`Rocks.proj:23`, `Program.cs:516`) | NMake 명령의 호출 대상이다. 솔루션 빌드가 여러 프로젝트를 동시에 돌리면 도구 빌드가 겹친다(7.2절) |
| CLI 명령 | `build`·`test`·`sdk`·`game`·`bootstrap`·`unity-profile`(`Program.cs:45, 59-66`) | `clean`·`projectfiles`·`new-project`·`sdk-register`·`associate`가 없다 |
| compile DB | 출력마다 `compile_commands.json`. 원본 소스마다 논리 명령 하나이고 `directory`·`file`·`output`·`arguments`만 쓴다(`CompilationDatabase.cs:13-18`). 모듈 이름은 없다 | 파일을 모듈·프로젝트에 나누려면 계획(`NativePlan.LogicalCompiles`, `ActionGraph.cs:88-89`)을 직접 써야 한다 |
| 정의 | 모듈 정의는 `<out>/Definitions/<M>.Definitions.h`(`.Shared`·`.Extra` 변형)로 강제 include한다(`Pch.cs:30-36`, `ActionGraph.cs:284, 321`). 구성·플랫폼·타깃 매크로만 명령줄 `/D`다 | IntelliSense가 정의 헤더를 봐야 한다. 헤더는 그 구성을 빌드하거나 `--plan-only`를 돌린 출력에만 있다 |
| 컴파일러 판정 | `Base/Compiler.h:15-16`이 `__clang__`이 없으면 `#error`다 | MSVC 모드 IntelliSense(UE 방식)로는 모든 파일에 오류가 뜬다(8.5절) |
| 타깃 | 규칙 어셈블리의 모든 `TargetRules`(`RuleLoader.cs:63-64`). 지금 16개: `Engine`(Game 종류, 라이브러리 묶음)과 Program 15개. `TargetType`은 `Game`·`Program`뿐이다(`TargetType.cs`) | 엔진 프로젝트와 Program 프로젝트를 나누는 기준이다 |
| 구성·플랫폼 | `Debug`·`Development`·`Release`·`Asan`·`Coverage`(`NativeBuildModels.cs:49`), `Windows`·`MacOS`·`Android`(`NativeBuildModels.cs:6`) | 솔루션 구성·플랫폼 표의 원천이다 |
| 게임 SDK 연결 | `.rocksproject`는 `engineVersion`만 있고 모르는 필드를 거부한다(`Descriptors.cs:184-207`, 허용 키 `:189`). SDK 경로는 매번 `--sdk`로 준다 | 프로젝트 파일 생성기가 SDK를 찾을 방법이 없다(10.1절) |
| SDK 구성 | SDK 하나에 구성 하나다(`SdkManifest.Configuration`, `InstalledSdk.cs:33`). `Verify`가 구성이 다르면 거부한다(`InstalledSdk.cs:495-501`) | 게임 솔루션이 Debug·Development·Release를 함께 가질 수 없다(10.3절) |
| 게임 입력 snapshot | 프로젝트 루트 전체를 내용째 해시하고 `.git` 디렉터리, 출력, SDK만 뺀다(`GameInputSnapshot.cs:22, 51-54`). 빌드 시작과 끝을 비교한다(`InstalledSdk.cs:318, 457-458`) | Visual Studio가 `.vs/`의 IntelliSense DB를 쓰기 권한으로 열고 빌드 중에도 고친다. 읽기 실패나 snapshot 불일치로 게임 빌드가 실패한다(10.6절) |
| 게임 출력 | `<project>/out/<Configuration>`(`InstalledSdk.cs:218, 290`). 타깃 이름이 없다 | 게임 타깃이 둘 이상이면(Editor·Server 예정, BuildTargets 3.1절) 출력이 겹친다. 정리 명령의 대상도 정할 수 없다 |
| 정리 | 정리 명령이 없다 | NMake Clean·Rebuild가 부를 것이 없다 |
| 템플릿 | `Templates/Game/Foundation`은 빌드되는 실제 프로젝트다(IntegrationTests가 복사해 빌드한다, `Rocks.Build.IntegrationTests/Program.cs:170`). 이름은 `Game.rocksproject`·`SampleGame` 모듈·`Game` 타깃·`sample` namespace이고 ID는 고정값이다(`SampleGame.rocksids.json`) | 흔한 단어("Game")라 문자열 치환이 `GameObject` 같은 무관한 이름까지 바꾼다. 템플릿으로 만든 프로젝트끼리 타입 ID가 겹친다 |

## 2. 용어 대응

| Rocks | UE 5.8 대응 | 뜻 |
|---|---|---|
| 프로젝트 파일 생성 `projectfiles` | UBT `-ProjectFiles`(`Modes/GenerateProjectFilesMode.cs:18`) | 솔루션·프로젝트 파일을 만드는 CLI 명령 |
| 엔진 모드 / 게임 모드 | 엔진 솔루션 / foreign 프로젝트 솔루션 | `--root`(저장소) / `--project`(게임 `.rocksproject`) |
| 엔진 솔루션 `Rocks.slnx` | `UE5.sln`·`UE5.slnx` | 저장소 루트의 솔루션 |
| 엔진 프로젝트 `Rocks` | `UE5` 프로젝트 | `Core/`·`Engine/` 모듈 파일을 담고 엔진 타깃을 구성으로 갖는 NMake 프로젝트 |
| 프로젝트 파일 디렉터리 `out/ProjectFiles` | `Engine/Intermediate/ProjectFiles`(`ProjectFileGenerator.cs:451`) | 생성한 `.vcxproj`·props·정의 헤더 사본 |
| IDE 빌드 진입점 `Build/IdeBuild.bat` | `Engine/Build/BatchFiles/Build.bat`·`Rebuild.bat`·`Clean.bat` | NMake 명령이 부르는 스크립트 |
| IDE 빌드 잠금 | `-WaitMutex`(`VCProject.cs:2281-2282`) | 솔루션 빌드의 Rocks.Build 호출을 하나씩 돌린다 |
| 파일별 IntelliSense | `bUsePerFileIntellisense`(`VCProjectFileGenerator.cs:84`) | 파일마다 include·정의·강제 include를 단다 |
| 정리 `clean` | `CleanMode`(`Modes/CleanMode.cs:21`) | 타깃·구성·플랫폼 출력 하나를 지운다 |
| SDK 연결 `engineAssociation` | `EngineAssociation` | 게임이 쓸 설치 SDK |
| SDK 등록부 | `HKCU\SOFTWARE\Epic Games\Unreal Engine\Builds`(`DesktopPlatformWindows.cpp:28`) | 이 컴퓨터에 등록한 설치 SDK 목록 |
| 프로젝트 선택기 | UnrealVersionSelector(`-projectfiles`, `UnrealVersionSelector.cpp:392`) | 셸 메뉴에서 `.rocksproject`를 받아 맞는 SDK의 생성기를 부른다 |
| 파일 종류 `Rocks.ProjectFile` | `Unreal.ProjectFile`(`DesktopPlatformWindows.cpp:526-556`) | `.rocksproject`의 셸 연결 |
| 템플릿 정의 `Template.json` | `Config/TemplateDefs.ini` | 무시 목록·치환 대상 확장자 |
| 새 프로젝트 `new-project` | `GameProjectUtils::CreateProjectFromTemplate`(`GameProjectUtils.cpp:1677`) | 템플릿을 복사·치환해 새 게임 프로젝트를 만든다 |
| ID 재발급 `ids reissue` | 새 `ProjectID` GUID(`GameProjectUtils.cpp:1904`) | 템플릿 identity 명세의 ID를 새로 뽑는다 |
| 다중 구성 SDK | 설치 빌드(여러 구성의 엔진 바이너리를 함께 싣는다) | SDK 하나에 여러 구성의 라이브러리를 담는다 |

## 3. 목표와 범위

목표:
- 엔진 저장소를 Visual Studio에서 열어 탐색·IntelliSense·빌드·디버그(F5)를 한다.
- IntelliSense가 실제 빌드와 같은 include·정의·강제 include를 본다. 같은지 Visual Studio 없이 기계적으로 검사한다.
- 생성 결과는 결정적이다. 같은 입력이면 byte가 같고, 바뀐 파일만 다시 쓴다.
- 게임 프로젝트는 SDK 연결만으로 솔루션을 만든다. 엔진 저장소가 없어도 된다(SDK가 `Tools/Rocks.Build.Cli`를 싣는다, `InstalledSdk.cs:174-177`).
- 템플릿으로 새 게임 프로젝트를 만든다.

범위 밖: VS Code·Rider·Xcode 생성(13절), Visual Studio가 직접 컴파일하는 MSBuild 프로젝트(18절), `.sln` 출력, 원격·Linux 디버깅, 에디터(Rocks Studio) 안의 새 프로젝트 UI.

## 4. 측정: 논리 명령과 Visual Studio 속성

2026-11-21 Windows 출력의 `compile_commands.json`을 분류했다(스크립트로 인자 철자별 집계, 모듈은 `output`의 `obj/<모듈>/` 조각으로 판별).

| 출력 | 항목 | 모듈 | 서로 다른 인자 묶음(소스·`/Fo` 제외) |
|---|---|---|---|
| `dotnet-windows-foundation-debug` | 1369 | 38 | 45 |
| `dotnet-windows-commandlet-debug` | 970 | — | — |
| `dotnet-windows-enginetests-release` | 1832 | — | — |
| `dotnet-windows-enginetests-coverage` | 1888 | — | — |
| `ci-foundation-asan`(더 이른 스냅숏) | 1356 | — | — |

Foundation Debug에서 모듈 31개는 모든 파일이 같은 인자다. 나머지 7개(CoreTypeBindings·ConfigTestFixtures·CurvesTestFixtures·FoundationTests·LaunchProbe·LaunchWindowedProbe·LaunchEmbeddedHost)는 묶음이 2개이고, 차이는 강제 include뿐이다. 공유 PCH를 쓰는 파일은 `.Shared.Definitions.h` → `ObjectSharedPch.h` → `.Extra.Definitions.h`, 쓰지 않는 파일은 `<M>.Definitions.h` 하나다. 파일당 `/FI`는 0개(10), 1개(645), 3개(714)다. `/I`는 파일당 평균 약 26개, 명령 평균 길이는 3,296자다. 모든 항목의 `directory`는 저장소 루트다.

경로 인자(`/I`·`/FI`·`/Fo`·소스)를 빼면 다섯 출력 전체에서 나온 인자 철자는 아래가 전부다. 이 표가 8.2절 분류표의 첫 판이다.

| 인자 | 나온 곳 | Visual Studio 쪽 | 근거 |
|---|---|---|---|
| `/I<dir>` | 전부 | `AdditionalIncludeDirectories`(파일별 묶음 속성) | include 탐색 |
| `/DWIN32` `/D_WINDOWS` `/DNOMINMAX` `/DWIN32_LEAN_AND_MEAN` `/DROCKS_PLATFORM_WINDOWS=1` `/DROCKS_BUILD_*=1` `/DROCKS_MONOLITHIC=1` `/DROCKS_LOG_COMPILED_MINIMUM=<n>` `/DNDEBUG` | 전부(구성마다 값이 다름) | `NMakePreprocessorDefinitions`(구성 조건) | 한 출력의 모든 항목에서 같았다 |
| `/FI<out>/Definitions/*.h` | 전부 | `ForcedIncludeFiles`(사본 경로로 바꿈, 8.4절) | 모듈 정의 |
| `/FI<src>/…/CoreSharedPch.h`·`ObjectSharedPch.h` | 전부 | `ForcedIncludeFiles` + `AdditionalOptions`의 `/Yu"<헤더>"` | PCH 경계 힌트(UE `VCProject.cs:1385, 1611`) |
| `/clang:-std=c++23` | C++ 항목 | `/std:c++latest` | IntelliSense는 `/clang:`을 모른다. `Compiler.h:19-20`이 `__cpp_if_consteval`을 요구한다 |
| `/clang:-std=c17` | C 항목 | `/std:c17` | |
| `/TP` `/TC` | 전부 | 버림(확장자로 판정, 다르면 `CompileAs`) | C 소스는 `.c`다 |
| `/MDd` `/MD` | 구성별 | `AdditionalOptions` | `_DEBUG`·`_DLL` 미리 정의 매크로 |
| `/GR-` `/EHs-c-` `/Zc:__cplusplus` `/utf-8` | C++ 항목 | `AdditionalOptions` | `_CPPRTTI`·`__cpp_exceptions`·`__cplusplus` 값, 원본 문자 집합 |
| `/fsanitize=address` | Asan | `AdditionalOptions` | `__SANITIZE_ADDRESS__` |
| `--no-default-config` | 전부 | 버림 | clang 설정 파일 탐색만 바꾼다 |
| `/nologo` `/c` `/W4` `/WX` `-Wunique-object-duplication` `/Z7` | 전부 | 버림 | 진단·출력 형식 |
| `/Od` `/O2` | 구성별 | 버림 | 코드 생성. clang이 정의하는 `__OPTIMIZE__`는 제품 코드가 쓰지 않는다(정책으로 지킨다, 8.2절) |
| `/clang:-ffp-contract=off` `-fno-strict-aliasing` `-fno-strict-overflow` `-flto=thin` `-fprofile-instr-generate` `-fcoverage-mapping` | 구성·모듈별 | 버림 | 코드 생성 |
| `/clang:-fmacro-prefix-map=<root>\=` | 전부 | 버림 | `__FILE__` 철자만 바꾼다 |

결론: 논리 명령은 Visual Studio IntelliSense 속성으로 남김없이 옮겨진다. 모듈 안에서 달라지는 것은 강제 include뿐이므로 파일별 속성은 모듈 × 강제 include 모양의 수(Foundation Debug 45개)로 접힌다. 측정한 출력 하나는 검증 목록 밖의 clang-cl(Swift 도구 모음 안의 것)로 빌드됐다. 그래서 LLVM 설치 위치는 컴파일러 경로에서 짐작하지 않고 검사한다(8.5절).

## 5. 결정 요약

- 출력: 엔진은 루트 `Rocks.slnx` + `out/ProjectFiles/`, 게임은 `<project>/<이름>.slnx` + `<project>/out/ProjectFiles/`. `.slnx`만 쓴다(PJD1–PJD3).
- 배치: 엔진 프로젝트 `Rocks` 하나(엔진 타깃이 구성), Program 타깃마다 프로젝트, C# 도구·규칙 프로젝트는 빌드하지 않는 항목으로 넣는다(PJD4, PJD31).
- 구성: Debug·Development·Release·Asan × Windows(x64)·Android(arm64, NDK가 있을 때)(PJD5).
- IntelliSense: 계획의 논리 명령을 전부 분류해 파일별 속성으로 접는다. 모르는 인자는 생성 오류다. 정의 헤더는 내용 주소 사본, PCH 헤더는 강제 include + `/Yu` 힌트, IntelliSense는 ClangCL 모드다(PJD6–PJD12).
- NMake: `Build/IdeBuild.bat`가 도구를 잠금 아래에서 준비하고 Rocks.Build를 IDE 빌드 잠금 아래에서 부른다. `clean` 명령을 새로 둔다(PJD14–PJD16).
- 재생성: 수동이다. 빌드가 상태 지문과 대조해 낡았으면 경고만 한다(PJD17).
- 게임: 출력에 타깃 이름, snapshot에서 점 디렉터리·`out/`·`Saved/`·최상위 `.slnx` 제외, `engineAssociation`(경로 또는 등록 ID, 없으면 `engineVersion`으로 등록 SDK를 고름), 다중 구성 SDK(PJD18–PJD23).
- 템플릿: 고유 이름 `FoundationTemplate`의 실제 프로젝트, `Template.json`, 두 단계 치환, `ids reissue`, `new-project` 명령(PJD24–PJD26).
- 진입점: 루트 `GenerateProjectFiles.bat`, `Build/*.ps1`, HKCU 셸 메뉴(PJD27–PJD28).

## 6. 엔진 모드 솔루션

### 6.1 명령과 출력 위치

`Rocks.Build.Cli projectfiles --root <저장소>`가 만든다. 같은 명령에 `--project <.rocksproject>`를 주면 게임 모드다(10절). 둘 다 주거나 둘 다 없으면 오류다.

| 파일 | 위치 | 내용 |
|---|---|---|
| 솔루션 | `<root>/Rocks.slnx` | 폴더·프로젝트·구성 대응 |
| 엔진 프로젝트 | `<root>/out/ProjectFiles/Rocks.vcxproj`(+`.filters`) | `Core/`·`Engine/` 모듈 파일 |
| Program 프로젝트 | `<root>/out/ProjectFiles/Programs/<타깃>.vcxproj`(+`.filters`) | 타깃의 모듈 파일(6.2절) |
| 공통 props | `<root>/out/ProjectFiles/Rocks.ProjectFiles.props` | `RocksRoot` 등 경로 기준 속성 |
| 도구 모음 props | `<root>/out/ProjectFiles/Rocks.Toolchain.props` | 이 컴퓨터의 `LLVMInstallDir`·`LLVMToolsVersion`·시스템 `IncludePath`(8.5절) |
| 정의 헤더 사본 | `<root>/out/ProjectFiles/Definitions/<내용 SHA-256 앞 16자>.h` | 8.4절 |
| 상태 지문 | `<root>/out/ProjectFiles/ProjectFiles.state.json` | 9절 |

솔루션은 UE처럼 루트에 둔다(`UE5.sln`·`UE5.slnx`, `VCProjectFileGenerator.cs:741, 769`). 프로젝트는 이미 무시되는 `out/` 아래에 둔다(`.gitignore`의 `/out/`). `.gitignore`에 `/Rocks.slnx`를 더한다. `.vs/`·`*.user`는 이미 무시한다.

솔루션 형식은 `.slnx` 하나다. UE 5.8은 `.sln`과 `.slnx`를 함께 쓰지만(`VCProjectFileGenerator.cs:779-786`) Rocks는 하위 호환을 두지 않는다. 쓰기는 `Microsoft.VisualStudio.SolutionPersistence` 패키지로 한다. UE와 같은 패키지이고(`UnrealBuildTool.csproj:71`, 1.0.52) C# 도구는 패키지를 쓸 수 있다. 기준 Visual Studio는 2026(18.x)이다. 검증한 MSVC STL 14.52가 그 세대이고(`Build/README.md` 요구 사항), 프로젝트의 `MinimumVisualStudioVersion`은 18.0이다.

### 6.2 프로젝트 배치

| 솔루션 폴더 | 프로젝트 | 구성 원천 | 파일 |
|---|---|---|---|
| `Engine` | `Rocks` | `Type != Program`인 타깃(지금 `Engine`) | `ModuleDirectory`가 `Core/` 또는 `Engine/` 아래인 모듈 |
| `Programs` | Program 타깃마다 `<타깃>` | 그 타깃 하나 | 아래 소유 규칙으로 정한 모듈 |
| `Programs/Tools` | `Build/Format/Rocks.Format.slnx`의 C# 도구 프로젝트 | 빌드 안 함(PJD31) | 각 `.csproj` 그대로 |
| `Programs/Rules` | `Build/Rules/Rocks.Engine.Rules.csproj`, `Build/Format/Rocks.Template.Rules.csproj` | 빌드 안 함 | `*.Build.cs`·`*.Target.cs` 편집용 |

UE와 같이 엔진 소스는 프로젝트 하나에 담고(`ProjectFileGenerator.cs:1314`의 `Engine` 폴더), Program은 프로젝트를 따로 둔다(`ProjectFileGenerator.cs:1450`의 `Programs` 폴더). 모듈마다 프로젝트를 두지 않는다(18절).

**파일 소유.** 소스 하나는 프로젝트 하나에만 나온다.
- 타깃의 집 디렉터리는 그 타깃 `Modules[0]`의 `ModuleDirectory`다. `TargetRules`에는 파일 경로가 없다(`RuleLoader.cs:63-64`는 타입만 만든다). 주 모듈 위치가 Program 폴더와 같다.
- 모듈 M의 소유 프로젝트: `ModuleDirectory`가 `Core/`·`Engine/` 아래면 `Rocks`다. 아니면 집 디렉터리가 M 디렉터리의 가장 가까운 조상(같음 포함)인 Program 타깃 중 이름 ordinal 첫 번째다. 둘 다 아니면 `Rocks`다.
- 프로젝트의 파일: 소유 모듈의 `Sources`(손으로 쓴 것)와, 그 모듈 public·private include 디렉터리 및 소스 디렉터리의 헤더(`.h/.hh/.hpp/.hxx/.inl/.inc/.ipp/.tpp`, README 헤더 이름 목록의 확장자에서 `.c` 제외)다. 출력 아래 생성 파일(리플렉션 생성 소스, BuildInfo·모듈 표 TU, 정의 헤더)은 넣지 않는다. UE도 `.gen.cpp`를 넣지 않는다.
- `.filters`는 `ModuleDirectory` 기준 디렉터리 구조를 그대로 옮긴다.

**IntelliSense 원천 타깃.** 프로젝트 P의 구성 C(타깃 T, 플랫폼 X)에서 파일 f(모듈 M)의 설정은 T의 계획이 M에 닿으면 그 계획에서 온다. 닿지 않으면 같은 C·X에서 M에 닿는 타깃 중 이름 ordinal 첫 번째의 계획에서 온다. 어느 타깃도 닿지 않는 모듈의 파일은 `ClInclude`·`None`으로만 넣고 생성기가 한 줄 알린다. 예: `Core/Source/Developer/ReflectionTestFixtures`는 `Engine` 타깃이 닿지 않으므로, `Rocks` 프로젝트의 파일이지만 IntelliSense는 그 모듈에 닿는 첫 타깃의 계획에서 온다.

### 6.3 타깃 × 구성 × 플랫폼

솔루션 구성은 `Debug`·`Development`·`Release`·`Asan`이다. `Coverage`는 `Build/MeasureCoverage.ps1`이 쓰는 측정 구성이라 넣지 않는다. 솔루션 플랫폼은 `Windows`와 `Android`다.

| 솔루션 | 프로젝트 구성 | 근거 |
|---|---|---|
| `<C>`·`Windows` | `<C>|x64` | Visual Studio가 아는 플랫폼은 그 이름을 쓴다(UE `Win64`→`x64`, `VCProject.cs:303, 730`) |
| `<C>`·`Android` | `Android_arm64_<C>|x64` | Visual Studio가 모르는 플랫폼은 구성 이름에 플랫폼·아키텍처를 붙이고 플랫폼은 `x64`로 둔다(`VCProject.cs:736-750`) |
| Game이 아닌 종류의 엔진 타깃(예정: Editor·Client·Server) | `<C>_<종류>|x64`, 솔루션 구성은 `<C> <종류>` | `VCProject.cs:752-755`. 지금은 `Engine` 하나라 해당 없음 |

- Android는 생성할 때 NDK를 찾으면(`NativeToolchains.DiscoverAndroidAsync`, CLI 기본 ABI arm64) 넣고, 못 찾으면 빼고 알린다. UE도 SDK가 설치된 플랫폼만 넣는다(`VCProject.cs:706`). x86_64는 CLI로만 빌드한다.
- macOS는 Visual Studio가 없는 호스트라 넣지 않는다(13절).
- Android 구성은 F5가 없다(`NMakeOutput`이 비어 있다). 기기 실행은 `test --platform android --adb`다.

### 6.4 C# 프로젝트

C# 프로젝트 목록의 단일 출처는 `Build/Format/Rocks.Format.slnx`다. 저장소의 모든 `.cs`가 이 솔루션의 프로젝트에 속하는지 format 검사가 이미 확인한다(`Build/README.md` 별도 회귀 검사). 생성기는 이 파일을 같은 패키지로 읽어 경로를 옮겨 적는다.

솔루션 빌드는 이 프로젝트들을 빌드하지 않는다(slnx `Build Project="false"`). `IdeBuild.ps1`이 잠금 아래에서 `Programs/Rocks.Tools.csproj`를 빌드하므로, Visual Studio가 같은 출력(`out/dotnet/`)을 동시에 빌드하면 파일 잠금 충돌이 난다. UE도 UBT 프로젝트는 설정이 켜졌을 때만 솔루션 빌드에 넣는다(`VCProjectFileGenerator.cs:765`의 `DebugPredicate`).

### 6.5 결정성과 갱신

- 프로젝트 GUID는 이름 기반 UUID v5(SHA-1)다. 이름 공간은 생성기 상수 GUID, 이름은 루트 기준 프로젝트 파일 경로(`/` 구분, 대소문자 그대로)다. UE는 같은 목적으로 MD5 이름 기반 GUID를 만들되 기존 파일의 GUID를 먼저 다시 읽는다(`VCProject.cs:113-129`, `VCProjectFileGenerator.cs:544-575`). Rocks는 다시 읽지 않는다. 같은 경로면 언제나 같은 GUID다.
- 순서: 프로젝트·파일·속성 묶음·구성은 모두 ordinal 정렬이다. 시각·사용자 이름·임시 경로를 쓰지 않는다.
- 경로: 저장소 안 경로는 `$(RocksRoot)` 기준 상대 경로로 쓴다. 이 컴퓨터에만 맞는 값(LLVM·MSVC·Windows SDK·NDK·SDK 위치)은 `Rocks.Toolchain.props` 하나에만 둔다. 그래서 그 파일을 뺀 생성물은 작업 사본 위치와 무관하게 같다(16절 V4).
- 인코딩: UTF-8(BOM 없음), CRLF. 정의 헤더 사본은 빌드가 쓰는 것과 같은 바이트다(LF).
- 갱신: 내용이 같으면 쓰지 않는다(UE `WriteFileIfChanged`, `ProjectFileGenerator.cs:4038`). Visual Studio가 바뀌지 않은 프로젝트를 다시 읽지 않게 하기 위해서다. 생성이 끝나면 `out/ProjectFiles`에서 이번 실행이 만들지 않은 파일(없어진 타깃의 프로젝트, 쓰지 않게 된 정의 헤더 사본)을 지운다. 생성은 `out/ProjectFiles`의 `OutputInvocationLock`을 잡고 한다.

## 7. NMake 명령

### 7.1 명령줄

| 속성 | 엔진 모드 값 |
|---|---|
| `NMakeBuildCommandLine` | `"$(RocksRoot)Build\IdeBuild.bat" -Action Build -Target <T> -Configuration <C> -Platform <X>` |
| `NMakeReBuildCommandLine` | 같은 인자, `-Action Rebuild` |
| `NMakeCleanCommandLine` | 같은 인자, `-Action Clean` |
| `NMakeOutput` | 계획의 실행 파일 출력(`NativePlan.ModuleOutputs[Modules[0]]`). 실행 파일이 아니면 비운다 |
| `LocalDebuggerWorkingDirectory` | `$(RocksRoot)` |
| `NMakeCompile` / `NMakeCompileFileCommandLine` | `… -Action Compile -Files "$(SelectedFiles)"`(PJ7, 7.4절) |

UE도 Build·Rebuild·Clean 배치 파일을 부르고(`VCProject.cs:2233-2235, 2516-2518`) 실행 파일을 `NMakeOutput`에 둔다(`VCProject.cs:2520-2527`). 게임 모드는 10.4절이다.

`IdeBuild.ps1`(엔진 모드)이 하는 일:
1. 도구 준비 잠금(이름 있는 mutex `Local\Rocks.Tools.<루트 경로 SHA-256 앞 16자>`)을 잡고 `dotnet build Programs/Rocks.Tools.csproj --nologo -v:q`를 돈다. `Rocks.proj`의 `BootstrapTools`와 같은 일이다.
2. IDE 빌드 잠금(`Local\Rocks.IdeBuild.<같은 해시>`)을 잡는다(7.2절).
3. `dotnet out/dotnet/bin/Rocks.Build.Cli/Debug/net10.0/Rocks.Build.Cli.dll build --root <root> --target <T> --configuration <C> [--platform android]`을 돈다. Rebuild는 같은 잠금 안에서 `clean` 다음 `build`다. 종료 코드를 그대로 돌려준다.

`IdeBuild.bat`는 `pwsh -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0IdeBuild.ps1" %*`만 하는 얇은 껍데기다. `pwsh`가 없으면 설치 안내를 영어로 내고 1을 돌려준다. 컴파일 진단은 clang-cl의 MSVC 형식(`file(line,col): error`)이라 Visual Studio 오류 목록에 그대로 잡힌다.

### 7.2 직렬화

솔루션 빌드는 여러 NMake 프로젝트를 병렬로 부른다. 지금 CLI는 출력 디렉터리마다 잠금(`OutputInvocationLock`, 최대 30분 대기)만 있고 타깃마다 출력이 다르다. 그래서 타깃 N개가 각자 `--jobs`(기본 `min(8, 코어 수)`, `Program.cs:522-525`)만큼 컴파일러를 띄운다. 또 `BootstrapTools`의 `dotnet build`들이 같은 `out/dotnet/obj`에 동시에 쓴다.

UE는 모든 호출에 `-WaitMutex`를 붙여 UBT를 하나씩 돌린다(`VCProject.cs:2281-2282`, "so that building the whole solution doesn't fail"). Rocks도 IDE에서 부른 빌드는 IDE 빌드 잠금으로 하나씩 돈다. 잠금은 명시 gate다. 시간 제한이나 재시도에 기대지 않는다. 버려진 mutex(`AbandonedMutexException`)는 얻은 것으로 본다. 명령줄 빌드(`Rocks.proj`)는 이 잠금을 쓰지 않는다.

### 7.3 정리와 다시 빌드

CLI에 `clean`을 더한다. 출력 경로 계산은 `build`·`game`과 같은 함수를 쓴다.
- 엔진: `clean --root <root> --target <T> --configuration <C> [--platform …]` → `out/dotnet-<platform>-<target>-<configuration>`(`Program.cs:140-141`).
- 게임: `clean --project <dir> --target <T> --configuration <C>` → `<project>/out/<platform>-<target>-<configuration>`(PJD18).
- 그 출력의 `OutputInvocationLock`을 잡은 뒤 잠금 파일을 뺀 모든 항목을 지운다. 출력이 없으면 할 일이 없는 성공이다.
- 지울 디렉터리는 `<root>/out`(게임은 `<project>/out`)의 엄격한 하위여야 한다. `--output`으로 준 비표준 출력도 이 조건을 만족할 때만 받는다.

UE Clean은 타깃의 중간 산출물과 바이너리를 지운다(`Modes/CleanMode.cs`). Rocks는 타깃·구성·플랫폼마다 출력 디렉터리 하나에 모든 산출물이 있으므로 디렉터리를 비우면 된다. Rebuild는 UE처럼 정리 다음 빌드다(`Rebuild.bat`이 `Build.bat … -Rebuild`를 부른다).

### 7.4 파일 하나 컴파일

Visual Studio의 파일 컴파일(Ctrl+F7)은 `NMakeCompileFileCommandLine`으로 Rocks.Build를 부른다(UE `-Files=$(SelectedFiles)`, `VCProject.cs:2562`). `build --files <;로 나눈 경로>`는 계획에서 그 원본 소스의 compile action과 필요한 PCH action만 돌린다. 유니티 묶음 소속 소스는 작업 파일 분리와 같은 경로로 단독 번역 단위로 뺀다. 헤더를 고르면 오류로 알린다(헤더 단독 컴파일은 `PublicHeaderChecks`가 맡는다). 단계 PJ7이다.

## 8. IntelliSense

### 8.1 원천

IntelliSense는 빌드 계획에서 만든다. `compile_commands.json` 파일을 다시 읽지 않는다.
- 생성기는 `build`와 같은 규칙 평가·`MetadataLayout.Apply`·`ActionGraph` 계획을 프로세스 안에서 돌린다. 메타 생성(MetaCompiler 실행)과 action 실행은 하지 않고, 빌드 출력 디렉터리에는 아무것도 쓰지 않는다.
- 계획 요청은 `Unity = false`, `Adaptive = false`(git을 부르지 않는다), `Pch = true`다. 논리 명령은 유니티와 무관하고(`Programs/Rocks.Build/README.ko.md` 유니티 빌드 절), PCH를 켜야 PCH 헤더가 강제 include 목록에 나온다.
- 쓰는 것: `NativePlan.LogicalCompiles`(모듈·소스·인자·강제 include, `ActionGraph.cs:88-89`), 계획의 생성 파일 목록(정의 헤더 경로와 내용, `PlannedFile`), `ModuleOutputs`, 도구 모음.
- `compile_commands.json` 파일을 쓰지 않는 이유: 모듈 이름이 없고(`CompilationDatabase.cs:13-18`), 빌드한 구성 하나만 있고, 그 구성을 빌드하기 전에는 없다.

계획은 타깃 × 구성 × 플랫폼마다 한 번이다(지금 엔진 모드 16 × 4 × 최대 2 = 128회). 규칙 어셈블리는 한 번만 올라가고 `LoadAll`이 문맥마다 `Configure`를 다시 부른다(`RuleLoader.cs:47-56`). 도구 모음 탐색은 플랫폼마다 한 번이다. 계획끼리는 독립이라 병렬로 돈다. 소요 시간은 PJ1에서 재서 기록한다. 목표는 엔진 모드 전체 10초 이하이고, 넘으면 원인을 측정으로 찾는다.

### 8.2 인자 분류표

`IntelliSenseArguments.Classify`가 논리 명령의 인자를 하나씩 분류한다.

| 분류 | 처리 |
|---|---|
| 경로(`/I`·`-I`·`/FI`·`-include`) | 파일별 속성으로 모은다 |
| 정의(`/D`·`-D`) | 같은 구성의 모든 항목에서 같으면 `NMakePreprocessorDefinitions`, 다르면 파일별 `PreprocessorDefinitions` |
| 옮김(표의 `/std:`·`/MD`·`/GR-` 등) | 대응 철자로 `AdditionalOptions`에 넣는다 |
| 버림 | 표에 근거와 함께 적은 인자만 버린다 |

표에 없는 인자는 **생성 오류**다(영어 문구, 인자와 모듈 이름을 적는다). 빌드 쪽에 새 인자가 생기면 IntelliSense에 의미가 있는지 사람이 정해 표에 넣는다. 조용히 버리면 빌드와 IntelliSense가 모르게 갈라진다. 4절 표가 첫 판이고, Android(clang++ GNU 철자 `-std=c++23`·`-include` 등)는 PJ0에서 같은 방법으로 재서 더한다.

버린 인자가 IntelliSense가 보는 매크로를 바꾸지 않는다는 전제는 정책으로 지킨다. `/O2`가 정의하는 clang의 `__OPTIMIZE__`처럼 최적화 인자가 만드는 미리 정의 매크로(`__OPTIMIZE__`·`__OPTIMIZE_SIZE__`·`__NO_INLINE__`)를 제품 소스에서 쓰면 정책 검사 `ide-invisible-macro`가 거부한다. 지금 저장소에 쓰는 곳이 없는지는 PJ0에서 확인한다.

### 8.3 파일별 속성

UE처럼 늘 파일별이다(`bUsePerFileIntellisense = true`가 기본, `VCProjectFileGenerator.cs:84`).
- 파일 f의 IntelliSense 묶음은 (include 목록, 파일별 정의, 강제 include 목록, 옮긴 옵션)이다. 같은 묶음의 파일은 속성 하나를 같이 쓴다. 묶음 번호는 (소유 모듈, 강제 include 목록의 모양)으로 정하고 구성과 무관하다. 값은 구성마다 다르다.
- 항목(`ClCompile Include=…`)에는 조건 없이 `$(RocksIncludes_<n>)`·`$(RocksForcedIncludes_<n>)`·`$(RocksOptions_<n>)`을 적는다. 속성 값은 구성 조건(`'$(Configuration)|$(Platform)'=='…'`)이 붙은 `PropertyGroup`에 적는다. 구성마다 파일 목록을 되풀이하지 않는다.
- 프로젝트에서 가장 많은 파일이 쓰는 묶음은 프로젝트 기본(`NMakeIncludeSearchPath`·`NMakeForcedIncludes`·`AdditionalOptions`)으로 올리고 그 파일에는 항목 메타데이터를 달지 않는다. UE도 공통 값을 접는다(`VCProject.cs:1366-1425, 1520-1625`).
- 헤더(`ClInclude`)에는 설정을 달지 않는다. Visual Studio가 헤더를 include하는 번역 단위의 설정을 빌려 쓴다.

Foundation Debug 기준 묶음은 45개, 파일은 1,369개다(4절). 구성 4개 × 플랫폼 2개여도 속성 정의는 `묶음 수 × 8`개이고 파일 항목은 한 번이다.

### 8.4 정의 헤더와 PCH

**정의 헤더.** 논리 명령의 강제 include 중 `<out>/Definitions/` 아래 경로는 같은 내용의 사본 `out/ProjectFiles/Definitions/<내용 SHA-256 앞 16자>.h`로 바꾼다. 내용은 계획의 `PlannedFile.Content`(`ModuleDefinitions.Content`, `Pch.cs:73`)이므로 빌드가 쓰는 파일과 byte가 같다. 순서도 논리 명령 그대로다([정의 헤더] → [PCH 헤더] → [추가 정의 헤더], UnityBuildPch UD16). 내용 주소라 구성·타깃이 달라도 같은 내용은 파일 하나다.
- 빌드 출력의 정의 헤더를 바로 가리키지 않는 이유: 그 구성을 빌드(또는 `--plan-only`)하기 전에는 없고, 쓰려면 그 출력의 잠금이 필요하다.
- `/D` 목록(`PreprocessorDefinitions`)으로 풀지 않는 이유: API 매크로 값이 비었거나(`#define ROCKS_CORE_API `) `__declspec(dllexport)`·`__attribute__((visibility("default")))`처럼 괄호·따옴표를 담는다. MSBuild 항목 구분자(`;`)와 이스케이프 규칙을 따로 다뤄야 하고, 강제 include 순서의 의미도 빌드와 달라진다.

**PCH.** 논리 명령은 PCH 바이너리 대신 PCH 헤더를 강제 include한다(UnityBuildPch UD15). 그대로 `ForcedIncludeFiles`에 두고, PCH 헤더를 쓰는 묶음은 `AdditionalOptions`에 `/Yu"<PCH 헤더 경로>"`를 더한다. IntelliSense가 그 헤더까지를 자체 PCH(iPCH) 경계로 쓴다. UE와 같다(`VCProject.cs:1385, 1611`).

### 8.5 clang-cl 모드와 도구 모음 props

UE는 clang으로 빌드할 때도 IntelliSense를 MSVC 컴파일러 기준으로 만든다(`GetCompilerForIntellisense`, `VCProjectFileGenerator.cs:259-266`, 도구 모음 `v14x`). Rocks는 따르지 않는다. `Base/Compiler.h:15-16`이 `__clang__`이 없으면 `#error`이므로 MSVC 모드에서는 모든 파일이 오류다. `__clang__`을 정의에 끼워 넣는 것은 판정을 속이는 것이고, IntelliSense가 MSVC 문법·내장 함수 규칙으로 해석하는 것도 그대로 남는다.

- 프로젝트의 `PlatformToolset`은 `ClangCL`이다. Visual Studio IntelliSense가 clang-cl을 흉내 낸다(`__clang__`·`__clang_major__` 정의, clang 확장 수용).
- `Rocks.Toolchain.props`에 `LLVMInstallDir`(탐색한 clang-cl이 있는 `bin`의 부모)와 `LLVMToolsVersion`(정규화한 `--version`의 주 버전)을 적는다. `LLVMInstallDir/lib/clang/<주 버전>/include`가 없으면 생성 오류다(4절의 비표준 clang-cl 사례).
- 같은 파일의 `IncludePath`에 도구 모음 환경의 `INCLUDE`(VsDevCmd가 만든 MSVC·Windows SDK include, `ROCKS_MSVC_TOOLSET`으로 고른 판)를 그대로 적는다. Visual Studio 기본값을 쓰면 빌드와 다른 STL 판을 볼 수 있다(STL 14.52는 Clang 22 미만을 거부한다). UE도 시스템 include를 명시한다(`DefaultSystemIncludePaths`, `VCProjectFileGenerator.cs:640`).
- `ClangCL` 도구 모음 정의는 Visual Studio 구성 요소 `Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset`이 준다. 빌드는 이 구성 요소를 쓰지 않지만 프로젝트를 읽으려면 있어야 한다. 생성기가 vswhere(`-requires` 그 구성 요소)로 확인하고 없으면 경고한다. 생성은 계속한다.
- 언어 표준은 `AdditionalOptions`의 `/std:c++latest`다. 빌드는 `/clang:-std=c++23`이고 Visual Studio의 `LanguageStandard`에는 C++23 고정 값이 없다. `Compiler.h:19-20`이 요구하는 기능 매크로는 이 설정에서 정의된다.

### 8.6 생성 헤더

리플렉션 생성 헤더(`<out>/Generated/<M>/…/*.generated.h`)는 그 구성의 메타 생성이 돈 뒤에만 있다. 생성기는 include 디렉터리를 계획 그대로 적고 메타 생성을 돌리지 않는다. 그래서 한 번도 빌드하지 않은 구성에서는 `#include "X.generated.h"`가 IntelliSense 오류다. UE도 UHT 출력이 생기기 전에는 같다. 생성기는 쓰려는 구성을 한 번 빌드하라고 알린다(17절 L3).

## 9. 재생성

프로젝트 파일이 낡는 원인은 소스·헤더 추가·삭제, 모듈·타깃 규칙 변경(include·정의·의존), 도구 모음 변경(LLVM·MSVC 판), 타깃 추가, SDK 연결 변경(게임)이다. 헤더 내용 변경은 해당하지 않는다.

- 재생성은 수동이다(UE와 같다). `GenerateProjectFiles.bat`, 셸 메뉴로 돌고, `new-project`·`associate` 끝에서 자동으로 한 번 돈다.
- 생성기는 `ProjectFiles.state.json`에 (타깃, 구성, 플랫폼)마다 지문을 적는다. 지문은 IntelliSense 모델(파일 목록, 묶음 값, `NMakeOutput`)의 정규 텍스트 SHA-256과 `Rocks.Toolchain.props` 내용의 SHA-256이다.
- `build`·`game`은 계획을 만든 뒤 같은 지문을 계산해 상태 파일의 값과 비교한다. 이미 만든 계획에서 뽑으므로 파일을 더 읽지 않는다. 상태 파일이 있고 값이 다르면 한 줄 경고한다: `warning RPF0001: Visual Studio project files are out of date for <target> <configuration> <platform> (<n> files added, <m> removed, settings changed). Run GenerateProjectFiles.bat.` MSBuild 정규 형식이라 Visual Studio 오류 목록에 나온다. 상태 파일이 없으면 아무것도 하지 않는다.
- 빌드 중 자동 재생성은 하지 않는다. 빌드 도중 프로젝트 파일이 바뀌면 Visual Studio가 다시 읽기를 묻고 진행 중인 솔루션 빌드가 흔들린다.
- Visual Studio에서 "새 항목 추가"로 파일을 더하면 Visual Studio가 `.vcxproj`를 직접 고친다. 다음 재생성이 계획 기준으로 덮어쓰고, 파일이 모듈 디렉터리에 있으면 그대로 다시 나온다.

## 10. 게임 모드

### 10.1 SDK 연결

`.rocksproject`에 선택 필드 `engineAssociation`(문자열)을 더한다. `fileVersion`은 1 그대로다. 하위 호환을 두지 않으므로 옛 도구가 새 파일을 거부해도 된다. `DescriptorReader.ReadProject`의 허용 키(`Descriptors.cs:189`)와 `Docs/Guides/RepositoryLayout.ko.md` 기술자 표를 함께 고친다.

해석 순서(`SdkAssociation.Resolve`):
1. 값이 경로(`/`·`\`를 담음)면 `.rocksproject` 디렉터리 기준 경로의 SDK다. 그 디렉터리에 `rocks-sdk.json`이 없으면 오류다.
2. 값이 `{GUID}`면 SDK 등록부(10.2절)의 그 ID다. 없으면 등록된 SDK 목록과 함께 오류다.
3. 필드가 없으면 등록 SDK 중 엔진 주·부가 `engineVersion`과 같은 것을 고른다. 패치가 가장 높은 것 하나다(같은 주·부 패치끼리는 서로 읽는다, `Build/README.md` 엔진 버전 절). 최고 패치가 둘 이상이면 모호하다는 오류와 함께 `associate`를 안내한다. 하나도 없으면 오류다.
4. 명령에 `--sdk`를 주면 위를 건너뛴다(CI·시험).

고른 SDK는 지금처럼 `Verify`(manifest 판, 플랫폼, 컴파일러 ABI, 구성)를 거친다. 생성기는 이 검사를 생성 때 먼저 돌려 빌드 전에 알린다.

UE는 `EngineAssociation`이 비면 상위 디렉터리에서 엔진을 찾고, 경로면 그 엔진, 아니면 등록 식별자다(`DesktopPlatformBase.cpp:454-500`). 설치 빌드는 "5.8" 같은 버전 문자열로 연결한다. Rocks는 버전 문자열 대신 이미 필수인 `engineVersion`을 쓴다. 게임은 언제나 설치 SDK로 빌드하므로(엔진 원본으로 게임을 빌드하는 경로가 없다) 상위 디렉터리 탐색은 두지 않는다. 등록 ID를 프로젝트에 쓰면 컴퓨터마다 달라 공유 저장소에서 깨진다. 그래서 `associate`가 경로·ID를 쓸 때 그 사실을 알린다. UE도 foreign 프로젝트가 아니면 연결을 비워 이식성을 지킨다(`DesktopPlatformBase.cpp:432-447`).

`associate --project <.rocksproject> (--sdk <경로> | --id <ID> | --version)`이 필드를 쓰거나 지우고(`--version`은 필드를 지운다) 프로젝트 파일을 다시 만든다. 기술자의 다른 필드와 순서는 그대로 두고 그 키만 고친다.

### 10.2 SDK 등록부

| 호스트 | 저장소 | 형식 |
|---|---|---|
| Windows | `HKCU\Software\Rocks\Sdks` | 값 이름 = `{GUID}`, 값 = SDK 절대 경로(REG_SZ) |
| macOS·Linux | `~/.config/Rocks/Sdks.json` | `{ "fileVersion": 1, "sdks": [ { "id": "{GUID}", "path": "…" } ] }` |

- 등록: `sdk` 내보내기는 끝에 등록한다(`--register on`이 기본, CI는 `off`). `sdk-register --sdk <경로>`와 `sdk-unregister --sdk <경로>|--id <ID>`를 둔다. ID는 등록 때 새로 만든다(UE `RegisterEngineInstallation`도 `FGuid::NewGuid`, `DesktopPlatformWindows.cpp:313-330`). 같은 경로를 다시 등록하면 기존 ID를 쓴다.
- 읽을 때 `rocks-sdk.json`이 없는 항목은 목록에서 빼고 알린다. 등록부를 고치지는 않는다(읽기가 쓰기를 하지 않는다).
- 관리자 권한이 필요 없는 사용자 범위만 쓴다. 기계 범위(HKLM)는 두지 않는다.
- 접근은 `ISdkRegistry`(Windows 레지스트리·JSON 파일 구현)로 감싼다. 시험은 레지스트리 구현에 임시 하위 키(`HKCU\Software\Rocks\Tests\<GUID>`)를 주고 끝에 지운다(16절 V8).

### 10.3 다중 구성 SDK

게임 솔루션은 Debug·Development·Release를 함께 가져야 한다. 구성마다 CRT(`/MDd`·`/MD`)와 구성 매크로가 달라 엔진 라이브러리를 섞을 수 없다. 그래서 SDK 하나가 여러 구성을 담는다.

- `sdk --configurations Debug,Development,Release`(기본은 이 셋, `Asan`은 지정할 때만)가 구성마다 엔진을 빌드한 뒤 SDK 하나로 조립한다.
- 배치: 구성과 무관한 부분(공개 헤더, `Tools/`, `Licenses/`, `Templates/`, 스크립트)은 한 벌이고, 구성마다 다른 부분(라이브러리, 메타데이터 descriptor·identity 명세 사본, 모듈 칸)은 `Configurations/<구성>/` 아래에 둔다. 한 벌로 둔 파일이 구성마다 다르면 내보내기 오류다(조립 때 내용 해시로 확인한다).
- manifest: `Configuration`(문자열)을 `Configurations`(구성 이름 → 파일 목록·모듈 목록)로 바꾸고 `ManifestVersion`을 13에서 14로 올린다(`InstalledSdk.cs:61`). `Verify`는 요청 구성이 목록에 있는지 본다.
- BuildTargets BD5의 SDK 변형(매크로 조합 × 링크 방식)과는 직교한다. 변형 축이 생기면 `Configurations/<구성>/<변형>/`으로 넓힌다.
- 게임 솔루션의 구성은 연결한 SDK가 가진 구성뿐이다. PJ5 전(구성 하나인 SDK)에도 게임 생성은 그 구성 하나로 동작한다.

UE 설치 빌드도 여러 구성의 엔진 바이너리를 함께 싣는다. 구성마다 SDK를 따로 두고 구성별 연결 표를 두는 안은 버렸다(18절).

### 10.4 게임 솔루션

| 파일 | 위치 |
|---|---|
| 솔루션 | `<project>/<이름>.slnx`(`<이름>` = `.rocksproject` 파일 이름) |
| 게임 프로젝트 | `<project>/out/ProjectFiles/<이름>.vcxproj` |
| Program 프로젝트 | `<project>/out/ProjectFiles/Programs/<타깃>.vcxproj` |
| SDK 프로젝트 | `<project>/out/ProjectFiles/RocksSdk.vcxproj` |
| props·정의 사본·상태 | 엔진 모드와 같은 이름, `<project>/out/ProjectFiles/` 아래 |

- 게임 프로젝트: Program이 아닌 게임 타깃이 구성이다(`Game` 종류는 `<C>`, 예정인 Editor·Server는 `<C> Editor`·`<C> Server`). 파일은 `.rocksproject` 디렉터리 아래 게임 모듈이다(Program 타깃이 소유하는 모듈은 제외, 6.2절 규칙).
- `RocksSdk`: SDK 공개 헤더를 `ClInclude`로만 담는 탐색용 프로젝트다. NMake 명령은 `@rem Nothing to build.`이고 솔루션 빌드에서 뺀다. UE foreign 프로젝트 솔루션도 엔진 프로젝트를 함께 넣는다.
- NMake: `"$(RocksSdkRoot)Build\IdeBuild.bat" -Action Build -Project "$(RocksProjectFile)" -Sdk "$(RocksSdkRoot)" -Target <T> -Configuration <C>`. `RocksSdkRoot`는 `Rocks.Toolchain.props`에 있다. 게임 모드 `IdeBuild.ps1`은 도구를 빌드하지 않고 SDK의 `Tools/Rocks.Build.Cli/Rocks.Build.Cli.dll game …`을 부른다. 연결 SDK가 바뀌면 재생성한다(`associate`가 한다).
- `NMakeOutput`은 게임 출력의 실행 파일, `LocalDebuggerWorkingDirectory`는 프로젝트 디렉터리다.
- IntelliSense 원천은 `game` 명령의 계획이다. `InstalledSdk.BuildGameCoreAsync`(`InstalledSdk.cs:283-450`)를 준비(검증·규칙·기술자·메타 배치·계획)와 실행으로 나눠 생성기가 준비만 부른다.

### 10.5 게임 출력 경로

게임 출력 기본값을 `<project>/out/<platform>-<target>-<configuration>`(소문자, 예: `out/windows-foundationtemplate-debug`)으로 바꾼다. 엔진 출력 이름(`dotnet-<platform>-<target>-<configuration>`, `Program.cs:140-141`)과 같은 순서이고 `dotnet-` 접두만 없다. 타깃이 둘 이상이어도 겹치지 않고 `clean`이 지울 곳이 하나로 정해진다. 템플릿 README와 IntegrationTests의 기대 경로를 같은 변경에서 고친다. 옛 경로는 지원하지 않는다.

### 10.6 입력 snapshot 제외

`GameInputSnapshot.Capture`(`GameInputSnapshot.cs:11-78`)의 열거에서 다음을 뺀다. 지금은 `.git` 디렉터리만 뺀다(`GameInputSnapshot.cs:51-54`).
- 깊이와 무관하게 이름이 `.`으로 시작하는 디렉터리(`.git`·`.vs`·`.vscode`·`.idea` 등). 엔진 include inventory가 이미 같은 규칙이다(`Programs/Rocks.Build/README.ko.md` 입력과 증분 계약).
- 프로젝트 루트 바로 아래의 `out/`(모든 구성·타깃 출력과 `out/ProjectFiles`)과 `Saved/`(실행 중 크래시 수집이 쓴다, `Templates/Game/Foundation/README.ko.md`).
- 프로젝트 루트 바로 아래의 `*.slnx` 파일(생성 솔루션).

이유: Visual Studio는 `.vs/<솔루션>/…/Browse.VC.db` 등을 쓰기 권한으로 열어 둔다. `File.OpenRead`(`GameInputSnapshot.cs:72`)는 읽기 공유만 허용하므로, 다른 핸들이 쓰기 권한을 가진 파일에서 공유 위반 예외가 난다. 열리더라도 IntelliSense가 빌드 중에 DB를 고치면 시작·끝 snapshot이 달라 빌드가 실패한다(`InstalledSdk.cs:457-458`). 다른 구성의 출력 `out/<다른 구성>`도 지금은 snapshot에 들어가므로, 같은 프로젝트의 두 구성을 동시에 빌드하면 서로를 실패시킨다.

한계: 규칙이 점 디렉터리·`Saved/` 안 파일을 읽으면 그 변경을 잡지 못한다. 규칙의 동적 입력은 명시해야 한다는 기존 계약(`Build/README.md` 엔진 버전 절 끝 문단)으로 다룬다. 명시 입력(`explicitInputs`)은 제외 규칙과 무관하게 그대로 본다.

## 11. 템플릿 생성

### 11.1 원칙

- 템플릿은 빌드·format·정책 검사를 통과하는 실제 프로젝트다. 자리 표시 토큰(`%PROJECTNAME%` 등)을 소스에 두지 않는다. 토큰은 C# 규칙 컴파일, clang-format, 리플렉션 생성을 모두 깨뜨린다.
- 템플릿 이름은 다른 이름과 우연히 겹치지 않는 고유 문자열이다. 치환은 그 문자열을 새 이름으로 바꾸는 것뿐이다. UE 템플릿도 `TP_ThirdPerson` 같은 실제 이름을 쓰고, `TemplateDefs.ini`의 `%TEMPLATENAME%`은 정의 파일 안의 변수일 뿐이다(`TemplateProjectDefs.cpp:139-145`).
- `Templates/Game/Foundation`의 이름을 `FoundationTemplate`으로 바꾼다: `FoundationTemplate.rocksproject`, `FoundationTemplate.Build.cs`(모듈 `FoundationTemplate`), `FoundationTemplate.Target.cs`(타깃 `FoundationTemplate`), `FoundationTemplate.rocksids.json`, namespace `foundationtemplate`, 진입점 `ROCKS_IMPLEMENT_MODULE(…, FoundationTemplate)`. `GameObject`처럼 템플릿 이름을 담지 않은 이름은 그대로 남는다(namespace가 프로젝트마다 다르다).
- 같은 변경에서 고칠 곳: `Rocks.proj`의 `GameTarget` 기본값(`Game`), IntegrationTests(`Program.cs:170-197`의 `SampleGame`), `Build/README.md`·템플릿 README, 엔진 버전과 템플릿 `engineVersion` 일치 시험.

### 11.2 템플릿 정의

템플릿 디렉터리에 `Template.json`을 둔다(UE `Config/TemplateDefs.ini` 대응, 영어).

```json
{
  "fileVersion": 1,
  "templateName": "FoundationTemplate",
  "description": "Minimal C++ game that uses Object and Reflection",
  "ignore": ["Template.json", "README.ko.md"],
  "replaceInExtensions": [".cpp", ".h", ".cs", ".json", ".rocksproject"]
}
```

- 모르는 필드는 오류다(기술자 읽기와 같은 엄격함).
- `templateName`은 C++ 식별자이고 대문자로 시작하며 길이 8 이상이다. 템플릿의 파일 이름과 `replaceInExtensions` 파일 내용에 치환되는 세 철자 밖의 대소문자 변형(예: `Foundationtemplate`)이 있으면 템플릿 검사 오류다. 철자가 섞여 일부만 바뀌는 것을 막는다.
- `ignore`는 템플릿 루트 기준 경로다. 템플릿 README는 엔진 저장소용 설명이라 복사하지 않는다.

### 11.3 이름 치환

치환 규칙은 세 개로 고정한다.

| 원래 | 바뀜 |
|---|---|
| `templateName` 그대로(`FoundationTemplate`) | 프로젝트 이름 그대로 |
| 대문자(`FOUNDATIONTEMPLATE`) | 대문자 |
| 소문자(`foundationtemplate`) | 소문자 |

- 모두 대소문자를 구분한다. UE는 원래 철자 규칙을 대소문자 무시로 둔다(`TP_ThirdPerson/Config/TemplateDefs.ini:49, 53`의 `bCaseSensitive=false`). Rocks는 고유 이름이라 무시할 이유가 없고, 무시하면 결과가 입력 철자에 따라 달라진다.
- 두 단계로 바꾼다. 먼저 규칙마다 자리 표시 문자열로 바꾸고, 그다음 자리 표시를 새 이름으로 바꾼다. 앞 규칙의 결과를 뒤 규칙이 다시 바꾸지 않는다. UE와 같다(`GameProjectUtils.cpp:1850-1868`).
- 같은 규칙을 상대 경로의 파일·디렉터리 이름과 `replaceInExtensions` 파일 내용에 적용한다. UE의 `FolderRenames`·`FilenameReplacements`·`ReplacementsInFiles` 세 목록을 하나로 합쳤다.
- 프로젝트 이름 검사: C++ 식별자, 대문자 시작, 3–32자, SDK 모듈 이름·엔진 예약 이름과 대소문자 무시로 달라야 한다(`InputValidation`의 모듈 이름 규칙과 같다, MD17). 대상 디렉터리는 없거나 비어 있어야 한다. UE도 이름의 문자·길이·예약어를 본다(`GameProjectUtils.cpp:633-790`).

### 11.4 기술자와 ID 재발급

- `.rocksproject`는 치환 뒤 `engineVersion`을 쓰는 SDK의 주·부로 맞추고 `engineAssociation`을 두지 않는다(10.1절 3번 해석). UE도 새 프로젝트의 연결을 비운다(`GameProjectUtils.cpp:1936-1962`).
- identity 명세(`<이름>.rocksids.json`)는 MetaCompiler에 새로 두는 `ids reissue --manifest <경로>`로 다시 쓴다. 활성 항목은 새 무작위 128비트 ID를 받고(`ids add`와 같은 생성기), `aliases`는 비우고, 은퇴 항목은 지운다. 새 프로젝트에는 이력이 없기 때문이다. 명세 형식은 MetaCompiler만 다룬다(`StableIdManifest.cs`, 명령 분기 `IdentityCommands.cs:48-179`). 생성기가 형식을 따로 구현하지 않는다(공용 기능 중복 금지).
- 재발급하지 않으면 같은 템플릿에서 만든 프로젝트·플러그인이 같은 타입 ID를 가진다. 한 빌드에 함께 들어가면 `ids verify-set` 규칙(한 ID는 한 명세에만)에 걸린다.

### 11.5 명령

`new-project --template <이름|디렉터리> --name <이름> --destination <부모 디렉터리> [--sdk <경로>] [--projectfiles on|off]`
1. 템플릿을 찾는다. 이름이면 SDK의 `Templates/Game/<이름>`(SDK 내보내기가 `Templates/`를 싣는다), 디렉터리면 그대로다.
2. `<부모>/<이름>/`에 `ignore`를 뺀 파일을 이름 치환하며 복사하고 내용을 치환한다. 중간에 실패하면 만든 디렉터리를 지운다(UE도 만든 파일 목록을 지운다).
3. 기술자를 고치고 `ids reissue`를 돈다.
4. 결과 디렉터리 전체(경로·내용)에 템플릿 이름 세 철자가 남지 않았는지 확인한다. 남으면 오류다.
5. Windows에서 `--projectfiles on`(기본)이면 게임 모드 생성을 돈다.

UE는 에디터 대화 상자에서 만든다. Rocks는 에디터 전에도 쓸 수 있도록 CLI 명령으로 두고, 나중의 에디터 UI도 같은 명령을 부른다.

## 12. 진입 스크립트와 셸 통합

| 파일 | 하는 일 |
|---|---|
| `GenerateProjectFiles.bat`(저장소 루트, 새 파일) | `pwsh … -File Build\GenerateProjectFiles.ps1 %*`. 탐색기에서 실행해도 되도록 실패하면 `pause`(UE `GenerateProjectFiles.bat`과 같다) |
| `Build/GenerateProjectFiles.ps1` | 엔진: 도구 준비 잠금 아래 도구 빌드 → `projectfiles --root`. 게임(`-Project`): SDK 도구로 `projectfiles --project` |
| `Build/IdeBuild.bat`·`.ps1` | NMake 명령(7.1절) |

- 스크립트는 `Build/` 아래 기존 스크립트처럼 영어다(`build-file-english` 정책). 루트의 새 `GenerateProjectFiles.bat`은 정책의 루트 파일 목록(`Docs/CodingStyle.md` `build-file-english`)에 더한다.
- SDK 내보내기는 `Build/GenerateProjectFiles.ps1`·`Build/IdeBuild.*`를 `<sdk>/Build/`에 싣는다. 스크립트는 루트에 `rocks-sdk.json`이 있으면 SDK 모드(도구 빌드 없음)로 돈다.
- `pwsh`(PowerShell 7)가 필요하다. 이미 `Build/*.ps1`의 전제다.

**셸 메뉴(Windows).** `shell-register`(SDK 등록 때 기본으로 함께 돈다, `--shell off`로 끈다)가 `HKCU\Software\Classes`에 쓴다.
- `.rocksproject` → ProgID `Rocks.ProjectFile`.
- `Rocks.ProjectFile\shell\rocksgenproj`(표시 이름 "Generate Visual Studio project files")의 명령은 선택기 `dotnet "<선택기 CLI>" projectfiles --project "%1"`이다.
- 선택기 CLI는 가장 최근에 등록한 SDK의 `Tools/Rocks.Build.Cli`이고, 그 경로를 `HKCU\Software\Rocks\ProjectSelector`에 둔다. 선택기는 연결을 해석하고, 고른 SDK가 자기가 아니면 그 SDK의 CLI로 같은 명령을 넘긴다. 연결 해석 구현은 C# 한 곳에만 있다. 선택기 SDK가 지워졌으면 메뉴가 재등록을 안내한다.

UE는 HKLM에 ProgID를 쓰고 UnrealVersionSelector가 메뉴를 받는다(`DesktopPlatformWindows.cpp:526-556`, `UnrealVersionSelector.cpp:392`). Rocks는 관리자 권한이 필요 없는 HKCU를 쓰고, 별도 선택기 실행 파일 없이 CLI가 그 역할을 한다. "SDK 바꾸기" 메뉴(UE `switchversion`)는 고르는 UI가 필요해서 두지 않고 `associate` CLI로 한다.

## 13. 다른 IDE

- **VS Code·clangd**: 범위 밖이다. 빌드 출력마다 있는 `compile_commands.json`(논리 명령, PCH 헤더·정의 헤더 강제 include 포함)을 clangd에 `--compile-commands-dir=out/<출력>`으로 주면 된다. 이 안내를 `Build/README.md`에 한 줄로 적는다(PJ2). `.vscode/` 생성은 하지 않는다.
- **Rider**: `.slnx`·NMake `.vcxproj`를 읽는다. 따로 만들지 않고 검증도 하지 않는다.
- **Xcode**: 범위 밖이다. macOS 개발 환경을 정할 때 별도 스펙으로 다룬다(UE `XcodeProjectFileGenerator` 대응).

## 14. 결정 사항

접두 `PJD`(결정)·`PJ`(단계)는 다른 문서와 겹치지 않는다(2026-11-21, `Docs`·`Build`·`Programs`의 `*.md`에서 `PJ[0-9]` 검색 0건).

| 번호 | 결정 | 상태 |
|---|---|---|
| PJD1 | 기준 Visual Studio는 2026(18.x). 솔루션은 `.slnx`만 쓰고 `Microsoft.VisualStudio.SolutionPersistence`로 쓴다 | 확정(권장안) |
| PJD2 | 생성은 `Rocks.Build.Cli projectfiles`, `--root`(엔진)·`--project`(게임) 중 하나 | 확정(권장안) |
| PJD3 | 엔진 솔루션은 루트 `Rocks.slnx`, 프로젝트·props·정의 사본·상태는 `out/ProjectFiles/`. `.gitignore`에 `/Rocks.slnx` | 확정(권장안) |
| PJD4 | 엔진 프로젝트 `Rocks` 하나(엔진 타깃이 구성) + Program 타깃마다 프로젝트. 파일 소유와 IntelliSense 원천 타깃은 6.2절 규칙 | 확정(권장안) |
| PJD5 | 구성 Debug·Development·Release·Asan(Coverage 제외), 플랫폼 Windows(x64)·Android(`Android_arm64_<C>`, NDK가 있을 때) | 확정(권장안) |
| PJD6 | IntelliSense 원천은 프로세스 안 계획의 `LogicalCompiles`(메타 생성·출력 쓰기 없음, 유니티·작업 파일 분리 끔) | 확정(권장안) |
| PJD7 | 인자 분류표를 두고 표에 없는 인자는 생성 오류. 버린 최적화 인자의 매크로는 정책 `ide-invisible-macro`로 막는다 | 확정(권장안) |
| PJD8 | 늘 파일별 IntelliSense. 묶음 속성으로 접고 가장 많은 묶음은 프로젝트 기본으로 | 확정(권장안) |
| PJD9 | 정의 헤더는 내용 주소 사본(`out/ProjectFiles/Definitions/`)을 같은 순서로 강제 include | 확정(권장안) |
| PJD10 | PCH 헤더는 강제 include 그대로 + `/Yu"<헤더>"` 힌트 | 확정(권장안) |
| PJD11 | `PlatformToolset=ClangCL`. `LLVMInstallDir`·`LLVMToolsVersion`·시스템 `IncludePath`는 탐색한 도구 모음 값으로 `Rocks.Toolchain.props`에. 구성 요소가 없으면 경고 | 확정(권장안) |
| PJD12 | 저장소 경로는 `$(RocksRoot)` 기준, 컴퓨터 값은 `Rocks.Toolchain.props`에만 | 확정(권장안) |
| PJD13 | 프로젝트 GUID는 경로 이름 기반 UUID v5. 정렬·인코딩(UTF-8, CRLF) 고정, 바뀐 파일만 쓰고 이번 실행이 만들지 않은 파일은 지움 | 확정(권장안) |
| PJD14 | NMake Build·Rebuild·Clean은 `Build/IdeBuild.bat`(→ `IdeBuild.ps1`). `NMakeOutput`은 계획의 실행 파일 | 확정(권장안) |
| PJD15 | IDE 빌드는 루트마다 이름 있는 mutex로 하나씩, 도구 준비는 별도 mutex | 확정(권장안) |
| PJD16 | CLI `clean`: 출력 디렉터리 하나를 잠금 아래에서 비운다. Rebuild = clean + build | 확정(권장안) |
| PJD17 | 재생성은 수동. 상태 지문을 두고 `build`·`game`이 낡음을 `warning RPF0001`로 알린다. 자동 재생성 없음 | 확정(권장안) |
| PJD18 | 게임 출력 기본 `<project>/out/<platform>-<target>-<configuration>` | 확정(권장안) |
| PJD19 | 게임 snapshot은 점 디렉터리(모든 깊이)와 최상위 `out/`·`Saved/`·`*.slnx`를 뺀다 | 확정(권장안) |
| PJD20 | `.rocksproject` 선택 필드 `engineAssociation`(경로 또는 등록 ID). 없으면 `engineVersion`으로 등록 SDK 중 최고 패치, 동률은 오류 | 확정(권장안) |
| PJD21 | SDK 등록부: Windows `HKCU\Software\Rocks\Sdks`, 그 밖 `~/.config/Rocks/Sdks.json`. `sdk`가 기본으로 등록, `sdk-register`·`sdk-unregister`·`associate` 명령 | 확정(권장안) |
| PJD22 | 다중 구성 SDK(기본 Debug·Development·Release), 구성별 부분은 `Configurations/<구성>/`, manifest 14 | 확정(권장안) |
| PJD23 | 게임 솔루션은 게임 프로젝트(게임 타깃이 구성) + Program 프로젝트 + 빌드하지 않는 `RocksSdk` 헤더 프로젝트 | 확정(권장안) |
| PJD24 | 템플릿은 고유 이름(`FoundationTemplate`)의 실제 프로젝트. 토큰 없음 | 확정(권장안) |
| PJD25 | `Template.json`. 치환은 세 철자·대소문자 구분·두 단계, 경로와 내용에 같은 규칙, 남은 이름 검사 | 확정(권장안) |
| PJD26 | `new-project` CLI, 기술자 재작성, MetaCompiler `ids reissue`(새 ID, 별칭 비움, 은퇴 항목 삭제). SDK가 `Templates/`를 싣는다 | 확정(권장안) |
| PJD27 | 루트 `GenerateProjectFiles.bat` + `Build/GenerateProjectFiles.ps1`·`Build/IdeBuild.*`(pwsh 7). SDK에도 싣는다 | 확정(권장안) |
| PJD28 | 셸 메뉴는 HKCU ProgID `Rocks.ProjectFile`. 선택기는 최근 등록 SDK의 CLI가 맡고 맞는 SDK로 넘긴다 | 확정(권장안) |
| PJD29 | 파일 하나 컴파일은 `build --files`(PJ7) | 확정(권장안) |
| PJD30 | VS Code·Rider·Xcode 생성은 범위 밖. clangd는 `compile_commands.json` 안내만 | 확정(권장안) |
| PJD31 | C# 프로젝트는 `Rocks.Format.slnx` 목록을 그대로 넣고 솔루션 빌드에서 뺀다 | 확정(권장안) |

## 15. 단계

각 단계는 Debug 빌드, format, policy, 바뀐 부분의 시험으로 검증한다. 수동 Visual Studio 확인은 PJ2와 PJ4 끝에 한 번씩 한다(16절 V11).

| 단계 | 내용 | 완료 기준 |
|---|---|---|
| PJ0 | 인자 분류표(`IntelliSenseArguments`), IntelliSense 모델(파일 소유·원천 타깃·묶음 접기·정의 사본), Android 철자 측정, 정책 `ide-invisible-macro` | 엔진 모든 타깃 × 구성 × 플랫폼 계획에서 미분류 인자 0, 모델을 다시 펼친 명령이 논리 명령과 같다(V1) |
| PJ1 | 엔진 모드 쓰기: `.vcxproj`·`.filters`·`Rocks.slnx`·props·정의 사본·상태, GUID, 바뀐 파일만 쓰기·정리, C# 프로젝트, 생성 시간 측정 | V2–V5 통과, 소요 시간 기록 |
| PJ2 | NMake: `Build/IdeBuild.*`, 두 mutex, CLI `clean`, Rebuild, 루트 `GenerateProjectFiles.bat`·`Build/GenerateProjectFiles.ps1`, 낡음 경고(`RPF0001`), `.gitignore`, README 안내(clangd 포함) | V6·V7 통과, 수동 확인(엔진) |
| PJ3 | 게임 선행: 출력 경로(10.5절), snapshot 제외(10.6절), `engineAssociation`·`SdkAssociation`, 등록부와 `sdk-register`·`sdk-unregister`·`associate`, SDK가 스크립트를 싣는다 | V8·V9 통과 |
| PJ4 | 게임 모드 생성: `BuildGameCoreAsync` 준비·실행 분리, 게임·Program·`RocksSdk` 프로젝트, 게임 NMake | 게임 fixture로 V2–V6 통과, 수동 확인(게임) |
| PJ5 | 다중 구성 SDK(10.3절), manifest 14, `Verify` 구성 목록 | V10 통과 |
| PJ6 | 템플릿: `FoundationTemplate` 개명과 영향 파일, `Template.json`, `new-project`, `ids reissue`, SDK가 `Templates/`를 싣는다 | V12 통과 |
| PJ7 | 파일 하나 컴파일(`build --files`, `NMakeCompileFileCommandLine`) | 지정 소스만 컴파일하는 action 목록 시험, 유니티 소속 소스 분리 시험 |
| PJ8 | 셸 통합(`shell-register`, 선택기 위임) | 임시 하위 키에 쓴 값의 시험(V8과 같은 방식), 수동 확인 1회 |

순서는 PJ0 → PJ8이다. PJ5는 PJ4와 독립이라 앞당길 수 있다. PJ4는 PJ5 전에는 SDK가 가진 구성 하나로 만든다.

## 16. 검증

Visual Studio 없이 결정적으로 검사한다. 시험은 `Rocks.Build.Tests`(fixture 규칙과 수동 도구 모음, 컴파일러·Visual Studio 불필요)와 `Rocks.ToolTests`·IntegrationTests(실제 도구 모음, Visual Studio IDE 불필요)에 나눈다.

| 번호 | 검사 | 방법 |
|---|---|---|
| V1 | 분류 완전성·왕복 | 엔진 모든 타깃 × 구성 × 플랫폼의 계획(실제 도구 모음, CI Windows)에서 모든 논리 명령 인자가 분류된다. 모델에서 파일마다 (include, 정의, 강제 include, 옵션)을 다시 펼친 것이 논리 명령의 해당 부분과 순서까지 같다. 버린 인자는 표의 버림 목록 안에 있다 |
| V2 | MSBuild 평가 | `Microsoft.Build`(+ `Microsoft.Build.Locator`로 .NET SDK의 MSBuild)로 생성 `.vcxproj`를 구성 × 플랫폼마다 평가한다(`ProjectLoadSettings.IgnoreMissingImports`라 Visual Studio의 `Microsoft.Cpp.*.props`가 없어도 된다). 평가한 `ClCompile` 항목의 `AdditionalIncludeDirectories`·`ForcedIncludeFiles`·`PreprocessorDefinitions`·`AdditionalOptions`와 NMake 속성을 모델과 비교한다. 모든 항목 파일이 존재한다 |
| V3 | 솔루션 왕복 | `.slnx`를 같은 패키지로 다시 읽어, 모든 프로젝트 경로가 있고 모든 솔루션 구성 × 플랫폼이 각 프로젝트의 존재하는 구성에 대응되며 C# 프로젝트의 빌드 표시가 꺼져 있음을 확인한다 |
| V4 | 결정성 | 같은 입력으로 두 번 생성하면 모든 파일 byte가 같다. 두 번째 실행은 아무 파일도 쓰지 않는다(파일 ID·수정 시각 불변). 다른 루트 경로(작업 사본 둘)에서 생성하면 `Rocks.Toolchain.props`를 뺀 모든 파일이 같다 |
| V5 | golden | fixture 규칙(실행 파일 1, 라이브러리 2, 공유 PCH 1, C 소스 1, 생성 소스 1)의 생성물을 `Programs/Rocks.Build.Tests/ProjectFiles/Golden/`과 byte 비교한다 |
| V6 | NMake 실행 | IntegrationTests가 V2로 평가한 `NMakeBuildCommandLine`·`NMakeCleanCommandLine`·`NMakeReBuildCommandLine` 문자열을 `cmd.exe /c`로 fixture 타깃에 실행한다. 빌드하면 `NMakeOutput`의 실행 파일이 생기고, Clean하면 출력에 잠금 파일만 남고, Rebuild하면 다시 생긴다 |
| V7 | 직렬화 | 시험이 IDE 빌드 잠금을 먼저 쥔 상태에서 `IdeBuild.ps1`을 띄우고, 잠금을 놓기 전까지 CLI가 시작하지 않음을 시험용 표지 파일로 확인한다. 순서는 잠금과 신호로만 만들고 시간 대기는 쓰지 않는다 |
| V8 | 연결 해석 | `SdkAssociation.Resolve`를 메모리 등록부로 시험한다: 경로, ID, 버전 최고 패치, 동률 오류, 없음 오류, 사라진 SDK 제외. 레지스트리 구현은 임시 하위 키에서 쓰기·읽기·지우기를 시험한다 |
| V9 | snapshot 제외 | 게임 fixture에서 `.vs/x.db`를 쓰기 권한·읽기 공유 없음으로 연 채 게임 빌드가 성공한다. 빌드 중간 지점(빌드 엔진 시험 hook)에서 `.vs`·`Saved/`·`out/<다른 구성>`·최상위 `.slnx`를 고쳐도 성공하고, `Source/`를 고치면 지금처럼 실패한다 |
| V10 | 다중 구성 SDK | fixture SDK를 두 구성으로 내보내 두 구성의 게임 빌드가 성공하고, 없는 구성은 `Verify`가 거부한다. 한 벌 부분이 구성마다 다르면 내보내기 오류다 |
| V11 | 수동(Visual Studio 2026) | 솔루션 열기, 지정 파일 목록(`Core`·`ObjectRuntime`·`FoundationTests` 각 3개)의 IntelliSense 오류 0, `Sandbox` F5, Clean·Rebuild, Android 구성 빌드, PJ7 뒤 Ctrl+F7. 결과를 개정 이력에 적는다 |
| V12 | 템플릿 | `new-project`로 만든 프로젝트를 fixture SDK로 빌드·실행한다(IntegrationTests). 경로·내용에 템플릿 이름 세 철자가 0건이고, 모든 ID가 템플릿과 다르고 서로 다르며, `ids verify`를 통과하고, 두 번 만든 프로젝트끼리 ID가 다르다. 대상 디렉터리가 비어 있지 않으면 아무것도 쓰지 않는다 |

## 17. 위험과 한계

| 번호 | 내용 | 대응 |
|---|---|---|
| L1 | ClangCL 모드 IntelliSense(EDG의 clang 흉내)가 C++23 기능에서 거짓 오류를 낼 수 있다. 이 컴퓨터는 vswhere에 Visual Studio 2019 16.11(도구 모음 v142)만 보이고 ClangCL 도구 모음이 없어 실측하지 못했다 | V11 수동 확인에서 오류 목록을 기록한다. 원인이 IntelliSense면 코드가 아니라 기록으로 다룬다. 기준은 빌드다 |
| L2 | `ClangCL` 도구 모음 구성 요소가 없으면 프로젝트가 열리지 않거나 IntelliSense가 꺼진다 | 생성기 경고(8.5절), README 요구 사항에 구성 요소 이름 |
| L3 | 빌드하지 않은 구성은 리플렉션 생성 헤더가 없어 IntelliSense 오류가 난다 | 생성기 안내. 메타 생성만 도는 옵션은 필요해지면 다음 개정에서 |
| L4 | 큰 프로젝트(구성당 파일 1,400–1,900개)의 IntelliSense 메모리 | 묶음 속성 접기(8.3절). UE도 같은 이유로 속성을 접는다(`VCProject.cs:1419-1422`) |
| L5 | 루트에 `Rocks.proj`와 `Rocks.slnx`가 함께 있으면, 프로젝트를 적지 않은 `dotnet msbuild`·`dotnet build`가 MSB1011(프로젝트 여럿)로 멈춘다 | 모든 문서 명령은 `Rocks.proj`를 적는다. README에 한 줄로 적는다 |
| L6 | 재생성 전까지 새 파일이 프로젝트에 없다 | 낡음 경고(9절) |
| L7 | 다중 구성 SDK는 크기와 내보내기 시간이 구성 수만큼 는다 | 기본 세 구성, Asan은 지정할 때만 |
| L8 | HKCU 등록부·셸 메뉴는 컴퓨터 상태를 바꾼다. SDK를 지워도 항목이 남는다 | 읽을 때 사라진 SDK를 빼고 알린다. `sdk-unregister`. CI는 `--register off` |
| L9 | 템플릿 개명이 IntegrationTests·문서·`Rocks.proj` 기본값에 닿는다 | PJ6 한 변경에서 함께 고친다 |
| L10 | 엔진 모드 계획 128회의 시간을 아직 재지 않았다 | PJ1 측정, 병렬 계획 |
| L11 | NMake 빌드가 `pwsh`에 의존한다 | `IdeBuild.bat`가 없으면 안내하고 실패 |
| L12 | Android 구성의 IntelliSense는 clang-cl x64 흉내라 NDK clang과 미리 정의 매크로가 다르다(대상 triple에서 오는 매크로) | 플랫폼 분기 코드를 읽는 용도로 충분하다. 기준은 빌드다 |
| L13 | CLI 자체 오류는 MSBuild 정규 형식이 아니라 출력 창에만 나온다 | `RPF0001` 외에는 후속 |
| L14 | snapshot 제외 영역을 규칙이 읽으면 변경을 놓친다 | 동적 입력 명시 계약 |
| L15 | 선택기 SDK가 지워지면 셸 메뉴가 실패한다 | 재등록 안내 |

## 18. 버린 대안

| 대안 | 버린 이유 |
|---|---|
| CMake로 Visual Studio 프로젝트 생성 | Rocks.Build는 CMake·Python을 부르지 않는다(`Programs/Rocks.Build/README.ko.md` 첫 문단). 빌드 계획이 두 벌이 된다 |
| Visual Studio가 직접 컴파일하는 MSBuild `ClCompile` 프로젝트 | 증분·PCH·유니티·BuildInfo·리플렉션 생성을 MSBuild로 다시 구현해야 한다. UE도 NMake다 |
| Open Folder(`CppProperties.json`) | 구성·디버그 대상·파일별 설정이 약하다 |
| `.sln` 함께 쓰기 | 하위 호환을 두지 않는다. 기준 Visual Studio가 `.slnx`를 읽는다 |
| IntelliSense MSVC 모드 + `__clang__` 정의 주입(UE 방식) | `Compiler.h`의 판정을 속이고 MSVC 문법으로 해석한다 |
| 정의를 `/D`(`PreprocessorDefinitions`)로 풀기 | 값의 괄호·따옴표·빈 값과 MSBuild 이스케이프, 강제 include 순서 의미 차이(8.4절) |
| 빌드 출력의 정의 헤더를 바로 강제 include | 빌드 전에는 없고 출력 잠금이 필요하다 |
| `compile_commands.json` 파일을 읽어 생성 | 모듈 정보가 없고 빌드한 구성 하나뿐이다 |
| 모르는 인자를 조용히 버림 | 빌드와 IntelliSense가 모르게 갈라진다 |
| 모듈마다 프로젝트 | 프로젝트 수백 개, 솔루션 열기 비용. UE도 엔진 프로젝트 하나다 |
| 빌드 중 자동 재생성 | 진행 중 솔루션 빌드에서 프로젝트 다시 읽기가 일어난다 |
| 무작위 GUID를 만들고 기존 파일에서 다시 읽기(UE) | 파일이 없어지면 바뀐다. 이름 기반이면 언제나 같다 |
| 템플릿 토큰(`%PROJECTNAME%`) | 템플릿이 빌드·format·리플렉션 검사를 통과하지 못한다 |
| 대소문자 무시 치환 | 고유 이름이면 불필요하고 결과가 입력 철자에 달린다 |
| 구성마다 SDK를 따로 두고 연결 표에 구성별 경로 | 등록·연결·검증이 구성 수만큼 늘고 공통 부분이 중복된다 |
| 연결을 환경 변수(`ROCKS_SDK`)로 | 셸·IDE·탐색기마다 환경이 달라 결정적이지 않다 |
| 상위 디렉터리에서 엔진 찾기(UE) | Rocks 게임은 엔진 원본으로 빌드하지 않는다 |
| HKLM 등록(UE ProgID) | 관리자 권한이 필요하다 |
| 별도 선택기 실행 파일(UE UnrealVersionSelector) | 연결 해석 구현이 둘이 된다. CLI가 같은 일을 한다 |
| C# 프로젝트를 솔루션 빌드에 포함 | `IdeBuild.ps1`의 도구 빌드와 같은 출력을 두고 경합한다 |
| 솔루션을 `out/ProjectFiles` 안에 | 찾기 어렵다. UE도 루트에 둔다 |

## 19. UE와 다르게 정한 점

| 항목 | UE 5.8 | Rocks | 이유 |
|---|---|---|---|
| 솔루션 형식 | `.sln` + `.slnx` | `.slnx`만 | 하위 호환 없음 |
| IntelliSense 컴파일러 | MSVC 기준(`GetCompilerForIntellisense`) | ClangCL 모드 | `Compiler.h`가 clang만 받는다 |
| 정의 전달 | 프로젝트 정의 목록 | 정의 헤더 내용 주소 사본 강제 include | 빌드와 같은 파일·순서 |
| 미분류 인자 | 해당 개념 없음 | 생성 오류 | 빌드와 IntelliSense의 조용한 차이 방지 |
| 프로젝트 GUID | MD5 이름 기반 + 기존 파일 재사용 | UUID v5 이름 기반만 | 결정성 |
| 연결이 없을 때 | 상위 디렉터리의 엔진 | `engineVersion`으로 등록 SDK | 게임은 늘 SDK로 빌드 |
| 등록 범위 | HKCU(소스 빌드)·HKLM(ProgID) | HKCU만 | 관리자 권한 불필요 |
| 템플릿 치환 | 목록 셋, 원래 철자는 대소문자 무시 | 규칙 셋 고정, 경로·내용 공통, 대소문자 구분, 남은 이름 검사 | 고유 이름 전제에서 단순·결정적 |
| 새 프로젝트 | 에디터 대화 상자 | CLI `new-project` | 에디터 전에도 쓴다 |
| 타입 ID | 해당 없음(ProjectID만 새로) | identity 명세 재발급 | Rocks 리플렉션 ID는 전역 정체성 |
| 솔루션 빌드 직렬화 | `-WaitMutex` | IDE 빌드 mutex + 도구 준비 mutex | 도구 빌드 경합까지 막는다 |

## 20. 다른 문서 개정

- `Build/README.md`: 이 스펙 링크를 더했다(이번 개정). PJ2에서 Visual Studio 절(생성·요구 구성 요소·clangd), PJ3에서 게임 출력 경로·SDK 등록, PJ5에서 SDK 구성을 고친다.
- `Programs/Rocks.Build/README.ko.md`: PJ1(생성기), PJ2(`clean`), PJ7(`--files`)을 구현할 때 갱신한다.
- `Docs/Guides/RepositoryLayout.ko.md` 기술자 표: `engineAssociation`(PJ3).
- [빌드 타깃](BuildTargets.ko.md) BD5: SDK 변형과 구성 축의 관계(10.3절)를 PJ5에서 적는다.
- `Docs/Implementation/ReflectionMetadata.ko.md`: `ids reissue`(PJ6).
- `Docs/CodingStyle.md`: `build-file-english`에 루트 `GenerateProjectFiles.bat`(PJ2), 정책 `ide-invisible-macro`(PJ0).
- `Templates/Game/Foundation/README.ko.md`: 출력 경로(PJ3), 개명·`new-project`(PJ6).

## 21. 개정 이력

| 개정 | 날짜 | 내용 |
|---|---|---|
| 1 | 2026-11-21 | 첫 판. 2026-11-14 검토(엔진 `--root` 먼저, 계획 기반 파일별 IntelliSense, NMake, 게임 쪽 결정 넷, UE식 템플릿)를 스펙으로 옮겼다. `compile_commands.json` 다섯 출력의 인자를 분류해 Visual Studio 속성 대응을 측정했다(4절). UE 근거는 5.8.4 UBT `ProjectFiles/VisualStudio`·`ProjectFileGenerator`·`GenerateProjectFilesMode`·`CleanMode`, 배치 파일, UnrealVersionSelector·DesktopPlatform, GameProjectGeneration·TemplateDefs. 결정 PJD1–PJD31은 권장안으로 확정했다 |
