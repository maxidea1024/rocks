# Rocks 빌드 및 검증

## 요구 사항

- .NET 10 SDK `10.0.302` (`global.json`, 최신 패치 롤포워드), Windows/macOS의 Clang C++23 도구 체인.
- `clang-format` 21.1.6. `ROCKS_CLANG_FORMAT`에 실행 파일 경로를 지정한다. `pwsh Build/InstallFormatter.ps1`은 고정 SHA-256을 확인한 후 네이티브 바이너리를 설치한다. C# 서식 검사의 `dotnet format`은 위 .NET SDK에 들어 있어 따로 설치하지 않는다.
- Windows는 Visual Studio C++ 도구와 Windows SDK가 필요하다. macOS는 Xcode 도구와 macOS SDK가 필요하며 기본 배포 대상은 macOS 13.0이다.
- C++ 컴파일러와 C# 컴파일러의 경고, MSBuild 경고는 오류로 취급한다. 경고 비활성화나 단계 축소는 허용하지 않는다.

## 엔진 빌드

```text
dotnet msbuild Rocks.proj /t:Test /p:Configuration=Debug /p:RocksTarget=Foundation
dotnet msbuild Rocks.proj /t:Test /p:Configuration=Development /p:RocksTarget=Foundation
dotnet msbuild Rocks.proj /t:Test /p:Configuration=Asan /p:RocksTarget=Foundation
dotnet msbuild Rocks.proj /t:Build /p:Configuration=Release /p:RocksTarget=Engine
```

### 빌드 구성

UE5.6 Debug·Development·Shipping에 대응한다. 구성마다 `ROCKS_BUILD_DEBUG`·`ROCKS_BUILD_DEVELOPMENT`·`ROCKS_BUILD_RELEASE` 중 정확히 하나를 1로 정의하며(`Base/Compiler.h`가 확인), 모듈은 이 매크로를 직접 정의할 수 없다.

| 구성 | 최적화 | 디버그 정보 | Windows CRT | NDEBUG | ThinLTO | 구성 매크로 | 용도 |
|---|---|---|---|---|---|---|---|
| Debug | 끔 | 있음(PDB) | `/MDd` | 없음 | 없음 | `ROCKS_BUILD_DEBUG` | 단계 실행·변수 확인 |
| Development | `-O2` | 있음(PDB) | `/MD` | 없음 | 없음 | `ROCKS_BUILD_DEVELOPMENT` | 일상 개발·성능 확인 |
| Release | `-O2` | 있음(PDB) | `/MD` | 정의 | 사용 | `ROCKS_BUILD_RELEASE` | 배포(UE Shipping) |
| Asan | 끔 | 있음(PDB) | `/MD` | 없음 | 없음 | `ROCKS_BUILD_DEBUG` | 메모리 오류 검출 |

- 계약 위반 검사(Fatal)는 모든 구성에서 유지한다. 구성 매크로로 빼는 것은 디버그 표시·치트·통계 같은 개발용 기능뿐이다.
- 대상 OS 매크로 `ROCKS_PLATFORM_WINDOWS`·`ROCKS_PLATFORM_MAC`·`ROCKS_PLATFORM_ANDROID`도 빌드가 정확히 하나를 1로 정의하고, `Base/Compiler.h`가 컴파일 대상과 맞는지 검증한다. 아키텍처 매크로 `ROCKS_ARCH_X64`·`ROCKS_ARCH_ARM64`는 `Compiler.h`가 정한다. 모듈은 `ROCKS_BUILD_*`·`ROCKS_PLATFORM_*`·`ROCKS_ARCH_*`를 정의할 수 없다. 사용 규칙(원시 컴파일러 매크로 금지, include 순서, `defined()` 금지)은 [플랫폼 계층](../Docs/Design/PlatformLayer.ko.md)의 "플랫폼 선택과 실패 정책"을 따른다.
- Release는 최적화된 모듈을 ThinLTO bitcode로 컴파일한다. 도구 탐색은 archiver·linker를 compiler와 같은 폴더에서 먼저 찾으며, Windows Release는 compiler 폴더의 `llvm-lib`·`lld-link`만 받는다(MSVC `lib.exe`·`link.exe`나 다른 LLVM 설치의 도구는 빌드 전에 거부). macOS는 compiler 옆에 `llvm-ar`가 없으면 Xcode `ar`(libLTO)를 쓴다. POSIX link driver에는 `-flto=thin`을 넘긴다.
- 모든 구성이 디버그 정보를 만든다. Release PDB는 UE Shipping처럼 배포 파일과 분리해 크래시 덤프 심볼화에 쓴다.
- 후속: ThinLTO 캐시(`/lldltocache`·`--thinlto-cache-dir`)가 없어 Release relink는 backend를 모두 다시 생성한다. 병렬 link 수와 LTO thread 수 조정도 측정 후 정한다.
- 강제 인라인(UE `FORCEINLINE` 대응)은 쓰지 않는다. 인라인 판단은 Clang 최적화(`-O2`, Release ThinLTO의 모듈 간 인라인)에 맡기고, 정책 검사(`forced-inline`)가 제품 소스의 `always_inline`·`__forceinline`을 거부한다. 성능 문제는 측정한 뒤 알고리즘·데이터 배치·모듈 최적화 설정으로 다룬다.

### 모듈 최적화 재정의

모듈 규칙의 `Optimization`으로 구성의 최적화 여부만 바꾼다(UE `ModuleRules.OptimizeCode` 대응). CRT·디버그 정보·구성 매크로·NDEBUG는 구성을 따르므로 같은 실행 파일 안에서 ABI가 섞이지 않는다.

```csharp
public sealed class Physics : EngineModuleRules
{
    protected override string ModuleName => "Physics";
    protected override string[] RequiredModules => ["Core"];
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
- Core(`ProvidesBuildInfo`)에 닿는 실행 파일마다 `<출력>/BuildInfo/<실행 파일>.BuildInfo.cpp`가 생겨 `rocks::build::ToolchainFingerprint()`(툴체인 지문 — 에셋 너겟 키, 구성 무관)와 `ToolBuildId()`(도구 빌드 ID — 진단·캐시 무효화, 패키지에 쓰지 않음)를 정의한다(`Core/Build/BuildInfo.h`). 원문은 같은 폴더의 `ToolchainFingerprint.txt`·`<실행 파일>.BuildInfo.txt`이고 `action-report.json`의 `ToolchainFingerprint`·`ToolBuildIds`에도 실린다. 실행 파일이 Core에 닿으면 입력 byte가 하나만 바뀌어도 생성 TU와 link가 다시 돈다. 자세한 규칙은 [Rocks.Build](../Programs/Rocks.Build/README.ko.md#빌드-주입-상수buildinfo)와 [ReflectionFoundations](../Docs/Design/ReflectionFoundations.ko.md) 8.1절을 따른다.

MSBuild는 .NET 도구와 C# 규칙을 빌드하고 단계를 조직한다. `Rocks.Build`가 네이티브 작업 그래프, 입력·출력 해시, 증분 여부, 실패 복구를 담당한다. `Build/Rules/*.Build.cs`는 모듈을, `*.Target.cs`는 실행 대상을 정의한다. 규칙은 신뢰된 프로젝트 코드이며 동적으로 읽는 외부 입력은 명시해야 한다.

## 설치 SDK와 게임 빌드

```text
dotnet msbuild Rocks.proj /t:InstalledSdk /p:SdkDestination=C:/work/rocks-sdk/debug /p:Configuration=Debug
dotnet msbuild Rocks.proj /t:Game /p:SdkDirectory=C:/work/rocks-sdk/debug /p:GameDirectory=C:/work/my-game /p:Configuration=Debug
```

SDK 대상 디렉터리는 새 경로여야 한다. SDK의 파일 목록과 내용, 플랫폼·아키텍처·컴파일러 및 SDK ABI·구성이 일치하지 않으면 게임 빌드를 중단한다. 게임 소스와 출력은 SDK 밖에 둔다. `Templates/Game/Foundation`은 C# 모듈·타깃 규칙과 생성된 CLASS/PROPERTY를 사용하는 예제다. 현재 SDK 내보내기는 외부 라이브러리와 임의 컴파일·링크 옵션이 있는 모듈을 지원하지 않으며 해당 선언을 오류로 거부한다.

SDK는 타깃에서 도달하는 모듈만 내보낸다. 고정 버전 외부 라이브러리(`Core/Source/ThirdParty`) 모듈은 정적 라이브러리로 들어가며 manifest `licenses`의 모든 라이선스 원문을 `Licenses/<모듈>/<원본 디렉터리 기준 경로>`에 함께 복사한다(예: `Licenses/Zstd/LICENSE`와 사전 생성기의 MIT 고지 `Licenses/Zstd/lib/dictBuilder/divsufsort.h`). 원본 안 경로를 유지하므로 이름이 같은 원문도 겹치지 않는다. BSD 계열(LZ4·zstd)과 MIT(zstd divsufsort)는 바이너리 배포 시 고지 의무가 있으므로 게임 배포 패키지에도 이 고지를 포함한다.

## 외부 라이브러리(C 원본)

빌드 도구는 확장자가 정확히 `.c`인 번역 단위를 C17로 컴파일한다(clang-cl `/TC /clang:-std=c17`, POSIX `-x c -std=c17`). 경고·FP·구성·CRT·최적화 프로필은 C++와 같고 C++ 전용 플래그만 빠진다. C++ 번역 단위의 인자는 바뀌지 않는다. `.c`는 `Core/Source/ThirdParty/<Lib>/` 아래에서만 허용하며(compile DB 감사), 모듈 인자로 언어·표준을 바꿀 수 없다.

라이브러리 폴더는 `<Lib>.Build.cs`(`ThirdPartyModuleRules` 상속, 이름만 적는다), `ThirdParty.json`(단일 출처), `<lib>-<version>/`(릴리스 원본을 byte 그대로)로 구성한다. 업그레이드 절차:

1. 새 릴리스를 받아 서명·해시를 확인하고 필요한 파일만 새 버전 디렉터리에 그대로 복사한다. 이전 버전 디렉터리를 지운다.
2. `ThirdParty.json`의 `version`·`upstreamDirectory`·`source`·경로를 고친 뒤 `dotnet out/dotnet/bin/Rocks.ToolTests/Debug/net10.0/Rocks.ToolTests.dll third-party --root . --write`로 `files`(SHA-256)와 `pragmaDirectives`를 다시 계산한다. 인자 없이 실행하면 차이만 보고한다.
3. 새로 기록된 진단 지시문을 검토한다. `native` 단계의 `Native.ThirdPartyDirectivesHideNoWarnings`가 지시문을 지운 사본도 경고 0인지 실제 compile DB 인자로 확인한다.
4. 압축 결과 golden(`Compression_DeterministicOutputGoldens`)이 바뀌면 의도한 변경인지 확인하고 갱신한다.

## 메타 선언

`ROCKS_CLASS()` 또는 `ROCKS_CLASS(Abstract)`를 `class`/`struct` 바로 앞에 두고, 리플렉션할 필드에 `ROCKS_PROPERTY(...)`를 붙인다. 선언에 ID나 스키마 버전 숫자를 넣지 않는다. 비공개 필드에는 `ROCKS_GENERATED_BODY()`가 필요하다. 타입 마커와 클래스 사이에는 공백·주석만 허용한다. 조건부 선언은 마커와 클래스를 같은 조건 블록에 둔다. 미지원 타입·상속·선언 위치·지정자와 모호한 선언은 오류다.

리플렉션 헤더는 그 헤더를 소유한 모듈의 규칙이 `Metadata`(모듈의 public 또는 private include 디렉터리 기준 철자)로 선언한다. `SourceModuleRules`는 `protected override IReadOnlyList<string> Metadata => ["Module/Type.h"];`로, `ModuleRules`를 직접 상속하는 게임 모듈은 `NativeModuleInput`의 `Metadata`와 명세 위치 `ModuleDirectory`로 적는다. 식별자는 모듈 폴더의 소스 관리되는 `<모듈>.rocksids.json`에 보관하며 일반 빌드는 파일을 수정하거나 누락된 ID를 자동 발급하지 않는다. 새 선언은 메타컴파일러의 명시적인 저작 명령으로 등록하고, 이름을 바꿀 때는 새 ID를 만들지 말고 `ids rename`을 사용한다. 삭제할 선언은 `ids retire`로 폐기하고, 타입을 다른 모듈로 옮길 때는 `ids move`로 ID를 유지한다. 모듈의 활성 명세 항목이 소스에 없거나, 두 명세에 같은 ID나 같은 이름이 있으면 빌드가 실패한다.

```text
dotnet out/dotnet/bin/Rocks.MetaCompiler/Debug/net10.0/Rocks.MetaCompiler.dll ids add --manifest SampleGame.rocksids.json --type sample::Character --field sample::Character::health
dotnet out/dotnet/bin/Rocks.MetaCompiler/Debug/net10.0/Rocks.MetaCompiler.dll ids rename --manifest SampleGame.rocksids.json --kind field --old sample::Character::health --new sample::Character::hitPoints
dotnet out/dotnet/bin/Rocks.MetaCompiler/Debug/net10.0/Rocks.MetaCompiler.dll ids move --from Old.rocksids.json --to New.rocksids.json --type sample::Character
```

빌드는 리플렉션 헤더가 있는 모듈을 의존 순서로 하나씩 생성한다. 생성 헤더는 `<출력>/Generated/<모듈>/Public|Private/<원본 철자의 .h를 .generated.h로>`이며 그 모듈의 public·private include로 전파된다(`#include <Module/Type.generated.h>`). 모듈 집계 헤더 `<모듈>/<모듈>.module.generated.h`의 `rocks::generated::<모듈>::RegisterModuleTypes`가 의존 모듈부터 public 타입을 등록하고, private 헤더의 타입은 `<모듈>/<모듈>.private.module.generated.h`의 `RegisterPrivateModuleTypes`가 등록한다. 다른 모듈의 타입은 그 모듈의 descriptor를 import해 상속·참조한다 — 빌드 중에는 이번 실행의 stage descriptor를 쓰고, 모두 성공하면 `<출력>/Metadata/<모듈>.rocksmeta.json`으로 게시한다. private 헤더의 타입은 그 모듈 안에서만 쓴다. 설치형 SDK는 리플렉션 모듈의 descriptor와 명세를 `Metadata/<모듈>/`에 담고(`rocks-sdk.json` 버전 4 — 4는 `ProvidesBuildInfo`를 더했고 이전 버전 SDK는 거부한다), 게임 빌드는 SDK 모듈을 다시 생성하지 않고 그 descriptor를 import한다. 사용 계약과 진단 코드는 [리플렉션 메타데이터](../Docs/Implementation/ReflectionMetadata.ko.md)를 따른다.

명세와 코드 변경은 함께 버전 관리한다. CI PR 검사는 실제 공통 조상·양쪽 부모·병합 결과의 모든 명세를 ID 기준으로 합쳐 비교해 이름/ID 충돌, 이력 손실 및 서로 다른 이름 변경을 거부한다(명세를 옮겨도 ID가 사라진 것으로 보지 않는다). 직접 push, 강제 push 또는 이 검사가 필수로 설정되지 않은 저장소는 자동으로 보호되지 않는다. 동일 선언에 두 ID가 이미 발급된 경우 자동으로 하나를 선택하지 않으며 데이터 마이그레이션 도구도 아직 없다. 소스에서 빠진 메타 입력은 자동 탐색하지 않으므로 모든 리플렉션 헤더를 소유 모듈의 `Metadata`에 등록해야 한다.

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
