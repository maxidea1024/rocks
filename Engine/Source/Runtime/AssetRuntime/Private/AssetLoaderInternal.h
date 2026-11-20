#pragma once

#include "PackageInternal.h"

#include <AssetRuntime/AssetLoader.h>
#include <AssetRuntime/Package.h>
#include <Core/Function.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Text/StringBuilder.h>
#include <Object/ConstructionContext.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Sync.h>
#include <Task/WorkerScheduler.h>

// 쿠킹 패키지 로더다(Docs/Design/AssetSystem.ko.md 11.6절). 모든 상태 전이는 소유(Game) 스레드가
// 하고, IO·worker 스레드는 패키지 작업의 버퍼·로드 상태만 만진 뒤 완료 큐에 작업을 넣는다(할당
// 없음).
//
// AssetLoader의 내부 타입과 Impl 선언이다(AssetLoader*.cpp만 포함한다 — 정의는 기능별 파일에
// 나뉘어 있다: Resolve(초기화·해석·요청), Stages(정리·허가·완료 처리), Publish(공개·Pump·종료),
// AssetLoader.cpp(공개 API)).
namespace rocks {

constexpr uint32 LoaderNoIndex = NumericLimits<uint32>::Max();
// 패키지 경로 builder의 내장 용량이다("<루트>/Packages/xx/<36자>-<16자>.rpkg" — 루트가 짧으면 할당
// 없이 담긴다).
constexpr usize LoaderPathInlineBytes = 160;
// FindByPath 질의 키 builder의 내장 용량이다(경로 상한 256 byte 근처까지 할당 없이).
constexpr usize LoaderKeyInlineBytes = 272;

enum class LoaderCompletion : uint8 {
    None,
    Io,
    Worker
};

enum class LoaderRetire : uint8 {
    None,
    Fail,
    Abandon
};

// RootRef 칸의 상태다(11.7절 2): 빈 칸, 공개(entryLoaded가 가리킴), 퇴출(RetirePublished 뒤 —
// RootRef만 쥐고 ReleaseRetired를 기다림).
enum class LoaderSlot : uint8 {
    Free,
    Published,
    Retired
};

struct LoaderCompletionQueue;

// 패키지 하나의 로드 작업이다. 주소가 고정이어야 하므로(IO·worker가 포인터를 쥔다) 하나씩 할당한다.
struct LoaderPackageJob {
    explicit LoaderPackageJob(Allocator& allocator) noexcept : file(allocator), path(allocator) {
    }

    LoaderCompletionQueue* queue = nullptr;
    const CookedManifest* manifest = nullptr;
    uint32 entry = 0;
    uint32 unit = 0;
    AssetLoaderPackagePhase phase = AssetLoaderPackagePhase::Queued;
    UniquePtr<PackageLoadState> state;
    Array<uint8> file;
    StringBuilder<LoaderPathInlineBytes> path;
    CookedPackageRead read;
    // io.Release가 남았는지다(Begin 성공 뒤 완료를 처리할 때까지).
    bool readBegun = false;
    // IO·worker에 넘긴 일이 있는지다(Game 스레드가 보는 값 — 완료를 꺼낼 때 지운다).
    bool asyncPending = false;
    // 버린 작업 표시다(worker가 원자 값으로 읽는다 — 1이면 Prepare를 하지 않는다).
    uint32 abandoned = 0;
    // 완료 결과(게시한 스레드가 쓰고, 큐 뮤텍스가 Game 스레드에 넘긴다).
    LoaderCompletion completion = LoaderCompletion::None;
    CookedPackageReadResult ioResult;
    ErrorCode workerError = ErrorCode::None;
    AssetDiagnostic workerDiagnostic;
    LoaderPackageJob* nextCompletion = nullptr;
    // 패키지의 임시 byte(파일 + bulkRawBytes, 포화)와 그중 tempBytesInFlight에 실제로 더한 양이다.
    uint64 tempBytes = 0;
    uint64 accountedBytes = 0;
    uint64 phaseStart = 0;
    uint64 phaseNanoseconds[AssetLoaderPackagePhaseCount] = {};
};

// 완료 큐다(침입 연결 — 게시에 할당이 없다).
struct LoaderCompletionQueue {
    platform::Mutex mutex;
    platform::ConditionVariable changed;
    LoaderPackageJob* head = nullptr;
    LoaderPackageJob* tail = nullptr;
};

inline void PostLoaderCompletion(LoaderPackageJob& job) noexcept {
    LoaderCompletionQueue& queue = *job.queue;
    queue.mutex.Lock();
    job.nextCompletion = nullptr;
    if (queue.tail == nullptr) {
        queue.head = &job;
    } else {
        queue.tail->nextCompletion = &job;
    }

    queue.tail = &job;
    // 깨움도 뮤텍스 안에서 한다 — 놓은 뒤에는 job·queue를 만지지 않는다(Game이 바로 지울 수 있다).
    queue.changed.NotifyAll();
    queue.mutex.Unlock();
}

inline void OnLoaderReadComplete(CookedPackageRead& read,
                                 const CookedPackageReadResult& result) noexcept {
    auto& job = *static_cast<LoaderPackageJob*>(read.owner);
    job.ioResult = result;
    job.completion = LoaderCompletion::Io;
    PostLoaderCompletion(job);
}

// worker 단계다(11.6절 3): 크기·SHA-256 → Prepare(압축 해제·검증) → 내용 대조.
inline void RunLoaderPrepare(void* context) noexcept {
    auto& job = *static_cast<LoaderPackageJob*>(context);
    job.completion = LoaderCompletion::Worker;
    job.workerError = ErrorCode::None;
    if (platform::AtomicLoad<platform::MemoryOrder::Acquire>(&job.abandoned) != 0) {
        job.workerError = ErrorCode::Cancelled;
        PostLoaderCompletion(job);
        return;
    }

    const CookedManifestEntry& entry = job.manifest->entries[job.entry];
    AssetDiagnostic diagnostic;
    Result<void> step = CheckCookedPackageFile(entry, job.file.View(), &diagnostic);
    if (step) {
        step = job.state->Prepare(Move(job.file), &diagnostic);
    }

    if (step) {
        step = CheckCookedPackageContents(*job.manifest, entry, job.state->Contents(), &diagnostic);
    }

    if (!step) {
        job.workerError = step.Error();
        job.workerDiagnostic = diagnostic;
    }

    PostLoaderCompletion(job);
}

inline uint64 LoaderDefaultClock(void*) noexcept {
    return platform::MonotonicNanoseconds();
}

// 요청 기록이다(번호 재사용 — 세대로 옛 handle을 거른다).
struct LoaderRequest {
    uint32 generation = 1;
    bool used = false;
    uint32 nextFree = LoaderNoIndex;
    uint32 entry = 0;
    LoadPriority priority = LoadPriority::Normal;
    CancelToken cancel;
    AssetLoadState state = AssetLoadState::Pending;
    ErrorCode error = ErrorCode::None;
    AssetDiagnostic diagnostic;
    ObjectHandle package{};
    uint32 unit = LoaderNoIndex;
};

// 로드 단위(Hard SCC)다.
struct LoaderUnit {
    explicit LoaderUnit(Allocator& allocator) noexcept
        : members(allocator), jobs(allocator), dependencies(allocator), dependents(allocator),
          requests(allocator), publishedDeps(allocator) {
    }

    bool used = false;
    uint32 nextFree = LoaderNoIndex;
    AssetLoaderUnitPhase phase = AssetLoaderUnitPhase::Queued;
    // 구성 패키지(매니페스트 항목 번호 — 오름차순)와 허가 뒤의 작업(같은 순서)이다.
    Array<uint32> members;
    Array<LoaderPackageJob*> jobs;
    // 아직 공개되지 않은 의존 단위와 그 단위에 기대는 단위다.
    Array<uint32> dependencies;
    Array<uint32> dependents;
    // 이 단위의 패키지를 기다리는 Pending 요청이다.
    Array<uint32> requests;
    // 이 단위가 Hard로 기대는 공개된(PostLoad까지 마친) 패키지 항목이다(11.7절 2 — 공개 보유).
    // 해석 때 만난 것과 의존 단위가 끝나며 넘어온 것이고, 용량은 해석 때 미리 잡는다.
    Array<uint32> publishedDeps;
    // publishedDeps의 보유를 publishedHolds에 걸었는지다(놓을 때 한 번만 놓는다).
    bool publishedHeld = false;
    uint32 unadmittedDependencies = 0;
    uint32 waitingDependencies = 0;
    uint32 interest = 0;
    uint32 preparedPackages = 0;
    uint32 aliveJobs = 0;
    // PostLoading 단위에서 다음에 PostLoad할 구성 번호다.
    uint32 postLoadNext = 0;
    LoadPriority priority = LoadPriority::Normal;
    uint64 sequence = 0;
    uint64 queuedAt = 0;
    uint64 tempBytes = 0;
    // 정리 작업 목록(침입 연결)이다.
    LoaderRetire retire = LoaderRetire::None;
    uint32 nextRetire = LoaderNoIndex;
    // 실패 원인(실패면): 처음 실패한 패키지 항목·오류·진단이다.
    uint32 failedEntry = LoaderNoIndex;
    ErrorCode failError = ErrorCode::None;
    AssetDiagnostic failDiagnostic;
};

// 바깥 코드를 부르는 동안 "콜백 중" 표시를 세우고 끝나면 이전 값으로 되돌린다(11.6절 18 — 호출마다
// 하나, 겹쳐도 바깥 표시를 먼저 풀지 않는다).
class LoaderCallbackScope {
public:
    explicit LoaderCallbackScope(bool& flag) noexcept : flag_(flag), previous_(flag) {
        flag_ = true;
    }

    ~LoaderCallbackScope() {
        flag_ = previous_;
    }

    LoaderCallbackScope(const LoaderCallbackScope&) = delete;
    LoaderCallbackScope& operator=(const LoaderCallbackScope&) = delete;

private:
    bool& flag_;
    bool previous_;
};

// Tarjan 반복 구현의 틀 하나다.
struct LoaderTarjanFrame {
    uint32 entry = 0;
    uint32 position = 0;
};

// 포화 덧셈이다(임시 byte 합 — bulkRawBytes는 u64 최댓값까지 유효하다, 11.6절 9).
inline uint64 LoaderSaturatingAdd(uint64 left, uint64 right) noexcept {
    return right > NumericLimits<uint64>::Max() - left ? NumericLimits<uint64>::Max()
                                                       : left + right;
}

// 여러 할당 단계의 처음 실패를 first에 남긴다(단계는 모두 실행한다 — 실패 뒤 단계는 버릴 상태에
// 용량만 더 잡는다).
inline void LoaderKeepFirst(ErrorCode& first, Result<void>&& result) noexcept {
    if (first == ErrorCode::None && !result) {
        first = result.Error();
    }
}

inline bool LoaderValidPriority(LoadPriority priority) noexcept {
    return static_cast<uint32>(priority) < LoadPriorityCount;
}

struct AssetLoader::Impl {
    Impl(AssetLoader& owner, const AssetLoaderConfig& config) noexcept
        : owner(owner), config(config), allocator(*config.allocator),
          access(config.registry->OwnerAccess()), root(allocator), entryUnit(allocator),
          entryLoaded(allocator), keyBytes(allocator), keyOffsets(allocator), keyOrder(allocator),
          loadedRoots(allocator), loadedHandles(allocator), slotStates(allocator),
          freeLoadedSlots(allocator), publishedHolds(allocator), published(allocator),
          requests(allocator), units(allocator), admitCandidates(allocator), readyUnits(allocator),
          postLoadUnits(allocator), tarjanIndex(allocator), tarjanLow(allocator),
          tarjanOnStack(allocator), tarjanVisited(allocator), tarjanFrames(allocator),
          tarjanStack(allocator), sccMembers(allocator), sccStarts(allocator), newUnits(allocator),
          dependencyCounts(allocator), workStack(allocator) {
    }

    AssetLoader& owner;
    const AssetLoaderConfig& config;
    Allocator& allocator;
    ObjectAccessContext access;
    uint64 ownerThread = 0;
    StringBuilder<LoaderPathInlineBytes> root;
    bool shutDown = false;
    // 바깥 코드(PostLoad handler·관찰자·시험 hook·IO·worker·시계·프레임 판정·공개 트랜잭션)가
    // 불리는 중인지다(11.6절 18 — 상태를 바꾸는 공개 함수는 WrongContext). LoaderCallbackScope로만
    // 바꾼다. (시계는 const 조회 안에서도 불리므로 mutable이다.)
    mutable bool inCallback = false;

    // 매니페스트 항목마다 진행 중 단위 번호와 공개 번호(없으면 LoaderNoIndex)다.
    Array<uint32> entryUnit;
    Array<uint32> entryLoaded;
    // FindByPath 색인: 항목마다 caseless 키를 이어 붙인 byte와 시작 위치, 키 순으로 정렬한 항목
    // 번호.
    Array<char> keyBytes;
    Array<uint64> keyOffsets;
    Array<uint32> keyOrder;
    // 공개한 패키지다. 칸 상태(LoaderSlot)는 빈 칸·공개·퇴출이고, ReleaseRetired가 돌려준 칸은
    // freeLoadedSlots로 다시 쓴다.
    Array<RootRef<Object>> loadedRoots;
    Array<ObjectHandle> loadedHandles;
    Array<uint8> slotStates;
    Array<uint32> freeLoadedSlots;
    // 항목마다 그 공개 패키지에 기대는 진행 중 단위 수다(11.7절 2 — 공개 보유).
    Array<uint32> publishedHolds;
    // TakePublished가 꺼내 갈 기록(PostLoad까지 끝난 패키지)과 다음 순번이다. pendingPublished는
    // 공개됐지만 아직 PostLoad 중인 패키지 수다(그 기록 용량을 공개 때 미리 잡는다).
    Array<AssetLoaderPublished> published;
    usize pendingPublished = 0;
    uint64 nextPublishedGroup = 0;

    Array<LoaderRequest> requests;
    uint32 freeRequest = LoaderNoIndex;
    Array<LoaderUnit> units;
    uint32 freeUnit = LoaderNoIndex;
    uint32 usedUnits = 0;
    // 허가 후보(의존 단위가 모두 허가됨)와 공개 준비 단위다. 용량은 단위 표 크기 이상으로 미리
    // 잡는다(추가에 할당이 없다).
    Array<uint32> admitCandidates;
    Array<uint32> readyUnits;
    // PostLoad를 기다리는 공개된 단위다(용량은 위 둘과 같이 잡는다).
    Array<uint32> postLoadUnits;
    uint32 retireHead = LoaderNoIndex;
    uint32 retireTail = LoaderNoIndex;

    // ResolveManifest 작업 공간이다(항목 표 크기 — 쓴 칸은 끝에 되돌린다).
    Array<uint32> tarjanIndex;
    Array<uint32> tarjanLow;
    Array<uint8> tarjanOnStack;
    Array<uint32> tarjanVisited;
    Array<LoaderTarjanFrame> tarjanFrames;
    Array<uint32> tarjanStack;
    Array<uint32> sccMembers;
    Array<uint32> sccStarts;
    Array<uint32> newUnits;
    Array<uint32> dependencyCounts;
    Array<uint32> workStack;

    LoaderCompletionQueue queue;
    // IO·worker에 넘긴 일 수다(Game 스레드 값).
    uint32 asyncInFlight = 0;
    uint32 packagesInFlight = 0;
    uint64 tempBytesInFlight = 0;
    uint64 admissions = 0;
    uint64 nextSequence = 0;
    uint64 syncWaits = 0;
    uint64 syncWaitNanoseconds = 0;
    // 최근 동기 대기 고리(다음 쓸 자리 = syncWaits % 용량)다.
    AssetLoaderSyncWait recentWaits[AssetLoaderSnapshot::RecentSyncWaitCapacity] = {};

    // 바깥 코드(설정 시계)는 콜백 중 표시 안에서 부른다(11.6절 18).
    uint64 Now() const noexcept;

    bool OnOwnerThread() const noexcept;

    uint32 EntryIndex(const CookedManifestEntry* entry) const noexcept;

    AssetId EntryAsset(uint32 entry) const noexcept;

    LoadPriority Effective(const LoaderUnit& unit) const noexcept;

    // a가 b보다 먼저인지다(실효 우선순위 → 생성 순서).
    bool Before(uint32 a, uint32 b) const noexcept;

    // ---- 초기화 ----

    Result<void> BuildPathIndex() noexcept;

    StringView KeyOf(uint32 entry) const noexcept;

    Result<void> Initialize(AssetDiagnostic* failure) noexcept;

    // ---- 단위 표 ----

    // 단위 칸 하나를 꺼낸다(용량은 호출자가 미리 잡았다 — 할당하지 않는다).
    uint32 TakeUnitSlot() noexcept;

    // 단위가 건 공개 보유를 놓는다(공개·실패·버림·칸 반납 — 한 번만, 11.7절 2).
    void ReleasePublishedHolds(LoaderUnit& unit) noexcept;

    void FreeUnitSlot(uint32 index) noexcept;

    static void RemoveValue(Array<uint32>& values, uint32 value) noexcept;

    // ---- ResolveManifest(11.6절 2) ----

    void ResetTarjan() noexcept;

    // 항목 entry의 i번째 의존이 이번 해석에서 따라갈 Hard 대상이면 그 항목 번호, 아니면
    // LoaderNoIndex다.
    uint32 HardTarget(uint32 entry, uint32 position) const noexcept;

    bool IsNew(uint32 entry) const noexcept;

    Result<void> Visit(uint32 entry, uint32& counter) noexcept;

    // root에서 닿는 새 항목들의 SCC를 sccMembers·sccStarts에 의존 먼저 순서로 담는다(반복 Tarjan).
    Result<void> FindComponents(uint32 rootEntry) noexcept;

    // 새 단위들을 지운다(해석 실패 되돌림 — 아직 다른 단위·항목 표에 잇지 않았다).
    void DiscardNewUnits() noexcept;

    // rootEntry에서 새 단위들을 만들어 잇는다. 성공하면 rootEntry가 속한 단위 번호를 준다(그 단위의
    // 요청 자리 하나와 Boost 작업 목록 용량도 잡는다). 실패(할당)면 로더 상태는 그대로다.
    Result<uint32> Resolve(uint32 rootEntry) noexcept;

    // unit과 그 의존 단위의 우선순위를 priority로 올린다(UE UpdatePackagePriorityRecursive —
    // 올리기만 한다). workStack 용량은 단위 표 크기 이상이다.
    void Boost(uint32 start, LoadPriority priority) noexcept;

    // ---- 요청 ----

    bool ValidHandle(AssetLoadHandle handle) const noexcept;

    void FinishRequest(uint32 index, AssetLoadState state, ErrorCode error,
                       const AssetDiagnostic& diagnostic, ObjectHandle package) noexcept;

    void FreeRequest(uint32 index) noexcept;

    // ---- 정리(실패·버림 — 침입 목록, 할당 없음) ----

    void QueueRetire(uint32 unit, LoaderRetire kind) noexcept;

    void DropInterest(uint32 unit) noexcept;

    // 실패한 단위 unit의 원인을 적고 정리 목록에 넣는다.
    void FailUnit(uint32 unit, uint32 failedEntry, ErrorCode error,
                  const AssetDiagnostic& diagnostic) noexcept;

    // job을 지운다. IO 요청은 완료를 처리할 때(HandleCompletion) 이미 돌려줬다 — 진행 중 일이 남은
    // 작업은 지우지 않는다.
    void DestroyJob(LoaderPackageJob* job) noexcept;

    void RemoveFromLists(uint32 unit) noexcept;

    void ProcessRetire() noexcept;

    // ---- 허가(11.6절 9) ----

    uint64 UnitBytes(const LoaderUnit& unit) const noexcept;

    void SetPhase(LoaderPackageJob& job, AssetLoaderPackagePhase phase) noexcept;

    Result<void> BeginPackage(uint32 unit, uint32 member, AssetDiagnostic& diagnostic) noexcept;

    void AdmitUnit(uint32 unit) noexcept;

    uint32 Admit() noexcept;

    // ---- 완료 처리 ----

    void MarkReadyIfDone(uint32 unit) noexcept;

    // 작업 job을 단위 목록에서 빼고 지운다(버린 단위의 진행 중 작업이 끝났을 때).
    void DrainRetiredJob(LoaderPackageJob* job) noexcept;

    void HandleCompletion(LoaderPackageJob* job) noexcept;

    uint32 DrainCompletions() noexcept;

    void CheckCancellations() noexcept;

    // Pending 요청을 끝내고 관심을 놓는다(정리는 호출자가 ProcessRetire로).
    void CancelRequest(uint32 index, ErrorCode error) noexcept;

    // ---- 공개(11.6절 10) ----

    uint32 PickReady() const noexcept;

    // 실패 진단의 asset이 구성 패키지면 그 항목, 아니면 첫 구성 항목이다.
    uint32 FailedMember(const LoaderUnit& unit, AssetId asset) const noexcept;

    void InstantiateUnit(uint32 unit) noexcept;

    // PostLoad를 마친 단위를 끝낸다: 요청 Succeeded, 기대는 단위의 의존 목록에서 빠지고 칸을
    // 돌려준다.
    void FinishUnit(uint32 unit) noexcept;

    // PostLoad 차례인 단위(의존 단위의 PostLoad가 모두 끝남) 중 가장 먼저인 것이다(없으면 NoIndex).
    uint32 PickPostLoad() const noexcept;

    // 단위 unit의 다음 구성 패키지 하나를 PostLoad한다. handler가 도는 동안은 콜백 중 표시를
    // 쥐고(상태를
    // 바꾸는 재진입 거부), 단위 참조를 쥐지 않는다(돌아온 뒤 다시 찾는다).
    void PostLoadNext(uint32 unit) noexcept;

    // ---- Pump ----

    AssetLoaderPumpResult PumpOnce(const AssetLoaderPumpBudget& budget) noexcept;

    // ---- 종료 ----

    void ShutdownAll() noexcept;

    void WaitCompletions() noexcept;
};

} // namespace rocks
