#include <Core/Memory/ExternalAllocator.h>
#include <Base/Checked.h>
#include <Core/Move.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

namespace rocks {

namespace {

// 덧셈이나 서로 다른 포인터의 대소 비교 없이 요청된 저장 범위의 겹침을 확인한다.
bool Overlaps(uptr left, usize leftSize, uptr right, usize rightSize) noexcept {
    return left <= right ? right - left < leftSize : left - right < rightSize;
}

} // namespace

struct ExternalAllocator::Record {
    void* address = nullptr;
    usize bytes = 0;
    Record* next = nullptr;
    bool releasing = false;
};

// backend가 A -> B -> A 순서로 돌아와도 같은 인스턴스의 재진입을 진단한다.
struct ExternalAllocator::EntryScope {
    explicit EntryScope(const ExternalAllocator* owner) noexcept : owner(owner) {
        for (EntryScope* item = activeEntry_; item != nullptr; item = item->previous) {
            if (item->owner == owner) {
                return;
            }
        }

        previous = activeEntry_;
        activeEntry_ = this;
        entered = true;
    }

    ~EntryScope() noexcept {
        if (entered) {
            if (activeEntry_ != this) {
                platform::Fatal("ExternalAllocator entry stack corrupted");
            }
            activeEntry_ = previous;
        }
    }

    EntryScope(const EntryScope&) = delete;
    EntryScope& operator=(const EntryScope&) = delete;

    const ExternalAllocator* owner;
    EntryScope* previous = nullptr;
    bool entered = false;
};

struct ExternalAllocator::FlightScope {
    explicit FlightScope(ExternalAllocator& owner) noexcept : owner(owner) {
    }

    ~FlightScope() noexcept {
        owner.EndFlight();
    }

    FlightScope(const FlightScope&) = delete;
    FlightScope& operator=(const FlightScope&) = delete;

    ExternalAllocator& owner;
};

thread_local ExternalAllocator::EntryScope* ExternalAllocator::activeEntry_ = nullptr;

ExternalAllocator::ExternalAllocator(ExternalAllocatorBackend backend,
                                     Allocator* metadataAllocator) noexcept
    : backend_(Move(backend)), metadataAllocator_(metadataAllocator) {
    if (metadataAllocator_ == this) {
        platform::Fatal("ExternalAllocator metadata allocator cannot be itself");
    }
}

ExternalAllocator::~ExternalAllocator() noexcept {
    EntryScope entry(this);
    if (!entry.entered) {
        platform::Fatal("ExternalAllocator destroyed during reentrant operation");
    }

    {
        platform::LockGuard lock(gate_);
        if (inFlight_ != 0 || records_ != nullptr || stats_.liveAllocations != 0) {
            platform::Fatal("ExternalAllocator destroyed with active calls or live allocations");
        }
    }
    // 모듈 release 콜백이 통계를 읽더라도 mutex는 아직 살아 있고 변경 재진입은 차단된다.
    backend_.module.Reset();
}

bool ExternalAllocator::IsAvailable() const noexcept {
    return backend_.allocate != nullptr && backend_.deallocate != nullptr;
}

Result<void> ExternalAllocator::BeginFlight() noexcept {
    platform::LockGuard lock(gate_);
    if (inFlight_ == static_cast<uint64>(-1)) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }
    ++inFlight_;
    return Result<void>::Success();
}

void ExternalAllocator::EndFlight() noexcept {
    platform::LockGuard lock(gate_);
    if (inFlight_ == 0) {
        platform::Fatal("ExternalAllocator active call underflow");
    }
    --inFlight_;
}

Result<void*> ExternalAllocator::AllocateRecord() noexcept {
    if (metadataAllocator_) {
        return metadataAllocator_->Allocate(sizeof(Record), alignof(Record));
    }

    void* storage = platform::AlignedAllocate(sizeof(Record), alignof(Record));
    if (!storage) {
        return Result<void*>::Failure(ErrorCode::OutOfMemory);
    }
    return Result<void*>::Success(storage);
}

void ExternalAllocator::FreeRecord(void* storage) noexcept {
    if (metadataAllocator_) {
        if (!metadataAllocator_->Deallocate(storage)) {
            platform::Fatal("ExternalAllocator metadata allocator rejected owned record");
        }
    } else {
        platform::AlignedFree(storage);
    }
}

Result<void*> ExternalAllocator::Allocate(usize bytes, usize alignment, uint32 tag) noexcept {
    EntryScope entry(this);
    if (!entry.entered) {
        return Result<void*>::Failure(ErrorCode::Busy);
    }
    if (!IsPowerOfTwo(alignment)) {
        return Result<void*>::Failure(ErrorCode::InvalidArgument);
    }
    if (bytes == 0) {
        return Result<void*>::Success(nullptr);
    }
    if (bytes > MaxSize - (alignment - 1)) {
        return Result<void*>::Failure(ErrorCode::Overflow);
    }
    if (!IsAvailable()) {
        return Result<void*>::Failure(ErrorCode::Unsupported);
    }

    auto begin = BeginFlight();
    if (!begin) {
        return Result<void*>::Failure(begin.Error());
    }
    FlightScope flight(*this);

    // backend에 주소를 요청하기 전에 독립 기록을 준비하여 기록 없는 게시를 막는다.
    auto memory = AllocateRecord();
    if (!memory) {
        return Result<void*>::Failure(memory.Error());
    }
    void* storage = memory.Value();
    if (!storage || reinterpret_cast<uptr>(storage) % alignof(Record) != 0) {
        platform::Fatal("ExternalAllocator metadata allocator returned invalid storage");
    }

    auto allocation = backend_.allocate(backend_.context, bytes, alignment, tag);
    if (!allocation) {
        FreeRecord(storage);
        return Result<void*>::Failure(allocation.Error());
    }
    void* address = allocation.Value();
    if (!address || reinterpret_cast<uptr>(address) % alignment != 0) {
        platform::Fatal("ExternalAllocator backend returned invalid allocation");
    }

    const uptr addressValue = reinterpret_cast<uptr>(address);
    if (bytes > MaxSize - addressValue) {
        platform::Fatal("ExternalAllocator backend returned overflowing address range");
    }
    if (Overlaps(addressValue, bytes, reinterpret_cast<uptr>(storage), sizeof(Record))) {
        platform::Fatal("ExternalAllocator backend overlaps allocation metadata");
    }

    auto* record = ConstructAt<Record>(storage);
    record->address = address;
    record->bytes = bytes;
    bool overflow = false;
    {
        platform::LockGuard lock(gate_);
        // 다른 스레드가 backend free를 마친 직후 같은 주소를 재발급할 수 있다. 해당 반환이
        // ledger에서 제거될 때까지 기다려 정상적인 주소 재사용과 실제 중복 발급을 구별한다.
        for (;;) {
            Record* collision = nullptr;
            for (Record* item = records_; item != nullptr; item = item->next) {
                // 내부 기록의 수명까지 덮어쓰면 소유권 추적이 깨지므로 발급을 중단한다.
                if (Overlaps(addressValue, bytes, reinterpret_cast<uptr>(item), sizeof(Record))) {
                    platform::Fatal("ExternalAllocator backend overlaps allocation metadata");
                }
                if (Overlaps(addressValue, bytes, reinterpret_cast<uptr>(item->address),
                             item->bytes)) {
                    collision = item;
                    break;
                }
            }
            if (!collision) {
                break;
            }
            if (!collision->releasing) {
                platform::Fatal("ExternalAllocator backend returned overlapping live allocation");
            }
            if (changed_.Wait(gate_) != platform::WaitResult::Signaled) {
                platform::Fatal("ExternalAllocator address reuse wait failed");
            }
        }

        constexpr uint64 maximum = static_cast<uint64>(-1);
        overflow = stats_.liveAllocations == maximum || stats_.totalAllocations == maximum ||
                   static_cast<uint64>(bytes) > maximum - stats_.liveBytes;
        if (!overflow) {
            record->next = records_;
            records_ = record;
            ++stats_.liveAllocations;
            ++stats_.totalAllocations;
            stats_.liveBytes += static_cast<uint64>(bytes);
        }
    }

    if (overflow) {
        if (!backend_.deallocate(backend_.context, address)) {
            platform::Fatal("ExternalAllocator backend rejected rollback");
        }
        DestroyAt(record);
        FreeRecord(storage);
        return Result<void*>::Failure(ErrorCode::Overflow);
    }

    return Result<void*>::Success(address);
}

Result<void> ExternalAllocator::Deallocate(void* address) noexcept {
    EntryScope entry(this);
    if (!entry.entered) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    if (!address) {
        return Result<void>::Success();
    }
    if (!IsAvailable()) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    auto begin = BeginFlight();
    if (!begin) {
        return begin;
    }
    FlightScope flight(*this);

    Record* record = nullptr;
    {
        platform::LockGuard lock(gate_);
        for (Record* item = records_; item != nullptr; item = item->next) {
            if (item->address == address) {
                record = item;
                break;
            }
        }
        if (!record) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }
        if (record->releasing) {
            return Result<void>::Failure(ErrorCode::Busy);
        }
        record->releasing = true;
    }

    auto released = backend_.deallocate(backend_.context, address);
    if (!released) {
        platform::LockGuard lock(gate_);
        record->releasing = false;
        changed_.NotifyAll();
        return Result<void>::Failure(released.Error());
    }

    {
        platform::LockGuard lock(gate_);
        Record** previous = &records_;
        while (*previous != record) {
            previous = &(*previous)->next;
        }
        *previous = record->next;
        --stats_.liveAllocations;
        stats_.liveBytes -= static_cast<uint64>(record->bytes);
        changed_.NotifyAll();
    }
    DestroyAt(record);
    FreeRecord(record);
    return Result<void>::Success();
}

AllocationStats ExternalAllocator::Stats() const noexcept {
    platform::LockGuard lock(gate_);
    return stats_;
}

} // namespace rocks
