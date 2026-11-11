#include <Core/Memory/DebugAllocator.h>
#include <Base/Checked.h>
#include <Core/Move.h>
#include <Platform/Fatal.h>

namespace rocks {

namespace {

constexpr usize GuardSize = 16;
constexpr uint8 FrontByte = 0xA5;
constexpr uint8 BackByte = 0x5A;
constexpr uint8 NewByte = 0xCD;
constexpr uint8 FreedByte = 0xDD;

void Fill(unsigned char* data, usize count, uint8 value) noexcept {
    for (usize index = 0; index < count; ++index) {
        data[index] = value;
    }
}

// 손상된 할당기 상태에서도 heap이나 로그 시스템을 호출하지 않고 위치를 남긴다.
[[noreturn]] void ReportCorruption(const char* message, const DebugMemoryReport& report) noexcept {
    char text[256]{};
    usize used = 0;
    auto append = [&](const char* source) noexcept {
        while (*source && used + 1 < sizeof(text)) {
            text[used++] = *source++;
        }
    };
    auto decimal = [&](uint64 value) noexcept {
        char digits[20]{};
        usize count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value);
        while (count && used + 1 < sizeof(text)) {
            text[used++] = digits[--count];
        }
    };
    auto hexadecimal = [&](uptr value) noexcept {
        constexpr char digits[] = "0123456789abcdef";
        char buffer[2 * sizeof(uptr)]{};
        usize count = 0;
        do {
            buffer[count++] = digits[value & 0xF];
            value >>= 4;
        } while (value);
        while (count && used + 1 < sizeof(text)) {
            text[used++] = buffer[--count];
        }
    };
    append(message);
    append(" (address=0x");
    hexadecimal(reinterpret_cast<uptr>(report.address));
    append(", tag=");
    decimal(report.tag);
    append(", offset=");
    decimal(static_cast<uint64>(report.offset));
    append(")");
    text[used] = '\0';
    platform::Fatal(text);
}

} // namespace

struct DebugAllocator::Record {
    Record* next = nullptr;
    void* storage = nullptr;
    unsigned char* address = nullptr;
    usize bytes = 0;
    uint32 tag = 0;
};

// A -> B -> A를 포함해 backing callback의 같은 인스턴스 재진입을 잡는다.
struct DebugAllocator::EntryScope {
    explicit EntryScope(const DebugAllocator* owner) noexcept : owner(owner) {
        for (EntryScope* current = activeEntry_; current; current = current->previous) {
            if (current->owner == owner) {
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
                platform::Fatal("DebugAllocator entry stack corrupted");
            }
            activeEntry_ = previous;
        }
    }

    EntryScope(const EntryScope&) = delete;
    EntryScope& operator=(const EntryScope&) = delete;

    const DebugAllocator* owner;
    EntryScope* previous = nullptr;
    bool entered = false;
};

thread_local DebugAllocator::EntryScope* DebugAllocator::activeEntry_ = nullptr;

DebugAllocator::DebugAllocator(Allocator& backing, usize quarantineBytes) noexcept
    : backing_(backing), quarantineLimit_(quarantineBytes) {
}

DebugAllocator::~DebugAllocator() noexcept {
    EntryScope entry(this);
    if (!entry.entered) {
        platform::Fatal("DebugAllocator destroyed during reentrant operation");
    }
    platform::LockGuard lock(gate_);
    if (stats_.allocations.liveAllocations != 0 || live_) {
        platform::Fatal("DebugAllocator destroyed with live allocations");
    }
    while (quarantineHead_) {
        if (!EvictOne()) {
            platform::Fatal("DebugAllocator backing rejected quarantine storage");
        }
    }
}

Result<void*> DebugAllocator::Allocate(usize bytes, usize alignment, uint32 tag) noexcept {
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

    usize prefix = 0;
    usize total = 0;
    if (!TryAdd(GuardSize, alignment - 1, prefix)) {
        return Result<void*>::Failure(ErrorCode::Overflow);
    }
    prefix &= ~(alignment - 1);
    if (!TryAdd(prefix, bytes, total) || !TryAdd(total, GuardSize, total)) {
        return Result<void*>::Failure(ErrorCode::Overflow);
    }

    platform::LockGuard lock(gate_);
    constexpr uint64 maximum = static_cast<uint64>(-1);
    if (stats_.allocations.liveAllocations == maximum ||
        stats_.allocations.totalAllocations == maximum ||
        static_cast<uint64>(bytes) > maximum - stats_.allocations.liveBytes) {
        return Result<void*>::Failure(ErrorCode::Overflow);
    }

    // 사용자 메모리와 소유 기록은 별도 저장소다. payload 경계 손상으로 목록이 바로 덮이지 않는다.
    auto metadata = backing_.Allocate(sizeof(Record), alignof(Record));
    if (!metadata) {
        return Result<void*>::Failure(metadata.Error());
    }
    if (!metadata.Value() || reinterpret_cast<uptr>(metadata.Value()) % alignof(Record) != 0) {
        platform::Fatal("DebugAllocator backing returned invalid record storage");
    }

    auto storage = backing_.Allocate(total, alignment, tag);
    if (!storage) {
        if (!backing_.Deallocate(metadata.Value())) {
            platform::Fatal("DebugAllocator backing rejected unused record");
        }
        return Result<void*>::Failure(storage.Error());
    }
    if (!storage.Value() || reinterpret_cast<uptr>(storage.Value()) % alignment != 0) {
        platform::Fatal("DebugAllocator backing returned invalid payload storage");
    }

    auto* record = ConstructAt<Record>(metadata.Value());
    record->storage = storage.Value();
    record->address = static_cast<unsigned char*>(storage.Value()) + prefix;
    record->bytes = bytes;
    record->tag = tag;
    Fill(record->address - GuardSize, GuardSize, FrontByte);
    Fill(record->address, bytes, NewByte);
    Fill(record->address + bytes, GuardSize, BackByte);
    record->next = live_;
    live_ = record;
    ++stats_.allocations.liveAllocations;
    stats_.allocations.liveBytes += static_cast<uint64>(bytes);
    ++stats_.allocations.totalAllocations;
    return Result<void*>::Success(record->address);
}

Result<void> DebugAllocator::Deallocate(void* address) noexcept {
    EntryScope entry(this);
    if (!entry.entered) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    if (!address) {
        return Result<void>::Success();
    }

    platform::LockGuard lock(gate_);
    Record** link = &live_;
    while (*link && (*link)->address != address) {
        link = &(*link)->next;
    }
    if (!*link) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }
    Record* record = *link;
    CheckRecord(*record, false);

    if (record->bytes > quarantineLimit_ || quarantineLimit_ == 0) {
        // 실패 시 현재 payload를 바꾸지 않도록 반환 전에 poison하지 않는다.
        auto released = backing_.Deallocate(record->storage);
        if (!released) {
            return released;
        }
        *link = record->next;
        --stats_.allocations.liveAllocations;
        stats_.allocations.liveBytes -= static_cast<uint64>(record->bytes);
        ReleaseRecord(record);
        return Result<void>::Success();
    }

    while (stats_.quarantineBytes > quarantineLimit_ - record->bytes) {
        auto evicted = EvictOne();
        if (!evicted) {
            return evicted;
        }
    }

    *link = record->next;
    --stats_.allocations.liveAllocations;
    stats_.allocations.liveBytes -= static_cast<uint64>(record->bytes);
    Fill(record->address, record->bytes, FreedByte);
    record->next = nullptr;
    if (quarantineTail_) {
        quarantineTail_->next = record;
    } else {
        quarantineHead_ = record;
    }
    quarantineTail_ = record;
    stats_.quarantineBytes += record->bytes;
    ++stats_.quarantineAllocations;
    return Result<void>::Success();
}

DebugMemoryReport DebugAllocator::Inspect(const Record& record, bool quarantined) const noexcept {
    for (usize index = 0; index < GuardSize; ++index) {
        if ((record.address - GuardSize)[index] != FrontByte) {
            return {DebugMemoryIssue::FrontGuard, record.address, index, record.tag};
        }
    }
    for (usize index = 0; index < GuardSize; ++index) {
        if (record.address[record.bytes + index] != BackByte) {
            return {DebugMemoryIssue::BackGuard, record.address, index, record.tag};
        }
    }
    if (quarantined) {
        for (usize index = 0; index < record.bytes; ++index) {
            if (record.address[index] != FreedByte) {
                return {DebugMemoryIssue::FreedPayload, record.address, index, record.tag};
            }
        }
    }
    return {};
}

void DebugAllocator::CheckRecord(const Record& record, bool quarantined) const noexcept {
    const auto report = Inspect(record, quarantined);
    switch (report.issue) {
    case DebugMemoryIssue::None:
        return;
    case DebugMemoryIssue::FrontGuard:
        ReportCorruption("DebugAllocator detected front guard corruption", report);
    case DebugMemoryIssue::BackGuard:
        ReportCorruption("DebugAllocator detected back guard corruption", report);
    case DebugMemoryIssue::FreedPayload:
        ReportCorruption("DebugAllocator detected write after free", report);
    }
    platform::Fatal("DebugAllocator detected unknown corruption");
}

Result<DebugMemoryReport> DebugAllocator::Validate() const noexcept {
    EntryScope entry(this);
    if (!entry.entered) {
        return Result<DebugMemoryReport>::Failure(ErrorCode::Busy);
    }
    platform::LockGuard lock(gate_);
    for (Record* record = live_; record; record = record->next) {
        auto report = Inspect(*record, false);
        if (report.issue != DebugMemoryIssue::None) {
            return Result<DebugMemoryReport>::Success(report);
        }
    }
    for (Record* record = quarantineHead_; record; record = record->next) {
        auto report = Inspect(*record, true);
        if (report.issue != DebugMemoryIssue::None) {
            return Result<DebugMemoryReport>::Success(report);
        }
    }
    return Result<DebugMemoryReport>::Success({});
}

Result<void> DebugAllocator::EvictOne() noexcept {
    Record* record = quarantineHead_;
    if (!record) {
        platform::Fatal("DebugAllocator quarantine accounting corrupted");
    }
    CheckRecord(*record, true);
    auto released = backing_.Deallocate(record->storage);
    if (!released) {
        return released;
    }
    quarantineHead_ = record->next;
    if (!quarantineHead_) {
        quarantineTail_ = nullptr;
    }
    stats_.quarantineBytes -= record->bytes;
    --stats_.quarantineAllocations;
    ReleaseRecord(record);
    return Result<void>::Success();
}

void DebugAllocator::ReleaseRecord(Record* record) noexcept {
    DestroyAt(record);
    if (!backing_.Deallocate(record)) {
        platform::Fatal("DebugAllocator backing rejected owned record");
    }
}

Result<void> DebugAllocator::FlushQuarantine() noexcept {
    EntryScope entry(this);
    if (!entry.entered) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    platform::LockGuard lock(gate_);
    while (quarantineHead_) {
        auto result = EvictOne();
        if (!result) {
            return result;
        }
    }
    return Result<void>::Success();
}

DebugAllocatorStats DebugAllocator::Stats() const noexcept {
    EntryScope entry(this);
    if (!entry.entered) {
        platform::Fatal("DebugAllocator statistics accessed during reentrant operation");
    }
    platform::LockGuard lock(gate_);
    return stats_;
}

} // namespace rocks
