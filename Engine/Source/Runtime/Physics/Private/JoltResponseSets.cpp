// 응답 묶음 표(Docs/Design/PhysicsModule 6.1절, 7.1.6절).
#include "JoltResponseSets.h"

#include "JoltLayers.h"

namespace rocks::physics {

namespace {

// 칸 0의 응답이다(모든 채널 Block — CollisionResponses 기본값).
constexpr CollisionResponses AllBlock{};

// 색인 bit가 담을 수 있는 가장 큰 색인이다.
constexpr uint32 MaxResponseSetIndex = LayerResponseSetMask;

} // namespace

ResponseSetTable::ResponseSetTable(Allocator& allocator) noexcept
    : entries_(allocator), free_(allocator) {
}

Result<uint32> ResponseSetTable::Acquire(const CollisionResponses& responses) noexcept {
    if (responses == AllBlock) {
        return Result<uint32>::Success(0);
    }

    // 서로 다른 묶음은 profile 수 정도라 앞에서부터 비교한다(많아지면 해시 — 7.1.6절 한계).
    for (usize position = 0; position < entries_.Size(); ++position) {
        Entry& entry = entries_[position];
        if (entry.references != 0 && entry.responses == responses) {
            ++entry.references;
            return Result<uint32>::Success(static_cast<uint32>(position + 1));
        }
    }

    if (!free_.IsEmpty()) {
        const uint32 index = free_.TryPopBack().Value();
        Entry& entry = entries_[index - 1];
        entry.responses = responses;
        entry.references = 1;
        ++used_;
        return Result<uint32>::Success(index);
    }

    if (entries_.Size() >= MaxResponseSetIndex) {
        return Result<uint32>::Failure(ErrorCode::CapacityExceeded);
    }

    // 빈 칸 목록도 칸 수만큼 미리 잡아 Release가 실패하지 않게 한다.
    if (auto reserved = free_.TryEnsureCapacity(entries_.Size() + 1); !reserved) {
        return Result<uint32>::Failure(reserved.Error());
    }

    if (auto pushed = entries_.TryPushBack(Entry{responses, 1}); !pushed) {
        return Result<uint32>::Failure(pushed.Error());
    }

    ++used_;
    return Result<uint32>::Success(static_cast<uint32>(entries_.Size()));
}

void ResponseSetTable::Release(uint32 index) noexcept {
    if (index == 0 || index > entries_.Size()) {
        return;
    }

    Entry& entry = entries_[index - 1];
    if (entry.references == 0) {
        return;
    }

    --entry.references;
    if (entry.references == 0) {
        --used_;
        // Acquire가 칸 수만큼 미리 잡았으므로 실패하지 않는다.
        (void)free_.TryPushBack(index);
    }
}

const CollisionResponses& ResponseSetTable::Get(uint32 index) const noexcept {
    if (index == 0 || index > entries_.Size() || entries_[index - 1].references == 0) {
        return AllBlock;
    }

    return entries_[index - 1].responses;
}

} // namespace rocks::physics
