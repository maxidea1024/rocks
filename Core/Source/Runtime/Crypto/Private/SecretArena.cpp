#include "SecretArena.h"

#include <Crypto/Secret.h>

#include <Base/Checked.h>
#include <Base/Compiler.h>
#include <Core/Containers/HashMap.h>
#include <Core/Memory/AddressSanitizer.h>
#include <Core/Move.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <Platform/PlatformMath.h>
#include <Platform/Sync.h>
#include <Platform/VirtualMemory.h>

#include <sodium.h>

// 비밀 메모리 구현이다(Docs/Design/Crypto.ko.md 10.2절). 두 저장소를 나눈다.
//
// - 키 저장소(SecretArena): 키·키 상태 전용. [guard page][64 KiB][guard page] arena를 필요할 때
//   덧붙인다(최대 64개). chunk(4 KiB)가 처음 쓸 때 크기 등급 하나를 맡고, 슬롯 사용을 64 bit
//   마스크로 기록한다. 슬롯은 등급 크기로 정렬된다. 버퍼가 이 저장소를 쓰지 않으므로 큰 평문 버퍼가
//   키 자리를 차지해 키가 잠금 없는 메모리로 밀려나는 일이 없다.
// - 버퍼 할당기(GetSecretAllocator): 평문이 지나가는 버퍼용. 일반 힙에서 할당하고 해제 때 요청 크기
//   전체를 지운다. 블록 크기는 블록 밖 표(주소 → 크기)에 두므로, 버퍼 앞을 넘겨 쓰는 결함이 할당기
//   자료 구조를 망가뜨리지 않고 남의 주소도 역참조하지 않는다.
namespace rocks::crypto {
namespace {

constexpr usize ChunkBytes = 4096;
constexpr usize ChunkCount = detail::SecretArenaBytes / ChunkBytes;
constexpr uint32 MinimumSlotShift = 6;  // 64 B
constexpr uint32 MaximumSlotShift = 12; // 4 KiB = ChunkBytes
constexpr uint8 FreeChunk = 0xFF;
static_assert((usize{1} << MaximumSlotShift) == ChunkBytes &&
              ChunkBytes == detail::SecretSlotMaxBytes);
static_assert(ChunkBytes >> MinimumSlotShift == 64,
              "가장 작은 등급의 슬롯 수가 64 bit 마스크와 같아야 한다");
static_assert(ChunkCount * ChunkBytes == detail::SecretArenaBytes);

// 버퍼 할당기의 최소 정렬이다. SIMD 경로와 키 확장 상태가 기대할 수 있는 정렬을 슬롯과 같게 준다.
constexpr usize BufferMinimumAlignment = 64;

usize RoundUp(usize value, usize multiple) noexcept {
    return (value + multiple - 1) / multiple * multiple;
}

// 64 이상인 2의 거듭제곱 등급의 shift다. value는 1 이상 4096 이하다.
uint32 SlotShift(usize value) noexcept {
    const uint32 shift =
        64u - platform::PlatformMath::CountLeadingZeros64(static_cast<uint64>(value - 1));
    return shift < MinimumSlotShift ? MinimumSlotShift : shift;
}

// arena 하나다. region_ 소유와 슬롯 기록을 묶는다. 모든 멤버 접근은 SecretArenaSet의 mutex가
// 보호한다.
struct Arena {
    platform::VirtualMemoryRegion region;
    uint8* begin = nullptr;
    bool locked = false;
    uint8 chunkShift[ChunkCount];
    uint64 chunkUsed[ChunkCount];
};

class SecretArenaSet {
public:
    Result<void*> Allocate(usize bytes, usize alignment) noexcept {
        if (bytes == 0 || bytes > ChunkBytes || !IsPowerOfTwo(alignment) ||
            alignment > ChunkBytes) {
            return Result<void*>::Failure(ErrorCode::InvalidArgument);
        }

        const uint32 shift = SlotShift(bytes > alignment ? bytes : alignment);
        platform::LockGuard guard(mutex_);
        for (usize index = 0; index < arenaCount_; ++index) {
            if (void* slot = TryAllocateSlot(arenas_[index], shift, bytes)) {
                return Result<void*>::Success(slot);
            }
        }

        if (arenaCount_ == detail::MaxSecretArenas || !CreateArena(arenas_[arenaCount_])) {
            return Result<void*>::Failure(ErrorCode::OutOfMemory);
        }

        ++arenaCount_;
        return Result<void*>::Success(TryAllocateSlot(arenas_[arenaCount_ - 1], shift, bytes));
    }

    void Free(void* address) noexcept {
        if (address == nullptr) {
            return;
        }

        const uptr value = reinterpret_cast<uptr>(address);
        platform::LockGuard guard(mutex_);
        for (usize index = 0; index < arenaCount_; ++index) {
            const uptr begin = reinterpret_cast<uptr>(arenas_[index].begin);
            if (value >= begin && value - begin < detail::SecretArenaBytes) {
                FreeSlot(arenas_[index], value - begin);
                return;
            }
        }

        platform::Fatal("Crypto: freed a secret slot that the secret arena does not own");
    }

    bool TryGetRange(usize index, const uint8*& begin, usize& bytes) noexcept {
        platform::LockGuard guard(mutex_);
        if (index >= arenaCount_) {
            return false;
        }

        begin = arenas_[index].begin;
        bytes = detail::SecretArenaBytes;
        return true;
    }

    void FillStatus(SecretMemoryStatus& status) noexcept {
        platform::LockGuard guard(mutex_);
        status.arenaCount = arenaCount_;
        status.arenaCapacityBytes = arenaCount_ * detail::SecretArenaBytes;
        status.arenaUsedBytes = usedBytes_;
        status.arenaLockedBytes = 0;
        for (usize index = 0; index < arenaCount_; ++index) {
            status.arenaLockedBytes += arenas_[index].locked ? detail::SecretArenaBytes : 0;
        }

        // libsodium은 MADV_DONTDUMP가 있을 때만(이 대상 중 Android) madvise를 부르고 결과는
        // 확인하지 않는다.
        status.dumpExclusionRequested = IsAndroid && arenaCount_ != 0;
    }

private:
    // 예약·commit 실패는 OutOfMemory로 돌려준다. 잠금 실패는 한도 문제이므로 잠그지 않은 채
    // 쓴다(상태로 보고).
    static bool CreateArena(Arena& arena) noexcept {
        // 페이지 경계가 chunk 경계여야 슬롯 정렬이 성립한다. 페이지는 4 KiB의 배수(4·16·64 KiB)여야
        // 한다.
        platform::VirtualMemoryInfo info{};
        if (!platform::QueryVirtualMemoryInfo(info) || info.pageSize < ChunkBytes ||
            info.pageSize % ChunkBytes != 0) {
            return false;
        }

        const usize guard = info.pageSize;
        const usize committed = RoundUp(detail::SecretArenaBytes, info.pageSize);
        if (!arena.region.Reserve(guard + committed + guard)) {
            return false;
        }

        // commit 실패는 OutOfMemory로 돌려주고 레코드를 다음 시도에 다시 쓴다. 예약을 풀지 못하면
        // 레코드가 예약을 쥔 채 남아 이후 arena 증가가 모두 막히므로 결함으로
        // 멈춘다(VirtualMemoryRegion 소멸의 해제 실패와 같은 규칙).
        if (!arena.region.Commit(guard, committed)) {
            if (!arena.region.Release()) {
                platform::Fatal(
                    "Crypto: could not release a secret arena reservation after a failed commit");
            }

            return false;
        }

        arena.begin = static_cast<uint8*>(arena.region.Address()) + guard;
        // sodium_mlock은 Android에서 MADV_DONTDUMP를 먼저 요청하고 mlock한다(Windows는
        // VirtualLock).
        arena.locked = sodium_mlock(arena.begin, committed) == 0;
        for (usize chunk = 0; chunk < ChunkCount; ++chunk) {
            arena.chunkShift[chunk] = FreeChunk;
            arena.chunkUsed[chunk] = 0;
        }

        // 쓰지 않는 슬롯은 ASan에서 접근 금지다. 할당할 때 요청 크기만큼만 푼다.
        AsanPoisonMemory(arena.begin, detail::SecretArenaBytes);
        return true;
    }

    // shift 등급의 빈 슬롯을 찾고, 없으면 빈 chunk 하나에 등급을 맡긴다. 같은 등급 chunk를 먼저
    // 채워 등급이 섞여 조각나는 것을 줄인다.
    void* TryAllocateSlot(Arena& arena, uint32 shift, usize bytes) noexcept {
        const usize slotBytes = usize{1} << shift;
        const usize slots = ChunkBytes >> shift;
        const uint64 full = slots == 64 ? ~uint64{0} : (uint64{1} << slots) - 1;
        usize chosen = ChunkCount;
        usize firstFree = ChunkCount;
        for (usize chunk = 0; chunk < ChunkCount; ++chunk) {
            if (arena.chunkShift[chunk] == shift && arena.chunkUsed[chunk] != full) {
                chosen = chunk;
                break;
            }

            if (firstFree == ChunkCount && arena.chunkShift[chunk] == FreeChunk) {
                firstFree = chunk;
            }
        }

        if (chosen == ChunkCount) {
            chosen = firstFree;
        }

        if (chosen == ChunkCount) {
            return nullptr;
        }

        arena.chunkShift[chosen] = static_cast<uint8>(shift);
        const uint32 slot =
            platform::PlatformMath::CountTrailingZeros64(~arena.chunkUsed[chosen] & full);
        arena.chunkUsed[chosen] |= uint64{1} << slot;
        usedBytes_ += slotBytes;
        uint8* const address = arena.begin + chosen * ChunkBytes + slot * slotBytes;
        // 빈 슬롯은 0이다(새로 commit한 페이지이거나 해제 때 지운 슬롯). 요청 크기 뒤 꼬리는
        // poison으로 남긴다.
        AsanUnpoisonMemory(address, bytes);
        return address;
    }

    // offset은 arena 시작 기준이다. 살아 있는 슬롯의 시작이 아니면 Fatal이다.
    void FreeSlot(Arena& arena, usize offset) noexcept {
        const usize chunk = offset / ChunkBytes;
        const uint8 shift = arena.chunkShift[chunk];
        if (shift == FreeChunk) {
            platform::Fatal("Crypto: freed a secret slot in an unused secret arena chunk");
        }

        const usize slotBytes = usize{1} << shift;
        const usize within = offset % ChunkBytes;
        const uint64 bit = uint64{1} << (within >> shift);
        if (within % slotBytes != 0 || (arena.chunkUsed[chunk] & bit) == 0) {
            platform::Fatal("Crypto: freed a secret slot twice or by an interior address");
        }

        // 슬롯 전체를 지운다. 요청 크기보다 큰 등급 슬롯의 꼬리에도 라이브러리가 쓴 값이 있을 수
        // 있다.
        uint8* const address = arena.begin + offset;
        AsanUnpoisonMemory(address, slotBytes);
        sodium_memzero(address, slotBytes);
        AsanPoisonMemory(address, slotBytes);
        arena.chunkUsed[chunk] &= ~bit;
        usedBytes_ -= slotBytes;
        if (arena.chunkUsed[chunk] == 0) {
            arena.chunkShift[chunk] = FreeChunk;
        }
    }

    platform::Mutex mutex_;
    Arena arenas_[detail::MaxSecretArenas];
    usize arenaCount_ = 0;
    usize usedBytes_ = 0;
};

// 버퍼 할당기의 블록 표(주소 → 크기)가 쓰는 저장소다. 기록 없는 플랫폼 정렬 할당이다(표 자체는
// 비밀이 아니다).
class PlatformTableAllocator final : public Allocator {
public:
    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        (void)tag;
        if (!IsPowerOfTwo(alignment)) {
            return Result<void*>::Failure(ErrorCode::InvalidArgument);
        }

        if (bytes == 0) {
            return Result<void*>::Success(nullptr);
        }

        void* memory = platform::AlignedAllocate(bytes, alignment);
        if (memory == nullptr) {
            return Result<void*>::Failure(ErrorCode::OutOfMemory);
        }

        return Result<void*>::Success(memory);
    }

    Result<void> Deallocate(void* address) noexcept override {
        if (address != nullptr) {
            platform::AlignedFree(address);
        }

        return Result<void>::Success();
    }
};

class SecretBufferAllocator final : public Allocator {
public:
    SecretBufferAllocator() noexcept : blocks_(tableAllocator_) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        (void)tag;
        if (!IsPowerOfTwo(alignment)) {
            return Result<void*>::Failure(ErrorCode::InvalidArgument);
        }

        if (bytes == 0) {
            return Result<void*>::Success(nullptr);
        }

        const usize effective =
            alignment > BufferMinimumAlignment ? alignment : BufferMinimumAlignment;
        if (bytes > MaxSize - effective) {
            return Result<void*>::Failure(ErrorCode::Overflow);
        }

        void* memory = platform::AlignedAllocate(bytes, effective);
        if (memory == nullptr) {
            return Result<void*>::Failure(ErrorCode::OutOfMemory);
        }

        {
            platform::LockGuard guard(mutex_);
            auto inserted = blocks_.Insert(reinterpret_cast<uptr>(memory), bytes);
            if (inserted && inserted.Value() == InsertOutcome::Inserted) {
                bytes_ += bytes;
                return Result<void*>::Success(memory);
            }

            // 플랫폼 할당기가 살아 있는 주소를 다시 줄 수는 없다. AlreadyPresent는 표 손상이다.
            if (inserted) {
                platform::Fatal(
                    "Crypto: secret buffer allocator table already holds a live address");
            }
        }

        // 표에 기록하지 못한 블록은 아직 아무도 쓰지 않았다. 지울 내용 없이 돌려준다.
        platform::AlignedFree(memory);
        return Result<void*>::Failure(ErrorCode::OutOfMemory);
    }

    Result<void> Deallocate(void* address) noexcept override {
        if (address == nullptr) {
            return Result<void>::Success();
        }

        usize bytes = 0;
        {
            platform::LockGuard guard(mutex_);
            const uptr key = reinterpret_cast<uptr>(address);
            const usize* found = blocks_.Find(key);
            if (found == nullptr) {
                return Result<void>::Failure(ErrorCode::NotFound);
            }

            bytes = *found;
            (void)blocks_.Erase(key);
            bytes_ -= bytes;
        }

        // 표에서 뺀 블록은 이 스레드만 본다. 요청 크기 전체를 지운 뒤 돌려준다.
        sodium_memzero(address, bytes);
        platform::AlignedFree(address);
        return Result<void>::Success();
    }

    void FillStatus(SecretMemoryStatus& status) noexcept {
        platform::LockGuard guard(mutex_);
        status.bufferAllocations = blocks_.Count();
        status.bufferBytes = bytes_;
    }

private:
    platform::Mutex mutex_;
    PlatformTableAllocator tableAllocator_;
    HashMap<uptr, usize> blocks_;
    usize bytes_ = 0;
};

// 두 저장소를 한 번에 만들고 일부러 파괴하지 않는다. 정적 객체의 소멸 순서와 무관하게 마지막
// 해제(전역 키 등)까지 쓸 수 있어야 하고, 잠근 페이지는 프로세스와 함께 OS가 거둔다. 함수 안 static
// 초기화는 동시 첫 호출에도 한 번만 실행된다.
struct SecretMemory {
    SecretArenaSet arenas;
    SecretBufferAllocator buffers;
};

SecretMemory& Instance() noexcept {
    alignas(SecretMemory) static unsigned char storage[sizeof(SecretMemory)];
    static SecretMemory* const instance = ConstructAt<SecretMemory>(storage);
    return *instance;
}

} // namespace

namespace detail {

Result<void*> TryAllocateSecretSlot(usize bytes, usize alignment) noexcept {
    return Instance().arenas.Allocate(bytes, alignment);
}

void FreeSecretSlot(void* slot) noexcept {
    Instance().arenas.Free(slot);
}

bool TryGetSecretArenaRange(usize index, const uint8*& begin, usize& bytes) noexcept {
    return Instance().arenas.TryGetRange(index, begin, bytes);
}

} // namespace detail

SecretMemoryStatus GetSecretMemoryStatus() noexcept {
    SecretMemoryStatus status;
    Instance().arenas.FillStatus(status);
    Instance().buffers.FillStatus(status);
    return status;
}

Allocator& GetSecretAllocator() noexcept {
    return Instance().buffers;
}

} // namespace rocks::crypto
