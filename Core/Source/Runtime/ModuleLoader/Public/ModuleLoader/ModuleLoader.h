#pragma once

#include <Base/Types.h>
#include <Core/Delegate.h>
#include <Core/Memory/Allocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Module/ModuleEntry.h>
#include <Core/Optional.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <ModuleLoader/HostServices.h>
#include <ModuleLoader/ModuleReport.h>
#include <ModuleLoader/ModuleTypeRegistrar.h>

namespace rocks {

namespace detail {

struct ModuleLoaderTestAccess;

} // namespace detail

// 모듈 하나의 상태다(Docs/Design/ModuleLoader.ko.md 3.4절). BinaryLoaded(Modular)는 ML4에 더한다.
enum class ModuleState : uint8 {
    // 시작한 적이 없다.
    NotLoaded,
    // 의존 시작 또는 Startup이 진행 중이다.
    Starting,
    // Startup이 성공했다.
    Started,
    // 묶음 종료 절차가 이 모듈을 내리는 중이다(PreUnload 뒤 Shutdown 전후).
    ShuttingDown,
    // 내렸다. 다시 Load할 수 있다(새 수명 객체를 만든다).
    Stopped,
    // 마지막 시작이 실패했다(수명 객체 생성 또는 Startup). 다시 Load하면 처음부터 시도한다.
    Failed
};

// 동시에 붙일 수 있는 타입 등록기의 최대 수다(동시에 사는 Engine마다 하나 —
// Docs/Design/ModuleLoader.ko.md 3.11절).
inline constexpr usize MaxTypeRegistrars = 4;

// 모듈의 종료 시점을 정하는 묶음이다(3.3절). 안쪽 묶음일수록 값이 크다.
enum class ModuleGroup : uint8 {
    // 부트 모듈·EarliestPossible: 로더와 같이 산다.
    Process,
    // PostConfigInit: HostSettings·LogHost와 같이 산다.
    Host,
    // PreDefault–PostEngineInit: Engine 수명에 묶인다.
    Engine
};

// Changed 이벤트의 종류다. Unloaded는 실행 중 해제(ML6)가 낸다.
enum class ModuleChange : uint8 {
    Started,
    Unloaded
};

// 진단용 열거자 이름이다. 모르는 값은 "Unknown"이다. 반환 문자열은 정적 수명이다.
const char* ModuleStateName(ModuleState state) noexcept;
const char* ModuleGroupName(ModuleGroup group) noexcept;

// 단계가 속한 묶음이다(3.3절 표). EarliestPossible은 Process, PostConfigInit은 Host, PreDefault부터
// PostEngineInit까지는 Engine이다. None은 묶음이 없으므로 쓰지 않는다(Process를 돌려준다).
constexpr ModuleGroup LoadingPhaseGroup(LoadingPhase phase) noexcept {
    switch (phase) {
    case LoadingPhase::EarliestPossible:
    case LoadingPhase::None:
        return ModuleGroup::Process;
    case LoadingPhase::PostConfigInit:
        return ModuleGroup::Host;
    case LoadingPhase::PreDefault:
    case LoadingPhase::Default:
    case LoadingPhase::PostDefault:
    case LoadingPhase::PostEngineInit:
        return ModuleGroup::Engine;
    }

    return ModuleGroup::Process;
}

// 모듈 로더다(UE FModuleManager 대응, 3.4절). 빌드가 만든 모듈 표(Monolithic GeneratedModuleTable,
// Modular 모듈 manifest — TryCreateFromManifest)로 모듈을
// 단계(StartPhase)나 이름(Load)으로 의존 순서대로 시작하고, 묶음마다 시작 역순으로 내린다.
// 프로세스에 하나이고 Launch가 소유한다(MD4 — launch::ApplicationContext::Modules()). 전역 접근자는
// 없다. 필요한 곳은 참조를 받는다.
//
// 사용 예(Launch·엔진 호스트·Engine이 나눠 하는 일, 3.11절):
//   auto created = ModuleLoader::TryCreate(allocator, GeneratedModuleTable());
//   UniquePtr<ModuleLoader> loader = Move(created.Value());
//   ModuleLoadReport boot = loader->StartPhase(LoadingPhase::EarliestPossible); // Launch
//   ...HostSettings·LogHost...
//   ModuleLoadReport host = loader->StartPhase(LoadingPhase::PostConfigInit);   // 엔진 호스트
//   ModuleLoadReport engine = loader->StartEngineGroup(); //
//   Engine::Initialize(PreDefault–PostDefault) if (!engine.IsOk()) { ...기록하고 시작 중단... }
//   loader->Load("Probe");                                // 요청 시 적재 모듈
//   auto* probe = loader->Find<ProbeModule>("Probe");
//   loader->ShutdownEngineGroup();                        // Engine::Shutdown
//   loader->ShutdownGroup(ModuleGroup::Host);             // 엔진 호스트
//   loader->ShutdownGroup(ModuleGroup::Process);          // Launch 정리
//
// 스레드: Load·StartPhase·엔진 묶음·ShutdownGroup·Services·Changed 구독은 로더를 만든 스레드(로더
// 스레드)에서만 한다. Load·StartPhase·StartEngineGroup은 다른 스레드에서 부르면 WrongThread
// report이고, ShutdownGroup·ShutdownEngineGroup은 Fatal이다.
// IsStarted·State·Find·LeaseCount는 어느 스레드에서든 부를 수 있다(상태 칸을 원자적으로 읽는다).
// 다른 스레드에서 얻은 수명 객체 포인터는 그 모듈이 내려가기 전까지만 유효하며, 내리는 시점과의
// 동기화는 호출자 책임이다.
//
// 재진입: 모듈 Startup 안에서 다른 모듈을 Load할 수 있다. Load가 진행 중일 때(모듈 Startup, 중첩
// Load, Changed 구독자 안)나 묶음 종료 콜백(PreUnload·Shutdown) 안에서 ShutdownGroup을 부르면
// Fatal이다(시작 중인 의존 사슬 아래에서 모듈을 내리지 않게).
namespace detail {

struct ModuleManifestData;

} // namespace detail

namespace platform {

class DynamicLibraryLease;

} // namespace platform

class ROCKS_MODULELOADER_API ModuleLoader final {
    struct CreateTag {
        explicit CreateTag() = default;
    };

public:
    // table로 로더를 만든다. table의 행 배열과 진입 기술은 로더보다 오래 산다(생성 TU의 정적 상수).
    // allocator는 모듈 기록·수명 객체·서비스 표·이벤트 구독 저장소에 쓰며 로더보다 오래 산다. 만든
    // 스레드가 로더 스레드가 된다. 현재 묶음은 프로세스 묶음이다.
    // 실패:
    // - InvalidArgument: 표가 비었거나, 이름이 빈 값·식별자가 아니거나 겹치거나, 진입 함수가
    //   null이거나, 의존이 표에 없거나 앞 행이 아니거나, 진입 기술의 entryVersion이
    //   ModuleEntryVersion이 아니거나 이름이 행 이름과 다르거나 create가 null이다.
    // - 할당 실패는 allocator의 오류다.
    static Result<UniquePtr<ModuleLoader>> TryCreate(Allocator& allocator,
                                                     ModuleTable table) noexcept;

    // Modular 실행 파일의 로더를 모듈 manifest로 만든다(Docs/Design/ModuleLoader.ko.md 3.5·3.13·
    // 3.14절). manifestPath(절대 경로, UTF-8)의 manifest를 엄격하게 해석하고, 모든 행(호스트 모듈
    // 포함)의 공유 라이브러리를 의존 순서로 적재해 export 진입점 RocksModuleEntry로 진입 기술을
    // 얻은 뒤(Prepare) TryCreate와 같은 로더를 만든다. 로더가 manifest 표와 라이브러리를 소유하며,
    // 로더 소멸이 모든 모듈을 내린 뒤 라이브러리를 적재 역순으로 닫는다. Launch의 Modular 진입이
    // 부른다. 실패하면 report에 코드·실패 모듈·단계·문구를 채우고(호출자가 한 번 기록한다) 이미 연
    // 라이브러리를 닫는다:
    // - NotFound(manifest·모듈 파일이 없다) → ErrorCode::NotFound.
    // - Incompatible(manifest 경로가 절대 경로가 아님·읽기 실패·형식·키, 파일 크기, 진입 기술 대조)
    //   → InvalidArgument(할당 실패는 OutOfMemory).
    // - OsLoadFailed(OS 적재 실패 — nativeError·OS 문구) → DependencyFailed.
    // - EntryMissing(RocksModuleEntry가 없다) → NotFound.
    //
    // 사용 예(Launch의 Modular 진입):
    //   ModuleLoadReport report;
    //   auto created = ModuleLoader::TryCreateFromManifest(allocator, manifestPath, report);
    //   if (!created) {
    //       Log::Error(LogLaunch, "Module {Module} failed at {Stage}: {Message}", report.failed,
    //                  report.stage, report.message);
    //   }
    static Result<UniquePtr<ModuleLoader>> TryCreateFromManifest(Allocator& allocator,
                                                                 StringView manifestPath,
                                                                 ModuleLoadReport& report) noexcept;

    // TryCreate 전용이다(UniquePtr::TryMake가 부른다). 직접 부를 수 없다.
    ModuleLoader(CreateTag, Allocator& allocator, ModuleTable table) noexcept;

    // 아직 시작해 있는 모듈을 안쪽 묶음부터 종료 절차로 내린 뒤 모든 수명 객체를 파괴한다. 엔진
    // 묶음 사용자(StartEngineGroup)가 남아 있으면 Fatal이다.
    // SupportsAutomaticShutdown() == false 모듈은 Shutdown 없이 객체만 파괴한다(UE와 같다). 로더
    // 스레드에서 파괴한다. 발급한 lease가 남아 있으면 Fatal이다(lease 해제가 파괴된 기록을 쓰지
    // 않게). 호스트는 파괴 전에 ModulesWithLeases로 남은 lease를 진단해 기록한다. 타입 등록기가
    // 붙어 있으면 그 등록을 되돌리고 모두 뗀다(등록기는 그때까지 살아 있어야 한다).
    ~ModuleLoader() noexcept;

    ModuleLoader(const ModuleLoader&) = delete;
    ModuleLoader& operator=(const ModuleLoader&) = delete;

    // name 모듈을 시작한다. 표의 의존을 먼저 의존 순서로 시작하고(MD7), 이미 시작해 있으면 할 일
    // 없이 성공이다. Failed·Stopped 모듈은 처음부터 다시 시도한다. 실패 report(3.4절 코드표):
    // NotFound(표에 없음), StartupFailed(이 모듈의 생성·Startup 실패, error에 모듈 오류),
    // DependencyFailed(의존 실패, failed에 처음 실패한 모듈), Cycle(시작 중인 모듈 재적재),
    // WrongThread, ShuttingDown. 실패해도 이미 시작한 의존 모듈은 내리지 않는다 — 호출자가 시작을
    // 중단하고 ShutdownGroup으로 정리한다(3.3절).
    // 성공한 모듈마다 Changed(name, Started)를 시작 순서대로 낸다.
    ModuleLoadReport Load(StringView name) noexcept;

    // 아직 돌지 않은 단계부터 phase까지 차례로 돈다(3.3·3.11절, MD6). 단계마다 현재 묶음을 그
    // 단계의 묶음(LoadingPhaseGroup)으로 바꾸고, 표 순서(의존 순서)로 그 단계 값을 가진 행
    // 가운데 요청 시 적재 행(onRequest)이 아닌 행을 Load처럼 시작한다. 이미 시작한 모듈은 할 일이
    // 없다.
    // - 이미 돈 단계를 다시 부르면 할 일 없이 성공이다(UE와 같다).
    // - 실패하면 그 단계를 멈추고 report를 돌려준다(requested는 실패한 단계 이름 — 부른 단계보다
    // 앞일
    //   수 있다, 나머지는 Load와 같다).
    //   그 단계는 돈 것으로 치지 않는다. 이미 시작한 모듈은 내리지 않는다 — 호출자가 그 묶음을
    //   ShutdownGroup으로 내린다.
    // - 로더 스레드가 아니면 WrongThread, 종료 중이면 ShuttingDown report다.
    // - 계약 위반은 Fatal이다: phase가 None이거나, 지금 묶음보다 앞 묶음의 단계를 부르거나(UE는
    //   Error 로그), Load 진행 중이거나 묶음 종료 콜백 안에서 부른다.
    ModuleLoadReport StartPhase(LoadingPhase phase) noexcept;

    // phase를 다 돌았는지다(ShutdownGroup이 그 묶음을 내리면 다시 false다). None은 false다.
    bool PhaseStarted(LoadingPhase phase) const noexcept;

    // 지금 묶음이다. 마지막으로 돈(또는 도는 중인) 단계의 묶음이고, Load로 시작하는 모듈이 여기에
    // 속한다(3.3절). 묶음을 내리면 바깥 묶음으로 돌아간다.
    ModuleGroup CurrentGroup() const noexcept {
        return currentGroup_;
    }

    // 엔진 묶음 사용자를 하나 더한다(MD9 참조 수 — 동시에 여러 Engine). 처음 사용자면
    // StartPhase(PostDefault)로 엔진 앞 단계(PreDefault·Default·PostDefault)를 돈다. 그것이
    // 실패하면 엔진 묶음을 스스로 내리고 사용자 수를 되돌린 뒤 실패 report를 돌려준다. 성공하면
    // 반드시 ShutdownEngineGroup으로 짝을 맞춘다. PostEngineInit은 각 Engine이 StartPhase로 부른다.
    // 로더 스레드가 아니면 WrongThread report다(사용자 수는 그대로). StartPhase와 같은 계약 위반은
    // Fatal이다.
    ModuleLoadReport StartEngineGroup() noexcept;

    // 엔진 묶음 사용자를 하나 뺀다. 마지막 사용자면 엔진 묶음을 내린다(ShutdownGroup(Engine)과 같은
    // 절차). 사용자가 없거나, 로더 스레드가 아니거나, ShutdownGroup과 같은 계약 위반이면 Fatal이다.
    void ShutdownEngineGroup() noexcept;

    // 지금 엔진 묶음 사용자 수다.
    usize EngineGroupUsers() const noexcept {
        return engineUsers_;
    }

    // group과 그보다 안쪽 묶음에서 시작한 모듈을 내린다(MD9, UE UnloadModulesAtShutdown 절차).
    // 1. SupportsAutomaticShutdown() == true인 Started 모듈을 시작 순번의 역순으로 고른다.
    // 2. 고른 모듈 전체의 PreUnload를 먼저 부른다. 이 동안 대상 모듈은 모두 Started로 보인다.
    // 3. 각 모듈의 Shutdown을 역순으로 부르고 수명 객체를 파괴한 뒤 상태를 Stopped로 바꾼다. 모듈은
    //    자기 Shutdown 직전에만 ShuttingDown이 되므로(UE bIsReady), Shutdown 동안 아직 내리지 않은
    //    의존 모듈은 Find로 보인다.
    // Changed는 내지 않는다. 진행 중의 Load는 ShuttingDown이다. Process 묶음을 내리면 그 뒤 모든
    // Load가 ShuttingDown이다(프로세스 종료). 호스트·엔진 묶음은 다시 시작할 수 있다(다음 Engine).
    // 내린 묶음과 안쪽 묶음의 단계는 다시 돌 수 있게 되돌리고 지금 묶음은 바깥 묶음이 된다(엔진
    // 묶음을 내리면 다음 Engine이 엔진 단계를 다시 돈다). 내릴 모듈이 없으면 할 일이 없다. 로더
    // 스레드가 아니거나, Load 진행 중이거나, 묶음 종료 콜백 안에서 부르거나, 엔진 묶음
    // 사용자(StartEngineGroup)가 남아 있으면 Fatal이다.
    void ShutdownGroup(ModuleGroup group) noexcept;

    // name 모듈이 Started인지다. 표에 없으면 false다.
    bool IsStarted(StringView name) const noexcept;

    // name 모듈의 상태다. 표에 없으면 빈 값이다. 다른 스레드에서는 읽는 순간의 값이다.
    Optional<ModuleState> State(StringView name) const noexcept;

    // name 모듈의 수명 객체다. Started 모듈만 돌려주고, 로더 스레드에서는 Startup 중(Starting)인
    // 모듈도 돌려준다(UE GetModule 규칙). 묶음 종료 중에는 자기 Shutdown이 시작된 모듈부터
    // null이다. 그 밖과 표에 없는 이름은 null이다.
    ModuleImplementation* Find(StringView name) const noexcept;

    // Find의 형식 있는 판이다. 검사 없이 static_cast한다(UE GetModuleChecked<T>). T는 그 모듈의
    // 수명 객체 클래스나 그 기반이어야 한다(계약).
    template <class T>
    T* Find(StringView name) const noexcept {
        return static_cast<T*>(Find(name));
    }

    // name 모듈에 발급한 lease 중 살아 있는 수다(3.6절 MD16, 진단용). 표에 없으면 0이다.
    usize LeaseCount(StringView name) const noexcept;

    // 표의 모듈 수다.
    usize ModuleCount() const noexcept;

    // manifest 로더의 호스트 모듈(manifest 행 host — 실행 파일 모듈) 공유 라이브러리에서 C 기호
    // symbol을 찾는다(3.14절 — Launch가 앱 기술 RocksApplicationDescription을 찾는다). 찾으면
    // true와 주소를 돌려주고, 주소는 lease가 살아 있는 동안 유효하다. 호출자는 로더를 파괴하기 전에
    // lease를 놓는다(남으면 라이브러리를 닫지 못한다). 정적 표 로더이거나 호스트 행이 없거나 기호가
    // 없으면 false다.
    bool FindHostSymbol(const char* symbol, void*& address,
                        platform::DynamicLibraryLease& lease) const noexcept;

    // 표 index번째 행의 진입 기술이다(정적 표는 행의 entry가 돌려준 것, manifest 로더는 Prepare가
    // 얻은 것). index가 ModuleCount() 이상이면 Fatal이다.
    const ModuleEntry& EntryAt(usize index) const noexcept;

    // 생성 때 받은 모듈 표다(의존 순서). 설정 등록(HostSettings, 3.10절)이 모든 모듈의
    // registerSettings를 이 순서로 부른다. manifest로 만든 로더(TryCreateFromManifest)의 행은 entry
    // 칸이 null이다 — 진입 기술은 EntryAt으로 읽는다.
    ModuleTable Table() const noexcept {
        return table_;
    }

    // name 모듈의 코드 수명 토큰을 하나 더 발급한다(3.6절 MD16). 모듈 코드·데이터를 가리키는 등록을
    // 잡는 쪽(설정 문맥 등)이 쓴다. 어느 스레드에서든 부를 수 있다. 표에 없으면 NotFound다. 로더가
    // 파괴되기 전에 놓아야 한다.
    Result<ModuleLease> AcquireLease(StringView name) noexcept;

    // lease가 남은 모듈의 이름을 names에 앞에서부터 담고(넘치면 버린다) 그런 모듈의 수를 돌려준다
    // (3.10절 남은 lease 진단). 종료 뒤 0이 아니면 모듈 코드를 가리키는 등록이 남았다는 뜻이다.
    // 이름은 정적 수명이다(모듈 표).
    usize ModulesWithLeases(Span<StringView> names) const noexcept;

    // 타입 등록기를 붙이고 이미 시작한 모듈의 타입을 시작 순서로 그 등록기에 몰아서
    // 등록한다(3.6·3.10·3.11절 MD15). 등록기는 MaxTypeRegistrars개까지 함께 붙는다(동시에 사는
    // Engine마다 자기 TypeRegistry). 붙은 뒤 시작하는 모듈은 Startup 직전에 붙은 모든 등록기에
    // 등록하고(하나라도 실패하면 이 모듈이 이미 등록한 등록기를 되돌리고 StartupFailed, 단계
    // "RegisterTypes"), Startup이 실패하면 되돌린다. 묶음 종료는 각 모듈의 Shutdown 뒤 모든
    // 등록기에서 그 모듈 타입을 되돌린다. 몰아서 등록하다 실패하면 이번에 등록한 것을 시작 역순으로
    // 되돌리고 붙이지 않은 채 report(StartupFailed, failed에 그 모듈, error에 등록기 오류)를
    // 돌려준다. 로더 스레드가 아니면 WrongThread report다. 같은 등록기가 이미 붙어 있거나, 자리가
    // 없거나, Load 진행 중이거나, 묶음 종료 콜백 안에서 부르면 Fatal이다. registrar는 뗄 때까지
    // 살아 있어야 한다.
    ModuleLoadReport AttachTypeRegistrar(ModuleTypeRegistrar& registrar) noexcept;

    // registrar에 남은 타입 등록을 시작 역순으로 되돌리고 그 등록기를 뗀다. 붙어 있지 않으면 할
    // 일이 없다. 로더 스레드가 아니거나, Load 진행 중이거나, 묶음 종료 콜백 안에서 부르면
    // Fatal이다.
    void DetachTypeRegistrar(ModuleTypeRegistrar& registrar) noexcept;

    // 지금 붙어 있는 타입 등록기 수다.
    usize TypeRegistrarCount() const noexcept;

    // 호스트 서비스 표다(3.1절). 로더 스레드에서만 쓴다.
    HostServices& Services() noexcept {
        return services_;
    }

    // 모듈이 시작을 마칠 때(Started) 로더 스레드에서 알린다(UE OnModulesChanged). 묶음 종료 절차로
    // 내릴 때는 알리지 않는다. 구독·해제는 로더 스레드에서 Core Delegate 규칙대로 한다.
    MulticastDelegate<void(StringView, ModuleChange)> Changed;

private:
    friend struct detail::ModuleLoaderTestAccess;

    // 모듈 하나의 기록이다(UE FModuleInfo 대응). 생성 뒤 배열이 자라지 않으므로 주소가
    // 고정된다(lease 계수 주소를 콜백 문맥으로 쓴다). state·object·leases는 다른 스레드가
    // 원자적으로 읽는다.
    struct Record {
        const ModuleTableRow* row = nullptr;
        const ModuleEntry* entry = nullptr;
        // 의존 색인 범위(dependencies_ 안)다.
        usize dependencyStart = 0;
        usize dependencyCount = 0;
        // 수명 객체의 소유권이다. 로더 스레드만 바꾼다.
        UniquePtr<ModuleImplementation> owned;
        // owned의 주소를 다른 스레드에 발행한 값이다(state보다 먼저 쓴다).
        ModuleImplementation* object = nullptr;
        ModuleState state = ModuleState::NotLoaded;
        // Startup에 들어갈 때의 현재 묶음이다.
        ModuleGroup group = ModuleGroup::Process;
        // Startup이 성공한 순번이다. 0이면 시작해 있지 않다.
        uint64 startOrder = 0;
        // 진행 중인 묶음 종료가 이 모듈의 PreUnload를 불렀고 Shutdown이 남았다.
        bool pendingShutdown = false;
        // 이 모듈의 타입을 등록한 등록기 자리의 비트다(registrars_ 색인).
        uint32 registeredSlots = 0;
        // 살아 있는 lease 수다.
        usize leases = 0;
    };

    bool OnLoaderThread() const noexcept;
    usize IndexOf(StringView name) const noexcept;
    // resolved가 비어 있지 않으면 행의 entry 칸 대신 그 진입 기술을 쓴다(manifest 표 — 같은 색인).
    Result<void> Initialize(ModuleTable table,
                            Span<const ModuleEntry* const> resolved = {}) noexcept;
    bool StartRecord(usize index, ModuleLoadReport& report) noexcept;
    void ShutdownRecords(ModuleGroup group) noexcept;
    bool RunPhase(LoadingPhase phase, ModuleLoadReport& report) noexcept;
    bool RejectStart(ModuleLoadReport& report, StringView requested, bool phase) const noexcept;
    void CheckShutdownCall(bool engineGroup) const noexcept;
    bool RegisterRecordTypes(usize index, usize slot, ModuleLoadReport& report) noexcept;
    bool RegisterRecordTypesAll(usize index, ModuleLoadReport& report) noexcept;
    void UnregisterRecordTypes(usize index, usize slot) noexcept;
    void UnregisterRecordTypesAll(usize index) noexcept;
    void UnregisterSlotTypes(usize slot) noexcept;
    usize SlotOf(const ModuleTypeRegistrar& registrar) const noexcept;
    void CheckRegistrarCall() const noexcept;

    Allocator* allocator_;
    // manifest로 만든 로더의 표·라이브러리 저장소다(TryCreateFromManifest). 정적 표 로더는
    // null이다. 소멸자가 모듈을 모두 내린 뒤에 놓는다.
    UniquePtr<detail::ModuleManifestData> manifest_;
    HostServices services_;
    ModuleTable table_;
    // 붙은 타입 등록기다. 빈 자리는 null이다.
    ModuleTypeRegistrar* registrars_[MaxTypeRegistrars] = {};
    Array<Record> records_;
    // 모든 행의 의존 색인을 이어 둔 배열이다. 행마다 [dependencyStart, dependencyStart + count)다.
    Array<usize> dependencies_;
    uint64 loaderThread_;
    ModuleGroup currentGroup_ = ModuleGroup::Process;
    // 아직 다 돌지 않은 첫 단계다(LoadingPhase 값 순서). None이면 모든 단계를 돌았다.
    LoadingPhase nextPhase_ = LoadingPhase::EarliestPossible;
    // 엔진 묶음 사용자 수다(StartEngineGroup·ShutdownEngineGroup).
    usize engineUsers_ = 0;
    // 다음 시작 순번이다. 0은 시작하지 않은 모듈의 값이다.
    uint64 nextStartOrder_ = 1;
    // 진행 중인 Load 호출 수(Startup 안의 중첩 Load 포함)다. 0이 아니면 ShutdownGroup은 Fatal이다.
    usize loadDepth_ = 0;
    bool shutdownInProgress_ = false;
    bool processShutdown_ = false;
};

} // namespace rocks
