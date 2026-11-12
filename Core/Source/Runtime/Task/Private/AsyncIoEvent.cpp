#include <Task/AsyncIoEvent.h>

#include <Core/FileError.h>
#include <Core/Move.h>

namespace rocks {
namespace {

// 완료 콜백 문맥이다. 콜백이 이벤트를 완료한 뒤 스스로 해제한다(마지막 사용자).
struct IoCompletion {
    IoCompletion(Allocator& owner, WorkerTaskEvent&& completion, usize expected) noexcept
        : allocator(&owner), event(Move(completion)), expectedBytes(expected) {
    }

    Allocator* allocator;
    WorkerTaskEvent event;
    usize expectedBytes;
};

void Finish(IoCompletion* completion, ErrorCode error) noexcept {
    // 이미 완료·취소된 이벤트(Shutdown 등)는 false이며 그대로 둔다.
    if (error == ErrorCode::None) {
        (void)completion->event.TryTrigger();
    } else {
        (void)completion->event.TryFail(error);
    }
    Allocator* allocator = completion->allocator;
    DestroyAt(completion); // 완료한 이벤트의 소멸은 취소를 요청하지 않는다(이미 terminal).
    (void)allocator->Deallocate(completion);
}

void OnComplete(const platform::AsyncIoResult& result, void* context) noexcept {
    auto* completion = static_cast<IoCompletion*>(context);
    Finish(completion, AsyncIoCompletionError(result, completion->expectedBytes));
}

// 문맥을 만들고 submit(callback, context)로 제출한다. 즉시 실패하면 이벤트를 Fail하고 문맥을
// 정리한다.
template <typename Submit>
Result<void> SubmitWithEvent(WorkerTaskEvent&& event, usize bytes, Allocator& allocator,
                             Submit submit) noexcept {
    if (!event.IsValid()) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    auto memory = allocator.Allocate(sizeof(IoCompletion), alignof(IoCompletion));
    if (!memory) {
        return Result<void>::Failure(memory.Error()); // 이벤트는 호출자에게 남는다.
    }
    auto* completion = ConstructAt<IoCompletion>(memory.Value(), allocator, Move(event), bytes);
    const platform::FileStatus status = submit(&OnComplete, completion);
    if (!status) {
        const ErrorCode error = FileErrorCode(status);
        Finish(completion, error);
        return Result<void>::Failure(error);
    }

    return Result<void>::Success();
}

} // namespace

ErrorCode AsyncIoCompletionError(const platform::AsyncIoResult& result,
                                 usize expectedBytes) noexcept {
    if (!result.status) {
        return FileErrorCode(result.status);
    }
    return result.bytes == expectedBytes ? ErrorCode::None : ErrorCode::InvalidArgument;
}

Result<void> TrySubmitReadEvent(platform::AsyncReadFile& file, platform::AsyncIoRequest& request,
                                uint64 offset, void* destination, usize bytes,
                                WorkerTaskEvent&& event, Allocator& allocator) noexcept {
    return SubmitWithEvent(Move(event), bytes, allocator,
                           [&](platform::AsyncIoCallback callback, void* context) noexcept {
                               return file.SubmitRead(request, offset, destination, bytes, callback,
                                                      context);
                           });
}

Result<void> TrySubmitWriteEvent(platform::AsyncWriteFile& file, platform::AsyncIoRequest& request,
                                 uint64 offset, const void* source, usize bytes,
                                 WorkerTaskEvent&& event, Allocator& allocator) noexcept {
    return SubmitWithEvent(Move(event), bytes, allocator,
                           [&](platform::AsyncIoCallback callback, void* context) noexcept {
                               return file.SubmitWrite(request, offset, source, bytes, callback,
                                                       context);
                           });
}

} // namespace rocks
