#pragma once

#include <Core/Containers/Span.h>
#include <Core/Function.h>
#include <Platform/Thread.h>

namespace rocks {
namespace detail {
struct WorkerState;
struct WorkerRecord;
} // namespace detail

enum class WorkerTaskState {
    Pending,
    Running,
    Completing,
    Succeeded,
    Failed,
    Cancelled
};

struct WorkerTaskStatus {
    WorkerTaskState state = WorkerTaskState::Pending;
    ErrorCode error = ErrorCode::None;
    bool cancellationRequested = false;
};

// body 호출 동안 빌리는 실행 문맥이다. 저장하거나 다른 thread에서 사용하지 않는다.
// 취소는 협력적 요청이며 실행 중 함수를 강제 중단하거나 이미 발생한 효과를 되돌리지 않는다.
class WorkerTaskContext {
public:
    bool IsCancellationRequested() const noexcept;
    void Fail(ErrorCode error) noexcept;

    ErrorCode Error() const noexcept {
        return error_;
    }

private:
    explicit WorkerTaskContext(detail::WorkerRecord* record) noexcept : record_(record) {
    }

    detail::WorkerRecord* record_;
    ErrorCode error_ = ErrorCode::None;
    friend class WorkerScheduler;
};

// scheduler 종료·파괴 뒤에도 결과를 유지한다. allocator는 마지막 handle보다 오래 살아 있어야 한다.
// 별도 handle 래퍼의 동시 호출은 허용하지만 같은 래퍼의 move/Reset 경합은 caller가 막는다.
// terminal 관측은 body capture와 dependency 보존분 정리가 모두 끝난 뒤에만 가능하다.
class WorkerTaskHandle {
public:
    WorkerTaskHandle() noexcept = default;
    WorkerTaskHandle(WorkerTaskHandle&&) noexcept;
    WorkerTaskHandle& operator=(WorkerTaskHandle&&) noexcept;
    WorkerTaskHandle(const WorkerTaskHandle&) = delete;
    WorkerTaskHandle& operator=(const WorkerTaskHandle&) = delete;
    ~WorkerTaskHandle() noexcept;
    Result<WorkerTaskHandle> Clone() const noexcept;
    void Reset() noexcept;

    bool IsValid() const noexcept {
        return record_ != nullptr;
    }

    Result<WorkerTaskStatus> Status() const noexcept;
    Result<void> RequestCancel() const noexcept;
    // 아직 worker가 꺼내지 않은(Pending) 작업을 호출 스레드에서 바로 회수해 Cancelled로 끝낸다.
    // body는 실행하지 않고 capture·dependency 보존분은 이 스레드가 정리한다(gate 밖). worker가 비지
    // 않아도 끝나므로, 호출 스레드가 모든 일을 마친 뒤 남은 helper 작업을 기다리지 않을 때 쓴다.
    // 회수했으면 true, 이미 실행 중이거나 끝났으면 false(아무것도 바꾸지 않는다)다. 이 작업에
    // 의존한 작업은 RequestCancel과 같이 DependencyFailed가 된다. invalid handle은 StaleHandle이다.
    //
    // 사용 예:
    //   auto revoked = helper.TryRevoke();
    //   if (revoked && !revoked.Value()) {
    //       helper.Wait();   // 이미 실행 중이면 끝날 때까지 기다린다
    //   }
    Result<bool> TryRevoke() const noexcept;
    // timeout은 TimedOut 값이며 취소하지 않는다. invalid handle은 StaleHandle이다.
    // 같은 scheduler의 worker/body/capture cleanup 중 호출은 완료 여부와 무관하게 WrongThread다.
    Result<platform::WaitResult> Wait(uint32 timeoutMs = platform::InfiniteTimeout) const noexcept;

private:
    explicit WorkerTaskHandle(detail::WorkerRecord* record) noexcept : record_(record) {
    }

    detail::WorkerRecord* record_ = nullptr;
    friend class WorkerScheduler;
    friend class WorkerTaskEvent;
};

// 외부 완료 이벤트다(UE 5.6 UE::Tasks::FTaskEvent 대응). body가 없고 worker가 실행하지 않으며,
// 호출자가 TryTrigger·TryFail로 완료한다. 파일 IO 완료 콜백·다른 시스템의 신호를 scheduler 작업의
// dependency로 잇는다(예: 비동기 읽기가 끝나면 해제 작업 실행, Task/AsyncIoEvent.h).
//
// 사용 예:
//   auto event = scheduler.TryCreateEvent();
//   auto after = Span<const WorkerTaskHandle>(&event.Value().Handle(), 1);
//   auto decode = scheduler.Submit(Move(body), after);
//   // 다른 스레드(IO 완료 등)에서:
//   event.Value().TryTrigger();   // decode가 실행된다(TryFail이면 DependencyFailed)
//
// 계약:
// - Pending 동안 scheduler의 maxInFlight 한 칸을 쓴다. Status·Wait·Clone·dependency 사용은 일반
//   handle과 같다.
// - TryTrigger는 Succeeded, TryFail(error)은 Failed(error, None이면 InvalidArgument)로 완료하고 이
//   호출이 완료했으면 true를 돌려준다. 이미 완료·취소된 뒤면 false다(아무것도 바꾸지 않는다). 어느
//   스레드에서든 부를 수 있다(worker body 포함). 취소 요청 뒤라도 worker가 처리하기 전에
//   Trigger하면 성공으로 끝난다.
// - 취소: RequestCancel·TryRevoke·Shutdown·Trigger 없이 소멸·Pending 이벤트 위에 이동 대입하면
//   Cancelled로 끝나고, 이 이벤트에 의존한 작업은 DependencyFailed다. Shutdown(Cancel)은 바로
//   취소한다. Shutdown(Drain)은 더 진행할 수 없을 때(실행·정리 중인 작업이 없고 실행할 수 있는
//   작업도 없을 때) 남은 이벤트를 취소한다. 실행 중인 작업이 Trigger할 이벤트는 기다리지만
//   scheduler 밖(IO 등)의 완료는 기다리지 않는다. 늦은 Trigger는 false다.
// - 이동 가능하다. 빈 이벤트(기본 생성·이동된 뒤)의 Trigger·Fail은 StaleHandle이다. 완료 함수는
//   여러 스레드가 동시에 불러도 되지만, 이동·소멸은 같은 객체의 어떤 호출과도 겹치지 않아야 한다.
class WorkerTaskEvent {
public:
    WorkerTaskEvent() noexcept = default;
    WorkerTaskEvent(WorkerTaskEvent&& other) noexcept = default;
    WorkerTaskEvent& operator=(WorkerTaskEvent&& other) noexcept;
    WorkerTaskEvent(const WorkerTaskEvent&) = delete;
    WorkerTaskEvent& operator=(const WorkerTaskEvent&) = delete;
    ~WorkerTaskEvent() noexcept;

    // dependency·Wait·Status·Clone용 handle이다.
    const WorkerTaskHandle& Handle() const noexcept {
        return handle_;
    }

    bool IsValid() const noexcept {
        return handle_.IsValid();
    }

    Result<bool> TryTrigger() const noexcept;
    Result<bool> TryFail(ErrorCode error) const noexcept;

private:
    explicit WorkerTaskEvent(WorkerTaskHandle&& handle) noexcept
        : handle_(static_cast<WorkerTaskHandle&&>(handle)) {
    }

    // 아직 완료되지 않았으면 취소를 요청한다(소멸·대입 전).
    void CancelPending() noexcept;

    WorkerTaskHandle handle_;
    friend class WorkerScheduler;
};

// 테스트·호스트 정책에서 thread 생성 실패를 명시적으로 주입할 수 있는 최소 경계다.
// Success는 제공된 Thread에 entry/context로 실제 Start를 한 경우에만 반환한다.
// 실패 시 새 thread를 만들지 않으며 callback/context는 Start 호출이 끝날 때까지 유효하다.
using WorkerThreadLauncher = platform::ThreadStatus (*)(void*, platform::Thread&,
                                                        platform::Thread::Entry, void*) noexcept;

struct WorkerSchedulerConfig {
    uint32 workerCount = 1;
    uint32 maxInFlight = 256;
    uint32 maxDependencies = 16;
    WorkerThreadLauncher launch = nullptr;
    void* launchContext = nullptr;
};
enum class WorkerShutdownMode {
    Drain,
    Cancel
};

struct WorkerSchedulerStats {
    uint32 workerCount = 0;
    uint32 inFlight = 0;
    uint32 reservedSubmissions = 0;
    uint64 generation = 0;
    bool accepting = false;
};

// 기준 구현은 mutex 보호 선형 ready 탐색이다. 우선순위/work stealing/lock-free는 제공하지 않는다.
// allocator는 병행 Allocate/Deallocate를 지원해야 한다. worker<=64, inFlight<=65536,
// deps<=64이며 inFlight 한도는 pending+running+capture cleanup+제출 예약의 합이다.
// Start/Shutdown은 생성 thread 전용이다. Submit/Stats와 별도 handle 호출은 병행 가능하다.
// scheduler 파괴 전 모든 외부 API 호출을 종료한다. callback에서 scheduler 자체 파괴는 금지다.
class WorkerScheduler {
public:
    explicit WorkerScheduler(Allocator&) noexcept;
    ~WorkerScheduler() noexcept;
    WorkerScheduler(const WorkerScheduler&) = delete;
    WorkerScheduler& operator=(const WorkerScheduler&) = delete;
    Result<void> Start(WorkerSchedulerConfig = {}) noexcept;
    // body/dependency 입력 래퍼는 호출 동안 유효하고 변경되지 않아야 한다. 성공 때만 body를
    // 소비한다. 같은 Start 세대의 기존 handle만 허용하므로 새 node에서 기존 node로의 DAG만
    // 생성된다. 중복/invalid dependency는 InvalidArgument, 다른 scheduler/이전 세대는
    // WrongContext다. dependency 실패·취소는 body 없이 DependencyFailed다. 모든 거부/OOM은 caller
    // body를 보존한다.
    Result<WorkerTaskHandle> Submit(Function<void(WorkerTaskContext&)>&& body,
                                    Span<const WorkerTaskHandle> dependencies = {}) noexcept;
    // 외부 완료 이벤트를 만든다(WorkerTaskEvent). 실패: 시작 전·종료 중 ShuttingDown, maxInFlight
    // 초과 CapacityExceeded, 할당 실패는 그 오류다.
    Result<WorkerTaskEvent> TryCreateEvent() noexcept;
    // 새 제출을 닫고 cleanup·native Join을 완료한다. 강제 종료·제한시간 종료를 보장하지 않는다.
    // 완료되지 않은 외부 이벤트는 Cancel이면 바로, Drain이면 더 진행할 수 없을 때 Cancelled로
    // 끝낸다. Cancel은 pending body를 생략하고 running에는 협력적 요청을 게시한다. 명시 Fail이
    // 취소보다 우선한다. 자기 worker/capture cleanup에서는 WrongThread, lifecycle/Submit
    // 재진입에서는 Busy다. Shutdown 뒤 Start 재사용 가능하며 이전 세대 handle 결과는 그대로
    // 유지한다.
    Result<void> Shutdown(WorkerShutdownMode = WorkerShutdownMode::Drain) noexcept;
    WorkerSchedulerStats Stats() const noexcept;
    // body 및 capture/dependency cleanup 실행 문맥을 포함한다. Submit 전 deadlock 검사용이다.
    bool IsCurrentWorker() const noexcept;

private:
    static void RunWorker(void*) noexcept;
    Allocator& allocator_;
    uint64 owner_;
    mutable platform::Mutex lifecycleGate_;
    detail::WorkerState* state_ = nullptr;
    bool transitioning_ = false;
};
} // namespace rocks
