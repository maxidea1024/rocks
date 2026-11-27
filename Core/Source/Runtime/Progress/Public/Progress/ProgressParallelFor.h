#pragma once

#include <Progress/ProgressBindingSuspend.h>
#include <Progress/ProgressCounter.h>
#include <Task/ParallelFor.h>

namespace rocks {

// 진행을 계수로 보고하는 ParallelFor다(Docs/Design/ProgressReporting 6.5절). 호출 스레드의 바인딩을
// ProgressBindingSuspend로 떼고(호출 스레드도 조각을 실행하므로 조각 안의 라이브러리 구간이 Primary
// 진행을 흐리지 않게) 원소 하나가 끝날 때마다 counter.Add(1)을 한다. body(index)는 void 또는
// ErrorCode를 돌려준다. 나머지 계약은 ParallelFor와 같다.
//
// 사용 예:
//   ProgressScope scope(1.0);
//   scope.EnterStep(1.0, compileText);
//   auto counter = scope.MakeCounter(shaders.Size());
//   if (!counter) { return Result<void>::Failure(counter.Error()); }
//   auto done = ProgressParallelFor(scheduler, scratch, shaders.Size(), counter.Value(),
//                                   [&](usize index) noexcept { Compile(shaders[index]); });
//
// 직접 ParallelFor를 부르는 코드는 바인딩된 스레드에서 ProgressBindingSuspend를 걸어야 한다(규칙).
template <typename Body>
Result<void> ProgressParallelFor(WorkerScheduler& scheduler, Allocator& scratch, usize count,
                                 ProgressCounter& counter, Body&& body,
                                 const ParallelForOptions& options = {}) noexcept {
    ProgressBindingSuspend suspend;
    return ParallelFor(
        scheduler, scratch, count,
        [&](usize begin, usize end) noexcept -> ErrorCode {
            for (usize index = begin; index < end; ++index) {
                if constexpr (IsSameV<decltype(body(index)), void>) {
                    body(index);
                } else {
                    static_assert(IsSameV<decltype(body(index)), ErrorCode>,
                                  "ProgressParallelFor body must return void or ErrorCode");
                    const ErrorCode error = body(index);
                    if (error != ErrorCode::None) {
                        return error;
                    }
                }

                counter.Add(1);
            }

            return ErrorCode::None;
        },
        options);
}

} // namespace rocks
