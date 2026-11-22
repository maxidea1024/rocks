# Visual Studio 프로젝트 파일 생성 설계

기준일: 2026-11-22, 개정 22. Rocks 저장소와 게임 프로젝트에서 Visual Studio 솔루션·프로젝트 파일을 만드는 방법을 정한다(UE 5.8 GenerateProjectFiles 대응).
- **엔진 모드**: 저장소 루트에서 `projectfiles --root`로 엔진·Programs·C# 도구 솔루션을 만든다. 엔진에는 `.rocksproject`가 없다.
- **IntelliSense**: 부작용 없는 IDE 계획 모드로 빌드 계획의 논리 compile 명령(`compile_commands.json`과 같은 원천)을 만들고, 파일별 설정으로 옮긴다.
- **NMake 빌드**: 프로젝트의 빌드·다시 빌드·정리는 Rocks.Build를 부른다. Visual Studio가 직접 컴파일하지 않는다.
- **게임 모드**: `.rocksproject`의 SDK 연결, 다중 구성 SDK, 입력 snapshot 제외, 정리 명령, 출력 경로를 먼저 정하고 같은 생성기를 쓴다.
- **템플릿 생성**: 빌드되는 실제 프로젝트인 템플릿을 이름 치환과 ID 재발급으로 새 게임 프로젝트로 만든다.

2026-11-14 검토 결과(엔진 먼저, 계획 기반 IntelliSense, NMake, 게임 쪽 결정 네 가지, UE식 템플릿 생성)를 출발점으로 삼는다.

대상 코드는 `Programs/Rocks.Build`(ActionGraph·CompilationDatabase·Pch·Descriptors), `Programs/Rocks.Build.Cli`(Program·InstalledSdk·GameInputSnapshot·MetadataLayout), `Programs/Rocks.Build.Abstractions/RuleLoader.cs`, `Programs/Rocks.MetaCompiler`(identity 명세 명령), `Rocks.proj`, `Programs/Directory.Build.props`, `Templates/Game/Foundation`이다. 줄 번호는 f5942fac 기준이다.

UE 근거는 UE 5.8.4(`release`, 로컬 클론 `C:\github\UnrealEngine`, f45729c0b6f0)다. UBT 경로는 `Engine/Source/Programs/UnrealBuildTool/` 기준으로 적는다(`ProjectFiles/VisualStudio/VCProject.cs`·`VCProjectFileGenerator.cs`·`VisualStudioSolutionBuilder.cs`, `ProjectFiles/ProjectFileGenerator.cs`, `Modes/GenerateProjectFilesMode.cs`·`Modes/CleanMode.cs`). 그 밖에 `GenerateProjectFiles.bat`, `Engine/Build/BatchFiles/{Build,Rebuild,Clean,GenerateProjectFiles}.bat`, `Engine/Source/Programs/UnrealVersionSelector`, `Engine/Source/Developer/DesktopPlatform`, `Engine/Source/Editor/GameProjectGeneration`, `Templates/*/Config/TemplateDefs.ini`을 읽었다. Visual Studio MSBuild 동작은 VC 대상 파일(`Microsoft.Makefile.props`·`Microsoft.Cpp.DesignTime.targets`·`Microsoft.Cpp.Current.targets`·`Microsoft.MakeFile.targets`)을 근거로 적는다. UE 이름은 대응표와 근거에만 적는다. 레거시와 deprecated 경로는 따르지 않는다(`.sln` 병행 출력, VS 2022 이하 형식 선택, `.suo` 쓰기, UE4 시절 `EngineAssociation` 추정).

상태: **설계(개정 2)**. 개정 1의 결정은 사용자가 권장안대로 승인했고(2026-11-21), 개정 2는 리뷰 지적을 반영해 결정 일부를 고치고 PJD32–PJD43을 더했다. 구현 단계는 PJ0–PJ8(15절)이다. 착수는 사용자 지시를 받은 뒤다.

## 1. 현재 상태와 문제

| 항목 | 현재 | 이 스펙에 주는 영향 |
|---|---|---|
| 솔루션 | C# 서식 검사용 `Build/Format/Rocks.Format.slnx`뿐이다. C++ 프로젝트 파일은 없다 | Visual Studio에서 엔진 C++을 열 방법이 없다. C# 프로젝트 목록은 이 파일이 이미 전부 가진다 |
| 빌드 진입 | `dotnet msbuild Rocks.proj /t:Build·Test·InstalledSdk·Game`(`Rocks.proj:45-62`). `BootstrapTools`가 매번 `dotnet build Programs/Rocks.Tools.csproj`를 돈다(`Rocks.proj:42-44`). CLI 경로는 Debug 고정이다(`Rocks.proj:23`, `Program.cs:516`) | NMake 명령의 호출 대상이다. 솔루션 빌드가 여러 프로젝트를 동시에 돌리면 도구 빌드와 도구 사용이 겹친다(7.2절) |
| CLI 명령 | `build`·`test`·`sdk`·`game`·`bootstrap`·`unity-profile`(`Program.cs:45, 59-66`) | `clean`·`projectfiles`·`new-project`·`sdk-register`·`associate`가 없다 |
| compile DB | 출력마다 `compile_commands.json`. 원본 소스마다 논리 명령 하나이고 `directory`·`file`·`output`·`arguments`만 쓴다(`CompilationDatabase.cs:13-18`). 모듈 이름은 없다 | 파일을 프로젝트에 나누고 원천 타깃을 고르려면 계획(`NativePlan.LogicalCompiles`, `ActionGraph.cs:88-89`)을 직접 써야 한다 |
| 계획의 부작용·전제 | 계획 단계가 생성물의 존재를 검사한다: 소스(`ActionGraph.cs:250-255` — 리플렉션 생성 소스는 메타 생성이 게시한 뒤에 생긴다, `MetadataLayout.cs:37-41`), include 디렉터리(`:258-263` — `<out>/Generated/<M>` 포함), prebuilt(`:540-543`), ASan thunk(`:573-574`). CLI는 계획 전에 출력에 쓴다(`Program.cs:196-201, 288-291`) | 메타 생성 없이, 출력에 쓰지 않고는 지금 계획을 만들 수 없다. IDE 계획 모드가 필요하다(8.1절) |
| 정의 | 모듈 정의는 `<out>/Definitions/<M>.Definitions.h`(`.Shared`·`.Extra` 변형)로 강제 include한다(`Pch.cs:30-36`, `ActionGraph.cs:284, 321`). 구성·플랫폼·타깃 매크로만 명령줄 `/D`다 | IntelliSense가 정의 헤더를 봐야 한다. 헤더는 그 타깃·구성을 빌드하거나 `--plan-only`를 돌린 출력에만 있다 |
| 컴파일러 판정 | `Base/Compiler.h:15-16`이 `__clang__`이 없으면, `:79-84`가 `ROCKS_PLATFORM_*`이 대상 OS와 다르면 `#error`다 | MSVC 모드 IntelliSense(UE 방식)로는 모든 파일에 오류가 뜬다(8.5절). Android 구성을 clang-cl x64로 흉내 내도 모든 파일이 오류다(6.3절) |
| 타깃 | 규칙 어셈블리의 모든 `TargetRules`(`RuleLoader.cs:63-64`). 지금 15개: Game 종류 2개(`Engine` 라이브러리 묶음, `Sandbox` — `Type`을 적지 않아 기본값 Game, `BuildRules.cs:58`)와 Program 13개. `TargetType`은 `Game`·`Program`뿐이다(`TargetType.cs`). 타깃에는 정의 파일 경로가 없다 | 종류로 엔진 프로젝트를 고르면 `Engine`과 `Sandbox`가 같은 구성 이름이 된다. 프로젝트는 `.Target.cs` 위치로 나눈다(6.2절) |
| 모듈 위치 | `ModuleDirectory`는 선택 칸이고(`NativeBuildModels.cs:293-298`) `SourceModuleRules`만 채운다(`SourceModuleRules.cs:190, 202`). Program 모듈은 비어 있다(예: `Sandbox.Build.cs:10-21`). 손으로 쓴 소스 1146개 중 407개가 둘 이상의 모듈로 컴파일된다(예: `CompressionTraining/Tests/*.cpp`) | 모듈 기준으로는 파일 소유가 정해지지 않는다. 파일 경로로 정한다(6.2절) |
| 구성·플랫폼 | `Debug`·`Development`·`Release`·`Asan`·`Coverage`(`NativeBuildModels.cs:49`), `Windows`·`MacOS`·`Android`(`NativeBuildModels.cs:6`) | 솔루션 구성·플랫폼 표의 원천이다 |
| 게임 SDK 연결 | `.rocksproject`는 `engineVersion`만 있고 모르는 필드를 거부한다(`Descriptors.cs:184-207`, 허용 키 `:189`). SDK 경로는 매번 `--sdk`로 준다 | 프로젝트 파일 생성기가 SDK를 찾을 방법이 없다(10.1절) |
| SDK 구성 | SDK 하나에 구성 하나다(`SdkManifest.Configuration`, `InstalledSdk.cs:33`). `Verify`가 구성이 다르면 거부한다(`InstalledSdk.cs:495-501`) | 게임 솔루션이 Debug·Development·Release를 함께 가질 수 없다(10.3절) |
| 게임 입력 snapshot | 프로젝트 루트 전체를 내용째 해시하고 `.git` 디렉터리, 출력, SDK만 뺀다(`GameInputSnapshot.cs:22, 51-54`). 빌드 시작과 끝을 비교한다(`InstalledSdk.cs:318, 457-458`). 게임 규칙 탐색(`InstalledSdk.cs:370-372` 앞의 규칙 어셈블리 준비)도 프로젝트 아래를 훑는다 | Visual Studio가 `.vs/`의 IntelliSense DB를 쓰기 권한으로 열고 빌드 중에도 고친다. 읽기 실패나 snapshot 불일치로 게임 빌드가 실패한다(10.6절) |
| 게임 출력 | `<project>/out/<Configuration>`(`InstalledSdk.cs:218, 290`). 타깃 이름이 없다 | 게임 타깃이 둘 이상이면(Editor·Server 예정, BuildTargets 3.1절) 출력이 겹친다. 정리 명령의 대상도 정할 수 없다 |
| 정리 | 정리 명령이 없다 | NMake Clean·Rebuild가 부를 것이 없다 |
| C# 출력 | 모든 C# 프로젝트가 `out/dotnet/bin`·`out/dotnet/obj`를 쓴다(`Programs/Directory.Build.props:10-11`) | Visual Studio의 C# 디자인 타임 빌드·restore가 `IdeBuild`의 도구 빌드와 같은 파일을 쓴다(6.4절) |
| 템플릿 | `Templates/Game/Foundation`은 빌드되는 실제 프로젝트다(IntegrationTests가 복사해 빌드한다, `Rocks.Build.IntegrationTests/Program.cs:170`). 이름은 `Game.rocksproject`·`SampleGame` 모듈·`Game` 타깃·`sample` namespace이고 ID는 고정값이다(`SampleGame.rocksids.json`) | 흔한 단어("Game")라 문자열 치환이 `GameObject` 같은 무관한 이름까지 바꾼다. 템플릿으로 만든 프로젝트끼리 타입 ID가 겹친다 |

## 2. 용어 대응

| Rocks | UE 5.8 대응 | 뜻 |
|---|---|---|
| 프로젝트 파일 생성 `projectfiles` | UBT `-ProjectFiles`(`Modes/GenerateProjectFilesMode.cs:18`) | 솔루션·프로젝트 파일을 만드는 CLI 명령 |
| 엔진 모드 / 게임 모드 | 엔진 솔루션 / foreign 프로젝트 솔루션 | `--root`(저장소) / `--project`(게임 `.rocksproject`) |
| IDE 계획 모드 | (UBT가 IntelliSense용 컴파일 환경을 만드는 단계) | 출력에 쓰지 않고 생성물 존재를 요구하지 않는 계획 |
| 엔진 솔루션 `Rocks.slnx` | `UE5.sln`·`UE5.slnx` | 저장소 루트의 솔루션 |
| 엔진 프로젝트 `Rocks` | `UE5` 프로젝트 | `Core/`·`Engine/` 파일을 담고 그 아래 정의된 타깃을 구성으로 갖는 NMake 프로젝트 |
| 프로젝트 파일 디렉터리 `out/ProjectFiles` | `Engine/Intermediate/ProjectFiles`(`ProjectFileGenerator.cs:451`) | 생성한 `.vcxproj`·props·정의 헤더 사본 |
| IDE 빌드 진입점 `Build/IdeBuild.bat` | `Engine/Build/BatchFiles/Build.bat`·`Rebuild.bat`·`Clean.bat` | NMake 명령이 부르는 스크립트 |
| 도구 준비 `Build/Tools.ps1` | `BuildUBT.bat` | 빌드 도구를 필요할 때만 빌드한다 |
| IDE 빌드 잠금 | `-WaitMutex`(`VCProject.cs:2281-2282`) | 솔루션 빌드의 도구 준비·Rocks.Build 호출을 하나씩 돌린다 |
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
- 생성은 `out/ProjectFiles`(게임은 `<project>/out/ProjectFiles`)와 솔루션 파일 밖에 아무것도 쓰지 않는다.
- 생성 결과는 결정적이다. 같은 입력이면 byte가 같고, 바뀐 파일만 다시 쓴다.
- IDE에서 빌드한 결과와 터미널(`Rocks.proj`)에서 빌드한 결과가 같다. 한쪽에서 빌드한 뒤 다른 쪽은 0 action이다.
- 게임 프로젝트는 SDK 연결만으로 솔루션을 만든다. 엔진 저장소가 없어도 된다(SDK가 `Tools/Rocks.Build.Cli`를 싣는다, `InstalledSdk.cs:174-177`).
- 템플릿으로 새 게임 프로젝트를 만든다.

범위 밖: VS Code·Rider·Xcode 생성(13절), Visual Studio가 직접 컴파일하는 MSBuild 프로젝트(18절), `.sln` 출력, 원격·Linux 디버깅, 에디터(Rocks Studio) 안의 새 프로젝트 UI.

## 4. 측정: 논리 명령과 Visual Studio 속성

2026-11-21 Windows 출력의 `compile_commands.json`을 분류했다(스크립트로 인자 철자별 집계, 모듈은 `output`의 `obj/<모듈>/` 조각으로 판별). 리뷰어가 같은 결과를 재현했다(표 밖 철자 0건, 파일당 `/FI` 0·1·3).

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
| `/I<dir>` | 전부 | `AdditionalIncludeDirectories`(전체 목록, 8.3절) | include 탐색 |
| `/DWIN32` `/D_WINDOWS` `/DNOMINMAX` `/DWIN32_LEAN_AND_MEAN` `/DROCKS_PLATFORM_WINDOWS=1` `/DROCKS_BUILD_*=1` `/DROCKS_MONOLITHIC=1` `/DROCKS_LOG_COMPILED_MINIMUM=<n>` `/DNDEBUG` | 전부(구성마다 값이 다름) | `PreprocessorDefinitions`(전체 목록, 8.3절) | 한 출력의 모든 항목에서 같았다 |
| `/FI<out>/Definitions/*.h` | 전부 | `ForcedIncludeFiles`(사본 경로로 바꿈, 8.4절) | 모듈 정의 |
| `/FI<src>/…/CoreSharedPch.h`·`ObjectSharedPch.h` | 전부 | `ForcedIncludeFiles` + `AdditionalOptions`의 `/Yu"<헤더>"` | PCH 경계 힌트(UE `VCProject.cs:1385, 1611`) |
| `/clang:-std=c++23` | C++ 항목 | `/std:c++23preview`(V11에서 받지 않으면 `/std:c++latest`) | IntelliSense는 `/clang:`을 모른다. clang-cl 17 이상의 `/std:c++latest`는 C++26이다. `Compiler.h:19-20`이 `__cpp_if_consteval`을 요구한다 |
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

결론: 논리 명령은 Visual Studio IntelliSense 속성으로 남김없이 옮겨진다. 모듈 안에서 달라지는 것은 강제 include뿐이므로 파일별 속성은 모듈 × 강제 include 모양의 수(Foundation Debug 45개)로 접힌다. 측정한 출력 하나는 검증 목록 밖의 clang-cl(Swift 도구 모음 안의 것)로 빌드됐다. 그래서 LLVM 설치 위치는 컴파일러 경로에서 짐작하지 않고 컴파일러에게 묻는다(8.5절).

## 5. 결정 요약

- 출력: 엔진은 루트 `Rocks.slnx` + `out/ProjectFiles/`, 게임은 `<project>/<이름>.slnx` + `<project>/out/ProjectFiles/`. `.slnx`만 쓴다(PJD1–PJD3).
- 계획: IDE 계획 모드(존재 검사·archive·link·출력 쓰기 없음)와 부작용 없는 계획 함수 분리(PJD32).
- 배치: `.Target.cs` 위치로 프로젝트를 정한다. `Core/`·`Engine/` 아래 타깃은 엔진 프로젝트 `Rocks`의 구성, 그 밖은 타깃마다 프로젝트. 파일은 경로로 프로젝트에 속하고, IntelliSense는 (파일, 구성)마다 원천 명령을 고른다(PJD4, PJD33). C# 프로젝트는 빌드하지 않는 항목이고 Visual Studio 안 C# 빌드는 출력 경로를 나눈다(PJD31, PJD38).
- 구성: Debug·Development·Release·Asan × Windows(x64)·Android(arm64, 빌드 전용, NDK가 있을 때)(PJD5, PJD34).
- IntelliSense: 논리 명령을 전부 분류해 파일별 전체 목록으로 쓴다. 기본값은 `ItemDefinitionGroup`에 직접 쓰고 `NMakeIncludeSearchPath`는 비운다. 정의 헤더는 내용 주소 사본, PCH 헤더는 강제 include + `/Yu` 힌트, IntelliSense는 ClangCL 모드다(PJD6–PJD12, PJD35).
- 생성 헤더: 메타 생성은 돌리지 않고, 없는 생성 헤더는 어느 타깃·구성을 빌드하면 생기는지 알린다(PJD36).
- NMake: `Build/IdeBuild.bat`가 IDE 빌드 잠금 하나 안에서 도구 준비(입력 지문이 같으면 건너뜀)와 Rocks.Build 실행을 한다. 컴파일러는 절대 경로로 넘긴다. `clean` 명령을 새로 둔다(PJD14–PJD16, PJD37, PJD39).
- 재생성: 수동이다. 빌드가 생성기와 같은 필터의 상태 지문과 대조해 낡았으면 경고만 한다(PJD17).
- 게임: 출력에 타깃 이름, snapshot·규칙 탐색에서 점 디렉터리·`out/`·`Saved/`·최상위 `.slnx` 제외, `engineAssociation`(경로 또는 등록 ID, 없으면 `engineVersion`과 Verify 호환 조건으로 등록 SDK를 고름), 명시적으로만 하는 등록, 다중 구성 SDK(PJD18–PJD23, PJD40).
- 템플릿: 고유 이름 `FoundationTemplate`의 실제 프로젝트, `Template.json`, 두 단계 치환, `ids reissue`, `new-project` 명령(PJD24–PJD26, PJD43).
- 진입점: 루트 `GenerateProjectFiles.bat`·`GenerateProjectFiles.ps1`, `Build/*.ps1`, 명시 등록하는 HKCU 셸 메뉴(PJD27–PJD28).
- 디버그: 실행 프로젝트를 솔루션 첫 항목에, Asan 런타임 경로를 디버거 환경에(PJD41). 파일 하나 컴파일은 논리 명령을 별도 object로(PJD29, PJD42).

## 6. 엔진 모드 솔루션

### 6.1 명령과 출력 위치

`Rocks.Build.Cli projectfiles --root <저장소>`가 만든다. 같은 명령에 `--project <.rocksproject>`를 주면 게임 모드다(10절). 둘 다 주거나 둘 다 없으면 오류다.

| 파일 | 위치 | 내용 |
|---|---|---|
| 솔루션 | `<root>/Rocks.slnx` | 폴더·프로젝트·구성 대응 |
| 엔진 프로젝트 | `<root>/out/ProjectFiles/Rocks.vcxproj`(+`.filters`) | `Core/`·`Engine/` 파일 |
| 타깃 프로젝트 | `<root>/out/ProjectFiles/Programs/<타깃>.vcxproj`(+`.filters`) | 그 타깃 정의 디렉터리 아래 파일(6.2절) |
| 공통 props | `<root>/out/ProjectFiles/Rocks.ProjectFiles.props` | `RocksRoot` 등 경로 기준 속성 |
| 도구 모음 props(앞) | `<root>/out/ProjectFiles/Rocks.Toolchain.props` | `Microsoft.Cpp.Default.props` **앞**에 import. 이 컴퓨터의 `LLVMInstallDir`·`LLVMToolsVersion`·`RocksCompiler`(clang-cl 절대 경로)·(게임) `RocksSdkRoot` |
| 도구 모음 props(뒤) | `<root>/out/ProjectFiles/Rocks.ToolchainPaths.props` | `Microsoft.Cpp.props` **뒤**에 import. 시스템 `IncludePath`, Asan 디버거 환경(8.5절, 7.1절) |
| 정의 헤더 사본 | `<root>/out/ProjectFiles/Definitions/<내용 SHA-256 앞 16자>.h` | 8.4절 |
| Visual Studio 중간 파일 | `<root>/out/ProjectFiles/Intermediate/<프로젝트>/<구성>_<플랫폼>/` | 프로젝트의 `IntDir`·`OutDir`로 명시한다(UE `VCProjectFileGenerator.cs:621-622`) |
| 상태 | `<root>/out/ProjectFiles/ProjectFiles.state.json` | 지문(9절)과 이번 실행이 만든 파일 목록(6.5절) |

솔루션은 UE처럼 루트에 둔다(`UE5.sln`·`UE5.slnx`, `VCProjectFileGenerator.cs:741, 769`). 프로젝트는 이미 무시되는 `out/` 아래에 둔다(`.gitignore`의 `/out/`). `.gitignore`에 `/Rocks.slnx`를 더한다. `.vs/`·`*.user`는 이미 무시한다.

솔루션 형식은 `.slnx` 하나다. UE 5.8은 `.sln`과 `.slnx`를 함께 쓰지만(`VCProjectFileGenerator.cs:779-786`) Rocks는 하위 호환을 두지 않는다. 쓰기는 `Microsoft.VisualStudio.SolutionPersistence` 패키지로 한다. UE와 같은 패키지이고(`UnrealBuildTool.csproj:71`, 1.0.52) C# 도구는 패키지를 쓸 수 있다. 기준 Visual Studio는 2026(18.x)이다. 검증한 MSVC STL 14.52가 그 세대이고(`Build/README.md` 요구 사항), 프로젝트의 `MinimumVisualStudioVersion`은 18.0이다.

### 6.2 프로젝트 배치

**타깃 정의 디렉터리.** 타깃 T의 정의 디렉터리는 T의 규칙 타입을 선언한 `.Target.cs` 파일의 디렉터리다. 생성기는 규칙 어셈블리와 같은 glob(`Build/Rules/Rocks.Engine.Rules.csproj`: `Core/Source/**`·`Engine/Source/**`·`Engine/Plugins/**`·`Programs/**`의 `*.Target.cs`, `Programs/Rocks.*` 제외)으로 파일을 모으고, `class <타입 이름>` 선언이 정확히 한 파일에 있는지 찾는다(0개나 2개 이상이면 생성 오류). UE도 타깃마다 `.Target.cs` 경로를 안다. `ModuleDirectory`(선택 칸, Program 모듈은 비어 있음)나 주 모듈의 소스 공통 접두는 쓰지 않는다. 앞은 비어 있고, 뒤는 `ReflectionFixtures`처럼 주 모듈이 `Core/`에 있는 Program 타깃을 엔진 쪽으로 잘못 보낸다.

| 솔루션 폴더 | 프로젝트 | 구성 원천 | 파일 |
|---|---|---|---|
| `Engine` | `Rocks` | 정의 디렉터리가 `Core/`·`Engine/` 아래인 타깃(지금 `Engine`) | 경로가 `Core/`·`Engine/` 아래인 파일 |
| `Programs` | 그 밖의 타깃마다 `<타깃>`(지금 `Sandbox`와 Program 13개) | 그 타깃 하나 | 경로가 그 정의 디렉터리 아래인 파일(아래 소유 규칙) |
| `Programs/Tools` | `Build/Format/Rocks.Format.slnx`의 C# 도구 프로젝트 | 빌드 안 함(PJD31) | 각 `.csproj` 그대로 |
| `Programs/Rules` | `Build/Rules/Rocks.Engine.Rules.csproj`, `Build/Format/Rocks.Template.Rules.csproj` | 빌드 안 함 | `*.Build.cs`·`*.Target.cs` 편집용 |

UE와 같이 엔진 소스는 프로젝트 하나에 담고(`ProjectFileGenerator.cs:1314`의 `Engine` 폴더), Program은 프로젝트를 따로 둔다(`ProjectFileGenerator.cs:1450`의 `Programs` 폴더). 모듈마다 프로젝트를 두지 않는다(18절). `Rocks` 프로젝트 안에서 두 타깃의 프로젝트 구성 이름이 같아지면(같은 종류의 엔진 타깃 둘) 생성 오류다. UE도 같은 경우 `BuildException`이다(`VCProject.cs:758-760`).

**파일 소유(경로 기준).** 파일 하나는 프로젝트 하나에만 나온다.
- 대상 파일: 어떤 타깃이든 **어느 구성**(Debug·Development·Release·Asan, Windows)의 계획에서 컴파일하는 손으로 쓴 소스의 합집합, 그 소스를 컴파일하는 모듈의 public·private include 디렉터리와 소스 디렉터리의 헤더(`.h/.hh/.hpp/.hxx/.inl/.inc/.ipp/.tpp`)다. 출력 아래 생성 파일(리플렉션 생성 소스, BuildInfo·모듈 표 TU, 정의 헤더, `PublicHeaderChecks` 생성 TU)은 넣지 않는다. UE도 `.gen.cpp`를 넣지 않는다.
- 경로가 `Core/`·`Engine/` 아래면 `Rocks`다.
- 아니면 정의 디렉터리가 그 파일의 가장 가까운 조상인 타깃들 중 하나다. 그런 타깃이 여럿이면(예: `Programs/FoundationTests`의 `Foundation`·`EngineTests`·`ReflectionFixtures`·`Core`) 다음 순서로 고른다: (1) 정의 파일 이름(`<이름>.Target.cs`)과 이름이 같은 타깃(`Foundation.Target.cs` → `Foundation`), (2) 기준 구성(Development)에서 그 파일을 컴파일하는 타깃, (3) 이름 ordinal.
- 어느 정의 디렉터리 아래에도 없으면(예: `.Target.cs`가 없는 `Programs/LaunchTests`) 기준 구성에서 그 파일을 컴파일하는 Program 타깃 중 이름 ordinal 첫 번째의 프로젝트에 넣는다. 그런 타깃도 없을 때만 `Rocks`에 넣는다. 알림은 디렉터리마다 한 줄이다.
- `.filters`는 저장소 기준 디렉터리 구조를 그대로 옮긴다.
- 모델은 (파일, 프로젝트 구성)마다 고른 논리 명령을 함께 싣고, 검사(16절 V1)는 묶음에 접힌 값을 그 명령과 대조한다.

**IntelliSense 원천((파일, 구성) 단위).** 프로젝트 P의 구성 C(타깃 T)에서 소스 f의 설정은 다음 순서로 고른 논리 명령 하나에서 온다. 407개 소스가 여러 모듈로 컴파일되므로 모듈이 아니라 명령을 고른다.
1. T의 C 계획이 f를 컴파일하면 그 명령이다. 같은 계획에서 f를 컴파일하는 모듈이 여럿이면 모듈 이름 ordinal 첫 번째다.
2. 아니면 C에서 f를 컴파일하는 타깃 중 이름 ordinal 첫 번째 타깃의 명령이다(같은 모듈 규칙).
3. 아무 타깃도 컴파일하지 않으면 설정 없이 넣고 알린다.

### 6.3 타깃 × 구성 × 플랫폼

솔루션 구성은 `Debug`·`Development`·`Release`·`Asan`이다. `Coverage`는 `Build/MeasureCoverage.ps1`이 쓰는 측정 구성이라 넣지 않는다. 솔루션 플랫폼은 `Windows`와 `Android`다.

| 솔루션 | 프로젝트 구성 | 근거 |
|---|---|---|
| `<C>`·`Windows` | `<C>|x64` | Visual Studio가 아는 플랫폼은 그 이름을 쓴다(UE `Win64`→`x64`, `VCProject.cs:303, 730`) |
| `<C>`·`Android` | `Android_arm64_<C>|x64` | Visual Studio가 모르는 플랫폼은 구성 이름에 플랫폼·아키텍처를 붙이고 플랫폼은 `x64`로 둔다(`VCProject.cs:736-750`) |
| `Rocks` 안의 Game이 아닌 종류 엔진 타깃(예정: Editor·Client·Server) | `<C>_<종류>|x64`, 솔루션 구성은 `<C> <종류>` | `VCProject.cs:752-755`. 지금은 `Engine` 하나라 해당 없음 |

- **Android 구성은 빌드 전용이다.** NMake 명령만 Android 빌드를 부르고, IntelliSense 속성은 같은 `<C>`의 Windows 값을 그대로 쓴다. clang-cl x64로 흉내 내면 `__ANDROID__`가 없고 `_M_X64`가 정의되며 NDK sysroot도 없어 `Compiler.h:79-84`에서 모든 번역 단위가 `#error`이기 때문이다. 그래서 Android 계획은 IntelliSense에 쓰지 않는다.
- Android는 생성할 때 NDK를 찾으면(`NativeToolchains.DiscoverAndroidAsync`, CLI 기본 ABI arm64) 넣고, 못 찾으면 빼고 알린다. UE도 SDK가 설치된 플랫폼만 넣는다(`VCProject.cs:706`). x86_64는 CLI로만 빌드한다.
- macOS는 Visual Studio가 없는 호스트라 넣지 않는다(13절).
- Android 구성은 F5가 없다(`NMakeOutput`이 비어 있다). 기기 실행은 `test --platform android --adb`다.

### 6.4 C# 프로젝트

C# 프로젝트 목록의 단일 출처는 `Build/Format/Rocks.Format.slnx`다. 저장소의 모든 `.cs`가 이 솔루션의 프로젝트에 속하는지 format 검사가 이미 확인한다(`Build/README.md` 별도 회귀 검사). 생성기는 이 파일을 같은 패키지로 읽어 경로를 옮겨 적는다.

- 솔루션 빌드는 이 프로젝트들을 빌드하지 않는다(slnx `Build Project="false"`). UE도 UBT 프로젝트는 설정이 켜졌을 때만 솔루션 빌드에 넣는다(`VCProjectFileGenerator.cs:765`의 `DebugPredicate`).
- Visual Studio는 C# 프로젝트를 열면 디자인 타임 빌드와 restore를 스스로 돈다. 이것이 `out/dotnet/obj`를 쓰면 `IdeBuild`의 도구 빌드와 겹친다. 그래서 C# 출력 경로를 정하는 props(`Programs/Directory.Build.props:10-11`과 규칙 프로젝트 쪽 같은 설정)가 `'$(BuildingInsideVisualStudio)' == 'true'`일 때 `out/dotnet-vs/bin`·`out/dotnet-vs/obj`를 쓰게 한다(PJD38). IDE 안 C# 작업은 도구 실행 파일(`out/dotnet/bin`)을 건드리지 않는다.

### 6.5 결정성과 갱신

- 프로젝트 GUID는 이름 기반 UUID v5(SHA-1)다. 이름 공간은 생성기 상수 GUID, 이름은 루트 기준 프로젝트 파일 경로(`/` 구분, 대소문자 그대로)다. UE는 같은 목적으로 MD5 이름 기반 GUID를 만들되 기존 파일의 GUID를 먼저 다시 읽는다(`VCProject.cs:113-129`, `VCProjectFileGenerator.cs:544-575`). Rocks는 다시 읽지 않는다. 같은 경로면 언제나 같은 GUID다.
- 순서: 프로젝트·파일은 ordinal 정렬, 프로젝트 구성은 타깃 이름 ordinal 다음 구성 순서(Debug·Development·Release·Asan)다. 묶음 번호는 묶음 key(구성별 네 값의 정규 텍스트)의 ordinal 정렬 순서로 매긴다 — 소스를 하나 더해도 새 묶음이 생기지 않는 한 번호가 바뀌지 않아 프로젝트 파일이 불필요하게 다시 써지지 않는다. 시각·사용자 이름·임시 경로를 쓰지 않는다.
- 경로: 저장소 안 경로는 `$(RocksRoot)` 기준 상대 경로로 쓴다. 이 컴퓨터에만 맞는 값(LLVM·컴파일러·MSVC·Windows SDK·NDK·SDK 위치)은 두 도구 모음 props에만 둔다. 그래서 그 두 파일을 뺀 생성물은 작업 사본 위치와 무관하게 같다(16절 V4).
- 인코딩: UTF-8(BOM 없음), CRLF. 정의 헤더 사본은 빌드가 쓰는 것과 같은 바이트다(LF).
- 갱신: 내용이 같으면 쓰지 않는다(UE `WriteFileIfChanged`, `ProjectFileGenerator.cs:4038`). Visual Studio가 바뀌지 않은 프로젝트를 다시 읽지 않게 하기 위해서다.
- 정리: 생성이 끝나면 **직전 `ProjectFiles.state.json`의 생성 파일 목록에 있었고 이번에 만들지 않은 파일**만 지운다(없어진 타깃의 프로젝트, 쓰지 않게 된 정의 헤더 사본). 목록에 없는 파일은 건드리지 않는다. `.vcxproj.user`(Visual Studio가 쓰는 사용자 디버그 설정)와 `Intermediate/` 아래 Visual Studio 중간 파일이 여기에 든다. UE도 `.user`는 없을 때만 만든다(`VCProject.cs:1855-1864`).
- 생성은 `out/ProjectFiles`의 `OutputInvocationLock`을 잡고 한다.

## 7. NMake 명령

### 7.1 명령줄과 디버그

| 속성 | 엔진 모드 값 |
|---|---|
| `NMakeBuildCommandLine` | `"$(RocksRoot)Build\IdeBuild.bat" -Action Build -Target <T> -Configuration <C> -Platform <X> -Compiler "$(RocksCompiler)"`(Android 행은 `-Compiler`를 넣지 않는다 — CLI가 `--platform android`와 `--compiler`를 함께 받지 않는다) |
| `NMakeReBuildCommandLine` | 같은 인자, `-Action Rebuild` |
| `NMakeCleanCommandLine` | 같은 인자, `-Action Clean` |
| `NMakeOutput` | 실행 파일 출력 경로(IDE 계획 모드가 계산한 `ModuleOutputs[Modules[0]]`). 실행 파일이 아니면 비운다 |
| `LocalDebuggerWorkingDirectory` | `$(RocksRoot)` |
| `LocalDebuggerEnvironment`(Asan 구성) | `PATH=<clang_rt.asan_dynamic DLL 디렉터리>;$(PATH)`(`Rocks.ToolchainPaths.props`) |
| `NMakeCompile` / `NMakeCompileFileCommandLine` | PJ7부터만 내보낸다(7.4절) |

UE도 Build·Rebuild·Clean 배치 파일을 부르고(`VCProject.cs:2233-2235, 2516-2518`) 실행 파일을 `NMakeOutput`에 둔다(`VCProject.cs:2520-2527`). 게임 모드는 10.4절이다.

- **컴파일러 경로.** Visual Studio는 NMake 명령 전에 `PATH`를 `$(ExecutablePath)`로 바꾼다(`Microsoft.Cpp.Current.targets:89-102`). PATH에서 컴파일러를 찾으면 터미널과 다른 clang-cl을 고를 수 있다. 그래서 생성기가 탐색한 clang-cl 절대 경로를 `RocksCompiler`로 적고 `IdeBuild`가 `--compiler`로 넘긴다. 같은 컴파일러면 IDE 빌드와 터미널 빌드의 action key가 같다(16절 V6). 도구 모음이 바뀌면 재생성 경고가 난다(9절).
- **Asan 디버그.** Asan 실행 파일은 선택한 compiler runtime DLL(`clang_rt.asan_dynamic*`)이 필요하다. 빌드는 그 디렉터리를 찾는다(`Toolchain.cs:72`, resource 디렉터리 조회 `NativeEnvironment.cs:193-205`). 생성기는 같은 값을 `LocalDebuggerEnvironment`에 넣어 F5가 같은 DLL을 쓰게 한다.
- **시작 프로젝트.** Visual Studio는 사용자 설정이 없으면 솔루션의 첫 프로젝트를 시작 프로젝트로 쓴다. 그러나 `.slnx` 직렬화기(`Microsoft.VisualStudio.SolutionPersistence`)가 폴더와 프로젝트를 이름순으로 정렬해 쓰므로 생성기가 첫 항목을 정할 수 없다(실측: 폴더 `/Engine/`가 `/Programs/`보다 앞, 폴더 밖 프로젝트는 폴더 뒤). UE는 `.suo`를 써서 고르지만(`VCProjectFileGenerator.cs:790-792`) Rocks는 `.suo`를 쓰지 않는다. 그래서 시작 프로젝트는 사용자가 한 번 고르고("Set as Startup Project", Visual Studio가 `.vs/`에 기억한다) README에 그 방법을 적는다(개정 5).
- **natvis.** 자리만 정한다. 시각화 파일이 생기면 `Build/VisualStudio/Rocks.natvis`를 솔루션 항목으로 넣는다(UE `VCProjectFileGenerator.cs:747-758`).

`IdeBuild.ps1`(엔진 모드)은 IDE 빌드 잠금(이름 있는 mutex `Local\Rocks.IdeBuild.<루트 경로 SHA-256 앞 16자>`) 하나를 잡고 그 안에서 다음을 한다.
1. 도구 준비(`Build/Tools.ps1`, 7.2절).
2. `dotnet out/dotnet/bin/Rocks.Build.Cli/Debug/net10.0/Rocks.Build.Cli.dll build --root <root> --target <T> --configuration <C> --compiler <경로> [--platform android]`. Rebuild는 같은 잠금 안에서 `clean` 다음 `build`다.
3. 종료 코드를 그대로 돌려준다.

잠금을 기다려야 하면 기다리기 전에 `Waiting for the IDE build lock ...`을 한 줄 낸다(V7이 이 줄로 순서를 단언한다). **환경 정리.** Visual Studio는 Makefile 프로젝트도 NMake 명령 전에 `SetBuildDefaultEnvironmentVariables`로 `INCLUDE`·`EXTERNAL_INCLUDE`·`LIB`·`LIBPATH`·`PATH`를 넣는다(`Microsoft.Cpp.Current.targets:80-114`). `VCToolsInstallDir`는 넣지 않으므로 빌드가 VsDevCmd를 다시 부르고, VsDevCmd는 기존 `INCLUDE` 앞에 자기 경로를 덧붙인다. 그러면 keyed 환경(`INCLUDE`·`LIB`)의 원문이 터미널과 달라 도구 모음 key가 갈라지고(IDE 빌드 뒤 터미널 빌드가 전부 다시 컴파일한다), 같은 `INCLUDE`로 렌더링한 props 때문에 `RPF0001` 거짓 경고가 난다. 그래서 `IdeBuild.ps1`은 CLI를 부르기 전에 이 네 변수를 지운다. UE도 같은 이유로 `INCLUDE`를 비운다(`VCToolChain.cs:151-154`). 최상위 실패는 `error RB0001: …`(CLI)·`error RB0002: …`(도구 준비) 한 줄을 더 내 Visual Studio 오류 목록에 잡히게 한다. `IdeBuild.bat`는 `pwsh -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0IdeBuild.ps1" %*`만 하는 얇은 껍데기다. `pwsh`가 없으면 설치 안내를 영어로 내고 1을 돌려준다. 컴파일 진단은 clang-cl의 MSVC 형식(`file(line,col): error`)이라 Visual Studio 오류 목록에 그대로 잡힌다.

### 7.2 직렬화와 도구 준비

솔루션 빌드는 여러 NMake 프로젝트를 병렬로 부른다. 지금 CLI는 출력 디렉터리마다 잠금(`OutputInvocationLock`, 최대 30분 대기)만 있고 타깃마다 출력이 다르다. 그래서 타깃 N개가 각자 `--jobs`(기본 `min(8, 코어 수)`, `Program.cs:522-525`)만큼 컴파일러를 띄운다. 도구 쪽은 두 문제가 있다. 첫째, 한 프로젝트가 도구를 다시 빌드하는 동안 다른 프로젝트가 같은 dll을 실행 중이면 덮어쓰기가 실패한다(MSB3027). 도구 빌드만 잠그고 실행을 잠그지 않으면 이 경합이 남는다. 둘째, 15개 프로젝트가 할 일 없는 `dotnet build`를 매번 되풀이한다.

- **한 잠금 범위.** UE는 모든 호출에 `-WaitMutex`를 붙여 UBT를 하나씩 돌린다(`VCProject.cs:2281-2282`, "so that building the whole solution doesn't fail"). Rocks도 IDE에서 부른 빌드는 IDE 빌드 잠금 하나 안에서 도구 준비와 CLI 실행을 함께 한다. `GenerateProjectFiles.ps1`도 같은 잠금을 쓴다. 도구를 덮어쓰는 동안 그 도구를 실행하는 IDE 호출이 없다.
- **필요할 때만 빌드.** `Build/Tools.ps1`은 도구 입력 지문을 계산해 `out/dotnet/.rocks-tools.json`의 값과 같으면 `dotnet build`를 건너뛴다. 입력은 `Programs/Rocks.Tools.csproj`가 참조하는 프로젝트들의 디렉터리(`bin`·`obj` 제외), 규칙 어셈블리 glob의 `*.Build.cs`·`*.Target.cs`, `Build/Rules/`, `Directory.Build.props`류·`global.json`·`Directory.Build.rsp`의 파일 경로와 내용 SHA-256, 그리고 `dotnet --version`이다. 빌드가 성공한 뒤에만 지문을 쓴다. 기록하는 입력 지문과 파일 memo는 **빌드 전에** 계산한 값이고 출력 해시만 빌드 뒤 값이다 — 빌드 중에 바뀐 입력은 다음 호출에서 다르게 보여 다시 빌드한다(개정 10). 그래서 솔루션 빌드 한 번에 도구 빌드는 많아야 한 번이다. 입력 집합이 MSBuild가 실제로 보는 입력을 덮는지는 시험한다(16절 V13).
- **잠금 성질.** 잠금은 명시 gate다. 시간 제한이나 재시도에 기대지 않는다. mutex는 같은 스레드에서 잡고 놓는다. 버려진 mutex(`AbandonedMutexException`)는 얻은 것으로 본다.
- **`Rocks.proj`.** `BootstrapTools`(`Rocks.proj:42-44`)는 `pwsh Build/Tools.ps1`을 부르도록 바꿔 같은 잠금과 지문을 쓴다. 그 뒤의 CLI 실행(`Rocks.proj:45-62`의 각 `Exec`)은 잠금 밖이다. 터미널 빌드가 도는 동안 IDE가 도구 원본 변경으로 도구를 다시 빌드하면 경합할 수 있다. 이 한계를 `Build/README.md`에 적는다(17절 L16). 터미널 빌드까지 IDE 잠금으로 묶으면 다른 세션의 터미널 빌드도 모두 줄을 서게 되어 그렇게 하지 않는다.

### 7.3 정리와 다시 빌드

CLI에 `clean`을 더한다. 출력 경로 계산은 `build`·`game`과 같은 함수를 쓴다. Rocks 출력 표지는 `.native-build.lock`, 또는 `.rocks-invocation.lock`과 실행 기록(`action-report.json`·`game-build-evidence.json`·`build-evidence.json`)이다 — 규칙·메타 단계에서 실패한 출력은 호출 잠금만 있고, 그 뒤 Rebuild가 막히면 안 된다(개정 13). 프로젝트 파일 디렉터리(`<루트>/out/ProjectFiles`와 그 아래)는 호출 잠금이 있어도 늘 거부한다(개정 14).
- 엔진: `clean --root <root> --target <T> --configuration <C> [--platform …]` → `out/dotnet-<platform>-<target>-<configuration>`(`Program.cs:140-141`).
- 게임: `clean --project <dir> --target <T> --configuration <C>` → `<project>/out/<platform>-<target>-<configuration>`(PJD18). 게임 모드는 `--root`·`--platform`·`--android-abi`를 거부하고, 경계 루트(출력이 그 `out`의 엄격한 하위여야 하는 곳)는 프로젝트 디렉터리다(개정 9).
- 지울 디렉터리는 `<root>/out`(게임은 `<project>/out`)의 엄격한 하위이고, Rocks 출력 표지 `.native-build.lock`이 있어야 한다. 아니면 거부한다. `--output`으로 준 비표준 출력도 같은 조건을 만족할 때만 받는다. 출력이 없으면 할 일이 없는 성공이다.
- 그 출력의 `OutputInvocationLock`을 잡은 뒤 잠금 파일을 뺀 모든 항목을 지운다.

UE Clean은 타깃의 중간 산출물과 바이너리를 지운다(`Modes/CleanMode.cs`). Rocks는 타깃·구성·플랫폼마다 출력 디렉터리 하나에 모든 산출물이 있으므로 디렉터리를 비우면 된다. Rebuild는 UE처럼 정리 다음 빌드다(`Rebuild.bat`이 `Build.bat … -Rebuild`를 부른다).

### 7.4 파일 하나 컴파일

Visual Studio의 파일 컴파일(Ctrl+F7)은 `NMakeCompileFileCommandLine`으로 Rocks.Build를 부른다(UE `-WorkingDir=$(MSBuildProjectDirectory) -Files=$(SelectedFiles)`, `VCProject.cs:2562`). PJ7 전에는 이 속성을 내보내지 않는다.
- 명령: `… -Action Compile -WorkingDir "$(MSBuildProjectDirectory)" -Files "$(SelectedFiles)"`. `$(SelectedFiles)`의 상대 경로는 `-WorkingDir`(프로젝트 파일 디렉터리, `out/ProjectFiles` 또는 `out/ProjectFiles/Programs`) 기준으로 풀고, 그 결과를 저장소 기준 경로로 바꾼다.
- `build --files <;로 나눈 경로>`는 평소 빌드처럼 메타 생성을 최신으로 맞춘 뒤, 고른 소스의 **논리 명령**을 그대로 돌린다. object는 `<out>/SingleFile/<소스 경로 해시>.obj`로 따로 내고 증분 기록에 넣지 않는다. 그래서 유니티 묶음·PCH 바이너리·작업 파일 분리와 무관하고, 평소 빌드의 object와 기록을 건드리지 않는다. 논리 명령은 PCH 바이너리 대신 PCH 헤더를 강제 include하므로 PCH action이 필요 없다.
- 헤더를 고르면 오류로 알린다(헤더 단독 컴파일은 `PublicHeaderChecks`가 맡는다).

구현(PJ7, 개정 17):
- **계획·실행.** `SingleFileCompile.RunAsync(request, sources)`(`Programs/Rocks.Build/SingleFileCompile.cs`)가 그 요청을 IDE 계획 모드(`IdePlanner.Plan`)로 계획해 고른 소스의 논리 명령을 찾는다 — IntelliSense 원천과 같은 함수라 Visual Studio가 보는 명령과 컴파일하는 명령이 같다. 한 소스를 여러 모듈이 컴파일하면 모듈 이름 ordinal 첫째다(8.1절 원천 규칙). 계획에 없는 소스(그 타깃이 컴파일하지 않음)와 헤더(`.h`·`.hh`·`.hpp`·`.hxx`·`.inl`·`.inc`·`.ipp`·`.tpp`)는 아무것도 컴파일하기 전에 오류다.
- **object와 기록.** object는 `<출력>/SingleFile/<저장소 기준 경로 SHA-256 앞 16자>.obj`(macOS `.o`)이고, 같은 이름의 `.json`에 소스·모듈·실행한 인자를 남긴다(시험이 compile_commands.json과 대조한다). 논리 명령의 `/Fo<경로>`(clang 드라이버는 `-o <경로>`)만 바꾼다. 낡음 지문·명령 비교가 object 경로를 빼는 규칙과 같은 함수(`CompileObjectArguments`)다 — 지문이 POSIX의 `-o <경로>`를 빼지 못해 macOS에서 빌드 계획과 IDE 계획의 지문이 갈렸다(개정 18). 강제 include하는 정의 헤더는 계획이 만든 내용으로 `WriteIfChanged`한다 — 평소 빌드와 같은 byte라 증분 기록을 건드리지 않는다. 증분 기록(`native-cache.json`), action-report, 평소 object는 바꾸지 않는다. 명령줄이 호스트 한계를 넘으면 빌드와 같은 규칙의 response file(`<해시>.rsp`)로 넘긴다.
- **원천은 1단계만(개정 19).** 엔진과 게임 모두 그 타깃이 컴파일하지 않으면 같은 구성에서 그 파일을 컴파일하는 타깃 이름을 IDE 계획으로 찾아 안내한다(게임은 개정 21). 안내를 찾다 실패한 타깃은 빼고(안내 단계), 원래 오류는 그대로다. IntelliSense 원천은 그 타깃이 컴파일하지 않으면 같은 구성에서 컴파일하는 다른 타깃의 명령을 빌려 온다(8.1절 2단계). Ctrl+F7은 빌린 타깃의 메타 생성·정의 헤더를 그 타깃 출력에 맞춰야 해서 비용이 크므로 1단계(그 타깃)만 컴파일한다. 그 타깃이 컴파일하지 않으면 같은 구성에서 그 파일을 컴파일하는 타깃 이름을 IDE 계획으로 찾아 오류에 적는다(그 타깃 프로젝트에서 Ctrl+F7을 누르면 된다).
- **object 이름(개정 19).** 해시는 디스크 철자(`PathSpelling.OnDisk`)의 저장소 기준 경로다 — Visual Studio가 같은 파일을 다른 대소문자로 넘겨도 같은 object다. 실행할 때 원본이 없어진 기록(`.json`·object·`.rsp`)은 지운다.
- **CLI.** `build --files <;로 나눈 경로>`(상대 경로는 `--root` 기준)와 게임 `game --files …`(상대 경로는 `--project` 기준)는 메타 생성까지 평소처럼 한 뒤 네이티브 빌드 대신 이것을 돈다. `test`·`sdk`, `--plan-only on`, 묶음 실측과는 함께 쓰지 않는다. build-evidence는 "single-file compile"로 끝난다(PASS가 아니다). 게임은 `game-build-evidence.json`의 status가 `SINGLE-FILE`이다(PASS가 아니다, 개정 19).
- **비용(개정 19).** Ctrl+F7 한 번은 도구 지문 확인(`Tools.ps1`), 규칙 로드·메타 배치·메타 생성(바뀐 것이 없으면 생성기 실행만), IDE 계획 하나, 컴파일 하나다. build-evidence의 저장소 소스 snapshot(전체 해시)과 git 조회도 돈다 — 다른 빌드 증거와 같은 형식을 지키려고 남겼고, MathBench Debug 실측(이 PC, 메타·도구 최신): `build --files` 한 파일 약 3.2–3.4초(컴파일 포함), 같은 상태의 0 action 빌드 약 6.8초, 그중 소스 snapshot은 약 0.7초다. 줄이려면 evidence에 snapshot 생략 모드를 더해야 해서(공용 `BuildEvidence`) 이번에는 하지 않았다(17절 L22).
- **NMake.** 엔진·게임 프로젝트의 Windows 행에 `NMakeCompileFileCommandLine`을 **`NMakeCompile` item 메타데이터**로(행마다 `ItemDefinitionGroup`) 내보낸다 — Visual Studio 18 `Microsoft.MakeFile.targets`는 `%(NMakeCompile.NMakeCompileFileCommandLine)`만 읽고, 속성으로 두면 MSB8005 경고만 내고 아무것도 실행하지 않는다(개정 18, UE `VCProject.cs:2558-2563`도 ItemDefinitionGroup). 명령: `… IdeBuild.bat -Action Compile -WorkingDir "$(MSBuildProjectDirectory)" -Files "$(SelectedFiles)" …`(나머지 인자는 Build와 같다). `IdeBuild.ps1`은 `-Files`를 `;`로 나눠 각 항목을 `-WorkingDir` 기준 절대 경로로 풀어 `--files`로 넘긴다. Android 행과 SDK 헤더 프로젝트에는 내보내지 않는다.
- **시험 경로 표시(개정 21).** 시험은 Visual Studio MSBuild 경로와 대체 경로(평가한 명령을 cmd로) 중 어느 쪽을 탔는지 한 줄 출력한다. `ROCKS_REQUIRE_VS_MSBUILD=1`이면 Visual Studio 18 MSBuild가 없을 때 실패한다(Visual Studio 18이 있는 개발 PC·CI 이미지용 — GitHub 기본 이미지에는 Visual Studio 18이 없어 CI에서는 켜지 않는다).
- **시험.** IntegrationTests V6(엔진 MathBench)와 게임 프로젝트 파일 시험이 `SelectedFiles`를 프로젝트 디렉터리 상대 경로로 주어 평가한 `NMakeCompileFileCommandLine`을 cmd로 돌린다: object가 생기고, 기록한 인자가 빌드한 compile_commands.json의 그 소스 명령(`/Fo` 제외)과 같고, `native-cache.json`·action-report가 byte 그대로이며, 뒤의 터미널 빌드는 0 action이다. 헤더를 고르면 실패한다.

## 8. IntelliSense

### 8.1 원천: IDE 계획 모드

IntelliSense는 빌드 계획에서 만든다. `compile_commands.json` 파일을 다시 읽지 않는다. 지금 계획은 생성물이 있어야 하고 출력에 먼저 쓰므로(1절) 다음을 PJ0에서 만든다.

- **부작용 없는 계획 함수.** `Program.cs`의 `build` 경로와 `InstalledSdk.BuildGameCoreAsync`(`InstalledSdk.cs:283-450`)에서 규칙 평가·기술자 해석·`MetadataLayout.Apply`·`NativeBuildRequest` 구성을 순수 함수로 떼어 낸다. 이 함수는 디렉터리를 만들거나 잠금을 잡거나 보고서를 쓰지 않는다(지금 `Program.cs:196-201, 288-291`에서 하는 일은 호출자에 남는다). `build`·`game`·`projectfiles`가 같은 함수를 쓴다.
- **IDE 계획 모드.** `NativeBuildRequest.PlanMode = Ide`이면 `ActionGraph`가 compile 논리 명령과 실행 파일 출력 경로만 만든다. 공개 진입점은 `IdePlanner.Plan`이고, `NativeBuildEngine.BuildAsync`는 이 모드를 거부한다. 엔진 쪽 계획 함수는 `EnginePlanning`이고 게임 쪽은 PJ5에서 나눈다.
  - 리플렉션 생성 소스(`ReflectionSources`)와 BuildInfo·모듈 표·`PublicHeaderChecks` 생성 TU는 논리 명령을 만들지 않는다(IntelliSense 대상 파일이 아니다).
  - 소스 존재 검사(`ActionGraph.cs:250-255`)는 손으로 쓴 소스에만 하고, include 디렉터리 존재 검사(`:258-263`)는 하지 않는다. 출력 아래 include 디렉터리(`<out>/Generated/<M>`)는 경로만 적는다.
  - archive·link·prebuilt 확인(`:540-543`)·ASan thunk 확인(`:573-574`)은 만들지 않는다. PCH 계획은 논리 명령의 PCH 강제 include를 정하려고 그대로 세운다(만들어진 PCH action은 쓰지 않고 버린다). 정의 헤더 내용(`PlannedFile`)은 만든다. 쓰지는 않는다.
  - 논리 명령은 PCH 헤더를 따로 싣는다(`IdeCompile.PchHeader`). 정의 헤더도 PCH 헤더도 아닌 강제 include는 분류 오류다.
  - 유니티와 작업 파일 분리는 끈다(git을 부르지 않는다). 논리 명령은 유니티와 무관하다(`Programs/Rocks.Build/README.ko.md` 유니티 빌드 절). PCH는 켠다. 그래야 PCH 헤더가 강제 include 목록에 나온다.
- **쓰는 것:** `NativePlan.LogicalCompiles`(모듈·소스·인자·강제 include, `ActionGraph.cs:88-89`), 정의 헤더 경로와 내용, 실행 파일 출력 경로, 도구 모음.
- **같음의 보장:** 같은 요청을 일반 모드로 계획했을 때의 손으로 쓴 소스 논리 명령과 IDE 모드의 논리 명령이 같다(16절 V1). 다른 점은 검사와 생성 action뿐이다.
- `compile_commands.json` 파일을 쓰지 않는 이유: 모듈 이름이 없고(`CompilationDatabase.cs:13-18`), 빌드한 구성 하나만 있고, 그 구성을 빌드하기 전에는 없다.

계획은 Windows 타깃 × 구성마다 한 번이다(지금 15 × 4 = 60회. Android 계획은 6.3절에 따라 만들지 않는다). 규칙 어셈블리는 한 번만 올라가고 `LoadAll`이 문맥마다 `Configure`를 다시 부른다(`RuleLoader.cs:47-56`). 도구 모음 탐색은 한 번이다. 계획끼리는 독립이라 병렬로 돈다. 소요 시간은 PJ1에서 재서 기록한다. 목표는 엔진 모드 전체 10초 이하이고, 넘으면 원인을 측정으로 찾는다.

### 8.2 인자 분류표

`IntelliSenseArguments.Classify`가 논리 명령의 인자를 하나씩 분류한다.

| 분류 | 처리 |
|---|---|
| 경로(`/I`·`/FI`) | 파일별 속성으로 모은다 |
| 정의(`/D`) | 파일별 `PreprocessorDefinitions` 전체 목록에 넣는다 |
| 옮김(표의 `/std:`·`/MD`·`/GR-` 등) | 대응 철자로 `AdditionalOptions`에 넣는다 |
| 버림 | 표에 근거와 함께 적은 인자만 버린다 |

표에 없는 인자는 **생성 오류**다(영어 문구, 인자와 모듈 이름을 적는다). 빌드 쪽에 새 인자가 생기면 IntelliSense에 의미가 있는지 사람이 정해 표에 넣는다. 조용히 버리면 빌드와 IntelliSense가 모르게 갈라진다. 4절 표가 첫 판이다. IntelliSense는 Windows 계획만 쓰므로 GNU 철자(clang++)는 표에 넣지 않는다.

버린 인자가 IntelliSense가 보는 매크로를 바꾸지 않는다는 전제는 정책으로 지킨다. `/O2`가 정의하는 clang의 `__OPTIMIZE__`처럼 최적화 인자가 만드는 미리 정의 매크로(`__OPTIMIZE__`·`__OPTIMIZE_SIZE__`·`__NO_INLINE__`)를 제품 소스에서 쓰면 정책 검사 `ide-invisible-macro`가 거부한다. 지금 저장소에 쓰는 곳이 없는지는 PJ0에서 확인한다.

### 8.3 파일별 속성과 Makefile 프로젝트의 합성 규칙

UE처럼 늘 파일별이다(`bUsePerFileIntellisense = true`가 기본, `VCProjectFileGenerator.cs:84`). 쓰는 방식은 Visual Studio Makefile 프로젝트의 IntelliSense 합성 규칙에 맞춘다.
- 항목 메타데이터 `ForcedIncludeFiles`·`PreprocessorDefinitions`는 NMake 기본값(`NMakeForcedIncludes`·`NMakePreprocessorDefinitions`)에 **더해지지 않고 바꾼다**(`Microsoft.Makefile.props:15-19`).
- `IncludePath`(= `$(NMakeIncludeSearchPath);…`)는 항목 `AdditionalIncludeDirectories`에 **더해진다**(`Microsoft.Cpp.DesignTime.targets:249-251`). 그래서 어떤 묶음의 include를 `NMakeIncludeSearchPath`로 올리면 다른 묶음의 파일도 그 `/I`를 본다.

이에 따라:
- 파일 f의 IntelliSense 묶음은 (include 전체 목록, 정의 전체 목록, 강제 include 전체 목록, 옮긴 옵션)이다. 차이분이 아니라 언제나 전체 목록이다(UE도 전체 목록을 쓴다, `VCProject.cs:1416-1420, 1582`).
- 생성기는 `Microsoft.Cpp.props` import 뒤에 `ItemDefinitionGroup`(구성 조건)의 `<ClCompile>` 기본값으로 프로젝트에서 가장 많은 파일이 쓰는 묶음의 네 값을 직접 쓴다. 그 묶음의 파일에는 항목 메타데이터를 달지 않는다.
- 다른 묶음의 파일은 항목에 네 메타데이터를 모두 단다. 값은 조건 없는 `$(RocksIncludes_<n>)`·`$(RocksDefines_<n>)`·`$(RocksForcedIncludes_<n>)`·`$(RocksOptions_<n>)`이고, 속성 값은 구성 조건(`'$(Configuration)|$(Platform)'=='…'`)이 붙은 `PropertyGroup`에 적는다. 묶음 key는 그 파일의 프로젝트 구성별 네 값 전체(정규 텍스트)이고, 번호는 key의 ordinal 정렬 순서다. 구성과 무관한 번호다. 구성마다 파일 목록을 되풀이하지 않는다. UE도 공통 값을 접는다(`VCProject.cs:1366-1425, 1520-1625`).
- `NMakeIncludeSearchPath`·`NMakeForcedIncludes`·`NMakePreprocessorDefinitions`는 비운다. `IncludePath`에는 시스템 include만 둔다(8.5절).
- Android 구성의 속성 값은 같은 `<C>` Windows 값과 같다(6.3절).
- 헤더(`ClInclude`)에는 설정을 달지 않는다. Visual Studio가 헤더를 include하는 번역 단위의 설정을 빌려 쓴다.

Foundation Debug 기준 묶음은 45개, 파일은 1,369개다(4절). 속성 정의는 `묶음 수 × 구성 수`이고 파일 항목은 한 번이다.

### 8.4 정의 헤더와 PCH

**정의 헤더.** 논리 명령의 강제 include 중 `<out>/Definitions/` 아래 경로는 같은 내용의 사본 `out/ProjectFiles/Definitions/<내용 SHA-256 앞 16자>.h`로 바꾼다. 내용은 계획의 `PlannedFile.Content`(`ModuleDefinitions.Content`, `Pch.cs:73`)이므로 빌드가 쓰는 파일과 byte가 같다(리뷰어 확인: 사본 내용에 절대 경로가 없다). 순서도 논리 명령 그대로다([정의 헤더] → [PCH 헤더] → [추가 정의 헤더], UnityBuildPch UD16). 내용 주소라 구성·타깃이 달라도 같은 내용은 파일 하나다.
- 빌드 출력의 정의 헤더를 바로 가리키지 않는 이유: 그 타깃·구성을 빌드(또는 `--plan-only`)하기 전에는 없고, 쓰려면 그 출력의 잠금이 필요하다.
- `/D` 목록(`PreprocessorDefinitions`)으로 풀지 않는 이유: API 매크로 값이 비었거나(`#define ROCKS_CORE_API `) `__declspec(dllexport)`·`__attribute__((visibility("default")))`처럼 괄호·따옴표를 담는다. MSBuild 항목 구분자(`;`)와 이스케이프 규칙을 따로 다뤄야 하고, 강제 include 순서의 의미도 빌드와 달라진다.

**PCH.** 논리 명령은 PCH 바이너리 대신 PCH 헤더를 강제 include한다(UnityBuildPch UD15). 그대로 `ForcedIncludeFiles`에 두고, PCH 헤더를 쓰는 묶음은 `AdditionalOptions`에 `/Yu"<PCH 헤더 경로>"`를 더한다. IntelliSense가 그 헤더까지를 자체 PCH(iPCH) 경계로 쓴다. UE와 같다(`VCProject.cs:1385, 1611`). 효과는 V11에서 확인한다.

### 8.5 clang-cl 모드와 도구 모음 props

UE는 clang으로 빌드할 때도 IntelliSense를 MSVC 컴파일러 기준으로 만든다(`GetCompilerForIntellisense`, `VCProjectFileGenerator.cs:259-266`, 도구 모음 `v14x`). Rocks는 따르지 않는다. `Base/Compiler.h:15-16`이 `__clang__`이 없으면 `#error`이므로 MSVC 모드에서는 모든 파일이 오류다. `__clang__`을 정의에 끼워 넣는 것은 판정을 속이는 것이고, IntelliSense가 MSVC 문법·내장 함수 규칙으로 해석하는 것도 그대로 남는다.

- 프로젝트의 `PlatformToolset`은 `ClangCL`이다. Visual Studio IntelliSense가 clang-cl을 흉내 낸다(`__clang__`·`__clang_major__` 정의, clang 확장 수용 — V11에서 확인).
- **import 순서.** `Rocks.Toolchain.props`(`LLVMInstallDir`·`LLVMToolsVersion`·`RocksCompiler`)는 `Microsoft.Cpp.Default.props` 앞에 import한다. 도구 모음 props가 이 값을 읽기 때문이다. `Rocks.ToolchainPaths.props`(`IncludePath`·Asan 디버거 환경)는 `Microsoft.Cpp.props` 뒤에 import한다. 그 전에 쓰면 VC props가 기본값으로 덮는다.
- `LLVMInstallDir`는 컴파일러의 resource 디렉터리(`/clang:-print-resource-dir`, `NativeEnvironment.cs:193-205`와 같은 조회, `<LLVM>/lib/clang/<주 버전>`)에서 거슬러 올라간 LLVM 루트이고, `LLVMToolsVersion`은 그 `<주 버전>`이다. 조회 결과가 이 모양이 아니면 생성 오류다(4절의 비표준 clang-cl 사례).
- `IncludePath`는 [컴파일러 resource include(`<LLVM>/lib/clang/<주>/include`)] 다음 [도구 모음 환경의 `INCLUDE`(VsDevCmd가 만든 MSVC·Windows SDK include, `ROCKS_MSVC_TOOLSET`으로 고른 판)]으로 통째로 쓴다. 기존 값에 붙이지 않는다. 덮어쓰므로 ClangCL props가 넣는 resource include(`Microsoft.Cpp.ClangCl.Common.props`)를 직접 넣어야 `<intrin.h>`·`<immintrin.h>`·`<stdatomic.h>`가 빌드와 같은 헤더로 풀린다. `ExternalIncludePath`는 `INCLUDE`와 같게 쓴다 — 비워 두면 `Microsoft.Cpp.Current.targets:58-71`이 Visual Studio 기본값(다른 MSVC 판 STL, UnitTest include, SDK 중복)을 뒤에 붙인다. Visual Studio 기본값을 쓰면 빌드와 다른 STL 판을 볼 수 있다(STL 14.52는 Clang 22 미만을 거부한다). UE도 시스템 include를 명시한다(`DefaultSystemIncludePaths`, `VCProjectFileGenerator.cs:640`, `VCProject.cs:1422`).
- `ClangCL` 도구 모음 정의는 Visual Studio 구성 요소 `Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset`이 준다. 이 구성 요소가 없어도 NMake 빌드 자체는 된다(`Microsoft.MakeFile.targets:43`). IntelliSense는 제대로 돌지 않는다. 생성기가 vswhere(`-requires` 그 구성 요소)로 확인하고 없으면 경고한다. 생성은 계속한다.
- 언어 표준은 4절 표대로 `/std:c++23preview`다. ClangCL 모드 IntelliSense가 받지 않으면(V11) `/std:c++latest`로 바꾸고 이 절을 고친다. 빌드는 `/clang:-std=c++23`이고, Visual Studio의 `LanguageStandard` 칸에는 C++23 고정 값이 없어 `AdditionalOptions`로 준다.

### 8.6 생성 헤더

리플렉션 생성 헤더(`<out>/Generated/<M>/…/*.generated.h`)는 **그 타깃·구성의 출력**에서 메타 생성이 돈 뒤에만 있다. IntelliSense include 경로는 그 파일의 원천 명령(6.2절)을 만든 타깃의 출력을 가리킨다. 예를 들어 `Rocks` 프로젝트의 엔진 파일은 `Engine` 타깃 출력의 `Generated/`를 가리키므로, `Foundation`만 빌드한 상태에서는 엔진 파일의 `#include "X.generated.h"`가 IntelliSense 오류다. 원인은 "그 타깃을 빌드하지 않음"이다.

권장안(PJD36): 생성기는 메타 생성을 돌리지 않는다. 생성이 끝나면 원천 명령이 가리키는 생성 include 디렉터리 중 없는 것을 모아 타깃·구성별로 한 줄씩 알린다: `note: reflection headers for <n> files are not generated yet; build target <T> (<C>) once.` 버린 안은 생성기가 MetaCompiler만 `out/ProjectFiles/Generated/<타깃>-<구성>`에 돌리는 것이다. 타깃 × 구성마다 clang AST 실행이 들어 생성이 느려지고, 빌드 출력과 별도의 생성 헤더 트리가 생겨 빌드와 IntelliSense가 다른 생성 헤더를 볼 수 있다(18절).

## 9. 재생성

프로젝트 파일이 낡는 원인은 소스·헤더 추가·삭제, 모듈·타깃 규칙 변경(include·정의·의존), 도구 모음 변경(LLVM·MSVC 판·컴파일러 경로), 타깃 추가, SDK 연결 변경(게임)이다. 헤더 내용 변경은 해당하지 않는다.

- 재생성은 수동이다(UE와 같다). `GenerateProjectFiles.bat`, 셸 메뉴로 돌고, `new-project`·`associate` 끝에서 자동으로 한 번 돈다.
- 생성기는 `ProjectFiles.state.json`에 (타깃, 구성)마다 지문을 적는다. 지문은 그 계획에서 뽑은 IntelliSense 모델 조각(대상 파일 목록과 각 파일의 묶음 값, `NMakeOutput`)의 정규 텍스트 SHA-256과 두 도구 모음 props 내용의 SHA-256이다.
- `build`·`game`은 계획을 만든 뒤 같은 지문을 계산해 상태 파일의 값과 비교한다. 이미 만든 계획에서 뽑으므로 파일을 더 읽지 않는다. 거짓 경고를 막으려고 생성기와 **같은 필터 함수**(`IntelliSenseModel.Select`)를 쓴다. 손으로 쓴 소스의 논리 명령만 보고(리플렉션 생성 소스·생성 TU·`PublicHeaderChecks` 제외), 유니티·작업 파일 분리는 논리 명령에 영향이 없다. 사용자가 `--pch off`로 빌드하면 논리 명령의 강제 include가 생성기 모델(PCH 켬)과 다르므로 비교하지 않는다. `--platform android` 빌드도 비교하지 않는다.
- 상태 파일이 있고 값이 다르면 한 줄 경고한다: `warning RPF0001: Visual Studio project files are out of date for <target> <configuration> (<n> files added, <m> removed, settings changed). Run GenerateProjectFiles.bat.` MSBuild 정규 형식이라 Visual Studio 오류 목록에 나온다. 상태 파일이 없으면 아무것도 하지 않는다. 늘 이 경고 형식이다. 저장소 `Directory.Build.rsp`는 `-warnAsError`이므로 `-warnNotAsError:RPF0001`을 함께 두어 경고로 남긴다(개정 21 — 개정 18의 터미널 `note`는 `-tl:on`·`-v:q`에서 보이지 않았고, 명령줄 `MSBuild.exe Rocks.slnx`에서는 여전히 오류였다).
- 빌드 중 자동 재생성은 하지 않는다. 빌드 도중 프로젝트 파일이 바뀌면 Visual Studio가 다시 읽기를 묻고 진행 중인 솔루션 빌드가 흔들린다.
- Visual Studio에서 "새 항목 추가"로 파일을 더하면 Visual Studio가 `.vcxproj`를 직접 고친다. 다음 재생성이 계획 기준으로 덮어쓰고, 파일이 모듈 디렉터리에 있으면 그대로 다시 나온다.

## 10. 게임 모드

### 10.1 SDK 연결

`.rocksproject`에 선택 필드 `engineAssociation`(문자열)을 더한다. `fileVersion`은 1 그대로다. 하위 호환을 두지 않으므로 옛 도구가 새 파일을 거부해도 된다. `DescriptorReader.ReadProject`의 허용 키(`Descriptors.cs:189`)와 `Docs/Guides/RepositoryLayout.ko.md` 기술자 표를 함께 고친다.

해석 순서(`SdkAssociation.Resolve`):
1. 값이 경로(`/`·`\`를 담음)면 `.rocksproject` 디렉터리 기준 경로의 SDK다. 그 디렉터리에 `rocks-sdk.json`이 없으면 오류다.
2. 값이 `{GUID}`면 SDK 등록부(10.2절)의 그 ID다. 없으면 등록된 SDK 목록과 함께 오류다.
3. 필드가 없으면 등록 SDK 중 **`Verify`와 호환되는 후보**만 남긴다: manifest 판, **요청 구성**(manifest 14의 `Configurations`에 요청 구성이 있음 — 개정 10·11), 플랫폼, 아키텍처, 컴파일러·SDK ABI가 이 컴퓨터의 도구 모음과 맞고, 엔진 주·부가 `engineVersion`과 같은 것. 같은 경로(디스크 철자, 호스트 대소문자 규칙)를 두 ID로 등록했으면 하나로 본다(UE `DesktopPlatformWindows.cpp:345-372`도 중복 경로를 지운다). 그중 패치가 가장 높은 것 하나를 고른다(같은 주·부 패치끼리는 서로 읽는다, `Build/README.md` 엔진 버전 절). 최고 패치가 둘 이상이면 모호하다는 오류와 함께 `associate`를 안내한다. 후보가 없으면 버린 후보마다 이유를 적은 오류다.
4. 명령에 `--sdk`를 주면 위를 건너뛴다(CI·시험). 그래서 `game --sdk`(`Rocks.proj` `/p:SdkDirectory`)는 선택이다. 고르면 `Installed SDK: <경로> (…)` 한 줄을 낸다(개정 9).

고른 SDK는 지금처럼 `Verify`를 거친다. 요청 구성이 SDK에 있는지는 구성마다 본다(10.3절). 생성기는 이 검사를 생성 때 먼저 돌려 빌드 전에 알린다.

UE는 `EngineAssociation`이 비면 상위 디렉터리에서 엔진을 찾고, 경로면 그 엔진, 아니면 등록 식별자다(`DesktopPlatformBase.cpp:454-500`). 설치 빌드는 "5.8" 같은 버전 문자열로 연결한다. Rocks는 버전 문자열 대신 이미 필수인 `engineVersion`을 쓴다. 게임은 언제나 설치 SDK로 빌드하므로(엔진 원본으로 게임을 빌드하는 경로가 없다) 상위 디렉터리 탐색은 두지 않는다. 등록 ID를 프로젝트에 쓰면 컴퓨터마다 달라 공유 저장소에서 깨진다. 그래서 `associate`가 경로·ID를 쓸 때 그 사실을 알린다. UE도 foreign 프로젝트가 아니면 연결을 비워 이식성을 지킨다(`DesktopPlatformBase.cpp:432-447`).

`associate --project <.rocksproject 또는 그 하나만 있는 디렉터리> (--sdk <경로> | --id <ID> | --version on)`이 필드를 쓰거나 지운다(`--version on`은 필드를 지운다 — CLI 옵션은 모두 값을 받으므로 `on`을 적는다). `--sdk`는 프로젝트 디렉터리 기준 상대 경로(`/` 구분, 구분자가 없으면 `./`를 붙임)로, `--id`는 등록부의 철자로 쓴다(ID 비교는 대소문자를 가리지 않는다). 게임 프로젝트 파일이 있으면 다시 만든다 — 게임 생성기가 PJ5이므로 그때 더한다. 기술자의 다른 필드와 순서는 그대로 두고 그 키만 고친다.

### 10.2 SDK 등록부

| 호스트 | 저장소 | 형식 |
|---|---|---|
| Windows | `HKCU\Software\Rocks\Sdks` | 값 이름 = `{GUID}`, 값 = SDK 절대 경로(REG_SZ) |
| macOS·Linux | `~/.config/Rocks/Sdks.json` | `{ "fileVersion": 1, "sdks": [ { "id": "{GUID}", "path": "…" } ] }` |

- **등록은 명시적으로만 한다.** `sdk` 내보내기의 기본은 `--register off`다. 시험·CI·임시 SDK가 사용자 등록부에 쌓이지 않게 하기 위해서다. 등록은 `sdk --register on` 또는 `sdk-register --sdk <경로>`로 한다. `sdk-unregister --sdk <경로>|--id <ID>`를 둔다. ID는 등록 때 새로 만든다(UE `RegisterEngineInstallation`도 `FGuid::NewGuid`, `DesktopPlatformWindows.cpp:313-330`). 같은 경로를 다시 등록하면 기존 ID를 쓴다.
- 기본이 `off`이므로 기존 `sdk` 호출자(`Rocks.proj`의 `InstalledSdk` 대상, IntegrationTests의 SDK 내보내기, CI 작업)는 바꾸지 않아도 된다. PJ3에서 이 호출자 목록을 다시 확인하고 어느 것도 등록하지 않음을 시험으로 남긴다.
- 읽을 때 `rocks-sdk.json`이 없거나 manifest 판이 다른 항목은 목록에서 빼고 알린다. 등록부를 고치지는 않는다(읽기가 쓰기를 하지 않는다).
- 관리자 권한이 필요 없는 사용자 범위만 쓴다. 기계 범위(HKLM)는 두지 않는다.
- 접근은 `ISdkRegistry`(Windows 레지스트리·JSON 파일 구현)로 감싼다. 시험은 레지스트리 구현에 임시 하위 키(`HKCU\Software\Rocks\Tests\<GUID>`)를 주고 끝에 지운다(16절 V8).
- **쓰기는 엄격하고 남의 항목을 보존한다(개정 10).** 읽기(`List`)는 너그럽다: 파일이 깨졌거나 판을 모르면 빈 목록, 틀린 항목(문자열 칸 없음, 상대 경로, 레지스트리 값이 문자열이 아님)은 빼고 이유를 해석 오류의 이유 목록에 싣는다. 등록·해제는 파일을 엄격하게 읽어, 깨졌거나 판을 모르면 아무것도 쓰지 않고 `Next: fix or delete <파일>` 안내와 함께 오류다. 읽을 수 없는 항목과 모르는 칸은 그대로 두고 다시 쓴다(새 판·다른 도구가 둔 항목을 지우지 않는다). 레지스트리의 문자열이 아닌 값도 ID로 지울 수 있다.
- **잠금(개정 10, 개정 12·14에서 바꿈).** 등록·해제의 읽기-고치기-쓰기는 잠금 파일 안에서 한다(두 CLI가 동시에 등록해도 항목을 잃지 않는다). JSON 등록부는 `<등록부>.lock`, Windows 레지스트리 등록부는 `%LOCALAPPDATA%\Rocks\SdkRegistry.lock`을 공유 없이(`FileShare.None`) 연 핸들이 잠금이다 — 세션·프로세스와 무관하게 같은 파일을 쓰는 모든 CLI 사이에서 배제하고, 프로세스가 죽으면 운영체제가 닫는다. 이름 있는 mutex는 쓰지 않는다(`Local\`은 로그온 세션, macOS·Linux .NET은 `getsid` 세션 범위이고, `Global\`은 다른 사용자·높은 무결성 프로세스가 먼저 만들면 열 수 없다). 다시 열어 보는 것은 다른 핸들이 쥔 경우뿐이다: Windows 공유 위반 `0x80070020`·`0x80070021`, Unix flock 경합(`EWOULDBLOCK` — Linux 11, macOS 35). 2분이 넘으면 `Next:` 안내와 함께 실패한다.
- JSON 등록부는 중복 키를 `JsonException`으로 읽는다(`AllowDuplicateProperties = false`). 허용하면 `JsonObject` 접근이 `ArgumentException`을 던져 너그러운 읽기가 해석 도중에 죽었다(개정 12).
- 해석 오류의 이유는 "registered but missing"(등록됐지만 `rocks-sdk.json`이 없음, `sdk-unregister --id` 안내)과 판·구성·도구 모음 불일치를 구별해 적는다.
- 환경 변수 `ROCKS_SDK_REGISTRY`에 파일 경로를 주면 호스트와 무관하게 그 JSON 파일이 등록부다(개정 9). IntegrationTests는 CLI 하위 프로세스에 이 변수를 주어 사용자 등록부를 건드리지 않고 "내보내기는 등록하지 않는다"와 등록·`--sdk` 없는 게임 빌드를 끝까지 돌린다. UE는 같은 목적의 재정의가 없다(시험이 사용자 레지스트리를 쓴다).

### 10.3 다중 구성 SDK

게임 솔루션은 Debug·Development·Release를 함께 가져야 한다. 구성마다 CRT(`/MDd`·`/MD`)와 구성 매크로가 달라 엔진 라이브러리를 섞을 수 없다. 그래서 SDK 하나가 여러 구성을 담는다.

- `sdk --configurations Debug,Development,Release`(기본은 이 셋, `Asan`은 지정할 때만)가 구성마다 엔진을 빌드한 뒤 SDK 하나로 조립한다.
- 배치: 구성과 무관한 부분(공개 헤더, `Tools/`, `Licenses/`, `Templates/`(PJ6), `Build/` 스크립트, `global.json`·`.clang-format`)은 한 벌이고, 구성마다 다른 부분(라이브러리, 메타데이터 descriptor·identity 명세 사본, 모듈 칸)은 `Configurations/<구성>/` 아래에 둔다. 한 벌로 둔 파일이 구성마다 다르면 내보내기 오류다(조립 때 내용 해시로 확인한다).
- manifest: `Configuration`(문자열)을 `Configurations`(구성 이름 → 파일 목록·모듈 목록)로 바꾸고 `ManifestVersion`을 13에서 14로 올린다(`InstalledSdk.cs:61`). `Verify`는 요청 구성이 목록에 있는지 본다.
- BuildTargets BD5의 SDK 변형(매크로 조합 × 링크 방식)과는 직교한다. 변형 축이 생기면 `Configurations/<구성>/<변형>/`으로 넓힌다.
- 게임 솔루션의 구성은 연결한 SDK가 가진 구성뿐이다. 게임 생성기(PJ5)는 manifest 14의 `Configurations`를 읽으므로 이 절(PJ4)이 먼저다.

구현(PJ4, 개정 11):
- **명령.** `sdk --configurations <목록>`(`+`·쉼표·세미콜론, 이름은 정확히, 중복 거부, 구성 순서로 정렬, `NativeConfiguration`의 모든 이름 — Asan·Coverage도 적으면 받는다). `sdk`는 `--configuration`을 받지 않는다(안내 오류). `--output`은 구성이 하나일 때만 받는다 — 구성마다 기본 출력 `out/dotnet-<platform>-<target>-<구성>`과 그 증거를 쓴다. `Rocks.proj` `InstalledSdk`는 `SdkConfigurations`(기본 `Debug+Development+Release`)를 넘긴다. MSBuild `-p:`는 쉼표·세미콜론을 따옴표 안에서도 속성 구분자로 읽어(MSB1006, pwsh·cmd 모두) `-p:SdkConfigurations=Debug+Release`로 쓴다(개정 12). `/t:InstalledSdk`는 `Configuration`을 쓰지 않으므로 Debug가 아니면 경고한다.
- **흐름.** `SdkExportCommand`가 대상 검사(새 경로, 루트 아님) 뒤 `SdkAssembly`(대상 옆 stage)를 만들고 구성마다 같은 `sdk` 빌드 흐름(`BuildCommand.RunAsync`)을 `--configuration <C>`로 부른다. 그 흐름의 끝에서 `SdkAssembly.AddConfigurationAsync`가 구성 stage에 지금까지의 배치(헤더·라이브러리·메타데이터·라이선스·도구·스크립트)를 만들고 합친다. 모든 구성이 성공하면 `Publish`가 manifest를 쓰고 대상으로 옮긴다. 하나라도 실패하면 stage를 지우고 대상은 생기지 않는다. `--register on`은 게시 뒤 등록하고, 등록이 실패하면 SDK는 게시된 채 `sdk-register`로 다시 등록하라고 알린다(개정 12).
- **입력 일관성(개정 12).** 구성마다 빌드 시작의 저장소 snapshot(build-evidence `sourceBefore` — 규칙·소스·스크립트)이 첫 구성과 같아야 더하고, 게시 직전에 다시 찍은 snapshot도 같아야 게시한다. 구성 빌드 사이에 소스를 고치면 서로 다른 소스의 라이브러리가 한 SDK에 섞이기 때문이다. 시험 hook `ROCKS_SDK_EXPORT_TEST_FAIL_AFTER=<구성>`(`ROCKS_SDK_EXPORT_TEST=1`과 함께일 때만)은 그 구성을 더한 뒤 실패시켜 원자성을 실제 명령으로 시험한다.
- **합치기.** 최상위가 `Libraries/`·`Metadata/`인 파일은 `Configurations/<C>/` 아래로 옮기고 그 구성의 파일 목록에 넣는다. 나머지는 한 벌이다: 처음 본 구성의 파일을 두고, 다음 구성은 내용 SHA-256이 같아야 하며, 한 구성에만 있는 한 벌 파일도 오류다. 생성 공개 헤더(`Headers/<M>/<n>/…generated.h`)와 모듈 include 배치(`Includes` 번호)도 한 벌이다 — 구성에 따라 달라지면 내보내기가 실패하므로 그때 구성별 헤더로 옮긴다(17절 L19). 구성에 따라 다른 것은 라이브러리와 descriptor·identity 명세뿐이다(정의 헤더·`compile_commands.json`·BuildInfo는 게임 빌드가 다시 계산한다).
- **manifest 14.** `{ Version: 14, EngineVersion, Configurations: { "<C>": { Modules: [...], Files: ["Configurations/<C>/…"] } }, Platform, Architecture, CompilerIdentity, SdkIdentity, Files }`. 모듈 선언은 구성마다 싣는다(규칙이 구성에 따라 정의·의존을 바꿀 수 있다). `Libraries`·`Descriptor`·`IdentityManifest`는 그 구성 디렉터리 아래 경로이고 `Includes`는 한 벌 `Headers/` 경로다. 모든 구성은 같은 플랫폼·ABI·엔진 버전이어야 한다(조립 때 확인).
- **Verify.** 판·엔진 버전·플랫폼·아키텍처·ABI·파일 목록은 SDK 전체에 대해 보고, 내용 해시는 한 벌 파일과 요청 구성의 파일만 본다(다른 구성의 라이브러리는 이 빌드가 읽지 않는다, 개정 12). 게임 빌드는 시작할 때 한 번만 해시하고, 그때 해시 전에 기록한 (크기, 수정 시각)과 파일 목록으로 빌드 끝에 바뀌지 않았는지 본다(`VerifiedSdk`). 그 기록 없이 부르는 `PublishVerifiedGameResult`는 전체를 검증한다. 요청 구성이 `Configurations`에 없으면 가진 구성 목록과 함께 거부한다. 구성마다 파일 목록이 `Configurations/<C>/` 아래이고 그 아래 실제 파일과 같아야 하며(모르는 구성 디렉터리도 거부), 요청 구성의 모듈이 그 구성 파일만 가리키는지 본다. 등록부 해석의 호환 조건(10.1절)도 `Configurations`에 요청 구성이 있는가로 바꿨다.

UE 설치 빌드도 여러 구성의 엔진 바이너리를 함께 싣는다. 구성마다 SDK를 따로 두고 구성별 연결 표를 두는 안은 버렸다(18절).

### 10.4 게임 솔루션

| 파일 | 위치 |
|---|---|
| 솔루션 | `<project>/<이름>.slnx`(`<이름>` = `.rocksproject` 파일 이름) |
| 게임 프로젝트 | `<project>/out/ProjectFiles/<이름>.vcxproj` |
| 타깃 프로젝트 | `<project>/out/ProjectFiles/Programs/<타깃>.vcxproj` |
| SDK 프로젝트 | `<project>/out/ProjectFiles/RocksSdk.vcxproj` |
| props·정의 사본·중간 파일·상태 | 엔진 모드와 같은 이름, `<project>/out/ProjectFiles/` 아래 |

- 게임 프로젝트: 정의 디렉터리가 프로젝트 루트인 타깃이 구성이다(`Game` 종류는 `<C>`, 예정인 Editor·Server는 `<C> Editor`·`<C> Server`). 하위 디렉터리에 정의된 타깃은 자기 프로젝트를 가진다(6.2절 규칙). 파일 소유와 IntelliSense 원천도 6.2절 규칙 그대로다(루트가 `Core/`·`Engine/` 대신 프로젝트 루트).
- `RocksSdk`: SDK 공개 헤더를 `ClInclude`로만 담는 탐색용 프로젝트다. NMake 명령은 `@rem Nothing to build.`이고 솔루션 빌드에서 뺀다. UE foreign 프로젝트 솔루션도 엔진 프로젝트를 함께 넣는다.
- NMake: `"$(RocksSdkRoot)Build\IdeBuild.bat" -Action Build -Project "$(RocksProjectFile)" -Sdk "$(RocksSdkRoot)." -Target <T> -Configuration <C> -Compiler "$(RocksCompiler)"`. 게임 모드 `IdeBuild.ps1`은 도구를 빌드하지 않고, 같은 IDE 빌드 잠금 안에서 SDK의 `Tools/Rocks.Build.Cli/Rocks.Build.Cli.dll game …`을 부른다. 연결 SDK가 바뀌면 재생성한다(`associate`가 한다).
- `NMakeOutput`은 게임 출력의 실행 파일, `LocalDebuggerWorkingDirectory`는 프로젝트 디렉터리다. 시작 프로젝트는 사용자가 고른다(7.1절 — 직렬화기가 순서를 정한다).
- IntelliSense 원천은 8.1절의 부작용 없는 계획 함수(게임 쪽)를 IDE 계획 모드로 부른 결과다.

구현(PJ5, 개정 13):
- **명령.** `projectfiles --project <게임 디렉터리 또는 .rocksproject> [--sdk <SDK>] [--compiler <clang-cl>] [--model-only on]`(Windows 호스트). SDK는 게임 빌드와 같은 연결 해석(10.1절)으로 고르되 호환 조건에서 구성을 빼고(어느 구성이든 생성은 한다), 고른 SDK를 IDE 구성 중 첫 구성으로 `Verify`한다. 솔루션 구성은 SDK `Configurations` ∩ {Debug, Development, Release, Asan}이고 Android 구성은 없다(SDK 내보내기가 Android를 지원하지 않는다).
- **계획.** 게임 쪽 부작용 없는 계획 함수 `GamePlanning`(`Programs/Rocks.Build.Cli/Game/GamePlanning.cs`)을 게임 빌드(`BuildGameCoreAsync`)와 생성기가 함께 쓴다: 규칙 어셈블리 컴파일(`CompileRulesAsync` — 생성기는 `<project>/out/ProjectFiles/Rules`, 빌드는 `<출력>/Rules`), 기술자·플러그인 적용과 SDK 모듈 합치기(`Layout`), 요청(`Request`). 생성기는 메타 배치를 `createDirectories: false`로 부르고 출력·잠금·보고서를 만들지 않는다. 타깃 정의 디렉터리는 규칙 탐색 파일(`GameProjectLayout.RuleFiles`)의 `*.Target.cs`에서 찾는다(`TargetDefinitions.FromFiles`).
- **모델.** `IntelliSenseModel.Create`에 게임 배치(`IdeModelLayout`: 주 프로젝트 이름, 헤더 전용 프로젝트 이름·루트)를 준다. 주 프로젝트(`<.rocksproject 이름>`)의 구성은 정의 디렉터리가 프로젝트 루트 또는 `<루트>/Source`인 타깃이다(UE 기본 배치는 `Source/<이름>.Target.cs`, 개정 14). 파일 소유는 가장 가까운 정의 디렉터리이고 그것이 주 타깃이면 주 프로젝트다. 주 프로젝트 밖 타깃의 이름이 주 프로젝트 이름이나 `RocksSdk`와 같으면(대소문자 무시) 프로젝트 경로가 겹치므로 타깃 이름을 바꾸거나 루트·`Source/`에 정의하라는 생성 오류다. SDK 아래 헤더는 `RocksSdk` 프로젝트(헤더만, NMake `@rem Nothing to build.`, 솔루션 빌드에서 빠짐)로 간다. `RocksSdk`의 구성은 주 프로젝트의 Game 종류 타깃(없으면 첫 주 타깃), 주 타깃이 없으면 전체 중 첫 Game 종류 타깃(없으면 첫 타깃)의 구성이다.
- **파일.** 공통 props에 `RocksSdkRoot`(SDK, 끝 `\`)와 `RocksProjectFile`(`.rocksproject` 절대 경로)을 더하고 `RocksRoot`는 게임 디렉터리다. 솔루션에서 주 프로젝트는 폴더 밖이고 `RocksSdk`는 `/Rocks/`, 하위 디렉터리 타깃은 `/Programs/` 폴더다. 직렬화기가 폴더를 앞에 쓰므로 주 프로젝트를 첫 항목으로 정할 수 없고, 시작 프로젝트는 엔진 모드처럼 사용자가 한 번 고른다(7.1절 시작 프로젝트). C# 프로젝트는 넣지 않는다. 상태 파일·정리 규칙은 엔진 모드와 같다(`<project>/out/ProjectFiles`, `<project>/<이름>.slnx`).
- **NMake.** 게임 `IdeBuild.ps1`(SDK 안)은 IDE 빌드 잠금(SDK 루트 기준) 안에서 SDK CLI로 Build=`game --sdk --project --target --configuration --compiler`, Clean=`clean --project --target --configuration`, Rebuild=둘을 차례로 한다. 엔진 작업 사본의 `IdeBuild.ps1`에 `-Project`를 주면 `RB0002`다(게임은 언제나 설치 SDK로 빌드한다). `GenerateProjectFiles.ps1 -Project <경로> [-Sdk <SDK>]`는 그 스크립트가 속한 도구의 CLI로 `projectfiles --project`를 부른다(엔진 작업 사본·SDK 모두).
- **associate.** 연결을 바꾼 뒤 게임에 생성 상태 파일(`out/ProjectFiles/ProjectFiles.state.json`)이 있으면 게임 프로젝트 파일을 다시 만든다(생성 때의 컴파일러). 다시 만들기가 어떤 이유로든 실패하면 연결은 바뀐 채 두고, 실패 이유와 `GenerateProjectFiles.ps1 -Project` 안내를 내며 종료 코드 2다(개정 14 — 실패 원인마다 1·0·`RB0001`로 갈렸다).
- **낡은 연결 검사(개정 14).** IDE 빌드(`ROCKS_IDE_BUILD=1`)에서 `game --sdk`는 `.rocksproject`에 `engineAssociation`이 있으면 그것이 해석하는 SDK와 `--sdk`가 같은지 본다. 다르면 "project files are stale" 오류(`RB0001`)로 `GenerateProjectFiles.ps1 -Project`를 안내한다 — 다시 만들기가 실패한 뒤 옛 vcxproj가 옛 SDK로 조용히 빌드하지 않게. 연결이 없으면(등록부 선택) 검사하지 않는다(17절 L21).
- **SDK CLI로 위임(개정 14).** 생성은 고른 SDK의 CLI(`<SDK>/Tools/Rocks.Build.Cli/Rocks.Build.Cli.dll projectfiles --project … --sdk <SDK>`)가 한다. 다른 CLI(엔진 작업 사본·다른 SDK)가 받으면 SDK를 고른 뒤 그 CLI를 하위 프로세스로 부른다. 게임 규칙 어셈블리는 SDK의 `Rocks.Build.dll`을 참조하므로 다른 판의 `Rocks.Build`가 이미 올라온 프로세스에서 읽으면 `MissingMethodException` 등이 난다. UE도 연결된 엔진의 UBT로 생성한다.
- **잠금과 model-only(개정 14).** 생성은 시작부터 `out/ProjectFiles`의 호출 잠금을 쥔다(규칙 컴파일·쓰기 모두 그 안, 동시 생성은 차례로 돈다). `--model-only on`은 규칙을 임시 디렉터리에 컴파일하고 지워, 게임 디렉터리에 아무것도 쓰지 않는다.
- **구성 검증(개정 14).** IDE 구성마다 manifest 구조(모듈 선언·구성 파일 목록)를 검증하고 내용 해시는 한 번만 한다. 생성 뒤 엔진 모드와 같이 ClangCL 구성 요소가 없으면 경고하고, 생성 헤더가 아직 없는 (타깃, 구성)을 안내한다.
- **정리.** 규칙·메타 단계에서 실패한 게임 출력은 호출 잠금만 있다. `clean`은 호출 잠금과 함께 실행 기록(`action-report.json`·`game-build-evidence.json`·`build-evidence.json`)이 있으면 Rocks 빌드 출력으로 받는다. `<루트>/out/ProjectFiles`와 그 아래는 늘 거부한다(생성 영역은 호출 잠금이 있다, 7.3절·개정 14).
- **낡음 경고.** 게임 빌드의 `RPF0001`은 PJ5에서 넣지 않는다(엔진 모드만, 17절 L20).

### 10.5 게임 출력 경로

게임 출력 기본값을 `<project>/out/<platform>-<target>-<configuration>`(소문자, 예: `out/windows-foundationtemplate-debug`)으로 바꾼다. 엔진 출력 이름(`dotnet-<platform>-<target>-<configuration>`, `Program.cs:140-141`)과 같은 순서이고 `dotnet-` 접두만 없다. 타깃이 둘 이상이어도 겹치지 않고 `clean`이 지울 곳이 하나로 정해진다. 템플릿 README와 IntegrationTests의 기대 경로를 같은 변경에서 고친다. 옛 경로는 지원하지 않는다.

### 10.6 입력 snapshot과 규칙 탐색 제외

`GameInputSnapshot.Capture`(`GameInputSnapshot.cs:11-78`)의 열거에서 다음을 뺀다. 지금은 `.git` 디렉터리만 뺀다(`GameInputSnapshot.cs:51-54`). 게임 규칙 파일 탐색(`InstalledSdk.cs:370-372` 앞에서 규칙 어셈블리를 만들 때 프로젝트 아래 `*.Build.cs`·`*.Target.cs`를 모으는 단계)도 **같은 필터 함수**(`GameProjectLayout.IsExcluded`)를 쓴다. 그래야 `out/` 아래 사본이나 `.vs` 안 파일이 규칙으로 컴파일되지 않는다.
- 깊이와 무관하게 이름이 `.`으로 시작하는 디렉터리(`.git`·`.vs`·`.vscode`·`.idea` 등). 엔진 include inventory가 이미 같은 규칙이다(`Programs/Rocks.Build/README.ko.md` 입력과 증분 계약).
- 프로젝트 루트 바로 아래의 `out/`(모든 구성·타깃 출력과 `out/ProjectFiles`)과 `Saved/`(실행 중 크래시 수집이 쓴다, `Templates/Game/Foundation/README.ko.md`).
- 프로젝트 루트 바로 아래의 `*.slnx` 파일(생성 솔루션).

이유: Visual Studio는 `.vs/<솔루션>/…/Browse.VC.db` 등을 쓰기 권한으로 열어 둔다. `File.OpenRead`(`GameInputSnapshot.cs:72`)는 읽기 공유만 허용하므로, 다른 핸들이 쓰기 권한을 가진 파일에서 공유 위반 예외가 난다. 열리더라도 IntelliSense가 빌드 중에 DB를 고치면 시작·끝 snapshot이 달라 빌드가 실패한다(`InstalledSdk.cs:457-458`). 다른 구성의 출력 `out/<다른 구성>`도 지금은 snapshot에 들어가므로, 같은 프로젝트의 두 구성을 동시에 빌드하면 서로를 실패시킨다.

한계: 규칙이 점 디렉터리·`Saved/` 안 파일을 읽으면 그 변경을 잡지 못한다. 규칙의 동적 입력은 명시해야 한다는 기존 계약(`Build/README.md` 엔진 버전 절 끝 문단)으로 다룬다. 명시 입력(`explicitInputs`)은 제외 규칙과 무관하게 그대로 보고, 명시 입력이 디렉터리면 **그 아래 전체**에 제외 규칙을 적용하지 않는다(개정 10 — 명시한 `Saved/`의 파일이 빠지지 않는다).

세부(개정 10):
- `out`·`Saved`·`.slnx` 비교는 호스트와 무관하게 대소문자를 가리지 않는다. macOS APFS 기본 볼륨도 대소문자를 가리지 않아 `saved/`·`OUT/`이 같은 디렉터리다. 점 디렉터리 판정은 첫 글자만 보므로 대소문자와 무관하다.
- 규칙 탐색 `RuleFiles(project, output, sdk)`는 링크를 만나면 따라가지 않고 오류다(입력 snapshot과 같은 규칙). 프로젝트 안에 둔 SDK와 출력 아래로는 내려가지 않는다.
- **링크의 뜻(개정 12).** 링크는 `FileInfo`·`DirectoryInfo.LinkTarget`이 있는 항목(symlink·junction)이다. 다른 reparse point — OneDrive Files On-Demand 자리 표시자(Windows 11 기본 문서·바탕 화면), 데이터 중복 제거 파일 — 는 그 자리의 보통 파일이라 받는다. 입력 snapshot, 규칙 탐색, SDK 패키징·inventory가 같은 판정(`FileLinks.IsLink`)을 쓴다.

## 11. 템플릿 생성

### 11.1 원칙

- 템플릿은 빌드·format·정책 검사를 통과하는 실제 프로젝트다. 자리 표시 토큰(`%PROJECTNAME%` 등)을 소스에 두지 않는다. 토큰은 C# 규칙 컴파일, clang-format, 리플렉션 생성을 모두 깨뜨린다.
- 템플릿 이름은 다른 이름과 우연히 겹치지 않는 고유 문자열이다. 치환은 그 문자열을 새 이름으로 바꾸는 것뿐이다. UE 템플릿도 `TP_ThirdPerson` 같은 실제 이름을 쓰고, `TemplateDefs.ini`의 `%TEMPLATENAME%`은 정의 파일 안의 변수일 뿐이다(`TemplateProjectDefs.cpp:139-145`).
- `Templates/Game/Foundation`의 이름을 `FoundationTemplate`으로 바꾼다: `FoundationTemplate.rocksproject`, `FoundationTemplate.Build.cs`(모듈 `FoundationTemplate`), `FoundationTemplate.Target.cs`(타깃 `FoundationTemplate`), `FoundationTemplate.rocksids.json`, namespace `foundationtemplate`, 진입점 `ROCKS_IMPLEMENT_MODULE(…, FoundationTemplate)`. `GameObject`처럼 템플릿 이름을 담지 않은 이름은 그대로 남는다(namespace가 프로젝트마다 다르다). 디렉터리 이름 `Foundation`은 그대로다.
- 같은 변경에서 고칠 곳(PJ6에서 `SampleGame`·`Game.rocksproject`·`Game` 타깃 철자를 저장소 전체에서 다시 찾아 확정한다):
  - `Rocks.proj`의 `GameTarget` 기본값(`Game`)
  - IntegrationTests(`Rocks.Build.IntegrationTests/Program.cs:170-197`의 `SampleGame`)
  - `Rocks.Build.Tests`의 `DescriptorTests`·`EngineVersionTests`(템플릿 기술자 경로·이름, 엔진 버전과 템플릿 `engineVersion` 일치)
  - `Rocks.ToolTests`의 `SelfTests.Policy.cs`·`SelfTests.EnglishArtifacts.cs`(템플릿 경로를 쓰는 정책 자체 시험)
  - `Build/README.md`, 템플릿 README, 그 밖에 템플릿 이름을 적은 `Docs` 문서 10여 곳

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
- 프로젝트 이름 검사: C++ 식별자, 대문자 시작, 3–32자, SDK 모듈 이름·엔진 예약 이름과 대소문자 무시로 달라야 한다(`InputValidation`의 모듈 이름 규칙과 같다, MD17). 세 철자가 모두 쓰이므로 대문자·소문자 형도 검사한다. 소문자 형은 namespace가 되므로 C++ 키워드, `std`, `rocks`(엔진 namespace), `posix`와 같으면 거부한다. 대문자 형이 엔진 매크로 접두(`ROCKS`)와 같으면 거부한다. 대상 디렉터리는 없거나 비어 있어야 한다. UE도 이름의 문자·길이·예약어를 본다(`GameProjectUtils.cpp:633-790`).

### 11.4 기술자와 ID 재발급

- `.rocksproject`는 치환 뒤 `engineVersion`을 쓰는 SDK의 주·부로 맞추고 `engineAssociation`을 두지 않는다(10.1절 3번 해석). UE도 새 프로젝트의 연결을 비운다(`GameProjectUtils.cpp:1936-1962`).
- identity 명세(`<이름>.rocksids.json`)는 MetaCompiler에 새로 두는 `ids reissue --manifest <경로>`로 다시 쓴다. 활성 항목은 새 무작위 128비트 ID를 받고(`ids add`와 같은 생성기), `aliases`는 비우고, 은퇴 항목은 지운다. 새 프로젝트에는 이력이 없기 때문이다. 명세 형식은 MetaCompiler만 다룬다(`StableIdManifest.cs`, 명령 분기 `IdentityCommands.cs:48-179`). 생성기가 형식을 따로 구현하지 않는다(공용 기능 중복 금지).
- 재발급하지 않으면 같은 템플릿에서 만든 프로젝트·플러그인이 같은 타입 ID를 가진다. 한 빌드에 함께 들어가면 `ids verify-set` 규칙(한 ID는 한 명세에만)에 걸린다.

### 11.5 명령

`new-project --template <템플릿 디렉터리 이름|경로> --name <이름> --destination <부모 디렉터리> [--sdk <경로>] [--projectfiles on|off]`
1. 템플릿을 찾는다. `--template`이 경로 구분자를 담으면 그 디렉터리, 아니면 SDK의 `Templates/Game/<값>`(디렉터리 이름, 예: `Foundation`)이다. SDK 내보내기가 `Templates/`를 싣는다. `templateName`(`FoundationTemplate`)은 치환에만 쓰는 내부 값이고 인자로 받지 않는다.
2. `<부모>/<이름>/`에 `ignore`를 뺀 파일을 이름 치환하며 복사하고 내용을 치환한다. 중간에 실패하면 만든 디렉터리를 지운다(UE도 만든 파일 목록을 지운다).
3. 기술자를 고치고 `ids reissue`를 돈다.
4. 결과 디렉터리 전체(경로·내용)에 템플릿 이름 세 철자가 남지 않았는지 확인한다. 남으면 오류다.
5. Windows에서 `--projectfiles on`(기본)이면 게임 모드 생성을 돈다.

UE는 에디터 대화 상자에서 만든다. Rocks는 에디터 전에도 쓸 수 있도록 CLI 명령으로 두고, 나중의 에디터 UI도 같은 명령을 부른다.

### 11.6 구현(PJ6, 개정 15)

- **템플릿.** `Templates/Game/Foundation`의 이름은 `FoundationTemplate`이다(파일 `FoundationTemplate.rocksproject`·`.Build.cs`·`.Target.cs`·`.rocksids.json`·`Source/FoundationTemplateModule.cpp`, 모듈·타깃 `FoundationTemplate`, namespace `foundationtemplate`). `Template.json`은 11.2절 그대로다.
- **기본 타깃.** 게임 빌드·`clean --project`·생성기는 `--target`이 없으면 `.rocksproject` 이름의 타깃을 쓴다(`GameProjectLayout.DefaultTarget`, UE도 `<프로젝트>.Target.cs`가 기본 게임 타깃). `Rocks.proj`의 `GameTarget` 기본값(`Game`)을 없애고 값이 있을 때만 `--target`을 넘긴다. 템플릿에서 만든 프로젝트는 타깃 이름이 프로젝트 이름이라 따로 적지 않아도 된다.
- **SDK.** `new-project`는 `--sdk`가 없으면 자기 CLI가 든 SDK(`<SDK>/Tools/Rocks.Build.Cli/`)를 쓰고, 엔진 작업 사본의 CLI면 `--sdk`를 요구한다. SDK 내보내기는 `Templates/`를 한 벌 부분으로 싣는다.
- **이름 검사(개정 16·19 보강).** 소문자 형 거부에 Windows `rpcndr.h`·`windef.h` 매크로 `small`·`hyper`·`pascal`·`cdecl`도 더한다(개정 19). 템플릿 이름을 대소문자 무시로 담은 이름(`FoundationTemplateX`)은 치환이 결과를 다시 건드리는 것처럼 보이므로 쓰기 전에 거부한다. Windows 예약 장치 이름(`CON`·`PRN`·`AUX`·`NUL`·`COM0`–`COM9`·`LPT0`–`LPT9`, 대소문자 무시)은 파일·디렉터리 이름이 될 수 없어 거부한다(UE `Paths.cpp:1734-1736`, `FileHelper.cpp:31-33`). 소문자 형은 11.3절 목록에 더해 표준 C 매크로·전역(`errno`·`stdin`·`stdout`·`stderr`), Windows 헤더 매크로(`near`·`far`·`interface`), GNU 사전 정의 매크로(`linux`·`unix`)를 거부한다 — namespace 이름이 매크로로 바뀌면 생성 코드가 깨진다. 11.3절에 더해 예약 이름은 `Rocks`·`RocksSdk`(게임 솔루션의 SDK 헤더 프로젝트, 10.4절)·`Engine`·`Core`·`Std`·`Posix`와 SDK의 모든 구성의 모듈 이름이다(대소문자 무시). 대문자 형 `ROCKS`는 소문자 형 `rocks`와 같은 이름이라 함께 막힌다.
- **복사 대상(개정 16).** 템플릿의 빌드·IDE 산출물(`GameProjectLayout.IsExcluded`: 점 디렉터리, 최상위 `out/`·`Saved/`, 최상위 `*.slnx`)은 SDK 내보내기와 `new-project` 모두 기본으로 뺀다. `Template.json` `ignore`는 파일·디렉터리(그 아래 전부)와 glob(`*`·`?`, 한 경로 성분 안)을 받는다.
- **인코딩(개정 16).** 치환 대상 파일은 엄격한 UTF-8로 읽고(잘못된 바이트는 오류), BOM이 있으면 남기며, NUL 바이트가 있으면 텍스트가 아니라 거부한다.
- **기술자 이름.** 게임 생성기는 `.rocksproject` 이름이 SDK 헤더 프로젝트 이름(`RocksSdk`, 대소문자 무시)이면 거부한다(손으로 만든 프로젝트, 개정 16).
- **model-only 규칙 디렉터리(개정 16).** 규칙 어셈블리는 기본 load context에 올라 파일이 잠기므로 `rocks-game-rules-<pid>-<guid>` 임시 디렉터리를 지우기는 best-effort이고, 실패하면 남긴다. 다음 생성 때 프로세스가 없는 pid의 디렉터리를 지운다.
- **실패.** 복사·기술자·재발급·남은 철자 검사 중 실패하면 만든 디렉터리를 지운다(있던 빈 디렉터리는 비우고 남긴다). 비어 있지 않은 대상은 아무것도 쓰기 전에 거부한다.
- **재발급.** 결과의 모든 `*.rocksids.json`에 SDK MetaCompiler `ids reissue --manifest`를 돈다(`StableIdManifest.Reissue` — `Register`와 같은 생성기, aliases 비움, 은퇴 항목 삭제).
- **프로젝트 파일.** Windows에서 `--projectfiles on`(기본)이면 끝에 게임 모드 생성(10.4절)을 같은 SDK로 돈다(`--compiler`를 넘긴다).
- **시험(V12).** IntegrationTests의 게임 fixture 셋(기본·리플렉션·드라이브 루트)을 `new-project --name SampleGame`으로 만든다 — 모듈·타깃·기술자 이름이 모두 `SampleGame`이 되고 namespace는 `samplegame`이다. 본 흐름이 남은 철자 0, `Template.json`·README 미복사, ID 전부 재발급(템플릿과 다름, 두 프로젝트끼리도 다름), 비어 있지 않은 대상 거부를 단언하고, 그 fixture로 빌드·실행·리플렉션 시험이 그대로 돈다. `GameProjectTests`가 이름 검사 거부 사례, 두 단계·대소문자 구분 치환, 섞인 철자·모르는 필드 거부를 시험한다.

## 12. 진입 스크립트와 셸 통합

| 파일 | 하는 일 |
|---|---|
| `GenerateProjectFiles.bat`(저장소 루트, 새 파일) | `pwsh … -File Build\GenerateProjectFiles.ps1 %*`. 탐색기에서 실행해도 되도록 실패하면 `pause`(UE `GenerateProjectFiles.bat`과 같다). 실행 위치는 판정하지 않는다 — `%cmdcmdline%`는 터미널에서도 `cmd /c "<전체 경로>"`라 구별할 수 없다(개정 10) |
| `GenerateProjectFiles.ps1`(저장소 루트) | PowerShell에서 쓰는 같은 진입점이다(개정 22). 인자(`-Compiler`·`-Project`·`-Sdk`)를 그대로 `Build/GenerateProjectFiles.ps1`에 넘기고 종료 코드를 돌려준다. 터미널용이라 멈추지 않는다. `#Requires -Version 7.0`이라 Windows PowerShell 5.1에서는 바로 거부된다 |
| `Build/GenerateProjectFiles.ps1` | IDE 빌드 잠금 안에서 엔진: 도구 준비 → `projectfiles --root`. 게임(`-Project`): SDK 도구로 `projectfiles --project` |
| `Build/Tools.ps1` | 도구 준비(지문 비교, 다르면 `dotnet build`). `Rocks.proj` `BootstrapTools`도 부른다 |
| `Build/IdeBuild.bat`·`.ps1` | NMake 명령(7.1절) |

- 스크립트는 `Build/` 아래 기존 스크립트처럼 영어다(`build-file-english` 정책). 루트의 `GenerateProjectFiles.bat`·`GenerateProjectFiles.ps1`은 정책의 루트 파일 목록(`Docs/CodingStyle.md` `build-file-english`)에 더한다.
- SDK 내보내기는 `Build/GenerateProjectFiles.ps1`·`Build/IdeBuild.*`·`Build/Tools.ps1`을 `<sdk>/Build/`에 싣는다(파일 해시 목록에 든다). `Tools.ps1`은 루트에 `rocks-sdk.json`이 있으면 SDK 모드다: `Update-RocksTools`는 빌드하지 않고 실린 CLI(`Tools/Rocks.Build.Cli/Rocks.Build.Cli.dll`)가 있는지만 보며, `$RocksCli`가 그 경로다(엔진 모드는 `out/dotnet/bin/…`). `IdeBuild.ps1`·`GenerateProjectFiles.ps1`의 게임 모드(`-Project`)는 게임 생성기(PJ5)와 함께 들어가므로, 그 전에는 SDK 안에서 `error RB0002`로 멈춘다(개정 9). 게임 호출 형태(`-Project`·`-Sdk`, `-Platform` 없음)가 매개변수 바인딩 오류 대신 이 줄을 내도록 두 매개변수를 미리 선언하고 `-Platform`은 기본 `Windows`다. SDK 모드의 `Tools.ps1 -ListInputs`는 해당 없음을 알리고 0이다(개정 10).
- `pwsh`(PowerShell 7)가 필요하다. Windows는 설치 마법사가 늘 설치하고, macOS도 `Rocks.proj` `BootstrapTools`가 `Build/Tools.ps1`을 부르므로 설치 마법사 기본 단계에서 `Build/Toolchain.json`에 고정한 공식 tar.gz(SHA-256)를 푼다(개정 8, [설치 마법사](SetupWizard.ko.md)).

**셸 메뉴(Windows).** `shell-register`가 명시적으로 부를 때만 `HKCU\Software\Classes`에 쓴다. SDK 등록이 자동으로 셸 메뉴를 만들지 않는다.
- `.rocksproject` → ProgID `Rocks.ProjectFile`.
- `Rocks.ProjectFile\shell\rocksgenproj`(표시 이름 "Generate Visual Studio project files")의 명령은 선택기 `dotnet "<선택기 CLI>" projectfiles --project "%1"`이다.
- 선택기 CLI는 `shell-register` 때 **유효한 등록 SDK 중 엔진 버전이 가장 높은 것**의 `Tools/Rocks.Build.Cli`이고, 그 경로를 `HKCU\Software\Rocks\ProjectSelector`에 둔다. 선택기는 연결을 해석하고, 고른 SDK가 자기가 아니면 그 SDK의 CLI로 같은 명령을 넘긴다. 연결 해석 구현은 C# 한 곳에만 있다. 선택기 SDK가 지워졌으면 메뉴가 `shell-register` 재실행을 안내한다.

UE는 HKLM에 ProgID를 쓰고 UnrealVersionSelector가 메뉴를 받는다(`DesktopPlatformWindows.cpp:526-556`, `UnrealVersionSelector.cpp:392`). Rocks는 관리자 권한이 필요 없는 HKCU를 쓰고, 별도 선택기 실행 파일 없이 CLI가 그 역할을 한다. "SDK 바꾸기" 메뉴(UE `switchversion`)는 고르는 UI가 필요해서 두지 않고 `associate` CLI로 한다.

구현(PJ8, 개정 20):
- **명령.** `shell-register`와 `shell-unregister`(Windows 전용, 옵션 없음). 등록부는 10.2절과 같은 기본 등록부(`ROCKS_SDK_REGISTRY` 재정의 포함)를 읽는다.
- **연결 충돌 정책(PJD44, 개정 21).** 다른 프로그램이 `.rocksproject`를 가져갔으면 그 연결을 바꾸지 않는다. Rocks는 동사(`SystemFileAssociations`)와 "연결 프로그램" 후보(`OpenWithProgids`)만 더하고, 기본 연결은 비어 있을 때만 잡는다. UE는 HKLM에 ProgID를 쓰고 HKCU의 `.uproject` 키와 `UserChoice`를 지워 기본 연결을 되찾는다(`DesktopPlatformWindows.cpp:519-582`) — Rocks는 HKCU만 쓰고 사용자가 고른 연결을 존중한다.
- **선택기 SDK.** 등록 SDK 중 `rocks-sdk.json`이 있고 manifest 판이 이 CLI의 판과 같고 엔진 버전을 읽을 수 있고 `Tools/Rocks.Build.Cli/Rocks.Build.Cli.dll`이 있는 것만 유효하다. 그중 엔진 버전(주·부·패치)이 가장 높은 것, 같으면 등록 ID ordinal 첫째다. 유효한 SDK가 없으면 버린 이유를 적은 오류다. manifest를 읽거나 파싱하지 못한 SDK는 그 SDK만 버리고 이유에 적는다(개정 21).
- **쓰는 값(HKCU, 개정 21).** 메뉴 동사는 `Software\Classes\SystemFileAssociations\.rocksproject\shell\rocksgenproj`(기본값 "Generate Visual Studio project files")와 그 `command`에 둔다 — 기본 연결·`UserChoice`와 무관하게 메뉴에 나온다(ProgID 동사는 `UserChoice`가 다른 프로그램이면 나오지 않는다). ProgID `Software\Classes\Rocks.ProjectFile`(기본값 `Rocks game project`)을 두고, `Software\Classes\.rocksproject`의 기본값은 **비어 있을 때만** 쓰며 `OpenWithProgids`에는 `Rocks.ProjectFile` 값만 더한다. 선택기는 `Software\Rocks\ProjectSelector`(값 `Sdk`·`Cli`·`Shim`)다. 쓴 뒤 `SHChangeNotify(SHCNE_ASSOCCHANGED)`로 탐색기에 알린다(UE에는 이 호출이 없다 — Rocks가 더한 것이다).
- **메뉴 명령(개정 21).** `"<pwsh.exe 절대 경로>" -NoProfile -ExecutionPolicy Bypass -File "<shim>" -Project "%1"`. cmd를 거치지 않으므로 `%1`·경로 안의 `%VAR%`·`!`가 확장되지 않는다. shim은 `shell-register`가 `%LOCALAPPDATA%\Rocks\ShellMenu.ps1`에 쓰는 작은 스크립트다: 선택기 SDK의 `Build/GenerateProjectFiles.ps1`이 없으면(SDK를 지움) `shell-register`를 다시 하라고, `dotnet`이 없으면 설치하라고 알리고, 그 스크립트를 `-Project`로 부른 뒤 실패하면 Enter를 기다려 오류를 보인다. 생성은 그 스크립트가 IDE 빌드 잠금 안에서 SDK CLI의 `projectfiles --project`로 하고, 연결 해석과 다른 SDK CLI로의 위임은 그 명령이 한다(10.4절). `pwsh`는 `shell-register` 때 PATH에서 찾고, 없으면 등록하지 않고 설치하라는 오류다.
- **지우기(개정 21).** `shell-unregister`는 우리가 쓴 값만 지운다: 메뉴 동사 키, `.rocksproject` 기본값(그 값이 `Rocks.ProjectFile`일 때), `OpenWithProgids`의 `Rocks.ProjectFile` 값, ProgID, 선택기 키, shim. 키는 비었을 때만 지운다(다른 프로그램의 연결과 `OpenWithProgids` 항목이 남는다).
- **시험.** `GameProjectTests`가 레지스트리의 임시 하위 키(`HKCU\Software\Rocks\Tests\<GUID>`)를 Classes·선택기 키로 주어 쓰기·값·지우기를 시험한다: 가장 높은 유효 SDK 선택(판이 다른 manifest·없는 CLI·사라진 SDK 제외), 같은 버전의 ID 순, 명령 문자열, 다른 프로그램 연결 보존, 유효한 SDK 없음 오류. 실제 HKCU Classes에는 쓰지 않는다(수동 확인은 사용자 점검 목록).

## 13. 다른 IDE

- **VS Code·clangd**: 범위 밖이다. 빌드 출력마다 있는 `compile_commands.json`(논리 명령, PCH 헤더·정의 헤더 강제 include 포함)을 clangd에 `--compile-commands-dir=out/<출력>`으로 주면 된다. 이 안내를 `Build/README.md`에 한 줄로 적는다(PJ2). `.vscode/` 생성은 하지 않는다.
- **Rider**: `.slnx`·NMake `.vcxproj`를 읽는다. 따로 만들지 않고 검증도 하지 않는다.
- **Xcode**: 범위 밖이다. macOS 개발 환경을 정할 때 별도 스펙으로 다룬다(UE `XcodeProjectFileGenerator` 대응).

## 14. 결정 사항

접두 `PJD`(결정)·`PJ`(단계)는 다른 문서와 겹치지 않는다(2026-11-21, `Docs`·`Build`·`Programs`의 `*.md`에서 `PJ[0-9]` 검색 0건). 개정 1의 PJD1–PJD31은 2026-11-21 사용자가 권장안대로 승인했다. 개정 2에서 고친 항목은 상태 칸에 적는다.

| 번호 | 결정 | 상태 |
|---|---|---|
| PJD1 | 기준 Visual Studio는 2026(18.x). 솔루션은 `.slnx`만 쓰고 `Microsoft.VisualStudio.SolutionPersistence`로 쓴다 | 확정 |
| PJD2 | 생성은 `Rocks.Build.Cli projectfiles`, `--root`(엔진)·`--project`(게임) 중 하나 | 확정 |
| PJD3 | 엔진 솔루션은 루트 `Rocks.slnx`, 프로젝트·props·정의 사본·중간 파일·상태는 `out/ProjectFiles/`. `.gitignore`에 `/Rocks.slnx` | 확정(개정 2: 중간 파일 위치 추가) |
| PJD4 | 프로젝트는 `.Target.cs` 정의 디렉터리로 정한다. `Core/`·`Engine/` 아래 타깃은 `Rocks`의 구성, 그 밖은 타깃마다 프로젝트. 같은 구성 이름이 겹치면 생성 오류 | 확정(개정 2: 종류 기준 → 정의 위치 기준) |
| PJD5 | 구성 Debug·Development·Release·Asan(Coverage 제외), 플랫폼 Windows(x64)·Android(`Android_arm64_<C>`, NDK가 있을 때) | 확정 |
| PJD6 | IntelliSense 원천은 IDE 계획 모드의 `LogicalCompiles`(메타 생성·출력 쓰기 없음, 유니티·작업 파일 분리 끔, PCH 켬) | 확정(개정 2: IDE 계획 모드) |
| PJD7 | 인자 분류표를 두고 표에 없는 인자는 생성 오류. 버린 최적화 인자의 매크로는 정책 `ide-invisible-macro`로 막는다 | 확정 |
| PJD8 | 늘 파일별 IntelliSense. 묶음 속성으로 접고 가장 많은 묶음은 `ItemDefinitionGroup` 기본값으로 | 확정(개정 2: 기본값 위치) |
| PJD9 | 정의 헤더는 내용 주소 사본(`out/ProjectFiles/Definitions/`)을 같은 순서로 강제 include | 확정 |
| PJD10 | PCH 헤더는 강제 include 그대로 + `/Yu"<헤더>"` 힌트 | 확정 |
| PJD11 | `PlatformToolset=ClangCL`. `LLVMInstallDir`·`LLVMToolsVersion`은 resource 디렉터리 조회로, 시스템 `IncludePath`는 도구 모음 환경으로. props import 순서는 8.5절. 구성 요소가 없으면 경고 | 확정(개정 2: 조회 방식·import 순서) |
| PJD12 | 저장소 경로는 `$(RocksRoot)` 기준, 컴퓨터 값은 두 도구 모음 props에만 | 확정 |
| PJD13 | 프로젝트 GUID는 경로 이름 기반 UUID v5. 정렬·인코딩(UTF-8, CRLF) 고정, 바뀐 파일만 쓰기. 정리는 직전 상태 목록에 있던 파일만, `.user`·중간 파일은 건드리지 않음 | 확정(개정 2: 정리 범위) |
| PJD14 | NMake Build·Rebuild·Clean은 `Build/IdeBuild.bat`(→ `IdeBuild.ps1`). `NMakeOutput`은 IDE 계획 모드의 실행 파일 경로 | 확정 |
| PJD15 | IDE 빌드 잠금 하나가 도구 준비와 CLI 실행을 함께 덮는다. `GenerateProjectFiles.ps1`도 같은 잠금 | 확정(개정 2: 잠금 범위) |
| PJD16 | CLI `clean`: `.native-build.lock`이 있는 Rocks 출력 하나를 잠금 아래에서 비운다. Rebuild = clean + build | 확정(개정 2: 표지 조건) |
| PJD17 | 재생성은 수동. 상태 지문을 두고 `build`·`game`이 생성기와 같은 필터로 낡음을 `warning RPF0001`로 알린다(`--pch off`·Android 빌드는 비교 안 함). 자동 재생성 없음 | 확정(개정 2: 필터) |
| PJD18 | 게임 출력 기본 `<project>/out/<platform>-<target>-<configuration>` | 확정 |
| PJD19 | 게임 snapshot과 게임 규칙 탐색은 같은 필터로 점 디렉터리(모든 깊이)와 최상위 `out/`·`Saved/`·`*.slnx`를 뺀다 | 확정(개정 2: 규칙 탐색 포함) |
| PJD20 | `.rocksproject` 선택 필드 `engineAssociation`(경로 또는 등록 ID). 없으면 `Verify` 호환 후보 중 `engineVersion`이 같은 최고 패치, 동률은 오류 | 확정(개정 2: 호환 후보) |
| PJD21 | SDK 등록부: Windows `HKCU\Software\Rocks\Sdks`, 그 밖 `~/.config/Rocks/Sdks.json`. 등록은 명시적으로만(`sdk` 기본 `--register off`, `sdk-register`), `sdk-unregister`·`associate` 명령 | 확정(개정 2: 기본 off) |
| PJD22 | 다중 구성 SDK(기본 Debug·Development·Release), 구성별 부분은 `Configurations/<구성>/`, manifest 14 | 확정 |
| PJD23 | 게임 솔루션은 게임 프로젝트(루트에 정의된 타깃이 구성) + 하위 디렉터리 타깃 프로젝트 + 빌드하지 않는 `RocksSdk` 헤더 프로젝트 | 확정(개정 2: 정의 위치 기준) |
| PJD24 | 템플릿은 고유 이름(`FoundationTemplate`)의 실제 프로젝트. 토큰 없음 | 확정 |
| PJD25 | `Template.json`. 치환은 세 철자·대소문자 구분·두 단계, 경로와 내용에 같은 규칙, 남은 이름 검사 | 확정 |
| PJD26 | `new-project` CLI, 기술자 재작성, MetaCompiler `ids reissue`(새 ID, 별칭 비움, 은퇴 항목 삭제). SDK가 `Templates/`를 싣는다 | 확정 |
| PJD27 | 루트 `GenerateProjectFiles.bat`·`GenerateProjectFiles.ps1`(개정 22) + `Build/GenerateProjectFiles.ps1`·`Build/Tools.ps1`·`Build/IdeBuild.*`(pwsh 7). SDK에도 싣는다 | 확정(개정 2: `Tools.ps1`) |
| PJD28 | 셸 메뉴는 `shell-register`로만 만드는 HKCU ProgID `Rocks.ProjectFile`. 선택기는 유효한 등록 SDK 중 최고 판의 CLI가 맡고 맞는 SDK로 넘긴다 | 확정(개정 2: 명시 등록·최고 판) |
| PJD29 | 파일 하나 컴파일은 `build --files`(게임은 `game --files`, PJ7). 그 전에는 `NMakeCompileFileCommandLine`을 내보내지 않는다 | 확정(개정 2: 내보내기 시점, 개정 17 구현) |
| PJD30 | VS Code·Rider·Xcode 생성은 범위 밖. clangd는 `compile_commands.json` 안내만 | 확정 |
| PJD31 | C# 프로젝트는 `Rocks.Format.slnx` 목록을 그대로 넣고 솔루션 빌드에서 뺀다 | 확정 |
| PJD32 | 부작용 없는 계획 함수(엔진 `build`·게임 `game` 공통)와 IDE 계획 모드(존재 검사·archive·link·PCH·생성 TU 없음)를 PJ0에 둔다 | 확정(개정 2, 권장안) |
| PJD33 | 파일 소유는 경로 기준(`Core/`·`Engine/` → `Rocks`, 그 밖은 가장 가까운 정의 디렉터리의 타깃). IntelliSense 원천은 (파일, 구성)마다 그 타깃의 명령, 없으면 ordinal 첫 타깃의 명령 | 확정(개정 2, 권장안) |
| PJD34 | Android 구성은 빌드 전용. IntelliSense는 같은 `<C>`의 Windows 값을 쓰고 Android 계획은 만들지 않는다 | 확정(개정 2, 권장안) |
| PJD35 | Makefile 합성 규칙에 맞춰 파일별 값은 언제나 전체 목록, `NMakeIncludeSearchPath`·`NMakeForcedIncludes`·`NMakePreprocessorDefinitions`는 비우고 `IncludePath`는 시스템 include만 | 확정(개정 2, 권장안) |
| PJD36 | 생성기는 메타 생성을 돌리지 않고, 없는 생성 헤더는 빌드할 타깃·구성 이름으로 알린다 | 확정(개정 2, 권장안) |
| PJD37 | 도구 준비는 입력 지문이 같으면 `dotnet build`를 건너뛴다(`out/dotnet/.rocks-tools.json`). `Rocks.proj` `BootstrapTools`도 같은 스크립트 | 확정(개정 2, 권장안) |
| PJD38 | Visual Studio 안 C# 빌드(`BuildingInsideVisualStudio`)는 `out/dotnet-vs/`를 쓴다 | 확정(개정 2, 권장안) |
| PJD39 | IDE 빌드는 생성 때 탐색한 clang-cl 절대 경로(`RocksCompiler`)를 `--compiler`로 넘긴다 | 확정(개정 2, 권장안) |
| PJD40 | 기존 `sdk` 호출자는 바꾸지 않고(기본 off), 아무도 등록하지 않음을 시험한다 | 확정(개정 2, 권장안) |
| PJD41 | 시작 프로젝트는 사용자가 고른다(`.slnx` 직렬화기가 항목을 정렬해 첫 항목을 정할 수 없다 — 개정 5에서 바꿈). Asan 구성은 `LocalDebuggerEnvironment`에 runtime DLL 경로. natvis는 자리만 | 확정(개정 5) |
| PJD42 | `build --files`는 메타 생성을 맞춘 뒤 논리 명령을 별도 object(`<out>/SingleFile/`)로 돌리고 증분 기록에 넣지 않는다. 경로 기준은 `-WorkingDir` | 확정(개정 2, 권장안) |
| PJD44 | `.rocksproject` 연결 충돌: 동사는 `SystemFileAssociations`, `OpenWithProgids`에 더하고, 기본 연결은 비었을 때만 잡으며, 해제는 우리 값만 지운다(UE와 달리 남의 연결·`UserChoice`를 지우지 않는다) | 확정(개정 21) |
| PJD43 | `--template`은 템플릿 디렉터리 이름 또는 경로. 프로젝트 이름의 소문자 형이 C++ 키워드·`std`·`rocks`·`posix`, 대문자 형이 `ROCKS`면 거부 | 확정(개정 2, 권장안) |

## 15. 단계

각 단계는 Debug 빌드, format, policy, 바뀐 부분의 시험으로 검증한다. 수동 Visual Studio 확인은 PJ2와 PJ5 끝에 한 번씩 한다(16절 V11).

| 단계 | 내용 | 완료 기준 |
|---|---|---|
| PJ0 | 선행: 부작용 없는 계획 함수 분리(엔진 — 게임 쪽은 쓰는 단계인 PJ5로 옮겼다, 개정 3), IDE 계획 모드(8.1절). 그 위에 인자 분류표(`IntelliSenseArguments`), IntelliSense 모델(정의 디렉터리·파일 소유·(파일, 구성) 원천·묶음 접기·정의 사본·필터 함수 `IntelliSenseModel.Select`), 정책 `ide-invisible-macro` | V1 통과(깨끗한 사본, 미분류 인자 0, 일반 모드와 논리 명령 같음, `out/ProjectFiles` 밖 쓰기 0) |
| PJ1 | 엔진 모드 쓰기: `.vcxproj`·`.filters`·`Rocks.slnx`·두 도구 모음 props(import 순서)·정의 사본·중간 파일 경로·상태, GUID, 바뀐 파일만 쓰기·상태 목록 기준 정리, C# 프로젝트와 `out/dotnet-vs` 분리, 시작 프로젝트, 생성 시간 측정 | V2–V5 통과, 소요 시간 기록 |
| PJ2 | NMake: `Build/Tools.ps1`(지문)·`Build/IdeBuild.*`(한 잠금 범위, 대기 표시, `--compiler`, Asan 디버거 환경), `Rocks.proj` `BootstrapTools` 전환, 엔진 `clean`, Rebuild, 루트 `GenerateProjectFiles.bat`·`Build/GenerateProjectFiles.ps1`, 생성 헤더 안내, 낡음 경고(`RPF0001`), `.gitignore`, README 안내(clangd, `Rocks.proj` 잠금 한계) | V6·V7·V13·V14 통과, 수동 확인(엔진) |
| PJ3 | 게임 선행: 출력 경로(10.5절), 게임 `clean`, snapshot·규칙 탐색 제외(10.6절), `engineAssociation`·`SdkAssociation`(호환 후보), 등록부와 `sdk-register`·`sdk-unregister`·`associate`, 기존 `sdk` 호출자 확인, SDK가 스크립트를 싣는다 | V8·V9 통과, 게임 `clean` 경계 시험 |
| PJ4 | 다중 구성 SDK(10.3절), manifest 14, `Verify` 구성 목록 | V10 통과 |
| PJ5 | 게임 모드 생성(manifest 14 기준): 게임 쪽 부작용 없는 계획 함수 분리(`InstalledSdk.BuildGameCoreAsync`)와 IDE 모드, 게임·타깃·`RocksSdk` 프로젝트, 게임 NMake | 게임 fixture로 V2–V6 통과, 수동 확인(게임) |
| PJ6 | 템플릿: `FoundationTemplate` 개명과 영향 파일(11.1절 목록), `Template.json`, `new-project`, `ids reissue`, SDK가 `Templates/`를 싣는다 | V12 통과 |
| PJ7 | 파일 하나 컴파일(`build --files`, `NMakeCompileFileCommandLine` 내보내기 시작) | 논리 명령·별도 object·증분 기록 불변 시험, `-WorkingDir` 상대 경로 시험 |
| PJ8 | 셸 통합(`shell-register`, 선택기 최고 판·위임) | 임시 하위 키에 쓴 값의 시험(V8과 같은 방식), 수동 확인 1회 |

순서는 PJ0 → PJ8이다. 게임 생성기(PJ5)는 manifest 14를 읽으므로 다중 구성 SDK(PJ4)가 먼저다.

## 16. 검증

Visual Studio 없이 결정적으로 검사한다. 시험은 `Rocks.Build.Tests`(fixture 규칙과 수동 도구 모음, 컴파일러·Visual Studio 불필요)와 `Rocks.ToolTests`·IntegrationTests(실제 도구 모음, Visual Studio IDE 불필요)에 나눈다. Visual Studio가 있는 환경에서만 도는 검사는 V2b·V11이다.

| 번호 | 검사 | 방법 |
|---|---|---|
| V1 | IDE 계획·분류·왕복 | **빌드한 적 없는 깨끗한 사본**에서 엔진 모든 타깃 × 구성의 IDE 계획(실제 도구 모음)을 만든다. 모든 논리 명령 인자가 분류된다. 모델 검사(`IntelliSenseModel.Verify`): 모든 (파일, 프로젝트 구성)에서 묶음에 접힌 값을 그 구성에서 고른 명령과 분류표 없이 철자로 대조한다(`/I`·`/D`·`/FI` 순서, 옵션, `/TP`·`/TC`, `/Yu`). 값이 없으면 그 구성에서 정말 아무 계획도 컴파일하지 않는지 본다. 같은 요청의 일반 모드 계획과 손으로 쓴 소스의 논리 명령이 같다: 단위 시험은 생성 소스·생성 include를 실제로 두고 유니티·모듈 PCH·공유 PCH를 켠 plan-only compile DB와 비교하고, CI Windows는 Foundation Debug 빌드 뒤 `projectfiles --model-only on --compare-compile-db`를 돈다. 생성 전후 작업 사본의 파일 목록과 수정 시각(루트 디렉터리 포함)을 비교해 `out/ProjectFiles`와 `Rocks.slnx` 밖에서 바뀐 것이 0이다 |
| V2 | MSBuild 평가 | `Microsoft.Build`(+ `Microsoft.Build.Locator`로 .NET SDK의 MSBuild)로 생성 `.vcxproj`를 구성 × 플랫폼마다 평가한다(`ProjectLoadSettings.IgnoreMissingImports`라 Visual Studio의 `Microsoft.Cpp.*.props`가 없어도 된다). 평가한 `ClCompile` 항목과 `ItemDefinitionGroup` 기본값에 8.3절 합성 규칙(항목 값이 기본값을 바꿈, `IncludePath`는 더함)을 적용해 얻은 파일별 (include, 정의, 강제 include, 옵션)이 모델과 같다. NMake 속성을 비교한다. 모든 항목 파일이 존재한다. Android 구성의 값이 Windows와 같다 |
| V2b | 디자인 타임(vswhere로 Visual Studio 18 이상과 ClangCL 구성 요소가 있을 때만) | Visual Studio의 `MSBuild.exe -t:GetClCommandLines;GetProjectDirectories -getTargetResult:…`를 fixture 프로젝트와 실제 생성 프로젝트 하나에 돌린다. 단언: 마지막 인자가 이기는 매크로 인자(`/MDd`·`/MD`·`/EHs-c-`·`/GR-`·`/std:`)는 디자인 타임 기본 인자(`/W1 /EHsc /MD …`) **뒤**에 와서 이긴다, `/FI`·`/I` 순서가 모델과 같다, `IncludePath`가 [resource include, `INCLUDE`] 순서다. CI 필수 검사가 아니다(이미지에 Visual Studio 18이 없으면 건너뛴다) |
| V3 | 솔루션 왕복 | `.slnx`를 같은 패키지로 다시 읽어, 모든 프로젝트 경로가 있고 모든 솔루션 구성 × 플랫폼이 각 프로젝트의 존재하는 구성에 대응되며 C# 프로젝트의 빌드 표시가 꺼져 있고 첫 프로젝트가 실행 파일 프로젝트임을 확인한다 |
| V4 | 결정성·정리 | 같은 입력으로 두 번 생성하면 모든 파일 byte가 같다. 두 번째 실행은 아무 파일도 쓰지 않는다(파일 ID·수정 시각 불변). 다른 루트 경로(작업 사본 둘)에서 생성하면 두 도구 모음 props를 뺀 모든 파일이 같다. 타깃 하나를 지우고 재생성하면 그 프로젝트는 지워지고, 미리 둔 `.vcxproj.user`와 `Intermediate/` 아래 파일은 남는다 |
| V5 | golden | fixture 규칙(실행 파일 1, 라이브러리 2, 공유 PCH 1, C 소스 1, 생성 소스 1, 두 모듈이 함께 컴파일하는 소스 1, 한 디렉터리에 정의된 타깃 2)의 생성물을 `Programs/Rocks.Build.Tests/ProjectFiles/Golden/`과 byte 비교한다 |
| V6 | NMake 실행·IDE와 터미널 | IntegrationTests가 V2로 평가한 `NMakeBuildCommandLine`·`NMakeCleanCommandLine`·`NMakeReBuildCommandLine` 문자열을 `cmd.exe /c`로 fixture 타깃에 실행한다. PATH는 일부러 다른 컴파일러가 앞에 오게 둔다. 빌드하면 `NMakeOutput`의 실행 파일이 생기고, 그 뒤 터미널 `Rocks.proj` 빌드는 0 action이다. Clean하면 출력에 잠금 파일만 남고, Rebuild하면 다시 생긴다. 개정 10: 시험이 먼저 `projectfiles --root`를 돌린다(깨끗한 트리에서 건너뛰지 않는다). cmd 경로는 `INCLUDE`·`EXTERNAL_INCLUDE`·`LIB`·`LIBPATH`를 일부러 다른 값으로 두어, `IdeBuild.ps1`이 지우지 않으면 터미널 0 action 단언이 깨지게 한다 |
| V7 | 직렬화 | 시험이 IDE 빌드 잠금을 한 스레드에서 잡은 상태로 `IdeBuild.ps1`을 띄운다. `Waiting for the IDE build lock` 줄이 나온 뒤, CLI 시작 표지가 아직 없음을 단언하고, 같은 스레드에서 잠금을 놓은 뒤에야 표지가 생김을 단언한다. 시간 대기는 쓰지 않는다 |
| V8 | 연결 해석·등록 | `SdkAssociation.Resolve`를 메모리 등록부로 시험한다: 경로, ID, 호환 후보 중 최고 패치, 호환되지 않는 더 높은 패치는 무시, 동률 오류, 없음 오류(이유 목록), 사라진 SDK 제외. 레지스트리 구현은 임시 하위 키에서 쓰기·읽기·지우기를 시험한다. `sdk` 기본 호출과 기존 호출자가 등록부에 쓰지 않는다. 위치: IntegrationTests `GameProjectTests`(`--game-project`)와 본 흐름(`ROCKS_SDK_REGISTRY` 등록부로 내보내기 뒤 파일 없음 → `sdk-register` → `--sdk` 없는 게임 빌드 0 action → `sdk-unregister` 뒤 오류) |
| V9 | snapshot·규칙 탐색 제외 | 게임 fixture에서 `.vs/x.db`를 쓰기 권한·읽기 공유 없음으로 연 채 게임 빌드가 성공한다. 빌드 중간 지점(빌드 엔진 시험 hook)에서 `.vs`·`Saved/`·`out/<다른 구성>`·최상위 `.slnx`를 고쳐도 성공하고, `Source/`를 고치면 지금처럼 실패한다. `out/` 아래 둔 `*.Build.cs` 사본은 규칙으로 컴파일되지 않는다. 구현(개정 9): 빌드 시작·끝 비교가 같은 `GameInputSnapshot.Capture`이므로 빌드 중간 hook 대신 `GameProjectTests`가 Capture 값으로 단언한다(빼는 곳을 만들고 고쳐도 같음, `.vs` 파일을 공유 없이 연 채 Capture 성공, 소스·중첩 `Saved/`·`out/`·점 파일·명시 입력은 바뀜). 본 흐름의 첫 게임 빌드는 `.vs/…/Browse.VC.db`를 공유 없이 연 채 성공한다 |
| V10 | 다중 구성 SDK | fixture SDK를 두 구성으로 내보내 두 구성의 게임 빌드가 성공하고, 없는 구성은 `Verify`가 거부한다. 한 벌 부분이 구성마다 다르면 내보내기 오류다 |
| V11 | 수동(Visual Studio 2026) | 솔루션 열기. ClangCL 모드에서 `__clang__`이 정의되는지. 지정 파일 목록(`Core`·`ObjectRuntime`·`FoundationTests` 각 3개)의 IntelliSense 오류 0과 C++23 거짓 오류 목록. `/std:c++23preview` 수용 여부(아니면 `/std:c++latest`). `/Yu` 힌트 유무에 따른 IntelliSense 준비 시간. `.slnx` 열기. props import 순서가 `LLVMInstallDir`·`IncludePath`에 반영되는지. `Sandbox` F5, Asan F5, Clean·Rebuild, Android 구성 빌드, PJ7 뒤 Ctrl+F7. 엔진 모드 계획 60회 소요 시간. 결과를 개정 이력에 적는다 |
| V12 | 템플릿 | `new-project`로 만든 프로젝트를 fixture SDK로 빌드·실행한다(IntegrationTests). 경로·내용에 템플릿 이름 세 철자가 0건이고, 모든 ID가 템플릿과 다르고 서로 다르며, `ids verify`를 통과하고, 두 번 만든 프로젝트끼리 ID가 다르다. 대상 디렉터리가 비어 있지 않으면 아무것도 쓰지 않는다. 이름 검증(키워드·`std`·`rocks`·`ROCKS`·SDK 모듈 이름·길이)의 거부 사례 |
| V13 | 도구 준비 | 지문이 같으면 `dotnet build`를 부르지 않는다(시험용 표지). 도구 원본 하나를 고치면 부른다. 지문 입력 집합이 각 도구 프로젝트를 MSBuild로 평가한 `Compile`·`ProjectReference`·`None`(복사 대상) 항목과 import 파일을 모두 덮는다 |
| V14 | 정리·경고 | `clean`은 `.native-build.lock`이 없는 디렉터리와 `out` 밖 경로를 거부한다. `RPF0001`: 소스 추가 뒤 경고가 나고, 재생성 뒤 사라지며, `--pch off`·`--unity off`·`--adaptive off`·`--platform android` 빌드와 `PublicHeaderChecks`만 바뀐 경우에는 나지 않는다 |

## 17. 위험과 한계

| 번호 | 내용 | 대응 |
|---|---|---|
| L1 | ClangCL 모드 IntelliSense(EDG의 clang 흉내)가 C++23 기능에서 거짓 오류를 낼 수 있다. 이 컴퓨터는 vswhere에 Visual Studio 2019 16.11(도구 모음 v142)만 보이고 ClangCL 도구 모음이 없어 실측하지 못했다 | V11 수동 확인에서 오류 목록을 기록한다. 원인이 IntelliSense면 코드가 아니라 기록으로 다룬다. 기준은 빌드다 |
| L2 | `ClangCL` 도구 모음 구성 요소가 없으면 IntelliSense가 제대로 돌지 않는다. NMake 빌드는 된다(`Microsoft.MakeFile.targets:43`) | 생성기 경고(8.5절), README 요구 사항에 구성 요소 이름 |
| L3 | 원천 타깃을 그 구성으로 빌드하지 않았으면 리플렉션 생성 헤더가 없어 IntelliSense 오류가 난다(엔진 파일은 `Engine` 타깃) | 생성기가 빌드할 타깃·구성을 알린다(8.6절) |
| L4 | 큰 프로젝트(구성당 파일 1,400–1,900개)의 IntelliSense 메모리 | 묶음 속성 접기(8.3절). UE도 같은 이유로 속성을 접는다(`VCProject.cs:1419-1422`) |
| L5 | 루트에 `Rocks.proj`와 `Rocks.slnx`가 함께 있으면, 프로젝트를 적지 않은 `dotnet msbuild`·`dotnet build`가 MSB1011(프로젝트 여럿)로 멈춘다 | 모든 문서 명령은 `Rocks.proj`를 적는다. README에 한 줄로 적는다 |
| L6 | 재생성 전까지 새 파일이 프로젝트에 없다 | 낡음 경고(9절) |
| L7 | 다중 구성 SDK는 크기와 내보내기 시간이 구성 수만큼 는다 | 기본 세 구성, Asan은 지정할 때만 |
| L8 | HKCU 등록부·셸 메뉴는 컴퓨터 상태를 바꾼다. SDK를 지워도 항목이 남는다 | 명시 등록만, 읽을 때 사라진 SDK를 빼고 알림, `sdk-unregister` |
| L9 | 템플릿 개명이 시험·정책 자체 시험·문서·`Rocks.proj` 기본값에 닿는다 | PJ6 한 변경에서 함께 고친다(11.1절 목록) |
| L10 | 엔진 모드 계획 60회의 시간 | 병렬 계획(PJ1). 실측(리뷰어, 2026-11-21): 계획 7,985 ms, 쓰기까지 9.0–9.7 s, 두 번째 실행은 쓰기 0. 이 PC: 계획 5.2–6.9 s, 전체 6.4–8.0 s |
| L11 | NMake 빌드가 `pwsh`에 의존한다 | `IdeBuild.bat`가 없으면 안내하고 실패 |
| L12 | Android 구성에서는 Android 전용 분기(`#if ROCKS_PLATFORM_ANDROID`)가 IntelliSense에 비활성으로 보인다. Windows 값을 쓰기 때문이다 | Android 코드는 빌드가 기준이다. clangd에 Android 출력의 `compile_commands.json`을 주면 그 분기를 본다 |
| L13 | CLI 자체 오류는 MSBuild 정규 형식이 아니라 출력 창에만 나온다 | `RPF0001` 외에는 후속 |
| L14 | snapshot 제외 영역을 규칙이 읽으면 변경을 놓친다 | 동적 입력 명시 계약 |
| L15 | 선택기 SDK가 지워지면 셸 메뉴가 실패한다 | `shell-register` 재실행 안내 |
| L16 | 터미널 `Rocks.proj`의 CLI 실행은 IDE 잠금 밖이다. 터미널 빌드 도중 IDE가 도구를 다시 빌드하면 경합할 수 있다 | 도구 준비는 같은 잠금·지문을 쓰므로 원본이 바뀌었을 때만 생긴다. `Build/README.md`에 적는다 |
| L17 | 도구 지문 입력 집합이 MSBuild 입력을 빠뜨리면 낡은 도구를 쓴다 | V13이 평가 항목과 대조한다 |
| L18 | IDE 빌드 잠금(`Local\`)은 로그온 세션 단위다. 다른 세션(서비스·원격 데스크톱)의 빌드와는 직렬화되지 않는다 | 같은 작업 사본을 여러 세션에서 동시에 빌드하지 않는다. 출력 디렉터리 잠금은 세션과 무관하게 막는다 |
| L20 | 게임 빌드는 프로젝트 파일 낡음 경고(`RPF0001`)를 내지 않는다(PJ5) | 게임 소스를 더하면 `GenerateProjectFiles.ps1 -Project`로 다시 만든다. 필요해지면 엔진 모드와 같은 지문 비교를 게임 빌드에 붙인다 |
| L21 | 연결 없이 등록부로 SDK를 고른 게임은 IDE 빌드가 낡은 SDK를 잡지 못한다(검사는 `engineAssociation`이 있을 때만, PJ5 리뷰) | 등록부를 바꾸면 `GenerateProjectFiles.ps1 -Project`로 다시 만든다 |
| L22 | Ctrl+F7도 build-evidence의 저장소 소스 snapshot·git 조회를 돈다(7.4절 비용) | 느리다고 판단되면 `BuildEvidence`에 snapshot 생략 모드를 더한다 |
| L19 | 한 벌로 둔 생성 공개 헤더나, 규칙이 구성에 따라 바꾼 모듈 include 배치가 구성마다 다르면 다중 구성 내보내기가 실패한다(PJ4). Development를 넣은 조립은 CI 비용 때문에 IntegrationTests에서 돌리지 않는다(Debug·Release만) | 실패가 파일 이름과 두 구성을 알린다. 그런 헤더가 생기면 `Headers/`를 구성별로 옮기고 manifest 판을 올린다 |

## 18. 버린 대안

| 대안 | 버린 이유 |
|---|---|
| CMake로 Visual Studio 프로젝트 생성 | Rocks.Build는 CMake·Python을 부르지 않는다(`Programs/Rocks.Build/README.ko.md` 첫 문단). 빌드 계획이 두 벌이 된다 |
| Visual Studio가 직접 컴파일하는 MSBuild `ClCompile` 프로젝트 | 증분·PCH·유니티·BuildInfo·리플렉션 생성을 MSBuild로 다시 구현해야 한다. UE도 NMake다 |
| Open Folder(`CppProperties.json`) | 구성·디버그 대상·파일별 설정이 약하다 |
| `.sln` 함께 쓰기 | 하위 호환을 두지 않는다. 기준 Visual Studio가 `.slnx`를 읽는다 |
| IntelliSense MSVC 모드 + `__clang__` 정의 주입(UE 방식) | `Compiler.h`의 판정을 속이고 MSVC 문법으로 해석한다 |
| 일반 계획을 그대로 쓰고 그 전에 메타 생성·`--plan-only`를 돌림 | 생성이 출력에 쓰고 타깃 × 구성마다 clang AST를 돌린다. 생성 결과가 빌드 이력에 달린다 |
| 생성기가 MetaCompiler만 `out/ProjectFiles/Generated/`에 돌림 | 느리고, 빌드 출력과 다른 생성 헤더 트리를 IntelliSense가 본다(8.6절) |
| 정의를 `/D`(`PreprocessorDefinitions`)로 풀기 | 값의 괄호·따옴표·빈 값과 MSBuild 이스케이프, 강제 include 순서 의미 차이(8.4절) |
| 빌드 출력의 정의 헤더를 바로 강제 include | 빌드 전에는 없고 출력 잠금이 필요하다 |
| `compile_commands.json` 파일을 읽어 생성 | 모듈 정보가 없고 빌드한 구성 하나뿐이다 |
| 모르는 인자를 조용히 버림 | 빌드와 IntelliSense가 모르게 갈라진다 |
| 공통 묶음을 `NMakeIncludeSearchPath`·`NMakeForcedIncludes`로 올리고 항목에 차이분 | Makefile 합성 규칙에서 include는 모든 파일에 더해지고 강제 include·정의는 항목이 바꾼다(8.3절) |
| 타깃 종류로 엔진 프로젝트를 고름 | `Engine`과 `Sandbox`가 같은 구성 이름이 된다 |
| `ModuleDirectory`나 주 모듈 소스 공통 접두로 타깃 위치를 정함 | 앞은 Program 모듈에 비어 있고, 뒤는 주 모듈이 `Core/`에 있는 Program 타깃을 엔진 쪽으로 보낸다 |
| 모듈 기준 파일 소유 | 손으로 쓴 소스 407개가 여러 모듈로 컴파일된다 |
| Android 구성도 clang-cl x64로 IntelliSense | 모든 번역 단위가 `Compiler.h`에서 `#error`다 |
| 모듈마다 프로젝트 | 프로젝트 수백 개, 솔루션 열기 비용. UE도 엔진 프로젝트 하나다 |
| 빌드 중 자동 재생성 | 진행 중 솔루션 빌드에서 프로젝트 다시 읽기가 일어난다 |
| 무작위 GUID를 만들고 기존 파일에서 다시 읽기(UE) | 파일이 없어지면 바뀐다. 이름 기반이면 언제나 같다 |
| 도구 빌드만 잠그고 실행은 잠그지 않음 | 실행 중인 dll을 덮어쓰다 실패한다(MSB3027) |
| 터미널 `Rocks.proj` 빌드까지 IDE 잠금으로 | 다른 세션의 터미널 빌드도 모두 줄을 선다 |
| PATH에서 컴파일러 찾기(IDE 빌드) | Visual Studio가 PATH를 바꿔 터미널과 다른 컴파일러를 고를 수 있다 |
| `out/ProjectFiles`에서 이번에 만들지 않은 파일을 모두 지움 | `.user`·Visual Studio 중간 파일을 지운다 |
| 템플릿 토큰(`%PROJECTNAME%`) | 템플릿이 빌드·format·리플렉션 검사를 통과하지 못한다 |
| 대소문자 무시 치환 | 고유 이름이면 불필요하고 결과가 입력 철자에 달린다 |
| 구성마다 SDK를 따로 두고 연결 표에 구성별 경로 | 등록·연결·검증이 구성 수만큼 늘고 공통 부분이 중복된다 |
| `sdk` 내보내기가 기본으로 등록 | 시험·CI·임시 SDK가 사용자 등록부에 쌓인다 |
| 연결을 환경 변수(`ROCKS_SDK`)로 | 셸·IDE·탐색기마다 환경이 달라 결정적이지 않다 |
| 상위 디렉터리에서 엔진 찾기(UE) | Rocks 게임은 엔진 원본으로 빌드하지 않는다 |
| HKLM 등록(UE ProgID) | 관리자 권한이 필요하다 |
| 별도 선택기 실행 파일(UE UnrealVersionSelector) | 연결 해석 구현이 둘이 된다. CLI가 같은 일을 한다 |
| C# 프로젝트를 솔루션 빌드에 포함 | `IdeBuild`의 도구 빌드와 같은 출력을 두고 경합한다 |
| 파일 하나 컴파일을 유니티 묶음에서 빼서 물리 action으로 | 평소 빌드의 object·기록과 얽힌다. 논리 명령을 별도 object로 돌리면 독립이다 |
| 솔루션을 `out/ProjectFiles` 안에 | 찾기 어렵다. UE도 루트에 둔다 |

## 19. UE와 다르게 정한 점

| 항목 | UE 5.8 | Rocks | 이유 |
|---|---|---|---|
| 솔루션 형식 | `.sln` + `.slnx` | `.slnx`만 | 하위 호환 없음 |
| IntelliSense 컴파일러 | MSVC 기준(`GetCompilerForIntellisense`) | ClangCL 모드 | `Compiler.h`가 clang만 받는다 |
| 정의 전달 | 프로젝트 정의 목록 | 정의 헤더 내용 주소 사본 강제 include | 빌드와 같은 파일·순서 |
| 미분류 인자 | 해당 개념 없음 | 생성 오류 | 빌드와 IntelliSense의 조용한 차이 방지 |
| 프로젝트 GUID | MD5 이름 기반 + 기존 파일 재사용 | UUID v5 이름 기반만 | 결정성 |
| 시작 프로젝트 | `.suo` 쓰기 | 솔루션 첫 항목 | `.suo`는 쓰지 않는다 |
| 연결이 없을 때 | 상위 디렉터리의 엔진 | `engineVersion`과 호환 조건으로 등록 SDK | 게임은 늘 SDK로 빌드 |
| 등록 범위·시점 | HKCU(소스 빌드)·HKLM(ProgID), 실행 시 자동 등록 | HKCU만, 명시 등록만 | 관리자 권한 불필요, 시험이 등록부를 더럽히지 않음 |
| 템플릿 치환 | 목록 셋, 원래 철자는 대소문자 무시 | 규칙 셋 고정, 경로·내용 공통, 대소문자 구분, 남은 이름 검사 | 고유 이름 전제에서 단순·결정적 |
| 새 프로젝트 | 에디터 대화 상자 | CLI `new-project` | 에디터 전에도 쓴다 |
| 타입 ID | 해당 없음(ProjectID만 새로) | identity 명세 재발급 | Rocks 리플렉션 ID는 전역 정체성 |
| 솔루션 빌드 직렬화 | `-WaitMutex` | IDE 빌드 잠금 하나가 도구 준비·실행을 덮음, 도구는 지문이 다를 때만 빌드 | 도구 덮어쓰기 경합과 반복 빌드까지 막는다 |
| 파일 하나 컴파일 | 대상 action을 돌림 | 논리 명령을 별도 object로 | 평소 빌드 상태와 독립 |

## 20. 다른 문서 개정

- `Build/README.md`: 이 스펙 링크(개정 1). PJ2에서 Visual Studio 절(생성·요구 구성 요소·clangd·`Rocks.proj` 잠금 한계), PJ3에서 게임 출력 경로·SDK 등록, PJ4에서 SDK 구성을 고친다.
- `Programs/Rocks.Build/README.ko.md`: PJ0(IDE 계획 모드), PJ2(`clean`), PJ7(`--files`)을 구현할 때 갱신한다.
- `Docs/Guides/RepositoryLayout.ko.md` 기술자 표: `engineAssociation`(PJ3).
- [빌드 타깃](BuildTargets.ko.md) BD5: SDK 변형과 구성 축의 관계(10.3절)를 PJ4에서 적는다.
- `Docs/Implementation/ReflectionMetadata.ko.md`: `ids reissue`(PJ6).
- `Docs/CodingStyle.md`: `build-file-english`에 루트 `GenerateProjectFiles.bat`(PJ2), 정책 `ide-invisible-macro`(PJ0).
- `Templates/Game/Foundation/README.ko.md`: 출력 경로(PJ3), 개명·`new-project`(PJ6).

## 21. 개정 이력

| 개정 | 날짜 | 내용 |
|---|---|---|
| 1 | 2026-11-21 | 첫 판. 2026-11-14 검토(엔진 `--root` 먼저, 계획 기반 파일별 IntelliSense, NMake, 게임 쪽 결정 넷, UE식 템플릿)를 스펙으로 옮겼다. `compile_commands.json` 다섯 출력의 인자를 분류해 Visual Studio 속성 대응을 측정했다(4절). UE 근거는 5.8.4 UBT `ProjectFiles/VisualStudio`·`ProjectFileGenerator`·`GenerateProjectFilesMode`·`CleanMode`, 배치 파일, UnrealVersionSelector·DesktopPlatform, GameProjectGeneration·TemplateDefs. 결정 PJD1–PJD31은 권장안으로 확정했다(같은 날 사용자 승인) |
| 2 | 2026-11-21 | 리뷰 반영. 심각: 계획이 생성물 존재를 요구하고 출력에 쓰므로 부작용 없는 계획 함수와 IDE 계획 모드를 PJ0 선행으로 두고, 생성 헤더는 빌드할 타깃·구성을 알리는 안으로 정했다(8.1·8.6절, PJD32·PJD36). `ModuleDirectory`가 선택 칸이고 소스 407개가 여러 모듈로 컴파일되므로 프로젝트는 `.Target.cs` 위치, 파일 소유는 경로, IntelliSense 원천은 (파일, 구성) 단위로 바꿨다(6.2절, PJD4·PJD33). `Sandbox`가 Game 종류라 타깃 수(15: Game 2·Program 13)와 계획 횟수를 고쳤다. 높음: Android는 빌드 전용(PJD34), Makefile 합성 규칙에 맞춘 `ItemDefinitionGroup` 기본값·전체 목록(8.3절, PJD35, V2·V2b), 도구 준비·실행을 한 잠금으로 묶고 지문으로 건너뜀(7.2절, PJD15·PJD37, V13). 중간: 컴파일러 절대 경로(PJD39), 상태 목록 기준 정리와 `IntDir`·`OutDir`(6.5절), 명시 등록·호환 후보·선택기 최고 판(10.1·10.2·12절), 단계 순서(다중 구성 SDK를 게임 생성 앞으로, 게임 `clean`은 PJ3, PJ7 전 `NMakeCompileFileCommandLine` 없음), `RPF0001` 필터, Asan 디버거 환경·시작 프로젝트·natvis 자리, 파일 하나 컴파일은 논리 명령·별도 object(7.4절), C# 디자인 타임 출력 분리(PJD38). 낮음: 규칙 탐색 제외, 템플릿 개명 범위, 이름 검증, `--template` 의미. 언어 표준은 `/std:c++23preview`로 바꾸고 V11에 미검증 항목을 모았다 |
| 3 | 2026-11-21 | PJ0 구현에서 정한 점. (1) 게임 쪽 계획 함수 분리는 그것을 처음 쓰는 PJ5로 옮겼다. 엔진 쪽은 `EnginePlanning`(`Programs/Rocks.Build.Cli/EnginePlanning.cs`)이 규칙 해석·플러그인 적용·메타 배치·`NativeBuildRequest` 구성을 맡고 `build`와 `projectfiles`가 같이 쓴다. (2) IDE 계획 모드는 `NativeBuildRequest.PlanMode = Ide`와 공개 진입점 `IdePlanner.Plan`이다. 모듈 표 TU 경로는 더하지만 규칙 생성 TU 쓰기(`GeneratedSources.Write`)·BuildInfo 준비·git 조회를 하지 않는다. `ActionGraph`는 출력 아래 소스를 빼고, include 디렉터리 존재를 보지 않고, archive·link 대신 출력 경로만 적는다. `MetadataLayout.Apply`는 `createDirectories: false`를 받는다. (3) 묶음 번호의 key는 "(소유 모듈, 강제 include 모양)" 대신 그 파일의 구성별 네 값 전체(구성 순서대로)로 정했다. 같은 key면 모든 구성에서 값이 같으므로 접기가 정확하고, 구성과 무관한 번호라는 성질은 그대로다. (4) PJ0의 CLI는 `projectfiles --root <root> --model-only on`이다. 모델을 만들고 요약만 출력하며 아무 파일도 쓰지 않는다(쓰기는 PJ1). `--compare-compile-db <compile_commands.json> --target <T> --configuration <C>`는 V1의 "일반 모드와 논리 명령 같음"을 빌드한 출력과 대조한다(`/Fo`는 object 변형 디렉터리가 달라 뺀다). (5) 헤더 목록은 `IdePlan`이 싣는 모듈의 public·private include 디렉터리(하위 포함)와 소스 디렉터리(그 디렉터리만)에서 모은다. 저장소 루트를 include하는 모듈은 루트를 훑지 않고 알린다. (6) 대상 소스는 기준 구성만이 아니라 **모든 구성**의 계획에서 컴파일하는 손으로 쓴 소스의 합집합이다. 기준 구성(Development)은 같은 정의 디렉터리 타깃 사이의 동률을 가를 때만 쓴다. 한 구성에서만 컴파일하는 파일도 목록에서 빠지지 않게 하기 위해서다. (7) 측정(2026-11-21, 이 PC, LLVM 22.1.8): 엔진 모드 15 타깃 × 4 구성 = 60 계획을 순차로 만들고 논리 명령 23,950개를 왕복 검사하는 데 8.8–12.0초(세 번 잰 범위). 목표 10초를 넘을 때가 있어 PJ1에서 계획을 병렬로 돌린다. `--compare-compile-db`로 Foundation Debug 빌드 출력과 대조한 결과 소스 892개 불일치 0(한 소스를 여러 모듈이 컴파일하면 명령 목록끼리 비교한다). 정의 헤더 사본 77개, `Rocks` 프로젝트 소스 1,260개·헤더 951개·묶음 69개. `Programs/LaunchTests`에는 `.Target.cs`가 없어 그 파일 7개가 "정의 디렉터리 밖"으로 `Rocks`에 들어간다(알림). (8) `projectfiles --model-only on`을 돌린 앞뒤로 작업 사본 파일 목록이 같다(V1의 "밖에 쓴 파일 0개") |
| 4 | 2026-11-21 | PJ0 리뷰 반영. (1) 모델은 (파일, 프로젝트 구성)마다 고른 명령(`IdeChoice`)을 싣고, `IntelliSenseModel.Verify`가 묶음 값을 그 명령과 분류표 없이 철자로 대조한다(16절 V1). (2) 논리 명령이 유니티·작업 파일 분리 설정과 무관하도록 모듈 PCH 하한을 원본 소스 수로 세고, 분리된 번역 단위도 논리 명령은 PCH를 쓴다([유니티 빌드·PCH](UnityBuildPch.ko.md) 8.1·7.2절 같은 날 개정). (3) 같음 시험에 생성 소스·생성 include·유니티·모듈 PCH·공유 PCH를 넣고, CI Windows에 V1 단계(`projectfiles --model-only on --compare-compile-db`)를 더했다. (4) 저장소 루트 수정 시각이 바뀌던 원인은 VsDevCmd 확장 `vcpkg.bat` → `vcpkg-init.cmd` 1행의 `> $null`(작업 디렉터리에 `$null` 파일을 만들고 지운다)이었다. VsDevCmd와 vswhere를 `ProbeDirectory`(`Directory.CreateTempSubdirectory`, 끝나면 지움)에서 돌린다. `-no_ext`는 vcvars도 꺼서 쓰지 않는다. 고친 뒤 `projectfiles` 앞뒤로 루트 수정 시각이 같다. (5) 같은 정의 디렉터리의 동률은 정의 파일 이름과 같은 타깃 → 기준 구성 컴파일 → ordinal 순(6.2절). (6) 정의 디렉터리 밖 소스는 기준 구성에서 그 소스를 컴파일하는 Program 타깃으로, 헤더는 같은 디렉터리 소스를 따르고, 알림은 디렉터리마다다. (7) `IdeCompile.PchHeader`를 명시하고(`LogicalCompile.PchHeader`), 정의 헤더도 PCH 헤더도 아닌 강제 include는 오류다. (8) `BuildAsync`는 `PlanMode = Ide`를 거부한다. (9) 본문 6.2·6.5·8.1·8.3절에 개정 3 결정을 반영했다. (10) 묶음 번호는 key의 ordinal 정렬 순서다. `ide-invisible-macro`는 시험 소스에도 적용하고 주석·리터럴을 지운 뒤 본다(구성 매크로 검증 시험 `Base/Tests/BaseTests.cpp`만 예외). 실측: 60 계획, (파일, 구성) 값 5,332개 대조, 9.6초. `Foundation` 프로젝트 소스 9개, `Programs/LaunchTests` 소스 6개는 `Core` 타깃(그 소스를 기준 구성에서 컴파일하는 Program 타깃 중 ordinal 첫째) 프로젝트로 간다 |
| 5 | 2026-11-21 | PJ1 구현에서 정한 점. (1) 쓰기는 `Programs/Rocks.Build.Cli/ProjectFiles/`(`VisualStudioProjects` 렌더링 — 아무것도 쓰지 않는 순수 함수, `ProjectFilesOutput` 쓰기·정리·상태)이고 `projectfiles --root`가 기본으로 쓴다(`--model-only on`은 쓰지 않는다). (2) 솔루션은 `Microsoft.VisualStudio.SolutionPersistence` 1.0.52(UE와 같은 패키지, 저장소 첫 C# 패키지 참조)로 쓴다. 직렬화기가 폴더·프로젝트를 정렬하므로 시작 프로젝트를 첫 항목으로 둘 수 없어 PJD41을 바꿨다(7.1절). 구성 대응 규칙은 플랫폼 `*` → `x64` 하나와, 이름이 다른 경우(Android, 엔진의 Game 외 종류)만 BuildType 규칙으로 적고, 프로젝트에 없는 조합은 `Build=false`다. (3) NMake 명령은 PJ2의 `Build/IdeBuild.bat`를 가리킨다. PJ2 전에는 Visual Studio 빌드가 그 스크립트가 없다는 오류로 실패한다(IntelliSense·탐색은 된다). (4) 어느 구성에서도 컴파일되지 않는 소스는 `None` 항목이다(`ClCompile`이면 기본 묶음 값을 물려받아 틀린 IntelliSense를 본다). (5) `IntDir`·`OutDir`은 `out/ProjectFiles/Intermediate/<프로젝트>/<구성>_x64/`다. (6) Visual Studio 안 C# 빌드(`BuildingInsideVisualStudio`)는 `out/dotnet-vs/`를 쓴다(`Programs/Directory.Build.props`). (7) 계획은 (타깃, 구성)마다 병렬로 돈다. (8) 검증: V2(MSBuild 평가 — `Microsoft.Build` 17.14.28(17.11.4는 취약점 경고 NU1903) + `Microsoft.Build.Locator` 1.7.8, 시험 프로젝트에만), V3(솔루션 왕복), V4(결정성·두 루트·두 번째 실행 무쓰기·정리 범위), V5(golden — 손으로 만든 모델, `Programs/Rocks.Build.IntegrationTests/ProjectFiles/Golden/*.golden`)는 IntegrationTests에 둔다 |
| 6 | 2026-11-21 | PJ1 리뷰 반영. (1) `IncludePath`를 [resource include] + `INCLUDE`로 쓰고 `ExternalIncludePath`를 `INCLUDE`로 쓴다(8.5절 — 덮어쓰면 ClangCL의 resource include가 빠지고 VS 기본 `ExternalIncludePath`가 붙었다). (2) V2b(Visual Studio 디자인 타임 명령줄)를 IntegrationTests에 더하고, CI Windows에 쓰기 모드 `projectfiles --root .`를 한 번 더 돈다. (3) 모듈 PCH 하한은 **손으로 쓴 소스만** 센다(권장안). 리플렉션 생성 소스는 IDE 계획에 없으므로 세면 IDE와 일반 모드의 PCH 유무가 갈라진다. 다른 안(물리는 번역 단위 수, 논리는 적격이면 PCH)은 논리 명령이 실제 컴파일과 더 벌어져 버렸다([유니티 빌드·PCH](UnityBuildPch.ko.md) 8.1절 같은 날 개정). (4) 낡음 지문은 (타깃, 구성)마다 그 계획의 손으로 쓴 소스 논리 명령(`IdePlanner.Select`와 같은 필터 — 모듈·소스·인자·강제 include·PCH 헤더, 정렬)·실행 파일 경로·두 도구 모음 props 내용 해시의 SHA-256이다(`IdePlanner.Fingerprint`). PJ2의 `RPF0001`이 계획 하나로 같은 값을 계산한다. 상태 파일 판을 2로 올렸다. (5) 상태 파일은 형식을 검증하고(목록·항목이 null이면 없는 것으로 본다), 경로 비교는 호스트 규칙(Windows 대소문자 무시)이며, 루트에서 지우는 파일은 그 솔루션 하나뿐이다. (6) Android 행 NMake 명령에서 `-Compiler`를 뺐다(7.1절). (7) SDK가 `Tools/Rocks.Build.Cli`를 통째로 싣으므로 `Microsoft.VisualStudio.SolutionPersistence`의 MIT 고지와 패키지 NOTICE를 그 출력의 `Licenses/`에 함께 싣는다. 패키지를 쓰는 프로젝트는 NU1901–NU1904를 오류로 올리지 않는다(새 advisory가 빌드를 깨지 않게, 경고는 남는다). (8) ClangCL 확인 vswhere에 `-version [18.0,)`를 준다 |
| 7 | 2026-11-21 | PJ2 구현에서 정한 점. (1) `Build/Tools.ps1`은 직접 실행하면(`Rocks.proj` `BootstrapTools`) IDE 빌드 잠금을 잡고 도구를 준비한 뒤 놓고, `IdeBuild.ps1`·`GenerateProjectFiles.ps1`은 이 파일을 dot-source해 같은 스레드에서 잠금을 잡은 채 도구 준비와 CLI 실행을 한다(이름 있는 mutex는 소유 스레드에서 재진입한다). 도구 지문은 입력(`Programs/Rocks.*` 전체·`Build/Rules`·`Build/Format`·규칙 glob의 `*.Build.cs`·`*.Target.cs`·props·`Build/Toolchain.json`(임베디드 리소스)·`global.json`·`Directory.Build.rsp`, 내용 SHA-256)과 `dotnet --version`이고, 도구 출력 DLL 넷의 해시도 함께 기록해 출력이 없어지거나 바뀌면 다시 빌드한다(`out/dotnet/.rocks-tools.json`). `-ListInputs`가 입력 목록을 내 V13이 MSBuild 평가 항목과 대조한다. (2) `IdeBuild.ps1`의 `-Action`은 Build·Rebuild·Clean이다(Compile은 PJ7). 시험 hook `ROCKS_IDE_BUILD_PROBE`(그 파일을 잠금을 쥔 뒤 쓰고 멈춘다)로 V7을 시간 대기 없이 단언한다. (3) `clean`은 잠금 파일 둘(`.rocks-invocation.lock`·`.native-build.lock`)을 남긴다. 게임 `clean`(`--project`)은 PJ3다. (4) `RPF0001`: `build`·`test`(Windows 호스트, Android 아님, `--pch on`)가 끝나면 그 빌드 계획에서 `IdePlanner.Select`로 고른 논리 명령, 실행 파일 경로, **지금 도구 모음으로 렌더링한** 두 도구 모음 props의 해시로 지문을 계산해 상태 파일의 (타깃|구성) 값과 비교한다. 상태 파일이 없으면 아무것도 하지 않고, 그 키가 없으면(새 타깃) 낡은 것으로 본다. 메시지는 추가·삭제 파일 수를 싣지 않는다(상태 파일에 (타깃, 구성)별 파일 목록을 두지 않는다). (5) 생성 헤더 안내(8.6절)는 타깃마다 한 줄로 모은다: 원천 명령이 가리키는 출력 아래 include 디렉터리 중 없는 것이 있는 (타깃, 구성)을 세어 구성 목록과 함께 알린다. (6) 루트 `GenerateProjectFiles.bat`과 `Build/*.bat`는 `pwsh`가 없으면 안내하고 1을 돌려준다. 루트 bat은 실패하면 `pause`한다. 정책 `build-file-english`의 루트 파일 목록에 더했다. (7) V6은 IntegrationTests가 실제 저장소의 생성 프로젝트(MathBench)로 NMake Build·터미널 0 action·Clean·Rebuild를 확인한다. PATH 앞에 다른 컴파일러를 두는 변형은 이 PC·CI에 두 번째 clang-cl이 없어 하지 않는다 |
| 8 | 2026-11-22 | PJ2 리뷰 반영. (1) `IdeBuild.ps1`이 CLI 전에 `INCLUDE`·`EXTERNAL_INCLUDE`·`LIB`·`LIBPATH`를 지운다(7.1절 환경 정리). V6은 Visual Studio 18이 있으면 VS `MSBuild.exe -t:Build`로 생성 프로젝트를 빌드한 뒤 터미널 빌드 0 action과 `RPF0001` 없음을 단언한다. (2) 도구 빌드 출력을 화면으로 보내고(`| Out-Host`) 종료 코드는 `[int]`로 넘긴다 — 출력이 함수 반환값에 섞여 도구를 다시 빌드한 회차에 실패가 0으로 바뀌었다. V6에 도구 재빌드 + 실패 타깃 사례를 넣었다. (3) macOS 기본 설치 단계에 PowerShell 7(고정 tar.gz)을 넣었다(12절). (4) `RPF0001`은 기본 출력이고 IDE 구성(Debug·Development·Release·Asan)일 때만 비교한다. 상태 파일에 생성 때 컴파일러를 적고(판 3) 빌드 컴파일러가 다르면 비교 대신 note 한 줄을 낸다. (5) 시험: V13 도구 건너뜀·재빌드, V14 `RPF0001`(소스 추가 → 경고, 재생성 → 없음, `--pch off`·`--unity off`·`--adaptive off` → 없음), V6 MSBuild·실패·도구 재빌드. Android·`PublicHeaderChecks`는 조건 코드와 단위 시험(`IdePlanner.Select`가 출력 아래 소스를 뺀다)으로 덮는다. (6) 최상위 실패 `error RB0001`·`RB0002`(7.1절). (7) 시험 hook `ROCKS_IDE_BUILD_PROBE`는 `ROCKS_IDE_BUILD_TEST=1`과 함께일 때만 동작하고, 혼자 있으면 warning을 내고 무시한다. (8) IDE 빌드 잠금 이름은 디스크 철자로 맞춘 루트 경로(소문자)의 해시다. `Local\`은 로그온 세션 단위라 다른 세션(서비스·원격)의 빌드와는 겹칠 수 있다(17절 L18). 기다리는 동안 Ctrl+C로 끊을 수 있게 짧은 주기로 다시 기다린다(순서는 잠금이 정하고 주기는 응답성만 정한다). (9) 도구 지문 입력은 `Rocks.Tools.csproj`의 ProjectReference 닫힘에 든 프로젝트 디렉터리(시험 프로젝트·golden 제외)와 규칙 glob·props·`global.json`·`Directory.Build.rsp`·`.clang-format`·`.editorconfig`·`Build/Toolchain.json`이다. 출력 검사는 도구 출력 디렉터리 넷의 모든 DLL 해시다(의존 DLL 포함). 파일마다 (크기, 수정 시각, 만든 시각)이 기록과 같으면 기록한 해시를 다시 쓰고, 다르면 다시 해시한다(IncrementalStore의 큰 산출물 검증과 같은 규칙). 솔루션 빌드 한 번에 한 번만 확인하는 방법은 Visual Studio가 솔루션 빌드 식별자를 NMake에 넘기지 않아 두지 않았다. (10) `GenerateProjectFiles.bat`은 탐색기에서 실행했을 때만(`%cmdcmdline%`에 자기 경로) 끝에 멈춘다 |
| 9 | 2026-11-22 | PJ3 구현에서 정한 점. (1) 게임 출력 기본은 `GameProjectLayout.DefaultOutput`(`<project>/out/<windows|macos>-<target>-<configuration>`, 소문자)이고 게임 빌드·SDK 게임 규칙 평가·게임 `clean`이 같이 쓴다. 타깃이 없으면 `Game`이다. (2) 제외 규칙은 `GameProjectLayout.IsExcluded` 하나다: 깊이와 무관한 점 디렉터리(점 파일은 입력), 루트 바로 아래 `out/`·`Saved/`(Windows는 대소문자 무관), 루트의 `*.slnx`. `GameInputSnapshot.Capture`는 재귀 열거에서만 이를 적용하고 명시 입력은 그대로 본다. 규칙 탐색 `GameProjectLayout.RuleFiles`는 빼는 디렉터리와 출력 아래로 내려가지 않는다. (3) 게임 `clean`은 `clean --project <dir> --target <T> --configuration <C> [--output]`이다(7.3절). (4) `engineAssociation`은 빈 문자열·공백을 거부한다. `SdkAssociation.Resolve`의 호환 검사는 `InstalledSdk.Incompatibility`(manifest 판·플랫폼·아키텍처·컴파일러·SDK ABI — `Verify`에서 구성 검사를 뺀 것)다. `game --sdk`와 `Rocks.proj` `SdkDirectory`는 선택이다(10.1절). (5) `associate --version on`(값 필수인 CLI 파서), 상대 경로·등록부 철자로 쓴다. 게임 프로젝트 파일 재생성은 PJ5로 미룬다. (6) `sdk --register on|off`(기본 off). 기존 호출자(`Rocks.proj` `InstalledSdk`, IntegrationTests의 SDK 내보내기 넷, CI)는 바꾸지 않았고 본 흐름이 등록 파일이 생기지 않음을 확인한다. `ROCKS_SDK_REGISTRY` 재정의를 더했다(10.2절). (7) SDK는 `Build/` 스크립트 넷을 싣고 `Tools.ps1` SDK 모드는 도구를 빌드하지 않는다. 게임 모드 NMake·생성 스크립트는 PJ5다(12절). (8) 시험: `DescriptorTests`(`engineAssociation`), IntegrationTests `GameProjectTests`(V8·V9·게임 `clean` 경계, `--game-project`), 본 흐름의 기대 출력 경로·잠긴 `.vs` 빌드·등록부 흐름 |
| 10 | 2026-11-22 | PJ2·PJ3 리뷰 반영. (1) 등록부 해석의 호환 후보에 요청 구성을 넣었다(`InstalledSdk.Incompatibility(sdk, toolchain, configuration)`, 10.1절) — 같은 패치의 Debug·Release SDK를 함께 등록하면 모호 오류나 Verify 불일치가 났다. PJ4에서 `Configurations` 판정으로 바꾼다. (2) JSON 등록부 쓰기는 엄격하게 읽고 깨졌거나 판을 모르면 쓰지 않으며, 읽을 수 없는 항목·모르는 칸은 보존한다(10.2절 — 깨진 항목 하나가 다음 등록 때 다른 항목을 모두 지웠다). (3) `Tools.ps1` stamp는 빌드 전 입력 지문·memo와 빌드 뒤 출력 해시를 기록한다(7.2절). (4) `GenerateProjectFiles.bat`은 실패했을 때만 `pause`한다(12절). (5) 등록·해제는 `Local\Rocks.SdkRegistry` mutex 안에서 하고, 해석 후보는 경로로 중복을 지운다. (6) SDK의 `IdeBuild.ps1`이 게임 호출 형태에서도 `RB0002`를 내고, SDK 모드 `-ListInputs`는 해당 없음이다. (7) 명시 입력 디렉터리 아래에는 제외 규칙을 적용하지 않고, `RuleFiles`는 링크를 거부하며 프로젝트 안 SDK를 건너뛴다(10.6절). (8) `out`·`Saved`·`.slnx`는 호스트와 무관하게 대소문자를 가리지 않는다. (9) 시험: `sdk --register on`(본 흐름 Release 내보내기), 구성만 다른 두 SDK를 등록한 뒤 `--sdk` 없는 Debug·Release 게임 빌드가 각 SDK를 고름, JSON 등록부 보존·엄격 쓰기·동시 등록 16개, 레지스트리의 틀린 값, 빈 `HKCU\Software\Rocks\Tests` 정리, 상대 경로 항목 제외, "registered but missing", 같은 경로 두 ID, 명시 입력 디렉터리, 링크 거부, 프로젝트 안 SDK. (10) V6은 `projectfiles --root`를 먼저 돌리고 cmd 경로에 일부러 다른 `INCLUDE`·`LIB` 등을 둔다(16절) |
| 11 | 2026-11-22 | PJ4 구현에서 정한 점(10.3절). (1) `sdk --configurations`(쉼표·세미콜론, 기본 Debug·Development·Release), `sdk`의 `--configuration` 거부, `--output`은 구성 하나일 때만. (2) `SdkExportCommand`·`SdkAssembly`(`Programs/Rocks.Build.Cli/Game/`)가 구성마다 같은 빌드 흐름을 부르고 구성 stage를 합친다. `InstalledSdk.ExportAsync`는 구성 stage 배치 함수 `StageConfiguration`으로 바뀌었다. (3) 구성별 부분은 최상위 `Libraries/`·`Metadata/`이고 모듈 선언도 구성별이다. 한 벌 부분이 다르거나 한 구성에만 있으면 오류다. (4) manifest 14, `Verify`의 구성별 파일 목록 검사, 등록부 해석의 구성 판정. (5) `Rocks.proj` `SdkConfigurations`. (6) 시험: IntegrationTests 본 흐름의 SDK 경로·manifest 기대를 14로 바꿨고, Release 내보내기를 `Debug,Release` 두 구성 SDK로 만들어 두 구성의 게임 빌드(V10), 없는 구성 거부, 구성만 다른 두 등록 SDK의 선택·동률을 확인한다. `GameProjectTests`가 합치기(한 벌 파일 내용 차이·한 구성에만 있는 파일·구성별 경로)와 구성 목록 파서를 시험한다 |
| 12 | 2026-11-22 | PJ3 반영·PJ4 리뷰 반영. (1) 구성 목록에 `+`를 받고 `Rocks.proj`·문서를 `-p:SdkConfigurations=Debug+Release`로 바꿨다(쉼표·세미콜론은 `-p:` 구분자라 MSB1006). IntegrationTests의 두 구성 내보내기를 pwsh에서 그 명령으로 `/t:InstalledSdk`를 부르게 바꿨다. (2) 등록부 잠금: JSON은 잠금 파일(`FileShare.None`), 레지스트리는 `Global\Rocks.SdkRegistry.<SID>`. 자식 프로세스 둘이 같은 등록부에 동시에 등록하는 시험(JSON·레지스트리). (3) 중복 JSON 키를 `JsonException`으로. (4) 링크 판정을 `LinkTarget`으로 좁혔다(10.6절). (5) Verify 해시 범위를 한 벌 + 요청 구성으로 줄이고, 게임 빌드는 한 번만 해시한다(끝은 크기·수정 시각). (6) 구성 사이 소스 snapshot 일관성. (7) 시험: `sdk --configuration` 거부, 여러 구성 `--output` 거부, 첫 구성 뒤 실패의 원자성(시험 hook), `/t:InstalledSdk` 경로, `--register on`은 trailing 내보내기로 옮김. Development 조립은 돌리지 않는다(L19). (8) 10.3절 배치 목록에서 `Templates/`를 뺐고(싣지 않는다) Coverage도 받음을 적었다. `/t:InstalledSdk`의 `Configuration` 경고. (9) `Tools.ps1` memo는 입력 항목을 빌드 전, 출력 DLL 항목을 빌드 뒤 값으로 합친다. (10) `--register on` 등록 실패는 게시 사실과 `sdk-register` 안내로 알린다 |
| 13 | 2026-11-22 | PJ5 구현에서 정한 점(10.4절 구현 단락). (1) `projectfiles --project`, SDK 해석은 구성 조건 없이, 구성은 SDK 구성 ∩ IDE 구성, Android 없음. (2) `GamePlanning`을 빌드와 생성기가 함께 쓰고, 생성기는 규칙을 `out/ProjectFiles/Rules`에 컴파일한다. (3) `IdeModelLayout`으로 주 게임 프로젝트·하위 타깃 프로젝트·`RocksSdk` 헤더 프로젝트. (4) 공통 props `RocksSdkRoot`·`RocksProjectFile`, 게임 프로젝트는 솔루션 폴더 밖(첫 항목은 정할 수 없어 시작 프로젝트는 사용자가 고른다). (5) SDK `IdeBuild.ps1` 게임 모드, `GenerateProjectFiles.ps1 -Project`. (6) `associate`가 생성 상태가 있으면 다시 만든다. (7) 게임 빌드 `RPF0001`은 넣지 않았다(L20). (8) 시험: IntegrationTests 본 흐름에서 템플릿 게임으로 생성(V4 두 번째 실행 0 쓰기), 솔루션 왕복(V3, 게임은 폴더 밖, `RocksSdk` 빌드 제외), MSBuild 평가(V2: NMake 속성·`ClCompile`이 모델과 같은 파일, 모든 항목 파일 존재), NMake Build·Clean·Rebuild를 cmd로 실행(V6). 게임 golden(V5)은 실제 SDK 내용에 묶여 두지 않았다 — 렌더링은 엔진 모드 golden이 덮는다 |
| 14 | 2026-11-22 | PJ4 반영·PJ5 리뷰 반영(10.2·10.4·7.3절). (1) 게임 주 프로젝트 타깃은 정의 디렉터리가 루트 또는 `<루트>/Source`(UE 배치), 이름 충돌은 생성 오류, `RocksSdk` 구성 대체 규칙. (2) `clean`은 `out/ProjectFiles`와 그 아래를 거부하고, 호출 잠금만 있는 출력은 실행 기록이 있을 때만 받는다. (3) `associate` 재생성 실패는 연결 유지·종료 코드 2로 하나, IDE 빌드의 낡은 연결 검사. (4) 게임 생성은 고른 SDK의 CLI로 위임한다. (5) 생성은 시작부터 `out/ProjectFiles` 잠금을 쥐고, model-only는 임시 디렉터리에 규칙을 컴파일한다. (6) IDE 구성마다 구조 검증, 해시는 한 번. (7) 독립 시험 모드도 `ROCKS_SDK_REGISTRY`로 격리하고 기대값을 하나로. (8) 등록부 잠금을 잠금 파일로 통일(레지스트리는 `%LOCALAPPDATA%\Rocks\SdkRegistry.lock`), 공유 위반만 다시 시도. (9) 문서: `-Sdk "$(RocksSdkRoot)."`, `IdeProject` 주석, 게임 모드 ClangCL 경고·생성 헤더 안내, 구성이 없는 SDK의 안내 문구. (10) 시험: 두 배치·이름 충돌(`IdePlanTests`), `clean`의 `out/ProjectFiles`·실행 기록 없는 출력 거부, `associate` 실패 종료 코드, 낡은 연결 검사, 구성 교집합, 동시 생성 |
| 15 | 2026-11-22 | PJ6 구현(11.6절). 템플릿을 `FoundationTemplate`으로 바꾸고 `Template.json`을 더했다. 게임 기본 타깃은 `.rocksproject` 이름이다(`Rocks.proj` `GameTarget` 기본값 삭제). `new-project`(SDK 기본은 CLI가 든 SDK, 예약 이름에 `RocksSdk` 등, 실패 시 정리, 끝에 게임 프로젝트 파일), MetaCompiler `ids reissue`, SDK가 `Templates/`를 싣는다. IntegrationTests의 게임 fixture를 `new-project`로 만들고 V12를 단언한다 |
| 16 | 2026-11-22 | CI 복구와 PJ6 리뷰 반영(11.6절). CI: `Tools.ps1`의 `Get-Item`·`Get-ChildItem`에 `-Force`(Unix 점 파일), 설치 마법사 셀프 테스트 클로저(`-Command` 호출에서 스크립트 함수를 못 봄)를 고치고 IntegrationTests가 셀프 테스트를 `-Command`·`-File` × pwsh·5.1로 돈다. 리뷰: model-only 임시 규칙 디렉터리 정리, Windows 예약 장치 이름·매크로 소문자 이름 거부, 템플릿 산출물 제외와 `ignore` 디렉터리·glob, 템플릿 이름을 담은 이름 거부, `RocksSdk` 기술자 거부, 엄격 UTF-8·BOM 보존·NUL 거부, Unix 잠금 경합 판정, 시험(`Reissue` 단위, SDK 조회·`OwnSdk`, 실패 정리), 문서 정리 |
| 17 | 2026-11-22 | PJ7 구현(7.4절 구현 단락). `SingleFileCompile`(IDE 계획의 논리 명령, `SingleFile/<해시>.obj`·`.json`, 정의 헤더 `WriteIfChanged`, 증분 기록 불변), `build --files`·`game --files`, `NMakeCompileFileCommandLine`(Windows 행, `-WorkingDir`·`$(SelectedFiles)`), `IdeBuild.ps1 -Action Compile`. 시험은 V6·게임 프로젝트 파일 시험에 더했다 |
| 18 | 2026-11-22 | PJ6 반영·PJ7 리뷰 반영(7.4절). `NMakeCompileFileCommandLine`을 `NMakeCompile` item 메타데이터로 옮겼다(속성은 VS가 읽지 않는다). object 경로 인자 규칙을 `CompileObjectArguments`(`/Fo…`, `-o <경로>`)로 모아 낡음 지문·명령 비교·파일 하나 컴파일이 함께 쓴다(macOS 지문 불일치). 시험은 VS 18이 있으면 `MSBuild.exe -t:NMakeCompileSelectedFiles`를 실제로 돌리고, V6에 공백·한글 이름 두 파일을 한 번에 고르는 사례를 더했다. `RPF0001`은 Visual Studio 빌드(`ROCKS_IDE_BUILD=1`)에서만 `warning` 형식이고 터미널에서는 `note RPF0001:`이다 — 저장소 `Directory.Build.rsp`의 `-warnAsError` 때문에 `Rocks.proj` Exec가 경고 형식을 오류로 바꿔, 소스를 더한 뒤 터미널 빌드가 실패했다(V6의 두 파일 사례가 드러냈다) |
| 19 | 2026-11-22 | PJ7 리뷰 나머지(7.4절·10.2절·11.6절). Ctrl+F7은 1단계 원천만, 안 되면 컴파일하는 타깃 이름을 안내. 게임 `--files`의 evidence는 `SINGLE-FILE`. object 해시는 디스크 철자, 없어진 원본의 기록 정리. 소문자 매크로 `small`·`hyper`·`pascal`·`cdecl`. 등록부 잠금 경합 시험(자식이 잠금을 쥐고 신호, 부모가 경합을 관찰한 뒤 gate로 해제). `Running`은 `HasExited` 예외를 실행 중으로 본다. Ctrl+F7 비용을 적었다(L22). 시험: `CompileObjectArguments`·목록 파서 단위, IDE 계획 fixture의 여러 파일·한글/공백 경로·response file·계획에 없는 소스·컴파일 실패 종료 코드·여러 모듈 ordinal 선택, CLI 거부(`test`·`sdk`·`--plan-only`) |
| 20 | 2026-11-22 | PJ8 구현(12절 구현 단락): `shell-register`·`shell-unregister`, 선택기 SDK(유효한 등록 SDK 중 최고 엔진 버전), HKCU Classes·ProjectSelector 값, `cmd.exe` 메뉴 명령(지워진 선택기 안내, 실패 시 멈춤), `SHChangeNotify`, 임시 하위 키 시험 |
| 21 | 2026-11-22 | 최종 리뷰 반영. (1) 탐색기 메뉴는 `SystemFileAssociations` 동사, `.rocksproject` 기본값은 비었을 때만, `OpenWithProgids` 추가, 해제는 우리 값만(PJD44, UE와 다른 점). (2) `RPF0001`은 늘 `warning`이고 `Directory.Build.rsp`에 `-warnNotAsError:RPF0001`. (3) 메뉴는 cmd 없이 `pwsh -File <shim> -Project "%1"`, shim이 SDK의 `GenerateProjectFiles.ps1`을 IDE 잠금 안에서 부르고 SDK·dotnet 없음을 안내. (4) 손상된 manifest는 그 SDK만 버린다. (5) "UE도 SHChangeNotify" 문구 정정. (6) 게임 Ctrl+F7도 컴파일하는 타깃 안내, 안내 단계 catch를 넓힘. (7) 시험: `RemoveOrphans`, 대소문자가 다른 경로의 같은 object 이름, 실제 엔진의 안내 문구, 경로 표시와 `ROCKS_REQUIRE_VS_MSBUILD`, 남의 연결을 심은 등록·해제, shim 회귀(`%`·`!`·공백 경로, SDK·dotnet 없음) |
| 22 | 2026-11-22 | 루트에 `GenerateProjectFiles.ps1`을 더했다(12절, PJD27). bat과 같은 진입점을 PowerShell에서 쓰도록 `Build/GenerateProjectFiles.ps1`에 인자를 그대로 넘기고 종료 코드를 돌려준다. 정책 `build-file-english` 루트 목록에 더했고, IntegrationTests가 인자 전달·종료 코드·5.1 거부를 확인한다 |
