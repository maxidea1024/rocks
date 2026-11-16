#pragma once

#include <Task/WorkerScheduler.h>

namespace rocks {

// WorkerScheduler 위에서 작업을 넣은 순서대로 하나씩 실행하는 직렬 체인이다(UE UE::Tasks::FPipe,
// 렌더 그래프 설정 작업 체인 대응). 전용 스레드 없이 공유 자원 하나에 대한 작업을 직렬화한다.
// 체인은 가볍고 여러 개 둘 수 있다. 전용 스레드가 필요하면 DedicatedLane을 쓴다.
//
// 사용 예:
//   SerialTaskChain chain(scheduler, TaskLane::Render);
//   auto first = chain.Submit(Move(buildPasses));     // 앞 작업
//   auto second = chain.Submit(Move(compilePasses));  // first가 끝난 뒤 실행
//   chain.Wait();                                     // 지금까지 넣은 작업이 모두 끝날 때까지
//
// 계약:
// - 새 작업은 직전에 수락한 작업에 순서 전용 선행(WorkerSubmitOptions::orderDependencies)을 건다.
//   그래서 직전 작업이 끝난 뒤(terminal)에만 시작하고 체인 안의 두 작업은 겹쳐 실행되지 않는다.
//   직전 작업이 실패·취소·회수되어도 다음 작업은 실행한다(체인은 순서만 정하고 결과를 잇지 않는다).
// - dependencies는 일반 선행이다. 그 가운데 하나라도 실패·취소되면 그 작업만
//   DependencyFailed이고 체인의 다음 작업은 그대로 실행한다. 순서 전용 선행 1개가 scheduler의
//   maxDependencies 한도를 함께 쓴다.
// - 실행 순서는 수락 순서다. 여러 스레드가 동시에 Submit하면 체인 내부 잠금을 얻은 순서가
//   수락 순서다.
// - 작업 body와 capture 정리는 lane 표지 lane 안에서 실행한다(CurrentTaskLane() == lane).
// - 실패·body 보존·취소·대기 규칙은 WorkerScheduler::Submit·WorkerTaskHandle과 같다(거부되면 body를
//   소비하지 않는다).
// - scheduler는 체인보다 오래 살아야 한다. 체인을 파괴해도 이미 수락한 작업은 계속 실행한다(체인은
//   작업이 참조하는 상태를 갖지 않는다). scheduler를 Shutdown한 뒤 다시 Start하면 체인을 이어 쓸 수
//   있다(이전 세대 작업은 Shutdown이 모두 끝냈으므로 선행에서 뺀다).
// - Submit·Last·Wait는 어느 스레드에서나 부를 수 있다. Submit은 내부 잠금을 쥔 채
//   scheduler.Submit을 부르므로 scheduler allocator 콜백에서 같은 체인을 다시 부르면 교착이다.
class SerialTaskChain {
public:
    explicit SerialTaskChain(WorkerScheduler& scheduler, TaskLane lane = TaskLane::Worker) noexcept;
    ~SerialTaskChain() noexcept = default;

    SerialTaskChain(const SerialTaskChain&) = delete;
    SerialTaskChain& operator=(const SerialTaskChain&) = delete;

    // 작업을 체인 맨 뒤에 넣고 그 handle을 돌려준다. 실패: WorkerScheduler::Submit과 같다(시작
    // 전·종료 중 ShuttingDown, 용량 초과 CapacityExceeded, 잘못된 선행 InvalidArgument·WrongContext
    // 등).
    Result<WorkerTaskHandle> Submit(Function<void(WorkerTaskContext&)>&& body,
                                    Span<const WorkerTaskHandle> dependencies = {}) noexcept;

    // 마지막으로 수락한 작업의 handle 사본이다. 아직 없거나 이미 끝나 체인이 놓은 뒤면 빈
    // handle이다(IsValid false).
    Result<WorkerTaskHandle> Last() const noexcept;

    // 호출 전에 수락한 체인 작업이 모두 끝날 때까지 기다린다(마지막 작업이 끝나면 앞 작업도 모두
    // 끝났다). 기다릴 작업이 없으면 바로 Signaled다. 같은 scheduler의 worker에서 부르면
    // WrongThread다.
    Result<platform::WaitResult> Wait(uint32 timeoutMs = platform::InfiniteTimeout) const noexcept;

private:
    WorkerScheduler& scheduler_;
    TaskLane lane_;
    mutable platform::Mutex gate_;
    WorkerTaskHandle last_;
};

} // namespace rocks
