## 14. 시험 계획

### 14.1 원칙

- sleep으로 순서를 만들지 않는다. "다른 스레드가 이제 기다리고 있다"는 훅·이벤트로 안다.
- 단언은 결정적이다. 결과(정확히 한 번, 순서, 수)가 스케줄에 따라 달라지면 시험이 틀린 것이다.
- 스케줄을 넓게 훑는 stress는 기본 실행에서 뺀다. 기본 실행은 짧고 결정적이다.

### 14.2 결정적 순서: ParkingLot 시험 훅

- 훅은 `SyncHooks`의 시험 전용 `onParked(address)`·`onUnparked(address)`다. Debug·Development에만 컴파일하고 Release에는 없다. 설치는 시험 소스가 PlatformBase의 `Private/Common/Sync/SyncTestAccess.h`로 한다(정책 TestAccess 규칙, 모듈 코드에는 `ROCKS_TESTING`이 없다).
- 호출 지점은 10.4절이다. `onParked`는 enqueue와 `BeforeWait`가 끝난 뒤 잠들기 직전이다. 그래서 훅이 불린 시점에는 깨우는 쪽이 반드시 그 대기자를 큐에서 찾는다.
- 시험은 훅에서 시험용 gate를 연다. gate는 시험 대상(ParkingLot 계열)에 기대지 않도록 TestSupport가 `WaiterSignal` 위에 따로 만든 `TestGate`다. 예: "스레드 B가 `Mutex`를 기다리며 잠들었다"가 gate로 확인된 뒤에 A가 `Unlock`하고 B가 깨어 잡았는지 단언한다. UE `SharedMutexTest.cpp` 244행의 `Sleep(0.001f)`를 이 방식으로 대체한다.
- **시한 시험(가짜 OS 대기).** 시계만 바꾸면 OS 대기(`WaitOnAddress`의 ms, futex 절대 시한)는 실제 시간으로 잔다. 그래서 두 seam을 함께 둔다. 둘 다 `SyncTestAccess.h`로 설치한다.
  - 단조 시계 함수 포인터(`WaitDeadline`이 쓴다).
  - `WaiterSignal`의 시한 대기 함수 포인터. 가짜 대기는 OS를 부르지 않고 시험의 `TestGate`를 기다린다. 시험이 "시간이 지났다"를 정하면(가짜 시계를 deadline 뒤로 옮기고 gate를 연다) `TimedOut`으로 돌아간다. 깨우기가 먼저 오면 `Signaled`다. 무한 대기는 seam을 거치지 않는다.
  - 그래서 시한 시험은 실제 시간을 쓰지 않고, "시한 전 깨움"과 "시한 뒤 시간 초과"를 각각 정해진 순서로 만든다.
  - **구현(개정 4).** seam은 `Private/Common/Sync/SyncSeams.h`(설치 함수)와 `SyncTestAccess.h`(시험 통로)다. 가짜 시한 대기는 받은 대상 `WaiterSignal`을 무한으로 기다린다. 시험이 "시간이 지났다"를 정하면 표시를 세운 뒤 그 신호를 `Notify`하고, 깬 가짜 대기는 표시를 보고 `Reset`한 뒤 시간 초과를 돌려준다. 신호를 바꾸지 않고 깨우는 별도 OS 경로를 두면 깨우기가 대기보다 먼저 올 때 사라지므로(주소 대기는 값이 같으면 잔다) 두지 않았다. `WaitDeadline`은 `SyncMonotonicNanoseconds()`(가짜 시계가 있으면 그것)를 쓴다. `InfiniteTimeout`·`WaitResult`는 SY0에서 `Platform/Sync/WaitTypes.h`로 옮겼다(7.4절, `Sync.h`가 include한다).
- 시험 목록(SY1–SY4):
  - ParkingLot: 콜백 있는 `WakeOne`·`WakeMultiple`·`WakeAll`의 `mayHaveMoreWaiters` 값, 표 키우기 중 대기자 보존, **TLS 파괴 뒤 대기**(스레드 종료 중 다른 `thread_local` 소멸자에서 경합 대기 → 임시 레코드로 잠들고 깨어 반환됨, 그 뒤 영구 풀의 레코드 수가 원래대로임, D31).
  - Mutex: 잠든 대기자 깨움, `TryLockFor` 시한(가짜 OS 대기), `InitiallyLocked`(다른 스레드의 첫 해제가 보유 스택 검사에 걸리지 않음).
  - Mutex 대기 bit 해제 시점(UE `IM` 199–204행과 같다): 대기자 하나가 잠든 상태에서 `Unlock` → 대기자가 깨어 잡은 뒤에도 대기 bit는 **남아 있음**을 단언하고, 그 대기자의 `Unlock`이 빈 큐를 보고 나서야 bit가 **지워짐**을 단언한다.
  - ReadWriteMutex: 쓰기 대기 중 새 읽기가 기다림(쓰기 우선), 마지막 읽기 해제가 쓰기를 깨움, 쓰기 해제가 읽기 전부를 깨움, 읽기 재진입이 쓰기 대기자 없이도 Debug Fatal(자식 프로세스, 14.6절).
  - RecursiveMutex: 횟수, 다른 스레드 대기 후 마지막 해제에서 깨움.
  - ConditionVariable: `BeforeWait`에서 푼 직후의 `NotifyOne`이 사라지지 않음(훅으로 그 지점에 알림을 끼운다).
  - **대기 표시 경쟁(10.3절, D29).** 타입마다(ConditionVariable·AutoResetEvent·Semaphore·OnceFlag·ManualResetEvent·Mutex) 같은 대본을 돈다. W1이 잠든다(`onParked`로 확인). W2가 대기 표시를 세운 뒤 bucket에 들어가기 직전에 단계 훅으로 멈춘다. 그 사이 다른 스레드가 W1을 위한 `Notify`·`Signal`·`Release`·`Unlock`을 끝낸다. W2를 진행시켜 잠들게 한 뒤(`onParked`), W2를 위한 알림 하나가 W2를 깨우는지 단언한다. ConditionVariable은 알림을 `mutex` 밖에서 부르는 판(`WorkerScheduler` 모양)도 돈다. 개정 1 설계는 이 시험에서 W2가 깨지 않는다.
  - EventCount: PrepareWait와 Wait 사이에 Notify를 끼우면 Wait가 바로 돌아옴. Notify가 대기자 없을 때 비싼 경로를 타지 않음(카운터 값으로 확인).
  - AutoResetEvent: 대기자 둘에 Signal 하나 → 정확히 하나 통과(나머지는 `onUnparked`가 불리지 않았음으로 확인).
  - Semaphore: 최대값 범위(2^31 − 1 초과 생성은 Fatal), `Release(n)`이 n명까지 깨움.
  - Once: `CallOnce` 실패 기록(이후 호출이 실행하지 않고 false), `CallOnceUntilSuccess` 실패 뒤 대기자 하나가 다시 실행해 성공, 실행 중 대기자 깨움, 실행 중 재진입 Debug Fatal.
  - ReclaimDomain(SY9): 대기자 없을 때 독자 이탈이 `FetchSub` 하나뿐임(ParkingLot 호출 수 0), `Synchronize`가 옛 세대 독자 이탈의 `WakeAll`로 깨어남.

### 14.3 interleaving 전수 시험: 단계 스케줄러

- 원자 연산 사이에 단계 지점을 둔다. Rocks에는 "시험 빌드"가 따로 없다. 모듈 코드는 늘 한 번 빌드되어 shipping과 시험이 함께 쓰고, `ROCKS_TESTING`은 시험 실행 파일 소스에만 정의된다(`Programs/Rocks.Tooling.Checks/PolicyChecks.TestAccess.cs` 95–101행 `testing-macro`, `Programs/FoundationTests/FoundationTests.Build.cs` 87행). 그래서 매크로를 빌드 종류로 바꾸지 않고 두 방식으로 나눈다(D24).
  - **Core 동시 컨테이너(헤더 템플릿).** 단계 지점은 템플릿 인자 `StepPolicy`의 `Step("name")` 호출이다(12.1절). 제품은 기본 `NoSteps`(빈 inline)를 쓰고 비용이 0이다. 시험은 `TestSteps` 정책 타입으로 다른 instantiation을 만든다. 정의가 TU마다 달라지지 않으므로 ODR 위반이 없다.
  - **ParkingLot·Mutex 등(.cpp 구현).** 단계 지점은 `ROCKS_SYNC_STEP("name")` 매크로다. Debug·Development에서는 스레드 지역 함수 포인터 하나를 relaxed로 읽어 null이 아니면 부르고, Release에서는 빈 매크로다. 함수 포인터는 시험이 `SyncTestAccess.h`로 설치한다. Development 비용은 단계 지점마다 TLS 읽기 하나와 분기 하나다. 단계 지점은 slow path(spin 뒤·대기 경로·깨우기 경로)에만 두고, 경합 없는 빠른 경로(CAS 하나·`fetch_sub` 하나)에는 두지 않는다. 그래서 무경합 잠금의 Development 비용은 늘지 않는다. Android API 26–28처럼 `thread_local`이 emutls 함수 호출이 되는 대상에서는 slow path마다 함수 호출 하나가 더 든다.
- 시험 지원의 `StepScheduler`가 스레드 2–3개를 돌린다. 한 번에 한 스레드만 다음 단계 지점까지 가게 하고, 순서는 (a) 시험이 적은 대본, 또는 (b) 작은 시나리오(예: 생산자 둘 × 연산 둘 + 소비자 하나)의 모든 순서를 깊이 우선으로 전수한다. CHESS·loom과 같은 생각이다.
- 단계 지점 사이의 코드는 원자 연산이 하나 이하가 되게 둔다. 그래야 "모든 순서"가 실제 가능한 순서를 덮는다(순차 일관 모델 기준. 약한 메모리 순서의 재배치는 이 시험이 잡지 못한다 → 14.5절).
- 대상: `IndexedFreeList`(ABA 시나리오 포함: 태그를 일부러 작게 만든 시험판으로 감김 재현. 태그 폭 static_assert를 피하려고 시험 전용 정책 인자로만 연다), `TakeAllStack`, `IntrusiveMpscQueue`, `BoundedSpscQueue`, `BoundedMpscQueue`(기존, `StepPolicy` 인자를 더한다), `EventCount`, `ReclaimDomain`, 그리고 10.3절 대기 표시 경쟁(14.2절).
- 외부 모델 검사기(Relacy·CDSChecker)는 쓰지 않는다(C++ 외부 라이브러리 금지 결정). 이 하네스가 그 역할의 일부를 한다(D24).

### 14.4 stress 시험

- `Test.h`에 `STRESS_TEST(name)`을 더해 별도 목록에 등록한다. `FoundationTests`(`Programs/FoundationTests/Private/Main.cpp` 157–175행의 `--filter`·`--seed`·`--repeat`)에 `--stress`를 더해 그 목록만 돈다. 기본 실행은 돌지 않는다.
- 위치는 각 모듈 `Tests/` 안 `*StressTests.cpp`다.
- 단언은 결정적이다(정확히 한 번, 생산자별 순서, 최종 수). 스케줄만 운에 맡긴다. 반복 수는 `--repeat`로 늘린다.
- 기존 `BoundedMpscQueueStressExactlyOnceInProducerOrder`(4×20000건)처럼 수십 ms 이하로 끝나는 짧은 경합 시험은 기본 실행에 남긴다. 기준: 기본 실행에 남는 경합 시험은 1개당 100 ms 이하, 결과 단언이 결정적인 것.
- ARM64 실기(macOS Apple silicon, Android)에서 stress를 돌린다. x86-64는 TSO라 약한 순서 버그를 드러내지 못한다.

### 14.5 ThreadSanitizer류 도구

- **Clang ThreadSanitizer는 Windows를 지원하지 않는다.** 실측(2026-11-18, clang-cl 21.1.6): `-fsanitize=thread`가 `unsupported option '-fsanitize=thread' for target 'x86_64-pc-windows-msvc'`로 거부된다. macOS(arm64·x86-64)에서 쓸 수 있다. Android NDK는 공식 지원으로 확인하지 못했다(17절).
- 권장: macOS 전용 빌드 구성 `Tsan`을 둔다. `NativeConfiguration`에 값을 더하고 입력 검사에서 macOS 밖을 거부한다(지금 Asan의 Android 거부와 같은 자리, `InputValidation.cs` 52–54행). 기본 검증(Debug·format·policy)에는 넣지 않고 동기화 단계 완료 때와 lock-free 변경 때 수동으로 돈다(Asan을 기본에서 뺀 방침과 같다).
- TSan은 `__atomic_*` builtin을 이해하므로 원자로 짠 잠금·ParkingLot의 happens-before를 추적한다. 보고 품질을 위해 `Mutex` 계열에 `__tsan_mutex_create/pre_lock/post_lock/pre_unlock/post_unlock` 주석을 `ROCKS_THREAD_SANITIZER`일 때만 붙인다.
- **한계: 단독 fence.** TSan은 `atomic_thread_fence`만으로 만든 순서를 모델하지 못해 오탐을 낸다. 그래서 StoreLoad 순서는 RMW로 만든다는 규칙을 둔다(5·12.5절). SY6에서 `LogSystem`의 fence 기반 Dekker(411·440행)가 `EventCount`(RMW)로 바뀌면 이 오탐 원인도 사라진다.
- UE의 자체 race detector(3.12절)에 해당하는 것은 만들지 않는다.
- 13.1절의 정적 분석이 모든 플랫폼에서 도는 1차 방어선이다.

### 14.6 부정 대조와 성능

- Fatal 경로(자기 교착, 읽기 재진입, 남의 잠금 해제, 잠금 쥔 채 대기·task 대기, 순위 위반, 실행 중 CallOnce 재진입, Semaphore 최대값 범위)는 기존 자식 프로세스 harness로 메시지를 확인한다.
- 성능은 마이크로 벤치(무경합 잠금·풀기, N 스레드 경합, 읽기 비율별 ReadWriteMutex, SRWLOCK·`pthread_mutex_t` 대비)를 수동으로 돈다. 판정 gate로 쓰지 않는다.
