#pragma once

#include <Core/Result.h>
#include <Progress/ProgressTypes.h>

namespace rocks {

class ProgressTaskHandle;
class ProgressScope;

// 여러 스레드가 올리는 완료 수와 늘어날 수 있는 총량이다(Docs/Design/ProgressReporting 6.4절).
//
// - 작업 수준 계수(ForTask)는 작업을 계수 모양으로 만든다("셰이더 123 / 4,096"). 작업마다 하나다.
// - 구간 계수(ProgressScope::MakeCounter)는 만들 때의 현재 구간 몫을 채운다. 작업마다 동시에
//   하나이고, 그 구간이 끝난 뒤의 Add는 스냅샷이 쓰지 않는다.
//
// 사용 예:
//   auto counter = ProgressCounter::ForTask(task, jobs.Size());
//   ...                                      // 아무 worker에서
//   counter.Value().Add();
//
// 스레드: Add·AddTotal은 아무 스레드에서나 부를 수 있고 원자 더하기다(UI 스레드면 펌프 조건만 더
// 본다). 이동·소멸은 한 스레드에서 한다. 작업이 끝났거나 hub가 닫힌 뒤의 Add는 아무 일도 하지
// 않는다. 빈(아무 일도 하지 않는) 계수는 기본 생성 값이다.
class ROCKS_PROGRESS_API ProgressCounter {
public:
    ProgressCounter() noexcept = default;
    ProgressCounter(ProgressCounter&& other) noexcept;
    ProgressCounter& operator=(ProgressCounter&& other) noexcept;
    ProgressCounter(const ProgressCounter&) = delete;
    ProgressCounter& operator=(const ProgressCounter&) = delete;
    ~ProgressCounter() noexcept;

    // 작업 수준 계수를 만든다. 오류: 빈 handle InvalidArgument, hub가 닫혔으면 ShuttingDown, 끝난
    // 작업 ScopeClosed, 이미 있으면 AlreadyExists.
    static Result<ProgressCounter> ForTask(const ProgressTaskHandle& task, uint64 total) noexcept;

    void Add(uint64 done = 1) noexcept;
    void AddTotal(uint64 more) noexcept;

    // 실제 작업에 이어진 계수인지다(빈 계수면 false).
    bool IsActive() const noexcept {
        return record_ != nullptr;
    }

private:
    friend class ProgressScope;

    enum class Kind : uint8 {
        Task,
        Scope
    };

    ProgressCounter(detail::ProgressTaskRecord* record, Kind kind) noexcept
        : record_(record), kind_(kind) {
    }

    void Release() noexcept;

    detail::ProgressTaskRecord* record_ = nullptr;
    Kind kind_ = Kind::Task;
};

} // namespace rocks
