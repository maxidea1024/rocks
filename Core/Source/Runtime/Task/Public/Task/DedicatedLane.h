#pragma once

#include <Core/Function.h>
#include <Platform/Thread.h>
#include <Task/TaskLane.h>

namespace rocks {

enum class DedicatedLaneMode : uint8 {
    // 전용 스레드 하나가 큐를 소비한다.
    Thread,
    // 스레드 없이 Post한 스레드가 큐를 소비한다(헤드리스·시험·단일 스레드 플랫폼).
    Inline
};

// 시험·호스트 정책이 lane 스레드 생성을 대신하거나 실패를 주입하는 경계다(WorkerThreadLauncher와
// 같은 계약). Success는 주어진 thread에서 entry·argument·options로 실제 Start를 했을 때만 돌려준다.
// 실패면 스레드를 만들지 않아야 한다(실패를 돌려주면서 시작했으면 Start가 그 스레드를 빈 큐로
// 끝내고 Join한 뒤 실패를 돌려준다). context는 Start 호출 동안만 쓴다.
using DedicatedLaneLauncher = platform::ThreadStatus (*)(
    void* context, platform::Thread& thread, platform::Thread::Entry entry, void* argument,
    const platform::ThreadOptions& options) noexcept;

struct DedicatedLaneConfig {
    DedicatedLaneMode mode = DedicatedLaneMode::Thread;
    // 작업 실행 중 CurrentTaskLane()이 돌려줄 값이다(예: 렌더 lane은 TaskLane::Render).
    TaskLane lane = TaskLane::None;
    // Thread 모드의 스레드 이름·우선순위·스택 크기다(Platform/Thread.h ThreadOptions). Start가
    // 이름을 복사하므로 Start 뒤에 원본을 버려도 된다. Inline 모드에서는 쓰지 않는다.
    platform::ThreadOptions thread;
    // nullptr이면 platform::Thread::Start로 시작한다. Inline 모드에서는 쓰지 않는다.
    DedicatedLaneLauncher launch = nullptr;
    void* launchContext = nullptr;
};

struct DedicatedLaneStats {
    // 수락한 작업 수와 끝난 작업 수다(Start 세대와 무관한 누계). 차이가 큐에 남았거나 실행 중인
    // 수다.
    uint64 posted = 0;
    uint64 completed = 0;
    bool running = false;
    // 일반 Post를 받는 중이다. Shutdown이 시작되면 false다(종료 중에는 lane 작업의 후속만 받는다).
    bool accepting = false;
    DedicatedLaneMode mode = DedicatedLaneMode::Thread;
};

// 직렬 FIFO 큐 하나를 한 실행 흐름이 차례로 소비하는 전용 lane 실행기다(UE 렌더 스레드·named
// thread 대응). Render·Audio·Physics처럼 전용 스레드가 필요한 lane이 쓴다. 일반 병렬 작업은
// WorkerScheduler, worker 위의 직렬 실행은 SerialTaskChain을 쓴다.
//
// 사용 예:
//   DedicatedLane lane(allocator);
//   DedicatedLaneConfig config;
//   config.lane = TaskLane::Render;
//   config.thread.name = "RenderLane";
//   lane.Start(config);
//   auto task = Function<void()>::TryCreate(allocator, [&scene]() noexcept { scene.Draw(); });
//   lane.Post(Move(task.Value()));
//   ...
//   lane.Shutdown();   // 남은 작업을 모두 실행한 뒤 스레드를 Join한다
//
// 실행 순서:
// - 작업은 수락 순서(FIFO)대로 하나씩 실행한다. 여러 생산자가 동시에 Post하면 내부 잠금을
//   얻은 순서가 수락 순서다. 같은 생산자가 차례로 Post한 작업은 그 순서를 지킨다.
// - 작업 안에서 다시 Post하면 그 작업은 큐 맨 뒤에 붙고 지금 작업이 끝난 뒤 차례가 오면 실행한다.
// - Inline 모드도 큐를 쓴다(trampoline). Post는 늘 큐에 넣고, 그 lane을 비우고 있는 실행
//   흐름이 없으면 이 Post가 큐가 빌 때까지 그 자리에서 실행한다. 그래서 작업 안의 재진입
//   Post는 중첩 실행되지 않고 큐 뒤에 붙어 Thread 모드와 실행 순서가 같다. 다른 스레드가
//   비우는 중에 Post하면 그 스레드가 실행한다.
//
// 수명·스레드:
// - Start·Shutdown은 생성 스레드 전용이다(다른 스레드는 WrongThread). Post·Flush·IsExecutingTask·
//   Stats는 어느 스레드에서나 부를 수 있다.
// - allocator는 큐 칸을 할당하며 여러 스레드의 동시 Allocate·Deallocate를 지원해야 하고 lane과 모든
//   작업보다 오래 살아야 한다.
// - 작업(Function)과 그 capture 소멸자는 lane 실행 흐름에서 실행한다. 작업은 이 lane의
//   Flush·Shutdown을 부를 수 없다(WrongThread).
// - 실행 중(Start 성공 뒤 Shutdown 전)에 파괴하면 Fatal이다. 파괴 전에 Shutdown한다.
// - Shutdown이 반환하면 lane 코드를 실행 중인 스레드가 없다(Inline 모드에서 큐를 비우던 다른
//   스레드도 잠금을 놓은 뒤로는 lane을 건드리지 않는다). 다만 파괴 전에 다른 스레드의
//   Post·Flush·Stats·IsExecutingTask 호출도 모두 반환해야 한다(거부되는 Post도 큐 칸 반환을
//   위해 lane을 쓴다).
class ROCKS_TASK_API DedicatedLane {
public:
    explicit DedicatedLane(Allocator& allocator) noexcept;
    ~DedicatedLane() noexcept;

    DedicatedLane(const DedicatedLane&) = delete;
    DedicatedLane& operator=(const DedicatedLane&) = delete;
    DedicatedLane(DedicatedLane&&) = delete;
    DedicatedLane& operator=(DedicatedLane&&) = delete;

    // lane을 연다. Thread 모드는 스레드를 만들고 그 스레드가 시작을 알린 뒤(handshake 이벤트)에
    // 돌아온다. 그래서 Start 성공 뒤의 작업은 스레드 이름·우선순위가 적용된 상태에서 실행된다.
    // 실패: 다른 스레드 WrongThread, 이미 실행 중 Busy, 잘못된 mode·스레드 옵션 InvalidArgument,
    // 스레드 생성 실패(launcher 실패 포함) OutOfMemory(OS 자원 부족), 범위 밖 lane InvalidArgument.
    // 실패하면 아무것도 바꾸지 않는다(Stats의 mode도 이전 값이다). Shutdown 뒤 다시
    // Start할 수 있다.
    Result<void> Start(const DedicatedLaneConfig& config = {}) noexcept;

    // 작업을 큐 맨 뒤에 넣는다. 성공 때만 task를 소비한다(실패하면 호출자가 그대로 가진다).
    // 실패: 빈 task InvalidArgument, 시작 전·종료 뒤 ShuttingDown, 큐 칸 할당 실패는 그 오류.
    // Shutdown이 진행 중이면 이 lane 작업 안의 Post만 받고(남은 작업이 이어 붙이는 후속을 잃지
    // 않게) 그 밖의 Post는 ShuttingDown이다. Inline 모드에서는 위 trampoline 규칙대로 반환 전에
    // 실행할 수 있다.
    Result<void> Post(Function<void()>&& task) noexcept;

    // 이 호출 전에 수락한 모든 작업(실행 중인 것 포함)이 끝날 때까지 기다린다. 그 뒤 Post한 작업은
    // 기다리지 않는다. 실패: 이 lane의 작업·capture 소멸 중이거나 Inline 모드에서 이 lane을 비우는
    // 중첩 실행 안이면 WrongThread(자기 자신을 기다리게 된다), 시작 전이면 ShuttingDown. 대기 시간
    // 제한은 없다(실행 중인 작업이 끝나지 않으면 돌아오지 않는다).
    Result<void> Flush() noexcept;

    // 새 Post를 닫고 남은 작업(종료 중 lane 작업이 넣은 후속 포함)을 모두 실행한 뒤 lane을 닫는다
    // (Drain). Thread 모드는 스레드가 끝났음을 알린 뒤(handshake 이벤트) Join한다. 성공하면 큐는
    // 비어 있고 다시 Start할 수 있다. 실패: 다른 스레드나 이 lane 실행 흐름 안 WrongThread, 시작
    // 전이면 성공(할 일 없음).
    Result<void> Shutdown() noexcept;

    // 지금 이 스레드가 이 lane의 작업(또는 그 capture 소멸)을 실행하고 있으면 true다. 스레드 ID가
    // 아니라 실행 중 표지로 판별한다. 그래서 Inline 모드에서도 lane 작업 밖(같은 스레드의 일반
    // 코드)에서 lane 전용 API를 부르는 잘못이 드러난다. 다른 lane의 작업이 이 lane의 Inline Post를
    // 불러 중첩 실행된 작업은 그 안쪽 lane만 true다.
    //
    // 사용 예(lane 전용 API 검사):
    //   Result<void> RenderScene::Submit() {
    //       if (!renderLane_.IsExecutingTask()) {
    //           return Result<void>::Failure(ErrorCode::WrongThread);
    //       }
    //       ...
    //   }
    bool IsExecutingTask() const noexcept;

    DedicatedLaneStats Stats() const noexcept;

private:
    struct Node;

    static void RunThread(void* raw) noexcept;
    // 시작이 실패했는데 스레드가 시작되어 있으면 빈 큐로 끝내고 Join한다(Start 실패 경로).
    void StopUnstartedThread() noexcept;
    // 큐 맨 앞 작업 하나를 꺼낸다. 없으면 nullptr다. gate_ 안에서 부른다.
    Node* PopLocked() noexcept;
    // 작업 하나를 실행 표지 안에서 실행하고 capture·큐 칸을 정리한 뒤 완료를 게시한다(gate_ 밖).
    void RunNode(Node* node) noexcept;
    // Inline 모드에서 큐가 빌 때까지 실행한다. draining_을 얻은 실행 흐름만 부른다.
    void DrainInline() noexcept;
    // 이 스레드가 이 lane의 실행 흐름 안(가장 안쪽이 아니어도)에 있는지 본다.
    bool IsInsideLane() const noexcept;

    Allocator& allocator_;
    uint64 owner_;
    mutable platform::Mutex gate_;
    // 큐에 작업이 들어옴·종료 요청·작업 완료·Inline 비우기 끝을 모두 알린다(대기자는 조건을 다시
    // 본다).
    platform::ConditionVariable changed_;
    // Thread 모드 시작·종료 handshake다. lane 스레드가 진입 직후 started_, 루프를 마친 뒤
    // stopped_를 켠다.
    platform::ManualResetEvent started_;
    platform::ManualResetEvent stopped_;
    platform::Thread thread_;
    Node* head_ = nullptr;
    Node* tail_ = nullptr;
    uint64 posted_ = 0;
    uint64 completed_ = 0;
    DedicatedLaneMode mode_ = DedicatedLaneMode::Thread;
    TaskLane lane_ = TaskLane::None;
    // Start 성공부터 Shutdown 끝까지 true다.
    bool running_ = false;
    // 일반 Post를 받는다. Shutdown이 시작되면 false다(lane 작업 안의 Post는 stopping_ 동안 받는다).
    bool accepting_ = false;
    bool stopping_ = false;
    // Inline 모드에서 어떤 실행 흐름이 큐를 비우는 중이다.
    bool draining_ = false;
};

} // namespace rocks
