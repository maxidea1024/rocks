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

// slab 앞 64 B의 머리다. carvedBlocks 말고는 그 등급의 잠금 안에서만 읽고 쓴다.
struct SlabHeader {
    // 등급의 "내줄 블록이 있는 slab" 목록(이중 연결) 또는 빈 slab 보관소(단일 연결)의 연결이다.
    SlabHeader* next = nullptr;
    SlabHeader* previous = nullptr;
    // 돌려받은 블록의 목록이다(블록의 첫 word로 잇는다).
    void* freeList = nullptr;
    // slab 밖에 나가 있는 블록 수다(사용자가 쥔 블록). 0이 되면 slab을 푼다.
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

uptr ReadMarker(void* block) noexcept {
    return static_cast<FreeBlock*>(block)->marker;
}

void WriteMarker(void* block, uptr marker) noexcept {
    static_cast<FreeBlock*>(block)->marker = marker;
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

// 등급 하나의 상태다. 모두 mutex 안에서 읽고 쓴다.
struct ClassState {
    platform::Mutex mutex;
    // 내줄 블록이 있는 slab의 목록이다(돌려받은 블록이 있거나 아직 다 떼지 않은 slab).
    SlabHeader* available = nullptr;
    // 사용자에게 내준·돌려받은 누적 블록 수다.
    uint64 allocated = 0;
    uint64 freed = 0;
};

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
    // 범위 잠금 안에서 부른다.
    bool CommitSlab(usize slabIndex) noexcept;
    enum class DecommitOutcome : uint8 {
        Decommitted,
        Untouched,
        Lost
    };
    DecommitOutcome DecommitSlab(usize slabIndex) noexcept;
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
    Result<void*> AllocateLarge(usize bytes, usize alignment, uint32 tag, bool fallback) noexcept;
    Result<void> DeallocateLarge(void* address) noexcept;
    void Trim() noexcept;

    Allocator& backing;

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
    // 시험 주입: commitAttempts가 이 값이 되는 commit을 실패시킨다(0이면 끈다).
    uint64 commitAttempts = 0;
    uint64 commitFailureAttempt = 0;
    DecommitOutcome injectedDecommit = DecommitOutcome::Decommitted;

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

        if (region.Reserve(bytes)) {
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
    ++commitAttempts;
    if (commitFailureAttempt != 0 && commitAttempts == commitFailureAttempt) {
        return false;
    }

    // 실패하면 region이 스스로 닫혀(Poisoned) 뒤의 commit도 실패한다 — 그때부터 새 slab은 없고
    // 작은 요청은 큰 경로로 간다. 이미 commit한 slab은 그대로 쓴다.
    return static_cast<bool>(region.Commit(slabIndex << SlabShift, SlabBytes));
}

BinnedAllocator::State::DecommitOutcome
BinnedAllocator::State::DecommitSlab(usize slabIndex) noexcept {
    if (injectedDecommit != DecommitOutcome::Decommitted) {
        return injectedDecommit;
    }

    const auto result = region.Decommit(slabIndex << SlabShift, SlabBytes);
    if (result) {
        return DecommitOutcome::Decommitted;
    }

    // Poisoned는 OS를 부르지 않고 거절한 것이라 slab이 commit된 그대로다. OS 실패는 그 slab의
    // 상태를 알 수 없다(보호 변경과 페이지 반환이 여러 단계다).
    return result.code == platform::VirtualMemoryCode::Poisoned ? DecommitOutcome::Untouched
                                                                : DecommitOutcome::Lost;
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
    DecommitOutcome outcome = DecommitOutcome::Untouched;
    if (retainedCount >= retainedLimit) {
        outcome = DecommitSlab(slabIndex);
    }

    if (outcome == DecommitOutcome::Untouched) {
        // 한도 안이거나, 닫힌 region이 decommit을 거절했다(commit된 채 보관해 다시 쓴다).
        slab->next = retained;
        retained = slab;
        ++retainedCount;
        return;
    }

    --committedSlabs;
    if (outcome == DecommitOutcome::Decommitted) {
        MarkReleased(slabIndex);
    }

    // Lost: 상태를 알 수 없는 slab은 다시 쓰지 않는다(주소 64KB를 파괴 때까지 잃는다).
}

void BinnedAllocator::State::Trim() noexcept {
    platform::LockGuard lock(rangeMutex);
    SlabHeader* kept = nullptr;
    uint32 keptCount = 0;
    while (retained != nullptr) {
        SlabHeader* slab = retained;
        retained = slab->next;
        const usize slabIndex = SlabIndexOf(slab);
        const DecommitOutcome outcome = DecommitSlab(slabIndex);
        if (outcome == DecommitOutcome::Untouched) {
            slab->next = kept;
            kept = slab;
            ++keptCount;
            continue;
        }

        --committedSlabs;
        if (outcome == DecommitOutcome::Decommitted) {
            MarkReleased(slabIndex);
        }
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

    // 5단계: 반환 표지가 서 있으면 이미 돌려받았거나 떼기만 하고 내준 적 없는 블록이다.
    const uptr marker = MarkerOf(address);
    if (ReadMarker(address) == marker) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    WriteMarker(address, marker);
    ReturnBlock(group, address);
    ++group.freed;
    return Result<void>::Success();
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
}

BinnedAllocator::~BinnedAllocator() noexcept {
    // 소유자의 수명 오류를 숨기는 일괄 반환을 하지 않는다.
    if (Stats().liveAllocations != 0) {
        platform::Fatal("BinnedAllocator destroyed with live allocations");
    }

    State& state = *state_;
    // 살아 있는 블록이 없으면 쓰는 slab도 없다. 보관한 slab은 범위를 풀 때 함께 풀린다.
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
        if (void* block = state.AllocateDirect(ClassOfRequest(bytes, alignment))) {
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

    return state.DeallocateDirect(address, slabIndex, sizeClass, blockIndex);
}

void BinnedAllocator::Trim() noexcept {
    state_->Trim();
}

AllocationStats BinnedAllocator::Stats() const noexcept {
    State& state = *state_;
    AllocationStats stats{};
    for (uint32 sizeClass = 0; sizeClass < ClassCount; ++sizeClass) {
        ClassState& group = state.classes[sizeClass];
        platform::LockGuard lock(group.mutex);
        const uint64 live = group.allocated - group.freed;
        stats.liveAllocations += live;
        stats.liveBytes += live * uint64{Classes.blockBytes[sizeClass]};
        stats.totalAllocations += group.allocated;
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

void BinnedAllocator::TestHooks::FailSlabCommitOnAttempt(BinnedAllocator& allocator,
                                                         uint64 oneBasedAttempt) noexcept {
    platform::LockGuard lock(allocator.state_->rangeMutex);
    allocator.state_->commitAttempts = 0;
    allocator.state_->commitFailureAttempt = oneBasedAttempt;
}

void BinnedAllocator::TestHooks::SetSlabDecommitFault(BinnedAllocator& allocator,
                                                      DecommitFault fault) noexcept {
    using Outcome = BinnedAllocator::State::DecommitOutcome;
    platform::LockGuard lock(allocator.state_->rangeMutex);
    allocator.state_->injectedDecommit = fault == DecommitFault::None      ? Outcome::Decommitted
                                         : fault == DecommitFault::Refused ? Outcome::Untouched
                                                                           : Outcome::Lost;
}

void BinnedAllocator::TestHooks::FailLedgerStorageOnAttempt(BinnedAllocator& allocator,
                                                            uint64 oneBasedAttempt) noexcept {
    platform::LockGuard lock(allocator.state_->largeMutex);
    allocator.state_->ledgerStorage.FailOnAttempt(oneBasedAttempt);
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
