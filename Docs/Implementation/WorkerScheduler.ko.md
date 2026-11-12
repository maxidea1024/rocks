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

RequestCancel은 요청 게시이며 실행 중 body를 강제로 중단하지 않는다. pending 작업은 worker가 정리할 때 body를 생략한다. 실행 중 작업은 취소를 폴링하여 반환해야 하며 이미 발생한 부수 효과를 되돌리지 않는다. 명시적 body 실패와 dependency 실패가 있으면 Failed가 우선하고, 그 외 취소 요청을 받은 작업은 body 반환·cleanup 이후 Cancelled가 된다.

Drain은 신규 제출을 닫고 수락한 작업을 완료한다. Cancel은 pending과 running 작업에 취소를 요청한 뒤 같은 정리·합류 절차를 수행한다. 무한 실행 body나 사용자 cleanup을 강제로 종료하지 않으며 제한시간 내 Shutdown 완료를 보장하지 않는다. Submit/lifecycle allocator callback에서 재진입한 Shutdown은 `Busy`이며, worker/capture cleanup에서는 `WrongThread`이다. Submit은 할당 없는 TLS 연결 프레임 스택으로 기록한다. A.Submit의 allocator가 B.Submit을 호출하고 그 allocator가 A.Shutdown을 호출하는 경우에도 바깥 A의 활성 제출을 검색하여 Busy로 거부한다. 최상단 B만 검사하면 A의 예약 반환과 worker Join이 서로 기다리는 교착이 발생한다. 제출 중이 아닌 별도 scheduler의 정상 Shutdown은 허용하며, 정상·OOM 복귀 모두 프레임을 복원한다.

선택적인 `WorkerThreadLauncher`는 공용 Thread 생성의 실패 주입과 호스트 정책을 위한 경계이다. Success는 제공된 Thread에서 지정 entry/context로 실제 Start를 한 경우에만 반환한다. 실패 시 새 thread를 만들지 않는 계약을 따른다. 부분 시작 실패는 이미 시작한 worker를 종료·Join한 뒤 배열을 해제한다. 성공한 Shutdown 후 Start를 반복할 수 있다. 첫 Start 준비 중 생성한 작은 control storage는 실패하더라도 scheduler 파괴까지 유지될 수 있다.

## 구현 및 검증 범위

현재 구현은 mutex와 선형 ready 탐색을 이용한다. 우선순위, work stealing, lock-free 큐, job affinity, fiber 대기, 성능 우위는 이 단계의 제공 범위가 아니다. `InlineExecutor`의 기존 owner-thread handle 계약도 변경하지 않는다.

`Core/Source/Runtime/Task/Tests/WorkerSchedulerTests.cpp`의 초기 10개 사례는 8개 실제 worker 동시 실행, 8개 외부 producer의 제출 경합, fan-in과 실패 전파, pending·running 취소, 큐 제한·OOM 시 Function 보존, Start의 세 할당 지점 실패, 실제 두 thread 시작 이후 세 번째 시작 실패, capture 정리 중 재진입, 이전 세대 dependency 거부, 100회 재시작, scheduler 밖의 handle 수명, 제출 예약과 Shutdown 경합을 포함한다. 이 기록은 구현 및 사례 정의이며, 실제 실행 판정은 통합 빌드 보고서에서 별도로 확정한다.

`Core/Source/Runtime/Task/Tests/WorkerSchedulerReentryReviewTests.cpp`의 추가 2개 사례는 서로 다른 scheduler의 중첩 Submit에서 바깥·안쪽 Shutdown 거부, 무관한 scheduler 종료 허용, 내부 record OOM 시 caller Function 보존과 실행 생략, 예약 계수·TLS 프레임 복원 및 최종 allocator ledger 0을 검사한다. 이 사례의 실제 실행 결과도 해당 통합 빌드 증거를 따른다.
