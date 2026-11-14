#include <Core/Hash.h>
#include <Object/ReferenceObserver.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

namespace rocks {

namespace {

constexpr uint32 ObserverIndexTag = 0x4f425358u; // 'OBSX'
constexpr usize MinimumObserverCapacity = 8;

// (소유 패키지, 대상 패키지, 원인, 트랜잭션) 키의 해시다(Core 해시 도구로 구성 요소를 섞는다).
uint64 KeyHash(ObjectHandle owner, ObjectHandle target, ReferenceChangeCause cause,
               uint64 transaction) noexcept {
    const Hash<ObjectHandle> hash;
    const uint64 pair = HashCombine(hash(owner), hash(target));
    return HashCombine(pair, HashCombine(MixHash(transaction), static_cast<uint64>(cause)));
}

void ReleaseObserverMemory(Allocator& allocator, void* memory) noexcept {
    if (memory != nullptr && !allocator.Deallocate(memory)) {
        platform::Fatal("ReferenceObserver allocation ownership mismatch");
    }
}

} // namespace

ReferenceObserver::ReferenceObserver(Allocator& allocator, usize maxKeys) noexcept
    : allocator_(allocator), maxKeys_(maxKeys > MaxKeysLimit ? MaxKeysLimit : maxKeys),
      entries_(allocator) {
}

ReferenceObserver::~ReferenceObserver() {
    if (registry_ != nullptr) {
        platform::Fatal("ReferenceObserver destroyed while attached to an ObjectRegistry");
    }

    ReleaseObserverMemory(allocator_, index_);
    index_ = nullptr;
}

usize ReferenceObserver::FreeKeys() const noexcept {
    usize limit = entries_.Capacity() < maxKeys_ ? entries_.Capacity() : maxKeys_;
    if (indexCapacity_ / 2 < limit) {
        limit = indexCapacity_ / 2;
    }

    return limit > entries_.Size() ? limit - entries_.Size() : 0;
}

uint32 ReferenceObserver::Find(uint64 hash, ObjectHandle owner, ObjectHandle target,
                               ReferenceChangeCause cause, uint64 transaction) const noexcept {
    if (indexCapacity_ == 0) {
        return 0;
    }

    // 적재율이 1/2 이하라 빈 칸이 반드시 있어 탐사가 끝난다.
    const usize mask = indexCapacity_ - 1;
    for (usize slot = static_cast<usize>(hash) & mask;; slot = (slot + 1) & mask) {
        const uint32 position = index_[slot];
        if (position == 0) {
            return 0;
        }

        const ReferenceEdgeDelta& entry = entries_[position - 1];
        if (entry.ownerPackage == owner && entry.targetPackage == target && entry.cause == cause &&
            entry.transaction == transaction) {
            return position;
        }
    }
}

bool ReferenceObserver::Contains(ObjectHandle owner, ObjectHandle target,
                                 ReferenceChangeCause cause, uint64 transaction) const noexcept {
    return Find(KeyHash(owner, target, cause, transaction), owner, target, cause, transaction) != 0;
}

void ReferenceObserver::Add(ObjectHandle owner, ObjectHandle target, int64 delta,
                            ReferenceChangeCause cause, uint64 transaction) noexcept {
    const uint64 hash = KeyHash(owner, target, cause, transaction);
    if (const uint32 position = Find(hash, owner, target, cause, transaction); position != 0) {
        entries_[position - 1].delta += delta;
        return;
    }

    if (FreeKeys() == 0) {
        platform::Fatal("ReferenceObserver record without a reservation");
    }

    ReferenceEdgeDelta entry;
    entry.ownerPackage = owner;
    entry.targetPackage = target;
    entry.delta = delta;
    entry.cause = cause;
    entry.transaction = transaction;
    if (!entries_.TryPushBack(entry)) {
        platform::Fatal("Reserved ReferenceObserver entry unexpectedly failed");
    }

    const usize mask = indexCapacity_ - 1;
    usize slot = static_cast<usize>(hash) & mask;
    while (index_[slot] != 0) {
        slot = (slot + 1) & mask;
    }

    index_[slot] = static_cast<uint32>(entries_.Size());
}

Result<void> ReferenceObserver::Reserve(usize newKeys) noexcept {
    const usize size = entries_.Size();
    if (newKeys > maxKeys_ - size) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    if (FreeKeys() >= newKeys) {
        return Result<void>::Success();
    }

    if (reserving_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    // 항목 배열은 기하 성장(상한 maxKeys), 색인은 적재율 1/2를 지키는 2의 거듭제곱이다.
    const usize needed = size + newKeys;
    usize capacity = entries_.Capacity() > maxKeys_ / 2 ? maxKeys_ : entries_.Capacity() * 2;
    if (capacity < MinimumObserverCapacity) {
        capacity = MinimumObserverCapacity < maxKeys_ ? MinimumObserverCapacity : maxKeys_;
    }

    if (capacity < needed) {
        capacity = needed;
    }

    usize indexCapacity = 16;
    while (indexCapacity / 2 < capacity) {
        indexCapacity *= 2;
    }

    // 할당 콜백이 Registry에 재진입해 옛 표에 Add할 수 있다(단일 간선 지점). 그 항목도 아래에서
    // 옮긴다. 콜백의 중첩 Reserve·TakeChanges는 Busy다.
    reserving_ = true;
    Array<ReferenceEdgeDelta> grown(allocator_);
    auto reserved = grown.TryReserve(capacity);
    if (!reserved) {
        reserving_ = false;
        return reserved;
    }

    auto table =
        allocator_.Allocate(indexCapacity * sizeof(uint32), alignof(uint32), ObserverIndexTag);
    if (!table) {
        reserving_ = false;
        return Result<void>::Failure(table.Error());
    }

    auto* fresh = static_cast<uint32*>(table.Value());
    if (entries_.Size() > capacity) {
        // 콜백 재진입 기록이 새 공간보다 많아졌다(계약상 일어나지 않는다 — 단일 간선 지점은 키를
        // 둘까지만 더한다). 옮길 수 없으므로 그대로 둔다.
        ReleaseObserverMemory(allocator_, fresh);
        reserving_ = false;
        return Result<void>::Failure(ErrorCode::Busy);
    }

    platform::MemoryZero(fresh, indexCapacity * sizeof(uint32));
    for (usize position = 0; position < entries_.Size(); ++position) {
        if (!grown.TryPushBack(entries_[position])) {
            platform::Fatal("Reserved ReferenceObserver move unexpectedly failed");
        }

        const ReferenceEdgeDelta& entry = entries_[position];
        const usize mask = indexCapacity - 1;
        usize slot = static_cast<usize>(KeyHash(entry.ownerPackage, entry.targetPackage,
                                                entry.cause, entry.transaction)) &
                     mask;
        while (fresh[slot] != 0) {
            slot = (slot + 1) & mask;
        }

        fresh[slot] = static_cast<uint32>(position + 1);
    }

    // 새 표를 먼저 게시하고 옛 저장소는 그 뒤에 반환한다(반환 콜백이 재진입해도 일관된 표를 본다).
    Array<ReferenceEdgeDelta> previous(Move(entries_));
    entries_ = Move(grown);
    uint32* previousIndex = index_;
    index_ = fresh;
    indexCapacity_ = indexCapacity;
    ReleaseObserverMemory(allocator_, previousIndex);
    previous.Reset();
    reserving_ = false;
    return Result<void>::Success();
}

Result<void> ReferenceObserver::TakeChanges(Array<ReferenceEdgeDelta>& out) noexcept {
    if (registry_ != nullptr && platform::CurrentThreadId() != ownerThread_) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (!out.IsEmpty() || &out.GetAllocator() != &allocator_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (reserving_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    // 0이 된 항목을 제자리에서 지운다(처음 생긴 순서 유지).
    usize kept = 0;
    for (usize position = 0; position < entries_.Size(); ++position) {
        if (entries_[position].delta != 0) {
            entries_[kept++] = entries_[position];
        }
    }

    if (!entries_.TryResize(kept)) {
        platform::Fatal("ReferenceObserver shrink unexpectedly failed");
    }

    Array<ReferenceEdgeDelta> taken(Move(entries_));
    entries_ = Move(out);
    out = Move(taken);
    if (index_ != nullptr) {
        platform::MemoryZero(index_, indexCapacity_ * sizeof(uint32));
    }

    return Result<void>::Success();
}

} // namespace rocks
