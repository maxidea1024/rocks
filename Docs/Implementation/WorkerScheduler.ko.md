# WorkerScheduler 구현 범위와 수명 규약

`WorkerScheduler`는 기존 `InlineExecutor`를 유지하면서 실제 worker 실행, 기존 handle을 이용한 의존성, 협력적 취소, 명시적 종료를 추가한다. 공용 `Platform::Thread`, mutex, condition variable만 사용하며 운영체제 헤더와 직접 호출은 포함하지 않는다.

## 공개 인터페이스

| 기능 | 인터페이스와 계약 |
|---|---|
| 생성·시작 | `WorkerScheduler(Allocator&)`, `Start(WorkerSchedulerConfig)` |
| 제출 | `Submit(Function<void(WorkerTaskContext&)>&&, Span<const WorkerTaskHandle>)` |
| 결과 보존 | 이동 전용 `WorkerTaskHandle`, 명시적 `Clone`, `Reset`, `Status` |
| 대기 | `Wait(timeoutMs)` → `Result<platform::WaitResult>`; timeout은 작업 취소가 아니다 |
| 취소 | `RequestCancel`; 실행 중 body는 `WorkerTaskContext::IsCancellationRequested`로 관측 |
| 회수 | `TryRevoke` → `Result<bool>`; 아직 worker가 꺼내지 않은 Pending 작업을 호출 스레드에서 바로 Cancelled로 끝낸다 |
| 외부 완료 이벤트 | `TryCreateEvent` → `Result<WorkerTaskEvent>`; body 없이 `TryTrigger`·`TryFail`로 완료하는 작업(UE 5.8 `UE::Tasks::FTaskEvent` 대응). `Task/AsyncIoEvent.h`의 `TrySubmitReadEvent`·`TrySubmitWriteEvent`가 AsyncFile 완료를 이벤트로 잇는다 |
| 실패 | body의 `WorkerTaskContext::Fail(ErrorCode)`; 최종 상태의 `error`로 관측 |
| 종료 | `Shutdown(Drain 또는 Cancel)`; native Join까지 회수 |
| 실행 문맥 검사 | `IsCurrentWorker`; body와 capture/dependency 정리 문맥을 포함 |

Start와 Shutdown은 scheduler 생성 스레드 전용이다. 다른 스레드에서는 `WrongThread`를 반환한다. Submit·Stats 및 서로 다른 handle 래퍼의 읽기·보존·취소·대기는 병행할 수 있다. 동일한 래퍼를 동시에 move/Reset하거나 입력 wrapper를 allocator callback에서 변경하는 사용은 지원하지 않는다. scheduler 파괴 전 모든 외부 API 호출을 종료해야 한다.

같은 scheduler worker에서 Wait는 이미 완료한 작업을 대상으로 하더라도 `WrongThread`이다. capture 소멸 중 callback에도 같은 제한을 적용한다. worker는 Submit·Cancel·Stats를 호출할 수 있다. GC 등 worker 외부에서 기다려야 하는 소비자는 작업을 제출하기 전에 `IsCurrentWorker`를 검사하고, 일부 제출만 성공한 경우에도 수락된 작업을 모두 대기한 뒤 stack context를 파괴해야 한다.

## 수락·용량·의존성

기본 worker 수는 1, 최대 동시 보유 작업 수는 256, 작업당 의존성 한도는 16이다. 설정 범위는 worker 1–64, 보유 작업 1–65,536, 의존성 0–64이다. 용량은 pending·running·capture cleanup 중인 작업과 아직 할당을 마치지 않은 제출 예약의 합으로 적용한다. 보존한 완료 handle 수는 이 실행 용량과 별개이며, 사용자가 반환하기 전까지 결과 storage를 유지한다.

Submit은 caller Function을 이동하기 전에 입력 검증, 용량 예약, dependency 보존, record 할당을 완료한다. OOM·용량 초과·종료·잘못된 dependency로 거부하면 caller Function 소유권을 그대로 유지한다. 제출 예약 도중 Shutdown이 시작된 경우 신규 승인을 거부하고 준비한 storage와 보존분을 정리한다. worker는 모든 제출 예약 정리가 끝나야 종료할 수 있다.

dependency는 같은 scheduler의 같은 Start 세대에 이미 발급된 handle만 허용한다. 새 작업에서 기존 작업으로만 간선을 추가하므로 순환을 생성할 수 없다. 중복·invalid dependency는 `InvalidArgument`, 다른 scheduler·이전 Start 세대의 dependency는 `WrongContext`이다. 모든 dependency 성공 뒤에 body를 실행하며, dependency 실패·취소는 body 실행 없이 `DependencyFailed`로 전파한다. 완료 handle을 다음 Start의 dependency로 재사용할 수는 없지만 그 완료 결과 조회는 계속 가능하다.

## 완료와 정리 순서

작업 상태는 `Pending → Running → Completing → Succeeded/Failed/Cancelled`를 기본으로 한다. 실행을 생략하는 취소·dependency 실패는 Pending에서 Completing으로 전이한다.

1. worker가 gate 안에서 작업을 선택하고 body 소유권을 분리한다.
2. gate 밖에서 body를 실행한다.
3. gate 밖에서 Function capture와 dependency 보존분을 정리한다.
4. 정리가 끝난 뒤 terminal 상태를 게시하고 대기자에게 알린다.
5. scheduler의 실행 보존분을 반환한다.

따라서 handle 보존이 capture 자원을 불필요하게 유지하지 않으며, terminal 관측이 capture cleanup보다 먼저 발생하지 않는다. 결과 record와 공유 control storage는 별도 보존 계수로 관리한다. handle은 scheduler 파괴 후에도 유효하지만 주입한 allocator는 마지막 handle·노드 해제까지 유효하고 해당 스레드의 할당·해제를 지원해야 한다.

## 취소·종료·시작 실패

TryRevoke는 Pending 작업만 회수한다. 상태를 Completing으로 바꿔 worker가 꺼내지 못하게 한 뒤, worker 정리와 같은 순서로 capture와 dependency 보존분을 gate 밖에서 이 스레드가 정리하고 Cancelled(error Cancelled)를 게시한다. worker가 비지 않아도 끝나므로, 호출 스레드가 helper 작업과 일을 나눠 하다 먼저 끝냈을 때 남은 helper를 기다리지 않는 데 쓴다(블록 컨테이너 병렬 압축·해제). 실행 중이거나 끝난 작업은 바꾸지 않고 false를 돌려주며, 회수된 작업에 의존한 작업은 DependencyFailed가 된다.

외부 완료 이벤트(`WorkerTaskEvent`)는 body가 없는 작업 기록이다. 일반 작업처럼 maxInFlight 한 칸을 쓰고 같은 세대의 dependency·Wait·Status·Clone에 쓰인다. worker는 이벤트를 실행하지 않고 취소 요청이 있을 때만 꺼내 Cancelled로 끝낸다. `TryTrigger`는 Succeeded, `TryFail(error)`은 Failed(None이면 InvalidArgument)로, Pending일 때만 gate 안에서 terminal을 게시하고 scheduler 보존분을 반환한다(body·dependency가 없어 정리할 것이 없다). 이미 완료·취소·취소 처리 중이면 false다. 취소 요청 뒤라도 worker가 처리하기 전에 Trigger하면 성공이다. Trigger 없이 이벤트 객체가 소멸하거나 Pending 이벤트 위에 이동 대입하면 취소를 요청해 의존 작업이 영원히 기다리지 않는다(DependencyFailed). Shutdown(Cancel)은 이벤트를 바로 취소한다. Shutdown(Drain)은 이벤트를 바로 취소하지 않고, worker가 더 진행할 수 없을 때(Running·Completing 작업이 없고 고를 작업도 없을 때) 남은 이벤트를 취소해 끝난다. 그래서 실행 중인 작업이 Trigger할 이벤트는 기다리지만, scheduler 밖(IO 등)의 완료는 기다리지 않는다(교차검증에서 Drain이 실행 중인 작업의 Trigger보다 먼저 이벤트를 취소하던 결함을 고쳤다). 늦은 Trigger는 false다. `TrySubmitReadEvent`·`TrySubmitWriteEvent(file, request, …, Move(event), allocator)`는 이벤트를 소유한 완료 문맥을 allocator에 만들어 콜백 문맥으로 넘기고, IO 스레드의 콜백이 이벤트를 완료한 뒤 그 문맥을 마지막으로 해제한다. AsyncFile은 완료를 게시한 뒤 콜백을 부르므로 request.Wait() 반환이 콜백 종료를 뜻하지 않는다. 처음 설계(호출자가 binding 객체를 쥐는 방식)는 Wait 뒤 binding을 지우면 콜백이 해제된 메모리를 쓰고(ASan 재현), 이벤트를 옮기면 Trigger가 사라지는 결함이 교차검증에서 나와 이 방식으로 바꿨다. 완료 규칙은 성공이고 전송 byte가 요청과 같으면 Trigger, 짧은 전송은 InvalidArgument, 그 밖은 Core 공용 `FileErrorCode`다. 제출이 즉시 실패하면 이벤트를 그 오류로 Fail하고, 문맥 할당이 실패하면 이벤트를 호출자에게 남긴다. 압축 해제처럼 source 길이를 제출 때 고정하는 작업이 IO가 채운 버퍼를 쓰게 한다. 기록 칸 번호를 기록에 저장해 회수·이벤트 완료가 gate 안에서 선형 탐색하지 않는다.

RequestCancel은 요청 게시이며 실행 중 body를 강제로 중단하지 않는다. pending 작업은 worker가 정리할 때 body를 생략한다. 실행 중 작업은 취소를 폴링하여 반환해야 하며 이미 발생한 부수 효과를 되돌리지 않는다. 명시적 body 실패와 dependency 실패가 있으면 Failed가 우선하고, 그 외 취소 요청을 받은 작업은 body 반환·cleanup 이후 Cancelled가 된다.

Drain은 신규 제출을 닫고 수락한 작업을 완료한다. Cancel은 pending과 running 작업에 취소를 요청한 뒤 같은 정리·합류 절차를 수행한다. 무한 실행 body나 사용자 cleanup을 강제로 종료하지 않으며 제한시간 내 Shutdown 완료를 보장하지 않는다. Submit/lifecycle allocator callback에서 재진입한 Shutdown은 `Busy`이며, worker/capture cleanup에서는 `WrongThread`이다. Submit은 할당 없는 TLS 연결 프레임 스택으로 기록한다. A.Submit의 allocator가 B.Submit을 호출하고 그 allocator가 A.Shutdown을 호출하는 경우에도 바깥 A의 활성 제출을 검색하여 Busy로 거부한다. 최상단 B만 검사하면 A의 예약 반환과 worker Join이 서로 기다리는 교착이 발생한다. 제출 중이 아닌 별도 scheduler의 정상 Shutdown은 허용하며, 정상·OOM 복귀 모두 프레임을 복원한다.

선택적인 `WorkerThreadLauncher`는 공용 Thread 생성의 실패 주입과 호스트 정책을 위한 경계이다. Success는 제공된 Thread에서 지정 entry/context로 실제 Start를 한 경우에만 반환한다. 실패 시 새 thread를 만들지 않는 계약을 따른다. 부분 시작 실패는 이미 시작한 worker를 종료·Join한 뒤 배열을 해제한다. 성공한 Shutdown 후 Start를 반복할 수 있다. 첫 Start 준비 중 생성한 작은 control storage는 실패하더라도 scheduler 파괴까지 유지될 수 있다.

## 구현 및 검증 범위

현재 구현은 mutex와 선형 ready 탐색을 이용한다. 우선순위, work stealing, lock-free 큐, job affinity, fiber 대기, 성능 우위는 이 단계의 제공 범위가 아니다. `InlineExecutor`의 기존 owner-thread handle 계약도 변경하지 않는다.

`Core/Source/Runtime/Task/Tests/WorkerSchedulerTests.cpp`의 초기 10개 사례는 8개 실제 worker 동시 실행, 8개 외부 producer의 제출 경합, fan-in과 실패 전파, pending·running 취소, 큐 제한·OOM 시 Function 보존, Start의 세 할당 지점 실패, 실제 두 thread 시작 이후 세 번째 시작 실패, capture 정리 중 재진입, 이전 세대 dependency 거부, 100회 재시작, scheduler 밖의 handle 수명, 제출 예약과 Shutdown 경합을 포함한다. 이 기록은 구현 및 사례 정의이며, 실제 실행 판정은 통합 빌드 보고서에서 별도로 확정한다.

외부 이벤트 시험 6개: 다른 스레드의 Trigger가 의존 작업을 풀고 재완료는 false, Fail·취소 요청·Trigger 없는 소멸·회수가 의존 작업을 DependencyFailed로 만듦, Drain이 아무도 완료하지 않는 이벤트를 취소·이벤트의 maxInFlight 사용·시작 전·종료 뒤 생성 거부·이전 세대 dependency 거부, AsyncIo 완료 규칙·즉시 제출 실패·문맥 할당 실패, Drain이 실행 중인 작업의 Trigger를 기다림·Cancel은 바로 취소, Pending 이벤트 위 이동 대입·이벤트 생성 OOM. 압축 쪽 통합 시험(`Compression_AsyncDecompressAfterAsyncFileRead`)은 실제 파일을 AsyncReadFile로 읽고 그 완료에 의존한 해제가 원본을 돌려주는지(40회 반복하며 객체를 바로 소멸), 짧은 읽기에서 해제가 실행되지 않는지 확인한다.

`Core/Source/Runtime/Task/Tests/WorkerSchedulerReentryReviewTests.cpp`의 추가 2개 사례는 서로 다른 scheduler의 중첩 Submit에서 바깥·안쪽 Shutdown 거부, 무관한 scheduler 종료 허용, 내부 record OOM 시 caller Function 보존과 실행 생략, 예약 계수·TLS 프레임 복원 및 최종 allocator ledger 0을 검사한다. 이 사례의 실제 실행 결과도 해당 통합 빌드 증거를 따른다.

## 작업 lane 표지(TaskLane)

`Task/TaskLane.h`의 `TaskLane`(None·Game·Worker·Render·Physics·Io·Audio·Gc, 계획 8.1절 lane 표)은 "지금 실행 중인 일이 어느 lane의 일인가"를 스레드가 아니라 실행 구간에 붙인다. `CurrentTaskLane()`은 이 스레드의 가장 안쪽 `TaskLaneScope` 값이고 범위 밖이면 None이다. 실행기가 작업 실행 구간에 scope를 둔다(DedicatedLane은 설정한 lane). 진단·계약 검사용이며 동기화나 접근 권한을 대신하지 않는다. 특정 lane의 "그 실행 흐름 안"인지 확인해야 하면 `DedicatedLane::IsExecutingTask`를 쓴다.

## 전용 lane 실행기(DedicatedLane)

`Task/DedicatedLane.h`는 직렬 FIFO 큐 하나를 실행 흐름 하나가 차례로 소비하는 실행기다(RenderBase CP1, UE 렌더 스레드 `RenderCore/Private/RenderingThread.cpp`·named thread 대응). Render·Audio·Physics처럼 전용 스레드가 필요한 lane이 쓴다.

| 기능 | 인터페이스와 계약 |
|---|---|
| 시작 | `Start(DedicatedLaneConfig{mode, lane, thread, launch, launchContext})`. 생성 스레드 전용. 실패하면 mode·lane을 포함해 아무것도 바꾸지 않는다. `launch`(`DedicatedLaneLauncher`)는 스레드 생성을 대신하거나 실패를 주입하는 시험·호스트 경계이며, 실패를 돌려주면서 스레드를 시작한 launcher(계약 위반)도 그 스레드를 빈 큐로 끝내고 Join한 뒤 실패를 돌려준다. Thread 모드는 lane 스레드가 진입해 이름·우선순위 적용을 마쳤음을 `started_` 이벤트로 알린 뒤 반환한다(시작 handshake). 잘못된 mode·스레드 옵션은 InvalidArgument, 스레드 생성 실패는 OutOfMemory, 실행 중 재시작은 Busy |
| 제출 | `Post(Function<void()>&&)`. 어느 스레드에서나. 큐 칸은 잠금 밖에서 할당하고, 수락이 확정된 뒤에만 task를 옮기므로 거부·할당 실패 때 호출자의 task가 그대로 남는다. 시작 전·종료 뒤 ShuttingDown |
| 대기 | `Flush()`. 호출 전에 수락한 작업이 모두 끝날 때까지 기다린다. 수락 수와 완료 수를 비교한다(실행 흐름이 하나라 완료가 수락 순서대로 늘어난다) |
| 종료 | `Shutdown()`. 생성 스레드 전용. 일반 Post를 닫고 남은 작업과 종료 중 lane 작업이 넣은 후속을 모두 실행한 뒤(Drain) 닫는다. Thread 모드는 lane 스레드가 큐가 빈 것과 종료 요청을 함께 본 뒤 `stopped_` 이벤트를 켜고(종료 handshake) Join한다. 재시작 가능 |
| 실행 중 판별 | `IsExecutingTask()`. 스레드 ID가 아니라 thread-local 실행 칸 사슬의 가장 안쪽 칸으로 판별한다 |

**Inline 모드(trampoline).** 스레드 없이 Post한 스레드가 큐를 소비하되 큐는 그대로 쓴다. Post는 늘 큐 맨 뒤에 넣고, 그 lane을 비우는 실행 흐름이 없으면 그 Post가 큐가 빌 때까지 실행한다. 비었음을 본 것과 비우는 표지를 내리는 것을 같은 잠금 안에서 하므로, 그 사이 다른 스레드가 넣은 작업은 비우던 흐름이 실행하거나 그 Post가 스스로 비운다. lane 작업 안의 재진입 Post는 중첩 실행되지 않고 큐 뒤에 붙으므로 실행 순서가 Thread 모드와 같다.

**실행 칸 사슬.** 작업 본문과 capture 소멸 구간마다 스택에 칸을 쌓는다. Inline lane A의 작업이 Inline lane B에 Post해 B를 그 자리에서 비우면 B 칸이 A 칸 위에 쌓인다. `IsExecutingTask`는 가장 안쪽 칸만 보므로 B 작업 안에서 A는 false다(Thread 모드에서 B 작업이 B 스레드에서 도는 것과 같다). `Flush`·`Shutdown`은 사슬 전체를 보고 자기 lane 위에서 부르면 WrongThread다(자기 자신을 기다리는 교착). 그래서 Inline 모드에서도 lane 작업 밖(같은 스레드의 일반 코드)의 lane 전용 API 호출이 스레드 ID 검사와 달리 드러난다.

**수명.** 큐 칸 allocator는 여러 스레드의 동시 Allocate·Deallocate를 지원하고 lane보다 오래 살아야 한다. 실행 중 파괴는 Fatal이다. 조건 변수 알림은 모두 잠금 안에서 한다. 그래서 `Shutdown`이 반환하면 lane 코드를 실행 중인 스레드가 없다(Inline 모드에서 큐를 비우던 다른 스레드도 `draining_`을 내리고 알린 잠금 구간 뒤로는 lane을 건드리지 않는다). 다만 파괴 전에 다른 스레드의 `Post`·`Flush`·`Stats`·`IsExecutingTask` 호출도 모두 반환해야 한다(거부되는 Post도 큐 칸을 돌려주려고 lane을 쓴다). 시작·종료 대기에 시간 제한은 없다(끝나지 않는 작업을 강제 종료하지 않는다).

시험(`Tests/DedicatedLaneTests.cpp`, 11개): gate로 막은 lane에 네 생산자가 동시에 넣은 작업의 생산자별 순서, 재진입 Post 순서가 두 모드에서 같음(바깥 작업이 A·B를 넣고 A가 C를, C가 D를 넣는 시나리오), Inline 가장 바깥 Post가 반환 전에 실행, 두 모드의 종료 Drain(남은 작업 0·종료 중 후속 수락·종료 뒤 거부와 task 보존), Flush, lane 작업 밖 lane 전용 API 거부와 Inline 중첩 실행의 판별·자기 대기 거부·lane 표지, 시작 계약(시작 전 Post 거부, 잘못된 옵션, 재시작, 스레드 이름 적용, 큐 칸 할당 실패 때 task 보존), 다른 스레드의 Start·Shutdown 거부, Thread 모드 종료 중 lane 작업 후속 수락과 외부 Post 거부(수락이 닫힌 것을 `Stats().accepting`으로 확인한 뒤 gate를 연다), Inline 모드에서 다른 스레드가 비우는 중의 Post를 그 스레드가 실행함과 Inline Shutdown 직후 lane 파괴(비우던 스레드의 해제 뒤 사용 회귀), launcher 실패·계약 위반 launcher 뒤 상태 불변. 순서는 모두 이벤트 gate나 명시 상태 조건으로 만들고 sleep에 기대지 않는다.

## 순서 전용 선행과 lane 옵션(WorkerSubmitOptions)

`Submit(body, dependencies, WorkerSubmitOptions{orderDependencies, lane})`(RenderBase CP2). `orderDependencies`는 끝난 뒤(terminal)에만 실행하되 결과를 보지 않는 선행이다. 실패·취소·회수된 순서 전용 선행은 `DependencyFailed`를 만들지 않는다(일반 선행은 그대로 전파한다). worker는 순서 전용 선행이 모두 terminal이 되기 전에는 그 작업을 실행·취소·`DependencyFailed` 어느 경로로도 고르지 않고, `TryRevoke`도 false다. 그래야 순서 전용 선행과 이 작업의 body·capture 정리가 겹치지 않고, 이 작업의 terminal이 선행 전체의 terminal을 뜻한다(교차검증에서 취소·선행 실패·회수 경로가 순서를 건너뛰던 결함을 고쳤다). Drain 정체 판정은 바뀌지 않는다. 순서 대기 사슬은 DAG라 끝이 실행 중 작업·고를 수 있는 작업·취소할 외부 이벤트 가운데 하나에 닿기 때문이다. 검사는 두 목록을 이어 한 번에 한다(같은 scheduler·같은 세대·두 목록 사이 중복 금지, 합이 `maxDependencies` 이하). 기록에는 dependency 칸별 bit mask로 남기고 worker의 ready 판정만 그 bit를 본다. `lane`은 body와 capture 정리 구간의 `TaskLaneScope` 값이며 기본은 Worker다(`TryRevoke`로 호출 스레드가 capture를 정리할 때는 표지를 바꾸지 않는다).

## 직렬 작업 체인(SerialTaskChain)

`Task/SerialTaskChain.h`는 넣은 순서대로 하나씩 실행하는 worker 위의 체인이다(UE `Core/Public/Tasks/Pipe.h` FPipe, 렌더 그래프 설정 task 대응). 새 작업은 직전에 수락한 작업을 순서 전용 선행으로 건다. 그래서 체인 안의 두 작업은 겹치지 않고, 앞 작업이 실패·취소·회수되어도 다음 작업은 실행한다(체인은 순서만 정하고 결과를 잇지 않는다). 사용자가 준 일반 선행이 실패하면 그 작업만 `DependencyFailed`다. 여러 스레드의 `Submit`은 체인 잠금 순서가 수락 순서이며, 잠금을 쥔 채 scheduler `Submit`을 부르므로 scheduler allocator 콜백에서 같은 체인을 부르면 교착이다. 이미 끝난 직전 작업은 선행에서 빼고 놓는다(결과 record를 오래 잡지 않고, scheduler 재시작 뒤 이전 세대 handle로 `WrongContext`가 나지 않게). `Last()`는 마지막 handle 사본, `Wait(timeout)`은 그 handle 대기다(worker에서 부르면 WrongThread). 체인 객체는 작업이 참조하는 상태가 없어 언제 파괴해도 수락한 작업은 계속 실행한다.

## 범위 병렬 실행(ParallelFor)

`Task/ParallelFor.h`의 `ParallelFor(scheduler, scratch, count, body(begin, end), ParallelForOptions{minBatchSize, maxHelpers})`는 [0, count)를 연속 batch로 나눠 호출 스레드와 helper 작업이 함께 실행한다(UE `Core/Public/Async/ParallelFor.h` 대응). body는 void 또는 `ErrorCode`를 돌려주며 복사하지 않고 참조로 여러 스레드에서 부른다. 템플릿은 body를 함수 포인터 하나로 감싸 비템플릿 `detail::RunParallelFor`에 넘긴다(본문 할당 없음).

- 분배: batch 크기는 max(`minBatchSize`, ⌈count / (참여자 × 4)⌉)이고, helper 수는 worker 수·`maxHelpers`·(batch 수 − 1)·64 이하다. batch는 원자 카운터로 번호 순서대로 가져간다.
- 오류: body가 오류를 돌려주면 새 batch를 가져가지 않고, 실패한 batch 가운데 가장 앞 batch의 오류를 돌려준다. batch를 번호 순서로만 가져가므로 실패한 batch보다 앞선 batch는 모두 실행되고, 결과는 순차 실행에서 처음 만나는 실패와 같다(병렬 압축의 job 분배와 같은 규칙).
- 정리: 호출 스레드가 일을 마치면 아직 꺼내지 않은 helper는 `TryRevoke`로 회수하고 실행 중인 helper만 기다린다. 반환 시 body와 공유 상태를 참조하는 helper가 모두 terminal이다.
- 문맥: helper는 호출 스레드의 lane 표지를 물려받는다. 렌더 lane이 부른 ParallelFor의 batch는 worker에서도 Render 문맥이다(UE parallel rendering thread 판별 대응). None과 Game은 물려주지 않고 helper를 Worker로 둔다(Game lane 전용 검사가 worker에서 통과하면 안 된다). 이때 호출 스레드가 실행한 batch는 호출자 표지 그대로라 batch마다 표지가 다를 수 있다.
- body 반환형은 void 또는 `ErrorCode`이고 그 밖은 static_assert로 거부한다.
- 혼자 실행: 같은 scheduler의 worker 안(helper를 기다릴 수 없다), scheduler 시작 전·종료 중, helper 본문 할당·제출 실패, `maxHelpers` 0이면 그만큼 helper 없이 호출 스레드가 남은 batch를 모두 실행하고 결과는 같다.

병렬 압축(`Compression/Private/CompressedBlocks.cpp`의 `RunParticipants`)은 참여자별 codec 상태를 두는 같은 분배 방식을 이미 쓴다. ParallelFor에 참여자별 문맥이 생기면 그쪽을 옮긴다(후속).

시험: `Tests/SerialTaskChainTests.cpp` 6개(4 worker에서 200개 작업의 순서·비중첩·Render 표지, 네 생산자의 생산자별 순서, 머리 작업을 gate로 막은 채 가운데 작업을 취소·회수 시도·선행 실패시켜도 gate 전에는 모두 Pending·`TryRevoke` false·체인 `Wait` 미완료이고 gate 뒤 capture 정리까지 머리 뒤에 일어남, `Shutdown(Cancel)`이 순서 대기 작업을 앞 작업 뒤에 정리함, 시작 전 거부와 body 보존·scheduler 재시작 3회, 순서 전용 선행·lane 옵션 검사와 거부), `Tests/ParallelForTests.cpp` 5개(10007개 원소를 한 번씩·Render 표지 상속·count 0, 처음 실패 batch의 결정성 20회와 앞 원소 실행, 시작 전·`maxHelpers` 0·worker 안 호출·helper 할당 실패 때 호출 스레드 실행, count < `minBatchSize`·count 1·Game 표지 비상속, batch를 쥔 helper가 막힌 채 `Shutdown(Cancel)`해도 범위 전체를 한 번씩 처리).
