#include <Crypto/Secret.h>

#include <Base/Checked.h>
#include <Base/Compiler.h>
#include <Core/Move.h>
#include <Platform/Memory.h>
#include <Platform/PlatformMath.h>
#include <Platform/Sync.h>
#include <Platform/VirtualMemory.h>

#include <sodium.h>

// SecretArena와 GetSecretAllocator 구현이다(Docs/Design/Crypto.ko.md 10.2절).
//
// 배치: [guard page][arena: 4 KiB chunk × 16][guard page]. guard page는 예약만 하고 commit하지 않아
// 앞뒤로 넘친 접근이 바로 접근 위반이 된다. chunk는 처음 쓸 때 한 크기 등급(64 B–4 KiB)을 맡고, 그
// 안의 슬롯 사용 여부를 64 bit 마스크로 기록한다. chunk가 4 KiB 경계에 있고 슬롯이 등급 크기의 배수
// 위치에 있으므로 슬롯은 등급 크기로 정렬된다.
//
// arena에 못 들어가는 할당(4 KiB 초과, 4 KiB 초과 정렬, arena 가득 참, arena 생성 실패)은 일반
// 힙으로 간다. 힙 블록 앞에는 이중 연결 목록 header를 두고, 해제 때 목록에서 주소를 찾은 뒤에만
// 역참조한다(남의 포인터를 읽지 않는다).
namespace rocks::crypto {
namespace {

constexpr usize ChunkBytes = 4096;
constexpr usize ChunkCount = 16;
constexpr usize ArenaBytes = ChunkBytes * ChunkCount;
constexpr uint32 MinimumSlotShift = 6;  // 64 B
constexpr uint32 MaximumSlotShift = 12; // 4 KiB = ChunkBytes
constexpr uint8 FreeChunk = 0xFF;
static_assert((usize{1} << MaximumSlotShift) == ChunkBytes);
static_assert(ChunkBytes >> MinimumSlotShift == 64,
              "가장 작은 등급의 슬롯 수가 64 bit 마스크와 같아야 한다");

// 힙 블록의 사용자 주소 바로 앞에 둔다. 사용자 주소는 블록 시작에서 offset만큼 떨어져 있다.
struct HeapHeader {
    HeapHeader* previous;
    HeapHeader* next;
    uint8* user;
    usize offset;
    usize bytes;
};

// 힙 사용자 주소의 최소 정렬이자 header 자리다. arena 슬롯과 같은 64 byte 정렬을 준다.
constexpr usize HeapMinimumOffset = 64;
static_assert(sizeof(HeapHeader) <= HeapMinimumOffset);

usize RoundUp(usize value, usize multiple) noexcept {
    return (value + multiple - 1) / multiple * multiple;
}

// 64 이상인 2의 거듭제곱 등급의 shift다. value는 1 이상 4096 이하다.
uint32 SlotShift(usize value) noexcept {
    const uint32 shift =
        64u - platform::PlatformMath::CountLeadingZeros64(static_cast<uint64>(value - 1));
    return shift < MinimumSlotShift ? MinimumSlotShift : shift;
}

class SecretAllocator final : public Allocator {
public:
    SecretAllocator() noexcept {
        for (usize chunk = 0; chunk < ChunkCount; ++chunk) {
            chunkShift_[chunk] = FreeChunk;
            chunkUsed_[chunk] = 0;
        }

        CreateArena();
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        (void)tag;
        if (!IsPowerOfTwo(alignment)) {
            return Result<void*>::Failure(ErrorCode::InvalidArgument);
        }

        if (bytes == 0) {
            return Result<void*>::Success(nullptr);
        }

        if (arena_ != nullptr && bytes <= ChunkBytes && alignment <= ChunkBytes) {
            const uint32 shift = SlotShift(bytes > alignment ? bytes : alignment);
            platform::LockGuard guard(mutex_);
            if (void* slot = TryAllocateSlot(shift)) {
                return Result<void*>::Success(slot);
            }
        }

        return AllocateHeap(bytes, alignment);
    }

    Result<void> Deallocate(void* address) noexcept override {
        if (address == nullptr) {
            return Result<void>::Success();
        }

        uint8* const pointer = static_cast<uint8*>(address);
        if (arena_ != nullptr && pointer >= arena_ && pointer < arena_ + ArenaBytes) {
            platform::LockGuard guard(mutex_);
            return FreeSlot(pointer);
        }

        HeapHeader* header = nullptr;
        {
            platform::LockGuard guard(mutex_);
            for (HeapHeader* entry = heap_; entry != nullptr; entry = entry->next) {
                if (entry->user == pointer) {
                    header = entry;
                    break;
                }
            }

            if (header == nullptr) {
                return Result<void>::Failure(ErrorCode::NotFound);
            }

            if (header->previous != nullptr) {
                header->previous->next = header->next;
            } else {
                heap_ = header->next;
            }

            if (header->next != nullptr) {
                header->next->previous = header->previous;
            }

            --heapAllocations_;
            heapBytes_ -= header->bytes;
        }

        // 목록에서 뺀 블록은 이 스레드만 본다. header까지 블록 전체를 지운 뒤 돌려준다.
        uint8* const block = pointer - header->offset;
        sodium_memzero(block, header->offset + header->bytes);
        platform::AlignedFree(block);
        return Result<void>::Success();
    }

    SecretMemoryStatus Status() noexcept {
        platform::LockGuard guard(mutex_);
        SecretMemoryStatus status;
        status.arenaAvailable = arena_ != nullptr;
        status.arenaLocked = locked_;
        status.dumpExclusionRequested = dumpExclusionRequested_;
        status.arenaCapacityBytes = arena_ != nullptr ? ArenaBytes : 0;
        status.arenaUsedBytes = arenaUsedBytes_;
        status.heapAllocations = heapAllocations_;
        status.heapBytes = heapBytes_;
        return status;
    }

private:
    // 생성 실패는 Fatal이 아니다. arena 없이도 소거는 보장되므로 힙만 쓰고 상태로 보고한다.
    void CreateArena() noexcept {
        // 페이지 경계가 chunk 경계여야 슬롯 정렬이 성립한다. 페이지는 4 KiB의 배수(4·16·64 KiB)여야
        // 한다.
        platform::VirtualMemoryInfo info{};
        if (!platform::QueryVirtualMemoryInfo(info) || info.pageSize < ChunkBytes ||
            info.pageSize % ChunkBytes != 0) {
            return;
        }

        const usize guard = info.pageSize;
        const usize committed = RoundUp(ArenaBytes, info.pageSize);
        if (!region_.Reserve(guard + committed + guard)) {
            return;
        }

        if (!region_.Commit(guard, committed)) {
            (void)region_.Release();
            return;
        }

        uint8* const arena = static_cast<uint8*>(region_.Address()) + guard;
        // sodium_mlock은 Linux·Android에서 MADV_DONTDUMP를 먼저 요청하고 mlock한다(Windows는
        // VirtualLock). 잠금 실패는 한도 문제이므로 잠그지 않은 채 계속한다.
        locked_ = sodium_mlock(arena, committed) == 0;
        dumpExclusionRequested_ = IsAndroid;
        arena_ = arena;
    }

    // mutex_를 잡고 부른다. shift 등급의 빈 슬롯을 찾고, 없으면 빈 chunk 하나에 등급을 맡긴다.
    void* TryAllocateSlot(uint32 shift) noexcept {
        const usize slotBytes = usize{1} << shift;
        const usize slots = ChunkBytes >> shift;
        const uint64 full = slots == 64 ? ~uint64{0} : (uint64{1} << slots) - 1;

        // 같은 등급 chunk의 빈 슬롯을 먼저 쓰고, 없을 때만 빈 chunk에 새로 등급을 맡긴다(등급이
        // 섞여 조각나는 것을 줄인다).
        usize chosen = ChunkCount;
        usize firstFree = ChunkCount;
        for (usize chunk = 0; chunk < ChunkCount; ++chunk) {
            if (chunkShift_[chunk] == shift && chunkUsed_[chunk] != full) {
                chosen = chunk;
                break;
            }

            if (firstFree == ChunkCount && chunkShift_[chunk] == FreeChunk) {
                firstFree = chunk;
            }
        }

        if (chosen == ChunkCount) {
            chosen = firstFree;
        }

        if (chosen == ChunkCount) {
            return nullptr;
        }

        chunkShift_[chosen] = static_cast<uint8>(shift);
        const uint32 slot =
            platform::PlatformMath::CountTrailingZeros64(~chunkUsed_[chosen] & full);
        chunkUsed_[chosen] |= uint64{1} << slot;
        arenaUsedBytes_ += slotBytes;
        return arena_ + chosen * ChunkBytes + slot * slotBytes;
    }

    // mutex_를 잡고 부른다. 이 arena가 준 살아 있는 슬롯이 아니면 NotFound다.
    Result<void> FreeSlot(uint8* pointer) noexcept {
        const usize offset = static_cast<usize>(pointer - arena_);
        const usize chunk = offset / ChunkBytes;
        const uint8 shift = chunkShift_[chunk];
        if (shift == FreeChunk) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        const usize slotBytes = usize{1} << shift;
        const usize within = offset % ChunkBytes;
        const uint64 bit = uint64{1} << (within >> shift);
        if (within % slotBytes != 0 || (chunkUsed_[chunk] & bit) == 0) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        // 슬롯 전체를 지운다. 요청 크기보다 큰 등급 슬롯의 뒷부분에도 사용자가 쓴 값이 있을 수
        // 있다.
        sodium_memzero(pointer, slotBytes);
        chunkUsed_[chunk] &= ~bit;
        arenaUsedBytes_ -= slotBytes;
        if (chunkUsed_[chunk] == 0) {
            chunkShift_[chunk] = FreeChunk;
        }

        return Result<void>::Success();
    }

    Result<void*> AllocateHeap(usize bytes, usize alignment) noexcept {
        const usize offset = alignment > HeapMinimumOffset ? alignment : HeapMinimumOffset;
        if (bytes > MaxSize - offset) {
            return Result<void*>::Failure(ErrorCode::Overflow);
        }

        uint8* const block = static_cast<uint8*>(platform::AlignedAllocate(offset + bytes, offset));
        if (block == nullptr) {
            return Result<void*>::Failure(ErrorCode::OutOfMemory);
        }

        uint8* const user = block + offset;
        HeapHeader* const header = reinterpret_cast<HeapHeader*>(user - sizeof(HeapHeader));
        header->previous = nullptr;
        header->user = user;
        header->offset = offset;
        header->bytes = bytes;

        platform::LockGuard guard(mutex_);
        header->next = heap_;
        if (heap_ != nullptr) {
            heap_->previous = header;
        }

        heap_ = header;
        ++heapAllocations_;
        heapBytes_ += bytes;
        return Result<void*>::Success(user);
    }

    platform::Mutex mutex_;
    platform::VirtualMemoryRegion region_;
    // 생성 뒤 바뀌지 않는다(nullptr이면 arena 없음).
    uint8* arena_ = nullptr;
    bool locked_ = false;
    bool dumpExclusionRequested_ = false;

    // 아래는 mutex_가 보호한다. chunkShift_는 chunk가 맡은 등급의 shift(빈 chunk는 FreeChunk)다.
    uint8 chunkShift_[ChunkCount];
    uint64 chunkUsed_[ChunkCount];
    usize arenaUsedBytes_ = 0;
    HeapHeader* heap_ = nullptr;
    usize heapAllocations_ = 0;
    usize heapBytes_ = 0;
};

SecretAllocator& Instance() noexcept {
    // 일부러 파괴하지 않는다. 정적 객체의 소멸 순서와 무관하게 마지막 해제(전역 키 등)까지 쓸 수
    // 있어야 하고, 잠근 페이지는 프로세스와 함께 OS가 거둔다. 함수 안 static 초기화는 동시 첫
    // 호출에도 한 번만 실행된다.
    alignas(SecretAllocator) static unsigned char storage[sizeof(SecretAllocator)];
    static SecretAllocator* const instance = ConstructAt<SecretAllocator>(storage);
    return *instance;
}

} // namespace

SecretMemoryStatus GetSecretMemoryStatus() noexcept {
    return Instance().Status();
}

Allocator& GetSecretAllocator() noexcept {
    return Instance();
}

} // namespace rocks::crypto
