#pragma once

#include <AssetNugget/NuggetCacheSettings.h>
#include <AssetNugget/NuggetFileSystemBackend.h>
#include <AssetNugget/NuggetPolicy.h>
#include <AssetNugget/NuggetScc.h>
#include <Core/Containers/HashMap.h>
#include <Core/Containers/SlotArray.h>
#include <Core/Memory/SharedPtr.h>
#include <Core/Memory/UniquePtr.h>
#include <Platform/Sync.h>
#include <Task/WorkerScheduler.h>

// 에셋 너겟 캐시다(Docs/Design/AssetSystem.ko.md 9.1절 결정 10·11·13–15, "A3c1 구현 세부").
// Memory 계층(프로세스 안) → backend 하나(FileSystem). 세션 고정 표가 한 세션이 쓴 Record digest를
// 지켜 같은 키가 세션 안에서 다른 값으로 바뀌어 보이지 않게 한다(DigestChanged), 적중한 Record의
// 소비 입력을 고정 표와 대조한다.
//
// Get·Put은 실패를 Result로 돌려주지 않는다 — 캐시 실패는 빌드 실패가 아니다(R-1). 결과의
// `failed`가 backend 실패이고 `required` 설정일 때 요청 객체(A3c2)가 요청 오류로 올린다.
//
// 스레드: Open은 한 스레드에서 한 번. 그 뒤 Get·Put·GetSccMember·PutScc·Stats는 여러 스레드에서
// 동시에 불러도 된다(내부 표는 mutex 하나로 지키고 backend IO는 잠금 밖에서 한다). allocator는 동시
// 할당을 지원해야 한다(System·Binned 등).
//
// 사용 예:
//   NuggetSystemClock clock;
//   NuggetCache cache(allocator, platform::PhysicalFileSystem(), clock);
//   auto settings = ReadNuggetCacheSettings(allocator, files, projectRoot, nullptr);
//   if (auto opened = cache.Open(settings.Value(), projectRoot); !opened) { ... }
//   NuggetCacheGetResult got = cache.Get(NuggetCacheGet{NuggetBucketTexture, key});
//   if (got.status == NuggetStatus::Hit) { /* got.values */ }
//   else { /* 만들고 */ cache.Put(NuggetCachePut{NuggetBucketTexture, key, values}); }
namespace rocks {

// Memory 계층·고정 보관 기본 상한과 항목 크기 계산 상수다(9.1절 결정 10·11).
inline constexpr usize NuggetMemoryBytes = 256ull * 1024 * 1024;
inline constexpr usize NuggetMemoryValueBytes = 64ull * 1024 * 1024;
inline constexpr usize NuggetPinnedBytes = 256ull * 1024 * 1024;
inline constexpr usize NuggetMemoryEntryBytes = 256;
inline constexpr usize NuggetMemoryInputBytes = 128;
// 진행 중 Put 요청 항목 값 byte 합의 기본 상한이다(9.1절 결정 12).
inline constexpr uint64 NuggetPendingPutBytes = 1024ull * 1024 * 1024;

class NuggetRequest;

// 상한이다(시험은 작게 준다).
struct NuggetCacheLimits {
    usize memoryBytes = NuggetMemoryBytes;
    usize memoryValueBytes = NuggetMemoryValueBytes;
    usize pinnedBytes = NuggetPinnedBytes;
    uint64 pendingPutBytes = NuggetPendingPutBytes;
};

// 시험 hook의 Shutdown 지점이다(제품 코드는 hook을 걸지 않는다).
enum class NuggetCacheStep : uint8 {
    // 새 Start를 막고 진행 중 Get 요청에 취소를 요청한 직후.
    AfterCancelGets,
    // Put 요청을 drainMs까지 기다리고 남은 Put 요청에 취소를 요청한 직후.
    AfterDrain
};

struct NuggetCacheHooks {
    void (*callback)(void* context, NuggetCacheStep step) noexcept = nullptr;
    void* context = nullptr;
};

// 적중한 곳이다.
enum class NuggetSource : uint8 {
    None,
    Memory,
    Local
};

// Get 항목이다. values가 비면 전체다.
struct NuggetCacheGet {
    StringView bucket{};
    Sha256Digest key{};
    Span<const uint32> values{};
    NuggetPolicy policy = NuggetPolicy::Default;
};

struct NuggetCacheGetResult {
    explicit NuggetCacheGetResult(Allocator& allocator) noexcept
        : record(allocator), values(allocator) {
    }

    NuggetStatus status = NuggetStatus::Miss;
    // 원인 오류다(정상 적중·미적중은 None).
    ErrorCode error = ErrorCode::None;
    // backend 실패(IO·할당 등 — NuggetCache::IsFailure)인 Miss다.
    bool failed = false;
    NuggetSource source = NuggetSource::None;
    Sha256Digest digest{};
    NuggetRecord record;
    // 청한 순서의 값(원본)이다. SkipData·DigestChanged·Miss면 비어 있다.
    Array<NuggetValue> values;
};

// Put 항목이다. values는 ValueId 오름차순이고 Put이 bytes를 Memory 계층으로 옮겨 갈 수 있다(소유권
// 이전 — 호출 뒤 bytes가 비어 있을 수 있다). inputs는 정규 순서(NuggetInput 비교 — 위반은 Dropped).
struct NuggetCachePut {
    StringView bucket{};
    Sha256Digest key{};
    Span<NuggetValue> values{};
    Span<const NuggetInput> inputs{};
    Span<const uint8> meta{};
    NuggetPolicy policy = NuggetPolicy::Default;
};

struct NuggetCachePutResult {
    NuggetStatus status = NuggetStatus::Dropped;
    ErrorCode error = ErrorCode::None;
    // backend 실패(IO·할당 등 — NuggetCache::IsFailure)인 Dropped다.
    bool failed = false;
    Sha256Digest digest{};
    Sha256Digest existing{};
};

// 통계다(누적 — 결정성 gate의 근거: Conflict 0건).
struct NuggetCacheStats {
    uint64 memoryHits = 0;
    uint64 localHits = 0;
    uint64 partials = 0;
    uint64 misses = 0;
    uint64 digestChanged = 0;
    uint64 inputMismatches = 0;
    uint64 corrupts = 0;
    uint64 stored = 0;
    uint64 exists = 0;
    uint64 conflicts = 0;
    uint64 dropped = 0;
    uint64 uncachedKeys = 0;
    // 지금 값이다.
    uint64 memoryBytes = 0;
    uint64 pinnedBytes = 0;
    uint64 pinnedKeys = 0;
};

class ROCKS_ASSETNUGGET_API NuggetCache {
public:
    // files·clock은 캐시보다 오래 살아야 한다.
    NuggetCache(Allocator& allocator, platform::FileSystem& files, NuggetClock& clock,
                const NuggetCacheLimits& limits = {}) noexcept;
    NuggetCache(const NuggetCache&) = delete;
    NuggetCache& operator=(const NuggetCache&) = delete;
    // 진행 중 요청이 있으면 Fatal이다(요청이 이 캐시를 가리킨다 — Shutdown으로 먼저 끝낸다).
    ~NuggetCache() noexcept;

    // 설정대로 backend를 연다(결정은 이때 한 번). off는 backend 없음, rockpile은 Unsupported,
    // auto·filesystem은 FileSystem backend(localPath — ResolveNuggetLocalPath). FileSystem 열기
    // 실패는 required면 그 오류, 아니면 off로 내리고 OpenError()에 남긴다. 오류: 두 번째 Open
    // InvalidArgument, 할당 실패.
    Result<void> Open(const NuggetCacheSettings& settings, StringView projectRoot) noexcept;

    bool IsRequired() const noexcept {
        return required_;
    }

    // backend가 있으면 true다(off·열기 실패로 내렸으면 false).
    bool IsEnabled() const noexcept {
        return backend_.Get() != nullptr;
    }

    // 열기 실패로 off로 내린 원인이다(아니면 None).
    ErrorCode OpenError() const noexcept {
        return openError_;
    }

    // 키 공통 머리의 프로젝트 세대다(WriteNuggetKeyHeader).
    Guid ProjectGeneration() const noexcept {
        return generation_;
    }

    NuggetCacheGetResult Get(const NuggetCacheGet& request) noexcept;
    NuggetCachePutResult Put(const NuggetCachePut& request) noexcept;

    // SCC Record Put이다(meta를 구성원 목록으로 채운다 — request.meta는 무시). 구성원이
    // NuggetSccMaxMembers를 넘으면 Dropped(CapacityExceeded — 캐시하지 않음, 실패 아님), 값 번호의
    // 구성원 순번이 구성원 수 이상이거나 구성원 목록이 잘못되면 Dropped(InvalidArgument).
    NuggetCachePutResult PutScc(Span<const AssetId> members,
                                const NuggetCachePut& request) noexcept;

    // SCC Record에서 구성원 asset의 값만 Get한다(meta로 순번을 찾아 그 순번의 값을 고른다).
    // 구성원이 없으면 Miss(NotFound), 그 순번의 값이 없으면 빈 Hit다. policy의 SkipData·SkipMeta·
    // PartialRecord는 두 번째 Get에 쓴다.
    NuggetCacheGetResult GetSccMember(StringView bucket, const Sha256Digest& sccKey, AssetId asset,
                                      NuggetPolicy policy = NuggetPolicy::Default) noexcept;

    NuggetCacheStats Stats() const noexcept;

    // 종료한다(9.1절 결정 12): 새 요청 Start를 막고(이후 Start는 모든 항목 Cancelled) 진행 중 Get
    // 요청에 취소를 요청한 뒤, Put 요청은 drainMs(전체 기한)까지 완료를 기다리고 남은 Put 요청에
    // 취소를 요청한 다음 모든 요청이 끝날 때까지 기다린다. 이후 동기 Get·Put은 Cancelled다. 두 번째
    // 호출은 아무것도 하지 않는다. 진행 중 요청의 scheduler worker에서 부르면 Fatal(기다리면 교착).
    void Shutdown(uint32 drainMs) noexcept;

    bool IsShutDown() const noexcept;

    // 시험 hook이다.
    void SetHooks(const NuggetCacheHooks& hooks) noexcept {
        hooks_ = hooks;
    }

    // 결과 오류가 backend 실패(failed — required 요청의 오류)인지다. 아닌 것: None,
    // Unsupported(다른 버전·읽기 전용), CapacityExceeded(한도 위반 — 캐시하지 않음, 9.1절 결정 7),
    // Busy(일시 경합 — 손상 교체 잠금 대기 초과·재게시 실패).
    static bool IsFailure(ErrorCode error) noexcept;

private:
    // 세션 고정 상태다(9.1절 결정 11).
    enum class PinState : uint8 {
        Normal,
        Changed,
        Regenerated,
        Uncached
    };

    struct Pin {
        Sha256Digest digest{};
        PinState state = PinState::Normal;
    };

    // Memory가 쥔 값 하나다(공유 버퍼 — 잠금 안에서는 참조만 얻고 복사는 잠금 밖에서 한다).
    struct HeldValue {
        uint32 id = 0;
        SharedPtr<Array<uint8>> bytes;
    };

    // Memory 항목이다(LRU 이중 연결 — newer는 더 최근, older는 더 오래된 쪽). Record도 공유한다.
    struct MemoryEntry {
        explicit MemoryEntry(Allocator& allocator) noexcept : bucket(allocator), values(allocator) {
        }

        Sha256Digest key{};
        String bucket;
        Sha256Digest digest{};
        SharedPtr<NuggetRecord> record;
        Array<HeldValue> values;
        usize bytes = 0;
        bool pinned = false;
        SlotHandle newer{};
        SlotHandle older{};
    };

    // Memory 적중을 잠금 안에서 본 결과다(Record·값은 참조).
    struct MemoryView {
        explicit MemoryView(Allocator& allocator) noexcept : values(allocator) {
        }

        bool found = false;
        NuggetStatus status = NuggetStatus::Miss;
        ErrorCode error = ErrorCode::None;
        Sha256Digest digest{};
        SharedPtr<NuggetRecord> record;
        // 청한 순서의 값이다.
        Array<HeldValue> values;
    };

    // 세션 고정 판정이다.
    enum class Verdict : uint8 {
        Accept,
        DigestChanged,
        InputMismatch,
        Uncached,
        OutOfMemory
    };

    using PinMap = HashMap<Sha256Digest, Pin, SeededHash<Sha256Digest>>;
    using EntryIndex = HashMap<Sha256Digest, SlotHandle, SeededHash<Sha256Digest>>;

    // Get·Put의 본체다. forRequest면 종료 뒤에도 실행한다(요청 항목 — drain).
    NuggetCacheGetResult Fetch(const NuggetCacheGet& request, bool forRequest) noexcept;
    NuggetCachePutResult Store(const NuggetCachePut& request, bool forRequest) noexcept;
    bool LookupMemory(const NuggetCacheGet& request, MemoryView& view) noexcept;
    Verdict Judge(const Sha256Digest& key, const Sha256Digest& digest,
                  Span<const NuggetInput> inputs) noexcept;
    void Insert(MemoryEntry&& candidate) noexcept;
    void Settle(const NuggetCachePut& request, const Sha256Digest& digest, bool planned,
                bool regenerated, MemoryEntry&& prepared, bool preparedAll) noexcept;
    usize EntryBytes(const NuggetRecord& record, Span<const HeldValue> values) const noexcept;
    void Link(SlotHandle handle) noexcept;
    void Unlink(SlotHandle handle) noexcept;
    void Evict(SlotHandle handle) noexcept;
    void EvictKey(const Sha256Digest& key) noexcept;
    void Trim() noexcept;
    void Count(uint64& counter) noexcept;

    // 요청(NuggetRequest)이 쓴다: 진행 중 목록 등록·해제(종료 뒤 등록은 false), 대기 Put byte
    // 예약·반환.
    friend class NuggetRequest;
    bool Register(NuggetRequest& request) noexcept;
    void Unregister(NuggetRequest& request) noexcept;
    bool ReservePutBytes(uint64 bytes) noexcept;
    void ReleasePutBytes(uint64 bytes) noexcept;
    // 진행 중 목록에서 조건에 맞는 첫 요청의 완료 handle 복제다(없으면 빈 handle).
    WorkerTaskHandle FirstPending(bool putOnly) noexcept;
    void Notify(NuggetCacheStep step) noexcept {
        if (hooks_.callback != nullptr) {
            hooks_.callback(hooks_.context, step);
        }
    }

    Allocator* allocator_;
    platform::FileSystem* files_;
    NuggetClock* clock_;
    NuggetCacheLimits limits_;
    UniquePtr<NuggetFileSystemBackend> backend_;
    bool opened_ = false;
    bool required_ = false;
    ErrorCode openError_ = ErrorCode::None;
    Guid generation_{};

    mutable platform::Mutex mutex_;
    PinMap pins_;
    EntryIndex index_;
    SlotArray<MemoryEntry> entries_;
    SlotHandle newest_{};
    SlotHandle oldest_{};
    usize memoryBytes_ = 0;
    usize pinnedBytes_ = 0;
    NuggetCacheStats stats_{};

    // 요청 목록(이중 연결)·종료 표지·대기 Put byte다. 목록은 requestMutex_로 지킨다.
    platform::Mutex requestMutex_;
    NuggetRequest* requests_ = nullptr;
    uint32 shutDown_ = 0;
    uint64 pendingPutBytes_ = 0;
    NuggetCacheHooks hooks_{};
};

} // namespace rocks
