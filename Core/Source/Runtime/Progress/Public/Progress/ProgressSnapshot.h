#pragma once

#include <Core/Containers/Array.h>
#include <Localization/Text.h>
#include <Progress/ProgressTypes.h>

// 받는 쪽이 당기는 진행 상태 복사본이다(Docs/Design/ProgressReporting 7.2·7.3절). 스냅샷은 소비자
// 소유이고 해제도 소비자 스레드에서 한다. 문구는 Text::Clone(노드 공유)으로 담으므로 hub에 준
// allocator와 문구를 만든 allocator는 스냅샷이 사라질 때까지 살아 있어야 한다.
namespace rocks {

// 살아 있는 작업 하나의 상태다.
struct ProgressTaskView {
    ProgressTaskId id = NoProgressTask;
    ProgressTaskId parent = NoProgressTask;
    Text title;
    // 바깥부터 깊이별 현재 구간 문구다(messageCount개, 빈 칸은 유효하지 않은 Text).
    Text messages[MaxReportedProgressDepth];
    uint32 messageCount = 0;
    // ProgressTaskHandle::SetMessage 문구다(없으면 유효하지 않은 Text).
    Text taskMessage;
    ProgressPresentation presentation = ProgressPresentation::Background;
    ProgressShape shape = ProgressShape::Indeterminate;
    // 보이는 비율 D(0–1)다. 작업마다 뒤로 가지 않는다(5.5절 4).
    double fraction = 0.0;
    // 하위 작업을 섞지 않은 자기 진행 S(0–1)다(5.5절 1). 뒤로 갈 수 있다(가장 바깥 구간을 다시
    // 열 때) — 하위 작업 없이 자기 단계만 보이는 받는 쪽(시작 화면 7.2절)이 쓴다.
    double selfFraction = 0.0;
    // Count 모양일 때 완료 수와 총량이다(done은 total로 자른다).
    uint64 done = 0;
    uint64 total = 0;
    // hub 시계 기준 시작 시각과 마지막으로 진행이 오른 것을 스냅샷이 본 시각이다.
    uint64 startNanoseconds = 0;
    uint64 lastProgressNanoseconds = 0;
    double modalDelaySeconds = DefaultModalProgressDelay;
    bool cancellable = false;
    bool cancelRequested = false;

    // 한 줄로 보일 문구다: 가장 안쪽의 비어 있지 않은 구간 문구, 없으면 작업 문구, 없으면 제목.
    const Text& DisplayMessage() const noexcept {
        for (uint32 depth = messageCount; depth != 0; --depth) {
            if (messages[depth - 1].IsValid()) {
                return messages[depth - 1];
            }
        }

        return taskMessage.IsValid() ? taskMessage : title;
    }
};

enum class ProgressEventKind : uint8 {
    Begin,
    End
};

// 시작·끝 사건이다(7.3절). serial은 1부터 늘고 전체 순서다.
struct ProgressEvent {
    uint64 serial = 0;
    ProgressEventKind kind = ProgressEventKind::Begin;
    ProgressTaskId id = NoProgressTask;
    // Begin에만 의미가 있다.
    ProgressTaskId parent = NoProgressTask;
    Text title;
    ProgressPresentation presentation = ProgressPresentation::Background;
    ProgressCancel cancel = ProgressCancel::NotCancellable;
    // End에만 의미가 있다. detail은 Finish(outcome, detail)의 사유다(없으면 유효하지 않은 Text).
    ProgressOutcome outcome = ProgressOutcome::Succeeded;
    Text detail;
    // End 때의 보이는 비율이다(Succeeded면 1).
    double fraction = 0.0;
};

// ProgressHub::Snapshot의 출력이다. 같은 객체를 다시 쓰면 배열 용량을 재사용한다.
//
// 사용 예(소비자마다 자기 cursor):
//   ProgressSnapshot snapshot(allocator);
//   uint64 cursor = 0;                      // 0은 "지금부터"
//   if (ProgressHub::Get().Snapshot(snapshot, cursor)) {
//       for (const ProgressEvent& event : snapshot.events) { ... }
//       for (const ProgressTaskView& task : snapshot.tasks) { Draw(task); }
//   }
struct ProgressSnapshot {
    explicit ProgressSnapshot(Allocator& allocator) noexcept : tasks(allocator), events(allocator) {
    }

    ProgressSnapshot(ProgressSnapshot&&) noexcept = default;
    ProgressSnapshot& operator=(ProgressSnapshot&&) noexcept = default;
    ProgressSnapshot(const ProgressSnapshot&) = delete;
    ProgressSnapshot& operator=(const ProgressSnapshot&) = delete;

    // 살아 있는 작업이다. 부모가 자식보다 앞에 온다(시작 순서).
    Array<ProgressTaskView> tasks;
    // cursor 뒤에 쌓인 사건이다(번호 순).
    Array<ProgressEvent> events;
    // 고리가 넘쳐 이 소비자가 놓친 사건 수다.
    uint64 lostEvents = 0;
    // 스냅샷을 뜬 hub 시계 시각이다.
    uint64 takenNanoseconds = 0;
};

} // namespace rocks
