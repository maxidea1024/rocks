#pragma once

#include <AssetRuntime/AssetDiagnostic.h>
#include <AssetRuntime/CancelToken.h>
#include <AssetRuntime/CookedManifest.h>
#include <AssetRuntime/CookedPackageIo.h>
#include <AssetRuntime/PackageLoad.h>
#include <Base/NumericLimits.h>
#include <Core/Containers/Array.h>

// 쿠킹 패키지 로더다(Docs/Design/AssetSystem.ko.md 11.2–11.6절, A1c). 쿠킹 매니페스트에서 요청
// 에셋의 Hard 전이 폐포를 구하고(ResolveManifest) Hard SCC마다 로드 단위를 만들어, 패키지를
// IO(CookedPackageIo) → worker(크기·SHA-256 → Prepare(압축 해제·검증) → 내용 대조) → Game Pump(단위
// 하나를 한 트랜잭션으로 InstantiatePackages → Commit)로 진행한다. UE 5.8
// AsyncLoading2(FAsyncLoadingThread2· EAsyncPackageLoadingState2) 대응이다.
//
// 요청 관심·lease·scope·OnReady callback·상주 단위 해제는 그 위의 AssetManager(A1d) 몫이다. 이
// 로더는 공개한 패키지를 Shutdown까지 붙잡는다.
//
// 사용 예(엔진 프레임):
//   AssetLoader loader(config);
//   if (auto started = loader.Initialize(&failure); !started) { ... }
//   auto handle = loader.Request({heroId, LoadPriority::High, cancel});
//   // 프레임마다:
//   loader.Pump({4, 2'000'000});   // 단위 4개 또는 2 ms
//   auto status = loader.Status(handle.Value());
//   if (status.Value().state == AssetLoadState::Succeeded) { use(status.Value().package); }
//   loader.Release(handle.Value());
//
// 스레드: 모든 공개 함수는 Initialize를 부른 스레드(에셋 객체 Registry의 소유 스레드 — Game)
// 전용이다. 다른 스레드에서 부르면 WrongThread다. IO 완료와 worker 단계는 다른 스레드에서 돌고
// 결과는 완료 큐로 Pump에 넘어온다.
namespace rocks {

class WorkerScheduler;

// 로드 우선순위다(11.6절 8, UE TAsyncLoadPriority 대응). 높을수록 먼저 허가·공개한다. 단위의
// 우선순위는 올라가기만 하고 의존 단위로 전파된다.
enum class LoadPriority : uint8 {
    Low,
    Normal,
    High,
    Critical
};

inline constexpr uint32 LoadPriorityCount = static_cast<uint32>(LoadPriority::Critical) + 1;

// worker 단계를 실행하는 경계다(11.6절 5). 기본 구현은 WorkerSchedulerLoaderWorker이고 시험은
// 손으로 돌리는 구현을 끼운다.
class AssetLoaderWorker {
public:
    using Job = void (*)(void* context) noexcept;

    // job(context)를 어느 스레드에서 정확히 한 번 실행한다. 실패(자원·종료)면 실행하지 않고 그
    // 오류를 돌려준다. Game 스레드에서 불린다.
    virtual Result<void> Run(Job job, void* context) noexcept = 0;

protected:
    ~AssetLoaderWorker() = default;
};

// WorkerScheduler에 제출하는 worker 구현이다(작업 묶음 하나를 allocator로 만든다). scheduler와
// allocator는 이 객체와 로더보다 오래 살아야 한다.
class WorkerSchedulerLoaderWorker final : public AssetLoaderWorker {
public:
    WorkerSchedulerLoaderWorker(WorkerScheduler& scheduler, Allocator& allocator) noexcept
        : scheduler_(scheduler), allocator_(allocator) {
    }

    Result<void> Run(Job job, void* context) noexcept override;

private:
    WorkerScheduler& scheduler_;
    Allocator& allocator_;
};

// 동기 대기 하나의 기록이다(11.6절 11).
struct AssetLoaderSyncWait {
    // 호출 지점(호출자가 준 정적 문자열 — 예: "TextureTool::Main")이다.
    const char* site = nullptr;
    AssetId asset{};
    uint64 nanoseconds = 0;
};

// 로더 관찰자다(Rocks Lens 연결은 EngineAssets가 이것으로 한다). Game 스레드에서 불리며 로더 상태를
// 바꾸는 함수를 부르면 안 된다.
class AssetLoaderObserver {
public:
    virtual void OnSyncWait(const AssetLoaderSyncWait& wait) noexcept = 0;

protected:
    ~AssetLoaderObserver() = default;
};

// PostLoad 단계 handler다(11.6절 16, UE UObject::PostLoad 호출 단계 대응). 공개한 단위의 패키지마다
// Game 스레드에서 한 번 불린다(의존 단위의 PostLoad가 끝난 뒤, 단위 안은 AssetId 순). 실패하지
// 않으며 로더 상태를 바꾸는 함수를 부르면 안 된다. 클래스별 dispatch는 handler를 준 쪽(EngineAssets
// — A5에서 클래스 hook과 연결)이 정한다.
class AssetPostLoadHandler {
public:
    virtual void PostLoad(ObjectHandle package, AssetId asset) noexcept = 0;

protected:
    ~AssetPostLoadHandler() = default;
};

// 동시 진행·임시 메모리 상한이다(11.6절 9).
struct AssetLoaderLimits {
    static constexpr uint32 DefaultMaxPackagesInFlight = 32;
    static constexpr uint64 DefaultMaxTempBytes = uint64{512} * 1024 * 1024;
    static constexpr uint32 DefaultAgingAdmissions = 16;

    // 허가된(IO·worker·공개 대기) 패키지 수의 상한이다(1 이상).
    uint32 maxPackagesInFlight = DefaultMaxPackagesInFlight;
    // 허가된 패키지의 임시 byte(파일 크기 + bulkRawBytes) 합의 상한이다.
    uint64 maxTempBytes = DefaultMaxTempBytes;
    // 대기 중 다른 단위가 이만큼 허가될 때마다 실효 우선순위가 한 단계 오른다(1 이상).
    uint32 agingAdmissions = DefaultAgingAdmissions;
};

// 로더 설정이다. 포인터는 모두 로더보다 오래 살아야 한다.
struct AssetLoaderConfig {
    // 로더 상태·패키지 로드 상태·압축을 푼 bulk의 allocator다. 여러 스레드의 할당·해제를 지원해야
    // 하고 로더가 공개한 Package(Registry)보다 오래 살아야 한다(AS 5.7절 16).
    Allocator* allocator = nullptr;
    // 에셋 객체 Registry(패키지 정책)와 그 TypeRegistry다.
    ObjectRegistry* registry = nullptr;
    TypeRegistry* types = nullptr;
    // 읽은(서명 확인을 마친) 쿠킹 매니페스트다.
    const CookedManifest* manifest = nullptr;
    // 쿠킹 루트다(패키지 경로 = 루트 + "/" + AppendCookedPackagePath). Initialize가 복사한다.
    StringView cookedRoot;
    // 실행 프로파일 id다(매니페스트와 다르면 Initialize가 IncompatibleCook).
    StringView runtimeProfile;
    CookedPackageIo* io = nullptr;
    AssetLoaderWorker* worker = nullptr;
    AssetLoaderObserver* observer = nullptr;
    AssetLoaderLimits limits;
    // 패키지 로드 옵션의 기준이다(읽기 옵션·그래프 한도). expectedKind는 Cooked로,
    // maxBulkRawBytes는 패키지마다 매니페스트 bulkRawBytes로 바꿔 쓴다(AS 5.7절 11).
    PackageLoadOptions packageOptions;
    // 시계다(ns, 단조). null이면 platform::MonotonicNanoseconds다.
    uint64 (*clock)(void* context) noexcept = nullptr;
    void* clockContext = nullptr;
    // "프레임 실행 중" 판정이다(참이면 WaitFor가 WrongContext). null이면 언제나 거짓이다.
    bool (*inFrame)(void* context) noexcept = nullptr;
    void* inFrameContext = nullptr;
    // PostLoad 단계 handler다(null이면 공개와 함께 PostLoad가 끝난 것으로 본다).
    AssetPostLoadHandler* postLoad = nullptr;
    // 시험 hook: 완료 대기가 조건 변수에서 실제로 기다리기 직전에 불린다(완료 큐 뮤텍스를 쥔 채 —
    // 로더를 부르면 안 된다, 11.6절 17).
    void (*waitEntered)(void* context) noexcept = nullptr;
    void* waitEnteredContext = nullptr;
};

// 요청이다(AS 11.1절 AssetRequest의 로더 쪽 부분 — expectedType·ReadyMask·scope는 AssetManager).
struct AssetLoadRequest {
    AssetId id{};
    LoadPriority priority = LoadPriority::Normal;
    CancelToken cancel;
};

// 요청 기록의 handle이다(번호·세대 — 돌려준 뒤에는 StaleHandle).
struct AssetLoadHandle {
    static constexpr uint32 NoIndex = NumericLimits<uint32>::Max();

    uint32 index = NoIndex;
    uint32 generation = 0;

    bool IsValid() const noexcept {
        return index != NoIndex;
    }
};

enum class AssetLoadState : uint8 {
    Pending,
    Succeeded,
    Failed,
    Cancelled
};

// 요청 하나의 결과다.
struct AssetLoadStatus {
    AssetLoadState state = AssetLoadState::Pending;
    // Succeeded·Pending이면 None이다.
    ErrorCode error = ErrorCode::None;
    AssetDiagnostic diagnostic;
    // Succeeded면 공개된 Package handle이다.
    ObjectHandle package{};
};

// Pump 예산이다(11.6절 10). 단위 사이에서 검사하고, 준비된 단위가 있으면 적어도 하나를 공개한다.
struct AssetLoaderPumpBudget {
    static constexpr uint32 UnlimitedUnits = NumericLimits<uint32>::Max();
    static constexpr uint64 UnlimitedNanoseconds = NumericLimits<uint64>::Max();

    uint32 maxUnits = UnlimitedUnits;
    uint64 maxNanoseconds = UnlimitedNanoseconds;
    // PostLoad 호출 수 상한이다(시간 예산은 공개와 같이 쓴다, 11.6절 16).
    uint32 maxPostLoads = UnlimitedUnits;
};

struct AssetLoaderPumpResult {
    // 이번 Pump가 처리한 IO·worker 완료 수다.
    uint32 completions = 0;
    // 허가한 단위 수다.
    uint32 admittedUnits = 0;
    // 공개를 시도한 단위 수다(실패 포함).
    uint32 instantiatedUnits = 0;
    // 부른 PostLoad 수다.
    uint32 postLoads = 0;
    // 예산 때문에 남긴 공개 준비 단위가 있는지다.
    bool budgetExhausted = false;
};

// 상태 관측(11.6절 12)의 표다.
enum class AssetLoaderUnitPhase : uint8 {
    // 허가 대기다.
    Queued,
    // 허가됐고 패키지가 IO·worker에 있거나 의존 단위를 기다린다.
    Loading,
    // 패키지가 모두 Prepared이고 의존 단위도 공개돼 Pump를 기다린다.
    Ready,
    // 관심이 0이 돼 버렸고 진행 중 IO·worker가 끝나기를 기다린다.
    Abandoned,
    // 실패했고 진행 중 IO·worker가 끝나기를 기다린다.
    Failed,
    // 공개했고 PostLoad를 기다리거나 하는 중이다(요청은 PostLoad가 끝나야 Succeeded).
    PostLoading
};

enum class AssetLoaderPackagePhase : uint8 {
    Queued,
    Reading,
    Preparing,
    Prepared,
    Count
};

inline constexpr uint32 AssetLoaderPackagePhaseCount =
    static_cast<uint32>(AssetLoaderPackagePhase::Count);

// 단위가 지금 기다리는 것이다.
enum class AssetLoaderWait : uint8 {
    None,
    // 동시 진행·임시 메모리 상한(또는 앞선 후보)에 걸려 허가를 기다린다.
    Admission,
    // 의존 단위(waitingOn)의 허가를 기다린다.
    DependencyAdmission,
    // 의존 단위(waitingOn)의 공개를 기다린다.
    Dependency,
    Io,
    Worker,
    // 공개 준비가 끝나 Pump 예산을 기다린다.
    Pump,
    // 의존 단위(waitingOn)의 PostLoad를 기다린다.
    PostLoadDependency,
    // 자기 PostLoad 차례(Pump 예산)를 기다린다.
    PostLoad
};

struct AssetLoaderRequestInfo {
    AssetLoadHandle handle;
    AssetId asset{};
    LoadPriority priority = LoadPriority::Normal;
    AssetLoadState state = AssetLoadState::Pending;
    ErrorCode error = ErrorCode::None;
    AssetDiagnostic diagnostic;
    // Pending이면 단위 번호, 아니면 NoIndex다.
    uint32 unit = AssetLoadHandle::NoIndex;
};

struct AssetLoaderUnitInfo {
    uint32 unit = 0;
    // 구성 패키지 중 AssetId가 가장 작은 것이다(단위를 가리키는 이름).
    AssetId asset{};
    AssetLoaderUnitPhase phase = AssetLoaderUnitPhase::Queued;
    LoadPriority priority = LoadPriority::Normal;
    LoadPriority effectivePriority = LoadPriority::Normal;
    uint32 packages = 0;
    uint32 preparedPackages = 0;
    uint32 interest = 0;
    AssetLoaderWait waiting = AssetLoaderWait::None;
    // waiting이 의존이면 그 단위의 첫 패키지 에셋이다.
    AssetId waitingOn{};
    uint64 tempBytes = 0;
    // phase가 Failed면 처음 실패한 패키지·오류·진단이다(11.6절 12).
    AssetId failedAsset{};
    ErrorCode failError = ErrorCode::None;
    AssetDiagnostic failDiagnostic;
};

struct AssetLoaderPackageInfo {
    AssetId asset{};
    uint32 unit = 0;
    AssetLoaderPackagePhase phase = AssetLoaderPackagePhase::Queued;
    uint64 tempBytes = 0;
    // 단계별 누적 시간이다(지금 단계는 조회 시각까지 포함).
    uint64 phaseNanoseconds[AssetLoaderPackagePhaseCount] = {};
};

struct AssetLoaderSnapshot {
    // 남기는 최근 동기 대기 수다(11.5절 3의 강제 대기 위치).
    static constexpr uint32 RecentSyncWaitCapacity = 8;

    explicit AssetLoaderSnapshot(Allocator& allocator) noexcept
        : requests(allocator), units(allocator), packages(allocator) {
    }

    Array<AssetLoaderRequestInfo> requests;
    Array<AssetLoaderUnitInfo> units;
    Array<AssetLoaderPackageInfo> packages;
    uint32 loadedPackages = 0;
    uint32 packagesInFlight = 0;
    uint64 tempBytesInFlight = 0;
    uint64 syncWaits = 0;
    uint64 syncWaitNanoseconds = 0;
    // 최근 동기 대기(오래된 것부터 recentSyncWaitCount개)다.
    AssetLoaderSyncWait recentSyncWaits[RecentSyncWaitCapacity] = {};
    uint32 recentSyncWaitCount = 0;
};

// 이름이다(로그·도구용, 정적 수명). 모르는 값은 "Unknown"이다.
const char* LoadPriorityName(LoadPriority priority) noexcept;
const char* AssetLoadStateName(AssetLoadState state) noexcept;
const char* AssetLoaderUnitPhaseName(AssetLoaderUnitPhase phase) noexcept;
const char* AssetLoaderPackagePhaseName(AssetLoaderPackagePhase phase) noexcept;
const char* AssetLoaderWaitName(AssetLoaderWait wait) noexcept;

class AssetLoader final : public PackageImportResolver {
public:
    explicit AssetLoader(const AssetLoaderConfig& config) noexcept;
    // Shutdown하지 않았으면 Shutdown한다. 초기화된 로더를 소유 스레드가 아닌 곳에서 파괴하면
    // platform::Fatal이다(진행 중 IO·worker를 남긴 채 상태를 지우는 수명 오류).
    ~AssetLoader();
    AssetLoader(const AssetLoader&) = delete;
    AssetLoader& operator=(const AssetLoader&) = delete;

    // 설정을 확인하고 경로 색인·항목 표를 만든다. 이 함수를 부른 스레드가 소유 스레드가 된다.
    //
    // 오류: null 포인터·0 한도·패키지 정책이 아닌 Registry·Registry와 다른 TypeRegistry는
    // InvalidArgument, 두 번째 호출은 InvalidArgument, 매니페스트 규칙 위반은
    // ValidateCookedManifest의 진단(CorruptPackage / InvalidArgument), 매니페스트 profileId ≠
    // runtimeProfile은
    // IncompatibleCook / Unsupported(section = ProfileId), 할당 실패는 그 오류(진단 None)다.
    Result<void> Initialize(AssetDiagnostic* failure = nullptr) noexcept;

    // 요청을 낸다. 이미 공개한 패키지면 바로 Succeeded이고, 진행 중 단위에 닿으면 합류한다.
    //
    // 오류: 초기화 전 InvalidArgument, Shutdown 뒤 ShuttingDown, 다른 스레드 WrongThread, 범위 밖
    // priority InvalidArgument, 매니페스트에 없는 id NotFound(진단 MissingDependency, asset = id),
    // 할당 실패는 그 오류(진단 None — 로더 상태는 요청 전과 같다).
    Result<AssetLoadHandle> Request(const AssetLoadRequest& request,
                                    AssetDiagnostic* failure = nullptr) noexcept;

    // 경로 힌트로 에셋을 찾는다(caseless — AS 4.2절 키). 오류: 경로 규칙 위반 InvalidArgument,
    // 없으면 NotFound, 초기화 전 InvalidArgument, 다른 스레드 WrongThread, 할당 실패는 그 오류다.
    Result<AssetId> FindByPath(StringView path) const noexcept;

    // 요청 결과다. 오류: 잘못된·돌려준 handle StaleHandle, 다른 스레드 WrongThread.
    Result<AssetLoadStatus> Status(AssetLoadHandle handle) const noexcept;

    // Pending 요청의 우선순위를 올린다(내리는 값은 요청 기록만 바꾸고 단위는 그대로 — 단위
    // 우선순위는 올라가기만 한다). 끝난 요청이면 아무것도 하지 않는다. 오류:
    // StaleHandle·WrongThread·범위 밖 priority InvalidArgument.
    Result<void> SetPriority(AssetLoadHandle handle, LoadPriority priority) noexcept;

    // 요청 기록을 돌려준다(Pending이면 취소와 같다 — 관심을 놓는다). 오류: StaleHandle·WrongThread.
    Result<void> Release(AssetLoadHandle handle) noexcept;

    // 완료 처리 → 취소 확인 → 허가 → 공개(예산 안)를 한다.
    // 오류: 초기화 전 InvalidArgument, 다른 스레드 WrongThread, Shutdown 뒤 ShuttingDown. 단위의
    // 실패는 그 요청들의 결과로 남고 Pump는 성공한다.
    Result<AssetLoaderPumpResult> Pump(const AssetLoaderPumpBudget& budget = {}) noexcept;

    // 완료 큐가 비지 않거나 진행 중 IO·worker가 없을 때까지 기다린다(시간 대기 없음 — 조건 변수).
    // 오류: 초기화 전 InvalidArgument, 다른 스레드 WrongThread.
    Result<void> WaitForCompletions() noexcept;

    // 요청이 끝날 때까지 Pump·WaitForCompletions를 되풀이하고 결과를 준다(도구·시험·초기화 전용 —
    // 11.6절 11). site는 호출 지점 정적 문자열이다. 오류: "프레임 실행 중"이면 WrongContext,
    // StaleHandle·WrongThread, Shutdown 뒤 ShuttingDown.
    Result<AssetLoadStatus> WaitFor(AssetLoadHandle handle, const char* site) noexcept;

    // 상태 표를 채운다(out의 배열은 비우고 다시 채운다). 오류: WrongThread, 할당 실패.
    Result<void> Describe(AssetLoaderSnapshot& out) const noexcept;

    // 공개한 패키지를 찾는다(InstantiatePackages의 묶음 밖 import 해석). 없으면 NotFound, 다른
    // 스레드 WrongThread다.
    Result<ObjectHandle> FindPackage(AssetId asset) noexcept override;

    // Pending 요청을 ShuttingDown으로 끝내고 미공개 단위를 버린 뒤 진행 중 IO·worker를 기다리고
    // 공개한 패키지의 RootRef를 놓는다. 두 번째 호출은 아무것도 하지 않는다. 오류: WrongThread.
    Result<void> Shutdown() noexcept;

private:
    struct Impl;

    AssetLoaderConfig config_;
    Impl* impl_ = nullptr;
};

} // namespace rocks
