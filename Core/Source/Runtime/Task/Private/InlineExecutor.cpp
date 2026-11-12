#include <Task/TaskScheduler.h>
#include <Platform/Clock.h>

namespace rocks {

// 결과 보관 수명과 캡처 수명을 분리한다. observer를 오래 유지해도 사용자 자원은 남지 않는다.
struct TaskHandle::Record {
    Allocator* allocator;
    uint64 owner;
    usize references;
    TaskState state;
    ErrorCode error;
};

TaskHandle::TaskHandle(Record* record) noexcept : record_(record) {
}

TaskHandle::TaskHandle(const TaskHandle& other) noexcept : record_(other.record_) {
    if (record_ != nullptr) {
        ROCKS_ASSERT(record_->owner == platform::CurrentThreadId());
        ++record_->references;
    }
}

TaskHandle::TaskHandle(TaskHandle&& other) noexcept : record_(other.record_) {
    other.record_ = nullptr;
}

TaskHandle& TaskHandle::operator=(const TaskHandle& other) noexcept {
    if (this != &other) {
        TaskHandle copy(other);
        *this = Move(copy);
    }
    return *this;
}

TaskHandle& TaskHandle::operator=(TaskHandle&& other) noexcept {
    if (this != &other) {
        Reset();
        record_ = other.record_;
        other.record_ = nullptr;
    }
    return *this;
}

TaskHandle::~TaskHandle() noexcept {
    Reset();
}

void TaskHandle::Reset() noexcept {
    // allocator의 해제 경로가 재진입하더라도 같은 handle을 중복 반환하지 않도록 먼저 비운다.
    Record* record = record_;
    record_ = nullptr;
    if (record == nullptr) {
        return;
    }
    ROCKS_ASSERT(record->owner == platform::CurrentThreadId());
    ROCKS_ASSERT(record->references > 0);
    if (--record->references == 0) {
        Allocator* allocator = record->allocator;
        DestroyAt(record);
        ROCKS_ASSERT(allocator->Deallocate(record).IsOk());
    }
}

Result<TaskState> TaskHandle::State() const noexcept {
    if (record_ == nullptr) {
        return Result<TaskState>::Failure(ErrorCode::StaleHandle);
    }
    if (record_->owner != platform::CurrentThreadId()) {
        return Result<TaskState>::Failure(ErrorCode::WrongThread);
    }
    return Result<TaskState>::Success(record_->state);
}

ErrorCode TaskHandle::Failure() const noexcept {
    if (record_ == nullptr) {
        return ErrorCode::StaleHandle;
    }
    if (record_->owner != platform::CurrentThreadId()) {
        return ErrorCode::WrongThread;
    }
    return record_->error;
}

TaskHandle::operator bool() const noexcept {
    return record_ != nullptr;
}

InlineExecutor::InlineExecutor(Allocator& allocator) noexcept
    : allocator_(allocator), owner_(platform::CurrentThreadId()) {
}

Result<TaskHandle> InlineExecutor::Submit(TaskDesc&& description) noexcept {
    if (platform::CurrentThreadId() != owner_) {
        return Result<TaskHandle>::Failure(ErrorCode::WrongThread);
    }
    if (closed_) {
        return Result<TaskHandle>::Failure(ErrorCode::ShuttingDown);
    }
    if (!description.body) {
        return Result<TaskHandle>::Failure(ErrorCode::InvalidArgument);
    }
    // caller 캡처를 이동하기 전에 완료 record 전체를 확보한다. 할당 실패 후에도
    // caller는 원래 body를 재시도하거나 직접 정리할 수 있다.
    auto memory = allocator_.Allocate(sizeof(TaskHandle::Record), alignof(TaskHandle::Record));
    if (!memory.IsOk()) {
        return Result<TaskHandle>::Failure(memory.Error());
    }
    auto* record = ConstructAt<TaskHandle::Record>(memory.Value(), &allocator_, owner_, 1,
                                                   TaskState::Succeeded, ErrorCode::None);
    Function<void(TaskContext&)> body(Move(description.body));
    TaskContext context;
    body(context);
    // 캡처 정리는 lock 없이 즉시 수행하며 executor가 별도 할당을 요구하지 않는다.
    // 사용자 소멸자의 재진입이 끝난 뒤 완료 상태를 공개하므로 handle 수명이
    // 캡처 자원의 수명을 연장하지 않는다. 사용자 정리 코드는 noexcept여야 한다.
    body.Reset();
    record->error = context.Error();
    record->state = context.Error() == ErrorCode::None ? TaskState::Succeeded : TaskState::Failed;
    return Result<TaskHandle>::Success(TaskHandle(record));
}

void InlineExecutor::Close() noexcept {
    ROCKS_ASSERT(platform::CurrentThreadId() == owner_);
    closed_ = true;
}
} // namespace rocks
