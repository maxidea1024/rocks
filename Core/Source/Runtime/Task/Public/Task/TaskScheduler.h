#pragma once

#include <Core/Function.h>

namespace rocks {

enum class TaskState : uint8 {
    Succeeded,
    Failed
};

// Submit 호출 동안만 유효한 실행 결과 통로다. 참조를 보관해 호출 이후 사용하지 않는다.
class TaskContext {
public:
    // 실행을 중단하지 않고 결과 코드만 기록한다. 여러 번 부르면 마지막 코드가 결과가 된다.
    void Fail(ErrorCode error) noexcept {
        error_ = error;
    }

    ErrorCode Error() const noexcept {
        return error_;
    }

private:
    ErrorCode error_ = ErrorCode::None;
};

struct TaskDesc {
    Function<void(TaskContext&)> body;
};

// 완료 결과만 공유하며 body 캡처를 보존하지 않는다. 복사·조회·해제는 생성 스레드에 한정한다.
// 내부 카운터는 atomic이 아니며 allocator는 마지막 handle 해제까지 살아 있어야 한다.
class TaskHandle {
public:
    TaskHandle() noexcept = default;
    TaskHandle(const TaskHandle&) noexcept;
    TaskHandle(TaskHandle&&) noexcept;
    TaskHandle& operator=(const TaskHandle&) noexcept;
    TaskHandle& operator=(TaskHandle&&) noexcept;
    ~TaskHandle() noexcept;
    void Reset() noexcept;
    // 빈 handle은 StaleHandle, 다른 스레드의 조회는 WrongThread를 반환한다.
    Result<TaskState> State() const noexcept;
    ErrorCode Failure() const noexcept;
    explicit operator bool() const noexcept;

private:
    struct Record;
    explicit TaskHandle(Record*) noexcept;
    Record* record_ = nullptr;
    friend class InlineExecutor;
};

// 현재는 생성 스레드에서 body를 즉시 실행하는 동기 기준 구현이다.
// worker·의존성 그래프·취소·별도 cleanup lane은 아직 제공하지 않는다.
// 성공 반환 시 body와 캡처 정리가 모두 끝나며, handle은 executor보다 오래 보관할 수 있다.
// 단, allocator는 executor와 남은 모든 완료 handle보다 오래 살아야 한다.
class InlineExecutor {
public:
    explicit InlineExecutor(Allocator&) noexcept;
    // 승인 전 오류는 caller의 body 소유권을 유지한다. 성공 승인 뒤에는 body를 소비한다.
    Result<TaskHandle> Submit(TaskDesc&&) noexcept;
    // 소유 스레드에서 새 제출만 닫는다. 이미 실행 중인 재진입 호출의 body는 중단하지 않는다.
    void Close() noexcept;

private:
    Allocator& allocator_;
    uint64 owner_;
    bool closed_ = false;
};
} // namespace rocks
