#pragma once

#include <AssetRuntime/AssetLoader.h>
#include <Base/NumericLimits.h>
#include <Core/Function.h>
#include <Object/ObjectRegistry.h>
#include <Object/ReferenceObserver.h>
#include <Task/TaskLane.h>

// 에셋 관리자다(Docs/Design/AssetSystem.ko.md 11.1·11.4·11.7절). 에셋 객체 Registry와 로더를
// 소유하고, 요청 관심(LoadTicket)·상주 수명(AssetLease)·수명 범위(AssetScope)·미준비 대상
// 정책(RunWhenReady)을 준다. 상주 단위(Hard SCC)의 보유 수가 0이 되면 Pump가 Discard하고 로더가
// 패키지를 놓는다.
//
// 사용 예(엔진 프레임 — EngineAssets가 Pump를 부른다):
//   AssetManager assets(config);
//   REQUIRE(assets.Initialize());
//   auto ticket = assets.RequestAsset(AssetRequest{heroId});
//   auto callback = AssetReadyCallback::TryCreate(
//       allocator, [](Result<AssetLease> lease, const AssetDiagnostic&) noexcept {
//           /* 성공이면 lease를 쥐어 상주를 유지한다 */
//       });
//   (void)assets.OnReady(ticket.Value(), TaskLane::Game, Move(callback.Value()));
//   assets.Pump();
//
// 스레드: 모든 함수는 Initialize를 부른 Game 스레드 전용이다(WrongThread). LoadTicket·AssetLease·
// DeferredCommand의 소멸은 어느 스레드에서도 된다(해제 큐 — Pump가 반영한다). AssetScope의 소멸은
// Game 스레드 전용이다.
namespace rocks {

class AssetManager;
class AssetScope;

// 요청이 기다리는 준비 수준이다(비트). Cpu는 객체 공개와 PostLoad까지다. Render는 계약이다
// (UnsupportedCapability — GPU 자원 준비는 렌더 단계가 정한다).
enum class ReadyMask : uint8 {
    Cpu = 1u << 0,
    Render = 1u << 1
};

// 아직 준비되지 않은 대상에 대한 명령의 정책이다(11.7절 9).
enum class NotReadyPolicy : uint8 {
    // 상주(PostLoad까지 끝남)가 아니면 NotReady를 돌려준다(기본).
    Fail,
    // Normal로 요청하고 준비되면 실행한다.
    Defer,
    // High로 요청(진행 중이면 승격)하고 준비되면 실행한다.
    PromoteAndDefer
};

// 에셋 요청이다(11.1절). expectedType이 null이 아니면 대표 에셋이 그 타입(IsA)이어야 한다(아니면
// TypeMismatch). scope가 있으면 그 scope의 수명에 묶인다(Revoke되면 ScopeClosed).
struct AssetRequest {
    AssetId id{};
    TypeId expectedType{};
    LoadPriority priority = LoadPriority::Normal;
    uint8 required = static_cast<uint8>(ReadyMask::Cpu);
    CancelToken cancel;
    AssetScope* scope = nullptr;
};

// 상주 수명 하나다(move-only). 쥐고 있는 동안 그 에셋의 상주 단위는 Discard되지 않는다. 소멸은
// 어느 스레드에서도 되며 다음 Pump가 반영한다. scope Revoke·Shutdown으로 회수된 lease는 비어 있는
// 것처럼 행동한다(IsValid false — 소멸은 무동작).
class AssetLease {
public:
    AssetLease() noexcept = default;
    AssetLease(AssetLease&& other) noexcept;
    AssetLease& operator=(AssetLease&& other) noexcept;
    AssetLease(const AssetLease&) = delete;
    AssetLease& operator=(const AssetLease&) = delete;
    ~AssetLease();

    // 쥐고 있는 lease인지다(Game 스레드에서 읽는다 — 회수된 lease는 false).
    bool IsValid() const noexcept;
    AssetId Asset() const noexcept;
    // 대표 에셋 객체다(에셋 객체 Registry — AssetManager::With 안에서 쓴다).
    ObjectHandle Object() const noexcept;
    ObjectHandle Package() const noexcept;

private:
    friend class AssetManager;

    AssetLease(AssetManager* manager, uint32 slot, uint32 generation) noexcept
        : manager_(manager), slot_(slot), generation_(generation) {
    }

    void Drop() noexcept;

    AssetManager* manager_ = nullptr;
    uint32 slot_ = 0;
    uint32 generation_ = 0;
};

// 요청 관심 하나다(move-only, 11.1절). 쥐고 있는 동안 로드가 진행되고, 끝난 뒤에도 상주를 붙잡는다.
// 놓으면(소멸) 진행 중 요청은 취소되고 등록된 callback은 Cancelled를 한 번 받는다.
class LoadTicket {
public:
    LoadTicket() noexcept = default;
    LoadTicket(LoadTicket&& other) noexcept;
    LoadTicket& operator=(LoadTicket&& other) noexcept;
    LoadTicket(const LoadTicket&) = delete;
    LoadTicket& operator=(const LoadTicket&) = delete;
    ~LoadTicket();

    bool IsValid() const noexcept {
        return manager_ != nullptr;
    }

private:
    friend class AssetManager;

    LoadTicket(AssetManager* manager, uint32 slot, uint32 generation) noexcept
        : manager_(manager), slot_(slot), generation_(generation) {
    }

    void Drop() noexcept;

    AssetManager* manager_ = nullptr;
    uint32 slot_ = 0;
    uint32 generation_ = 0;
};

// RunWhenReady가 돌려주는 보류 명령 handle이다(move-only). 놓아도 명령은 취소되지 않는다(취소는
// AssetManager::Cancel). 소멸은 어느 스레드에서도 된다.
class DeferredCommand {
public:
    DeferredCommand() noexcept = default;
    DeferredCommand(DeferredCommand&& other) noexcept;
    DeferredCommand& operator=(DeferredCommand&& other) noexcept;
    DeferredCommand(const DeferredCommand&) = delete;
    DeferredCommand& operator=(const DeferredCommand&) = delete;
    ~DeferredCommand();

    bool IsValid() const noexcept {
        return manager_ != nullptr;
    }

private:
    friend class AssetManager;

    DeferredCommand(AssetManager* manager, uint32 slot, uint32 generation) noexcept
        : manager_(manager), slot_(slot), generation_(generation) {
    }

    void Drop() noexcept;

    AssetManager* manager_ = nullptr;
    uint32 slot_ = 0;
    uint32 generation_ = 0;
};

// 수명 범위다(Tryout·World — 11.1절, move-only). Revoke(또는 소멸) 뒤 그 scope의 새 요청은
// ScopeClosed이고, 그 scope의 티켓·lease·보류 명령은 회수된다. Game 스레드에서만 소멸한다.
class AssetScope {
public:
    AssetScope() noexcept = default;
    AssetScope(AssetScope&& other) noexcept;
    AssetScope& operator=(AssetScope&& other) noexcept;
    AssetScope(const AssetScope&) = delete;
    AssetScope& operator=(const AssetScope&) = delete;
    ~AssetScope();

    bool IsValid() const noexcept {
        return manager_ != nullptr;
    }

    // 회수한다(두 번째부터는 무동작). 오류: WrongThread, 무효 scope InvalidArgument.
    Result<void> Revoke() noexcept;

private:
    friend class AssetManager;

    AssetScope(AssetManager* manager, uint32 index, uint32 generation) noexcept
        : manager_(manager), index_(index), generation_(generation) {
    }

    // 소멸과 같다(Revoke 뒤 칸을 돌려준다).
    void Close() noexcept;

    AssetManager* manager_ = nullptr;
    uint32 index_ = 0;
    uint32 generation_ = 0;
};

// 준비 callback이다. 실패면 lease 자리에 오류가, diagnostic에 로더 진단(없으면 빈 값)이 온다.
// PrimaryAsset과 지정 bundle 구성원의 lease 묶음이다(bundle lease — 11.4절 정책, 11.7절 10). 쥐고
// 있는 동안 모두 상주한다. 소멸은 어느 스레드에서도 된다(안의 lease가 해제 큐로 간다).
class PrimaryAssetLease {
public:
    explicit PrimaryAssetLease(Allocator& allocator) noexcept : leases_(allocator) {
    }

    PrimaryAssetLease(PrimaryAssetLease&&) noexcept = default;
    PrimaryAssetLease& operator=(PrimaryAssetLease&&) noexcept = default;
    PrimaryAssetLease(const PrimaryAssetLease&) = delete;
    PrimaryAssetLease& operator=(const PrimaryAssetLease&) = delete;

    // 대표 에셋이 먼저이고 그다음 구성원이 AssetId 순이다.
    Span<const AssetLease> Leases() const noexcept {
        return leases_.View();
    }

private:
    friend class AssetManager;

    Array<AssetLease> leases_;
};

// LoadPrimaryAsset의 묶음 티켓이다(move-only). 놓으면 구성 요청이 모두 취소되고 등록된 callback은
// Cancelled를 한 번 받는다. 소멸은 어느 스레드에서도 된다.
class PrimaryAssetTicket {
public:
    PrimaryAssetTicket() noexcept = default;
    PrimaryAssetTicket(PrimaryAssetTicket&& other) noexcept;
    PrimaryAssetTicket& operator=(PrimaryAssetTicket&& other) noexcept;
    PrimaryAssetTicket(const PrimaryAssetTicket&) = delete;
    PrimaryAssetTicket& operator=(const PrimaryAssetTicket&) = delete;
    ~PrimaryAssetTicket();

    bool IsValid() const noexcept {
        return manager_ != nullptr;
    }

private:
    friend class AssetManager;

    PrimaryAssetTicket(AssetManager* manager, uint32 slot, uint32 generation) noexcept
        : manager_(manager), slot_(slot), generation_(generation) {
    }

    void Drop() noexcept;

    AssetManager* manager_ = nullptr;
    uint32 slot_ = 0;
    uint32 generation_ = 0;
};

// 매니페스트로 셈한 용량이다(EstimatePrimaryAsset — 계획 §24.2 용량 audit).
struct AssetFootprint {
    uint32 packages = 0;
    uint64 packageBytes = 0;
    uint64 bulkRawBytes = 0;
};

using AssetReadyCallback = Function<void(Result<AssetLease>, const AssetDiagnostic&)>;
// PrimaryAsset 준비 callback이다. 실패면 처음 실패(대표 먼저, 그다음 구성원 AssetId 순)의 오류와
// 진단이 온다.
using PrimaryReadyCallback = Function<void(Result<PrimaryAssetLease>, const AssetDiagnostic&)>;
using DeferredRun = Function<void(const AssetLease&)>;
using DeferredFail = Function<void(ErrorCode, const AssetDiagnostic&)>;

// 동시에 살아 있을 수 있는 handle 객체(티켓·lease·보류 명령·묶음 티켓) 수 상한이다(11.7절 6 —
// 슬롯 블록 256칸 × 블록 표 4096). 넘는 요청·lease는 CapacityExceeded다(callback·보류 명령은 그
// 오류를 받는다). 소멸한 handle의 칸은 다음 Pump가 회수한다.
inline constexpr uint32 AssetManagerMaxLiveHandles = 256u * 4096u;

struct AssetManagerConfig {
    // 기본 허용 보류 명령 수다(11.7절 9).
    static constexpr uint32 DefaultMaxDeferredCommands = 4096;

    // 로더 설정이다. registry는 비워 둔다(AssetManager가 에셋 객체 Registry를 만들어 넣는다).
    AssetLoaderConfig loader;
    // 에셋 객체 Registry의 문맥 id와 설정이다(policy·referenceObserver는 AssetManager가 정한다).
    ContextId registryContext{};
    RegistryOptions registryOptions;
    // 관찰자 키 상한이다(ReferenceObserver::DefaultMaxKeys).
    usize observerMaxKeys = ReferenceObserver::DefaultMaxKeys;
    uint32 maxDeferredCommands = DefaultMaxDeferredCommands;
};

struct AssetManagerPumpBudget {
    static constexpr uint32 UnlimitedDiscards = NumericLimits<uint32>::Max();

    AssetLoaderPumpBudget loader;
    // 한 Pump의 패키지 Discard 수 상한이다(11.7절 3).
    uint32 maxDiscards = UnlimitedDiscards;
};

struct AssetManagerPumpResult {
    AssetLoaderPumpResult loader;
    uint32 residentUnitsCreated = 0;
    uint32 discardedPackages = 0;
    uint32 callbacks = 0;
    uint32 deferredCommands = 0;
};

// 상주 단위의 상태다(11.7절 3).
enum class ResidentUnitState : uint8 {
    Resident,
    Closing,
    Discarding
};

struct AssetManagerUnitInfo {
    // 구성 에셋은 AssetManagerSnapshot::unitAssets[firstAsset, firstAsset + assetCount)다.
    uint32 firstAsset = 0;
    uint32 assetCount = 0;
    ResidentUnitState state = ResidentUnitState::Resident;
    // 보유 내역(11.7절 3): 티켓 관심·lease·보류 명령, 로더 공개 보유, 다른 단위에서 오는 간선.
    uint64 tickets = 0;
    uint64 leases = 0;
    uint64 deferred = 0;
    uint64 loaderHolds = 0;
    int64 inboundEdges = 0;
};

struct AssetManagerSnapshot {
    explicit AssetManagerSnapshot(Allocator& allocator) noexcept
        : loader(allocator), units(allocator), unitAssets(allocator) {
    }

    AssetLoaderSnapshot loader;
    Array<AssetManagerUnitInfo> units;
    Array<AssetId> unitAssets;
    uint32 liveTickets = 0;
    uint32 liveLeases = 0;
    uint32 deferredCommands = 0;
    // Discarding 단위로 생긴 새 하드 간선 수다(11.7절 3 한계 — 0이 아니면 Destroyed를 가리킨다).
    uint64 lateEdgesIntoDiscarding = 0;
};

class AssetManager final {
public:
    explicit AssetManager(const AssetManagerConfig& config) noexcept;
    // Shutdown하지 않았으면 Shutdown한다. 살아 있는 lease·티켓·보류 명령 handle 객체가 남아 있으면
    // platform::Fatal이다(11.7절 6 — 종료 순서 World → lease·scope → AssetManager 위반).
    ~AssetManager();
    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    // 에셋 객체 Registry와 로더를 만들고 초기화한다. 이 함수를 부른 스레드가 소유 스레드다.
    // 오류: 설정 오류 InvalidArgument, 로더 초기화 오류(진단 포함), 두 번째 호출 InvalidArgument.
    Result<void> Initialize(AssetDiagnostic* failure = nullptr) noexcept;

    // AssetManager가 소유한 에셋 객체 Registry다(Initialize 뒤, 소유 스레드). 객체 읽기·필드 쓰기·
    // 하드 참조 편집과 상주 패키지 안 생성 트랜잭션은 된다(간선 변화는 계수된다). 넘겨받은 패키지와
    // 그 객체의 수명 변경(Discard·RequestDestroy·패키지 RootRef 조작)은 계약 위반이다(11.7절 1).
    ObjectRegistry& Registry() noexcept;
    // AssetManager가 소유한 로더다(Initialize 뒤, 소유 스레드). 조회(FindPackage·FindByPath·Status·
    // Describe·PublishedHolds)만 한다. 요청·해제·우선순위·Pump·WaitFor·Shutdown·넘겨받기 API를
    // 직접 부르는 것은 계약 위반이다 — 티켓·상주 단위 계수를 우회한다(11.7절 1).
    AssetLoader& Loader() noexcept;

    // 수명 범위를 만든다. 오류: WrongThread, Shutdown 뒤 ShuttingDown, 할당 실패.
    Result<AssetScope> CreateScope() noexcept;

    // 요청을 내고 티켓을 준다(11.7절 5). 대상이 이미 상주면 그 자리에서 보유를 1 올린다(revive).
    // 오류: WrongThread, Shutdown 뒤 ShuttingDown, Revoke된 scope ScopeClosed, required의 Render
    // UnsupportedCapability, 잘못된 priority·required InvalidArgument, 로더 Request
    // 오류(매니페스트에 없는 id NotFound 등 — 진단 포함), 슬롯 상한 CapacityExceeded, 할당 실패.
    Result<LoadTicket> RequestAsset(const AssetRequest& request,
                                    AssetDiagnostic* failure = nullptr) noexcept;

    // 티켓의 준비 callback을 등록한다(11.7절 5). 끝나면 다음 Pump에서 정확히 한 번 부른다. lane은
    // Task 모듈의 TaskLane이고 이번 엔진은 Game만 된다(Game 스레드 하나 — 11.1절).
    // 오류: WrongThread, 무효 티켓 StaleHandle, 이미 등록 AlreadyRegistered(callback은 그대로
    // caller에 남는다), lane이 Game이 아니면 Unsupported, 빈 callback InvalidArgument.
    Result<void> OnReady(LoadTicket& ticket, TaskLane lane, AssetReadyCallback&& callback) noexcept;

    // 결과가 나올 때까지 동기로 기다려 lease를 준다(도구·시험·초기화 전용 — 11.7절 8).
    // 오류: 프레임 실행 중·callback 안 WrongContext, RequestAsset의 오류, 로드 실패(오류·진단).
    Result<AssetLease> LoadBlocking(const AssetRequest& request, const char* site,
                                    AssetDiagnostic* failure = nullptr) noexcept;

    // 대상이 준비되면 run을, 실패하면 fail을 Pump에서 정확히 한 번 부른다(11.7절 9).
    // 오류: Fail 정책이고 상주가 아니면 NotReady(run·fail 모두 부르지 않는다), Revoke된 scope
    // ScopeClosed, 보류 큐 상한 CapacityExceeded, RequestAsset의 오류, 빈 함수 InvalidArgument.
    Result<DeferredCommand> RunWhenReady(AssetId asset, NotReadyPolicy policy, AssetScope* scope,
                                         DeferredRun&& run, DeferredFail&& fail) noexcept;

    // PrimaryAsset과 지정 bundle 구성원을 요청한다(11.7절 10, UE UAssetManager::LoadPrimaryAsset).
    // bundle 이름은 caseless로 찾고 bundle 사이 구성원 중복은 한 번만 요청한다. 오류:
    // PrimaryAsset이 아닌 항목 InvalidArgument, 없는 bundle 이름 NotFound(진단 asset = 대표),
    // 매니페스트에 없는 id NotFound, Revoke된 scope ScopeClosed, 그 밖은 RequestAsset과 같다.
    Result<PrimaryAssetTicket> LoadPrimaryAsset(AssetId asset, Span<const StringView> bundles,
                                                LoadPriority priority = LoadPriority::Normal,
                                                AssetScope* scope = nullptr,
                                                AssetDiagnostic* failure = nullptr) noexcept;

    // 묶음 티켓의 준비 callback이다(구성 요청이 모두 끝나면 다음 Pump에서 한 번). 오류는 티켓
    // OnReady와 같다.
    Result<void> OnReady(PrimaryAssetTicket& ticket, TaskLane lane,
                         PrimaryReadyCallback&& callback) noexcept;

    // 대표·지정 bundle 구성원의 Hard 전이 폐포에 대한 매니페스트 packageBytes·bulkRawBytes 합이다
    // (로드하지 않는다). 오류는 LoadPrimaryAsset의 인자 오류와 같고, 할당 실패는 그 오류다.
    Result<AssetFootprint> EstimatePrimaryAsset(AssetId asset,
                                                Span<const StringView> bundles) const noexcept;

    // 보류 명령을 취소한다(fail에 Cancelled를 다음 Pump에서 한 번). 이미 끝났으면 무동작.
    // 오류: WrongThread, 무효 handle StaleHandle.
    Result<void> Cancel(DeferredCommand& command) noexcept;

    // lease의 에셋 문맥 접근 범위를 열고 fn(대표 에셋)을 부른다. 오류: WrongThread, 회수·무효 lease
    // StaleHandle.
    template <class Fn>
    Result<void> With(const AssetLease& lease, Fn&& fn) noexcept {
        auto opened = BeginWith(lease);
        if (!opened) {
            return Result<void>::Failure(opened.Error());
        }

        ObjectAccessScope scope(opened.Value());
        fn(lease.Object());
        return Result<void>::Success();
    }

    // 11.7절 4의 순서로 한 단계 진행한다. 오류: WrongThread, callback 안 WrongContext, Shutdown 뒤
    // ShuttingDown, 초기화 전 InvalidArgument.
    Result<AssetManagerPumpResult> Pump(const AssetManagerPumpBudget& budget = {}) noexcept;

    // 상태 표를 채운다. 오류: WrongThread, 할당 실패.
    Result<void> Describe(AssetManagerSnapshot& out) const noexcept;

    // 11.7절 12. 두 번째 호출은 무동작. 오류: WrongThread, callback 안 WrongContext.
    Result<void> Shutdown() noexcept;

    struct Impl;

private:
    friend class AssetLease;
    friend class LoadTicket;
    friend class PrimaryAssetTicket;
    friend class DeferredCommand;
    friend class AssetScope;

    Result<ObjectAccessContext> BeginWith(const AssetLease& lease) const noexcept;

    AssetManagerConfig config_;
    Impl* impl_ = nullptr;
};

} // namespace rocks
