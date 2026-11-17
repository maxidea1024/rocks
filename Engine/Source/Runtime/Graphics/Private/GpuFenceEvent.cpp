#include <Graphics/GpuFenceEvent.h>

#include <Core/Move.h>

namespace rocks::graphics {

namespace {

// 통지 문맥이다. 통지가 이벤트를 완료한 뒤 스스로 해제한다(마지막 사용자).
struct FenceCompletion {
    FenceCompletion(Allocator& owner, WorkerTaskEvent&& completion) noexcept
        : allocator(&owner), event(Move(completion)) {
    }

    Allocator* allocator;
    WorkerTaskEvent event;
};

void Finish(FenceCompletion* completion, ErrorCode error) noexcept {
    // 이미 완료·취소된 이벤트(scheduler Shutdown 등)는 false이며 그대로 둔다.
    if (error == ErrorCode::None) {
        (void)completion->event.TryTrigger();
    } else {
        (void)completion->event.TryFail(error);
    }

    Allocator* allocator = completion->allocator;
    DestroyAt(completion); // 완료한 이벤트의 소멸은 취소를 요청하지 않는다(이미 terminal).
    (void)allocator->Deallocate(completion);
}

void OnFence(ErrorCode result, void* context) noexcept {
    Finish(static_cast<FenceCompletion*>(context), result);
}

} // namespace

Result<void> TrySubmitFenceEvent(Device& device, GpuFencePoint point, WorkerTaskEvent&& event,
                                 Allocator& allocator) noexcept {
    if (!event.IsValid()) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    auto memory = allocator.Allocate(sizeof(FenceCompletion), alignof(FenceCompletion));
    if (!memory) {
        return Result<void>::Failure(memory.Error()); // 이벤트는 호출자에게 남는다.
    }

    auto* completion = ConstructAt<FenceCompletion>(memory.Value(), allocator, Move(event));
    auto notified = device.NotifyFence(point, &OnFence, completion);
    if (!notified) {
        Finish(completion, notified.Error());
        return notified;
    }

    return Result<void>::Success();
}

} // namespace rocks::graphics
