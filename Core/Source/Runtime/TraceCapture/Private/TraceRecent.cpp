#include "TraceSessionInternal.h"

// 최근 기록 고리다(Docs/Design/TraceSystem.ko.md 8절 "최근 기록 고리"). 스레드 스트림 덩어리를 고정
// 크기 byte 고리에 담고, 넘치면 가장 오래된 덩어리부터 버린다. 칸 하나는 [u8 표시][덩어리 byte]다.
// 표시의 첫 비트는 "그 스레드 스트림이 이 덩어리에서 기록 경계로 시작한다"이다.
namespace rocks::detail {

namespace {

constexpr uint8 EntryRecordBoundary = 1u << 0;
constexpr usize EntryFlagBytes = 1;

} // namespace

Result<void> TraceRecentRing::Initialize(Allocator& allocator, usize capacity) noexcept {
    allocator_ = &allocator;
    if (capacity == 0) {
        return Result<void>::Success();
    }

    auto memory = allocator.Allocate(capacity, alignof(uint8));
    if (!memory) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    data_ = static_cast<uint8*>(memory.Value());
    capacity_ = capacity;
    return Result<void>::Success();
}

void TraceRecentRing::Release() noexcept {
    if (data_ != nullptr) {
        auto freed = allocator_->Deallocate(data_);
        (void)freed;
    }

    data_ = nullptr;
    capacity_ = 0;
    start_ = 0;
    used_ = 0;
}

void TraceRecentRing::CopyIn(usize position, const uint8* source, usize bytes) noexcept {
    const usize first = bytes < capacity_ - position ? bytes : capacity_ - position;
    platform::MemoryCopy(data_ + position, source, first);
    if (first < bytes) {
        platform::MemoryCopy(data_, source + first, bytes - first);
    }
}

void TraceRecentRing::CopyOut(usize position, uint8* destination, usize bytes) const noexcept {
    const usize first = bytes < capacity_ - position ? bytes : capacity_ - position;
    platform::MemoryCopy(destination, data_ + position, first);
    if (first < bytes) {
        platform::MemoryCopy(destination + first, data_, bytes - first);
    }
}

usize TraceRecentRing::EntryBytesAt(usize position, uint8& flags, uint16& stream) const noexcept {
    uint8 head[EntryFlagBytes + TraceFormat::ChunkHeaderBytes];
    CopyOut(position, head, sizeof(head));
    flags = head[0];
    const uint16 sizeAndFlags = LoadLittleEndian<uint16>(head + EntryFlagBytes);
    stream = LoadLittleEndian<uint16>(head + EntryFlagBytes + sizeof(uint16));
    return EntryFlagBytes + TraceFormat::ChunkHeaderBytes +
           (sizeAndFlags & TraceFormat::ChunkSizeMask);
}

void TraceRecentRing::Push(Span<const uint8> chunk, bool recordBoundary) noexcept {
    if (capacity_ == 0) {
        return;
    }

    const usize need = EntryFlagBytes + chunk.Size();
    if (need > capacity_) {
        ++droppedChunks_;
        droppedBytes_ += chunk.Size();
        return;
    }

    // 가장 오래된 덩어리부터 버린다.
    while (used_ + need > capacity_) {
        uint8 flags = 0;
        uint16 stream = 0;
        const usize entry = EntryBytesAt(start_, flags, stream);
        start_ = (start_ + entry) % capacity_;
        used_ -= entry;
        ++droppedChunks_;
        droppedBytes_ += entry - EntryFlagBytes;
        --chunks_;
    }

    const uint8 flags = recordBoundary ? EntryRecordBoundary : uint8{0};
    const usize position = (start_ + used_) % capacity_;
    CopyIn(position, &flags, EntryFlagBytes);
    CopyIn((position + EntryFlagBytes) % capacity_, chunk.Data(), chunk.Size());
    used_ += need;
    ++chunks_;
}

Result<TraceRecentCopy> TraceRecentRing::AppendTo(Array<uint8>& out) const noexcept {
    TraceRecentCopy copy;
    // 스트림마다 기록 경계로 시작한 덩어리를 만나기 전까지는 버린다(앞 덩어리를 잃어 기록 중간에서
    // 시작하는 덩어리는 해석할 수 없다). 스트림 수는 많지 않으므로 선형 표로 둔다.
    Array<uint16> synced(*allocator_);
    usize position = start_;
    usize remaining = used_;
    while (remaining != 0) {
        uint8 flags = 0;
        uint16 stream = 0;
        const usize entry = EntryBytesAt(position, flags, stream);
        bool keep = synced.Contains(stream);
        if (!keep && (flags & EntryRecordBoundary) != 0) {
            if (!synced.TryPushBack(stream)) {
                return Result<TraceRecentCopy>::Failure(ErrorCode::OutOfMemory);
            }

            keep = true;
        }

        if (keep) {
            auto appended = out.TryAppendUninitialized(entry - EntryFlagBytes);
            if (!appended) {
                return Result<TraceRecentCopy>::Failure(appended.Error());
            }

            CopyOut((position + EntryFlagBytes) % capacity_, appended.Value().Data(),
                    entry - EntryFlagBytes);
            ++copy.chunks;
            copy.bytes += entry - EntryFlagBytes;
        } else {
            ++copy.skippedChunks;
        }

        position = (position + entry) % capacity_;
        remaining -= entry;
    }

    return Result<TraceRecentCopy>::Success(copy);
}

} // namespace rocks::detail
