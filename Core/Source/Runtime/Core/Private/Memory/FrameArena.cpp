#include <Core/Memory/FrameArena.h>
#include <Core/Assert.h>
#include <Core/Memory/AddressSanitizer.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>

namespace rocks {

// 블록 머리다. payload는 머리 뒤 BlockHeaderBytes부터 블록 끝까지다.
struct FrameArena::Block {
    Block* next = nullptr;
    usize bytes = 0;
};

namespace {

// 블록은 cache line 정렬로 받고 머리도 그 크기로 채운다. 그래서 payload 시작이 64 byte 정렬이고,
// 그보다 큰 정렬만 블록 안에서 여유를 쓴다.
constexpr usize BlockAlignment = 64;
constexpr usize BlockHeaderBytes = 64;
static_assert(sizeof(void*) * 2 <= BlockHeaderBytes, "FrameArena block header must fit");

bool IsValidAlignment(usize alignment) noexcept {
    return alignment != 0 && (alignment & (alignment - 1)) == 0;
}

unsigned char* Payload(void* block) noexcept {
    return static_cast<unsigned char*>(block) + BlockHeaderBytes;
}

} // namespace

FrameArena::FrameArena(Allocator& backing, const FrameArenaConfig& config) noexcept
    : backing_(backing), blockBytes_(config.blockBytes < FrameArenaConfig::MinBlockBytes
                                         ? FrameArenaConfig::MinBlockBytes
                                         : config.blockBytes),
      maxRetainedBlocks_(config.maxRetainedBlocks) {
}

FrameArena::~FrameArena() noexcept {
    if (platform::AtomicLoad(&openWriters_) != 0) {
        platform::Fatal("FrameArena destroyed with an open FrameArenaWriter");
    }

    ROCKS_ASSERT(Reset().IsOk());
    ROCKS_ASSERT(Trim().IsOk());
}

Result<void*> FrameArena::TryAllocate(usize bytes, usize alignment) noexcept {
    if (!IsValidAlignment(alignment)) {
        return Result<void*>::Failure(ErrorCode::InvalidArgument);
    }

    if (bytes == 0) {
        return Result<void*>::Success(nullptr);
    }

    return AllocateFrom(sharedNext_, sharedEnd_, bytes, alignment);
}

Result<void> FrameArena::Reset() noexcept {
    {
        platform::LockGuard lock(gate_);
        if (resetting_ || platform::AtomicLoad(&openWriters_) != 0) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        resetting_ = true;
    }

    // 등록 목록은 머리에 넣는 단일 연결 목록이라 앞에서부터 부르면 등록 역순이다. 소멸자 안의 할당·
    // Reset은 resetting_ 때문에 Busy로 거부되므로 이 목록은 더 자라지 않는다.
    detail::FrameArenaDestructor* record = platform::AtomicExchange(&destructors_, nullptr);
    while (record != nullptr) {
        detail::FrameArenaDestructor* next = record->next;
        record->destroy(record->objects, record->count);
        record = next;
    }

    Block* release = nullptr;
    {
        platform::LockGuard lock(gate_);
        // 대형 블록은 수명마다 크기가 달라 남기지 않는다.
        while (largeBlocks_ != nullptr) {
            Block* block = largeBlocks_;
            largeBlocks_ = block->next;
            block->next = release;
            release = block;
        }

        // 표준 블록은 남길 수까지 비워 다음 수명에 다시 쓴다.
        while (usedBlocks_ != nullptr) {
            Block* block = usedBlocks_;
            usedBlocks_ = block->next;
            if (freeCount_ < maxRetainedBlocks_) {
                AsanPoisonMemory(Payload(block), block->bytes - BlockHeaderBytes);
                block->next = freeBlocks_;
                freeBlocks_ = block;
                ++freeCount_;
            } else {
                block->next = release;
                release = block;
            }
        }

        usedCount_ = 0;
        largeCount_ = 0;
        sharedNext_ = nullptr;
        sharedEnd_ = nullptr;
        platform::AtomicStore(&destructorCount_, uint64{0});
        platform::AtomicStore(&requestedBytes_, uint64{0});
        ++resets_;
    }

    // backing 반환은 잠금 밖에서 한다. resetting_이 아직 켜져 있어 다른 호출이 목록을 바꾸지
    // 않는다.
    while (release != nullptr) {
        Block* next = release->next;
        ReleaseBlock(release);
        release = next;
    }

    platform::LockGuard lock(gate_);
    resetting_ = false;
    return Result<void>::Success();
}

Result<void> FrameArena::Trim() noexcept {
    Block* release = nullptr;
    {
        platform::LockGuard lock(gate_);
        if (resetting_) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        release = freeBlocks_;
        freeBlocks_ = nullptr;
        freeCount_ = 0;
    }
    while (release != nullptr) {
        Block* next = release->next;
        ReleaseBlock(release);
        release = next;
    }

    return Result<void>::Success();
}

FrameArenaStats FrameArena::Stats() const noexcept {
    platform::LockGuard lock(gate_);
    FrameArenaStats stats;
    stats.usedBlocks = usedCount_;
    stats.largeBlocks = largeCount_;
    stats.retainedBlocks = freeCount_;
    stats.requestedBytes = platform::AtomicLoad(&requestedBytes_);
    stats.destructors = platform::AtomicLoad(&destructorCount_);
    stats.openWriters = platform::AtomicLoad(&openWriters_);
    stats.resets = resets_;
    return stats;
}

Result<FrameArena::Block*> FrameArena::AcquireBlock() noexcept {
    // gate_ 안에서 부른다.
    Block* block = freeBlocks_;
    if (block != nullptr) {
        freeBlocks_ = block->next;
        --freeCount_;
    } else {
        auto memory = backing_.Allocate(blockBytes_, BlockAlignment);
        if (!memory) {
            return Result<Block*>::Failure(memory.Error());
        }

        block = ConstructAt<Block>(memory.Value());
        block->bytes = blockBytes_;
        AsanPoisonMemory(Payload(block), blockBytes_ - BlockHeaderBytes);
    }

    block->next = usedBlocks_;
    usedBlocks_ = block;
    ++usedCount_;
    return Result<Block*>::Success(block);
}

bool FrameArena::TryBump(unsigned char*& next, unsigned char* end, usize bytes, usize alignment,
                         void*& result) noexcept {
    // cursor가 비어 있으면(nullptr) 둘 다 0이라 늘 실패한다.
    const auto address = reinterpret_cast<uptr>(next);
    const uptr aligned = (address + alignment - 1) & ~(static_cast<uptr>(alignment) - 1);
    const auto limit = reinterpret_cast<uptr>(end);
    if (aligned < address || aligned > limit || limit - aligned < bytes) {
        return false;
    }

    result = reinterpret_cast<void*>(aligned);
    next = reinterpret_cast<unsigned char*>(aligned + bytes);
    return true;
}

Result<void*> FrameArena::AllocateLarge(usize bytes, usize alignment) noexcept {
    usize total = 0;
    if (!TryAdd(bytes, alignment - 1, total) || !TryAdd(total, BlockHeaderBytes, total)) {
        return Result<void*>::Failure(ErrorCode::Overflow);
    }

    platform::LockGuard lock(gate_);
    if (resetting_) {
        return Result<void*>::Failure(ErrorCode::Busy);
    }

    auto memory = backing_.Allocate(total, BlockAlignment);
    if (!memory) {
        return Result<void*>::Failure(memory.Error());
    }

    Block* block = ConstructAt<Block>(memory.Value());
    block->bytes = total;
    block->next = largeBlocks_;
    largeBlocks_ = block;
    ++largeCount_;
    unsigned char* next = Payload(block);
    void* result = nullptr;
    ROCKS_ASSERT(TryBump(next, static_cast<unsigned char*>(memory.Value()) + total, bytes,
                         alignment, result));
    platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&requestedBytes_, uint64{bytes});
    return Result<void*>::Success(result);
}

Result<void*> FrameArena::AllocateFrom(unsigned char*& next, unsigned char*& end, usize bytes,
                                       usize alignment) noexcept {
    // 정렬 여유를 더해 표준 payload의 절반을 넘으면 대형 블록이다. 표준 블록에 넣으면 남는 공간이
    // 크게 버려지고, 블록보다 큰 요청은 아예 들어가지 않는다.
    usize worst = 0;
    const usize payloadBytes = blockBytes_ - BlockHeaderBytes;
    if (!TryAdd(bytes, alignment - 1, worst)) {
        return Result<void*>::Failure(ErrorCode::Overflow);
    }

    if (worst > payloadBytes / 2) {
        return AllocateLarge(bytes, alignment);
    }

    platform::LockGuard lock(gate_);
    if (resetting_) {
        return Result<void*>::Failure(ErrorCode::Busy);
    }

    void* result = nullptr;
    if (!TryBump(next, end, bytes, alignment, result)) {
        auto block = AcquireBlock();
        if (!block) {
            return Result<void*>::Failure(block.Error());
        }

        // 이전 블록의 남은 공간은 Reset까지 쓰지 않는다. 새 블록은 비어 있고 worst가 payload의 절반
        // 이하이므로 반드시 들어간다.
        next = Payload(block.Value());
        end = reinterpret_cast<unsigned char*>(block.Value()) + block.Value()->bytes;
        ROCKS_ASSERT(TryBump(next, end, bytes, alignment, result));
    }

    AsanUnpoisonMemory(result, bytes);
    platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&requestedBytes_, uint64{bytes});
    return Result<void*>::Success(result);
}

void FrameArena::PushDestructor(detail::FrameArenaDestructor* record) noexcept {
    // 여러 writer가 잠금 없이 등록하므로 머리에 원자적으로 넣는다(Release로 기록 내용을 게시한다).
    detail::FrameArenaDestructor* head =
        platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&destructors_);
    do {
        record->next = head;
    } while (!platform::AtomicCompareExchangeWeak<platform::MemoryOrder::Release,
                                                  platform::MemoryOrder::Relaxed>(&destructors_,
                                                                                  head, record));
    platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&destructorCount_, uint64{1});
}

void FrameArena::ReleaseBlock(Block* block) noexcept {
    // backing에 돌려주기 전에 poison을 푼다(backing이 그 메모리를 다시 쓸 수 있게).
    AsanUnpoisonMemory(Payload(block), block->bytes - BlockHeaderBytes);
    DestroyAt(block);
    if (!backing_.Deallocate(block).IsOk()) {
        platform::Fatal("FrameArena backing rejected its own block");
    }
}

void FrameArena::OpenWriter() noexcept {
    platform::AtomicFetchAdd(&openWriters_, uint64{1});
}

void FrameArena::CloseWriter() noexcept {
    platform::AtomicFetchSub(&openWriters_, uint64{1});
}

FrameArenaWriter::FrameArenaWriter(FrameArena& arena) noexcept : arena_(arena) {
    arena_.OpenWriter();
}

FrameArenaWriter::~FrameArenaWriter() noexcept {
    arena_.CloseWriter();
}

Result<void*> FrameArenaWriter::TryAllocate(usize bytes, usize alignment) noexcept {
    if (!IsValidAlignment(alignment)) {
        return Result<void*>::Failure(ErrorCode::InvalidArgument);
    }

    if (bytes == 0) {
        return Result<void*>::Success(nullptr);
    }

    // 대형 요청은 자기 블록에 자리가 남아 있어도 대형 블록으로 보낸다(공유 경로와 같은 판정). 그
    // 밖의 요청은 자기 블록에 들어가면 잠금 없이 끝낸다.
    usize worst = 0;
    const bool small =
        TryAdd(bytes, alignment - 1, worst) && worst <= (arena_.blockBytes_ - BlockHeaderBytes) / 2;
    void* result = nullptr;
    if (small && FrameArena::TryBump(next_, end_, bytes, alignment, result)) {
        AsanUnpoisonMemory(result, bytes);
        platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&arena_.requestedBytes_,
                                                                 uint64{bytes});
        return Result<void*>::Success(result);
    }

    return arena_.AllocateFrom(next_, end_, bytes, alignment);
}

} // namespace rocks
