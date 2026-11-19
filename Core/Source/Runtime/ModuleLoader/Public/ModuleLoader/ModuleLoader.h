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

// 모듈 로더다(UE FModuleManager 대응, 3.4절). 빌드가 만든 모듈 표(GeneratedModuleTable)로 모듈을
// 의존 순서대로 시작하고, 묶음마다 시작 역순으로 내린다. 프로세스에 하나이고 Launch가
// 소유한다(MD4). 전역 접근자는 없다. 필요한 곳은 참조를 받는다.
//
// 사용 예:
//   auto created = ModuleLoader::TryCreate(allocator, GeneratedModuleTable());
//   if (!created) { return created.Error(); }
//   UniquePtr<ModuleLoader> loader = Move(created.Value());
//   ModuleLoadReport report = loader->Load("Engine");          // Engine의 의존부터 시작한다
//   if (!report.IsOk()) { ...기록하고 loader->ShutdownGroup(ModuleGroup::Process)... }
//   auto* engine = loader->Find<EngineModule>("Engine");
//   ...
//   loader->ShutdownGroup(ModuleGroup::Process);              // 시작 역순으로 내린다
//
// 스레드: Load·ShutdownGroup·Services·Changed 구독은 로더를 만든 스레드(로더 스레드)에서만 한다.
// Load는 다른 스레드에서 부르면 WrongThread report이고, ShutdownGroup은 Fatal이다.
// IsStarted·State·Find·LeaseCount는 어느 스레드에서든 부를 수 있다(상태 칸을 원자적으로 읽는다).
// 다른 스레드에서 얻은 수명 객체 포인터는 그 모듈이 내려가기 전까지만 유효하며, 내리는 시점과의
// 동기화는 호출자 책임이다.
//
// 재진입: 모듈 Startup 안에서 다른 모듈을 Load할 수 있다. Load가 진행 중일 때(모듈 Startup, 중첩
// Load, Changed 구독자 안)나 묶음 종료 콜백(PreUnload·Shutdown) 안에서 ShutdownGroup을 부르면
// Fatal이다(시작 중인 의존 사슬 아래에서 모듈을 내리지 않게).
class ModuleLoader final {
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

    // TryCreate 전용이다(UniquePtr::TryMake가 부른다). 직접 부를 수 없다.
    ModuleLoader(CreateTag, Allocator& allocator, ModuleTable table) noexcept;

    // 아직 시작해 있는 모듈을 안쪽 묶음부터 종료 절차로 내린 뒤 모든 수명 객체를 파괴한다.
    // SupportsAutomaticShutdown() == false 모듈은 Shutdown 없이 객체만 파괴한다(UE와 같다). 로더
    // 스레드에서 파괴한다. 발급한 lease가 남아 있으면 Fatal이다(lease 해제가 파괴된 기록을 쓰지
    // 않게).
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

    // group과 그보다 안쪽 묶음에서 시작한 모듈을 내린다(MD9, UE UnloadModulesAtShutdown 절차).
    // 1. SupportsAutomaticShutdown() == true인 Started 모듈을 시작 순번의 역순으로 고른다.
    // 2. 고른 모듈 전체의 PreUnload를 먼저 부른다. 이 동안 대상 모듈은 모두 Started로 보인다.
    // 3. 각 모듈의 Shutdown을 역순으로 부르고 수명 객체를 파괴한 뒤 상태를 Stopped로 바꾼다. 모듈은
    //    자기 Shutdown 직전에만 ShuttingDown이 되므로(UE bIsReady), Shutdown 동안 아직 내리지 않은
    //    의존 모듈은 Find로 보인다.
    // Changed는 내지 않는다. 진행 중의 Load는 ShuttingDown이다. Process 묶음을 내리면 그 뒤 모든
    // Load가 ShuttingDown이다(프로세스 종료). 호스트·엔진 묶음은 다시 시작할 수 있다(다음 Engine).
    // 내릴 모듈이 없으면 할 일이 없다. 로더 스레드가 아니거나, Load 진행 중이거나, 묶음 종료 콜백
    // 안에서 부르면 Fatal이다.
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
        // 살아 있는 lease 수다.
        usize leases = 0;
    };

    bool OnLoaderThread() const noexcept;
    usize IndexOf(StringView name) const noexcept;
    Result<void> Initialize(ModuleTable table) noexcept;
    bool StartRecord(usize index, ModuleLoadReport& report) noexcept;
    void ShutdownRecords(ModuleGroup group) noexcept;

    Allocator* allocator_;
    HostServices services_;
    Array<Record> records_;
    // 모든 행의 의존 색인을 이어 둔 배열이다. 행마다 [dependencyStart, dependencyStart + count)다.
    Array<usize> dependencies_;
    uint64 loaderThread_;
    ModuleGroup currentGroup_ = ModuleGroup::Process;
    // 다음 시작 순번이다. 0은 시작하지 않은 모듈의 값이다.
    uint64 nextStartOrder_ = 1;
    // 진행 중인 Load 호출 수(Startup 안의 중첩 Load 포함)다. 0이 아니면 ShutdownGroup은 Fatal이다.
    usize loadDepth_ = 0;
    bool shutdownInProgress_ = false;
    bool processShutdown_ = false;
};

} // namespace rocks
