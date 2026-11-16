#pragma once

#include <Core/Traits.h>
#include <Task/WorkerScheduler.h>

namespace rocks {

struct ParallelForOptions {
    // batch 하나가 맡는 최소 원소 수다. 원소 하나의 일이 가벼우면 크게 잡아 분배 비용을 나눈다. 0은
    // 1로 본다.
    usize minBatchSize = 1;
    // 호출 스레드를 돕는 helper 작업의 최대 수다. 실제 수는 scheduler worker 수와 (batch 수 - 1)도
    // 넘지 않는다. 0이면 호출 스레드 혼자 실행한다(UE ForceSingleThread 대응).
    uint32 maxHelpers = NumericLimits<uint32>::Max();
};

namespace detail {

using ParallelForInvoke = ErrorCode (*)(void* body, usize begin, usize end) noexcept;

Result<void> RunParallelFor(WorkerScheduler& scheduler, Allocator& scratch, usize count, void* body,
                            ParallelForInvoke invoke, const ParallelForOptions& options) noexcept;

} // namespace detail

// [0, count)를 연속 batch로 나눠 호출 스레드와 worker helper가 함께 실행한다(UE ParallelFor 대응).
// body(begin, end)는 [begin, end) 범위를 처리하고 void 또는 ErrorCode를 돌려준다.
//
// 사용 예:
//   auto done = ParallelFor(scheduler, scratch, meshes.Size(),
//                           [&](usize begin, usize end) noexcept {
//                               for (usize index = begin; index < end; ++index) {
//                                   bounds[index] = ComputeBounds(meshes[index]);
//                               }
//                           });
//
// 계약:
// - 반환하면 모든 batch 실행과 helper 작업의 정리가 끝났다. 그래서 body와 그것이 참조하는 호출자의
//   지역 상태를 바로 버려도 된다. body는 복사하지 않고 참조로 여러 스레드에서 동시에 부르므로
//   범위마다 겹치지 않는 출력을 쓰거나 스스로 동기화해야 한다.
// - batch는 번호 순서로 하나씩 가져간다. body가 None이 아닌 ErrorCode를 돌려주면 새 batch를
//   가져가지 않고(이미 가져간 batch는 끝까지 실행) 실패한 batch 가운데 가장 앞 batch의 오류를
//   돌려준다. 실패한 batch보다 앞선 batch는 모두 실행되므로 결과는 순차 실행에서 처음 만나는
//   실패와 같다.
// - helper는 호출 스레드의 lane 표지를 물려받는다(CurrentTaskLane()이 None이면 Worker). 그래서 렌더
//   lane이 부른 ParallelFor의 batch는 worker에서도 Render 문맥이다.
// - 아직 worker가 꺼내지 않은 helper는 호출 스레드가 일을 마친 뒤 회수하고(TryRevoke) 실행 중인
//   helper만 기다린다. 그 helper는 가져간 batch 하나를 마치면 돌아온다.
// - 같은 scheduler의 worker 안에서 부르거나, scheduler가 시작 전·종료 중이거나, helper 제출·할당이
//   실패하면 그만큼 helper 없이 호출 스레드가 남은 batch를 모두 실행한다(worker가 다른 worker
//   작업을 기다리지 않는다는 scheduler 규칙). 이때도 결과는 같다.
// - scratch는 helper 작업 본문(Function)을 할당하며 여러 스레드의 해제를 지원해야 한다.
//   count가 0이면 body를 부르지 않고 성공한다.
template <typename Body>
Result<void> ParallelFor(WorkerScheduler& scheduler, Allocator& scratch, usize count, Body&& body,
                         const ParallelForOptions& options = {}) noexcept {
    using Stored = RemoveReferenceT<Body>;
    const detail::ParallelForInvoke invoke = [](void* object, usize begin,
                                                usize end) noexcept -> ErrorCode {
        auto& callable = *static_cast<Stored*>(object);
        if constexpr (IsSameV<decltype(callable(begin, end)), void>) {
            callable(begin, end);
            return ErrorCode::None;
        } else {
            return callable(begin, end);
        }
    };
    return detail::RunParallelFor(scheduler, scratch, count,
                                  const_cast<void*>(static_cast<const void*>(&body)), invoke,
                                  options);
}

} // namespace rocks
