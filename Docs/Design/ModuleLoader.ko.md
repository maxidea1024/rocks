# 모듈 로더 설계

기준일: 2026-11-21, 개정 9. 모듈의 수명(시작·종료)과 적재를 맡는 모듈 로더를 정한다. UE 5.8 `IModuleInterface`·`FModuleManager`·로딩 단계(`ELoadingPhase`)에 해당한다.

[빌드 타깃 설계](BuildTargets.ko.md) 3.8절은 Editor 타깃을 UE와 같이 모듈식(`Modular`)으로 링크하기로 정했다(BD17). 그리고 모듈 로더를 별도 스펙으로 넘기며 네 가지 계약을 요구했다(BD30).
1. 같은 소스가 `Monolithic`·`Modular`에서 같은 순서·결과로 동작한다.
2. 모듈 시작은 의존 순서, 종료는 역순이다. 리플렉션 타입 등록은 모듈 시작에 묶는다.
3. Rocks Studio는 프로젝트 기술자와 플러그인 활성 조건을 읽어 게임·플러그인 모듈을 실행 중에 적재한다.
4. 적재 실패는 원래 OS 오류 코드를 보존한 진단으로 끝낸다.

이 문서가 그 스펙이다. 설계가 애매한 곳은 UE 방식을 따르고, Rocks 원칙(전역 생성자 금지, `Result` 기반 오류, 명시적 소유)과 부딪히는 곳만 다르게 정한다(7절).

UE 근거는 UE 5.8.4(`release`, 로컬 클론 `C:\github\UnrealEngine`, commit `f45729c0b6f0`)이고, 경로는 `Engine/Source/` 기준이다. UE 이름은 대응표와 근거에만 적는다. hot reload·Live Coding(코드 다시 불러오기)은 범위 밖이다(9.2절).

상태: **설계 초안**(결정 MD1–MD22, 모두 권장안). 구현 단계는 ML0–ML6(6절)이며 착수 시점은 사용자가 정한다. ML0 구현·병합(개정 2에 구현 세부). ML2는 ML1(Launch 병합 대기) 전에 하는 연결을 3.10절(개정 3)에 정했고, [리플렉션 생성 소스](ReflectionGeneratedSources.ko.md) GS1 병합 뒤 그 구조 위에서 구현했다(2026-11-20 — ML1 뒤로 넘긴 것은 3.10절 끝). ML3(`DynamicModules`)은 개정 4에서 UE 근거로 순환 규칙을 고쳐(3.8절) 구현했다. ML1(단계 실행·묶음·Launch 통합)은 crash-launch 병합 뒤 개정 5에서 3.11절로 정해 구현했고, 그 후속(호스트 서비스·서비스를 쓰는 `Startup` 등록)은 개정 6 3.12절이다. ML0–ML4는 Rocks Studio(BS5) 전에, ML5는 BS4·BS5와 함께 한다. Modular 적재(ML4)는 [빌드 타깃](BuildTargets.ko.md) BS8 뒤다. 초안을 서브에이전트 1개로 교차 검증해 반영했다(10절).

## 1. 현재 상태

| 항목 | 현재 Rocks | 문제 |
|---|---|---|
| 모듈 수명 객체 | 없다. 모듈마다 시작·종료 함수가 없다 | 모듈이 자기 초기화를 둘 곳이 없어 호스트가 모듈별 등록 함수를 하나씩 부른다 |
| 시작 순서 | 실행 파일마다 `main`이 직접 조립한다. Sandbox 엔진 호스트: HostSettings → 크래시 처리 → LogHost → Trace → `Engine::Initialize`(`Programs/Sandbox/Private/EngineHost.cpp:301-391`) | 모듈이 늘면 호스트 코드가 계속 길어진다 |
| 리플렉션 등록 | 생성 집계 함수 `RegisterModuleTypes(ModuleRegistration&, const ModuleLease& = {})`(`Programs/Rocks.MetaCompiler/CppEmitter.cs:59-133`)를 호스트·래퍼가 명시 호출한다(`HostSettings.cpp:269-344`, `World.cpp:88-91`, `Templates/Game/Foundation/Source/Main.cpp:9-10`). 집계 함수는 import한 의존 모듈을 호출자의 lease로 재귀 등록한다 | 실제 `ModuleLease`를 넘기는 쪽이 없다. 모듈이 언제 등록되는지가 호출자마다 다르다 |
| 코드 수명 토큰 | `ModuleLease`(retain/release 토큰, 빈 lease는 정적 링크 코드 — `Core/Source/Runtime/Core/Public/Core/Lifetime/ModuleLease.h:7-93`)가 `RuntimeTypeOps.module`에 저장된다 | 발급자가 없다 |
| 공유 라이브러리 적재 | `platform::DynamicLibrary`(절대 경로만, `FileStatus{code, nativeError}` — `Core/Source/Runtime/PlatformBase/Public/Platform/DynamicLibrary.h:81-175`). 문서가 "엔진 모듈 로더가 아니다"라고 적는다 | POSIX는 `nativeError`가 0이고 `dlerror` 문구를 넘기지 않았다. ML0에서 `DynamicLibraryError`로 문구를 넘기도록 보강했다(3.5절) |
| 로딩 단계 | 기술자 `LoadingPhase { EarliestPossible, PostConfigInit, PreDefault, Default, PostDefault }`가 있다(`Programs/Rocks.Build/Descriptors.cs:35`). 엔진 모듈(`SourceModuleRules`)에는 단계가 없다 | 빌드가 읽기만 하고 실행 중 반영이 없다([저장소 구성](../Guides/RepositoryLayout.ko.md) 게임 빌드 연동) |
| 설정 등록 | CVar·설정 struct는 모듈 등록 함수에서 `ConfigContext::Register`로 올린다([CVar](CVarSystem.ko.md)). HostSettings는 `CompleteStartup` 직후 `ReportUnresolved`로 아무도 등록하지 않은 이름을 경고한다(`Core/Source/Runtime/HostConfig/Private/HostSettings.cpp:549-560`) | 모듈이 늦게 시작해 늦게 등록하면 설정 파일의 그 이름이 모두 오타 경고가 된다 |
| 빌드 식별 | `ToolchainFingerprint()`·`ToolBuildId()`(`Core/Public/Core/Build/BuildInfo.h`)는 Core가 선언하고 실행 파일마다 만드는 생성 TU가 정의한다 | 공유 라이브러리 사이 호환 식별자로 쓸 수 없다. Modular에서는 Core 공유 라이브러리에 미해결 기호가 생긴다 |
| 전역 생성자 | 금지가 원칙이다([설정 시스템](ConfigSystem.ko.md) 129행, [CVar](CVarSystem.ko.md) VD1). 예외는 시험 `TEST` 등록뿐이다(`TestSupport/Public/TestSupport/Test.h:13-33`) | UE식 정적 등록(전역 생성자)을 그대로 쓸 수 없다 |
| 실행 진입점 | 공통 진입점 `Launch` 모듈(`ROCKS_IMPLEMENT_APPLICATION`, 단계 실행기 `RunLaunchStages`)이 crash-launch 작업으로 진행 중이다(설계 `CrashHandlingAndLaunch.ko.md` 개정 3, 미병합) | 로더를 만들고 소유할 자리가 그 안에 있어야 한다 |
| 한 프로세스의 Engine 수 | 시험은 한 프로세스에서 `Engine`을 차례로 여러 번 만든다(`Engine/Source/Runtime/Engine/Tests/EngineTests.cpp`) | 엔진 단계 모듈의 시작·종료가 Engine 수명에 맞아야 한다 |

## 2. 용어 대응

| UE 5.8 | Rocks | 비고 |
|---|---|---|
| `IModuleInterface` | `ModuleImplementation` | 모듈 수명 객체의 기반 클래스 |
| `FDefaultModuleImpl` | `DefaultModuleImplementation` | 빈 구현 |
| `IMPLEMENT_MODULE(Class, Name)` | `ROCKS_IMPLEMENT_MODULE(Class, Name)` | 모듈 진입점 매크로 |
| `FModuleManager` | `ModuleLoader` | 프로세스에 하나, Launch가 소유 |
| `FModuleInfo` | `ModuleRecord` | 모듈 하나의 상태 |
| `EModuleLoadResult` | `ModuleLoadCode` + `ModuleLoadReport` | 원래 OS 오류 코드를 담는 report |
| `*.modules`(`FModuleManifest`) | `<Target>.modules.json`(`ModuleManifest`) | 모듈 이름 → 파일·의존·단계 |
| `BuildId` | `ModuleBuildId` | 같은 빌드 계열의 바이너리인지 판정 |
| `ELoadingPhase` | `LoadingPhase` | 기술자에 이미 있다 |
| `OnModulesChanged` | `ModuleLoader::Changed` | Core Delegate |
| `ProcessLoadedObjectsCallback` | 타입 등록기(`ModuleTypeRegistrar`) | 3.6절 |
| `LoadModuleBinaryOnly`·`bPreloadedBinaries` | `ModuleLoader::Prepare` | 3.5절, 설정 등록에 쓴다 |
| `DynamicallyLoadedModuleNames` | 모듈 규칙 `DynamicModules` | 3.8절 |

## 3. 설계

### 3.1 모듈 수명 객체

모든 C++ 모듈은 수명 객체 하나를 가진다(UE와 같다 — 모든 모듈이 `IMPLEMENT_MODULE`을 쓴다). header-only 모듈과 서드파티 모듈은 C++ 모듈 코드가 없으므로 면제다(모듈 표·manifest에는 들어가지 않는다).

- **진입점 계약은 Core에 둔다(개정 2)**: 수명 객체 기반 클래스·진입 기술(`ModuleEntry`)·진입점 매크로·모듈 표 형식은 Core 공개 헤더 `Core/Module/ModuleImplementation.h`·`Core/Module/ModuleEntry.h`에 둔다. 로더(`ModuleLoader`)·`ModuleStartContext`·`HostServices`·report는 `ModuleLoader` 모듈에 둔다(3.9절).
  - 이유: 모든 C++ 모듈이 진입점 매크로를 써야 하는데 `ModuleLoader`는 Core에 의존한다. 매크로를 `ModuleLoader`에 두면 Core 자신은 쓸 수 없고, 다른 모든 모듈 규칙에 `ModuleLoader` 의존을 더해야 한다. UE도 `IModuleInterface`·`IMPLEMENT_MODULE`이 Core에 있다(`Runtime/Core/Public/Modules/ModuleInterface.h`, `ModuleManager.h`).
  - 버린 대안: 모든 모듈 규칙에 `ModuleLoader` 의존을 더한다(규칙 수십 개를 고치고도 Core는 못 쓴다). 빌드가 `ModuleLoader` 의존을 몰래 넣는다(규칙에 보이지 않는 의존).
  - Core 헤더는 `ModuleStartContext`·`config::ConfigContext`·`ModuleRegistration`을 전방 선언만 한다. 그래서 Core가 상위 모듈에 의존하지 않는다.
- **Core 아래 모듈도 면제한다(개정 2)**: Core에 닿지 않는 모듈(지금 `PlatformBase`·`DeterministicMath`)은 진입점 정의를 include할 수 없으므로 수명 객체가 없다. UE에서는 이 코드(HAL·수학)가 Core 모듈 안에 있으므로 Core의 수명 객체가 이들을 대표한다. 빌드는 모듈 표 제공 모듈(Core, 규칙 표지 `ProvidesModuleTable`)에 닿지 않는 모듈을 모듈 표에서 뺀다.
- **실행 파일 모듈도 모듈이다(개정 2)**: 실행 파일 규칙도 C++ 모듈이므로 진입점 매크로를 쓰고 자기 모듈 표의 마지막 행이 된다(UE `IMPLEMENT_APPLICATION`이 `IMPLEMENT_GAME_MODULE`을 포함한다 — `Runtime/Core/Public/Modules/Boilerplate/ModuleBoilerplate.h`). 같은 소스를 여러 실행 파일이 컴파일하면(시험 runner) 이름 인자에 `ROCKS_MODULE_NAME`을 쓴다(3.2절).

```cpp
// Core/Source/Runtime/Core/Public/Core/Module/ModuleImplementation.h
class ModuleImplementation {
public:
    virtual ~ModuleImplementation() = default;

    // 모듈이 시작할 때 한 번 부른다. 의존 모듈은 모두 이미 시작했다.
    // 실패를 돌려주기 전에 이 함수가 한 부분 등록을 스스로 되돌려야 한다.
    // 실패하면 Shutdown은 불리지 않고, 적재 요청은 StartupFailed로 끝난다.
    virtual Result<void> Startup(ModuleStartContext& context) noexcept { return {}; }

    // 모듈 묶음을 내릴 때 그 묶음 전체의 Shutdown보다 먼저, 시작 역순으로 부른다(3.7절).
    virtual void PreUnload() noexcept {}

    // 시작한 모듈을 내릴 때 부른다(종료·명시 해제 모두). 시작 역순이다.
    virtual void Shutdown() noexcept {}

    // false면 종료 절차에서 자동으로 내리지 않는다(UE SupportsAutomaticShutdown).
    virtual bool SupportsAutomaticShutdown() const noexcept { return true; }

    // false면 실행 중 명시 해제(3.7절)를 거부한다(UE SupportsDynamicReloading).
    virtual bool SupportsUnload() const noexcept { return true; }
};
```

- UE의 호출 행렬(`Runtime/Core/Public/Modules/ModuleInterface.h:7-29`)을 따른다. 시작은 `Startup`만, 종료 절차는 `PreUnload` 전부 → `Shutdown` 역순, 실행 중 해제는 `Shutdown`만 부른다.
- UE `StartupModule`은 `void`다. Rocks는 `Result<void>`를 돌려준다. 모듈이 실패를 숨기지 않고 로더가 report로 올리게 하기 위해서다(7절).
- `PostLoadCallback`(UE hot reload·`LoadModuleWithCallback` 전용)과 `IsGameModule`(UE `QueryModule` 표시용)은 두지 않는다(9.1절).
- **`ModuleStartContext`**: `Startup`이 받는 문맥이다. 전역 접근자 대신 이것으로 호스트 서비스를 얻는다.
  - `ModuleLoader& Loader()`, `LoadingPhase Phase()`, `ModuleLease Lease()`(이 모듈의 코드 수명 토큰, 3.6절).
  - `HostServices& Services()`: Launch와 엔진 호스트가 단계마다 등록하는 서비스 표다(`ConfigContext`, `LogSystem`, `TraceSession` — 3.12절. `Engine`은 후속). `Find<T>()`로 찾고, 아직 등록되지 않은 서비스는 null이다. 서비스 형식 키는 공유 라이브러리마다 갈라지지 않게 서비스 형식을 가진 모듈의 cpp에 out-of-line으로 정의하고 export한다([빌드 타깃](BuildTargets.ko.md) BD28·BD29와 같은 규칙, 선언·정의 매크로 `ROCKS_DECLARE_HOST_SERVICE`·`ROCKS_DEFINE_HOST_SERVICE`).
- **기본 구현**: `DefaultModuleImplementation`은 빈 구현이다. 시작할 일이 없는 모듈은 `ROCKS_IMPLEMENT_MODULE(DefaultModuleImplementation, Name)`을 쓴다.
- **설정 등록 함수**: 수명 객체 클래스가 정적 함수 `static Result<void> RegisterSettings(ConfigContext&) noexcept`를 가지면 진입점 매크로가 이를 감지해 진입 기술에 싣는다(3.2절). 이 함수는 수명 객체 없이 부르며, 모듈의 CVar·설정 struct 등록만 한다(3.5절 설정 등록 시점).
  - 감지(개정 2 리뷰 반영): 이름이 있는지는 접근 권한·오버로드와 무관하게 이름 조회 모호성으로 본다(`Class`와 같은 이름의 데이터 멤버를 가진 기반을 함께 상속한 탐침에서 `&Probe::RegisterSettings`가 모호하면 `Class`에 그 이름이 있다. 이름 조회는 접근 검사보다 먼저다). 이름이 있는데 public 단일 정적 함수의 주소로 위 서명을 얻을 수 없으면(private·protected, 오버로드, 비정적, 다른 서명) `static_assert` 오류다. requires 식만 쓰면 private·오버로드 함수가 진단 없이 "없음"(null)이 되어 설정 등록이 조용히 빠졌다. `final` 클래스는 탐침이 상속할 수 없으므로 public 이름만 감지한다(한계: `final` 클래스의 private·오버로드 `RegisterSettings`는 감지하지 못한다). 진입 기술의 칸 형식은 `Result<void> (*)(config::ConfigContext&) noexcept`이고 Core는 `ConfigContext`를 전방 선언만 한다(개정 2). 불투명 포인터(`void*`)로 지우는 안은 형식 검사를 잃어 버렸다.

### 3.2 모듈 진입점

UE는 Monolithic에서 정적 등록 객체(`FStaticallyLinkedModuleRegistrant`), Modular에서 정적 `FModuleInitializerEntry`를 쓴다(`Runtime/Core/Public/Modules/ModuleManager.h:943-969`, `Runtime/Core/Private/Modules/ModuleManager.cpp:98-152`). 둘 다 전역 생성자다. Rocks는 전역 생성자 금지 원칙을 지켜, 진입점을 **상수 기술 + 함수**로 만든다(MD2).

```cpp
// 모듈의 cpp 하나에 쓴다. Name은 빌드의 모듈 이름과 정확히 같아야 한다.
ROCKS_IMPLEMENT_MODULE(CurvesModule, Curves)
```

- 매크로는 상수 기술 `ModuleEntry`와 그것을 돌려주는 함수 `RocksModuleEntry_<Name>()`을 정의한다.
  - `ModuleEntry`: `entryVersion`(구조체 ABI 버전), `name`, `create`(수명 객체를 만드는 함수), `registerTypes`(이 모듈 자신의 타입만 등록하는 생성 함수, 리플렉션이 없으면 null — 3.6절. 해제는 모듈별 `ModuleRegistration`이 기록으로 되돌리므로 함수를 따로 두지 않는다 — 개정 3), `registerSettings`(3.1절, 없으면 null).
  - 기술은 상수 초기화다(전역 생성자 없음). 수명 객체는 로더가 시작할 때 `create`로 만든다(UE도 첫 적재 때 만든다).
- **Monolithic**: 빌드가 실행 파일마다 모듈 표 TU를 만든다. 타깃에 들어간 모든 C++ 모듈의 `RocksModuleEntry_<Name>()`을 의존 순서로 나열하고, 모듈별 의존 이름·로딩 단계(3.3절)를 함께 싣는다. 이 표가 Monolithic의 manifest다. 표가 모든 진입 함수를 직접 참조하므로 정적 라이브러리에서 진입점 object가 빠지지 않고, `ROCKS_IMPLEMENT_MODULE`이 빠진 모듈은 링크 오류가 난다(UE `IMPLEMENT_MODULE_<Name>` 강제 참조와 같은 역할 — `ModuleManager.h:950, 968`).
- **Modular**: 매크로는 위에 더해 공유 라이브러리마다 하나뿐인 export 진입점 `extern "C" ROCKS_DLLEXPORT const ModuleEntry* RocksModuleEntry()`를 정의한다. UE의 주 경로는 정적 목록 `FModuleInitializerEntry::FindModule`이고 `GetDllExport("InitializeModule")`는 fallback이다(`ModuleManager.cpp:1222-1229`). Rocks는 그 fallback과 같은 방식만 쓴다. 빌드는 매크로가 같은 자리에 정의하는 export하지 않는 모듈별 C 기호 `RocksModuleEntryOf_<Name>`을 모듈 링크에 강제 참조로 넣어(Windows `/INCLUDE`, ELF·Mach-O `-u`) 매크로 누락을 링크 오류로 만든다(export 이름은 의존 모듈의 import 라이브러리도 내므로 그 이름으로는 잡지 못한다 — [빌드 타깃](BuildTargets.ko.md) BS8d1).
- 매크로 이름 인자가 빌드가 넣는 모듈 이름 정의(`ROCKS_MODULE_NAME`)와 다르면 `static_assert` 오류다(UE는 경고 — `ModuleManager.h:950`).
- 한 모듈에 매크로가 둘이면 링크 오류(같은 기호 중복)다.
- 시험 실행 파일·도구도 모듈 표를 가진다. 그래서 시험이 모듈 로더를 거쳐 모듈을 시작한다(ML0).
- **ML0 구현 세부(개정 2)**:
  - 진입 기술 칸: `entryVersion`(`ModuleEntryVersion`), `name`(NUL 종료 ASCII), `create`(`Result<UniquePtr<ModuleImplementation>> (*)(Allocator&) noexcept` — 로더가 받은 allocator로 만든다. 수명 객체 클래스는 noexcept 기본 생성자가 있어야 한다), `registerTypes`(ML2에서 채운다 — 3.10절), `registerSettings`. ML2에서 `unregisterTypes`를 빼고 `ModuleEntryVersion`을 2로 올렸다(개정 3).
  - 매크로는 이름 인자를 한 번 펼친 뒤 붙인다. 그래서 `ROCKS_IMPLEMENT_MODULE(DefaultModuleImplementation, ROCKS_MODULE_NAME)`도 된다(한 소스를 여러 실행 파일이 컴파일하는 시험 runner용). 비교는 문자열 상수 비교라 철자가 다르면 `static_assert`, `ROCKS_MODULE_NAME`이 없으면(빌드 밖 컴파일) 매크로를 쓰는 곳에서 `static_assert`다. 매크로가 없는 헤더 include는 영향이 없다.
  - 진입점을 가지는 모듈의 이름은 C 식별자(영문자·숫자·`_`, 숫자로 시작하지 않음)여야 한다(기호 이름을 만들기 때문). 빌드가 검사한다.
  - 모듈 표: 빌드가 Core(`ProvidesModuleTable`)에 닿는 실행 파일마다 `<출력>/GeneratedSources/<실행 파일>/<실행 파일>.ModuleTable.cpp`를 만들어 그 실행 파일 소스에 더한다(규칙 생성 TU와 같은 경로, 내용이 같으면 다시 컴파일하지 않음). 행은 `ModuleTableRow{name, entry, dependencies, dependencyCount, phase}`이고 정의 함수는 `rocks::GeneratedModuleTable()`이다. 상수 배열이라 전역 생성자가 없다.
  - 행의 의존은 빌드의 직접 의존 가운데 표에 든 모듈이다. 면제 모듈(header-only·서드파티·Core 아래)을 거치는 의존은 그 너머의 표 모듈로 바꿔 싣는다(예: 모듈 → header-only `MetaSchema` → Core면 Core). 그래서 `Load`가 의존 순서를 잃지 않는다.
  - 단계: 기술자 모듈(게임·플러그인)은 기술자의 `loadingPhase`, 엔진 모듈은 `Default`다(엔진 모듈 규칙의 단계는 ML1).

### 3.3 로딩 단계

기술자의 `LoadingPhase`를 실행 중에 반영한다. UE 값 가운데 Rocks 시작 흐름에 대응하는 것을 쓴다(UE `Runtime/Projects/Public/ModuleDescriptor.h:24-60`).

| 단계 | 언제 | 쓸 수 있는 서비스 | 묶음 | UE 대응 |
|---|---|---|---|---|
| `EarliestPossible` | 로더 생성 직후(Launch 도구 로그 시작 뒤, 본문 전) | 할당기·플랫폼 계층·도구 로그 | 프로세스 | `EarliestPossible` |
| `PostConfigInit` | HostSettings가 설정을 확정하고 LogHost가 시작한 뒤 | `ConfigContext`·`LogSystem`·`TraceSession` | 호스트 | `PostConfigInit` |
| `PreDefault`·`Default`·`PostDefault` | `Engine::Initialize` 안, 엔진 TypeRegistry·ModuleRegistration을 만든 뒤, ObjectRegistry·EngineSubsystem을 만들기 전 | 위 + `Engine`(초기화 중) | 엔진 | `LoadStartupModules`(`Runtime/Launch/Private/LaunchEngineLoop.cpp:4669-4696`) |
| `PostEngineInit` | `EngineClient::OnEngineStart` 뒤, 첫 프레임 전 | 위 + 시작을 마친 `Engine` | 엔진 | `PostEngineInit`(`LaunchEngineLoop.cpp:4863`) |
| `None` | 자동으로 시작하지 않는다. 코드가 `Load`로 명시 적재한다 | 적재 시점의 서비스 | 적재 시점의 묶음 | `None` |

- `PostEngineInit`·`None`을 기술자 `LoadingPhase`에 더하고, 엔진 모듈 규칙(`SourceModuleRules`)에도 `LoadingPhase` 속성(기본 `Default`)을 둔다(MD5).
- UE의 `PostSplashScreen`·`PreEarlyLoadingScreen`·`PreLoadingScreen`은 두지 않는다. 시작 화면·로딩 화면이 생기면 다시 본다(9.1절).
- **묶음**: 단계는 세 묶음으로 나뉜다. 묶음은 종료 시점을 정한다(3.7절).
  - 프로세스 묶음(부트 모듈·`EarliestPossible`): 로더와 같이 산다.
  - 호스트 묶음(`PostConfigInit`): HostSettings·LogHost와 같이 산다.
  - 엔진 묶음(`PreDefault`–`PostEngineInit`): `Engine` 수명에 묶인다. 한 프로세스에서 Engine을 차례로 여러 번 만들면 Engine마다 시작·종료한다. 동시에 둘 이상 있으면 처음 Engine이 시작하고 마지막 Engine이 내린다(참조 수).
  - `None` 모듈과 `DynamicModules`는 적재될 때 로더의 현재 묶음(가장 안쪽 묶음)에 속한다.
- **단계 순서 검사(MD6)**: 모듈의 의존 모듈은 단계가 같거나 더 이르거나 `None`이어야 한다. 빌드가 규칙 로드 때 검사한다(위반은 오류). 의존 시작(3.4절)이 늦은 단계 모듈을 조용히 앞당기지 않게 하기 위해서다.
- **단계 실행**: `ModuleLoader::StartPhase(phase)`가 그 단계 모듈을 manifest 순서(의존 순서)로 시작한다. 요청 시 적재 행(`DynamicModules` 관계로만 표에 든 모듈 — 3.8절)은 단계와 무관하게 대상이 아니다(UE `None`과 같다).
  - 아직 돌지 않은 앞 단계가 있으면 그 단계부터 차례로 돈다. 그래서 설정을 쓰지 않는 도구가 Engine을 만들어도 단계를 건너뛰지 않는다(그 경우 `PostConfigInit` 모듈은 `ConfigContext` 없이 시작하며, 필요한 서비스가 없으면 그 모듈의 `Startup`이 실패를 돌려준다).
  - 이미 돈 단계를 다시 부르면 할 일이 없고 성공이다(UE도 같은 단계 재호출을 허용한다 — `Runtime/Projects/Private/PluginManager.cpp:2909-2913`). 지금 묶음보다 앞 묶음의 단계를 부르는 것은 계약 위반 오류다(UE는 더 이른 단계 호출을 Error 로그로 알린다 — 같은 파일 2842-2937행).
  - 엔진 묶음을 내리면 엔진 단계는 다시 돌 수 있는 상태가 된다(다음 Engine).
- **실패**: 단계 안에서 모듈 하나가 실패하면 그 단계를 멈추고 report를 돌려준다(UE 플러그인 로딩도 첫 실패에서 멈춘다). 호출자(Launch·엔진 호스트·`Engine::Initialize`)가 시작을 중단하고, 그 묶음에서 이미 시작한 모듈을 역순으로 내린다(3.7절).

### 3.4 적재 API

```cpp
class ModuleLoader {
public:
    static Result<UniquePtr<ModuleLoader>> TryCreate(Allocator& allocator, ModuleTable table) noexcept;
    ModuleLoadReport Load(StringView name) noexcept;              // 의존부터 시작, 이미 시작했으면 성공
    ModuleLoadReport StartPhase(LoadingPhase phase) noexcept;     // ML1(3.11절)
    ModuleLoadReport StartEngineGroup() noexcept;                 // 엔진 묶음 참조 수(ML1)
    void ShutdownEngineGroup() noexcept;                          // ML1
    bool PhaseStarted(LoadingPhase phase) const noexcept;         // ML1
    ModuleGroup CurrentGroup() const noexcept;                    // ML1
    void ShutdownGroup(ModuleGroup group) noexcept;               // 3.7절
    ModuleUnloadReport Unload(StringView name) noexcept;          // 3.7절, ML6
    bool IsStarted(StringView name) const noexcept;
    Optional<ModuleState> State(StringView name) const noexcept;  // 진단·시험용 조회
    ModuleImplementation* Find(StringView name) const noexcept;
    template <class T> T* Find(StringView name) const noexcept;   // 검사 없는 static_cast(UE GetModuleChecked<T>)
    usize LeaseCount(StringView name) const noexcept;             // 살아 있는 lease 수(진단)
    usize ModuleCount() const noexcept;                           // 표의 모듈 수
    ModuleTable Table() const noexcept;                           // 만들 때 받은 모듈 표(ML2, 3.10절)
    Result<ModuleLease> AcquireLease(StringView name) noexcept;   // 모듈 lease 하나 더(ML2, 3.10절)
    usize ModulesWithLeases(Span<StringView> names) const noexcept; // 남은 lease 진단(ML2)
    ModuleLoadReport AttachTypeRegistrar(ModuleTypeRegistrar& registrar) noexcept; // ML2, 여럿(ML1)
    void DetachTypeRegistrar(ModuleTypeRegistrar& registrar) noexcept; // ML2, 여럿(ML1)
    usize TypeRegistrarCount() const noexcept;                    // 붙은 등록기 수(ML1)
    HostServices& Services() noexcept;
    MulticastDelegate<void(StringView, ModuleChange)> Changed;    // Started·Unloaded
};
```

- **ML0 구현 세부(개정 2)**:
  - 생성은 `TryCreate(allocator, table)`다. 모듈 기록을 할당하고 표를 검사한다(이름 중복·식별자 아님, 표에 없는 의존, 의존이 앞 행이 아님, 진입 기술의 `entryVersion`·이름 불일치는 `InvalidArgument`). 모듈 수명 객체도 이 allocator로 만든다. allocator는 로더보다 오래 산다.
  - `StartPhase`는 ML1, `Unload`는 ML6에 더한다. 그때까지 공개 API에 두지 않는다(부르면 할 일이 없는 함수를 두지 않는다). `ModuleUnloadCode`·`ModuleUnloadReport` 형식은 ML0에 둔다.
  - `Changed`는 UE `OnModulesChanged`처럼 여러 구독자를 받으므로 `MulticastDelegate`다(초안의 `Delegate`를 고쳤다). 구독·해제는 로더 스레드에서 한다.
  - 소멸: 아직 시작해 있는 모듈은 묶음 안쪽부터(엔진 → 호스트 → 프로세스) 종료 절차로 내린 뒤 모든 수명 객체를 파괴한다. `SupportsAutomaticShutdown() == false` 모듈은 `Shutdown` 없이 객체만 시작 역순으로 파괴한다(UE도 정적 소멸 때 객체만 사라진다). 발급한 lease가 남아 있으면 Fatal이다(해제 콜백이 파괴된 기록을 쓰지 않게).
  - 이름 조회는 표 크기에 비례하는 선형 비교다(모듈 수십 개). 기록 배열은 생성 뒤 바뀌지 않으므로 다른 스레드의 `IsStarted`·`State`·`Find`가 상태 칸만 원자적으로 읽는다.
  - 묶음: 모듈이 `Startup`에 들어갈 때의 현재 묶음을 기록한다. 현재 묶음은 생성 때 프로세스 묶음이고, 단계 실행(ML1)이 정한다. ML0에서는 시험 접근점(`Private/ModuleLoaderTestAccess.h`)만 바꾼다.
  - 종료 중 재적재: 묶음 종료가 진행 중이거나 프로세스 묶음 종료가 시작된 뒤의 `Load`는 `ShuttingDown`이다. 호스트·엔진 묶음 종료가 끝나면 다시 적재할 수 있다(다음 Engine).

- **의존 시작(MD7)**: `Load(name)`은 manifest의 의존(빌드의 공개·비공개 의존 전부)을 먼저 의존 순서로 시작한다. UE는 의존 DLL을 OS가 싣기만 하고 `StartupModule`은 부르지 않는다. 의존 모듈의 시작은 각 모듈이 `StartupModule`에서 `LoadModuleChecked`로 직접 부른다(`ModuleInterface.h:41-51`). Rocks는 BD30 계약(의존 순서 시작)에 따라 로더가 한다(7절).
- **상태**: `ModuleRecord`는 `NotLoaded → BinaryLoaded(Modular만) → Starting → Started → ShuttingDown → Stopped`와 `Failed`를 가진다.
  - 시작 순번은 `Startup`이 성공한 뒤에 매긴다(UE `LoadOrder` — `ModuleManager.cpp:1107`). 종료 역순은 이 순번을 따른다.
  - 자기 생성·`Startup`이 실패한 모듈은 `Failed`로 남는다(진단용). 실패 처리에서 수명 객체를 파괴하고 등록을 되돌려 두므로 다시 `Load`하면 처음부터 시도한다. 의존이 실패해 시작하지 못한 요청 모듈은 `Failed`가 아니라 시도 전 상태로 돌아간다(`Failed`였으면 `NotLoaded`, Modular는 `BinaryLoaded`). 프로세스 종료 중에는 다시 적재하지 않는다(`ShuttingDown`).
  - 실행 중 해제(3.7절)로 `Stopped`가 된 모듈은 다시 `Load`할 수 있다. 수명 객체를 새로 만들고 타입을 다시 등록한다.
- **재진입**: `Startup` 안에서 다른 모듈을 `Load`할 수 있다(UE와 같다). 단 지금 `Starting`인 모듈(자기 자신이나 시작 중인 의존 사슬)을 다시 적재하면 `Cycle` 오류다. UE는 초기화가 덜 된 포인터를 돌려준다(7절).
  - `Load`가 진행 중일 때(모듈 `Startup`, 그 안의 중첩 `Load`, `Changed` 구독자 안) `ShutdownGroup`을 부르면 Fatal이다(개정 2). 시작 중인 의존 사슬 아래에서 모듈을 내리면 `Startup`이 끝난 뒤 이미 내린 의존 위에서 `Started`가 되기 때문이다. 묶음 종료 콜백(`PreUnload`·`Shutdown`) 안의 `ShutdownGroup`도 Fatal이고, 그 안의 `Load`는 `ShuttingDown` report다.
- **스레드**: `Load`·`StartPhase`·`ShutdownGroup`·`Unload`는 로더를 만든 스레드(주 스레드, 엔진이 생기면 Game lane과 같은 스레드)에서만 부른다. 다른 스레드의 `Load`(ML1 `StartPhase`·ML6 `Unload`도)는 `WrongThread` report다(UE `NotLoadedByGameThread` — `ModuleManager.cpp:924-952`). report를 돌려주지 않는 `ShutdownGroup`은 계약 위반 Fatal이다(개정 2). `IsStarted`·`Find`는 어느 스레드에서든 되고 `Started` 상태 모듈만 돌려준다(`Starting` 중에는 로더 스레드에서만 보인다 — UE `GetModule` 규칙 `ModuleManager.cpp:1498-1523`).
- **종료 중 조회(개정 2)**: 묶음 종료 절차에서 모듈은 자기 `Shutdown` 직전에만 `ShuttingDown`이 된다. 그래서 `PreUnload` 단계 동안 대상 모듈 전원이, 각 `Shutdown` 동안 아직 내리지 않은 모듈(그 모듈의 의존 포함)이 `Find`·`IsStarted`에 `Started`로 보인다(UE는 `UnloadModule` 직전에만 `bIsReady`를 끈다 — `ModuleManager.cpp:1342-1348`). 종료 중의 새 `Load`·`Startup`은 상태가 아니라 로더의 종료 진행 표시로 막는다.
- **형식 있는 조회**: `Find<T>(name)`은 UE `GetModuleChecked<T>`와 같이 검사 없이 `static_cast`한다. T는 그 모듈의 수명 객체 클래스나 그 기반이어야 한다(계약). 공개 interface를 다른 모듈에 둘 때(3.8절)는 interface 클래스가 T다.
- **이름**: 모듈 이름은 대소문자를 구분해 비교한다. 대소문자만 다른 두 모듈 이름은 빌드가 거부한다(Windows 파일 이름 충돌). 의존 목록의 이름도 모듈 이름과 철자가 정확히 같아야 한다(대소문자만 다르면 거부 — 개정 2). 실행 중 비교가 대소문자를 구분하므로 빌드가 대소문자를 무시해 이어 준 의존이 모듈 표에서 끊기지 않게 하기 위해서다. UE `FName`은 대소문자를 무시한다(7절).
- **이벤트**: `Changed`는 로더 스레드에서 `Started`(모듈이 시작을 마침)·`Unloaded`(실행 중 해제)를 알린다(UE `OnModulesChanged` — `ModuleManager.h:522-534`). 종료 절차(3.7절)로 내릴 때는 알리지 않는다(UE와 같다 — `ModuleManager.cpp:1393-1400`). 구독은 Core Delegate 규칙을 따른다.
- **적재 report**: `ModuleLoadReport`는 코드(`ModuleLoadCode`), 요청한 모듈, 처음 실패한 모듈, `nativeError`(OS 오류 코드, `FileStatus`와 같은 이름), 단계 이름(`"OsLoad"`·`"RegisterTypes"`·`"Startup"` 등), 문구(경로·OS 문구·모듈의 `ErrorCode`)를 담는다. `Result<T>`는 `ErrorCode`만 담으므로(`Core/Result.h:9-25`) 원래 OS 코드를 담는 report 관례(`DeviceCreateReport` — [로깅](LoggingSystem.ko.md) 480-489행)를 따른다. 문자열 형식은 ML0에서 Core 문자열 형식으로 정한다.
  - 정한 형식(개정 2): `DeviceCreateReport`와 같이 힙 없는 고정 길이 `char` 칸(NUL 종료 UTF-8)이다 — 모듈 이름 2칸(`ModuleNameBytes`), 단계(`StageBytes`), 문구(`MessageBytes`), 자르면 `truncated`. 모듈의 실패 코드는 `error`(`ErrorCode`) 칸에 따로 담는다. 실패 경로에서 할당이 실패해 report를 잃지 않게 하고, report를 로더·표 수명과 무관하게 복사해 둘 수 있게 하려는 것이다. `String`을 쓰는 안은 allocator를 받아야 하고 할당 실패를 다시 다뤄야 해 버렸다.

| `ModuleLoadCode` | 뜻 | UE 대응 |
|---|---|---|
| `Success` | 시작했거나 이미 시작해 있다 | `Success` |
| `NotFound` | manifest에 없는 이름, 또는 파일이 없다 | `FileNotFound` |
| `Incompatible` | manifest의 `buildId`·파일 크기, 진입 기술의 `entryVersion`·이름이 기대와 다르다 | `FileIncompatible`(UE는 실제로 설정하지 않는다) |
| `OsLoadFailed` | OS 적재 실패(의존 공유 라이브러리 누락 포함). `nativeError`와 OS 문구를 담는다 | `CouldNotBeLoadedByOS` |
| `EntryMissing` | 진입점 `RocksModuleEntry`가 없다 | `FailedToInitialize` |
| `StartupFailed` | 타입 등록이나 `Startup`이 실패했다. 모듈의 `ErrorCode`를 담는다 | `FailedToInitialize` |
| `DependencyFailed` | 의존 모듈이 실패했다. 처음 실패한 모듈을 담는다 | 없음 |
| `Cycle` | 시작 중인 모듈을 다시 적재했다 | 없음 |
| `WrongThread` | 로더 스레드가 아니다 | `NotLoadedByGameThread` |
| `ShuttingDown` | 프로세스 종료 중이다 | `bWasUnloadedAtShutdown` `checkf` |

| `ModuleUnloadCode` | 뜻 |
|---|---|
| `Success` | 내렸다(공유 라이브러리 해제는 미뤘을 수 있다 — 3.7절) |
| `NotStarted` | 시작한 적이 없거나 이미 내렸다 |
| `Unsupported` | `SupportsUnload() == false`이거나 부트 모듈이다 |
| `Busy` | 시작한 의존 모듈이 있거나, 타입 해제가 불가능하다(살아 있는 인스턴스). 붙잡은 쪽(모듈 이름·타입)을 담는다 |
| `WrongThread` | 로더 스레드가 아니다 |

- **기록 책임**: 로더는 report를 돌려주고 직접 Error를 남기지 않는다. 실패를 확정하는 계층(Launch·엔진 호스트·Rocks Studio)이 한 번 기록한다([로깅](LoggingSystem.ko.md) 443-454행 규칙). 로그 문구는 영어이고 모듈 이름·파일 경로·`nativeError`·OS 문구를 담는다.

### 3.5 manifest, 빌드 식별, 실행 파일 링크(Modular)

- **manifest 파일**: 빌드가 Modular 타깃마다 `<Target>[-<Platform>-<Configuration>].modules.json`을 공유 라이브러리와 같은 폴더에 쓴다(UE `<App>.modules` — `Programs/UnrealBuildTool/System/ModuleManifest.cs:77-89`). 키는 영어다.
  - `buildId`: `ModuleBuildId` 문자열.
  - `modules`: 행 배열 `{ name, file, size, dependencies[], loadingPhase, onRequest, executable }`. 의존 순서로 나열한다(JSON 객체의 키 순서는 의미가 없어 배열이다 — [빌드 타깃](BuildTargets.ko.md) BS8d1). `size`는 링크가 끝난 파일의 바이트 수다. `schema`·`engineVersion`·`toolchainFingerprint`도 싣는다(`buildId` 재사용 판정).
  - 모르는 키는 오류다(UE 파서도 거부한다 — `Runtime/Core/Private/Modules/ModuleManifest.cpp:101-229`).
  - manifest는 링크가 모두 성공한 뒤 원자적으로 쓴다(임시 파일 → 이름 바꾸기).
- **manifest 위치**: 엔진 `Binaries/<Platform>/`, 게임 프로젝트 `Binaries/<Platform>/`, 켠 플러그인마다 `<Plugin>/Binaries/<Platform>/`(UE와 같다 — `LaunchEngineLoop.cpp:2541-2543`, `PluginManager.cpp:1856`). 로더는 이 폴더들의 manifest만 읽는다. 폴더를 훑어 파일을 찾지 않는다.
- **`ModuleBuildId`(MD13)**: UE와 같이 출력 폴더의 이전 manifest가 있으면 그 값을 재사용하고, 없거나 엔진 버전·툴체인 fingerprint가 바뀌면 새로 만든다(UE `CanRecycleBuildId` — `Programs/UnrealBuildTool/Modes/WriteMetadataMode.cs:103-117`, `UEBuildTarget.cs:2450-2459`). 값은 manifest에만 있고 코드에 넣지 않는다. 그래서 증분 빌드가 진입점 TU를 다시 컴파일하지 않는다.
  - 설치 SDK는 SDK manifest에 엔진 `buildId`를 싣고, 게임·플러그인 모듈은 빌드할 때 그 값을 받는다.
  - 판정: manifest의 `buildId`가 엔진 manifest와 다르면 그 manifest 전체가 `Incompatible`이다. UE는 "out-of-date" 로그를 남기고 건너뛴 뒤 결국 `FileNotFound`가 된다(`ModuleManager.cpp:1930-1966`). Rocks는 두 ID를 담은 `Incompatible`로 바로 알린다(7절).
  - manifest는 새것인데 공유 라이브러리가 옛것이거나 다른 파일인 경우는 `size` 대조로 잡는다(`Incompatible`). 내용 해시 대조는 비용 때문에 기본으로 하지 않는다.
- **실행 파일 링크(MD21)**: 계약 4(적재 실패의 report)를 지키려면 OS가 `main` 전에 싣는 라이브러리를 줄여야 한다. 그래서 Modular 실행 파일은 부트 모듈(3.9절)만 import한다. 실행 파일의 본문(`ROCKS_IMPLEMENT_APPLICATION`의 `run`)은 타깃의 **호스트 모듈**(예: Rocks Studio의 `StudioHost`)에 두고, 로더가 그 모듈을 적재해 부른다. 나머지 모든 모듈은 로더가 의존 순서로 적재한다.
  - 부트 모듈 공유 라이브러리가 없으면 OS가 `main` 전에 실패한다. 이 경우만 report 계약의 예외다(설치 무결성 문제). Monolithic에는 해당하지 않는다.
  - 모듈 공유 라이브러리 사이의 import는 그대로 둔다. 로더가 의존을 먼저 적재하므로, OS가 import를 해석할 때 이미 적재된 라이브러리를 쓴다.
- **OS 적재와 탐색**: `platform::DynamicLibrary::Open`(절대 경로)을 쓴다. 의존을 먼저 적재하므로 OS가 다른 폴더를 뒤질 필요가 없다. 빌드는 같은 폴더 rpath만 넣는다([빌드 타깃](BuildTargets.ko.md) 3.8절). Linux는 이름 해석을 위해 공유 라이브러리마다 `-soname`을 파일 이름으로 넣는다. 이 문서는 다음을 선행으로 요구한다(ML0).
  - POSIX `dlopen` 실패의 `dlerror` 문구를 report로 넘긴다. 지금 `nativeError`가 0이다.
  - Windows는 의존 DLL 누락(`ERROR_MOD_NOT_FOUND`)을 그대로 넘긴다(지금 동작).
  - ML0 구현(개정 2): `DynamicLibrary::Open`·`OpenSystem`에 선택 인자 `DynamicLibraryError*`를 더했다. 고정 길이 문구 칸(`MaxTextBytes`, 넘치면 UTF-8 경계에서 자르고 `truncated`)에 POSIX는 `dlerror` 문구를 담고(분류용 `stat`·`open`보다 먼저 받는다), Windows는 비워 둔다(`nativeError`가 원래 코드). 호출 시작에 비우므로 앞 실패의 문구가 남지 않는다. ML4 로더가 이 문구를 `ModuleLoadReport.message`에 옮긴다.
- **바이너리 준비(`Prepare`, MD22)**: 로더는 manifest의 모든 모듈(단계 `None`·`DynamicModules` 포함)을 시작 전에 의존 순서로 적재만 하고 진입 기술을 읽어 둔다(UE `LoadModuleBinaryOnly`·`bPreloadedBinaries` — `ModuleManager.cpp:2138-2149`, `PluginManager.cpp:2887-2905`). 설정 등록(아래)에 진입 기술이 필요하기 때문이다. Monolithic은 모듈 표를 읽는 것으로 끝난다.
- **설정 등록 시점(MD22)**: HostSettings는 지금 `options.registerSettings` 자리에서, 로더가 준비한 모든 모듈의 `registerSettings`를 의존 순서로 부른다. 그래서 늦은 단계 모듈의 CVar·설정 이름도 `CompleteStartup`·`ReportUnresolved` 전에 등록돼 오타 경고가 나지 않는다(UE에서는 CVar가 정적 초기화로 설정 로드 전에 생긴다). 모듈별 명시 등록 함수(`RegisterLogSettings` 등)는 각 모듈의 `RegisterSettings`로 옮긴다(ML2).
- **Monolithic**: manifest 파일이 없다. 모듈 표 TU(3.2절)가 같은 내용을 싣고, `buildId`·크기 검사와 OS 적재가 없다. 나머지 순서·상태·이벤트·report는 같다.

### 3.6 리플렉션 타입 등록과 코드 수명

- UE는 모듈을 적재할 때 `ProcessLoadedObjectsCallback`으로 UObject를 등록한다(`ModuleManager.cpp:1094, 1217`, 구독 `Runtime/CoreUObject/Private/UObject/UObjectBase.cpp:1057-1068`). 로더는 CoreUObject를 모른다.
- Rocks도 로더가 타입 시스템을 모르게 한다. 로더 모듈은 Core·PlatformBase에만 의존한다(MD3). 타입 등록은 레지스트리를 가진 쪽이 **타입 등록기**로 한다(MD15).
  - `ModuleTypeRegistrar`는 `Register(module)`·`CanUnregister(module)`·`Unregister(module)` 세 연산을 가진 interface다. 엔진이 `Engine::Initialize`에서 로더에 설정하고 `Engine::Shutdown`에서 뗀다. ML2의 형태는 3.10절이다.
  - 엔진은 모듈마다 `ModuleRegistration`을 따로 가진다. 그래서 모듈 하나의 타입만 골라 해제할 수 있다(지금 `ModuleRegistration`은 mark 기반 LIFO 해제다).
  - 등록 함수는 진입 기술의 `registerTypes`다. 메타 컴파일러는 지금의 재귀 집계 함수 외에 **이 모듈 자신의 타입만** 등록하는 함수를 더 낸다. 로더 경로에서는 의존 모듈이 먼저 등록돼 있으므로 재귀가 필요 없고, 재귀가 다른 모듈의 타입을 이 모듈의 lease로 잡는 일도 없다.
- **등록 시점**:
  - 등록기가 설정된 뒤 시작하는 모듈(엔진 묶음, 엔진이 있을 때 적재한 `None` 모듈): `Startup` 직전에 등록한다(UE도 `StartupModule` 전에 등록한다). 등록이 실패하면 등록기가 그 모듈의 부분 등록을 되돌리고, `Startup`·`Shutdown`은 불리지 않으며 report는 `StartupFailed`(단계 `"RegisterTypes"`)다.
  - 등록기가 설정되기 전에 시작한 모듈(부트·`EarliestPossible`·`PostConfigInit`): `Engine::Initialize`가 등록기를 설정할 때 시작 순서대로 몰아서 등록한다. 그래서 이 모듈들은 자기 `Startup`에서 엔진 레지스트리의 자기 타입을 쓸 수 없다. 몰아서 등록하다 실패하면 이미 등록한 것을 되돌리고 `Engine::Initialize`가 실패한다(모듈은 `Started`로 남는다. 시작이 중단되면 호출자가 호스트 묶음을 내린다).
  - HostSettings의 별도 레지스트리([설정 시스템](ConfigSystem.ko.md) — 로그가 엔진보다 먼저)는 지금처럼 필요한 설정 타입만 명시 등록한다. 엔진 레지스트리와 합치는 일은 이 문서 범위가 아니다.
- **코드 수명 토큰(MD16)**: 로더가 모듈마다 실제 `ModuleLease`를 발급한다. 수는 원자적으로 센다(해제는 어느 스레드에서든 일어난다 — `ModuleLease.h:9-10`).
  - 생성 등록 함수는 기본값 `{}` 대신 이 lease를 받아 `RuntimeTypeOps.module`에 넣는다.
  - 모듈 코드나 데이터를 가리키는 등록(CVar 핸들, trace 채널, 모듈 함수에 묶인 delegate, 서브시스템 클래스)도 lease를 잡는다. 모듈은 `Shutdown`에서 자기가 한 등록을 해제해 lease를 놓는다.
  - lease 수는 공유 라이브러리를 해제해도 되는지만 정한다(3.7절). 해제 가능 여부(`Busy`)는 lease가 아니라 의존 모듈과 타입 해제 가능 여부로 판정한다. 타입·핸들이 lease를 잡고 있는 동안에는 `Shutdown` 전 lease 수가 항상 0이 아니기 때문이다.
  - 정적 링크 모듈의 lease도 같은 방식으로 센다. 정적 링크 코드는 해제되지 않지만, 수를 세야 종료 뒤 남은 참조를 진단할 수 있다.

### 3.7 종료와 실행 중 해제

- **묶음 종료(MD9)**: `ShutdownGroup(group)`이 그 묶음에서 시작한 모듈을 내린다. 절차는 UE `UnloadModulesAtShutdown`과 같다(`ModuleManager.cpp:1438-1496`).
  1. 그 묶음의 자동 종료 대상(`SupportsAutomaticShutdown() == true`)을 시작 순번의 역순으로 고른다.
  2. 고른 모듈 전체의 `PreUnload`를 먼저 부른다.
  3. 각 모듈의 `Shutdown`을 역순으로 부르고, 등록기가 있으면 그 모듈의 타입을 해제한다. 상태를 `Stopped`로 바꾼다. 상태는 그 모듈의 `Shutdown` 직전에만 `ShuttingDown`으로 바꾸므로 2단계까지 대상 모듈은 `Started`로 보인다(개정 2, 3.4절 종료 중 조회).
  4. 공유 라이브러리는 해제하지 않는다(UE와 같다 — `ModuleManager.cpp:1376-1380`). 엔진 묶음은 다음 Engine이 다시 시작할 수 있다.
  - 대상 범위(개정 2): `ShutdownGroup(group)`은 그 묶음과 **그보다 안쪽 묶음**에 남은 모듈을 한 절차로 내린다(예: 호스트 묶음을 내리면 남은 엔진 묶음 모듈도). 안쪽 묶음 모듈이 바깥 묶음 모듈에 의존할 수 있으므로 바깥 묶음만 내리면 살아 있는 모듈의 의존이 먼저 사라진다. 안쪽 묶음 모듈은 나중에 시작했으므로 시작 역순이 곧 안쪽부터다. 정상 흐름(엔진 → 호스트 → 프로세스 순 호출)에서는 차이가 없다.
  - 수명 객체는 `Shutdown` 뒤 파괴하고, 다시 적재하면 새로 만든다(UE `UnloadModule`의 `Module.Reset()`과 같다).
- **묶음별 시점**: UE는 모든 모듈을 `AppPreExit` 뒤, 로그·설정이 살아 있을 때 한꺼번에 내린다(`LaunchEngineLoop.cpp:5182`). Rocks는 호스트 서비스가 단계마다 따로 사라지므로 묶음마다 내린다. 모듈 `Shutdown`이 자기가 쓴 서비스(로그·설정)를 아직 쓸 수 있게 하기 위해서다.
  - 엔진 묶음: `Engine::Shutdown` 안, 서브시스템·월드·ObjectRegistry 정리와 최종 GC 뒤, TypeRegistry 종료 전. 그다음 등록기를 뗀다.
  - 호스트 묶음: 엔진 호스트 본문이 LogHost를 멈추고 HostSettings를 파괴하기 전.
  - 프로세스 묶음(부트·`EarliestPossible`): Launch 정리에서, 도구 로그를 멈추기 전. 부트 모듈은 그중 마지막이다.
  - 이 순서는 [전체 계획](EngineImplementationPlan.ko.md) 3.2절 종료 순서 끝("type metadata/code lease와 trace writer 종료 → 모듈 역순 종료")과 같은 방향이다.
- **프로세스 종료 중**: 프로세스 묶음 종료가 시작되면 다시 적재하지 않는다(`ShuttingDown`). 종료 중에는 `Changed`를 내지 않는다.
- **시작 실패 정리**: 단계가 실패하면 그 묶음에서 시작한 모듈을 같은 절차로 내린다. Launch 단계 실행기(`RunLaunchStages`의 "시작한 단계만 역순 정리")와 같은 규칙이다.
- **실행 중 해제(MD10)**: `Unload(name)`은 Rocks Studio의 플러그인 끄기 같은 경우에 쓴다. UE `UnloadModule`은 `SupportsDynamicReloading`도 의존 모듈도 보지 않는다(`ModuleManager.cpp:1317-1407`). Rocks는 매달린 포인터를 막으려고 먼저 검사한다(7절).
  1. 거부 검사: `SupportsUnload() == false`나 부트 모듈이면 `Unsupported`. 시작한 모듈 가운데 이 모듈에 의존하는 것이 있으면 `Busy`(의존 모듈 이름). 등록기의 `CanUnregister`가 거부하면(이 모듈 타입의 살아 있는 인스턴스) `Busy`(타입 이름). 여기까지는 아무것도 바꾸지 않는다.
  2. 상태를 `ShuttingDown`으로 → `Shutdown` → 타입 해제(`Unregister`) → 수명 객체 파괴 → `Changed(Unloaded)`.
  3. (Modular) lease 수가 0이면 공유 라이브러리를 해제한다. 0이 아니면 해제를 미루고 남은 수를 진단으로 남긴다. lease가 0이 되면 로더 스레드의 다음 호출에서 해제한다(UE merged-modules의 지연 해제와 같은 방식 — `ModuleManager.cpp:887-910`).
  - 플러그인 끄기는 그 플러그인 모듈을 의존 역순으로 해제한다. 1단계 검사는 해제할 모듈 전체에 먼저 해서, 하나라도 거부되면 아무것도 내리지 않고 report를 돌려준다(편집기가 재시작을 안내한다).

### 3.8 실행 중에 적재하는 모듈

- UE `DynamicallyLoadedModuleNames`(링크하지 않고 실행 중에 적재하는 모듈)에 해당하는 모듈 규칙 `DynamicModules`를 둔다(MD11).
  - 빌드는 이 모듈을 타깃에 넣어 빌드하고 manifest에 싣지만, 소비 모듈에 링크하지 않는다(Modular). Monolithic에서는 정적 라이브러리를 실행 파일에 링크한다(UE와 같다).
  - 소비 모듈의 include 경로에는 들어가지 않는다. 쓰는 쪽은 `Find<T>(name)`으로 얻고, T(공개 interface)는 별도 모듈(링크 의존 가능)에 둔다.
  - 의존 규칙: `DynamicModules` 관계에도 모듈 type 포함 규칙([빌드 타깃](BuildTargets.ko.md) BD25)을 적용한다. 순환 검사의 참조 사슬에는 넣지 않는다(링크 의존만의 순환은 거부 — 아래 ML3 세부). 단계 순서 검사(MD6)는 링크 의존이 아니므로 적용하지 않는다. 이런 모듈은 단계 실행 대상이 아니다(아래 "요청 시 적재 행").
- 서드파티 도구 DLL(DXC 등)을 실행 중에 여는 경로는 모듈이 아니다. 지금처럼 `DynamicLibrary`로 직접 연다([빌드 타깃](BuildTargets.ko.md) 9.1절).
- **ML3 구현 세부(개정 4)** — UE 근거는 UnrealBuildTool(`Programs/UnrealBuildTool/`) 기준이다.
  - 규칙: `NativeModuleInput.DynamicModules`(모듈 이름 목록)이고, 엔진 모듈 규칙(`SourceModuleRules`)은 `DynamicModules` 속성으로 채운다. 게임·플러그인 규칙은 `NativeModuleInput`을 직접 채운다. 기술자(`modules[]`)에는 두지 않는다(UE도 `ModuleRules.DynamicallyLoadedModuleNames`만 있다 — `Configuration/Rules/ModuleRules.cs:1416-1419`).
  - **순환(MD11 개정)**: `DynamicModules` 관계는 순환 검사의 참조 사슬에 넣지 않는다. 링크 의존만으로 된 순환은 지금처럼 거부한다. 그래서 링크 의존을 거꾸로 닫는 관계(Engine이 `DynamicModules`로 둔 렌더러 모듈이 Engine에 링크 의존)를 허용한다. 엔진 계층(Core·`Engine/Source`) 모듈은 플러그인 모듈을 `DynamicModules`로도 둘 수 없다([빌드 타깃](BuildTargets.ko.md) 3.9절 BD39 — UE scope 검사와 같다). 켠 플러그인 모듈은 주 실행 파일에 링크되어 기술자 단계로 시작한다(BD36). UE도 같다 — 동적 적재 모듈을 만들 때 참조 사슬을 비운 채 시작하고(`Configuration/UEBuildModule.cs:1300-1301` "Dynamic loads aren't considered a reference chain"), UE의 대표적 쓰임(엔진이 렌더러·플랫폼 모듈을 실행 중에 적재하고 그 모듈이 엔진에 링크)이 바로 이 모양이다. 개정 3까지의 "순환 검사를 적용한다"를 따르면 이 쓰임이 막히므로 버렸다. 이 관계는 시작 순서를 만들지 않으므로(`Load`가 따라가지 않는다) 실행 중 순환 문제도 없다.
  - 대신 거부하는 것(입력 오류): 같은 모듈을 `Dependencies`와 `DynamicModules`에 함께 둠(UE도 오류 — `Configuration/UEBuildModuleCPP.cs:369-374`), 자기 자신, 실행 파일 모듈, 모르는 이름, 모듈 이름과 대소문자만 다른 철자(MD17), 같은 이름 중복. 모듈 표를 만드는 빌드에서는 진입점 모듈이 아닌 모듈(header-only·서드파티·Core 아래·표 제공 모듈에 닿지 않는 모듈 — 표에 행이 없어 `Load(이름)`이 찾지 못한다)도 거부한다.
  - **type 포함 규칙(BD25)**: 빌드의 의존 검사를 `DynamicModules` 관계에도 똑같이 한다(UE도 범위·플러그인 검사에 동적 적재 모듈을 넣는다 — `Configuration/UEBuildTarget.cs:1567, 1625`). BD25의 type 쌍 표는 아직 구현 전(BS2)이고, 지금 있는 형태는 기술자 모듈의 제외 검사다(Editor 모듈·Release의 Developer 모듈·지원하지 않는 플랫폼 — 빠진 모듈을 쓰면 오류). 이 검사를 `DynamicModules`에도 하고, Release 배제 정책(BD20)의 도달 집합도 `DynamicModules`를 따라간다(Monolithic은 링크하므로). BS2가 type 쌍 규칙을 넣을 때 같은 관계 목록에 적용한다.
  - **Monolithic 링크**: 실행 파일은 자기 링크 폐포의 모든 모듈이 `DynamicModules`로 둔 모듈과 그 링크 폐포를 함께 링크한다(동적 모듈의 `DynamicModules`도 따라간다). UE도 Monolithic 실행 파일에 동적 적재 모듈을 넣는다(`Configuration/UEBuildTarget.cs:2809-2822` `GetAllDependencyModules(bIncludeDynamicallyLoaded: true, …)`). 링크 순서는 링크 폐포 뒤에 동적 모듈 쪽을 더한 것이다(정적 라이브러리 순서 — 의존하는 쪽이 앞).
  - **include 경로·정의**: 소비 모듈의 컴파일에 동적 모듈의 public include·정의가 들어가지 않는다. UE도 컴파일 환경은 public·private 의존과 include 경로 모듈만 모은다(`Configuration/UEBuildModule.cs:627-648`).
  - **모듈 표(Monolithic manifest)**: 동적 모듈과 그 링크 폐포의 진입점 모듈도 행으로 싣는다. 위치는 실행 파일 링크 폐포의 행 뒤, 실행 파일 행 앞이다(실행 파일이 마지막인 규칙 유지). 행의 의존은 지금처럼 링크 의존만이다. 그래서 `Load(실행 파일)`은 동적 모듈을 따라 시작하지 않고, 코드가 `Load(이름)`으로 시작한다(시작하면 그 링크 의존을 먼저 시작). 단계 실행도 시작하지 않는다(다음 항목). 설정 등록(3.5절)은 표의 모든 행이 대상이므로 동적 모듈의 설정도 파일을 읽기 전에 등록된다(UE `Prepare`와 같은 범위).
  - **빌드 대상**: 타깃 도달 집합에 동적 모듈과 그 링크 폐포를 넣는다(빌드·메타데이터 생성·공개 헤더 검사). UE도 동적 적재 모듈을 바이너리에 묶어 빌드한다(`Configuration/UEBuildTarget.cs:4554-4565`).
  - **설치 SDK**: `SdkModule`에 `DynamicModules`를 싣는다(SDK manifest 10). 게임 실행 파일이 SDK 모듈을 링크하면 그 동적 모듈도 함께 링크해야 하기 때문이다.
  - **요청 시 적재 행(개정 4 리뷰 결정, UE 방식)**: 실행 파일의 링크 의존 폐포로는 닿지 않고 `DynamicModules` 관계로만 표에 든 행은 단계 실행(`StartPhase`) 대상이 아니다. 행의 단계 값(엔진 모듈은 `Default`)과 무관하게 코드의 `Load(이름)`으로만 시작한다. 링크 의존으로도 닿는 모듈(예: 실행 파일이 링크하고 다른 모듈이 적재 목록에도 둔 모듈)은 보통 행이다.
    - UE 근거: 단계 적재는 기술자(`.uplugin`·`.uproject`)의 모듈 목록만 돈다. `FModuleDescriptor::LoadModulesForPhase`가 `LoadingPhase == Descriptor.LoadingPhase`인 기술자 모듈만 `LoadModuleWithFailureReason`으로 적재한다(`Runtime/Projects/Private/ModuleDescriptor.cpp:779-806`, 플러그인마다 `Runtime/Projects/Private/PluginManager.cpp:2734`). 기술자에 없는 엔진 모듈 — `DynamicallyLoadedModuleNames`로 빌드에 들어간 모듈 — 은 코드가 `LoadModule`을 부를 때만 적재된다. 이는 `ELoadingPhase::None`의 뜻 "Do not automatically load this module"(`Runtime/Projects/Public/ModuleDescriptor.h:55-56`)과 같다. 그래서 요청 시 적재 행은 UE `None`에 대응한다.
    - 표지: 빌드가 행에 남긴다. ML1이 `ModuleTableRow`에 칸(요청 시 적재 표지)을 더하고 생성기가 `LinkClosure`의 동적 쪽 목록(링크 폐포 밖)으로 채우며, `StartPhase`가 그 행을 건너뛴다. 지금 표 생성기는 그 목록을 계산할 수 있다(`LinkClosure.Of(…, out onRequest)`). 칸은 Core 행 형식과 모든 시험 표를 함께 바꾸므로 단계 실행과 같이 ML1에서 넣는다.
    - 기술자 모듈(게임·플러그인)이 기술자의 단계로 자동 시작하려면 링크 의존이나 플러그인 활성(ML1·ML5의 기술자 모듈 행)으로 표에 들어가야 한다(UE도 단계 적재는 기술자 목록 소관이다).
  - **묶음(3.3절)**: 동적 모듈은 `Startup`에 들어갈 때 로더의 현재 묶음을 기록한다(ML0 구현 그대로). 묶음을 단계로 정하는 일은 ML1이다.
  - **Modular(ML4)**: manifest에 싣고 소비 모듈에 링크하지 않는 일은 ML4에서 한다(지금 빌드는 Monolithic뿐이다).

### 3.9 소유와 시작 흐름

- **로더 위치(MD3)**: 새 Core 런타임 모듈 `ModuleLoader`(`Core/Source/Runtime/ModuleLoader`)다. 의존은 Core·PlatformBase뿐이다. `ModuleStartContext`·manifest 읽기·`HostServices`·타입 등록기 interface도 이 모듈에 둔다. 수명 객체 기반 클래스·진입 기술·진입점 매크로·모듈 표 형식은 Core에 둔다(3.1절, 개정 2).
- **빌드 표지(개정 2)**: 모듈 규칙 입력(`NativeModuleInput`)에 `ProvidesModuleTable`(Core가 켠다 — 이 모듈에 닿는 실행 파일마다 모듈 표 TU를 만든다), `ThirdParty`(서드파티 규칙이 켠다 — 모듈 표에서 뺀다), `LoadingPhase`(기술자 모듈의 단계)를 더했다. 설치 SDK manifest는 앞 두 표지를 실어 게임 실행 파일도 모듈 표를 가진다(manifest 버전 6). 빌드는 모듈 표에 드는 모듈의 소스에만 `ROCKS_MODULE_NAME=<이름>`을 정의하고, 규칙·명령줄은 이 이름을 정의할 수 없다(예약).
- **소유(MD4)**: 프로세스에 하나이고 Launch의 `ApplicationContext`가 소유한다. 전역 접근자(UE `FModuleManager::Get()`)는 두지 않는다. 필요한 곳은 `ApplicationContext`·`ModuleStartContext`·`Engine` 설정으로 참조를 받는다. Launch 모듈은 `ModuleLoader`에 의존한다.
- **부트 모듈**: Launch 모듈의 의존 폐포(Launch·ModuleLoader·Core와 그 의존)다. 로더가 생기기 전에 이미 실행 중이다. 로더는 만들어질 때 이 모듈들을 의존 순서대로 바로 시작한다(`Startup` 호출). 폐포 안의 면제 모듈(PlatformBase·DeterministicMath·Base — 3.1절)은 수명 객체가 없으므로 시작 대상이 아니다(개정 2). 부트 모듈의 단계는 `EarliestPossible`로 강제한다(빌드 검사, MD6과 맞물림). 부트 모듈은 실행 중 해제할 수 없다.
- **Launch 단계와 맞물림**: crash-launch 설계(8.1절)의 단계에 다음을 더한다(그 문서 개정은 병합 뒤 — 8절).
  - 도구 로그 시작(7단계) 뒤, 본문(8단계) 전에 로더를 만들고 `Prepare`, 부트 모듈과 `EarliestPossible`을 시작한다. 대화상자 억제·도우미 시작(5단계)보다 뒤다.
  - 엔진 호스트 본문: HostSettings(설정 등록 때 모든 모듈의 `registerSettings`) → LogHost 시작 → `HostServices`에 등록 → `StartPhase(PostConfigInit)` → `Engine::Initialize`(등록기 설정, `PreDefault`·`Default`·`PostDefault`, 끝에 `PostEngineInit`) → 엔진 루프 → `Engine::Shutdown`(엔진 묶음 종료) → 호스트 묶음 종료 → LogHost·HostSettings 정리.
  - 도구 본문: 설정을 쓰는 도구는 `PostConfigInit`을 직접 부른다. 엔진을 만들지 않는 도구는 엔진 단계 모듈을 시작하지 않는다.
  - 종료: 본문 반환 → 프로세스 묶음 종료(도구 로그 정리 전) → 7·6단계 역순 정리 → 크래시 처리 제거.
- **Rocks Studio(ML5)**: 실행할 때 프로젝트 경로를 받는다.
  1. `.rocksproject`와 플러그인 활성 조건([빌드 타깃](BuildTargets.ko.md) 3.6절, 문맥: Editor 타깃·현재 플랫폼·현재 구성)으로 켤 플러그인을 정한다.
  2. 엔진·게임·켠 플러그인의 manifest를 읽고 `buildId`를 검사한다.
  3. 단계마다 엔진·게임·플러그인 모듈을 함께 의존 순서로 시작한다.
  4. 게임 모듈 적재 실패는 report와 함께 시작을 중단한다. 다시 빌드를 권하는 편집기 UI는 Rocks Studio 설계에서 정한다.
  - 빌드 시점에 이미 플러그인 조건을 해석해 게임 Editor 타깃을 만들었으므로, 실행 중 해석 결과와 빌드 결과가 다르면 `Incompatible`이다(게임 manifest에 빌드한 플러그인 목록을 싣고 대조한다).
- **시험(MD20)**: 시험·도구 실행 파일도 모듈 표를 가지고 로더로 모듈을 시작한다. 시험 `TEST` 정적 등록은 지금 예외를 유지한다. Modular 시험([빌드 타깃](BuildTargets.ko.md) BD31)에서 `TEST` 등록 목록의 머리는 TestSupport 공유 라이브러리에 한 벌만 두고(BD29), 시험 묶음이 들어간 공유 라이브러리는 실행 중 해제하지 않는다.

### 3.10 ML2 연결 — ML1 전(개정 3)

ML1(로딩 단계·묶음 전환·Launch 통합)은 crash-launch 병합을 기다린다. ML2는 그 전에 지금 있는 호스트(Sandbox·Commandlet·시험 runner·WorldBench)로 타입·설정 등록을 로더에 붙인다. 애매한 곳은 UE 5.8을 따랐다.

**재개 조건**: ML2는 [리플렉션 생성 소스](ReflectionGeneratedSources.ko.md)(빌드 시간 개선 1차분 — 등록 함수가 모듈 생성 소스로 가고 모듈 생성 헤더는 선언만 남는다) GS1이 병합된 뒤 재개한다. 두 작업이 같은 생성기·빌드 파일(`CppEmitter`·`MetadataLayout`·`ActionGraph`·`NativeBuildModels`)을 고치므로, 아래 생성 등록 함수 설계는 그 구조에 맞춘 것이다. 착수했던 코드는 되돌렸고 재개 때 이 절대로 다시 구현한다. GS1 병합(493f9a0d) 뒤 재개했다 — GS1 위에서 정한 세부는 아래 "GS1 위 세부"다.

**타입 등록 함수(MD15).**
- 정의 위치([리플렉션 생성 소스](ReflectionGeneratedSources.ko.md) 5.5절·GSD10): 메타 컴파일러가 모듈 생성 소스(`<out>/Generated/<M>/Source/<M>.module.generated.cpp`)에 **이 모듈 자신의 타입만**(public 다음 private) 등록하는 비inline 함수 `rocks::generated::<M>::RegisterModuleOwnTypes(ModuleRegistration&, const ModuleLease&)`를 더 정의하고, 모듈 생성 헤더(있는 쪽 모두 — public·private)에는 선언만 둔다(기본 인자 없음). 실패하면 이 호출이 등록한 것을 되돌린다. 재귀 함수 `RegisterModuleTypes`·`RegisterPrivateModuleTypes`는 그대로 두고 타입 등록 줄만 생성기의 같은 코드에서 낸다(GS2 병합 충돌을 줄이려고 재귀 함수가 새 함수를 부르게 바꾸지 않는다). 재귀 함수는 로더 밖(HostSettings 설정 타입, 시험, 게임 템플릿)에서 계속 쓴다.
- 진입 기술 연결: 빌드는 진입점 모듈의 소스에 `ROCKS_MODULE_TYPES=0|1`(`Metadata`가 있는 모듈이면 1)을 정의한다(예약 매크로). `ROCKS_IMPLEMENT_MODULE`은 이 값이 1이면 `RegisterModuleOwnTypes`를 **스스로 선언하고** 진입 기술 `registerTypes`에 싣는다. 그래서 모듈 cpp가 모듈 생성 헤더를 include할 필요가 없다(GSD6 — 손으로 쓴 헤더는 그것을 include하지 않는다). 생성 소스가 빠지면 링크 오류다. 빌드 밖 컴파일(정의 없음)은 0이다.
- 소스 없는 메타데이터 모듈(CoreTypeBindings·시험 fixture 등): GSD7로 정적 라이브러리가 되지만 손으로 쓴 진입점이 없다. 빌드(`MetadataLayout`)가 그런 모듈에만 진입점 생성 TU(`<M>.ModuleEntry.cpp` — `ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, <M>)` 한 줄)를 규칙 생성 소스로 더해 진입점 모듈로 만든다. 그래야 로더가 그 모듈의 타입을 등록할 수 있다.
- 버린 대안: 모듈 cpp가 모듈 생성 헤더를 include하고 매크로가 함수 유무를 감지하는 안(include를 빠뜨리면 조용히 null이고 GSD6과도 어긋난다), 자기 타입 함수를 집계 헤더의 inline으로 두는 안(초기 안 — 생성 소스 분리와 정면으로 겹친다), 빌드가 별도 TU(`<M>.ModuleTypes.cpp`)에서 집계 헤더의 inline 함수를 감싸는 안(모듈 생성 소스가 생기면 필요 없다).
- `unregisterTypes`는 두지 않는다. 등록기가 모듈마다 `ModuleRegistration`을 두므로 그 기록의 `Unregister`가 모듈 타입을 되돌린다(UE도 모듈별 해제 함수가 없다).
- GS1 위 세부(2026-11-20):
  - 진입점 판정(`ModuleTables.EntryModules`)이 리플렉션 생성 소스(`NativeModuleInput.ReflectionSources`)도 모듈 코드로 센다. GS1이 정한 면제(GSD14 — 리플렉션 생성 소스만 있는 모듈은 진입점이 없어도 된다)를 ML2가 거둔다. 그래서 설치 SDK 표지 `ReflectionOnlyLibrary`(SDK manifest 7)가 필요 없어 없앴고 SDK manifest를 8로 올렸다. 표지를 남겨 두는 안은 SDK 모듈과 소스 빌드 모듈이 다른 판정을 받게 되어 버렸다.
  - `ROCKS_MODULE_TYPES`는 새 규칙 표지 없이 `ReflectionSources`가 있으면 1이다(모듈 생성 소스가 있는 모듈 = 메타데이터 모듈). 설치 SDK 모듈은 컴파일하지 않으므로 해당이 없다.
  - 진입점 생성 TU `<M>.ModuleEntry.cpp`는 손으로 쓴 소스와 규칙 생성 소스가 모두 없는 메타데이터 모듈에만 `MetadataLayout`이 더한다(규칙 생성 소스 경로 — `<out>/GeneratedSources/<M>/`). 그 가운데 진입점 모듈(`EntryModules` — 서드파티가 아니고 모듈 표 제공 모듈에 닿는 모듈)만이다. 나머지는 빌드가 `ROCKS_MODULE_NAME`을 정의하지 않아 진입점 매크로가 컴파일되지 않는다.

**등록기와 로더(MD15·MD16).**
- `ModuleTypeRegistrar`(ModuleLoader 모듈): `Register(name, registerTypes, lease)`·`Unregister(name)`·`CanUnregister(name)`(ML6). 로더는 `registerTypes`가 있는 모듈만 부르고 로더가 발급한 그 모듈의 실제 lease를 넘긴다. 등록 함수는 이 lease를 `RuntimeTypeOps.module`에 넣는다.
- `ModuleLoader::AttachTypeRegistrar(registrar)`: 이미 시작한 모듈을 시작 순서로 몰아서 등록한다. 실패하면 이번에 등록한 것을 역순으로 되돌리고 붙이지 않으며 report(`StartupFailed`, 단계 `"RegisterTypes"`)를 돌려준다. 붙은 뒤 시작하는 모듈은 `Startup` 직전에 등록하고, `Startup`이 실패하면 되돌린다. 묶음 종료는 각 모듈의 `Shutdown` 뒤 그 모듈의 타입을 되돌린다. `DetachTypeRegistrar`는 남은 등록을 시작 역순으로 되돌린다. 등록기는 한 번에 하나다(동시에 여러 Engine이 같은 로더를 쓰는 참조 수는 ML1).
- 일반 구현 `ModuleTypeRegistrations`(ObjectRuntime): `TypeRegistry` 하나에 모듈마다 `ModuleRegistration`을 둔다. `Unregister`는 그 기록을 되돌리되, 남은 객체·native 파생 때문에 `Busy`면 기록 객체만 놓고(붙잡은 lease 반환) 남은 레코드는 `TypeRegistry::Shutdown`이 dependent 순서로 반납한다(지금 Engine의 종료 규칙과 같다). 실행 중 해제(ML6)는 `CanUnregister`로 먼저 거른다.

**엔진(MD15).**
- `EngineConfig::modules`(`ModuleLoader*`)를 더한다. 있으면 `Engine::Initialize`가 엔진 `TypeRegistry`의 `ModuleTypeRegistrations`를 그 로더에 붙이고, `Engine::Shutdown`·초기화 실패 정리가 `TypeRegistry` 종료 전에 뗀다.
- 없으면(지금의 시험·호스트) Engine이 이 실행 파일의 모듈 표(`GeneratedModuleTable()`)로 자기 로더를 만들고 `Engine` 모듈(의존 폐포 — World 등)을 적재해 같은 방식으로 등록한다. 엔진 종료 때 그 로더를 내린다. ML1에서 Launch가 늘 로더를 넘기면 이 대체 경로를 없앤다. 그래서 `Engine::Initialize`의 `RegisterWorldTypes` 명시 호출이 없어진다.
- `EngineConfig::registerModuleTypes`(로더 밖 게임 모듈 집계)와 `registerTypes`(native 타입)는 그대로 둔다.

**설정 등록(MD22, Monolithic).**
- `HostSettingsOptions::modules`(`ModuleLoader*`)를 더한다. `HostSettings::Initialize`는 지금 `RegisterLogSettings`·`RegisterTraceSettings`·`RegisterDeviceProfileSettings`를 부르던 자리에서 모듈 표 순서(의존 순서)로 모든 모듈의 `registerSettings`를 부른다. 그 뒤 `options.registerSettings`(호스트 고유 설정)를 부른다. 로더가 있으면 그 표를 쓰고, 각 모듈의 lease를 `ConfigContext::RetainModule`로 문맥에 맡긴다(문맥 파괴 때 놓는다 — CVar 핸들·설정 struct가 모듈 코드를 가리키는 동안 모듈 코드를 붙잡는다). 로더가 없으면 이 실행 파일의 `GeneratedModuleTable()`을 빈 lease(정적 링크 코드)로 쓴다.
- LogHost·TraceControl·Config 모듈의 설정 등록을 각 모듈 수명 객체의 `RegisterSettings`로 옮긴다. 공개 함수(`RegisterLogSettings` 등)는 시험이 쓰므로 남기고 호스트 코드는 부르지 않는다.
- HostSettings 자기 레지스트리의 설정 타입 등록(Config·TraceControl 집계)은 3.6절대로 명시 등록으로 남는다.

**호스트(MD4 대체, ML1까지 — 개정 5에서 3.11절로 대체).**
- Sandbox 엔진 호스트가 Launch를 대신해 프로세스 로더를 만든다: 이 실행 파일의 모듈 표로 만들고 실행 파일 모듈을 적재(모든 모듈 시작) → HostSettings·Engine에 넘김 → 끝에 `ShutdownGroup(Process)`. 로더는 HostSettings·Engine보다 오래 산다.
- Commandlet·시험 runner는 로더를 만들지 않는다(대체 경로를 쓴다). WorldBench는 엔진 없이 World 레지스트리를 만들므로 자기 로더와 `ModuleTypeRegistrations`로 등록한다.
- 남은 lease 진단(MD16): `ModuleLoader::ModulesWithLeases(names)`가 lease가 남은 모듈 이름과 수를 돌려준다. 호스트는 로더를 파괴하기 전에 불러 영어로 기록한다(이름은 상한까지, 넘친 모듈은 수만 — 로더 소멸의 Fatal은 마지막 안전장치로 남는다).
- 로더 조회 API(ML2): `Table()`은 만들 때 받은 모듈 표다(HostSettings가 설정 등록 순서로 쓴다). `AcquireLease(name)`은 그 모듈의 lease를 하나 더 발급한다(설정 문맥이 맡는다 — 어느 스레드에서든, 표에 없으면 `NotFound`). `TypeRegistrar()`는 지금 붙은 등록기다(없으면 null).

**ML1 뒤로 넘긴 것.**
- 모듈 `Startup`이 호스트 서비스(TraceSession·ConfigContext·Engine)를 쓰는 등록: 지금은 모든 모듈이 서비스가 생기기 전에 시작하므로(단계 없음) trace 채널·trace 타입 등록(`RegisterGpuTraceTypes` 등)과 그 lease, 콘솔 명령 등록을 `Startup`으로 옮길 수 없다.
- 엔진 묶음의 참조 수(동시에 여러 Engine)와 등록기의 묶음별 시점(`Engine::Shutdown`의 엔진 묶음 종료).
- 모듈별 CVar 핸들 단위 lease(지금은 문맥이 모듈 lease를 통째로 잡는다 — 실행 중 해제(ML6) 때 핸들 해제와 함께 정한다).

### 3.11 ML1 구현 세부(개정 5)

crash-launch(공통 실행 진입점 Launch) 병합 뒤 단계 실행·묶음·Launch 통합을 정한다. 애매한 곳은 UE 5.8을 따랐다. 3.10절의 "호스트(MD4 대체, ML1까지)"와 Engine 대체 로더는 이 절로 대체한다.

**단계와 모듈 표.**
- Core `LoadingPhase`에 `PostEngineInit`·`None`을 더한다(값 순서: `EarliestPossible`, `PostConfigInit`, `PreDefault`, `Default`, `PostDefault`, `PostEngineInit`, `None`). 빌드 기술자 `loadingPhase`도 같은 값을 받는다(MD5).
- 엔진 모듈 규칙(`SourceModuleRules`)에 `LoadingPhase` 속성(기본 `Default`)을 둔다. 기술자 모듈은 지금처럼 기술자 값이다.
- `ModuleTableRow`에 `onRequest`(요청 시 적재 행 — 3.8절)를 더한다. 생성기가 `LinkClosure.Of(…, out onRequest)`로 채운다. 행 형식은 실행 파일 안의 생성 TU와 로더 사이 계약이라 `ModuleEntryVersion`은 그대로다(진입 기술은 바뀌지 않는다).
- 실행 파일 모듈 행의 단계는 늘 `None`이다(빌드가 규칙 값과 무관하게 싣는다). 본문은 이미 실행 중인 코드이고, 단계 실행이 실행 파일 행을 시작하면 그 의존(시험 fixture 같은 `None` 모듈 포함)을 모두 끌어 시작하기 때문이다. UE에서도 Program의 주 모듈은 로딩 단계로 적재하지 않는다(게임 주 모듈은 실행 파일이 아니라 기술자 모듈이다). 필요하면 본문이 `Load`로 시작한다.
- 시험 fixture 모듈(ReflectionTestFixtures·ConfigTestFixtures·CurvesTestFixtures·WorldTestFixtures·AssetTestFixtures)은 규칙 단계가 `None`이다. 시험이 자기 레지스트리에 그 타입을 등록하므로, 시험 실행기의 엔진 묶음이 모든 Engine에 등록하면 겹친다(같은 이름 타입 `AlreadyExists`). UE에서도 시험·도구 모듈은 단계로 적재하지 않는다.

**빌드 검사.**
- **단계 순서(MD6)**: 모듈 표 행마다, 행의 의존(면제 모듈을 거친 대체 의존 포함 — 3.2절)의 단계는 그 행의 단계보다 늦을 수 없다(`None` 의존은 허용, `None` 행은 검사하지 않는다). 요청 시 적재 행도 같다. 실행 파일 행은 `None`이라 검사하지 않는다.
- **층별 기본 단계(개정 5 리뷰)**: 엔진 모듈 규칙의 기본 단계는 층이 정한다. Core 층(`CoreModuleRules`)은 `PostConfigInit`, Engine 층(`EngineModuleRules`)은 `PreDefault`다. UE는 기술자 단계 전에 핵심 모듈을 따로 올린다 — `FEngineLoop::LoadCoreModules`가 CoreUObject를, `LoadPreInitModules`가 Engine·Renderer를 올린다(`Runtime/Launch/Private/LaunchEngineLoop.cpp:4424-4450`). 그래서 `PostConfigInit` 모듈이 설정·로그·trace 모듈에, `PreDefault` 게임·플러그인 모듈이 Engine·World에 의존할 수 있고, 호스트 서비스 모듈(Config·LogHost·HostConfig·TraceControl 등)은 엔진마다 다시 시작하지 않고 호스트 묶음으로 산다. 모두 `Default`로 두는 안은 이 두 의존을 MD6이 막고 호스트 서비스 모듈이 Engine마다 시작·종료돼 버렸다. 위반은 모듈 표를 만들 때 오류다(영어 문구에 두 모듈과 단계). 모듈 표에 드는 모듈만 보므로 면제 모듈의 단계는 뜻이 없다.
- **부트 모듈(MD4)**: 새 표지 `ProvidesLaunch`(Launch 규칙이 켠다)를 둔다. 그 모듈의 링크 폐포에 든 진입점 모듈(지금은 Core·ModuleLoader·Launch)은 규칙 단계가 `EarliestPossible`이어야 한다. 아니면 오류다. 빌드가 몰래 바꾸지 않고 규칙에 적게 하는 것은 MD6 검사가 규칙 값으로 돌기 때문이다(Core가 `Default`로 남으면 `PostConfigInit` 모듈이 Core에 의존할 수 없다).
- 설치 SDK manifest 11: `SdkModule`에 `LoadingPhase`·`ProvidesLaunch`를 싣는다. 게임 실행 파일의 모듈 표가 SDK 모듈의 단계를 알아야 MD6·부트 검사가 소스 빌드와 같은 판정을 하기 때문이다.

**로더.**
- `ModuleLoadReport StartPhase(LoadingPhase phase)`: 아직 돌지 않은 단계부터 `phase`까지 차례로 돈다. 단계마다 현재 묶음을 그 단계의 묶음(`EarliestPossible` → 프로세스, `PostConfigInit` → 호스트, `PreDefault`–`PostEngineInit` → 엔진)으로 바꾸고, 표 순서(의존 순서)로 그 단계 값을 가진 행 가운데 `onRequest`가 아닌 행을 시작한다(의존은 `Load`처럼 먼저 시작한다).
  - 실패하면 그 단계를 멈추고 report를 돌려준다(`requested`는 실패한 단계 이름 — 부른 단계보다 앞일 수 있다). 그 단계는 돈 것으로 치지 않는다. 정리는 호출자가 그 묶음을 내린다(3.3절).
  - 이미 돈 단계를 다시 부르면 할 일 없이 성공이다(UE `PluginManager.cpp:2909-2913`). 지금 묶음보다 앞 묶음의 단계를 부르면 계약 위반 Fatal이다(UE는 Error 로그 — 2842-2937행. Rocks는 호스트 순서 오류를 숨기지 않는다). `None`을 부르거나, 로더 스레드가 아니면(이 경우는 `WrongThread` report), `Load` 진행 중·묶음 종료 콜백 안에서 부르면 Fatal이다(`ShutdownGroup`과 같은 규칙).
  - `ShutdownGroup(group)`은 그 묶음과 안쪽 묶음의 단계를 다시 돌 수 있게 되돌리고 현재 묶음을 바깥 묶음으로 돌린다(엔진 묶음을 내리면 다음 `StartPhase(PreDefault…)`가 다시 돈다 — 다음 Engine).
  - 조회: `bool PhaseStarted(LoadingPhase)`(그 단계를 다 돌았는지), `ModuleGroup CurrentGroup()`.
- **엔진 묶음 참조 수(MD9)**: `ModuleLoadReport StartEngineGroup()`은 참조 수를 올리고, 처음이면 `StartPhase(PostDefault)`를 돈다(엔진 앞 단계를 차례로). 실패하면 엔진 묶음을 스스로 내리고 참조 수를 되돌린 뒤 report를 돌려준다. `void ShutdownEngineGroup()`은 참조 수를 내리고 마지막이면 엔진 묶음을 내린다. 참조 수가 0이 아닐 때의 `ShutdownGroup`(어느 묶음이든)과 로더 파괴는 계약 위반 Fatal이다(엔진이 로더·호스트 묶음보다 먼저 끝나야 한다). `PostEngineInit`은 각 Engine이 `StartPhase(PostEngineInit)`으로 부른다(두 번째 Engine에는 할 일이 없다).
- **타입 등록기 여럿(MD15 개정)**: 동시에 사는 Engine마다 자기 `TypeRegistry`가 있으므로 등록기를 `MaxTypeRegistrars`(4)개까지 붙인다. `AttachTypeRegistrar(registrar)`는 그 등록기에만 시작한 모듈을 몰아서 등록하고, 붙은 뒤 시작하는 모듈은 붙은 모든 등록기에 `Startup` 직전 등록한다(하나라도 실패하면 이 모듈이 이미 등록한 등록기를 되돌리고 `StartupFailed`). 묶음 종료는 모든 등록기에서 되돌린다. `DetachTypeRegistrar(registrar)`는 그 등록기의 등록만 되돌린다. 같은 등록기를 두 번 붙이거나 자리가 없으면 Fatal이다. 조회 `TypeRegistrarCount()`. UE는 UObject 레지스트리가 프로세스에 하나라 이 문제가 없다(7절).

**Launch(MD4).**
- Launch 모듈이 `ModuleLoader`에 의존한다. 시작 단계 `Launch.Modules`를 도구 로그 단계 뒤에 둔다: 범용 할당자(`DefaultAllocator`)와 이 실행 파일의 `GeneratedModuleTable()`로 로더를 만들고 `StartPhase(EarliestPossible)`(부트 모듈)을 돈다. Modular의 `Prepare`는 ML4다.
  - 실패는 Launch가 Error로 한 번 기록하고(모듈·단계·문구) 본문 없이 `ModuleStartupFailureExitCode`(1)로 끝낸다(Abort 단계 — 시작한 단계만 역순 정리).
  - 정리(본문 반환 뒤, 도구 로그 정리 전): `ShutdownGroup(Process)` → 남은 lease 진단(Error, 이름은 `MaxLeaseReportNames` 16까지, 넘친 수) → 로더 파괴. 본문이 `RequestExitWithoutStaticTeardown`을 불렀으면(멈춘 출력 스레드를 포기해 설정 문맥이 lease를 쥔 채) 로더를 내리지도 파괴하지도 않는다(범용 할당자와 같은 규칙, 내장 진입은 그 요청을 무시하므로 내린다).
- `ApplicationContext::Modules()`가 그 로더다(본문 수명 동안 유효). 전역 접근자는 여전히 없다.

**호스트.**
- 엔진 호스트(Sandbox): `context.Modules()`를 쓴다. HostSettings(모든 모듈 `registerSettings`) → LogHost 시작 → `StartPhase(PostConfigInit)` → trace·엔진 → 엔진 종료 뒤 `ShutdownGroup(Host)` → LogHost 정지·HostSettings 파괴. 프로세스 묶음은 Launch가 내린다.
- WorldBench(엔진 없는 World 레지스트리): `context.Modules()`에서 `Load("World")`(프로세스 묶음)와 자기 `ModuleTypeRegistrations` 등록기를 붙였다 뗀다.
- 엔진 묶음의 범위: 실행 파일 표에서 엔진 단계 값을 가진 모든 행이다(3.10절 대체 로더는 Engine 모듈 폐포만 시작했다). 시험 실행기에서는 Engine마다 그래픽·셰이더·에셋 등 Engine 층 모듈을 시작·종료하고 그 메타데이터 타입을 각 Engine `TypeRegistry`에 등록한다. UE와 같은 동작이다. 같은 이름 타입이 겹치는 모듈(시험 fixture)은 `None`으로 뺀다.
- 등록기 자리(`MaxTypeRegistrars`)를 넘는 동시 Engine은 Fatal이다(report가 아니다). 동시에 사는 Engine 수는 프로그램 구조가 정하는 상수라 실행 중 조건이 아니라 프로그램 버그이기 때문이다.
- 시험 runner: Launch가 만든 로더를 시험 지원 함수 `test::ProcessModules()`(시험 runner 본문이 정의 — 실행 파일의 시험 전용 접근점, 제품 코드의 전역 접근자가 아니다)로 시험에 준다. 엔진을 만드는 시험은 이 로더를 `EngineConfig::modules`에 넣는다. 그래서 한 프로세스에서 Engine을 차례로 만드는 모든 시험이 같은 로더의 엔진 묶음을 매번 시작·종료한다. 로더 자체 시험은 지금처럼 자기 표로 따로 로더를 만든다.

**엔진.**
- `EngineConfig::modules`는 필수다. 없으면 `Initialize`가 `InvalidArgument`로 실패한다(Error 기록). 3.10절의 대체 로더와 `ROCKS_MODULE_NAME` 적재는 없앤다.
- `Initialize`의 타입 단계: 엔진 `TypeRegistry`·`ModuleRegistration`을 만든 뒤 등록기를 붙이고(이미 시작한 프로세스·호스트 묶음 모듈을 몰아서 등록) `StartEngineGroup()`(엔진 단계 모듈을 `Startup` 직전 등록하며 시작) → 게임 리플렉션 모듈·native 타입 → ObjectRegistry … → `EngineClient::OnEngineStart` → `StartPhase(PostEngineInit)`. 마지막이 실패하면 client의 `OnEngineShutdown`을 부른 뒤 초기화 실패 정리를 한다.
- `Shutdown`·초기화 실패 정리: World·ObjectRegistry 정리와 최종 GC 뒤, `ShutdownEngineGroup()` → 등록기 떼기 → `TypeRegistry::Shutdown`(3.7절 엔진 묶음 시점).

**ML1 뒤 후속(이 단계에서 하지 않음).**
- 호스트 서비스 등록: 3.12절(개정 6)에서 했다(`Engine` 서비스는 그대로 후속).
- 3.10절 끝에 넘긴 것(서비스를 쓰는 `Startup` 등록 — trace 채널·trace 타입·콘솔 명령, 핸들 단위 CVar lease)은 그대로 후속이다. 단계가 생겨 `PostConfigInit` 모듈이 설정 확정 뒤 시작하므로 옮길 수 있는 조건은 갖췄다.
- 기술자 모듈 행의 플러그인 활성(ML5)과 Modular `Prepare`(ML4).

### 3.12 ML1 후속 — 호스트 서비스와 서비스를 쓰는 Startup 등록(개정 6)

ML1(3.11절)으로 `PostConfigInit`·엔진 단계 모듈이 설정·로그·trace가 생긴 뒤에 시작하므로, 3.10절 끝에 넘긴 "서비스를 쓰는 `Startup` 등록"을 할 수 있게 됐다. 애매한 곳은 UE 5.8을 따랐다.

**서비스 키를 Core로(MD18 보강).**
- `HostServiceKey`·`HostServiceTraits`와 매크로 `ROCKS_DECLARE_HOST_SERVICE`·`ROCKS_DEFINE_HOST_SERVICE`를 Core `Core/Module/HostServiceKey.h`로 옮긴다. 서비스 표 `HostServices`는 그대로 ModuleLoader 모듈이다.
- 이유: 서비스 형식의 키는 그 형식을 가진 모듈이 선언·정의한다(MD18). 그런데 `LogSystem`은 Core에 있고 Core는 ModuleLoader에 의존할 수 없다(ModuleLoader가 Core에 의존한다). 진입점 계약을 Core에 둔 것(개정 2)과 같은 이유다. 키 헤더는 Core 기본 형식만 쓴다.
- 버린 안: 키를 ModuleLoader 모듈에 모아 정의(소유 모듈이 아닌 곳이 다른 모듈 형식을 알게 된다), Config·TraceCapture가 ModuleLoader에 의존(키 하나 때문에 의존이 는다).

**서비스로 둔 형식(이번).** `rocks::LogSystem`(Core), `rocks::config::ConfigContext`(Config), `rocks::TraceSession`(TraceCapture). 각 형식의 공개 헤더에서 선언하고 소유 모듈 cpp에서 정의한다.
- `Engine`은 이번에 두지 않는다. 쓰는 모듈이 없고, 동시에 여러 Engine이 엔진 묶음을 함께 쓸 때 어느 Engine을 서비스로 보일지(처음 Engine이 먼저 끝나면 남은 모듈이 끝난 Engine을 가리킨다)를 먼저 정해야 한다. 엔진 서비스가 필요한 모듈이 생길 때 정한다.

**호스트 등록.** 엔진 호스트(Sandbox)는 LogHost·설정 문맥·trace 세션을 만든 뒤 `StartPhase(PostConfigInit)` 직전에 세 서비스를 등록하고(trace 세션은 실행 중일 때만), `ShutdownGroup(Host)` 직후에 뗀다. 그래서 호스트·엔진 묶음 모듈의 `Startup`·`Shutdown` 동안 서비스가 산다. 등록 실패는 Warning이고 모듈은 서비스 없이 시작한다(서비스는 선택이다 — 모듈이 `Find` 결과 null을 다룬다).

**모듈 Startup 등록(첫 사용처).** RenderBase 모듈이 수명 객체 `Startup`에서 `TraceSession` 서비스가 실행 중이면 GPU trace 채널 "gpu"와 기록 형을 등록하고(`RegisterGpuTraceTypes`), `Shutdown`에서 형·채널 순으로 등록을 푼다(`UnregisterGpuTraceTypes`). 등록한 동안 모듈 lease를 잡는다(채널·형 객체가 모듈 데이터라 MD16). 지금까지는 시험만 이 함수를 불렀다.
- 등록 실패는 Warning이고 `Startup`은 성공한다. GPU 계측은 진단 기능이라 엔진 시작을 막지 않는다(UE trace 채널도 실패를 알리지 않는다). 정의 버퍼가 차서 난 `CapacityExceeded`도 다시 시도하지 않는다(한계).
- 같은 세션에서 다시 시작해도 다시 등록된다(다음 Engine·재적재). trace 세션이 등록을 푼 같은 이름 채널·같은 스키마 형의 id를 다시 쓰기 때문이다([trace](TraceSystem.ko.md) 개정 9).
- 서비스를 `Startup` 뒤에도 쥐는 모듈의 조건: 그 모듈은 호스트가 서비스를 등록한 기간 안에 시작하고 내려가는 묶음이어야 한다. 엔진 호스트는 서비스를 호스트 묶음 종료 뒤에 떼므로 호스트·엔진 묶음 모듈은 맞는다. 서비스 등록 기간에 프로세스 묶음으로 그런 모듈을 `Load`하면 서비스가 사라진 뒤 그 모듈이 내려가므로 호스트가 하지 않는다(계약). 한계: 다른 모듈의 형이 GPU 채널을 쓰고 있어(`Busy`) RenderBase가 등록을 다 풀지 못해도 Warning만 남기고 lease를 놓는다(수명 객체가 파괴되므로 lease를 붙잡으려면 누수시켜야 하고, 그러면 로더 파괴가 Fatal이 된다). Monolithic은 코드가 내려가지 않아 영향이 없고, Modular 해제(ML4·ML6)에서 다시 본다.
- 서비스가 없거나 세션이 멈춰 있으면 등록하지 않는다(시험 실행기·도구).
- UE는 trace 채널이 정적 객체라 등록 단계가 없다. Rocks는 세션이 채널을 명시 등록하므로(TraceSystem T 결정) 모듈 수명에 묶는다.

**그대로 두는 것.**
- trace 콘솔 명령(`TraceController::RegisterConsoleCommands`): `TraceController`가 호스트 객체(세션·설정 문맥을 함께 쥔다)라 호스트가 등록한다.
- 핸들 단위 CVar lease: 실행 중 해제(ML6)에서 핸들 해제와 함께 정한다(3.10절 끝).

### 3.13 ML4a 구현 세부 — manifest 적재(개정 9)

- **API**: `ModuleLoader::TryCreateFromManifest(allocator, manifestPath, executableEntry, report)`. `manifestPath`는 절대 경로(UTF-8 — Windows 드라이브·UNC, POSIX `/`; 상대 경로는 작업 폴더에 기대므로 `Incompatible`)이고 `executableEntry`는 실행 파일 행의 진입 함수(그 실행 파일의 `RocksModuleEntry_<Name>` — 실행 파일은 export 진입점을 두지 않는다, [빌드 타깃](BuildTargets.ko.md) BS8d1)다. 실패하면 `Result` 실패(`ErrorCode`)와 함께 `report`에 코드·실패 모듈·단계·문구를 채운다(3.4절 형식). 성공하면 지금 로더와 같다(`TryCreate(allocator, table)`과 같은 표 검사·단계·묶음·동적 모듈). `report`를 쓰는 까닭: 적재 실패는 이 함수에서만 나고 호출자(Launch·시험 runner — ML4b)가 한 번 기록한다.
- **manifest 읽기**: 파일 전체를 읽어 Core JSON(엄격)으로 해석한다. 최상위 키는 정확히 `schema`·`buildId`·`engineVersion`·`toolchainFingerprint`·`modules`이고, 행 키는 정확히 `name`·`file`·`size`·`dependencies`·`loadingPhase`·`onRequest`·`executable`이다(빠진 키·모르는 키·형식이 다른 값은 오류 — UE 파서도 모르는 키를 거부한다). `schema`는 2(행 키 `executable`을 더하며 올렸다), `buildId`는 32자 소문자 16진, 행 이름은 C 식별자, `file`은 경로 구분자·`..`·드라이브가 없는 파일 이름, `size`는 0 이상의 정수, `loadingPhase`는 단계 이름, 의존은 앞 행 이름(같은 이름 중복 없음)이다. 실행 파일 행은 하나 이하다. 위반은 `Incompatible`, 단계 `"Manifest"`, 문구에 키 경로·JSON 오류 위치다. 파일이 없으면 `NotFound`다.
- **행 키 `executable`(빌드 타깃 BS8d1에 더함)**: 실행 파일 행을 표시한다. 그 행은 OS로 싣지 않고 `executableEntry`를 진입 함수로 쓴다. 파일 크기는 똑같이 대조한다(실행 파일과 manifest가 같은 빌드인지). `executableEntry`가 없는데 실행 파일 행이 있으면 `InvalidArgument`다.
- **`Prepare`(MD22, 3.5절)**: 실행 파일이 아닌 행을 의존 순서로 처리한다.
  1. 경로는 manifest 폴더 + `file`이다. 파일이 없으면 `NotFound`(단계 `"Lookup"`).
  2. 크기가 `size`와 다르면 `Incompatible`(단계 `"Lookup"`, 두 크기를 문구에).
  3. `platform::DynamicLibrary::Open`(절대 경로 — 의존 DLL은 같은 폴더에서만 찾는다. 절대 경로 `Open`은 정책 `shader-tool-binary-load`가 셰이더 도구 쪽에만 허용하므로 이 파일 `ModuleManifest.cpp`를 예외에 더했다). 실패하면 `OsLoadFailed`, `nativeError`·OS 문구(POSIX `dlerror`)를 담는다(단계 `"OsLoad"`).
  4. `RocksModuleEntry`를 그 라이브러리에서 찾는다(핸들로 — 전역 조회는 모호하다). 없으면 `EntryMissing`(단계 `"Entry"`).
  5. 불러 얻은 진입 기술의 `entryVersion`·이름이 행과 다르면 `Incompatible`(단계 `"Entry"`).
  - 중간에 실패하면 이미 연 라이브러리를 역순으로 닫고 아무것도 만들지 않는다.
- **`buildId`**: 모양만 검사한다. manifest 사이의 `buildId` 대조(엔진·게임·플러그인)는 manifest가 여럿이 되는 ML5에서 한다.
- **표 저장소**: 로더가 manifest에서 만든 행 배열·이름·의존 배열·라이브러리·lease를 소유한다(정적 수명이 아니므로 로더와 같이 산다). 행의 `entry` 함수 칸은 비우고 `Prepare`가 얻은 진입 기술을 따로 넘겨 기록에 둔다(공개 `ModuleTableRow` 형식은 바꾸지 않는다). 그래서 표를 읽는 쪽은 진입 기술을 `ModuleLoader::EntryAt(index)`로 읽는다(설정 등록 `HostSettings`가 그렇게 바뀌었다 — manifest 로더의 `Table()` 행 `entry`는 null이다).
- **report**: `requested`는 manifest 파일 이름이다(경로 전체는 문구에 — 이름 칸이 짧다). `error`는 `Incompatible`에서 할당 실패(`OutOfMemory`)·표 검사 실패(`InvalidArgument`)를 담는다.
- **라이브러리 수명**: 로더 소멸이 모든 모듈을 내리고 수명 객체를 파괴하고 `Changed` 구독을 비운 뒤(바인딩 함수표가 모듈 라이브러리 안에 있을 수 있다) 라이브러리를 적재 역순으로 닫는다(3.7절). 실행 중 해제(ML6) 전에는 그 전에 닫지 않는다.
- **시험**: manifest 해석·검사의 양성·음성(키 하나씩 빼기·더하기, 형식, 의존 순서, 파일 이름, 실행 파일 행, schema·buildId 모양)과 `Prepare`의 실패 경로(파일 없음, 크기 다름, 의존 DLL 없음, 진입점 없음 — Windows 시험 라이브러리 fixture는 코드가 없는 데이터 DLL이라 진입점을 부르는 성공 경로와, 앞 행을 연 뒤 뒤 행에서 실패해 여러 라이브러리를 역순으로 닫는 경로는 BS8d5의 Modular FoundationTests가 본다). 실패한 `Prepare`가 연 라이브러리를 닫는지(진입점 없음), 음성 사례마다 문구가 그 이유를 말하는지, 상대 경로 거부도 본다.

## 4. 결정 요약

1. 모든 C++ 모듈은 수명 객체(`ModuleImplementation`)와 진입점 매크로를 가진다. `Startup`은 `Result<void>`를 돌려준다.
2. 진입점은 전역 생성자 없이 상수 기술 + 함수다. Monolithic은 빌드 생성 모듈 표, Modular는 공유 라이브러리마다 export 진입점 하나다.
3. 로더는 Core 런타임 모듈 `ModuleLoader`이고 Launch가 소유한다. 전역 접근자는 없다.
4. 모듈은 로딩 단계에 따라 시작하고, 의존은 로더가 먼저 시작한다. 단계는 프로세스·호스트·엔진 세 묶음으로 나뉘고, 묶음마다 PreUnload 전부 → Shutdown 역순으로 내린다. 엔진 묶음은 Engine 수명에 묶인다.
5. Modular는 manifest·`ModuleBuildId`(UE처럼 재사용)·파일 크기로 바이너리를 찾고 맞는지 검사한다. 실행 파일은 부트 모듈만 import하고 본문은 호스트 모듈에 둔다. 실패는 원래 OS 오류를 담은 report다.
6. 모든 모듈의 설정 등록은 HostSettings가 설정을 확정하기 전에 한다.
7. 리플렉션 타입 등록은 레지스트리 주인이 타입 등록기로 하고, 로더가 실제 `ModuleLease`를 발급한다.
8. 실행 중 해제는 의존 모듈·타입 인스턴스가 남으면 아무것도 바꾸지 않고 거부하고, lease가 남으면 공유 라이브러리 해제를 미룬다.

## 5. 결정 사항

| ID | 결정 | 상태 |
|---|---|---|
| MD1 | 모듈 수명 객체 `ModuleImplementation`(`Startup`·`PreUnload`·`Shutdown`·`SupportsAutomaticShutdown`·`SupportsUnload`). `Startup`은 `Result<void>`이고 실패 전에 부분 등록을 스스로 되돌린다. 문맥 `ModuleStartContext`(로더·단계·lease·`HostServices`). 선택 정적 함수 `RegisterSettings`. `PostLoadCallback`·`IsGameModule`은 두지 않는다 | 권장안 |
| MD2 | 진입점 매크로 `ROCKS_IMPLEMENT_MODULE(Class, Name)`은 상수 `ModuleEntry`(`entryVersion`·이름·`create`·타입 등록·설정 등록 — 타입 해제 칸은 ML2에서 뺐다)와 `RocksModuleEntry_<Name>()`을 정의한다. Monolithic은 빌드 생성 모듈 표(의존 순서·단계 포함), Modular는 export `RocksModuleEntry()` 하나와 강제 참조 링크. 전역 생성자를 쓰지 않는다. 이름 불일치는 `static_assert`. header-only·서드파티·Core 아래 모듈은 면제, 실행 파일 모듈은 포함(개정 2) | 권장안 |
| MD3 | 로더는 새 Core 런타임 모듈 `ModuleLoader`(의존 Core·PlatformBase)다. 타입 시스템을 모른다. 진입점 계약(수명 객체 기반·진입 기술·매크로·모듈 표 형식)은 모든 모듈이 쓰도록 Core에 둔다(개정 2) | 권장안 |
| MD4 | 로더는 프로세스에 하나이고 Launch `ApplicationContext`가 소유한다. 전역 접근자는 없다. 부트 모듈(Launch 의존 폐포)은 로더 생성 때 시작하고, 단계는 `EarliestPossible`로 강제하며, 해제할 수 없다 | 권장안 |
| MD5 | 기술자 `LoadingPhase`에 `PostEngineInit`·`None`을 더하고 엔진 모듈 규칙에도 `LoadingPhase`(기본 `Default`)를 둔다. 시작 화면·로딩 화면 단계는 두지 않는다 | 권장안 |
| MD6 | 의존 모듈의 단계는 같거나 더 이르거나 `None`이어야 한다(빌드 검사). `StartPhase`는 돌지 않은 앞 단계를 차례로 돌고, 이미 돈 단계는 성공으로 넘기며, 앞 묶음 단계 호출은 계약 위반이다. 단계 안 첫 실패에서 멈춘다 | 권장안 |
| MD7 | `Load`는 manifest 의존을 먼저 의존 순서로 시작한다. 시작 중인 모듈의 재적재는 `Cycle`이다. 적재·해제는 로더 스레드에서만, 조회는 어느 스레드에서든 `Started` 모듈만. `Failed`·실행 중 해제한 모듈은 다시 적재할 수 있다 | 권장안 |
| MD8 | 적재 결과는 `ModuleLoadReport`(코드·모듈·처음 실패한 모듈·`nativeError`·단계·문구), 해제 결과는 `ModuleUnloadReport`다. 로더는 기록하지 않고 실패를 확정하는 계층이 한 번 기록한다 | 권장안 |
| MD9 | 단계는 프로세스·호스트·엔진 묶음으로 나누고, 묶음마다 UE 절차(자동 종료 대상의 PreUnload 전부 → Shutdown 역순, 타입 해제, 공유 라이브러리는 해제하지 않음)로 내린다. 엔진 묶음은 `Engine::Shutdown`에서 TypeRegistry 종료 전, 호스트 묶음은 LogHost·HostSettings 정리 전, 프로세스 묶음은 Launch 정리에서 도구 로그 정리 전. 엔진 묶음은 Engine 수명에 묶이고 동시에 여럿이면 참조 수로 센다. 종료 절차에서는 `Changed`를 내지 않는다 | 권장안 |
| MD10 | 실행 중 해제는 먼저 `SupportsUnload`·부트 모듈·의존 모듈·타입 해제 가능 여부를 검사해 `Unsupported`·`Busy`로 거부한다(바뀌는 것 없음). 내린 뒤 lease가 남으면 공유 라이브러리 해제를 미룬다. 플러그인 끄기는 전체를 먼저 검사한다 | 권장안 |
| MD11 | 모듈 규칙 `DynamicModules`(UE `DynamicallyLoadedModuleNames`): Modular에서 링크하지 않고 manifest에만 싣는다. Monolithic은 링크한다. type 포함 규칙은 적용하고 단계 순서 검사는 적용하지 않는다. 순환 검사의 참조 사슬에는 넣지 않는다(UE와 같다 — 개정 4, 3.8절). `Dependencies`와 겹치거나 자기 자신이거나 모듈 표에 행이 없는 모듈이면 오류다. `DynamicModules`로만 표에 든 행은 단계 실행 대상이 아니다(UE `None`과 같다 — 개정 4 리뷰) | 권장안 |
| MD12 | Modular manifest `<Target>[-<Platform>-<Configuration>].modules.json`(`buildId`·`modules`의 `file`·`size`·`dependencies`·`loadingPhase`), 링크 성공 뒤 원자적으로 쓴다. 엔진·게임·플러그인 `Binaries/<Platform>/`의 manifest만 읽고 폴더를 훑지 않는다. 모르는 키는 오류 | 권장안 |
| MD13 | `ModuleBuildId`는 UE처럼 출력 폴더의 이전 값을 재사용하고 엔진 버전·툴체인 fingerprint가 바뀌면 새로 만든다. manifest에만 둔다(코드에 넣지 않음). 설치 SDK manifest에 실린다. 다르면 `Incompatible`, 파일 크기가 manifest와 다르면 `Incompatible` | 권장안 |
| MD14 | 의존을 먼저 적재하므로 OS 탐색은 같은 폴더만 쓴다. Linux는 `-soname`을 넣는다. POSIX `dlerror` 문구를 report로 넘기도록 `DynamicLibrary`를 보강한다 | 권장안 |
| MD15 | 타입 등록은 레지스트리 주인이 `ModuleTypeRegistrar`(`Register`·`CanUnregister`·`Unregister`)로 한다. 동시에 사는 Engine마다 등록기를 붙일 수 있다(최대 4개 — 개정 5, 3.11절). 엔진은 모듈마다 `ModuleRegistration`을 두고, 등록기를 `Engine::Initialize`에서 설정해 이미 시작한 모듈을 몰아서 등록하며 `Engine::Shutdown`에서 뗀다. 메타 컴파일러는 모듈 자신의 타입만 등록하는 함수를 더 낸다. HostSettings 레지스트리는 지금처럼 명시 등록 | 권장안 |
| MD16 | 로더가 모듈마다 실제 `ModuleLease`(원자적 수)를 발급하고, 생성 등록 함수와 모듈 코드를 가리키는 등록(CVar·trace 채널·delegate·서브시스템 클래스)이 그것을 잡는다. lease는 공유 라이브러리 해제 시점만 정한다 | 권장안 |
| MD17 | 모듈 이름은 대소문자를 구분해 비교하고, 대소문자만 다른 이름은 빌드가 거부한다 | 권장안 |
| MD18 | 형식 있는 조회는 `Find<T>(name)`이고 UE `GetModuleChecked<T>`처럼 검사 없는 `static_cast`다. `HostServices` 형식 키는 소유 모듈 cpp에 out-of-line으로 정의해 export한다. 키 형식·매크로는 Core에 둔다(개정 6 — Core의 `LogSystem`도 서비스가 되게) | 권장안 |
| MD19 | Rocks Studio는 프로젝트 기술자·플러그인 조건으로 켤 모듈을 정하고, 빌드가 게임 manifest에 실은 플러그인 목록과 다르면 `Incompatible`이다 | 권장안 |
| MD20 | 시험·도구 실행 파일도 모듈 표를 가지고 로더로 모듈을 시작한다. 시험 `TEST` 정적 등록은 예외를 유지하고, Modular에서 등록 목록 머리는 TestSupport에 한 벌, 시험 묶음 공유 라이브러리는 해제하지 않는다 | 권장안 |
| MD21 | Modular 실행 파일은 부트 모듈만 import하고, 본문은 타깃의 호스트 모듈에 두어 로더가 적재한다. 부트 모듈 누락만 report 계약의 예외다 | 권장안 |
| MD22 | 로더는 시작 전에 manifest의 모든 모듈 바이너리를 준비(`Prepare`)한다. HostSettings는 설정 확정 전에 모든 모듈의 `registerSettings`를 의존 순서로 부른다 | 권장안 |

## 6. 단계

| 단계 | 내용 | 완료 기준 |
|---|---|---|
| ML0 | Core: 수명 객체·진입 기술·진입점 매크로(Monolithic)·모듈 표 형식(개정 2). `ModuleLoader` 모듈: `ModuleStartContext`·`HostServices`(형식 키 매크로)·빌드 생성 모듈 표·의존 순서 시작·상태·`Changed`·report 두 종·묶음 종료·시작 실패 정리. `DynamicLibrary` POSIX `dlerror` 보강. 모든 기존 C++ 모듈에 `ROCKS_IMPLEMENT_MODULE`(대부분 기본 구현). 빌드: 모듈 이름 대소문자 충돌 검사(MD17) | 의존 순서 시작·역순 종료·실패 중단과 역순 정리·`Cycle`·`WrongThread`·`ShuttingDown`·`Failed` 재시도 시험, 매크로 누락·이름 불일치 링크·컴파일 오류 시험, 대소문자 충돌 거부 시험 |
| ML1 | 로딩 단계와 묶음: 기술자 `PostEngineInit`·`None`, 엔진 모듈 규칙 `LoadingPhase`, 모듈 표 행의 요청 시 적재 표지와 `StartPhase`의 건너뜀(3.8절), 단계 순서 빌드 검사(MD6), 부트 모듈 단계 강제, `StartPhase`의 앞 단계 차례 실행, Launch 통합(crash-launch 병합 뒤), 엔진 호스트의 `PostConfigInit`·호스트 묶음 종료, `Engine::Initialize`·`Shutdown`의 엔진 묶음과 참조 수, 타입 등록기 여럿. 세부는 3.11절(개정 5) | Sandbox 엔진 호스트가 모든 단계를 지나 부트·종료한다. 한 프로세스에서 Engine을 차례로 여러 번 만드는 시험이 엔진 묶음을 매번 시작·종료한다. 단계별 시작·종료 순서 시험 |
| ML2 | 타입 등록기(MD15)·모듈별 `ModuleRegistration`·모듈 자신의 타입만 등록하는 생성 함수, 실제 `ModuleLease`(MD16)와 생성 등록 함수의 lease 인자, 설정 등록(MD22, Monolithic), 기존 모듈별 등록 래퍼를 `Startup`·`RegisterSettings`로 옮기기, CVar·trace 채널 등록의 lease. ML1 전 연결과 ML1 뒤로 넘긴 것은 3.10절(개정 3) | 호스트 코드의 모듈별 명시 등록 호출이 HostSettings 설정 타입 외에 0. 늦은 단계 모듈의 CVar가 `ReportUnresolved` 경고 없이 설정 값을 받는 시험. 종료 뒤 남은 lease 진단 시험 |
| ML3 | `DynamicModules`(MD11)와 Monolithic 링크 규칙, type 검사, 링크 의존만의 순환 거부(동적 관계는 참조 사슬 아님), 표에 행이 없는 모듈 거부 | 실행 중 적재 모듈을 `Load`·`Find<T>(name)`으로 쓰는 시험 |
| ML4 | Modular 적재 — 선행: [빌드 타깃](BuildTargets.ko.md) BS8. manifest 쓰기·읽기(원자적 쓰기, 모르는 키 거부), `ModuleBuildId` 재사용·SDK manifest, 파일 크기 대조, export 진입점과 강제 참조, `Prepare`, 실행 파일의 부트 모듈 import와 호스트 모듈(MD21), Linux `-soname`, OS 적재·report, `Modular` FoundationTests를 로더로 실행 | `Modular` FoundationTests가 Windows·macOS에서 로더로 모듈을 적재해 통과한다. `buildId` 불일치·크기 불일치·의존 DLL 누락·진입점 없음·모르는 키의 report 시험. 증분 빌드에서 `buildId`가 바뀌지 않는 시험 |
| ML5 | Rocks Studio 적재 — BS4·BS5와 함께: 프로젝트·플러그인 manifest, 플러그인 조건 해석, 빌드 목록 대조(MD19) | 게임·플러그인 모듈이 있는 프로젝트를 Rocks Studio가 열고 닫는다 |
| ML6 | 실행 중 해제(MD10)와 플러그인 끄기, 공유 라이브러리 지연 해제 | `Busy`(의존 모듈·살아 있는 인스턴스)·`Unsupported` 시험(아무것도 바뀌지 않음), 해제 뒤 다시 적재 시험(Modular), lease가 남은 경우의 지연 해제 시험 |

## 7. UE와 다르게 정한 점

| 항목 | UE 5.8 | Rocks | 이유 |
|---|---|---|---|
| 모듈 등록 | 정적 등록 객체·정적 initializer 목록(전역 생성자) | 상수 기술 + 빌드 생성 모듈 표·export 진입점 | 전역 생성자 금지 원칙. 시작 순서가 정적 초기화 순서에 기대지 않는다 |
| 시작 함수 반환 | `void` | `Result<void>` | 실패를 숨기지 않고 report로 올린다 |
| 의존 모듈 시작 | 각 모듈이 `StartupModule`에서 직접 `LoadModuleChecked` | 로더가 의존 순서로 먼저 시작 | BD30 계약. 의존 시작을 빠뜨리는 실수가 없다 |
| 시작 중 재적재 | 초기화가 덜 된 포인터를 돌려준다 | `Cycle` 오류 | 덜 만든 객체를 쓰지 않게 |
| 종료 시점 | 모든 모듈을 한 번에(`AppPreExit` 뒤) | 프로세스·호스트·엔진 묶음마다 | Rocks 호스트 서비스(엔진·로그·설정)가 따로 사라지므로 모듈이 쓰던 서비스가 살아 있을 때 내린다. 시험이 Engine을 여러 번 만든다 |
| 실행 중 해제 | 검사 없이 내린다 | 먼저 검사해 거부, lease가 남으면 라이브러리 해제를 미룸 | 매달린 포인터 방지 |
| `BuildId` 불일치 | 로그를 남기고 건너뛴 뒤 `FileNotFound` | 두 ID를 담은 `Incompatible`, 파일 크기도 대조 | 원인을 바로 알린다 |
| 설정 등록 | CVar가 정적 초기화로 먼저 생긴다 | 바이너리를 준비한 뒤 모든 모듈의 설정 등록 함수를 설정 확정 전에 부른다 | 전역 생성자 없이 같은 효과 |
| Modular 실행 파일 | 엔진 모듈을 import한다 | 부트 모듈만 import, 본문은 호스트 모듈 | 적재 실패를 report로 만들 수 있게(대화상자·`main` 전 실패 방지) |
| 모듈 이름 비교 | 대소문자 무시(`FName`) | 대소문자 구분 + 빌드가 대소문자만 다른 이름 거부 | 결과는 같고 비교가 단순하다 |
| 로더 소유 | 전역 싱글톤 | Launch가 소유, 참조 전달 | 명시적 소유 원칙 |
| 로딩 단계 | 시작·로딩 화면 단계 포함 | 화면 단계 없음, 의존 단계 순서 검사, 건너뛴 앞 단계를 차례로 실행 | 대응하는 시작 흐름이 아직 없다. 늦은 단계 모듈이 조용히 앞당겨지지 않게 |
| 모듈 탐색 | 바이너리 폴더를 재귀로 훑어 manifest를 모은다 | 정해진 폴더의 manifest만 | 결과가 폴더 상태에 조용히 기대지 않게 |
| 타입 등록 대상 | 프로세스에 UObject 레지스트리 하나 | 로더에 등록기 여럿(Engine마다 `TypeRegistry`) | 한 프로세스에 Engine이 여럿 살 수 있다(시험·도구). 엔진 묶음은 참조 수로 함께 쓴다 |
| 앞 묶음 단계 재호출 | Error 로그 | 계약 위반 Fatal | 호스트 순서 오류를 숨기지 않는다 |

## 8. 다른 문서 개정

이번 개정에서 함께 고친 문서:
- [빌드 타깃](BuildTargets.ko.md): 3.8절 BD30과 9.2절의 "별도 스펙 모듈 로더"를 이 문서로 링크했다. BD30에 단계 관계(ML0–ML4는 BS5 전, ML5는 BS5와 함께)와 실행 파일 import 범위(MD21)를 적었다. 3.8절 Linux 링크 옵션에 `-soname`을, BS8에 빌드 정보 생성 TU의 Modular 정의 위치를 더했다.

구현 때 고칠 문서:
- [크래시 처리·실행](CrashHandlingAndLaunch.ko.md): 8.1절에 모듈 단계(로더 생성·`EarliestPossible`, 도구 로그 뒤), 8.2절에 프로세스 묶음 종료(도구 로그 정리 전)를 더했다(개정 5, ML1).
- [CVar](CVarSystem.ko.md) "모듈 언로드 때 핸들 해제(결정 필요)": MD16·MD22로 답했다(등록은 `RegisterSettings`, 핸들이 모듈 lease를 잡고, 모듈 `Shutdown`이 해제한다 — 개정 5).
- [저장소 구성](../Guides/RepositoryLayout.ko.md) 게임 빌드 연동: `loadingPhase` 실행 중 반영을 이 문서로 링크했다(개정 5).
- [AppInfo·CommandLine](AppInfoCommandLine.ko.md) 11절: `BuildDescription`에 `ModuleBuildId`를 더한다(ML4).
- [엔진 런타임](EngineRuntime.ko.md) 4.1·6절: `Engine::Initialize`·`Shutdown`의 엔진 묶음 호출과 타입 등록기를 적었다(개정 5).

## 9. 범위 밖과 채택하지 않은 UE 요소

### 9.1 채택하지 않은 UE 요소

| UE 5.8 요소 | 판단 |
|---|---|
| `PostLoadCallback`, `LoadModuleWithCallback`, `UnloadOrAbandonModuleWithCallback`, `AbandonModule` | hot reload용이다(9.2절) |
| `IsGameModule`, `IMPLEMENT_GAME_MODULE`, `IMPLEMENT_PRIMARY_GAME_MODULE` | 게임 모듈 표시와 게임 이름 등록이다. Rocks는 프로젝트 이름을 기술자·빌드 정보로 안다 |
| `UE_MERGED_MODULES`(여러 모듈을 라이브러리 하나로) | 실험 기능이다. 지연 해제 방식만 3.7절에서 빌렸다 |
| `PER_MODULE_BOILERPLATE`의 `operator new` 교체 | Rocks는 전역 `operator new`를 바꾸지 않는다([빌드 타깃](BuildTargets.ko.md) BD29) |
| `-ModulesBootstrap=`(경로 캐시 직렬화) | 폴더를 훑지 않으므로 필요 없다 |
| 시작 화면·로딩 화면 단계(`PostSplashScreen`·`PreEarlyLoadingScreen`·`PreLoadingScreen`) | 대응하는 시작 흐름이 생기면 다시 본다 |
| `IsLoadedInCurrentConfiguration`(실행 중 host type 필터) | 빌드가 타깃에 들어갈 모듈을 이미 거른다([빌드 타깃](BuildTargets.ko.md) BD4·BD25). manifest에 있는 모듈은 그 타깃에서 쓸 수 있다 |
| 콘솔 명령 `Module Load/Unload/Reload` | 편집기 도구가 생기면 다시 본다 |

### 9.2 범위 밖

- 코드 다시 불러오기(UE Live Coding·hot reload): Modular 적재(ML4)가 생긴 뒤 별도 스펙.
- Rocks Studio의 다시 빌드 안내 UI: Rocks Studio 설계.
- HostSettings와 엔진 레지스트리의 통합: [설정 시스템](ConfigSystem.ko.md)·[엔진 런타임](EngineRuntime.ko.md).
- 플러그인 C 함수 표 ABI([전체 계획](EngineImplementationPlan.ko.md) 2426행 "versioned C function table"): 바이너리 배포 플러그인용이다. 이 문서는 같은 빌드 계열에서 나온 모듈만 다룬다(`ModuleBuildId` 일치).

## 10. 개정 이력

| 개정 | 날짜 | 내용 |
|---|---|---|
| 9 | 2026-11-21 | ML4a 구현 세부(3.13절): `TryCreateFromManifest`(manifest 엄격 해석·`Prepare`·report), 행 키 `executable`과 실행 파일 진입 함수, 표 저장소·라이브러리 수명, 시험 범위(성공 경로는 BS8d5) |
| 8 | 2026-11-21 | ML4를 [빌드 타깃](BuildTargets.ko.md) BS8d와 한 묶음으로 진행한다(빌드 타깃 개정 14): ML4a(manifest 읽기·`Prepare`·report) → ML4b(실행 파일 연결 — `GeneratedModuleTable()`은 Monolithic에만, 부트 모듈 import, 시험 등록 머리). manifest 쓰기·export 진입점·강제 참조는 BS8d1이 한다. ML4 완료 기준(`Modular` FoundationTests를 로더로 실행)은 BS8d5와 같이 채운다 |
| 7 | 2026-11-21 | [빌드 타깃](BuildTargets.ko.md) 3.9절(BD36·BD39)에 맞춰 3.8절 순환 항목의 "Engine이 `DynamicModules`로 둔 플러그인 모듈"을 지웠다. 엔진 계층 모듈은 플러그인 모듈을 동적 적재로도 참조하지 않고, 켠 플러그인 모듈은 주 실행 파일에 링크된다 |
| 6 | 2026-11-20 | ML1 후속(3.12절): 호스트 서비스 키·매크로를 Core `Core/Module/HostServiceKey.h`로 옮김(MD18 보강), `LogSystem`·`ConfigContext`·`TraceSession`을 서비스로 선언, Sandbox가 `PostConfigInit` 전에 등록·호스트 묶음 종료 뒤 뗌, RenderBase 모듈 `Startup`이 GPU trace 채널·형을 등록하고 `Shutdown`이 풂(첫 서비스 사용처). `Engine` 서비스·핸들 단위 CVar lease는 그대로 후속 |
| 5 | 2026-11-20 | ML1 착수. 3.11절에 구현 세부를 정했다: `LoadingPhase`에 `PostEngineInit`·`None`, 엔진 모듈 규칙 `LoadingPhase`, 모듈 표 행 `onRequest`(요청 시 적재 — 단계 실행이 건너뜀), 실행 파일 행은 `None`(UE Program 주 모듈)·시험 fixture 모듈 `None`, 빌드의 단계 순서 검사(MD6, 모듈 표 행 기준)와 부트 모듈 검사(새 표지 `ProvidesLaunch`의 링크 폐포는 `EarliestPossible`이어야 함), SDK manifest 11(`LoadingPhase`·`ProvidesLaunch`), `StartPhase`(앞 단계 차례 실행, 이미 돈 단계는 성공, 앞 묶음 단계·`None`은 Fatal, 실패는 그 단계를 멈춤)와 `ShutdownGroup`의 단계 되돌림, 엔진 묶음 참조 수(`StartEngineGroup`·`ShutdownEngineGroup`), 타입 등록기 여럿(Engine마다, 최대 4 — MD15 개정), Launch가 로더를 소유(`Launch.Modules` 단계, `ApplicationContext::Modules()`, 정리에서 프로세스 묶음 종료·lease 진단), Sandbox `PostConfigInit`·호스트 묶음 종료, Engine 대체 로더 제거(`EngineConfig::modules` 필수), 시험 runner의 `test::ProcessModules()`. 후속: 호스트 서비스 등록·서비스를 쓰는 `Startup` 등록·핸들 단위 CVar lease. 다른 문서(8절)를 함께 고쳤다 |
| 4 | 2026-11-20 | ML3 구현. UE UnrealBuildTool 근거로 MD11을 고쳤다: `DynamicModules` 관계는 순환 검사의 참조 사슬에 넣지 않는다(UE "Dynamic loads aren't considered a reference chain" — 엔진이 동적 적재하는 모듈이 엔진에 링크하는 쓰임을 막지 않게). 대신 `Dependencies`와 겹침·자기 자신·실행 파일·모르는 이름·대소문자 철자를 거부한다. 3.8절에 ML3 구현 세부를 더했다: 규칙 속성, type 검사(BD25 구현 전에는 기술자 모듈 제외 검사와 Release 배제 도달 집합), Monolithic 링크와 링크 순서, include 경로 제외, 모듈 표의 동적 모듈 행 위치(실행 파일 행 앞, 행 의존은 링크 의존만), 빌드 대상·메타데이터 도달 집합, SDK manifest 10, 묶음 기록은 ML0 그대로, Modular는 ML4. 같은 날 리뷰 반영: 3.8절 옛 글머리·6절 ML3 행을 개정 MD11과 맞춤, 요청 시 적재 행 결정(UE `LoadModulesForPhase`는 기술자 목록만 돌므로 `DynamicModules`로만 닿는 행은 단계 실행 대상이 아니다 — 표지와 건너뜀은 ML1), 표에 행이 없는 모듈(header-only·서드파티·Core 아래·표 제공 모듈에 닿지 않음)과 중복 거부 |
| 3 | 2026-11-19 | ML2 착수. ML1 전의 연결을 3.10절에 정했다: 모듈 생성 소스(리플렉션 생성 소스 5.5절)의 자기 타입 함수 `RegisterModuleOwnTypes`와 `ROCKS_MODULE_TYPES`로 진입 기술 `registerTypes`를 채움(소스 없는 메타데이터 모듈은 빌드 진입점 TU로 진입점 모듈이 됨), `unregisterTypes` 제거(`ModuleEntryVersion` 2), `ModuleTypeRegistrar`·`AttachTypeRegistrar`·`DetachTypeRegistrar`와 일반 구현 `ModuleTypeRegistrations`(ObjectRuntime), `EngineConfig::modules`와 Engine 대체 로더, `HostSettingsOptions::modules`·`ConfigContext::RetainModule`과 모듈 `RegisterSettings`, Sandbox의 프로세스 로더, `ModulesWithLeases` 진단. ML1 뒤로 넘긴 것: 서비스를 쓰는 `Startup` 등록(trace 채널·타입), 엔진 묶음 참조 수, 핸들 단위 CVar lease. 같은 날 리플렉션 생성 소스 작업(rocks-78, GS1)과 생성기·빌드 파일이 겹쳐 착수한 코드를 되돌리고, 생성 등록 함수를 그 구조(모듈 생성 소스 정의·모듈 생성 헤더는 선언만)에 맞춰 고쳐 적었다. 구현은 그 병합 뒤 재개한다. 2026-11-20 GS1 병합 뒤 재개하며 GS1 위 세부를 3.10절에 더했다: 진입점 판정이 리플렉션 생성 소스를 세고 SDK 표지 `ReflectionOnlyLibrary`를 없앰(SDK manifest 8), `ROCKS_MODULE_TYPES`를 `ReflectionSources`로 판정, 재귀 등록 함수는 그대로 두고 `RegisterModuleOwnTypes`를 모듈 생성 소스에 더함. 같은 날 구현하고 교차 검증을 반영했다: 3.4절 API 목록에 `Table`·`AcquireLease`·`ModulesWithLeases`·`AttachTypeRegistrar`·`DetachTypeRegistrar`·`TypeRegistrar`, 3.10절에 로더 조회 API, 상태 줄·3.2절 `ModuleEntryVersion` 2·MD2 진입 기술 칸(해제 제거) 정정, 이전 버전 SDK manifest는 엄격한 읽기 전에 버전부터 비교해 버전 불일치로 안내 |
| 2 | 2026-11-19 | ML0 구현 반영. 진입점 계약을 Core(`Core/Module/ModuleImplementation.h`·`ModuleEntry.h`)로 옮겼다(`ModuleLoader`는 Core에 의존하므로 Core와 다른 모든 모듈이 매크로를 쓰려면 Core에 있어야 한다 — 3.1절). Core 아래 모듈(PlatformBase·DeterministicMath) 면제와 실행 파일 모듈 포함(3.1절), 진입 기술 칸 형식·매크로 이름 인자 펼침·식별자 이름 규칙·모듈 표 TU 형식·면제 모듈을 거치는 의존 대체·기술자 단계(3.2절), `TryCreate`·`State`·`LeaseCount`·`Services`, `Changed`를 `MulticastDelegate`로, `StartPhase`는 ML1·`Unload`는 ML6에 공개, 소멸 동작, report 고정 길이 칸(3.4절), 의존 철자 대소문자 검사(3.4절 MD17), `ShutdownGroup`의 안쪽 묶음 포함과 수명 객체 파괴(3.7절), `DynamicLibraryError`(3.5절), 빌드 표지 `ProvidesModuleTable`·`ThirdParty`·`LoadingPhase`와 SDK manifest 6(3.9절)을 적었다. 같은 날 서브에이전트 교차 검증을 반영했다: 종료 중 조회를 UE에 맞춤(모듈은 자기 `Shutdown` 직전에만 `ShuttingDown`), `Load` 진행 중(`Changed` 구독자 포함) `ShutdownGroup` Fatal, `RegisterSettings` 이름 감지를 접근 권한·오버로드와 무관하게(3.1절), 자동 종료 거부 모듈의 소멸 순서를 시작 역순으로, 실패 상태·`ShutdownGroup` 스레드 위반·부트 모듈 목록·ML0 행·1절 표·3.4절 코드 블록의 모순 정정 |
| 1 | 2026-11-19 | 첫 초안. [빌드 타깃](BuildTargets.ko.md) BD30이 넘긴 모듈 로더를 UE 5.8 `IModuleInterface`·`FModuleManager`·`ELoadingPhase` 기준으로 정했다. 사용자 지시("애매하면 언리얼 방식")에 따라 UE 구조를 따르고, 전역 생성자 금지·`Result` 오류·명시 소유와 부딪히는 곳만 7절에 차이로 적었다. 서브에이전트 1개로 교차 검증해 반영했다: 종료를 프로세스·호스트·엔진 묶음별로(모듈 `Shutdown` 때 로그·설정이 살아 있게), 실행 중 해제의 검사 순서와 lease 역할 분리·지연 해제, 한 프로세스의 여러 Engine(엔진 묶음·참조 수, 앞 단계 차례 실행), 타입 등록 시점과 실패 처리, 설정 등록 시점(바이너리 준비 + `RegisterSettings`), Modular 실행 파일의 부트 모듈 import와 호스트 모듈, `ModuleBuildId` 재사용과 파일 크기 대조, `Find<T>(name)`과 서비스 형식 키, Launch 단계 위치, 모듈 자신의 타입만 등록하는 생성 함수, 부트 모듈 정의, 해제 report, `Failed` 재시도, 종료 중 이벤트 없음, lease 원자 수, Linux `-soname`, Modular 시험 등록, 인용 정정(UE 진입점 주 경로·같은 단계 재호출) |
