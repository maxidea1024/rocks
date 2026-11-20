#pragma once

#include <AssetRuntime/AssetManager.h>
#include <AssetRuntime/Package.h>
#include <Core/Containers/HashMap.h>
#include <Core/Memory/UniquePtr.h>
#include <Platform/Atomics.h>
#include <Platform/Sync.h>

// AssetManager의 내부 상태다(Docs/Design/AssetSystem.ko.md 11.7절). 정의는 기능별 파일에 나뉜다:
// AssetManager.cpp(공개 API·handle 클래스), AssetManagerHandles.cpp(슬롯·해제
// 큐·티켓·callback·scope· 보류 명령), AssetManagerUnits.cpp(상주 단위·간선
// 표·병합·Closing·Discarding).
namespace rocks {

constexpr uint32 ManagerNoIndex = NumericLimits<uint32>::Max();
// handle 슬롯 블록 하나의 슬롯 수와 블록 표 크기다(11.7절 6 — 블록은 옮기지 않고 표는 고정 크기라
// 다른 스레드의 소멸자가 Game 스레드의 확장과 경쟁하지 않는다). 최대 handle 수 = 둘의 곱
// (AssetManagerMaxLiveHandles).
constexpr uint32 ManagerSlotsPerBlock = 256;
constexpr uint32 ManagerMaxSlotBlocks = AssetManagerMaxLiveHandles / ManagerSlotsPerBlock;
static_assert(ManagerMaxSlotBlocks * ManagerSlotsPerBlock == AssetManagerMaxLiveHandles,
              "AssetManagerMaxLiveHandles must be a whole number of slot blocks");

enum class ManagerSlotKind : uint8 {
    Free,
    Ticket,
    Lease,
    Deferred,
    PrimaryTicket
};

// 공개 handle 객체(LoadTicket·AssetLease·DeferredCommand) 하나의 슬롯이다. 주소가 고정이다.
// objectAlive·record·payload는 Game 스레드가 발급 때 쓰고, 다른 스레드는 소멸 때 nextReleased만
// 큐 뮤텍스 아래에서 쓴다(그 슬롯은 객체가 살아 있는 동안 재사용되지 않는다).
struct ManagerSlot {
    ManagerSlot* nextReleased = nullptr;
    // 이 슬롯의 번호다(블록을 만들 때 정한다).
    uint32 self = 0;
    uint32 generation = 1;
    uint32 nextFree = ManagerNoIndex;
    ManagerSlotKind kind = ManagerSlotKind::Free;
    // 객체가 살아 있는지(소멸이 큐를 거쳐 Pump가 처리하면 false)다.
    bool objectAlive = false;
    // scope Revoke·Shutdown이 보유를 이미 거두었는지다(소멸은 보유를 다시 줄이지 않는다).
    bool revoked = false;
    // 티켓·보류 명령 기록 번호(Ticket·Deferred)다.
    uint32 record = ManagerNoIndex;
    // lease의 보유 대상 패키지 기록 번호와 scope다(Lease).
    uint32 package = ManagerNoIndex;
    uint32 scope = ManagerNoIndex;
    AssetId asset{};
    ObjectHandle assetObject{};
    ObjectHandle packageHandle{};
};

struct ManagerSlotBlock {
    ManagerSlot slots[ManagerSlotsPerBlock];
};

// 티켓 기록이다(Game 스레드만). 공개 LoadTicket의 것(slot 있음)과 LoadBlocking·보류 명령의 내부
// 티켓(slot 없음)이 같은 표를 쓴다.
struct ManagerTicket {
    bool used = false;
    uint32 nextFree = ManagerNoIndex;
    uint32 slot = ManagerNoIndex;
    uint32 entry = ManagerNoIndex;
    AssetId asset{};
    TypeId expectedType{};
    uint32 scope = ManagerNoIndex;
    AssetLoadHandle loaderHandle{};
    bool loaderHeld = false;
    bool done = false;
    ErrorCode error = ErrorCode::None;
    AssetDiagnostic diagnostic;
    // 보유를 건 패키지 기록 번호다(성공해 관심을 쥔 동안 — 없으면 NoIndex).
    uint32 heldPackage = ManagerNoIndex;
    // 공개 티켓 객체가 소멸했는지다(callback 전달이 남아 있으면 기록은 그 뒤에 지운다).
    bool released = false;
    AssetReadyCallback callback;
    bool callbackRegistered = false;
    bool callbackDelivered = false;
};

// 보류 명령 기록이다(11.7절 9).
struct ManagerDeferred {
    bool used = false;
    uint32 nextFree = ManagerNoIndex;
    uint32 slot = ManagerNoIndex;
    uint32 ticket = ManagerNoIndex;
    uint32 scope = ManagerNoIndex;
    DeferredRun run;
    DeferredFail fail;
    // 끝을 앞당긴 원인(Cancelled·ScopeClosed·ShuttingDown — 없으면 None)이다.
    ErrorCode stop = ErrorCode::None;
    bool done = false;
};

// PrimaryAsset 묶음 티켓 기록이다(11.7절 10). 구성 요청마다 내부 티켓을 쥔다(대표가 먼저, 그다음
// 구성원 AssetId 순).
struct ManagerPrimary {
    explicit ManagerPrimary(Allocator& allocator) noexcept : tickets(allocator) {
    }

    bool used = false;
    uint32 nextFree = ManagerNoIndex;
    uint32 slot = ManagerNoIndex;
    uint32 scope = ManagerNoIndex;
    Array<uint32> tickets;
    PrimaryReadyCallback callback;
    bool callbackRegistered = false;
    bool callbackDelivered = false;
    bool released = false;
    // 끝을 앞당긴 원인(Cancelled·ScopeClosed·ShuttingDown — 없으면 None)이다.
    ErrorCode stop = ErrorCode::None;
};

struct ManagerScope {
    bool used = false;
    bool revoked = false;
    uint32 generation = 1;
    uint32 nextFree = ManagerNoIndex;
};

// 상주 패키지 기록이다(11.7절 3). 단위가 끝날 때까지 남는다(티켓·lease는 이 번호로 보유를 걸어
// 단위 병합과 무관하다).
struct ManagerPackage {
    bool used = false;
    uint32 nextFree = ManagerNoIndex;
    uint32 entry = ManagerNoIndex;
    AssetId asset{};
    ObjectHandle handle{};
    ObjectHandle assetObject{};
    uint32 loaderSlot = ManagerNoIndex;
    uint32 unit = ManagerNoIndex;
    // 같은 단위의 다음 패키지(침입 연결 — 병합이 할당 없이 이어 붙인다)다.
    uint32 nextInUnit = ManagerNoIndex;
    bool discarded = false;
};

// 상주 단위다(11.7절 3). 구성 패키지는 침입 연결 목록이고, 병합하면 목록을 한 단위로 이어 붙이고
// 다른 단위는 지운다(할당 없음).
struct ManagerUnit {
    bool used = false;
    uint32 nextFree = ManagerNoIndex;
    ResidentUnitState state = ResidentUnitState::Resident;
    uint32 firstPackage = ManagerNoIndex;
    uint32 lastPackage = ManagerNoIndex;
    uint32 packageCount = 0;
    uint64 tickets = 0;
    uint64 leases = 0;
    uint64 deferred = 0;
    int64 inbound = 0;
    // Discarding 단위에서 다음에 Discard할 패키지다.
    uint32 discardNext = ManagerNoIndex;
    // 도달 탐색 표지(탐색마다 다른 값)다.
    uint64 visitStamp = 0;
    uint64 backStamp = 0;
};

// (소유 패키지, 대상 패키지) 간선 쌍이다.
struct ManagerEdgeKey {
    ObjectHandle owner{};
    ObjectHandle target{};

    bool operator==(const ManagerEdgeKey&) const noexcept = default;
};

template <>
struct Hash<ManagerEdgeKey> {
    uint64 operator()(const ManagerEdgeKey& key) const noexcept {
        return HashCombine(Hash<ObjectHandle>{}(key.owner), Hash<ObjectHandle>{}(key.target));
    }
};

// 패키지 하나의 간선 이웃(다중도가 양수인 쌍)이다. 상주가 아닌 패키지(PostLoading 등)도 갖는다.
struct ManagerEdgeNode {
    explicit ManagerEdgeNode(Allocator& allocator) noexcept : in(allocator), out(allocator) {
    }

    Array<ObjectHandle> in;
    Array<ObjectHandle> out;
};

// 해제 큐다(11.7절 6 — 뮤텍스 + 침입 연결, 할당 없음).
struct ManagerReleaseQueue {
    platform::Mutex mutex;
    ManagerSlot* head = nullptr;
};

struct AssetManager::Impl {
    Impl(AssetManager& owner, const AssetManagerConfig& config) noexcept;
    ~Impl();

    AssetManager& owner;
    const AssetManagerConfig& config;
    Allocator& allocator;
    UniquePtr<ReferenceObserver> observer;
    UniquePtr<ObjectRegistry> registry;
    AssetLoaderConfig loaderConfig;
    UniquePtr<AssetLoader> loader;
    ObjectAccessContext access{};
    uint64 ownerThread = 0;
    bool shutDown = false;
    // callback·보류 명령 실행 중인지다(Pump·LoadBlocking·Shutdown은 WrongContext — 11.7절 5).
    bool inCallback = false;

    // handle 슬롯(고정 블록)과 빈 칸 목록, 살아 있는 handle 객체 수(다른 스레드가 줄인다)다.
    ManagerSlotBlock* blocks[ManagerMaxSlotBlocks] = {};
    uint32 blockCount = 0;
    uint32 freeSlot = ManagerNoIndex;
    uint32 liveObjects = 0;
    ManagerReleaseQueue releases;

    Array<ManagerTicket> tickets;
    uint32 freeTicket = ManagerNoIndex;
    Array<ManagerDeferred> deferred;
    uint32 freeDeferred = ManagerNoIndex;
    uint32 activeDeferred = 0;
    Array<ManagerScope> scopes;
    uint32 freeScope = ManagerNoIndex;
    Array<ManagerPrimary> primaries;
    uint32 freePrimary = ManagerNoIndex;

    Array<ManagerPackage> packages;
    uint32 freePackage = ManagerNoIndex;
    // 매니페스트 항목마다 지금 상주(Resident·Closing)인 패키지 기록 번호다(Discarding이면 없음).
    Array<uint32> entryPackage;
    HashMap<ObjectHandle, uint32> packageOf;
    Array<ManagerUnit> units;
    uint32 freeUnit = ManagerNoIndex;
    uint64 stamp = 0;
    uint32 createdUnits = 0;

    HashMap<ManagerEdgeKey, int64> edges;
    HashMap<ObjectHandle, uint32> edgeNodeOf;
    Array<ManagerEdgeNode> edgeNodes;
    Array<uint32> freeEdgeNodes;
    uint64 lateEdgesIntoDiscarding = 0;
    bool lateEdgeWarned = false;

    // Pump 작업 공간이다. published·changes는 처리하지 못한 것(할당 실패)을 다음 Pump까지 쥔다.
    Array<AssetLoaderPublished> published;
    Array<ReferenceEdgeDelta> changes;
    Array<uint32> workStack;
    Array<uint32> mergeSet;
    Array<uint32> cycleOwners;
    Array<uint32> cycleTargets;
    Array<AssetId> scratchAssets;
    Array<uint32> scratchSlots;
    Array<uint32> deliver;

    bool OnOwnerThread() const noexcept;
    ManagerSlot& Slot(uint32 index) noexcept;
    const ManagerSlot& Slot(uint32 index) const noexcept;
    bool ValidSlot(uint32 index, uint32 generation, ManagerSlotKind kind) const noexcept;

    // ---- handle 슬롯·해제 큐(AssetManagerHandles.cpp) ----
    Result<uint32> TakeSlot(ManagerSlotKind kind) noexcept;
    void FreeSlot(uint32 index) noexcept;
    void PostRelease(uint32 index) noexcept;
    void DrainReleases() noexcept;

    Result<uint32> TakeTicket() noexcept;
    void FreeTicket(uint32 index) noexcept;
    Result<uint32> StartTicket(const AssetRequest& request, AssetDiagnostic* failure) noexcept;
    void CompleteTicket(uint32 index) noexcept;
    void FinishTicketResult(uint32 index, AssetLoadStatus status) noexcept;
    void ReleaseTicketHold(uint32 index) noexcept;
    void EndTicket(uint32 index, ErrorCode error) noexcept;
    void CompleteTickets() noexcept;
    Result<AssetLease> MakeLease(uint32 package, uint32 scope) noexcept;
    uint32 DeliverCallbacks() noexcept;
    uint32 RunDeferred() noexcept;
    void FreeDeferred(uint32 index) noexcept;
    void RevokeScope(uint32 scope, ErrorCode reason) noexcept;

    // ---- PrimaryAsset·bundle(AssetManagerPrimary.cpp) ----
    Result<void> ResolvePrimary(AssetId asset, Span<const StringView> bundles, Array<AssetId>& out,
                                AssetDiagnostic* failure) const noexcept;
    Result<uint32> TakePrimary() noexcept;
    void FreePrimary(uint32 index) noexcept;
    void EndPrimary(uint32 index, ErrorCode reason) noexcept;
    void ReleasePrimary(uint32 index) noexcept;
    uint32 DeliverPrimary() noexcept;
    bool ScopeOpen(uint32 scope) const noexcept;

    // ---- 상주 단위(AssetManagerUnits.cpp) ----
    Result<void> IngestPublished() noexcept;
    Result<uint32> TakeUnit() noexcept;
    void FreeUnit(uint32 index) noexcept;
    uint32 UnitOf(ObjectHandle package) const noexcept;
    void AddHold(uint32 package, uint64 ManagerUnit::* counter) noexcept;
    void DropHold(uint32 package, uint64 ManagerUnit::* counter) noexcept;
    Result<void> ApplyEdgeChanges() noexcept;
    void LinkEdge(ObjectHandle owner, ObjectHandle target) noexcept;
    void UnlinkEdge(ObjectHandle owner, ObjectHandle target) noexcept;
    int64 InboundOf(uint32 unit) const noexcept;
    void CheckCycle(uint32 from, uint32 to) noexcept;
    void MergeUnits() noexcept;
    uint64 LoaderHolds(uint32 unit) const noexcept;
    void JudgeClosing() noexcept;
    uint32 ProcessClosing(uint32 maxDiscards) noexcept;
    void EndUnit(uint32 unit) noexcept;
};

} // namespace rocks
