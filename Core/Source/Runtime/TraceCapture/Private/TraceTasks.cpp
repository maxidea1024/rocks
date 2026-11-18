#include "TraceSessionInternal.h"

#include <Base/Endian.h>
#include <Platform/Clock.h>

// task 사건이다(Docs/Design/TraceSystem.ko.md 7절). Task 모듈이 만듦·실행 가능·의존 추가·시작·끝·
// 완료·취소·기다림을 부른다. 모든 사건은 순서 번호를 받는다(Trace.Task는 Ordered 형).
namespace rocks::detail {

using platform::AtomicLoad;
using Format = TraceStandardFormat;

namespace {

TraceScopeName TaskRunName{Format::TaskRunScopeName};

void WriteTaskEvent(TraceSessionGuard& guard, TraceSessionState& session, uint8 kind, uint64 task,
                    uint64 related, ErrorCode error) noexcept {
    const uint16 typeId = TraceEventTypeAccess::AcquireId(TraceTaskEventType);
    if (typeId == TraceEventType::NoId) {
        return;
    }

    uint8 fixed[Format::TaskFixedBytes];
    StoreLittleEndian<uint64>(fixed + Format::TaskTickOffset, platform::MonotonicTicks());
    StoreLittleEndian<uint64>(fixed + Format::TaskIdOffset, task);
    StoreLittleEndian<uint64>(fixed + Format::TaskRelatedOffset, related);
    fixed[Format::TaskKindOffset] = kind;
    // 오류는 이름으로 남긴다(ErrorCode 선언 순서는 영속 형식이 아니다).
    const TraceVariablePart parts[] = {TraceVariablePart::FromString(
        error == ErrorCode::None ? StringView() : StringView(ErrorCodeName(error)))};
    const TraceWriteResult written = WriteSessionRecord(session, guard.Slot(), TraceTaskEventType,
                                                        typeId, fixed, sizeof(fixed), parts);
    (void)written;
}

} // namespace

TraceScopeName& TraceTaskRunName() noexcept {
    return TaskRunName;
}

TraceTaskInfo CreateTraceTask() noexcept {
    TraceTaskInfo info;
    // 문맥 사본은 세션과 무관하게 받는다(상관 id 전파, 로깅 L15). 세션이 있으면 정의도 미리 쓴다.
    info.context = TraceCaptureContext();
    const uint64 serial = AtomicLoad<MemoryOrder::Acquire>(&currentSessionSerial);
    if (serial == 0) {
        return info;
    }

    TraceSessionGuard guard;
    TraceSessionState* session = guard.Session(serial);
    if (session == nullptr) {
        return info;
    }

    info.id.value = platform::AtomicFetchAdd<MemoryOrder::Relaxed>(&session->nextTaskId, 1) + 1;
    info.id.serial = serial;
    WriteTaskEvent(guard, *session, Format::TaskCreated, info.id.value, 0, ErrorCode::None);
    return info;
}

void WriteTraceTaskEvent(uint8 kind, const TraceTaskId& task, uint64 related,
                         ErrorCode error) noexcept {
    TraceSessionGuard guard;
    // task를 만든 세션일 때만 쓴다(다른 세션의 id는 뜻이 없다).
    if (TraceSessionState* session = guard.Session(task.serial)) {
        WriteTaskEvent(guard, *session, kind, task.value, related, error);
    }
}

uint64 CurrentTraceTask() noexcept {
    return CurrentTraceThreadSlot().currentTask;
}

void BeginTraceTaskExecution(const TraceTaskId& task, uint64& previousTask) noexcept {
    TraceThreadSlot& slot = CurrentTraceThreadSlot();
    previousTask = slot.currentTask;
    slot.currentTask = task.value;
    WriteTraceTaskEvent(Format::TaskStarted, task, 0, ErrorCode::None);
}

void EndTraceTaskExecution(const TraceTaskId& task, uint64 previousTask) noexcept {
    WriteTraceTaskEvent(Format::TaskFinished, task, 0, ErrorCode::None);
    CurrentTraceThreadSlot().currentTask = previousTask;
}

} // namespace rocks::detail
