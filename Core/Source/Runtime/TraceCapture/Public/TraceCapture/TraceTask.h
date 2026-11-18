#pragma once

#include <Base/Types.h>
#include <Core/Error.h>
#include <TraceCapture/TraceChannel.h>
#include <TraceCapture/TraceContext.h>
#include <TraceCapture/TraceCpuScope.h>

namespace rocks {

// 표준 채널 "task"다. 세션이 Start 때 등록하며 처음에는 꺼져 있다.
extern TraceChannel TraceTaskChannel;

// task의 trace 신원이다(Docs/Design/TraceSystem.ko.md 7절). id는 세션마다 1부터 매기는 64비트
// 카운터이고(task 객체 주소는 쓰지 않는다), serial은 그 세션이다. 만들 때 채널이 꺼져 있었으면 id가
// 0이고, 그 task의 모든 사건 함수는 분기 하나로 끝난다.
struct TraceTaskId {
    uint64 value = 0;
    uint64 serial = 0;

    bool IsTraced() const noexcept {
        return value != 0;
    }
};

// task를 만들 때 받는 trace 정보다. 실행할 스레드에서 복원할 문맥 사본을 함께 가진다.
struct TraceTaskInfo {
    TraceTaskId id;
    TraceContextToken context;
};

namespace detail {
TraceTaskInfo CreateTraceTask() noexcept;
void WriteTraceTaskEvent(uint8 kind, const TraceTaskId& task, uint64 related,
                         ErrorCode error) noexcept;
uint64 CurrentTraceTask() noexcept;
void BeginTraceTaskExecution(const TraceTaskId& task, uint64& previousTask) noexcept;
void EndTraceTaskExecution(const TraceTaskId& task, uint64 previousTask) noexcept;
TraceScopeName& TraceTaskRunName() noexcept;
} // namespace detail

// task를 만든다(사건 Created). 채널 "task"가 켜져 있으면 세션 task id를 받고 현재 문맥을 저장한다.
// 꺼져 있으면 분기 하나로 빈 정보를 돌려준다.
//
// 사용 예(scheduler 안):
//   record->trace = TraceCreateTask();
//   for (each dependency) TraceTaskDependency(record->trace.id, dependency->trace.id);
inline TraceTaskInfo TraceCreateTask() noexcept {
    if (TraceTaskChannel.IsEnabled()) {
        return detail::CreateTraceTask();
    }

    // 상관 id 전파는 채널과 무관하게 한다(로그가 같은 값을 쓴다, 로깅 L15). 스택이 비면 분기
    // 하나다.
    TraceTaskInfo info;
    if (CorrelationDepth() != 0) {
        info.context = CaptureCorrelation();
    }

    return info;
}

// 의존 추가(앞 prerequisite → 뒤 task)다. 둘 중 하나라도 기록하지 않은 task면 아무것도 하지 않는다.
inline void TraceTaskDependency(const TraceTaskId& task, const TraceTaskId& prerequisite) noexcept {
    if (task.IsTraced() && prerequisite.IsTraced()) {
        detail::WriteTraceTaskEvent(TraceStandardFormat::TaskDependencyAdded, task,
                                    prerequisite.serial == task.serial ? prerequisite.value : 0,
                                    ErrorCode::None);
    }
}

// 실행 가능해졌다(의존이 모두 성공했다).
inline void TraceTaskReady(const TraceTaskId& task) noexcept {
    if (task.IsTraced()) {
        detail::WriteTraceTaskEvent(TraceStandardFormat::TaskReady, task, 0, ErrorCode::None);
    }
}

// 끝 상태를 게시했다. error가 None이면 성공 완료, 그 밖은 실패 완료(오류 이름을 남긴다)다.
inline void TraceTaskCompleted(const TraceTaskId& task, ErrorCode error) noexcept {
    if (task.IsTraced()) {
        detail::WriteTraceTaskEvent(TraceStandardFormat::TaskCompleted, task, 0, error);
    }
}

inline void TraceTaskCancelled(const TraceTaskId& task) noexcept {
    if (task.IsTraced()) {
        detail::WriteTraceTaskEvent(TraceStandardFormat::TaskCancelled, task, 0, ErrorCode::None);
    }
}

// 이 스레드가 task를 기다리기 시작하고 끝냈다. 기다리는 쪽이 task 실행 중이면 그 task id가 함께
// 남는다(task 그래프의 기다림 간선).
inline void TraceTaskWaitBegin(const TraceTaskId& waited) noexcept {
    if (waited.IsTraced()) {
        detail::WriteTraceTaskEvent(TraceStandardFormat::TaskWaitBegin,
                                    TraceTaskId{detail::CurrentTraceTask(), waited.serial},
                                    waited.value, ErrorCode::None);
    }
}

inline void TraceTaskWaitEnd(const TraceTaskId& waited) noexcept {
    if (waited.IsTraced()) {
        detail::WriteTraceTaskEvent(TraceStandardFormat::TaskWaitEnd,
                                    TraceTaskId{detail::CurrentTraceTask(), waited.serial},
                                    waited.value, ErrorCode::None);
    }
}

// task 실행 구간이다. 생성 때 Started를 기록하고, 저장한 문맥을 복원하고, CPU 구간 "Task.Run"을
// 연다. 소멸 때 Finished를 기록하고 역순으로 닫는다. 기록하지 않은 task면 아무것도 하지 않는다.
// 스레드: 실행하는 스레드의 스택 객체로 쓴다.
class TraceTaskScope {
public:
    explicit TraceTaskScope(const TraceTaskInfo& info) noexcept
        : id_(info.id), context_(info.context) {
        if (id_.IsTraced()) {
            detail::BeginTraceTaskExecution(id_, previousTask_);
            if (TraceCpuChannel.IsEnabled()) {
                run_ = detail::BeginCpuScope(detail::TraceTaskRunName());
            }
        }
    }

    ~TraceTaskScope() noexcept {
        if (id_.IsTraced()) {
            if (run_ != 0) {
                detail::EndCpuScope(run_);
            }

            detail::EndTraceTaskExecution(id_, previousTask_);
        }
    }

    TraceTaskScope(const TraceTaskScope&) = delete;
    TraceTaskScope& operator=(const TraceTaskScope&) = delete;

private:
    TraceTaskId id_;
    TraceContextScope context_;
    uint64 previousTask_ = 0;
    uint64 run_ = 0;
};

} // namespace rocks
