#include "TraceSessionInternal.h"

#include <Base/Endian.h>
#include <Core/Hash/TypeHash.h>
#include <Platform/Memory.h>

// 세션 전역 이름 표다(Docs/Design/TraceSystem.ko.md 결정 T4). 같은 이름은 세션 안에서 id 하나다.
namespace rocks::detail {

using platform::AtomicLoad;
using platform::AtomicStore;

namespace {

// 이름 byte 저장소 덩어리 크기다. 덩어리 첫 8 byte는 다음 덩어리 주소다.
constexpr usize NameChunkBytes = 64 * 1024;
constexpr usize NameChunkHeaderBytes = sizeof(uint8*);
static_assert(TraceStandardFormat::MaxScopeNameBytes + NameChunkHeaderBytes <= NameChunkBytes,
              "a scope name must fit in one name chunk");

} // namespace

Result<void> TraceNameTable::Initialize(Allocator& allocator, usize capacity) noexcept {
    auto memory = allocator.Allocate(capacity * sizeof(Slot), alignof(Slot));
    if (!memory) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    allocator_ = &allocator;
    slots_ = static_cast<Slot*>(memory.Value());
    platform::MemoryZero(slots_, capacity * sizeof(Slot));
    mask_ = capacity - 1;
    return Result<void>::Success();
}

void TraceNameTable::Release() noexcept {
    while (chunk_ != nullptr) {
        uint8* next = nullptr;
        platform::MemoryCopy(&next, chunk_, sizeof(next));
        auto freed = allocator_->Deallocate(chunk_);
        (void)freed;
        chunk_ = next;
    }

    if (slots_ != nullptr) {
        auto freed = allocator_->Deallocate(slots_);
        (void)freed;
        slots_ = nullptr;
    }
}

uint64 TraceNameTable::HashName(StringView name) noexcept {
    const uint64 hash = MixHash(HashStringBytes(name));
    return hash != 0 ? hash : 1;
}

uint32 TraceNameTable::Find(StringView name, uint64 hash, const char*& stored,
                            uint32& storedLength) const noexcept {
    for (usize probe = 0; probe <= mask_; ++probe) {
        const Slot& slot = slots_[(hash + probe) & mask_];
        // 해시를 acquire로 읽으면 삽입 때 해시보다 먼저 쓴 text·length·id가 보인다.
        const uint64 slotHash = AtomicLoad<MemoryOrder::Acquire>(&slot.hash);
        if (slotHash == 0) {
            return 0;
        }

        if (slotHash == hash && slot.length == name.ByteCount() &&
            platform::MemoryEquals(slot.text, name.Data(), name.ByteCount())) {
            stored = slot.text;
            storedLength = slot.length;
            return slot.id;
        }
    }

    return 0;
}

char* TraceNameTable::CopyText(StringView name) noexcept {
    if (chunk_ == nullptr || chunkBytes_ - chunkUsed_ < name.ByteCount()) {
        auto memory = allocator_->Allocate(NameChunkBytes, alignof(uint8*));
        if (!memory) {
            return nullptr;
        }

        auto* fresh = static_cast<uint8*>(memory.Value());
        platform::MemoryCopy(fresh, &chunk_, sizeof(chunk_));
        chunk_ = fresh;
        chunkUsed_ = NameChunkHeaderBytes;
        chunkBytes_ = NameChunkBytes;
    }

    char* text = reinterpret_cast<char*>(chunk_ + chunkUsed_);
    platform::MemoryCopy(text, name.Data(), name.ByteCount());
    chunkUsed_ += name.ByteCount();
    return text;
}

uint32 TraceNameTable::FindOrInsert(TraceSessionState& session, StringView name, uint64 hash,
                                    bool truncated, const char*& stored,
                                    uint32& storedLength) noexcept {
    // name은 이미 MaxScopeNameBytes 이하로 자른 값이다(ResolveTraceName).
    if (const uint32 id = Find(name, hash, stored, storedLength); id != 0) {
        return id;
    }

    platform::LockGuard lock(insertMutex_);
    if (const uint32 id = Find(name, hash, stored, storedLength); id != 0) {
        return id;
    }

    // 열린 주소법이 길게 늘어지지 않도록 칸의 3/4까지만 쓴다.
    if ((count_ + 1) * 4 > (mask_ + 1) * 3) {
        platform::AtomicFetchAdd<MemoryOrder::Relaxed>(&rejected_, 1);
        return 0;
    }

    char* text = CopyText(name);
    if (text == nullptr) {
        platform::AtomicFetchAdd<MemoryOrder::Relaxed>(&rejected_, 1);
        return 0;
    }

    // 정의를 먼저 정의 버퍼에 넣고 칸을 공개한다. 그래서 이 id를 쓰는 구간 기록은 항상 이름 정의
    // 뒤에 나간다. 정의 버퍼가 차 있으면 넣지 않는다(복사한 byte는 저장소에 남고, 다음 요청이 다시
    // 시도한다).
    const uint32 id = static_cast<uint32>(count_ + 1);
    uint8 fixed[TraceStandardFormat::ScopeNameFixedBytes];
    StoreLittleEndian<uint32>(fixed + TraceStandardFormat::ScopeNameIdOffset, id);
    fixed[TraceStandardFormat::ScopeNameTruncatedOffset] = truncated ? 1 : 0;
    const TraceVariablePart parts[] = {{text, name.ByteCount()}};
    if (!WriteStandardDefinition(session, TraceScopeNameType, fixed, sizeof(fixed), parts)) {
        return 0;
    }

    usize index = hash & mask_;
    while (AtomicLoad<MemoryOrder::Relaxed>(&slots_[index].hash) != 0) {
        index = (index + 1) & mask_;
    }

    Slot& slot = slots_[index];
    slot.text = text;
    slot.length = static_cast<uint32>(name.ByteCount());
    slot.id = id;
    AtomicStore<MemoryOrder::Release>(&slot.hash, hash);
    AtomicStore<MemoryOrder::Relaxed>(&count_, count_ + 1);
    stored = text;
    storedLength = slot.length;
    return id;
}

} // namespace rocks::detail
