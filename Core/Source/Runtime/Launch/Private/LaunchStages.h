#pragma once

#include <Base/Types.h>
#include <Platform/CrashReporting.h>

// Launch 시작 단계의 순서 실행기다(설계 8.1절). RunApplication이 실제 단계로 쓰고, 시험이 가짜
// 단계로 순서·정리 계약을 확인한다.
namespace rocks::launch::detail {

// 단계 시작이 실패했을 때의 처리다.
enum class StageFailure : uint8 {
    Abort,    // 시작한 단계만 역순으로 정리하고 failureExitCode로 끝낸다(본문을 부르지 않는다)
    Continue, // 그 단계 없이 계속한다(정리도 하지 않는다). 시작 함수가 진단을 남긴다
};

template <typename State>
struct LaunchStage {
    const char* name; // 정적 수명. 크래시 기록의 단계 이름이다
    bool (*start)(State& state) noexcept;
    void (*stop)(State& state) noexcept; // nullptr이면 정리할 것이 없다
    StageFailure failure;
    int failureExitCode;
};

// 본문 단계 이름이다.
inline constexpr char MainStageName[] = "Main";

// stages를 순서대로 시작하고 모두 끝나면 body를 부른다. 그 뒤(또는 Abort 실패 뒤) 시작에 성공한
// 단계만 역순으로 한 번씩 정리한다. 단계마다 state.MarkStage(phase, name)를 부른다(시작: Startup,
// 본문: Running + MainStageName, 정리: Shutdown). 반환값은 body의 값이나 Abort 단계의 종료 코드다.
template <typename State, usize Count>
int RunLaunchStages(const LaunchStage<State> (&stages)[Count], State& state,
                    int (*body)(State& state) noexcept) noexcept {
    bool started[Count] = {};
    usize reached = 0;
    bool aborted = false;
    int exitCode = 0;
    for (; reached < Count; ++reached) {
        const LaunchStage<State>& stage = stages[reached];
        state.MarkStage(platform::RunPhase::Startup, stage.name);
        if (stage.start(state)) {
            started[reached] = true;
            continue;
        }

        if (stage.failure == StageFailure::Abort) {
            exitCode = stage.failureExitCode;
            aborted = true;
            break;
        }
    }

    if (!aborted) {
        state.MarkStage(platform::RunPhase::Running, MainStageName);
        exitCode = body(state);
    }

    for (usize index = reached; index-- > 0;) {
        if (started[index] && stages[index].stop != nullptr) {
            state.MarkStage(platform::RunPhase::Shutdown, stages[index].name);
            stages[index].stop(state);
        }
    }

    return exitCode;
}

} // namespace rocks::launch::detail
