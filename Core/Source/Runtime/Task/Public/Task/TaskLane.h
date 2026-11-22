#pragma once

#include <Base/Types.h>

namespace rocks {

// 작업이 어느 lane의 일로 실행되는지 나타내는 문맥 표지다(엔진 구현 계획 8.1절 lane 표).
// 스레드가 아니라 "지금 실행 중인 일"에 붙는다. 그래서 렌더 lane이 ParallelFor로 worker에 나눠 준
// 일도 Render로 보이고(UE의 parallel rendering thread 판별 대응), 인라인 모드 lane처럼 다른 lane의
// 스레드에서 실행되는 일도 자기 lane으로 보인다.
//
// 표지는 진단·계약 검사용이다(예: 렌더 전용 함수가 Render 문맥 밖 호출을 WrongThread로 거부).
// 접근 권한·동기화를 대신하지 않는다. 어느 lane의 "그 스레드에서" 실행 중인지까지 확인해야 하면
// DedicatedLane::IsExecutingTask를 쓴다.
enum class TaskLane : uint8 {
    // 표지 밖이다(lane 작업을 실행하지 않는 스레드).
    None,
    // Object 가변 상태·World를 다루는 게임 lane이다.
    Game,
    // 일반 worker 작업이다(WorkerScheduler 기본값).
    Worker,
    // 렌더 lane과 그 lane이 나눠 준 병렬 작업이다.
    Render,
    Physics,
    Io,
    Audio,
    Gc
};

// TaskLane 값의 수다. 외부에서 받은 lane 값의 범위 검사에 쓴다(IsValidTaskLane).
inline constexpr uint32 TaskLaneCount = static_cast<uint32>(TaskLane::Gc) + 1;

constexpr bool IsValidTaskLane(TaskLane lane) noexcept {
    return static_cast<uint32>(lane) < TaskLaneCount;
}

// 현재 스레드에서 가장 안쪽 TaskLaneScope의 lane이다. 범위 밖이면 None이다. 할당·잠금 없이
// thread-local 값만 읽는다.
ROCKS_TASK_API TaskLane CurrentTaskLane() noexcept;

// 범위 동안 현재 스레드의 lane 표지를 lane으로 바꾸고 소멸 때 들어올 때의 값으로 되돌린다. 실행기
// (DedicatedLane·WorkerScheduler·SerialTaskChain·ParallelFor)가 작업 실행 구간에 직접 둔다. 게임
// 루프처럼 실행기 밖에서 lane을 표시할 때도 쓴다.
//
// 사용 예:
//   TaskLaneScope scope(TaskLane::Game);
//   RunGameFrame();   // 안에서 CurrentTaskLane() == TaskLane::Game
//
// 같은 스레드에서 생성 역순으로 파괴해야 한다(스택 객체로만 쓴다). 복사·이동은 할 수 없다.
class ROCKS_TASK_API TaskLaneScope {
public:
    explicit TaskLaneScope(TaskLane lane) noexcept;
    ~TaskLaneScope() noexcept;

    TaskLaneScope(const TaskLaneScope&) = delete;
    TaskLaneScope& operator=(const TaskLaneScope&) = delete;

private:
    TaskLane previous_;
};

} // namespace rocks
