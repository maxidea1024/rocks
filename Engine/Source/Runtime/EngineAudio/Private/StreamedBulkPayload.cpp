#include "StreamedBulkPayload.h"

#include <Core/Memory/UniquePtr.h>
#include <Core/Move.h>

// StreamedBulk 어댑터다(Docs/Design/AudioSystem 3.46.3절).
namespace rocks::engine_audio_detail {

namespace {

BulkReadPriority ToBulkReadPriority(SoundReadPriority priority) noexcept {
    switch (priority) {
    case SoundReadPriority::Low:
        return BulkReadPriority::Low;
    case SoundReadPriority::Normal:
        return BulkReadPriority::Normal;
    case SoundReadPriority::High:
        return BulkReadPriority::High;
    case SoundReadPriority::Critical:
        return BulkReadPriority::Critical;
    }

    return BulkReadPriority::Normal;
}

SoundReadState ToSoundReadState(BulkReadState state) noexcept {
    switch (state) {
    case BulkReadState::Pending:
        return SoundReadState::Pending;
    case BulkReadState::Done:
        return SoundReadState::Done;
    case BulkReadState::Failed:
        return SoundReadState::Failed;
    case BulkReadState::Cancelled:
        return SoundReadState::Cancelled;
    }

    return SoundReadState::Failed;
}

// 읽기 하나의 callback 중계다. 손잡이와 따로 할당한다: 손잡이는 상태가 정해진 뒤(callback 전)
// 파괴될 수 있으므로 callback이 손잡이를 만지지 않게 한다. 중계 callback이 놓는다(즉시 실패면
// Read가 놓는다).
struct ReadRelay {
    Allocator* allocator = nullptr;
    SoundReadCallback callback = nullptr;
    void* context = nullptr;
};

void RelayBulkRead(void* opaque, const BulkReadResult& result) noexcept {
    auto* relay = static_cast<ReadRelay*>(opaque);
    const SoundReadCallback callback = relay->callback;
    void* const context = relay->context;
    // callback이 payload·손잡이를 놓을 수 있으므로 중계를 먼저 놓는다.
    (void)relay->allocator->Deallocate(relay);
    callback(context, result.error);
}

class StreamedBulkRequest final : public SoundPayloadRequest {
public:
    SoundReadState State() const noexcept override {
        return ToSoundReadState(handle_.State());
    }

    void Cancel() noexcept override {
        handle_.Cancel();
    }

    void SetPriority(SoundReadPriority priority) noexcept override {
        handle_.SetPriority(ToBulkReadPriority(priority));
    }

    // Pending이면 소멸이 Fatal이다(BulkReadHandle 규칙).
    BulkReadHandle handle_;
};

class StreamedBulkPayload final : public SoundStreamedPayload {
public:
    StreamedBulkPayload(Allocator& allocator, StreamedBulk bulk) noexcept
        : allocator_(allocator), bulk_(Move(bulk)) {
    }

    uint64 Bytes() const noexcept override {
        return bulk_.Bytes();
    }

    Result<SoundPayloadRead> Read(uint64 offset, Span<uint8> destination,
                                  SoundReadPriority priority, SoundReadCallback callback,
                                  void* context) noexcept override {
        using Started = Result<SoundPayloadRead>;
        if (callback == nullptr) {
            return Started::Failure(ErrorCode::InvalidArgument);
        }

        // 손잡이를 먼저 만든다: Read가 성공한 뒤에는 실패할 일이 없어야 Pending 손잡이를 놓치지
        // 않는다.
        auto request = UniquePtr<StreamedBulkRequest>::TryMake(allocator_);
        if (!request) {
            return Started::Failure(request.Error());
        }

        auto storage = allocator_.Allocate(sizeof(ReadRelay), alignof(ReadRelay));
        if (!storage) {
            return Started::Failure(storage.Error());
        }

        auto* relay = ConstructAt<ReadRelay>(storage.Value());
        relay->allocator = &allocator_;
        relay->callback = callback;
        relay->context = context;
        // callback은 이 호출 안에서(손잡이를 돌려주기 전) 불릴 수 있다 — 그 뒤 relay를 만지지
        // 않는다.
        auto handle =
            bulk_.Read(offset, destination, ToBulkReadPriority(priority), &RelayBulkRead, relay);
        if (!handle) {
            (void)allocator_.Deallocate(relay);
            return Started::Failure(handle.Error());
        }

        request.Value()->handle_ = Move(handle.Value());
        return Started::Success(SoundPayloadRead(Move(request.Value())));
    }

private:
    Allocator& allocator_;
    StreamedBulk bulk_;
};

} // namespace

Result<SharedPtr<SoundStreamedPayload>> MakeStreamedBulkPayload(Allocator& allocator,
                                                                StreamedBulk bulk) noexcept {
    using Made = Result<SharedPtr<SoundStreamedPayload>>;
    if (!bulk.IsValid()) {
        return Made::Failure(ErrorCode::InvalidArgument);
    }

    auto made = SharedPtr<StreamedBulkPayload>::TryMake(allocator, allocator, Move(bulk));
    if (!made) {
        return Made::Failure(made.Error());
    }

    return Made::Success(SharedPtr<SoundStreamedPayload>(Move(made.Value())));
}

} // namespace rocks::engine_audio_detail
