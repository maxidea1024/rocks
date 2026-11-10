# Implementation Round 4 — Platform 동시성 교차 검토

2026-11-10 기준 공개 `Thread.h`/`Sync.h`, Common의 Thread/Sync/WaitDeadline, Win64·Mac backend와 Platform 테스트를 독립 검토했다. 수명·동시 제어·deadline·native 오류의 실제 분기를 대조했으며 수정이 필요한 확정 runtime 결함은 발견하지 못했다. runtime은 변경하지 않았고 요청받은 동시 제어 fixture만 별도 파일에 추가했다.

## 반례별 확인

| 검토 경계 | 확인한 구현과 판정 |
|---|---|
| Start 직후 entry 실행 | state gate를 잡은 채 native thread를 만들고 join 소유권을 게시한다. 새 thread는 같은 gate를 지난 뒤 entry/context를 읽으므로 게시 전에 사용자 함수가 실행되지 않는다. 생성 실패는 새 join 소유권을 만들지 않는다. |
| 동시 Join/Start/Wait | Join은 gate 안에서 joining을 예약하고 native 대기는 gate 밖에서 수행한다. 다른 Join은 Busy, Start는 AlreadyJoinable이다. Wait는 별도 계수를 유지하고 성공 Join 뒤에도 이전 waiter가 남아 있으면 Start가 Busy로 재시작을 막는다. |
| entry 완료와 TLS 종료 | completion은 entry 반환만 의미한다. Windows는 `_beginthreadex` 반환·native handle 대기, Mac은 pthread_join으로 TLS/native 종료까지 회수한다. self 판별은 재사용 가능한 OS id 대신 실행 중 ThreadState의 thread-local identity를 사용한다. |
| timed wait와 spurious wake | 이벤트·세마포어·Thread 완료 대기는 predicate를 반복 검사한다. WaitDeadline은 최초 monotonic 시작 시각의 잔량을 계산하므로 wake마다 timeout을 연장하지 않는다. 양의 sub-ms 잔량은 올림하여 0으로 조기 절삭하지 않는다. |
| CV의 lock 반환 | Win64는 SRW exclusive lock, Mac은 해당 pthread mutex를 native CV에 넘긴다. 두 backend 모두 반환 뒤 다시 잡힌 mutex에서 상위 predicate를 확인한다. Mac 유한 대기는 relative API를 사용한다. |
| semaphore/event 상태 | 세마포어는 남은 maximum-count와 비교해 Release 전체를 승인하거나 거부한다. Wait 성공만 permit을 소비한다. ManualResetEvent는 Signal/Reset 사이의 상태를 gate로 보호하며 특정 waiter 공정성을 주장하지 않는다. |
| native 실패 | thread 생성 실패·native Join 실패는 SystemError로 전달하고 joinable은 성공 Join에서만 해제한다. mutex/CV 초기화·파괴의 비복구 오류는 Fatal, CV 대기 오류는 Failed다. native 호출의 실패를 정상 완료로 바꾸지 않는다. |

Windows CV의 spurious/stolen wake 및 lock 재획득 계약은 [Microsoft 공식 문서](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-sleepconditionvariablesrw)와 대조했다. Mac relative 진입점은 [Apple libpthread 공개 소스](https://github.com/apple-oss-distributions/libpthread/blob/main/src/pthread_cond.c)의 별도 relative 경로와 대조했다. 이는 Mac native 실행 성공을 대신하는 근거가 아니다.

## opaque storage와 C++ 수명

ThreadState는 공개 storage에 placement 생성하고 소멸한다. native thread handle도 backend가 placement 생성하며 크기·정렬 전제는 static_assert로 확인한다. Mutex/CV의 native C 구조체는 unsigned-char storage 안에서 초기화한다. char 배열을 썼다는 이유만으로 이를 확정 UB로 분류하지 않았다. implicit-lifetime object와 unsigned-char 배열의 storage 제공 규칙을 [C++ 객체 모델 초안](https://eel.is/c++draft/intro.object)과 [WG21 P0593R6](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2020/p0593r6.html)에서 확인했다. 후속 backend가 비trivial native C++ 객체를 넣는다면 같은 추론을 그대로 적용하지 말고 명시 생성·소멸을 추가해야 한다.

## 독립 fixture 보완

`Tests/Platform/ThreadControlTests.cpp`에 다음 2개 시험을 추가했다. OS 전처리·Sleep·비공개 runtime hook은 사용하지 않는다.

- `PlatformConcurrentJoinersTransferNativeOwnershipExactlyOnce`: ready semaphore와 launch/release event로 2개 Join 요청을 시작한다. 정확히 하나가 Success이고 다른 요청은 예약 경합의 Busy 또는 이미 회수된 NotJoinable이어야 한다.
- `PlatformConcurrentCompletionReadersAndJoinLeaveNoPreviousRunOwnership`: Join 1개와 완료 reader 5개가 경합한다. Join 전 admission은 Signaled, Join 완료 후 진입은 Failed를 허용한다. 모든 제어 호출이 끝난 뒤 join 소유권이 없어야 하고 다음 실행의 Start/Join도 성공해야 한다.

fixture는 시작 실패나 REQUIRE 조기 반환에도 release → controller Join → target Join 순서로 정리한다. 결과를 읽기 전에 controller를 모두 회수하며 테스트용 context를 살아 있는 thread보다 먼저 파괴하지 않는다. ready barrier는 요청 시작을 맞추지만 특정 OS 스케줄 또는 native reservation의 정확한 중간 시점을 강제했다고 주장하지 않는다.

기존 Platform tests는 timeout 후 소유권 유지, self Join, 재시작, TLS destructor 대기 구간, CV predicate, event sticky/reset, 세마포어 한도와 500개 순서 있는 생산/소비를 검사한다. Name/Object 공용 테스트 또한 Platform API로 이전했으며 후보 도착 predicate와 해제 event, thread 수명·계수 검증을 유지한다.

## 검증 한계와 완료 조건

- Start의 이전 waiter 잔존 Busy 분기는 public hook 없이 특정 순서를 강제하기 어려워 소스 검토와 모든 reader 종료 후 재시작 시험을 구분한다.
- OS native 생성·Join 실패를 강제로 주입하는 fixture는 없다. 오류 분기 검토를 실제 실패 주입 실행으로 표기하지 않는다.
- TLS/native Join에는 유한 timeout 보장이 없다. WaitForCompletion의 10초 테스트 상한을 native TLS 종료 상한으로 확대하지 않는다.
- 이 보고서 작성 시 최종 source freeze 이전이며 새 2개 fixture의 실제 실행 결과는 최종 build evidence로 확인해야 한다. macOS native·TSan·모든 스케줄 탐색은 아직 이 검토의 실행 증거가 아니다.
- 경고를 오류로 취급하는 기준을 유지한다. 검토를 통과시키기 위해 진단을 숨기거나 경고 비활성 옵션을 추가하지 않았다. 최신 CMake 경고 설정과 음성 대조군의 실제 성공은 build 담당의 최종 증거에 따른다.
