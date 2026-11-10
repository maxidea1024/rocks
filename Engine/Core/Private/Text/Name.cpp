#include <Core/Text/NamePool.h>
#include <Base/Checked.h>
#include <string.h>

namespace rocks {
namespace {

struct DomainSource {
    platform::Mutex gate;
    uint64 next = 1;
};

Result<uint64> TakeDomain() noexcept {
    // 첫 사용 시 구성하여 다른 translation unit의 전역 초기화 순서에 의존하지 않는다.
    // 발급량은 process 수명 동안만 유효하다. 마지막 값 뒤의 0은 영구 소진 sentinel이다.
    static DomainSource source;
    platform::LockGuard lock(source.gate);
    if (source.next == 0) {
        return Result<uint64>::Failure(ErrorCode::Overflow);
    }

    return Result<uint64>::Success(source.next++);
}

uint64 HashBytes(StringView value) noexcept {
    uint64 hash = 14695981039346656037ull;
    for (usize index = 0; index < value.ByteCount(); ++index) {
        hash ^= static_cast<uint8>(value.Data()[index]);
        hash *= 1099511628211ull;
    }

    return hash;
}

} // namespace

struct NamePool::Entry {
    Entry* next = nullptr;
    uint64 hash = 0;
    uint64 id = 0;
    usize bytes = 0;

    char* Data() noexcept {
        return reinterpret_cast<char*>(this + 1);
    }

    StringView View() noexcept {
        return StringView(Data(), bytes);
    }
};

NameLease::NameLease(NamePool& pool, Name name, StringView view) noexcept
    : pool_(&pool), name_(name), view_(view) {
}

NameLease::~NameLease() noexcept {
    Reset();
}

NameLease::NameLease(NameLease&& other) noexcept
    : pool_(other.pool_), name_(other.name_), view_(other.view_) {
    other.pool_ = nullptr;
    other.name_ = {};
    other.view_ = {};
}

NameLease& NameLease::operator=(NameLease&& other) noexcept {
    if (this != &other) {
        Reset();
        pool_ = other.pool_;
        name_ = other.name_;
        view_ = other.view_;
        other.pool_ = nullptr;
        other.name_ = {};
        other.view_ = {};
    }

    return *this;
}

NameLease::operator bool() const noexcept {
    return pool_ != nullptr;
}

Name NameLease::Identifier() const noexcept {
    return name_;
}

StringView NameLease::View() const noexcept {
    return view_;
}

Result<NameLease> NameLease::TryClone() const noexcept {
    if (pool_ == nullptr) {
        return Result<NameLease>::Failure(ErrorCode::StaleHandle);
    }

    return pool_->Acquire(name_);
}

void NameLease::Reset() noexcept {
    NamePool* pool = pool_;
    pool_ = nullptr;
    name_ = {};
    view_ = {};
    if (pool != nullptr) {
        pool->ReleaseLease();
    }
}

NamePool::NamePool(Allocator& allocator) noexcept : allocator_(&allocator) {
}

NamePool::~NamePool() noexcept {
    if (!Shutdown()) {
        platform::Fatal("NamePool destroyed with active operations or leases");
    }
}

Result<void> NamePool::Initialize(NamePoolConfig config) noexcept {
    {
        platform::LockGuard lock(gate_);
        if (state_ == State::Running) {
            return Result<void>::Failure(ErrorCode::AlreadyExists);
        }

        if (state_ != State::Stopped) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        state_ = State::Initializing;
        ++stats_.activeOperations;
    }

    // 전역 domain gate와 pool gate를 동시에 보유하지 않는다.
    auto domain = TakeDomain();
    platform::LockGuard lock(gate_);
    --stats_.activeOperations;
    if (state_ != State::Initializing) {
        // domain 발급 중 Shutdown이 admission을 닫았다면 다시 열지 않는다.
        return Result<void>::Failure(ErrorCode::ShuttingDown);
    }

    if (!domain) {
        state_ = State::Stopped;
        return Result<void>::Failure(domain.Error());
    }

    config_ = config;
    stats_ = {};
    stats_.domain = domain.Value();
    stats_.accepting = true;
    nextSerial_ = 1;
    state_ = State::Running;
    return Result<void>::Success();
}

Result<Name> NamePool::TryIntern(StringView source) noexcept {
    constexpr uint64 maximum = static_cast<uint64>(-1);
    constexpr uint64 maximumSerial = maximum >> BucketBits;
    {
        platform::LockGuard lock(gate_);
        if (state_ != State::Running) {
            return Result<Name>::Failure(ErrorCode::ShuttingDown);
        }

        if (stats_.activeOperations == maximum) {
            return Result<Name>::Failure(ErrorCode::Overflow);
        }

        ++stats_.activeOperations;
    }

    usize allocationBytes = 0;
    if (!TryAdd(sizeof(Entry), source.ByteCount(), allocationBytes) ||
        !TryAdd(allocationBytes, 1, allocationBytes)) {
        FinishOperation(0);
        return Result<Name>::Failure(ErrorCode::Overflow);
    }

    auto valid = source.ValidateUtf8();
    if (!valid) {
        FinishOperation(0);
        return Result<Name>::Failure(valid.Error());
    }

    const uint64 hash = HashBytes(source) & config_.hashMask;
    {
        platform::LockGuard lock(gate_);
        if (state_ != State::Running) {
            --stats_.activeOperations;
            return Result<Name>::Failure(ErrorCode::ShuttingDown);
        }

        if (Entry* found = FindBytes(source, hash)) {
            --stats_.activeOperations;
            return Result<Name>::Success(Name{stats_.domain, found->id});
        }

        if (nextSerial_ > maximumSerial) {
            --stats_.activeOperations;
            return Result<Name>::Failure(ErrorCode::Overflow);
        }

        const uint64 remainingEntries = config_.maxEntries - stats_.entryCount;
        const usize remainingBytes = config_.maxStorageBytes - stats_.storageBytes;
        if (stats_.pendingEntries >= remainingEntries ||
            allocationBytes > remainingBytes - stats_.pendingStorageBytes) {
            --stats_.activeOperations;
            return Result<Name>::Failure(ErrorCode::CapacityExceeded);
        }

        ++stats_.pendingEntries;
        stats_.pendingStorageBytes += allocationBytes;
    }

    auto allocation = allocator_->Allocate(allocationBytes, alignof(Entry));
    if (!allocation) {
        FinishOperation(allocationBytes);
        return Result<Name>::Failure(allocation.Error());
    }

    if (allocation.Value() == nullptr) {
        platform::Fatal("Allocator returned null for a nonzero name allocation");
    }

    Entry* candidate = ConstructAt<Entry>(allocation.Value());
    candidate->hash = hash;
    candidate->bytes = source.ByteCount();
    if (!source.IsEmpty()) {
        memcpy(candidate->Data(), source.Data(), source.ByteCount());
    }

    candidate->Data()[candidate->bytes] = '\0';
    Name result{};
    ErrorCode error = ErrorCode::None;
    {
        platform::LockGuard lock(gate_);
        if (state_ != State::Running) {
            error = ErrorCode::ShuttingDown;
        } else if (Entry* found = FindBytes(source, hash)) {
            result = Name{stats_.domain, found->id};
        } else if (nextSerial_ > maximumSerial) {
            error = ErrorCode::Overflow;
        } else {
            const usize bucket = static_cast<usize>(hash & (BucketCount - 1));
            candidate->id = (nextSerial_++ << BucketBits) | bucket;
            candidate->next = buckets_[bucket];
            buckets_[bucket] = candidate;
            ++stats_.entryCount;
            stats_.storageBytes += allocationBytes;
            --stats_.pendingEntries;
            stats_.pendingStorageBytes -= allocationBytes;
            --stats_.activeOperations;
            return Result<Name>::Success(Name{stats_.domain, candidate->id});
        }
    }

    // 중복 패자/종료 취소 후보를 gate 밖에서 해제한다. allocator callback이 진행되는 동안도
    // operation과 예산 예약을 유지하여 Shutdown 성공이 미완료 cleanup을 앞지르지 못하게 한다.
    FreeEntry(candidate);
    FinishOperation(allocationBytes);
    if (error != ErrorCode::None) {
        return Result<Name>::Failure(error);
    }

    return Result<Name>::Success(result);
}

NamePool::Entry* NamePool::FindBytes(StringView source, uint64 hash) const noexcept {
    const usize bucket = static_cast<usize>(hash & (BucketCount - 1));
    for (Entry* entry = buckets_[bucket]; entry != nullptr; entry = entry->next) {
        if (entry->hash == hash && entry->View() == source) {
            return entry;
        }
    }

    return nullptr;
}

Result<NameLease> NamePool::Acquire(Name name) noexcept {
    platform::LockGuard lock(gate_);
    if (state_ != State::Running) {
        return Result<NameLease>::Failure(ErrorCode::ShuttingDown);
    }

    if (!name.IsValid() || name.domain != stats_.domain) {
        return Result<NameLease>::Failure(ErrorCode::StaleHandle);
    }

    const usize bucket = static_cast<usize>(name.id & (BucketCount - 1));
    for (Entry* entry = buckets_[bucket]; entry != nullptr; entry = entry->next) {
        if (entry->id != name.id) {
            continue;
        }

        if (stats_.activeLeases == static_cast<uint64>(-1)) {
            return Result<NameLease>::Failure(ErrorCode::Overflow);
        }

        ++stats_.activeLeases;
        return Result<NameLease>::Success(NameLease(*this, name, entry->View()));
    }

    return Result<NameLease>::Failure(ErrorCode::StaleHandle);
}

void NamePool::ReleaseLease() noexcept {
    platform::LockGuard lock(gate_);
    if (stats_.activeLeases == 0) {
        platform::Fatal("NameLease count underflow");
    }

    --stats_.activeLeases;
}

void NamePool::FinishOperation(usize reservedBytes) noexcept {
    platform::LockGuard lock(gate_);
    if (stats_.activeOperations == 0 ||
        (reservedBytes != 0 &&
         (stats_.pendingEntries == 0 || stats_.pendingStorageBytes < reservedBytes))) {
        platform::Fatal("NamePool operation reservation underflow");
    }

    if (reservedBytes != 0) {
        --stats_.pendingEntries;
        stats_.pendingStorageBytes -= reservedBytes;
    }

    --stats_.activeOperations;
}

void NamePool::FreeEntry(Entry* entry) noexcept {
    DestroyAt(entry);
    if (!allocator_->Deallocate(entry)) {
        platform::Fatal("Name entry storage was not owned by its allocator");
    }
}

Result<void> NamePool::Shutdown() noexcept {
    Entry* detached[BucketCount]{};
    {
        platform::LockGuard lock(gate_);
        if (state_ == State::Stopped) {
            return Result<void>::Success();
        }

        if (state_ == State::Reclaiming) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        state_ = State::Closing;
        stats_.accepting = false;
        if (stats_.activeOperations != 0 || stats_.activeLeases != 0) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        for (usize index = 0; index < BucketCount; ++index) {
            detached[index] = buckets_[index];
            buckets_[index] = nullptr;
        }

        state_ = State::Reclaiming;
        stats_.reclaiming = true;
    }

    for (Entry* chain : detached) {
        while (chain != nullptr) {
            Entry* next = chain->next;
            FreeEntry(chain);
            chain = next;
        }
    }

    platform::LockGuard lock(gate_);
    stats_.entryCount = 0;
    stats_.storageBytes = 0;
    stats_.reclaiming = false;
    state_ = State::Stopped;
    return Result<void>::Success();
}

NamePoolStats NamePool::Stats() const noexcept {
    platform::LockGuard lock(gate_);
    return stats_;
}

} // namespace rocks
