#include <Core/Memory/BinnedAllocator.h>

#include "AddressLedger.h"
#include "BinnedAllocatorTestHooks.h"

#include <Base/Checked.h>
#include <Core/Move.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <Platform/PlatformMath.h>
#include <Platform/SecureRandom.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>
#include <Platform/VirtualMemory.h>

// 구조와 근거는 Docs/Design/GarbageCollectionScale.ko.md 18절이다.
namespace rocks {
namespace {

using platform::MemoryOrder;

// ---- slab과 블록의 배치 ----

constexpr usize SlabShift = 16;
constexpr usize SlabBytes = BinnedAllocator::SlabBytes;
static_assert(SlabBytes == usize{1} << SlabShift, "slab size must match the slab shift");

// slab 앞의 머리 크기다. 블록은 그 뒤를 채운다. 64의 배수라 블록 크기가 64의 배수인 등급은 블록
// 주소가 64로 정렬된다.
constexpr usize SlabHeaderBytes = 64;
// 모든 등급 크기의 약수이자 가장 작은 블록·기본 정렬이다(자유 블록의 word 둘: 연결과 반환 표지).
constexpr usize BlockGranularity = 16;
constexpr usize BlockGranularityShift = 4;
static_assert(BlockGranularity == usize{1} << BlockGranularityShift,
              "block granularity must match its shift");
static_assert(BlockGranularity >= 2 * sizeof(uptr),
              "a free block must hold the link word and the return marker");

// ---- 크기 등급(18.5절) ----

// 16, 32, …, 128(16 단계 8개), 그 위는 2배 구간마다 넷이다(160·192·224·256, …, 8192).
constexpr uint32 LinearClassCount = 8;
constexpr usize LinearClassLimit = LinearClassCount * BlockGranularity;
constexpr uint32 ClassesPerDoubling = 4;
constexpr uint32 DoublingCount = 6;
constexpr uint32 ClassCount = LinearClassCount + ClassesPerDoubling * DoublingCount;
// 크기 → 등급 표의 칸 수다. (bytes + 15) >> 4로 색인한다(1..512, 0번 칸은 쓰지 않는다).
constexpr usize SizeIndexCount = (BinnedAllocator::MaxSmallBytes >> BlockGranularityShift) + 1;
// 정렬 단계다: 16 이하, 32, 64.
constexpr uint32 AlignmentStepCount = 3;
// 스레드 캐시 묶음의 한도다: 64개 또는 64KB 가운데 작은 쪽이고 등급으로 정해진다(8192 등급은 8개).
constexpr uint32 MaxBundleBlocks = 64;
constexpr usize MaxBundleBytes = usize{64} << 10;
// 등급마다 가득 찬 묶음을 스레드 사이에 넘기는 칸 수다(포인터 여덟 = 64 B).
constexpr uint32 TransferSlotCount = 8;
// 블록 번호 = (offset / 16 × 역수) >> ReciprocalShift다. offset / 16은 4096 미만이고 등급 크기 /
// 16은 512 이하라 24비트 역수(올림)로 몫이 정확하다(오차 q·e / (s·2^24) < 2^-12 < 1 / s).
constexpr uint32 ReciprocalShift = 24;

constexpr usize ClassBlockBytes(uint32 sizeClass) noexcept {
    if (sizeClass < LinearClassCount) {
        return (usize{sizeClass} + 1) * BlockGranularity;
    }

    const uint32 doubling = (sizeClass - LinearClassCount) / ClassesPerDoubling;
    const uint32 step = (sizeClass - LinearClassCount) % ClassesPerDoubling;
    const usize base = LinearClassLimit << doubling;
    return base + (base / ClassesPerDoubling) * (usize{step} + 1);
}

struct ClassTables {
    uint16 blockBytes[ClassCount]{};
    // slab 하나에 드는 블록 수다.
    uint16 capacity[ClassCount]{};
    uint32 reciprocal[ClassCount]{};
    // 묶음 하나의 블록 수다.
    uint8 bundleBlocks[ClassCount]{};
    uint8 classOfSize[AlignmentStepCount][SizeIndexCount]{};
};

constexpr ClassTables BuildClassTables() noexcept {
    ClassTables tables{};
    for (uint32 sizeClass = 0; sizeClass < ClassCount; ++sizeClass) {
        const usize bytes = ClassBlockBytes(sizeClass);
        const usize units = bytes >> BlockGranularityShift;
        tables.blockBytes[sizeClass] = static_cast<uint16>(bytes);
        tables.capacity[sizeClass] = static_cast<uint16>((SlabBytes - SlabHeaderBytes) / bytes);
        tables.reciprocal[sizeClass] =
            static_cast<uint32>(((usize{1} << ReciprocalShift) + units - 1) / units);
        const usize byBytes = MaxBundleBytes / bytes;
        tables.bundleBlocks[sizeClass] =
            static_cast<uint8>(byBytes < MaxBundleBlocks ? byBytes : MaxBundleBlocks);
    }

    for (uint32 step = 0; step < AlignmentStepCount; ++step) {
        // 정렬이 16을 넘으면 블록 크기가 그 정렬의 배수인 첫 등급으로 올린다(머리 64 B와 블록
        // 크기가 모두 배수라 블록 주소가 정렬된다 — UE PromoteToLargerBin).
        const usize alignment = BlockGranularity << step;
        uint32 sizeClass = 0;
        for (usize index = 1; index < SizeIndexCount; ++index) {
            const usize bytes = index << BlockGranularityShift;
            while (tables.blockBytes[sizeClass] < bytes) {
                ++sizeClass;
            }

            uint32 promoted = sizeClass;
            while (tables.blockBytes[promoted] % alignment != 0) {
                ++promoted;
            }

            tables.classOfSize[step][index] = static_cast<uint8>(promoted);
        }
    }

    return tables;
}

constexpr ClassTables Classes = BuildClassTables();
static_assert(Classes.blockBytes[0] == BlockGranularity, "the smallest class must be 16 bytes");
static_assert(Classes.blockBytes[LinearClassCount - 1] == LinearClassLimit,
              "the linear classes must end at 128 bytes");
static_assert(Classes.blockBytes[ClassCount - 1] == BinnedAllocator::MaxSmallBytes,
              "the largest class must be MaxSmallBytes");
static_assert(BinnedAllocator::MaxSmallAlignment == BlockGranularity << (AlignmentStepCount - 1),
              "the alignment steps must end at MaxSmallAlignment");
static_assert(SlabHeaderBytes % BinnedAllocator::MaxSmallAlignment == 0,
              "the slab header must keep promoted blocks aligned");
static_assert(Classes.bundleBlocks[0] == MaxBundleBlocks, "small classes must use full bundles");
static_assert(Classes.bundleBlocks[ClassCount - 1] ==
                  MaxBundleBytes / BinnedAllocator::MaxSmallBytes,
              "the largest class bundle must be bounded by bytes");

constexpr uint32 AlignmentStepOf(usize alignment) noexcept {
    return alignment <= BlockGranularity ? 0u : (alignment == 2 * BlockGranularity ? 1u : 2u);
}

constexpr uint32 ClassOfRequest(usize bytes, usize alignment) noexcept {
    return Classes.classOfSize[AlignmentStepOf(alignment)]
                              [(bytes + BlockGranularity - 1) >> BlockGranularityShift];
}

// slab 안 offset이 sizeClass 블록의 시작이면 그 번호를 쓰고 참이다. 머리 안·16의 배수가 아닌
// offset·블록 가운데는 거짓이다. 주소를 읽지 않는다.
constexpr bool BlockIndexOfOffset(uint32 sizeClass, usize offsetInSlab,
                                  uint32& blockIndex) noexcept {
    if (offsetInSlab < SlabHeaderBytes) {
        return false;
    }

    const usize offset = offsetInSlab - SlabHeaderBytes;
    if ((offset & (BlockGranularity - 1)) != 0) {
        return false;
    }

    const uint64 units = offset >> BlockGranularityShift;
    const uint64 index = (units * Classes.reciprocal[sizeClass]) >> ReciprocalShift;
    if (index * (uint64{Classes.blockBytes[sizeClass]} >> BlockGranularityShift) != units) {
        return false;
    }

    blockIndex = static_cast<uint32>(index);
    return true;
}

// ---- slab ----

// slab 표의 "쓰지 않는 slab" 값이다. 쓰는 slab은 등급 + 1이다.
constexpr uint8 UnusedSlab = 0;
constexpr usize BitsPerWord = 64;

// slab 앞 64 B의 머리다. 그 등급의 잠금 안에서만 쓴다. carvedBlocks는 스레드 캐시 경로의 반환이
// 잠금 없이 읽는다(원자 접근).
struct SlabHeader {
    // 등급의 "내줄 블록이 있는 slab" 목록(이중 연결) 또는 빈 slab 보관소(단일 연결)의 연결이다.
    SlabHeader* next = nullptr;
    SlabHeader* previous = nullptr;
    // 돌려받은 블록의 목록이다(블록의 첫 word로 잇는다).
    void* freeList = nullptr;
    // slab 밖에 나가 있는 블록 수다(사용자가 쥔 블록 + 스레드 캐시·이전 칸에 든 블록). 0이 되면
    // slab을 푼다 — 캐시에 든 블록이 있는 slab은 풀리지 않는다.
    uint32 usedBlocks = 0;
    // 앞에서부터 떼어 낸 블록 수다(bump). 이 번호 이상의 블록은 내준 적이 없다.
    uint32 carvedBlocks = 0;
    uint32 capacity = 0;
    uint32 blockBytes = 0;
    bool listed = false;
};

static_assert(sizeof(SlabHeader) <= SlabHeaderBytes, "slab header does not fit its reserved bytes");

// 자유 블록의 word 둘이다. 살아 있는 블록에서는 사용자 데이터다.
struct FreeBlock {
    void* next;
    uptr marker;
};

static_assert(sizeof(FreeBlock) == BlockGranularity,
              "free block words must fill the smallest block");

void* ReadLink(void* block) noexcept {
    return static_cast<FreeBlock*>(block)->next;
}

void WriteLink(void* block, void* next) noexcept {
    static_cast<FreeBlock*>(block)->next = next;
}

void WriteMarker(void* block, uptr marker) noexcept {
    static_cast<FreeBlock*>(block)->marker = marker;
}

// 반환하는 블록에 표지를 세운다. 이미 서 있으면(중복 반환, 내준 적 없는 블록) 거짓이다. 비교
// 교환이라 같은 블록을 두 스레드가 함께 반환해도 한쪽만 참이다.
bool TryMarkReturned(void* block, uptr marker) noexcept {
    uptr* word = &static_cast<FreeBlock*>(block)->marker;
    uptr current = platform::AtomicLoad<MemoryOrder::Relaxed>(word);
    do {
        if (current == marker) {
            return false;
        }
    } while (
        !platform::AtomicCompareExchangeWeak<MemoryOrder::AcquireRelease, MemoryOrder::Relaxed>(
            word, current, marker));

    return true;
}

// 사용자에게 내주는 블록의 둘째 word 값이다(표지 없음).
constexpr uptr NoMarker = 0;
// 비밀값의 최상위 비트를 세운다: 표지가 0·작은 정수·사용자 주소와 같아질 수 없다(그런 값을 둘째
// word에 둔 살아 있는 블록의 반환이 NotFound가 되지 않는다).
constexpr uptr SecretHighBit = uptr{1} << 63;

// ---- 시험 주입(이 스레드에서 다음에 만드는 인스턴스에 적용) ----

struct ConstructionFaults {
    uint32 reserveFailures = 0;
    bool tableAllocationFails = false;
};

thread_local ConstructionFaults constructionFaults;

// 등급 하나의 상태다. transfer 말고는 mutex 안에서 읽고 쓴다.
struct ClassState {
    platform::Mutex mutex;
    // 내줄 블록이 있는 slab의 목록이다(돌려받은 블록이 있거나 아직 다 떼지 않은 slab).
    SlabHeader* available = nullptr;
    // 사용자에게 내준·돌려받은 누적 블록 수다(캐시 없는 경로의 몫 + 끝난 스레드 캐시에서 옮긴 몫).
    uint64 allocated = 0;
    uint64 freed = 0;
    // 이전 칸이다: 가득 찬 묶음(블록 수는 그 등급의 한도)의 첫 블록 또는 null. 넣기는 빈 칸에 비교
    // 교환, 꺼내기는 null과 교환이다(묶음을 통째로 주고받아 ABA가 없다).
    alignas(TransferSlotCount * sizeof(void*)) void* transfer[TransferSlotCount]{};
};

// 스레드 캐시의 묶음이다(블록의 첫 word로 이은 목록).
struct Bundle {
    void* head = nullptr;
    uint32 count = 0;
};

// 스레드 캐시의 등급 하나다(한 캐시 줄).
struct alignas(64) ClassCache {
    // 할당이 꺼내고 반환이 넣는 묶음이다.
    Bundle partial;
    // 비었거나 가득 찬(블록 수 = 등급의 한도) 예비 묶음이다.
    Bundle full;
    // 이 스레드가 이 등급에서 내준·돌려받은 누적 수다. 소유 스레드만 쓰고 Stats가 읽는다(원자
    // 접근).
    uint64 allocated = 0;
    uint64 freed = 0;
};

// 스레드 하나 × 인스턴스 하나의 캐시다. 인스턴스가 소유하고(등록 목록) 소유 스레드만 묶음을 만진다.
struct ThreadCache {
    // 인스턴스의 캐시 목록이다(등록부 잠금).
    ThreadCache* next = nullptr;
    ThreadCache* previous = nullptr;
    ClassCache classes[ClassCount];
};

// 스레드의 기록이다: 인스턴스 번호 → 그 인스턴스의 이 스레드 캐시. 스레드 종료 hook이 해제한다.
struct ThreadRecord {
    struct Entry {
        uint64 id = 0;
        ThreadCache* cache = nullptr;
    };

    // 마지막에 쓴 인스턴스다(빠른 경로가 먼저 본다). 0은 "없음"이다(인스턴스 번호는 1부터다).
    uint64 lastId = 0;
    ThreadCache* lastCache = nullptr;
    Entry* entries = nullptr;
    uint32 count = 0;
    uint32 capacity = 0;
    // 스레드 종료 hook이 지나간 스레드다(캐시를 다시 만들지 않는다).
    bool exited = false;
};

constexpr uint32 InitialRecordEntries = 4;

// 소멸자 없는 단순 포인터다. null이면 이 스레드는 아직 캐시를 만든 적이 없다.
thread_local ThreadRecord* threadRecord = nullptr;
// 스레드 종료 hook이 지나간 스레드가 가리키는 기록이다(읽기만 한다 — lastId 0이라 어느 인스턴스와도
// 맞지 않는다).
constinit ThreadRecord exitedRecord{0, nullptr, nullptr, 0, 0, true};

// 시험 주입: 이 스레드의 다음 캐시 만들기를 실패시킨다.
thread_local bool failNextCacheCreation = false;

} // namespace

struct BinnedAllocator::State {
    explicit State(Allocator& backingAllocator) noexcept : backing(backingAllocator) {
    }

    void ReserveRange(usize requestedBytes) noexcept;
    void ReleaseRange() noexcept;

    SlabHeader* SlabAt(usize slabIndex) const noexcept {
        return reinterpret_cast<SlabHeader*>(base + (slabIndex << SlabShift));
    }

    usize SlabIndexOf(const void* address) const noexcept {
        return (reinterpret_cast<uptr>(address) - base) >> SlabShift;
    }

    uptr MarkerOf(const void* block) const noexcept {
        return reinterpret_cast<uptr>(block) ^ secret;
    }

    // 등급 잠금을 쥔 채 부른다(잠금 순서: 등급 → 범위).
    SlabHeader* AcquireSlab(uint32 sizeClass) noexcept;
    void ReleaseSlab(SlabHeader* slab) noexcept;
    // 범위 잠금 안에서 부른다. 실패는 일시적일 수 있다(다음 호출이 다시 시도한다).
    bool CommitSlab(usize slabIndex) noexcept;
    bool DecommitSlab(usize slabIndex) noexcept;
    void MarkReleased(usize slabIndex) noexcept;
    usize TakeReleased() noexcept;

    void LinkAvailable(ClassState& group, SlabHeader* slab) noexcept;
    void UnlinkAvailable(ClassState& group, SlabHeader* slab) noexcept;

    // 등급 잠금 안에서 부른다. slab에서 최대 wanted개를 떼어 첫 word로 이은 목록을 chain에 쓰고 뗀
    // 개수를 돌려준다(slab을 더 얻지 못하면 wanted보다 적다). 뗀 블록은 모두 표지가 서 있다.
    uint32 TakeBlocks(ClassState& group, uint32 sizeClass, uint32 wanted, void*& chain) noexcept;
    // 등급 잠금 안에서 부른다. 표지가 선 블록 하나를 자기 slab에 돌려준다(slab이 비면 푼다).
    void ReturnBlock(ClassState& group, void* block) noexcept;

    void* AllocateDirect(uint32 sizeClass) noexcept;
    Result<void> DeallocateDirect(void* address, usize slabIndex, uint32 sizeClass,
                                  uint32 blockIndex) noexcept;

    // ---- 스레드 캐시 ----
    struct Registry;
    static Registry& GetRegistry() noexcept;
    // 스레드 종료 hook이다(platform::ThreadExitSlot — 끝나는 스레드에서 불린다).
    static void ReleaseThreadRecord(void* value) noexcept;

    static Registry* CreateRegistry() noexcept;
    // 이 스레드의 캐시다(빠른 경로가 "마지막에 쓴 인스턴스"에서 찾지 못했을 때 부른다). 없으면
    // 만든다. 캐시를 쓰지 않는 인스턴스·종료 중인 스레드·만들지 못한 경우는 nullptr이고 호출자는
    // 등급 잠금 경로로 간다.
    ThreadCache* FindOrCreateCache() noexcept;
    ThreadCache* CreateCache() noexcept;
    // 이 스레드의 기록에서 이 인스턴스의 캐시를 찾는다(만들지 않는다).
    ThreadCache* FindCurrentCache() const noexcept;
    // 부분 묶음이 빈 캐시를 채우고 블록 하나를 꺼낸다(가득 묶음 → 이전 칸 → slab 순서).
    void* RefillAndTake(ThreadCache& cache, uint32 sizeClass) noexcept;
    // 부분 묶음이 찬 캐시에 자리를 만든다(가득 묶음과 바꾸고, 밀려난 가득 묶음은 이전 칸이나
    // slab으로).
    void MakeRoom(ThreadCache& cache, uint32 sizeClass) noexcept;
    bool PutTransfer(uint32 sizeClass, void* chain) noexcept;
    void* TakeTransfer(uint32 sizeClass) noexcept;
    // 묶음의 블록을 모두 자기 slab으로 돌려준다(ReturnChain은 등급 잠금을 잡고, Locked는 잠금
    // 안에서 부른다).
    void ReturnChain(uint32 sizeClass, void* chain) noexcept;
    void ReturnChainLocked(ClassState& group, void* chain) noexcept;
    // 소유 스레드가 자기 캐시의 블록을 모두 slab으로 돌려준다(계수는 캐시에 남는다).
    void FlushCache(ThreadCache& cache) noexcept;
    // 이전 칸의 묶음을 모두 slab으로 돌려준다.
    void DrainTransfer() noexcept;
    // 등록부 잠금 안에서 부른다. 캐시의 블록을 돌려주고(가득 묶음은 useTransfer면 이전 칸 먼저)
    // 계수를 인스턴스 합계로 옮긴 뒤 캐시를 해제한다.
    void RetireCache(ThreadCache* cache, bool useTransfer) noexcept;
    // 캐시와 인스턴스의 누적 계수를 등급별로 더한다.
    void CollectCounters(uint64* allocated, uint64* freed) noexcept;
    Result<void*> AllocateLarge(usize bytes, usize alignment, uint32 tag, bool fallback) noexcept;
    Result<void> DeallocateLarge(void* address) noexcept;
    void Trim() noexcept;

    Allocator& backing;
    // 인스턴스 번호다(프로세스에서 재사용하지 않는다). 0이면 스레드 캐시를 쓰지 않는다.
    uint64 id = 0;
    // 살아 있는 인스턴스 목록과 이 인스턴스의 캐시 목록이다(등록부 잠금).
    State* registryNext = nullptr;
    State* registryPrevious = nullptr;
    ThreadCache* caches = nullptr;

    // ---- 주소 범위(생성 뒤 바뀌지 않는다) ----
    platform::VirtualMemoryRegion region;
    uptr base = 0;
    usize reservedBytes = 0;
    usize slabCount = 0;
    // slab 번호 → 등급 + 1(0은 쓰지 않는 slab)이다. 반환이 잠금 없이 읽으므로 원자 접근만 한다.
    uint8* slabTable = nullptr;
    // 쓴 뒤 decommit한 slab의 비트다(범위 잠금).
    uint64* releasedBits = nullptr;
    uptr secret = SecretHighBit;
    uint32 retainedLimit = 0;

    // ---- slab 범위 상태(rangeMutex) ----
    platform::Mutex rangeMutex;
    // commit한 채 보관하는 빈 slab이다.
    SlabHeader* retained = nullptr;
    uint32 retainedCount = 0;
    // 아직 한 번도 쓰지 않은 첫 slab 번호다.
    usize nextUnusedSlab = 0;
    usize releasedCount = 0;
    // releasedBits에서 선 비트가 있을 수 있는 가장 낮은 word다.
    usize releasedHint = 0;
    usize committedSlabs = 0;

    ClassState classes[ClassCount];

    // ---- 큰 할당(largeMutex) ----
    platform::Mutex largeMutex;
    detail::UntrackedPlatformAllocator ledgerStorage;
    detail::AddressLedger large{ledgerStorage};
    uint64 largeBytes = 0;
    uint64 largeTotal = 0;
    uint64 fallbackAllocations = 0;
};

// ---- 주소 범위 ----

void BinnedAllocator::State::ReserveRange(usize requestedBytes) noexcept {
    platform::VirtualMemoryInfo info{};
    if (!platform::QueryVirtualMemoryInfo(info) || info.pageSize == 0 ||
        SlabBytes % info.pageSize != 0) {
        return;
    }

    usize bytes = requestedBytes;
    if (bytes > MaxSize - (SlabBytes - 1)) {
        bytes = MaxSize - (SlabBytes - 1);
    }

    bytes = (bytes + SlabBytes - 1) & ~(SlabBytes - 1);
    // 설정이 MinReserveBytes보다 작으면 그 값 한 번만 시도한다(작게 쓰겠다는 요청을 더 줄이지
    // 않는다).
    const usize floor = bytes < MinReserveBytes ? bytes : MinReserveBytes;
    for (; bytes >= SlabBytes && bytes >= floor; bytes = (bytes / 2) & ~(SlabBytes - 1)) {
        if (constructionFaults.reserveFailures != 0) {
            if (constructionFaults.reserveFailures != TestHooks::AllReserveAttempts) {
                --constructionFaults.reserveFailures;
            }

            continue;
        }

        // 접근을 열어 둔 예약이다: POSIX에서 slab마다 보호를 바꾸지 않아 매핑이 쪼개지지
        // 않는다(설계 18.4절). 쓰지 않는 slab은 slab 표가 걸러 읽지 않으므로 접근 차단에 기대지
        // 않는다.
        if (region.Reserve(bytes, platform::VirtualMemoryAccess::Open)) {
            break;
        }
    }

    void* address = region.Address();
    if (address == nullptr) {
        return;
    }

    const usize slabs = bytes >> SlabShift;
    const usize words = (slabs + BitsPerWord - 1) / BitsPerWord;
    if (!constructionFaults.tableAllocationFails) {
        slabTable = static_cast<uint8*>(platform::AlignedAllocate(slabs, alignof(uint64)));
        releasedBits = static_cast<uint64*>(
            platform::AlignedAllocate(words * sizeof(uint64), alignof(uint64)));
    }

    if (slabTable == nullptr || releasedBits == nullptr) {
        // 표를 받지 못하면 예약 없이 연다(모든 요청이 큰 경로로 간다).
        ReleaseRange();
        return;
    }

    platform::MemoryZero(slabTable, slabs);
    platform::MemoryZero(releasedBits, words * sizeof(uint64));
    base = reinterpret_cast<uptr>(address);
    reservedBytes = bytes;
    slabCount = slabs;
}

void BinnedAllocator::State::ReleaseRange() noexcept {
    platform::AlignedFree(slabTable);
    platform::AlignedFree(releasedBits);
    slabTable = nullptr;
    releasedBits = nullptr;
    base = 0;
    reservedBytes = 0;
    slabCount = 0;
    if (!region.Release()) {
        platform::Fatal("BinnedAllocator could not release its address range");
    }
}

bool BinnedAllocator::State::CommitSlab(usize slabIndex) noexcept {
    // 실패(OS의 commit 한도 등)는 이 요청만 큰 경로로 보낸다. region은 닫히지 않으므로 다음 요청이
    // 같은 slab 번호를 다시 시도한다.
    return static_cast<bool>(region.Commit(slabIndex << SlabShift, SlabBytes));
}

bool BinnedAllocator::State::DecommitSlab(usize slabIndex) noexcept {
    // 실패하면 slab은 그대로다(접근을 열어 둔 예약 — 접근과 내용이 유지된다). 호출자가 commit된 채
    // 보관해 다시 쓰고, 다음 Trim이 다시 시도한다.
    return static_cast<bool>(region.Decommit(slabIndex << SlabShift, SlabBytes));
}

void BinnedAllocator::State::MarkReleased(usize slabIndex) noexcept {
    const usize word = slabIndex / BitsPerWord;
    releasedBits[word] |= uint64{1} << (slabIndex % BitsPerWord);
    ++releasedCount;
    if (word < releasedHint) {
        releasedHint = word;
    }
}

usize BinnedAllocator::State::TakeReleased() noexcept {
    // releasedCount가 0이 아닐 때만 부른다. 가장 낮은 번호부터 다시 쓴다(주소가 앞으로 모인다).
    usize word = releasedHint;
    while (releasedBits[word] == 0) {
        ++word;
    }

    releasedHint = word;
    const uint32 bit = platform::PlatformMath::CountTrailingZeros64(releasedBits[word]);
    return word * BitsPerWord + bit;
}

SlabHeader* BinnedAllocator::State::AcquireSlab(uint32 sizeClass) noexcept {
    platform::LockGuard lock(rangeMutex);
    SlabHeader* slab = retained;
    usize slabIndex = 0;
    if (slab != nullptr) {
        retained = slab->next;
        --retainedCount;
        slabIndex = SlabIndexOf(slab);
    } else {
        const bool reuse = releasedCount != 0;
        if (!reuse && nextUnusedSlab == slabCount) {
            return nullptr;
        }

        slabIndex = reuse ? TakeReleased() : nextUnusedSlab;
        if (!CommitSlab(slabIndex)) {
            return nullptr;
        }

        if (reuse) {
            releasedBits[slabIndex / BitsPerWord] &= ~(uint64{1} << (slabIndex % BitsPerWord));
            --releasedCount;
        } else {
            ++nextUnusedSlab;
        }

        ++committedSlabs;
    }

    // 다시 쓰는 slab에는 옛 블록의 표지가 남아 있다. 뗀 블록 수가 0에서 다시 시작하므로 반환의
    // 4단계가 막고, 내줄 때 표지를 지운다.
    slab = ConstructAt<SlabHeader>(SlabAt(slabIndex));
    slab->capacity = Classes.capacity[sizeClass];
    slab->blockBytes = Classes.blockBytes[sizeClass];
    platform::AtomicStore<MemoryOrder::Relaxed>(&slabTable[slabIndex],
                                                static_cast<uint8>(sizeClass + 1));
    return slab;
}

void BinnedAllocator::State::ReleaseSlab(SlabHeader* slab) noexcept {
    platform::LockGuard lock(rangeMutex);
    const usize slabIndex = SlabIndexOf(slab);
    // 표를 먼저 0으로 돌린다: 이 slab의 주소로 오는 잘못된 반환이 decommit된 메모리를 읽지 않는다.
    platform::AtomicStore<MemoryOrder::Relaxed>(&slabTable[slabIndex], UnusedSlab);
    if (retainedCount < retainedLimit || !DecommitSlab(slabIndex)) {
        // 한도 안이거나 decommit이 실패했다(slab은 commit된 그대로다 — 보관해 다시 쓴다).
        slab->next = retained;
        retained = slab;
        ++retainedCount;
        return;
    }

    --committedSlabs;
    MarkReleased(slabIndex);
}

void BinnedAllocator::State::Trim() noexcept {
    platform::LockGuard lock(rangeMutex);
    SlabHeader* kept = nullptr;
    uint32 keptCount = 0;
    while (retained != nullptr) {
        SlabHeader* slab = retained;
        retained = slab->next;
        const usize slabIndex = SlabIndexOf(slab);
        if (!DecommitSlab(slabIndex)) {
            // 이번에는 풀지 못했다. 보관해 두고 나머지를 계속 푼다(다음 Trim이 다시 시도한다).
            slab->next = kept;
            kept = slab;
            ++keptCount;
            continue;
        }

        --committedSlabs;
        MarkReleased(slabIndex);
    }

    retained = kept;
    retainedCount = keptCount;
}

// ---- 등급의 slab 목록 ----

void BinnedAllocator::State::LinkAvailable(ClassState& group, SlabHeader* slab) noexcept {
    slab->previous = nullptr;
    slab->next = group.available;
    if (group.available != nullptr) {
        group.available->previous = slab;
    }

    group.available = slab;
    slab->listed = true;
}

void BinnedAllocator::State::UnlinkAvailable(ClassState& group, SlabHeader* slab) noexcept {
    if (slab->previous != nullptr) {
        slab->previous->next = slab->next;
    } else {
        group.available = slab->next;
    }

    if (slab->next != nullptr) {
        slab->next->previous = slab->previous;
    }

    slab->next = nullptr;
    slab->previous = nullptr;
    slab->listed = false;
}

// ---- 작은 블록(등급 잠금 경로) ----

uint32 BinnedAllocator::State::TakeBlocks(ClassState& group, uint32 sizeClass, uint32 wanted,
                                          void*& chain) noexcept {
    uint32 taken = 0;
    void* head = nullptr;
    while (taken < wanted) {
        SlabHeader* slab = group.available;
        if (slab == nullptr) {
            slab = AcquireSlab(sizeClass);
            if (slab == nullptr) {
                break;
            }

            LinkAvailable(group, slab);
        }

        // 돌려받은 블록부터 쓴다(표지가 이미 서 있다).
        const uint32 takenBefore = taken;
        while (taken < wanted && slab->freeList != nullptr) {
            void* block = slab->freeList;
            slab->freeList = ReadLink(block);
            WriteLink(block, head);
            head = block;
            ++taken;
        }

        // 모자라면 앞에서부터 새로 뗀다. 표지를 세운 뒤에 뗀 블록 수를 게시한다: 뗀 블록은 내주기
        // 전까지 표지가 서 있다(표지 불변식 — 다시 쓰는 slab의 옛 내용과 무관하다).
        uint32 carved = slab->carvedBlocks;
        const uint32 before = carved;
        while (taken < wanted && carved < slab->capacity) {
            void* block =
                reinterpret_cast<uint8*>(slab) + SlabHeaderBytes + usize{carved} * slab->blockBytes;
            WriteMarker(block, MarkerOf(block));
            WriteLink(block, head);
            head = block;
            ++carved;
            ++taken;
        }

        if (carved != before) {
            platform::AtomicStore<MemoryOrder::Release>(&slab->carvedBlocks, carved);
        }

        slab->usedBlocks += taken - takenBefore;

        if (slab->freeList == nullptr && carved == slab->capacity) {
            UnlinkAvailable(group, slab);
        }
    }

    chain = head;
    return taken;
}

void BinnedAllocator::State::ReturnBlock(ClassState& group, void* block) noexcept {
    SlabHeader* slab = SlabAt(SlabIndexOf(block));
    WriteLink(block, slab->freeList);
    slab->freeList = block;
    --slab->usedBlocks;
    if (slab->usedBlocks == 0) {
        if (slab->listed) {
            UnlinkAvailable(group, slab);
        }

        ReleaseSlab(slab);
    } else if (!slab->listed) {
        LinkAvailable(group, slab);
    }
}

void* BinnedAllocator::State::AllocateDirect(uint32 sizeClass) noexcept {
    ClassState& group = classes[sizeClass];
    void* block = nullptr;
    {
        platform::LockGuard lock(group.mutex);
        if (TakeBlocks(group, sizeClass, 1, block) == 0) {
            return nullptr;
        }

        ++group.allocated;
    }

    // 표지가 없는 블록은 사용자에게 나가 있는 블록뿐이다: 내주는 순간에만 지운다.
    WriteMarker(block, NoMarker);
    return block;
}

Result<void> BinnedAllocator::State::DeallocateDirect(void* address, usize slabIndex,
                                                      uint32 sizeClass,
                                                      uint32 blockIndex) noexcept {
    ClassState& group = classes[sizeClass];
    platform::LockGuard lock(group.mutex);
    // 잠금 밖에서 읽은 표가 그 사이 바뀌었으면(slab이 풀렸거나 다른 등급이 됐다) 이 주소는 살아
    // 있는 블록이 아니다 — 살아 있는 블록이 있는 slab은 풀리지 않는다.
    if (platform::AtomicLoad<MemoryOrder::Relaxed>(&slabTable[slabIndex]) != sizeClass + 1) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    // 4단계: 아직 뗀 적 없는 블록이다.
    if (blockIndex >= SlabAt(slabIndex)->carvedBlocks) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    // 5단계: 반환 표지가 서 있으면 이미 돌려받았거나 떼기만 하고 내준 적 없는 블록이다. 스레드
    // 캐시 경로의 반환은 이 잠금을 잡지 않으므로 여기서도 비교 교환으로 세운다.
    if (!TryMarkReturned(address, MarkerOf(address))) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    ReturnBlock(group, address);
    ++group.freed;
    return Result<void>::Success();
}

// ---- 스레드 캐시 ----

// 살아 있는 인스턴스의 등록부다. 프로세스 수명 동안 살린다(파괴하지 않는다): 스레드 종료 hook이
// 정적 소멸 뒤에(프로세스 종료를 부른 스레드에서) 불릴 수 있다.
struct BinnedAllocator::State::Registry {
    platform::Mutex mutex;
    uint64 nextId = 1;
    State* instances = nullptr;
    platform::ThreadExitSlot exitSlot{&State::ReleaseThreadRecord};

    // 잠금 안에서 부른다. 살아 있는 인스턴스면 그 상태, 아니면 nullptr이다.
    State* Find(uint64 instanceId) const noexcept {
        for (State* state = instances; state != nullptr; state = state->registryNext) {
            if (state->id == instanceId) {
                return state;
            }
        }

        return nullptr;
    }
};

BinnedAllocator::State::Registry* BinnedAllocator::State::CreateRegistry() noexcept {
    void* storage = platform::AlignedAllocate(sizeof(Registry), alignof(Registry));
    if (storage == nullptr) {
        platform::Fatal("BinnedAllocator could not allocate its instance registry");
    }

    return ConstructAt<Registry>(storage);
}

BinnedAllocator::State::Registry& BinnedAllocator::State::GetRegistry() noexcept {
    static Registry* const registry = CreateRegistry();
    return *registry;
}

void BinnedAllocator::State::ReleaseThreadRecord(void* value) noexcept {
    auto* record = static_cast<ThreadRecord*>(value);
    // 이 스레드의 남은 종료 정리가 allocator를 써도 해제한 기록을 보지 않게 한다(등급 잠금 경로로
    // 간다).
    threadRecord = &exitedRecord;
    Registry& registry = GetRegistry();
    {
        platform::LockGuard lock(registry.mutex);
        for (uint32 index = 0; index < record->count; ++index) {
            // 먼저 파괴된 인스턴스의 캐시는 그 파괴가 이미 해제했다(번호가 없으면 건너뛴다).
            if (State* state = registry.Find(record->entries[index].id)) {
                state->RetireCache(record->entries[index].cache, true);
            }
        }
    }

    platform::AlignedFree(record->entries);
    platform::AlignedFree(record);
}

ThreadCache* BinnedAllocator::State::FindCurrentCache() const noexcept {
    ThreadRecord* record = threadRecord;
    if (id == 0 || record == nullptr) {
        return nullptr;
    }

    for (uint32 index = 0; index < record->count; ++index) {
        if (record->entries[index].id == id) {
            return record->entries[index].cache;
        }
    }

    return nullptr;
}

ThreadCache* BinnedAllocator::State::FindOrCreateCache() noexcept {
    if (id == 0) {
        return nullptr;
    }

    ThreadRecord* record = threadRecord;
    if (record != nullptr) {
        if (record->exited) {
            return nullptr;
        }

        if (ThreadCache* cache = FindCurrentCache()) {
            record->lastId = id;
            record->lastCache = cache;
            return cache;
        }
    }

    return CreateCache();
}

ThreadCache* BinnedAllocator::State::CreateCache() noexcept {
    Registry& registry = GetRegistry();
    if (failNextCacheCreation) {
        failNextCacheCreation = false;
        return nullptr;
    }

    if (!registry.exitSlot.IsValid()) {
        return nullptr;
    }

    ThreadRecord* record = threadRecord;
    if (record == nullptr) {
        void* storage = platform::AlignedAllocate(sizeof(ThreadRecord), alignof(ThreadRecord));
        if (storage == nullptr) {
            return nullptr;
        }

        record = ConstructAt<ThreadRecord>(storage);
        // 종료 hook에 걸지 못한 기록은 쓰지 않는다(스레드가 끝날 때 캐시를 돌려줄 길이 없다).
        if (!registry.exitSlot.Set(record)) {
            platform::AlignedFree(storage);
            return nullptr;
        }

        threadRecord = record;
    }

    void* storage = platform::AlignedAllocate(sizeof(ThreadCache), alignof(ThreadCache));
    if (storage == nullptr) {
        return nullptr;
    }

    ThreadCache* cache = ConstructAt<ThreadCache>(storage);
    {
        platform::LockGuard lock(registry.mutex);
        // 파괴된 인스턴스의 항목을 버린다(그 캐시는 인스턴스 파괴가 해제했다).
        uint32 kept = 0;
        for (uint32 index = 0; index < record->count; ++index) {
            if (registry.Find(record->entries[index].id) != nullptr) {
                record->entries[kept++] = record->entries[index];
            }
        }

        record->count = kept;
        if (record->count == record->capacity) {
            const uint32 capacity =
                record->capacity == 0 ? InitialRecordEntries : record->capacity * 2;
            auto* entries = static_cast<ThreadRecord::Entry*>(platform::AlignedAllocate(
                usize{capacity} * sizeof(ThreadRecord::Entry), alignof(ThreadRecord::Entry)));
            if (entries == nullptr) {
                platform::AlignedFree(storage);
                return nullptr;
            }

            for (uint32 index = 0; index < record->count; ++index) {
                entries[index] = record->entries[index];
            }

            platform::AlignedFree(record->entries);
            record->entries = entries;
            record->capacity = capacity;
        }

        record->entries[record->count++] = ThreadRecord::Entry{id, cache};
        cache->next = caches;
        if (caches != nullptr) {
            caches->previous = cache;
        }

        caches = cache;
    }

    record->lastId = id;
    record->lastCache = cache;
    return cache;
}

bool BinnedAllocator::State::PutTransfer(uint32 sizeClass, void* chain) noexcept {
    void** slots = classes[sizeClass].transfer;
    for (uint32 index = 0; index < TransferSlotCount; ++index) {
        if (platform::AtomicLoad<MemoryOrder::Relaxed>(&slots[index]) != nullptr) {
            continue;
        }

        void* expected = nullptr;
        if (platform::AtomicCompareExchangeStrong<MemoryOrder::AcquireRelease,
                                                  MemoryOrder::Relaxed>(&slots[index], expected,
                                                                        chain)) {
            return true;
        }
    }

    return false;
}

void* BinnedAllocator::State::TakeTransfer(uint32 sizeClass) noexcept {
    void** slots = classes[sizeClass].transfer;
    for (uint32 index = 0; index < TransferSlotCount; ++index) {
        if (platform::AtomicLoad<MemoryOrder::Relaxed>(&slots[index]) == nullptr) {
            continue;
        }

        if (void* chain =
                platform::AtomicExchange<MemoryOrder::AcquireRelease>(&slots[index], nullptr)) {
            return chain;
        }
    }

    return nullptr;
}

void BinnedAllocator::State::ReturnChainLocked(ClassState& group, void* chain) noexcept {
    while (chain != nullptr) {
        void* next = ReadLink(chain);
        ReturnBlock(group, chain);
        chain = next;
    }
}

void BinnedAllocator::State::ReturnChain(uint32 sizeClass, void* chain) noexcept {
    ClassState& group = classes[sizeClass];
    platform::LockGuard lock(group.mutex);
    ReturnChainLocked(group, chain);
}

void* BinnedAllocator::State::RefillAndTake(ThreadCache& cache, uint32 sizeClass) noexcept {
    ClassCache& slot = cache.classes[sizeClass];
    const uint32 limit = Classes.bundleBlocks[sizeClass];
    if (slot.full.head != nullptr) {
        slot.partial = slot.full;
        slot.full = Bundle{};
    } else if (void* chain = TakeTransfer(sizeClass)) {
        // 이전 칸의 묶음은 언제나 가득 찬 묶음이다(개수를 블록에 적지 않는다).
        slot.partial = Bundle{chain, limit};
    } else {
        ClassState& group = classes[sizeClass];
        platform::LockGuard lock(group.mutex);
        slot.partial.count = TakeBlocks(group, sizeClass, limit, slot.partial.head);
        if (slot.partial.count == 0) {
            return nullptr;
        }
    }

    void* block = slot.partial.head;
    slot.partial.head = ReadLink(block);
    --slot.partial.count;
    return block;
}

void BinnedAllocator::State::MakeRoom(ThreadCache& cache, uint32 sizeClass) noexcept {
    ClassCache& slot = cache.classes[sizeClass];
    if (slot.full.head != nullptr) {
        // 가득 묶음이 이미 있다: 그것을 이전 칸에 넣고, 칸이 없으면 slab으로 돌려준다.
        if (!PutTransfer(sizeClass, slot.full.head)) {
            ReturnChain(sizeClass, slot.full.head);
        }
    }

    slot.full = slot.partial;
    slot.partial = Bundle{};
}

void BinnedAllocator::State::FlushCache(ThreadCache& cache) noexcept {
    for (uint32 sizeClass = 0; sizeClass < ClassCount; ++sizeClass) {
        ClassCache& slot = cache.classes[sizeClass];
        if (slot.full.head != nullptr) {
            ReturnChain(sizeClass, slot.full.head);
            slot.full = Bundle{};
        }

        if (slot.partial.head != nullptr) {
            ReturnChain(sizeClass, slot.partial.head);
            slot.partial = Bundle{};
        }
    }
}

void BinnedAllocator::State::DrainTransfer() noexcept {
    for (uint32 sizeClass = 0; sizeClass < ClassCount; ++sizeClass) {
        while (void* chain = TakeTransfer(sizeClass)) {
            ReturnChain(sizeClass, chain);
        }
    }
}

void BinnedAllocator::State::RetireCache(ThreadCache* cache, bool useTransfer) noexcept {
    for (uint32 sizeClass = 0; sizeClass < ClassCount; ++sizeClass) {
        ClassCache& slot = cache->classes[sizeClass];
        // 가득 묶음만 이전 칸에 간다. 부분 묶음은 slab으로 바로 돌려준다(이전 칸의 묶음 크기는
        // 등급의 한도로 정해져 있다).
        void* fullChain = slot.full.head;
        if (fullChain != nullptr && useTransfer && PutTransfer(sizeClass, fullChain)) {
            fullChain = nullptr;
        }

        ClassState& group = classes[sizeClass];
        platform::LockGuard lock(group.mutex);
        ReturnChainLocked(group, fullChain);
        ReturnChainLocked(group, slot.partial.head);

        group.allocated += platform::AtomicLoad<MemoryOrder::Relaxed>(&slot.allocated);
        group.freed += platform::AtomicLoad<MemoryOrder::Relaxed>(&slot.freed);
    }

    if (cache->previous != nullptr) {
        cache->previous->next = cache->next;
    } else {
        caches = cache->next;
    }

    if (cache->next != nullptr) {
        cache->next->previous = cache->previous;
    }

    platform::AlignedFree(cache);
}

void BinnedAllocator::State::CollectCounters(uint64* allocated, uint64* freed) noexcept {
    for (ThreadCache* cache = caches; cache != nullptr; cache = cache->next) {
        for (uint32 sizeClass = 0; sizeClass < ClassCount; ++sizeClass) {
            const ClassCache& slot = cache->classes[sizeClass];
            allocated[sizeClass] += platform::AtomicLoad<MemoryOrder::Relaxed>(&slot.allocated);
            freed[sizeClass] += platform::AtomicLoad<MemoryOrder::Relaxed>(&slot.freed);
        }
    }

    for (uint32 sizeClass = 0; sizeClass < ClassCount; ++sizeClass) {
        ClassState& group = classes[sizeClass];
        platform::LockGuard lock(group.mutex);
        allocated[sizeClass] += group.allocated;
        freed[sizeClass] += group.freed;
    }
}

// ---- 큰 할당 ----

Result<void*> BinnedAllocator::State::AllocateLarge(usize bytes, usize alignment, uint32 tag,
                                                    bool fallback) noexcept {
    // 상위 호출은 잠금 밖에서 한다(상위가 이 allocator를 다시 불러도 막히지 않는다).
    auto allocated = backing.Allocate(bytes, alignment, tag);
    if (!allocated) {
        return allocated;
    }

    void* address = allocated.Value();
    const uptr value = reinterpret_cast<uptr>(address);
    if (address == nullptr || (value & (alignment - 1)) != 0) {
        platform::Fatal("BinnedAllocator backing returned invalid storage");
    }

    if (value > MaxSize - bytes) {
        platform::Fatal("BinnedAllocator backing returned overflowing address range");
    }

    if (value < base + reservedBytes && value + bytes > base) {
        platform::Fatal("BinnedAllocator backing returned overlapping storage");
    }

    ErrorCode failure = ErrorCode::None;
    {
        platform::LockGuard lock(largeMutex);
        if (auto inserted = large.Insert(address, bytes); !inserted) {
            // 상위가 살아 있는 주소를 다시 내줬다.
            if (inserted.Error() == ErrorCode::InvalidArgument) {
                platform::Fatal("BinnedAllocator backing returned overlapping storage");
            }

            failure = inserted.Error();
        } else {
            largeBytes += bytes;
            ++largeTotal;
            if (fallback) {
                ++fallbackAllocations;
            }
        }
    }

    if (failure != ErrorCode::None) {
        // 원장에 적지 못한 블록은 게시하지 않고 상위에 돌려준다.
        if (!backing.Deallocate(address)) {
            platform::Fatal("BinnedAllocator backing rejected the return of an unpublished block");
        }

        return Result<void*>::Failure(failure);
    }

    return Result<void*>::Success(address);
}

Result<void> BinnedAllocator::State::DeallocateLarge(void* address) noexcept {
    usize bytes = 0;
    {
        platform::LockGuard lock(largeMutex);
        // 주소 값만 비교한다(읽지 않는다). 먼저 원장에서 지우므로 두 스레드가 같은 주소를 함께
        // 반환해도 한쪽만 지나간다.
        if (!large.Remove(address, bytes)) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        largeBytes -= bytes;
    }

    auto returned = backing.Deallocate(address);
    if (!returned) {
        // 상위가 반환을 거부했다: 할당은 살아 있는 채로 남는다(기록을 되돌린다).
        platform::LockGuard lock(largeMutex);
        if (!large.Insert(address, bytes)) {
            platform::Fatal("BinnedAllocator could not restore the record of a rejected return");
        }

        largeBytes += bytes;
        return returned;
    }

    return Result<void>::Success();
}

// ---- 공개 API ----

BinnedAllocator::BinnedAllocator(Allocator& backing, const BinnedAllocatorConfig& config) noexcept {
    void* storage = platform::AlignedAllocate(sizeof(State), alignof(State));
    if (storage == nullptr) {
        platform::Fatal("BinnedAllocator could not allocate its state");
    }

    state_ = ConstructAt<State>(storage, backing);
    State& state = *state_;
    state.retainedLimit = config.retainedEmptySlabs;
    uptr secret = 0;
    if (!platform::TryFillSecureRandom(&secret, sizeof(secret))) {
        // OS 난수를 얻지 못하면 상태 주소를 섞어 쓴다(인스턴스마다 다르지만 예측할 수 있다 — 표지는
        // 오용 검출용이지 보안 경계가 아니다).
        constexpr uptr Mix = 0x9e3779b97f4a7c15ull;
        secret = reinterpret_cast<uptr>(storage) * Mix;
    }

    state.secret = secret | SecretHighBit;
    state.ReserveRange(config.reserveBytes != 0 ? config.reserveBytes : DefaultReserveBytes);
    constructionFaults = ConstructionFaults{};
    // 예약하지 못한 인스턴스는 작은 블록이 없으므로 캐시도 없다.
    if (config.threadCaches && state.slabCount != 0) {
        State::Registry& registry = State::GetRegistry();
        platform::LockGuard lock(registry.mutex);
        state.id = registry.nextId++;
        state.registryNext = registry.instances;
        if (registry.instances != nullptr) {
            registry.instances->registryPrevious = &state;
        }

        registry.instances = &state;
    }

    cacheId_ = state.id;
}

BinnedAllocator::~BinnedAllocator() noexcept {
    State& state = *state_;
    // 소유자의 수명 오류를 숨기는 일괄 반환을 하지 않는다(캐시를 해제하기 전에 센다).
    if (Stats().liveAllocations != 0) {
        platform::Fatal("BinnedAllocator destroyed with live allocations");
    }

    if (state.id != 0) {
        State::Registry& registry = State::GetRegistry();
        platform::LockGuard lock(registry.mutex);
        // 번호를 먼저 지운다: 그 뒤에 끝나는 스레드의 종료 hook은 이 인스턴스의 캐시를 건너뛴다.
        if (state.registryPrevious != nullptr) {
            state.registryPrevious->registryNext = state.registryNext;
        } else {
            registry.instances = state.registryNext;
        }

        if (state.registryNext != nullptr) {
            state.registryNext->registryPrevious = state.registryPrevious;
        }

        // 다른 스레드가 남긴 캐시까지 모두 비워 해제한다(그 스레드의 기록에는 죽은 번호만 남는다).
        while (state.caches != nullptr) {
            state.RetireCache(state.caches, false);
        }

        // 이 스레드의 기록에서는 항목도 지운다.
        ThreadRecord* record = threadRecord;
        if (record != nullptr && !record->exited) {
            uint32 kept = 0;
            for (uint32 index = 0; index < record->count; ++index) {
                if (record->entries[index].id != state.id) {
                    record->entries[kept++] = record->entries[index];
                }
            }

            record->count = kept;
            if (record->lastId == state.id) {
                record->lastId = 0;
                record->lastCache = nullptr;
            }
        }
    }

    state.DrainTransfer();
    {
        // 블록이 모두 돌아왔으면 쓰는 slab이 없다(commit된 것은 보관한 빈 slab뿐이다).
        platform::LockGuard lock(state.rangeMutex);
        if (state.committedSlabs != state.retainedCount) {
            platform::Fatal("BinnedAllocator slab accounting diverged");
        }
    }

    // 보관한 slab은 범위를 풀 때 함께 풀린다.
    state.ReleaseRange();
    DestroyAt(state_);
    platform::AlignedFree(state_);
}

Result<void*> BinnedAllocator::Allocate(usize bytes, usize alignment, uint32 tag) noexcept {
    if (!IsPowerOfTwo(alignment)) {
        return Result<void*>::Failure(ErrorCode::InvalidArgument);
    }

    if (bytes == 0) {
        return Result<void*>::Success(nullptr);
    }

    State& state = *state_;
    if (bytes > MaxSmallBytes || alignment > MaxSmallAlignment) {
        return state.AllocateLarge(bytes, alignment, tag, false);
    }

    if (state.slabCount != 0) {
        const uint32 sizeClass = ClassOfRequest(bytes, alignment);
        ThreadRecord* record = threadRecord;
        ThreadCache* cache = record != nullptr && record->lastId == cacheId_
                                 ? record->lastCache
                                 : state.FindOrCreateCache();
        if (cache != nullptr) {
            ClassCache& slot = cache->classes[sizeClass];
            void* block = slot.partial.head;
            if (block != nullptr) {
                slot.partial.head = ReadLink(block);
                --slot.partial.count;
            } else {
                block = state.RefillAndTake(*cache, sizeClass);
            }

            if (block != nullptr) {
                // 표지가 없는 블록은 사용자에게 나가 있는 블록뿐이다: 내주는 순간에만 지운다.
                WriteMarker(block, NoMarker);
                platform::AtomicStore<MemoryOrder::Relaxed>(&slot.allocated, slot.allocated + 1);
                return Result<void*>::Success(block);
            }
        } else if (void* block = state.AllocateDirect(sizeClass)) {
            return Result<void*>::Success(block);
        }
    }

    // 예약 범위가 없거나 다 찼거나 slab을 commit하지 못했다. 실패가 아니라 느린 경로다. 작은 블록의
    // 정렬 보장(16 이상)은 그대로 지킨다.
    return state.AllocateLarge(bytes, alignment < BlockGranularity ? BlockGranularity : alignment,
                               tag, true);
}

Result<void> BinnedAllocator::Deallocate(void* address) noexcept {
    if (address == nullptr) {
        return Result<void>::Success();
    }

    State& state = *state_;
    // 1단계: 예약 범위 밖이면 큰 할당 원장을 본다.
    const uptr offset = reinterpret_cast<uptr>(address) - state.base;
    if (offset >= state.reservedBytes) {
        return state.DeallocateLarge(address);
    }

    // 2단계: 쓰지 않는 slab이면 읽지 않는다(commit되지 않았을 수 있다).
    const usize slabIndex = offset >> SlabShift;
    const uint8 entry = platform::AtomicLoad<MemoryOrder::Relaxed>(&state.slabTable[slabIndex]);
    if (entry == UnusedSlab) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    // 3단계: 그 등급의 블록 경계가 아니면 가운데 주소다.
    const uint32 sizeClass = entry - 1u;
    uint32 blockIndex = 0;
    if (!BlockIndexOfOffset(sizeClass, offset & (SlabBytes - 1), blockIndex)) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    ThreadRecord* record = threadRecord;
    ThreadCache* cache = record != nullptr && record->lastId == cacheId_
                             ? record->lastCache
                             : state.FindOrCreateCache();
    if (cache == nullptr) {
        return state.DeallocateDirect(address, slabIndex, sizeClass, blockIndex);
    }

    // 4단계: 아직 뗀 적 없는 블록이다. slab 머리를 잠금 없이 읽는다 — 살아 있는 블록이 있는 slab은
    // 풀리지 않으므로 올바른 반환이 읽는 머리는 언제나 commit돼 있다(오용은 18.4절 한계 2).
    if (blockIndex >=
        platform::AtomicLoad<MemoryOrder::Acquire>(&state.SlabAt(slabIndex)->carvedBlocks)) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    // 5단계: 반환 표지가 서 있으면 이미 돌려받았거나 떼기만 하고 내준 적 없는 블록이다.
    if (!TryMarkReturned(address, state.MarkerOf(address))) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    ClassCache& slot = cache->classes[sizeClass];
    if (slot.partial.count == Classes.bundleBlocks[sizeClass]) {
        state.MakeRoom(*cache, sizeClass);
    }

    WriteLink(address, slot.partial.head);
    slot.partial.head = address;
    ++slot.partial.count;
    platform::AtomicStore<MemoryOrder::Relaxed>(&slot.freed, slot.freed + 1);
    return Result<void>::Success();
}

void BinnedAllocator::Trim() noexcept {
    State& state = *state_;
    // 호출 스레드의 캐시와 이전 칸을 slab으로 돌려준 뒤에 빈 slab을 푼다. 다른 스레드의 캐시는
    // 건드리지 않는다(그 스레드만 자기 묶음을 만진다).
    if (ThreadCache* cache = state.FindCurrentCache()) {
        state.FlushCache(*cache);
    }

    state.DrainTransfer();
    state.Trim();
}

AllocationStats BinnedAllocator::Stats() const noexcept {
    State& state = *state_;
    uint64 allocated[ClassCount]{};
    uint64 freed[ClassCount]{};
    if (state.id != 0) {
        // 캐시 목록과 끝난 스레드 계수의 이동이 등록부 잠금 아래라, 한 계수를 두 번 세지 않는다.
        State::Registry& registry = State::GetRegistry();
        platform::LockGuard lock(registry.mutex);
        state.CollectCounters(allocated, freed);
    } else {
        state.CollectCounters(allocated, freed);
    }

    AllocationStats stats{};
    for (uint32 sizeClass = 0; sizeClass < ClassCount; ++sizeClass) {
        // 다른 스레드가 쓰는 중이면 반환 계수를 먼저 읽을 수 있다(음수가 되지 않게 자른다).
        const uint64 live =
            allocated[sizeClass] > freed[sizeClass] ? allocated[sizeClass] - freed[sizeClass] : 0;
        stats.liveAllocations += live;
        stats.liveBytes += live * uint64{Classes.blockBytes[sizeClass]};
        stats.totalAllocations += allocated[sizeClass];
    }

    platform::LockGuard lock(state.largeMutex);
    stats.liveAllocations += state.large.Count();
    stats.liveBytes += state.largeBytes;
    stats.totalAllocations += state.largeTotal;
    return stats;
}

BinnedAllocatorUsage BinnedAllocator::Usage() const noexcept {
    State& state = *state_;
    BinnedAllocatorUsage usage{};
    usage.reservedBytes = state.reservedBytes;
    {
        platform::LockGuard lock(state.rangeMutex);
        usage.committedBytes = state.committedSlabs << SlabShift;
        usage.retainedSlabs = state.retainedCount;
    }

    platform::LockGuard lock(state.largeMutex);
    usage.largeAllocations = state.large.Count();
    usage.fallbackAllocations = state.fallbackAllocations;
    return usage;
}

usize BinnedAllocator::ReservedBytes() const noexcept {
    return state_->reservedBytes;
}

// ---- 시험 통로의 진입점(BinnedAllocatorTestHooks.h) ----

void BinnedAllocator::TestHooks::FailNextReserveAttempts(uint32 count) noexcept {
    constructionFaults.reserveFailures = count;
}

void BinnedAllocator::TestHooks::FailNextSlabTableAllocation() noexcept {
    constructionFaults.tableAllocationFails = true;
}

void BinnedAllocator::TestHooks::FailLedgerStorageOnAttempt(BinnedAllocator& allocator,
                                                            uint64 oneBasedAttempt) noexcept {
    platform::LockGuard lock(allocator.state_->largeMutex);
    allocator.state_->ledgerStorage.FailOnAttempt(oneBasedAttempt);
}

void BinnedAllocator::TestHooks::FailNextThreadCacheCreation() noexcept {
    failNextCacheCreation = true;
}

void BinnedAllocator::TestHooks::FlushCurrentThreadCache(BinnedAllocator& allocator) noexcept {
    BinnedAllocator::State& state = *allocator.state_;
    if (ThreadCache* cache = state.FindCurrentCache()) {
        state.FlushCache(*cache);
    }

    state.DrainTransfer();
}

bool BinnedAllocator::TestHooks::HasCurrentThreadCache(BinnedAllocator& allocator) noexcept {
    return allocator.state_->FindCurrentCache() != nullptr;
}

void BinnedAllocator::TestHooks::RunCurrentThreadExitHook() noexcept {
    BinnedAllocator::State::Registry& registry = BinnedAllocator::State::GetRegistry();
    void* record = registry.exitSlot.Get();
    if (record != nullptr && registry.exitSlot.Set(nullptr)) {
        BinnedAllocator::State::ReleaseThreadRecord(record);
    }
}

bool BinnedAllocator::TestHooks::IsCurrentThreadExited() noexcept {
    return threadRecord == &exitedRecord;
}

uint32 BinnedAllocator::TestHooks::CurrentThreadRecordEntries() noexcept {
    return threadRecord != nullptr ? threadRecord->count : 0;
}

uint32 BinnedAllocator::TestHooks::ThreadCacheCount(BinnedAllocator& allocator) noexcept {
    BinnedAllocator::State& state = *allocator.state_;
    if (state.id == 0) {
        return 0;
    }

    BinnedAllocator::State::Registry& registry = BinnedAllocator::State::GetRegistry();
    platform::LockGuard lock(registry.mutex);
    uint32 count = 0;
    for (ThreadCache* cache = state.caches; cache != nullptr; cache = cache->next) {
        ++count;
    }

    return count;
}

uint32 BinnedAllocator::TestHooks::TransferBundleCount(BinnedAllocator& allocator,
                                                       uint32 sizeClass) noexcept {
    uint32 count = 0;
    for (void* const& slot : allocator.state_->classes[sizeClass].transfer) {
        count += platform::AtomicLoad<MemoryOrder::Relaxed>(&slot) != nullptr ? 1u : 0u;
    }

    return count;
}

uint32 BinnedAllocator::TestHooks::TransferChainLength(BinnedAllocator& allocator,
                                                       uint32 sizeClass) noexcept {
    // 다른 스레드가 이 인스턴스를 쓰지 않을 때만 부른다(묶음을 따라 읽는다).
    for (void* const& slot : allocator.state_->classes[sizeClass].transfer) {
        void* chain = platform::AtomicLoad<MemoryOrder::Relaxed>(&slot);
        if (chain != nullptr) {
            uint32 length = 0;
            for (; chain != nullptr; chain = ReadLink(chain)) {
                ++length;
            }

            return length;
        }
    }

    return 0;
}

uint32 BinnedAllocator::TestHooks::BundleBlocks(uint32 sizeClass) noexcept {
    return Classes.bundleBlocks[sizeClass];
}

uint32 BinnedAllocator::TestHooks::ClassCount() noexcept {
    return rocks::ClassCount;
}

usize BinnedAllocator::TestHooks::ClassBlockBytes(uint32 sizeClass) noexcept {
    return Classes.blockBytes[sizeClass];
}

uint32 BinnedAllocator::TestHooks::ClassCapacity(uint32 sizeClass) noexcept {
    return Classes.capacity[sizeClass];
}

uint32 BinnedAllocator::TestHooks::ClassOf(usize bytes, usize alignment) noexcept {
    return ClassOfRequest(bytes, alignment);
}

bool BinnedAllocator::TestHooks::BlockIndexOf(uint32 sizeClass, usize offsetInSlab,
                                              uint32& blockIndex) noexcept {
    return BlockIndexOfOffset(sizeClass, offsetInSlab, blockIndex);
}

uint8 BinnedAllocator::TestHooks::SlabTableEntry(BinnedAllocator& allocator,
                                                 const void* address) noexcept {
    BinnedAllocator::State& state = *allocator.state_;
    const uptr offset = reinterpret_cast<uptr>(address) - state.base;
    if (offset >= state.reservedBytes) {
        return OutsideRange;
    }

    return platform::AtomicLoad<MemoryOrder::Relaxed>(&state.slabTable[offset >> SlabShift]);
}

} // namespace rocks
