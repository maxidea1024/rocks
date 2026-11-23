# Rocks 빌드 및 검증

처음 받은 컴퓨터에서는 설치 마법사(Windows `Setup.bat`, macOS `./Setup.sh`)를 먼저 실행한다. 순서와 수동 설치는 [빌드 환경 설정](../Docs/Guides/BuildEnvironment.ko.md)에 있다.

## 요구 사항

- .NET 10 SDK `10.0.302` (`global.json`, 최신 패치 롤포워드), Windows/macOS의 Clang C++23 도구 체인.
- `clang-format`(버전은 `Build/Toolchain.json`의 `clangFormat.version` — 지금 21.1.6). `ROCKS_CLANG_FORMAT`에 실행 파일 경로를 지정한다. `pwsh Build/InstallFormatter.ps1 [-ToolchainRoot <디렉터리>]`은 고정 SHA-256을 확인한 후 네이티브 바이너리를 `<디렉터리>/clang-format-<버전>`(기본 `out/toolchains`)에 설치한다. C# 서식 검사의 `dotnet format`은 위 .NET SDK에 들어 있어 따로 설치하지 않는다.
- Windows는 Visual Studio C++ 도구와 Windows SDK가 필요하다. 검증한 Windows 도구 모음은 LLVM 22.1.8과 이미지·Visual Studio의 기본 MSVC다. Windows 최소는 Clang 21이라 LLVM 21.1.6도 빌드되지만 검증 목록 밖으로 기록된다. 검증한 도구 모음·최소 Clang·LLVM 설치 파일 SHA-256은 `Build/Toolchain.json` 한 파일이 정한다([Clang 버전 지원](../Docs/Design/ClangVersionSupport.ko.md) 6절 — MetaCompiler는 형식 철자를 정규 형식 탐침으로 읽어 Clang 21·22의 JSON AST 모양을 모두 받는다). Rocks.Build는 도구 모음을 찾을 때 제조사별 최소 Clang(Apple clang과 업스트림 LLVM을 따로)과, Windows에서는 고른 MSVC STL(`yvals_core.h`)이 요구하는 최소 Clang을 첫 컴파일 전에 확인한다. 검증 목록(정규화한 `--version` 첫 줄 전체) 밖이면 한 줄 알리고 build-evidence에 `toolchainVerified: false`를 남긴 뒤 진행하고, `/p:RequireVerifiedToolchain=on`(Rocks.Build.Cli `--require-verified-toolchain on`, CI가 쓴다)이면 실패한다. MSVC STL 14.52 이상은 Clang 22 미만을 거부하므로(STL1000) 오래된 LLVM을 쓰려면 `ROCKS_MSVC_TOOLSET`(예: `14.44`)로 나란히 설치된 도구 모음을 고른다 — Visual Studio 환경을 만들 때 VsDevCmd `-vcvars_ver`로 넘기고, 그 버전이 아니면 빌드를 시작하지 않는다. `pwsh Build/InstallLlvm.ps1 [-Version <버전>] [-ToolchainRoot <디렉터리>]`은 표에 고정한 LLVM 공식 설치 파일(기본은 표의 `llvm.windows.version`)의 SHA-256을 확인한 후 7-Zip으로 풀어(설치 코드는 실행하지 않는다) `<디렉터리>/llvm-<버전>`(기본 `out/toolchains`)에 두고 그 실행 안에서 `bin`을 PATH 맨 앞에 둔다. 개발 PC는 저장소 밖 고정 위치에 받아 사용자 PATH에 영구히 둔다([빌드 환경 설정](../Docs/Guides/BuildEnvironment.ko.md) 2절). CI(Windows)는 LLVM 22.1.8과 이미지 기본 MSVC로, macOS는 Xcode의 Apple clang으로 돈다(`.github/workflows/foundation.yml`). macOS는 Xcode 도구와 macOS SDK가 필요하며 기본 배포 대상은 macOS 13.0이다.
- Android 교차 빌드의 NDK: CI는 `Build/Toolchain.json`의 `android.ndk.version`(지금 29.0.14206865)을 `pwsh Build/SelectAndroidNdk.ps1`로 골라 `ANDROID_NDK_HOME`에 둔다. 로컬은 `ANDROID_NDK_HOME`(또는 `ANDROID_NDK_ROOT`, `/p:AndroidNdk=<경로>` — Rocks.Build.Cli `--android-ndk`)으로 고르고, 없으면 Android SDK(`ANDROID_HOME` 먼저)의 가장 높은 NDK를 쓴다. Android 최소 Clang(20) 미만 NDK는 거부한다.
- C++ 컴파일러와 C# 컴파일러의 경고, MSBuild 경고는 오류로 취급한다. 경고 비활성화나 단계 축소는 허용하지 않는다.

## 엔진 빌드

```text
dotnet msbuild Rocks.proj /t:Test /p:Configuration=Debug /p:RocksTarget=Foundation
dotnet msbuild Rocks.proj /t:Test /p:Configuration=Development /p:RocksTarget=Foundation
dotnet msbuild Rocks.proj /t:Test /p:Configuration=Asan /p:RocksTarget=Foundation
dotnet msbuild Rocks.proj /t:Test /p:Configuration=Debug /p:RocksTarget=FoundationModular   # Modular(모듈마다 공유 라이브러리) 시험, native 시험과 크래시·Launch 하니스
dotnet msbuild Rocks.proj /t:Test /p:Configuration=Debug /p:RocksTarget=EngineModular       # Engine 층 Modular 시험(엔진 플러그인 시험 제외)
dotnet msbuild Rocks.proj /t:Build /p:Configuration=Release /p:RocksTarget=Engine
```

Visual Studio 솔루션·프로젝트 파일(UE GenerateProjectFiles 대응, [설계](../Docs/Design/ProjectFiles.ko.md)):

- 만들기: 루트의 `GenerateProjectFiles.bat`(탐색기에서 실행해도 된다, 실패하면 멈춘다)이나 PowerShell 7의 `./GenerateProjectFiles.ps1`(같은 인자, `Build/GenerateProjectFiles.ps1`에 넘긴다). 도구를 필요할 때만 빌드한 뒤 루트 `Rocks.slnx`와 `out/ProjectFiles/`를 쓰고 바뀐 파일만 다시 쓴다. 저장소 쪽 리플렉션 생성 헤더가 아직 없으면 어느 타깃을 한 번 빌드하라고 알린다.
- 요구: Visual Studio 2026, 구성 요소 "C++ Clang tools for Windows"(`Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset` — IntelliSense가 ClangCL 모드로 돈다), PowerShell 7(`pwsh`).
- Visual Studio의 빌드·다시 빌드·정리는 `Build/IdeBuild.bat`를 거쳐 `Rocks.Build.Cli build`·`clean`을 부른다. 생성할 때의 clang-cl을 그대로 쓰므로 IDE 빌드 뒤 같은 구성의 `Rocks.proj` 빌드는 0 action이다. 솔루션 빌드는 저장소마다 잠금 하나로 프로젝트를 하나씩 빌드한다.
- 파일 하나 컴파일(Ctrl+F7)은 `build --files <경로>`(게임은 `game --files`)를 부른다: 메타 생성을 맞춘 뒤 그 소스의 논리 명령(IntelliSense와 같은 명령)을 `<출력>/SingleFile/<해시>.obj`로 돌리고, 평소 object·증분 기록은 건드리지 않는다. 헤더는 고를 수 없다.
- 소스·모듈 규칙·도구 모음이 바뀌면 빌드가 `warning RPF0001`(Visual Studio 오류 목록에도 나온다)로 재생성을 알린다. 저장소 `Directory.Build.rsp`의 `-warnNotAsError:RPF0001` 덕분에 `-warnAsError`에서도 빌드를 실패시키지 않는다. 자동으로 다시 만들지 않는다.
- 시작 프로젝트는 처음 한 번 고른다("Set as Startup Project" — 솔루션 직렬화기가 항목을 이름순으로 써서 생성기가 정할 수 없다).
- `Rocks.proj`의 `BootstrapTools`도 같은 잠금과 도구 지문(`Build/Tools.ps1`, `out/dotnet/.rocks-tools.json`)을 쓴다. 그 뒤의 빌드 단계는 잠금 밖이라 터미널 빌드 도중 Visual Studio가 도구 원본 변경으로 도구를 다시 빌드하면 겹칠 수 있다.
- 루트에 `Rocks.proj`와 `Rocks.slnx`가 함께 있으므로 `dotnet msbuild`·`dotnet build`에는 늘 `Rocks.proj`를 적는다(적지 않으면 MSB1011).
- 게임 프로젝트: `pwsh <SDK>/Build/GenerateProjectFiles.ps1 -Project <게임> [-Sdk <SDK>]`(엔진 작업 사본의 같은 스크립트도 된다, CLI `projectfiles --project`)가 `<게임>/<이름>.slnx`와 `<게임>/out/ProjectFiles/`를 쓴다. SDK는 게임 빌드처럼 `engineAssociation`·등록부로 고르고, 솔루션 구성은 SDK가 가진 구성이다. 게임 프로젝트 하나(루트에 정의된 타깃), 하위 디렉터리 타깃 프로젝트, SDK 공개 헤더만 담는 `RocksSdk`(빌드 안 함)가 든다. Visual Studio 빌드는 SDK의 `Build/IdeBuild.bat`로 SDK CLI의 `game`·`clean --project`를 부른다. `associate`로 연결을 바꾸면 이미 만든 게임 프로젝트 파일을 다시 만든다.
- 탐색기 메뉴: `Rocks.Build.Cli shell-register`를 한 번 실행하면 `.rocksproject` 우클릭에 "Generate Visual Studio project files"가 생긴다(HKCU, 관리자 권한 불필요). 메뉴는 등록 SDK 중 엔진 버전이 가장 높은 SDK의 CLI로 생성하고, 프로젝트의 연결이 다른 SDK면 그 SDK의 CLI로 넘긴다. SDK를 지우거나 새로 등록했으면 다시 실행한다. `shell-unregister`로 지운다(다른 프로그램의 `.rocksproject` 연결은 남긴다).
- VS Code·clangd는 생성하지 않는다. 빌드 출력의 `compile_commands.json`을 `--compile-commands-dir=out/<출력>`으로 주면 된다.

### 빌드 구성

UE5.8 Debug·Development·Shipping에 대응한다. 구성마다 `ROCKS_BUILD_DEBUG`·`ROCKS_BUILD_DEVELOPMENT`·`ROCKS_BUILD_RELEASE` 중 정확히 하나를 1로 정의하며(`Base/Compiler.h`가 확인), 모듈은 이 매크로를 직접 정의할 수 없다.

| 구성 | 최적화 | 디버그 정보 | Windows CRT | NDEBUG | ThinLTO | 구성 매크로 | 용도 |
|---|---|---|---|---|---|---|---|
| Debug | 끔 | 있음(PDB) | `/MDd` | 없음 | 없음 | `ROCKS_BUILD_DEBUG` | 단계 실행·변수 확인 |
| Development | `-O2` | 있음(PDB) | `/MD` | 없음 | 없음 | `ROCKS_BUILD_DEVELOPMENT` | 일상 개발·성능 확인 |
| Release | `-O2` | 있음(PDB) | `/MD` | 정의 | 사용 | `ROCKS_BUILD_RELEASE` | 배포(UE Shipping) |
| Asan | 끔 | 있음(PDB) | `/MD` | 없음 | 없음 | `ROCKS_BUILD_DEBUG` | 메모리 오류 검출 |

- 계약 위반 검사(Fatal)는 모든 구성에서 유지한다. 구성 매크로로 빼는 것은 디버그 표시·치트·통계 같은 개발용 기능뿐이다.
- 대상 OS 매크로 `ROCKS_PLATFORM_WINDOWS`·`ROCKS_PLATFORM_MAC`·`ROCKS_PLATFORM_ANDROID`도 빌드가 정확히 하나를 1로 정의하고, `Base/Compiler.h`가 컴파일 대상과 맞는지 검증한다. 아키텍처 매크로 `ROCKS_ARCH_X64`·`ROCKS_ARCH_ARM64`는 `Compiler.h`가 정한다. 모듈은 `ROCKS_BUILD_*`·`ROCKS_PLATFORM_*`·`ROCKS_ARCH_*`를 정의할 수 없다. 사용 규칙(원시 컴파일러 매크로 금지, include 순서, `defined()` 금지)은 [플랫폼 계층](../Docs/Design/PlatformLayer.ko.md)의 "플랫폼 선택과 실패 정책"을 따른다.
- 로그 컴파일 하한 `ROCKS_LOG_COMPILED_MINIMUM`(`LogLevel` 숫자)은 빌드가 타깃의 모든 모듈에 같은 값으로 정의한다. 기본은 Debug·Asan Trace(0), Development Debug(1), Release Info(2)이고, 타깃이 `TargetRules.LogCompiledMinimum`으로 바꿀 수 있다. 모듈별 정의는 ODR 위반이라 거부하며, 설치 SDK export·SDK 게임 타깃은 미리 빌드한 엔진 라이브러리와 맞추려고 재정의를 거부한다([로깅 시스템](../Docs/Design/LoggingSystem.ko.md) 6.4절).
- 모든 compile action은 `-fmacro-prefix-map=<저장소 루트><구분자>=`(clang-cl은 `/clang:` 접두, 설치 SDK 게임 빌드는 SDK 루트도)를 받아 `__FILE__`·`__builtin_FILE()`이 저장소 기준 상대 경로(예: `Core\Source\Runtime\Core\Private\...`)가 된다. 로그·assert 위치에 빌드 장소가 남지 않는다. 디버그 정보 경로는 바꾸지 않는다(`-ffile-prefix-map`이 아니다). 도구 빌드 ID는 루트를 경로 키(`src:`)로 정규화한다.
- Release는 최적화된 모듈을 ThinLTO bitcode로 컴파일한다. 도구 탐색은 archiver·linker를 compiler와 같은 폴더에서 먼저 찾으며, Windows Release는 compiler 폴더의 `llvm-lib`·`lld-link`만 받는다(MSVC `lib.exe`·`link.exe`나 다른 LLVM 설치의 도구는 빌드 전에 거부). macOS는 compiler 옆에 `llvm-ar`가 없으면 Xcode `ar`(libLTO)를 쓴다. POSIX link driver에는 `-flto=thin`을 넘긴다.
- 모든 구성이 디버그 정보를 만든다. Release PDB는 UE Shipping처럼 배포 파일과 분리해 크래시 덤프 심볼화에 쓴다.
- 후속: ThinLTO 캐시(`/lldltocache`·`--thinlto-cache-dir`)가 없어 Release relink는 backend를 모두 다시 생성한다. 병렬 link 수와 LTO thread 수 조정도 측정 후 정한다.
- 강제 인라인(UE `FORCEINLINE` 대응)은 쓰지 않는다. 인라인 판단은 Clang 최적화(`-O2`, Release ThinLTO의 모듈 간 인라인)에 맡기고, 정책 검사(`forced-inline`)가 제품 소스의 `always_inline`·`__forceinline`을 거부한다. 최적화 인자가 정의하는 미리 정의 매크로(`__OPTIMIZE__`·`__OPTIMIZE_SIZE__`·`__NO_INLINE__`)도 시험 소스를 포함해 쓰지 않는다(주석·문자열 안은 허용, 구성별 최적화 여부를 검증하는 `Base/Tests/BaseTests.cpp`만 예외). Visual Studio IntelliSense는 최적화 인자를 받지 않아 그 분기를 빌드와 다르게 보기 때문이다(정책 `ide-invisible-macro`, [프로젝트 파일 설계](../Docs/Design/ProjectFiles.ko.md) 8.2절). 성능 문제는 측정한 뒤 알고리즘·데이터 배치·모듈 최적화 설정으로 다룬다.

### 유니티 빌드·PCH

컴파일 시간을 줄이는 세 가지가 기본으로 켜져 있다([설계](../Docs/Design/UnityBuildPch.ko.md), 구현 설명은 [Rocks.Build](../Programs/Rocks.Build/README.ko.md)).

- **유니티 빌드**: 규칙에서 `Unity => UnityMode.On`으로 켠 모듈의 C++ 소스를 묶음 번역 단위로 컴파일한다. 켠 모듈은 정책 검사(`unity-file-local-name` 등)가 묶여도 안전한지 확인한다. 리플렉션 생성 소스는 모든 모듈에서 자기들끼리 묶는다.
- **PCH**: Core(`Core/CoreSharedPch.h`)와 ObjectRuntime(`Object/ObjectSharedPch.h`)이 공유 PCH를 제공하고, 그 모듈에 닿는 모듈이 자기 인자로 미리 컴파일해 쓴다. 소스와 헤더는 PCH 헤더를 include하지 않는다(빌드가 강제 include한다).
- **작업 파일 분리**: `git status`가 수정·추가로 알리는 소스는 묶음에서 빼 단독으로 컴파일한다(고치는 파일의 반복 시간을 줄인다).

```text
dotnet msbuild Rocks.proj /t:Build /p:RocksTarget=Foundation /p:Unity=off /p:Pch=off    # 검증 변형: 소스마다 단독 번역 단위, PCH 없음
dotnet msbuild Rocks.proj /t:Test /p:RocksTarget=Foundation /p:Adaptive=off             # CI: 작업 파일 분리 끔(작업 트리 상태와 무관한 구성)
dotnet msbuild Rocks.proj /t:Build /p:RocksTarget=Foundation /p:Timing=on               # -ftime-trace 집계: out/<...>/Timing/Foundation.timing.json
```

CLI 옵션은 `--unity on|off`, `--pch on|off`, `--adaptive on|off`, `--timing on|off`다. object는 변형마다 다른 디렉터리(`obj/<모듈>/<변형>/`)에 두므로 옵션을 바꿔도 서로의 결과를 덮지 않는다. 묶인 소스는 PCH와 다른 소스의 include에 기대어 컴파일될 수 있으므로, include를 고친 뒤에는 검증 변형으로 한 번 빌드해 본다(CI가 Windows에서 돈다).

### 모듈 최적화 재정의

모듈 규칙의 `Optimization`으로 구성의 최적화 여부만 바꾼다(UE `ModuleRules.OptimizeCode` 대응). CRT·디버그 정보·구성 매크로·NDEBUG는 구성을 따르므로 같은 실행 파일 안에서 ABI가 섞이지 않는다.

```csharp
public sealed class Physics : EngineModuleRules
{
    protected override string ModuleName => "Physics";
    protected override string[] PublicDependencies => ["Core"];
    // Development에서 이 모듈만 단계 실행한다(커밋하지 않는다).
    protected override CodeOptimization Optimization => CodeOptimization.Never;
}
```

- `Default`: 구성을 따른다. `Never`: 모든 구성에서 끈다(Release에서는 ThinLTO에서도 빠진다). `Always`: Debug에서도 켠다(Debug에서 너무 느린 계산 모듈).
- 파일 일부만 끄려면 `ROCKS_DISABLE_OPTIMIZATION` … `ROCKS_ENABLE_OPTIMIZATION`(UE `UE_DISABLE_OPTIMIZATION` 대응)을 임시로 쓴다. Release에서는 컴파일 오류이며, 제품 소스에 남으면 정책 검사(`optimization-override`)가 거부한다. 테스트 소스는 예외다.
- 모듈 인자로 최적화 수준(`-O0~3`·`-Os`·`-Oz`·`-Og`·`-Ofast`·`--optimize`·`/O1`·`/O2`·`/Od`·`/Og`·`/Os`·`/Ot`·`/Ox`·`/Ob*`)과 LTO(`-flto*`·`-fno-lto`, link의 `/opt:lldlto*`·`-Wl,--lto*`·`-Wl,--thinlto*`)를 바꾸는 것은 거부한다. `/Oy-`·`/Oi`·`-fno-omit-frame-pointer`처럼 수준을 바꾸지 않는 인자는 받는다.
- 정책 검사(`optimization-override`)는 제품 소스의 `ROCKS_DISABLE_OPTIMIZATION`·`#pragma (clang) optimize`·`optnone`과 *.Build.cs에 커밋된 `CodeOptimization.Never`를 거부한다.

`Compiler`, `Jobs`, `NativeOutput` 속성으로 컴파일러 경로, 병렬 작업 수, 출력 위치를 지정할 수 있다. `RocksTarget=Core`는 오브젝트 모듈을 제외한 기반 테스트를 선택한다. 출력과 증거는 기본적으로 `out/` 아래에 기록한다.

작업 사본은 깊은 경로나 드라이브 루트(`subst X: <경로>` 뒤 `X:\`)에 두어도 된다. 명령줄이 Windows 한계(32,767자)를 넘는 compile·archive·link는 빌드 도구가 response file(`@파일`)로 넘기며, 증분 key·`compile_commands.json`·도구 빌드 ID는 펼친 인자를 그대로 쓴다. `-p:GameDirectory=X:\`처럼 끝이 역슬래시인 디렉터리 속성도 그대로 쓸 수 있다. `NativeOutput`은 증거 위치 때문에 작업 사본의 `out/` 아래여야 한다. 자세한 규칙은 [Rocks.Build](../Programs/Rocks.Build/README.ko.md#긴-명령줄response-file)를 따른다.

### 빌드 환경과 빌드 주입 상수

- 빌드는 프로세스 환경을 물려받지 않고 허용 목록(VS·Windows SDK·Xcode 선택·SOURCE_DATE_EPOCH 같은 Keyed 변수와 PATH·TEMP 같은 Runtime 변수)만 컴파일러에 넘긴다. Keyed 변수만 증분 key에 들어가므로 `dotnet msbuild`가 실행마다 바꾸는 세션 변수가 재빌드를 일으키지 않는다. 컴파일러·링커 동작을 바꾸는 변수(`CL`·`_CL_`·`LINK`·`_LINK_`·`CCC_OVERRIDE_OPTIONS`·`COMPILER_PATH`·`CPATH`·`C_INCLUDE_PATH`·`CPLUS_INCLUDE_PATH`·`OBJC_INCLUDE_PATH`·`LIBRARY_PATH`)가 있었으면 `Build environment: removed …` 한 줄로 알린다. 기본 config 파일은 받는 컴파일러면 `--no-default-config`로 끈다(받지 않는 컴파일러가 config 파일을 읽으면 오류).
- 모듈 인자로 FP 모델(`-ffast-math`·`/fp:*`·`-ffp-*`·ISA 선택 `-m*` 등)·`-mllvm`·`-Xlinker`·`-Ofast`·`-Xclang`·config 파일·링커·대상 선택·절대 경로를 넘길 수 없다. FP 모델은 구성·모듈마다 달라지지 않는다.
- Core(`ProvidesBuildInfo`)에 닿는 실행 파일마다 `<출력>/BuildInfo/<실행 파일>.BuildInfo.cpp`가 생겨 `rocks::build::ToolchainFingerprint()`(툴체인 지문 — 에셋 너겟 키, 구성 무관)·`ToolBuildId()`(도구 빌드 ID — 진단·캐시 무효화, 패키지에 쓰지 않음)·`CurrentEngineVersion()`(엔진 버전 — 아래 [엔진 버전](#엔진-버전))을 정의한다(`Core/Build/BuildInfo.h`). 원문은 같은 폴더의 `ToolchainFingerprint.txt`·`<실행 파일>.BuildInfo.txt`이고 `action-report.json`의 `ToolchainFingerprint`·`ToolBuildIds`에도 실린다. 실행 파일이 Core에 닿으면 입력 byte가 하나만 바뀌어도 생성 TU와 link가 다시 돈다. 자세한 규칙은 [Rocks.Build](../Programs/Rocks.Build/README.ko.md#빌드-주입-상수buildinfo)와 [ReflectionFoundations](../Docs/Design/ReflectionFoundations.ko.md) 8.1절을 따른다. Modular에서는 같은 TU가 실행 파일 대신 Core 공유 라이브러리에 들어간다([빌드 타깃](../Docs/Design/BuildTargets/03-Design/03.08-LinkType.ko.md) 3.8절 BS8d1).
- Core(`ProvidesModuleTable`)에 닿는 실행 파일마다 모듈 표 TU `<출력>/GeneratedSources/<실행 파일>/<실행 파일>.ModuleTable.cpp`가 생겨 `rocks::GeneratedModuleTable()`을 정의한다([모듈 로더](../Docs/Design/ModuleLoader.ko.md) 3.2절). 실행 파일에 닿는 C++ 모듈(서드파티·header-only·Core 아래 모듈 제외)을 의존 순서로 나열하고 모두의 `RocksModuleEntry_<모듈>()`을 참조하므로, 모듈 cpp에 `ROCKS_IMPLEMENT_MODULE(클래스, 모듈)`이 없으면 링크 오류다. 빌드는 그 모듈들의 소스에 `ROCKS_MODULE_NAME=<모듈>`과 `ROCKS_MODULE_TYPES=0|1`(리플렉션 생성 소스가 있으면 1 — 진입 기술이 모듈 생성 소스의 `RegisterModuleOwnTypes`를 싣는다)을 정의하며(규칙은 정의할 수 없다) 매크로 이름이 다르면 `static_assert` 오류다. 리플렉션 헤더만 있고 손으로 쓴 소스가 없는 모듈도 진입점 모듈이며, 그 진입점 TU `<출력>/GeneratedSources/<모듈>/<모듈>.ModuleEntry.cpp`는 빌드가 만든다([모듈 로더](../Docs/Design/ModuleLoader.ko.md) 3.10절). 대소문자만 다른 모듈 이름과 대소문자가 다른 의존 철자는 거부한다. Modular에서는 표 TU 대신 같은 행을 `<출력>/bin/<타깃>[-<플랫폼>-<구성>].modules.json` manifest에 쓰고, 진입점 모듈의 공유 라이브러리가 export 진입점 `RocksModuleEntry`를 낸다(BS8d1).
- 규칙은 저장소 입력에서 만든 번역 단위를 `NativeModuleInput.GeneratedSources`(파일 이름·내용)로 넘길 수 있다. 빌드가 출력 잠금 아래에서 `<출력>/GeneratedSources/<모듈>/<파일 이름>`에 내용이 바뀔 때만 써서 그 모듈 소스로 컴파일한다(BuildInfo와 같은 방식, 내용이 같으면 다시 컴파일하지 않는다). 이름은 구분자 없는 `.cpp`·`.c`이고 내용은 호스트 경로를 담지 않는다. 셰이더 도구 표(`Developer/ShaderToolTable`)가 Engine 루트 binary manifest에서 이렇게 만든다(아래 [미리 빌드한 바이너리](#미리-빌드한-바이너리bootstrap)).

### 엔진 버전

엔진 버전의 단일 출처는 `Build/Build.version`이다(UE `Engine/Build/Build.version` 대응, 0.1.0에서 시작). 형식은 UTF-8 한 줄 `{ "MajorVersion": 0, "MinorVersion": 1, "PatchVersion": 0 }`과 LF이며, 세 키만 정확히 한 번씩 0–65535 정수로 적고 주·부를 모두 0으로 두지 않는다. 다른 키·중복·주석·후행 쉼표·1 KiB 초과는 빌드 오류다. 빌드 CLI가 실행마다 한 번 읽어 생성 TU의 `rocks::build::CurrentEngineVersion()`과 도구 빌드 ID manifest의 `engine-version:` 줄, 설치 SDK manifest(`rocks-sdk.json`의 `EngineVersion`)에만 넣는다. 버전 헤더·매크로·compile 정의는 두지 않으므로 버전을 올려도 각 실행 파일의 생성 TU와 link만 다시 돈다. 설치 SDK 게임 빌드는 저장소 파일 대신 SDK manifest의 값을 쓰고, 게임 `.rocksproject`의 `engineVersion`("주.부")이 그 주·부와 다르면 멈춘다. 자세한 규칙은 [ReflectionFoundations](../Docs/Design/ReflectionFoundations.ko.md) 8.2절을 따른다.

올리는 규칙:

- **Patch**: 데이터 축(패키지 formatVersion·섹션 schemaVersion·타입 스키마·커스텀 버전·클래스 기본값 지문·쿠커 버전)과 공개 API를 바꾸지 않는 수정만 담은 릴리스. 같은 주·부의 모든 패치는 서로의 에디터 패키지를 읽는다(호환 판정은 패치를 보지 않는다).
- **Minor**: 릴리스 경계 — 데이터 축이나 공개 API가 바뀐 뒤의 첫 릴리스에 올린다(개발 중 변경마다 올리지 않는다). 더 새 부 버전이 저장한 에디터 패키지는 이전 엔진이 `NewerVersion`으로 거부하고, 게임 프로젝트는 `engineVersion`을 새 주·부로 바꿔야 연결된다.
- **Major**: 1.0 이후에 쓴다.
- 올릴 때 `Build/Build.version`과 게임 템플릿 `Templates/Game/Foundation/FoundationTemplate.rocksproject`의 `engineVersion`(주·부)을 같은 변경에서 함께 바꾼다(`Rocks.Build.Tests`가 둘의 일치와 파일의 한 줄 정규형을 확인한다).
- 엔진 버전은 쿠킹 패키지·cookKey·에셋 너겟 키에 들어가지 않는다. 쿠커 출력의 의미가 바뀌면 엔진 버전이 아니라 쿠커 버전을 올린다.

MSBuild는 .NET 도구와 C# 규칙을 빌드하고 단계를 조직한다. `Rocks.Build`가 네이티브 작업 그래프, 입력·출력 해시, 증분 여부, 실패 복구를 담당한다. `Build/Rules/*.Build.cs`는 모듈을, `*.Target.cs`는 실행 대상을 정의한다. 규칙은 신뢰된 프로젝트 코드이며 동적으로 읽는 외부 입력은 명시해야 한다.

## 설치 SDK와 게임 빌드

```text
dotnet msbuild Rocks.proj /t:InstalledSdk /p:SdkDestination=C:/work/rocks-sdk/0.1.0
dotnet msbuild Rocks.proj /t:Game /p:SdkDirectory=C:/work/rocks-sdk/0.1.0 /p:GameDirectory=C:/work/my-game /p:Configuration=Debug
```

설치 SDK 하나가 여러 구성을 담는다. 기본은 Debug·Development·Release이고 `/p:SdkConfigurations=Debug+Release`(CLI `sdk --configurations Debug+Release`, 쉼표도 된다)로 고른다 — MSBuild `-p:`는 쉼표·세미콜론을 속성 구분자로 읽으므로 `+`로 잇는다. Asan·Coverage는 적을 때만 들어간다. `/t:InstalledSdk`는 `/p:Configuration`을 쓰지 않는다(Debug가 아니면 경고). 구성마다 엔진을 기본 출력(`out/dotnet-<platform>-<target>-<구성>`)에 빌드한 뒤 한 SDK로 조립한다. 구성과 무관한 부분(`Headers/`·`Tools/`·`Licenses/`·`Build/` 스크립트 등)은 한 벌이고, 라이브러리와 리플렉션 descriptor·명세는 `Configurations/<구성>/Libraries/`·`Configurations/<구성>/Metadata/<모듈>/`에 있다. 한 벌 파일이 구성마다 다르면 내보내기 오류다. 게임 빌드는 요청 구성이 SDK에 없으면 가진 구성 목록과 함께 멈춘다([Visual Studio 프로젝트 파일](../Docs/Design/ProjectFiles.ko.md) 10.3절).

새 게임은 템플릿에서 만든다: `Rocks.Build.Cli new-project --template Foundation --name <이름> --destination <부모> [--sdk <SDK>] [--projectfiles on|off]`(SDK가 싣는 `Templates/Game/<이름>`, 경로도 된다). 템플릿 이름(`FoundationTemplate`)의 세 철자를 새 이름으로 바꿔 복사하고, `engineVersion`을 SDK에 맞추고, identity 명세를 `ids reissue`로 새 ID로 바꾸며, Windows에서는 Visual Studio 솔루션도 만든다. 게임 빌드는 `--target`(`/p:GameTarget`)이 없으면 `.rocksproject` 이름의 타깃을 빌드한다.

게임 출력은 `<게임>/out/<platform>-<target>-<configuration>`(소문자, 예: `out/windows-game-debug`)이다. 게임 입력 snapshot과 게임 규칙 파일 탐색은 이름이 `.`으로 시작하는 디렉터리(`.vs`·`.git` 등), 게임 루트 바로 아래의 `out/`·`Saved/`, 루트의 `*.slnx`를 보지 않는다. 그 안의 파일을 규칙이 읽으면 명시 입력으로 적는다. 게임 출력 하나를 비우려면 `Rocks.Build.Cli clean --project <게임> --target <T> --configuration <C>`를 쓴다(잠금 파일 둘만 남긴다).

`/p:SdkDirectory`(CLI `game --sdk`)는 선택이다. 없으면 게임 `.rocksproject`의 `engineAssociation`(SDK 경로 또는 등록 ID)으로, 그것도 없으면 이 컴퓨터에 등록한 SDK 중 `engineVersion`의 주·부가 같고 이 도구 모음과 호환되는 최고 패치로 고른다(최고 패치가 둘이면 오류). 등록은 명시적으로만 한다: `Rocks.Build.Cli sdk-register --sdk <SDK>`(또는 내보내기 때 `sdk --register on`), `sdk-unregister --sdk <SDK>|--id <ID>`. 등록부는 Windows `HKCU\Software\Rocks\Sdks`, macOS `~/.config/Rocks/Sdks.json`이고, 환경 변수 `ROCKS_SDK_REGISTRY`에 JSON 파일을 주면 그 파일을 쓴다(시험·CI). 연결은 `Rocks.Build.Cli associate --project <게임> (--sdk <SDK> | --id <ID> | --version on)`이 쓰거나 지운다. 경로·ID는 컴퓨터마다 다르므로 공유 프로젝트는 보통 연결 없이 둔다([Visual Studio 프로젝트 파일](../Docs/Design/ProjectFiles.ko.md) 10절).

SDK 대상 디렉터리는 새 경로여야 한다. SDK의 파일 목록과 내용, 플랫폼·아키텍처·컴파일러 및 SDK ABI·구성이 일치하지 않거나, 게임 `.rocksproject`의 `engineVersion`이 SDK 엔진 버전의 주·부와 다르면 게임 빌드를 중단한다. 게임 소스와 출력은 SDK 밖에 둔다. `Templates/Game/Foundation`은 C# 모듈·타깃 규칙과 생성된 CLASS/PROPERTY를 사용하는 예제다. 현재 SDK 내보내기는 외부 라이브러리와 임의 컴파일·링크 옵션이 있는 모듈을 지원하지 않으며 해당 선언을 오류로 거부한다.

SDK는 타깃에서 도달하는 모듈만 내보낸다. 고정 버전 외부 라이브러리(`Core/Source/ThirdParty`) 모듈은 정적 라이브러리로 들어가며 manifest `licenses`의 모든 라이선스 원문을 `Licenses/<모듈>/<원본 디렉터리 기준 경로>`에 함께 복사한다(예: `Licenses/Zstd/LICENSE`와 사전 생성기의 MIT 고지 `Licenses/Zstd/lib/dictBuilder/divsufsort.h`). 원본 안 경로를 유지하므로 이름이 같은 원문도 겹치지 않는다. BSD 계열(LZ4·zstd)과 MIT(zstd divsufsort)는 바이너리 배포 시 고지 의무가 있으므로 게임 배포 패키지에도 이 고지를 포함한다.

## 외부 라이브러리(C 원본)

빌드 도구는 확장자가 정확히 `.c`인 번역 단위를 C17로 컴파일한다(clang-cl `/TC /clang:-std=c17`, POSIX `-x c -std=c17`). 경고·FP·구성·CRT·최적화 프로필은 C++와 같고 C++ 전용 플래그만 빠진다. C++ 번역 단위의 인자는 바뀌지 않는다. `.c`는 외부 라이브러리 폴더(Core가 쓰면 `Core/Source/ThirdParty/<Lib>/`, 엔진·도구 전용이면 `Engine/Source/ThirdParty/<Lib>/`) 아래에서만 허용하며(compile DB 감사), 모듈 인자로 언어·표준을 바꿀 수 없다. manifest 종류는 `code`·`data`·`headers`(헤더 전용, 모듈 규칙 없음)·`binary`(Rocks가 컴파일하지 않는 미리 빌드한 바이너리)다([셰이더 시스템](../Docs/Design/ShaderSystem.ko.md) 8절).

라이브러리 폴더는 `<Lib>.Build.cs`(`ThirdPartyModuleRules` 상속, 이름만 적는다), `ThirdParty.json`(단일 출처), `<lib>-<version>/`(릴리스 원본을 byte 그대로)로 구성한다. 업그레이드 절차:

1. 새 릴리스를 받아 서명·해시를 확인하고 필요한 파일만 새 버전 디렉터리에 그대로 복사한다. 이전 버전 디렉터리를 지운다.
2. `ThirdParty.json`의 `version`·`upstreamDirectory`·`source`·경로를 고친 뒤 `dotnet out/dotnet/bin/Rocks.ToolTests/Debug/net10.0/Rocks.ToolTests.dll third-party --root . --write`로 `files`(SHA-256)와 `pragmaDirectives`를 다시 계산한다. 인자 없이 실행하면 차이만 보고한다.
3. 새로 기록된 진단 지시문을 검토한다. `native` 단계의 `Native.ThirdPartyDirectivesHideNoWarnings`가 지시문을 지운 사본도 경고 0인지 실제 compile DB 인자로 확인한다.
4. 압축 결과 golden(`Compression_DeterministicOutputGoldens`)이 바뀌면 의도한 변경인지 확인하고 갱신한다.

## 미리 빌드한 바이너리(bootstrap)

DXC·SPIRV-Cross·SPIRV-Tools처럼 Rocks 빌드가 컴파일하지 않는 도구는 manifest `kind: binary`로 `Engine/Source/ThirdParty/<Lib>/`에 둔다([셰이더 시스템](../Docs/Design/ShaderSystem.ko.md) 8절). 저장소에는 공개 헤더·라이선스·`Build/` 오프라인 빌드 스크립트·manifest만 두고, 바이너리는 사용자 단위 내용 주소 저장소(Windows `%LOCALAPPDATA%\Rocks\ThirdParty\<SHA-256>\`, macOS `~/Library/Application Support/Rocks/ThirdParty/<SHA-256>/`)에 명시적인 bootstrap 명령으로만 받는다. 기본 빌드·시험과 Foundation CI는 bootstrap 없이 돈다(헤더는 저장소에 있다). 워커가 도구를 여는 실행·시험만 bootstrap이 필요하다.

```text
dotnet msbuild Rocks.proj /t:ThirdPartyBootstrap
dotnet out/dotnet/bin/Rocks.Build.Cli/Debug/net10.0/Rocks.Build.Cli.dll bootstrap --root . [--library <Lib>] [--mirror <dir>] [--offline true] [--store <dir>] [--windows-sdk <dir>]
```

- 받은 archive는 저장소 아래 임시 디렉터리에서 archive·파일 SHA-256과 크기를 검사한 뒤 최종 디렉터리로 옮긴다. 검사에 실패하거나 끊기면 최종 디렉터리는 생기지 않고, 다음 실행이 남은 임시 디렉터리를 지우고 다시 받는다. macOS는 검사 뒤 quarantine 속성을 지운다. 다른 bootstrap이 저장소를 쓰고 있으면 기다리지 않고 실패한다.
- 이미 있으면 검사만 하고 받지 않는다. `--offline true`는 받지 않고 저장소만 검사한다. 저장소 파일이 manifest와 다르면 고치지 않고 오류다(그 디렉터리를 지우고 다시 실행한다).
- `--mirror`: asset 이름(`<Lib>-<버전>-r<revision>-<호스트>.tar.gz`)으로 archive를 찾는 디렉터리. 없으면 manifest URL(저장소 GitHub Releases)에서 받고, 인증 없이 받을 수 없으면(비공개 저장소) `gh release download`가 사용자의 `gh` 로그인으로 받는다.
- FXC(`D3DCompiler`)는 버전을 고정한 Windows SDK의 버전별 사본 `bin/10.0.22621.0/x64/d3dcompiler_47.dll`을(버전 구분 없는 `Redist` 본은 더 새 SDK가 덮어쓴다, [셰이더 시스템](../Docs/Design/ShaderSystem.ko.md) 8절 2항) SHA-256 검사 뒤 저장소로 복사한다. Apple Metal 도구(`MetalToolchain`, `status: system`)는 받지 않는다(최소 버전만 고정).
- 오프라인 도구 빌드: `pwsh Engine/Source/ThirdParty/<Lib>/Build/Build.ps1`(Windows x64), `sh Engine/Source/ThirdParty/<Lib>/Build/Build.sh`(macOS arm64)가 manifest `build`의 고정 원본을 받아 SHA-256을 검사하고 CMake·Ninja로 빌드해 archive와 `<asset>.host.json`(manifest `hosts` 항목)을 만든다. CMake·Ninja·Python 3·Visual Studio C++ 도구(macOS는 Xcode)가 필요하다. 수동 실행 workflow `.github/workflows/third-party-tools.yml`이 두 호스트에서 빌드하고, 입력 `publish`가 true면 Release에 올린다.
- 도구 표와 열기([셰이더 시스템](../Docs/Design/ShaderSystem.ko.md) 9절): `Developer/ShaderToolTable`은 빌드 대상 호스트의 도구 이름·저장소 디렉터리 이름·파일 SHA-256·role을 생성 TU로 가진다(메타데이터만, 파일을 열지 않는다). `Developer/ShaderToolLoader`(워커 자리)가 저장소 파일의 크기·SHA-256을 표와 대조한 뒤 `DynamicLibrary::Open`으로 절대 경로에서 열고 함수 표(DXC·FXC·SPIRV-Cross·SPIRV-Tools)를 얻는다. 저장소에 없으면 `NotBootstrapped`, 다르면 `DigestMismatch`로 bootstrap을 안내한다. 도구를 여는 EngineTests 시험은 저장소가 없으면 SKIP이다.
- 정책 검사: 도구 표는 ShaderCompiler·워커·ShaderFormat* 모듈만(`shader-tool-table`), 도구 binary 로드·binary 헤더·`DynamicLibrary::Open`은 워커·ShaderFormat*·ShaderToolLoader만(`shader-tool-binary-load`) 쓴다. Game 타깃(`TargetRules.Type`, 기본 Game, 도구·시험은 Program)은 Release 구성에서 Developer 모듈에 닿거나 d3dcompiler·dxcompiler를 링크하거나 Windows에서 OpenGLGraphics를 링크하면 안 된다(`release-*`). 알려진 예외 세 건(SH2까지 D3D12Graphics의 d3dcompiler와 Sandbox의 Windows OpenGLGraphics, SH5까지 D3D11Graphics의 d3dcompiler)은 `policy` 실행 때 `note: known Release exception until SH2`(또는 `SH5`)로 출력된다.

## 메타 선언

`ROCKS_CLASS()` 또는 `ROCKS_CLASS(Abstract)`를 `class`/`struct` 바로 앞에 두고, 리플렉션할 필드에 `ROCKS_PROPERTY(...)`를 붙인다. 선언에 ID나 스키마 버전 숫자를 넣지 않는다. 비공개 필드에는 `ROCKS_GENERATED_BODY()`가 필요하다. 타입 마커와 클래스 사이에는 공백·주석만 허용한다. 조건부 선언은 마커와 클래스를 같은 조건 블록에 둔다. 미지원 타입·상속·선언 위치·지정자와 모호한 선언은 오류다.

리플렉션 헤더는 모듈 규칙에 적지 않는다. 빌드가 모듈 폴더(`ModuleDirectory`) 안 public·private include 디렉터리의 헤더에서 줄 첫머리 표지 `ROCKS_CLASS(`·`ROCKS_STRUCT(`·`ROCKS_ENUM(`을 찾아 생성한다(UE UBT 방식 — [설계](../Docs/Design/ReflectionFoundations.ko.md) 1.10절). 철자는 그 include 디렉터리 기준이다. 헤더를 더하거나 표지를 붙이면 규칙을 고치지 않아도 된다. 모듈 소스 폴더 `Private/`(include 디렉터리가 아님)에 표지 헤더를 두면 빌드가 거부한다(RMC2003). `ModuleRules`를 직접 상속하는 게임 모듈은 명세 위치 `ModuleDirectory`를 적는다. 식별자는 모듈 폴더의 소스 관리되는 `<모듈>.rocksids.json`에 보관하며 일반 빌드는 파일을 수정하거나 누락된 ID를 자동 발급하지 않는다. 새 선언은 메타컴파일러의 명시적인 저작 명령으로 등록하고, 이름을 바꿀 때는 새 ID를 만들지 말고 `ids rename`을 사용한다. 삭제할 선언은 `ids retire`로 폐기하고, 타입을 다른 모듈로 옮길 때는 `ids move`로 ID를 유지한다. 모듈의 활성 명세 항목이 소스에 없거나, 두 명세에 같은 ID나 같은 이름이 있으면 빌드가 실패한다.

```text
dotnet out/dotnet/bin/Rocks.MetaCompiler/Debug/net10.0/Rocks.MetaCompiler.dll ids add --manifest MyGame.rocksids.json --type sample::Character --field sample::Character::health
dotnet out/dotnet/bin/Rocks.MetaCompiler/Debug/net10.0/Rocks.MetaCompiler.dll ids rename --manifest MyGame.rocksids.json --kind field --old sample::Character::health --new sample::Character::hitPoints
dotnet out/dotnet/bin/Rocks.MetaCompiler/Debug/net10.0/Rocks.MetaCompiler.dll ids move --from Old.rocksids.json --to New.rocksids.json --type sample::Character
```

빌드는 리플렉션 헤더가 있는 모듈을 의존 순서로 하나씩 생성한다. 생성 헤더는 `<출력>/Generated/<모듈>/Public|Private/<원본 철자의 .h를 .generated.h로>`이며 그 모듈의 public·private include로 전파된다(`#include <Module/Type.generated.h>`). 모듈 생성 헤더 `<모듈>/<모듈>.module.generated.h`는 `rocks::generated::<모듈>::RegisterModuleTypes`(의존 모듈부터 public 타입을 등록한다)의 선언만 담고, private 헤더의 타입을 등록하는 `RegisterPrivateModuleTypes`는 `<모듈>/<모듈>.private.module.generated.h`가 선언한다. 두 함수와 모듈 로더가 부르는 자기 타입 함수 `RegisterModuleOwnTypes`(의존 모듈 없이 이 모듈 타입만)의 정의는 모듈 생성 소스 `<출력>/Generated/<모듈>/Source/<모듈>.module.generated.cpp`에 있고 빌드가 그 모듈의 번역 단위로 한 번 컴파일한다(손으로 쓴 소스가 없는 리플렉션 모듈도 그래서 정적 라이브러리가 된다). 타입별 생성 헤더도 선언·컴파일 타임 형질·inline `Trace`만 담고, 스키마·연산 표·등록·복사·검증·정적 검사 같은 본문은 타입별 생성 소스 `<출력>/Generated/<모듈>/Source/Public|Private/<원본 철자의 .h를 .generated.cpp로>`에 있어 같은 방식으로 한 번 컴파일된다. 본문의 계약 위반(생성·소멸 noexcept, 접근 권한 등)은 이 compile action이 잡고 빌드가 컴파일러 출력 뒤에 `RMC3000` 안내를 붙인다. `--verify-metadata-sources on`을 주면 메타 단계가 생성 소스까지 게시 전에 컴파일해 원본 위치와 함께 거부한다. `GeneratedType<T>`를 쓰는 코드는 그 타입의 생성 헤더를 직접 include하고, 손으로 쓴 헤더는 모듈 생성 헤더를 include하지 않는다(정책 검사 `module-generated-header-include` — [리플렉션 생성 소스](../Docs/Design/ReflectionGeneratedSources.ko.md)). 다른 모듈의 타입은 그 모듈의 descriptor를 import해 상속·참조한다 — 빌드 중에는 이번 실행의 stage descriptor를 쓰고, 모두 성공하면 `<출력>/Metadata/<모듈>.rocksmeta.json`으로 게시한다. private 헤더의 타입은 그 모듈 안에서만 쓴다. 설치형 SDK는 리플렉션 모듈의 descriptor와 명세를 구성마다 `Configurations/<구성>/Metadata/<모듈>/`에 담고(`rocks-sdk.json` 버전 14 — 4는 `ProvidesBuildInfo`를, 5는 `EngineVersion`을, 6은 모듈 표 표지 `ProvidesModuleTable`·`ThirdParty`를 더했고, 8은 메타데이터 모듈 라이브러리가 진입점을 담게 바뀌었고, 9는 타입별 생성 헤더의 본문을 SDK 라이브러리로 옮기고 공유 PCH 제공 모듈의 `SharedPchHeader`를 더했고, 10은 실행 중 적재 모듈 목록 `DynamicModules`를, 11은 로딩 단계 `LoadingPhase`와 부트 표지 `ProvidesLaunch`를 더했고, 12는 의존 목록을 `PublicDependencies`·`PrivateDependencies`로 나눴고, 13은 `HasGlobalState`를, 14는 구성별 `Configurations`를 더했다. 이전 버전 SDK는 거부한다), 게임 빌드는 SDK 모듈을 다시 생성하지 않고 그 descriptor를 import한다. SDK는 생성 헤더와 라이브러리만 싣고 생성 소스는 싣지 않는다 — SDK 모듈의 타입 본문과 등록 함수는 SDK 라이브러리에서 링크된다. 사용 계약과 진단 코드는 [리플렉션 메타데이터](../Docs/Implementation/ReflectionMetadata.ko.md)를 따른다.

명세와 코드 변경은 함께 버전 관리한다. CI PR 검사는 실제 공통 조상·양쪽 부모·병합 결과의 모든 명세를 ID 기준으로 합쳐 비교해 이름/ID 충돌, 이력 손실 및 서로 다른 이름 변경을 거부한다(명세를 옮겨도 ID가 사라진 것으로 보지 않는다). 직접 push, 강제 push 또는 이 검사가 필수로 설정되지 않은 저장소는 자동으로 보호되지 않는다. 동일 선언에 두 ID가 이미 발급된 경우 자동으로 하나를 선택하지 않으며 데이터 마이그레이션 도구도 아직 없다. 리플렉션 헤더는 빌드가 표지로 찾는다(위 문단) — 모듈 폴더 안 include 디렉터리 밖에 둔 표지 헤더는 빌드가 거부한다(RMC2003).

`Category`와 `DisplayName`은 길이를 별도로 보관하는 UTF-8 바이트 문자열이다. 중간 NUL도 보존하므로 C 문자열 종료 문자로 길이를 추정하지 않는다. 생성 코드는 한글처럼 읽을 수 있는 문자를 그대로 표시하고, 제어 문자는 안전한 escape로 표현한다.

메타컴파일러의 `--clang-arg`는 현재 네이티브 Debug/Development/Release/ASan 프로필(모듈 최적화 재정의와 `--no-default-config` 포함)과 붙임형 매크로 정의(`-DNAME=value`, clang-cl의 `/DNAME=value`), macOS의 xcrun 선택 sysroot 및 최소 버전만 허용한다. include 디렉터리는 `--include-dir`에 실제 컴파일 순서대로 지정한다. 미지원 옵션을 추가하려면 네이티브 컴파일과 AST의 동등성을 검토하고 회귀 검사를 먼저 추가해야 한다. 오류는 코드·위치·원인·수정 안내와 함께 출력한다.

Transient 필드는 GC 참조 추적에는 참여하지만 저장 그래프에 포함되지 않는다. ReadOnly 필드는 일반 쓰기를 거부하고 복원 트랜잭션의 쓰기는 허용한다. Abstract 타입은 등록할 수 있으나 생성할 수 없다. 현재 지원 범위는 단일 공개 비가상 상속과 기본 스칼라·Guid·String·ObjectRef로 제한된다.

## 별도 회귀 검사

```text
dotnet run --project Programs/Rocks.Build.Tests
dotnet run --project Programs/Rocks.Build.IntegrationTests -- --root .
dotnet out/dotnet/bin/Rocks.ToolTests/Debug/net10.0/Rocks.ToolTests.dll format --root .
```

`format`은 C++ 소스를 clang-format 21.1.6으로, C# 소스를 `dotnet format Build/Format/Rocks.Format.slnx --verify-no-changes --severity warn`(규칙은 `.editorconfig`)으로 검사하고, 저장소의 `.cs`가 모두 그 솔루션의 프로젝트에 속하는지 확인한다. 게임 템플릿 규칙(`Templates/**/*.Build.cs`·`*.Target.cs`)은 게임 빌드가 SDK를 참조해 따로 컴파일하므로 `Build/Format/Rocks.Template.Rules.csproj`가 서식 검사에 넣는다. 새 C# 프로젝트는 그 솔루션에 더한다. `--write`를 붙이면 고친다.

이전 CMake/Python 경로는 2026-11-12 저장소 구성 개편 때 제거했다(git 이력에 남아 있다). 빌드와 검증은 C#/.NET 경로만 쓴다. 구현된 기능과 후속 과제는 실제 검증 범위와 구분하여 관리한다.
