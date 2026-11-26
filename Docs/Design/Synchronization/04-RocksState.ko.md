## 4. Rocks 현황

### 4.1 PlatformBase `Sync.h`·`Atomics.h`와 플랫폼 경계

- **`Platform/Sync.h`(120행)** — namespace `rocks::platform`.
  - `InfiniteTimeout`(8행), `WaitResult { Signaled, TimedOut, Failed }`(10행).
  - `Mutex`(19행): 64 byte·16 정렬 저장소, 비재진입, `Lock`·`TryLock`·`Unlock`. Windows는 SRWLOCK 배타(`Private/Win64/Platform.cpp` 30–48행), POSIX는 `pthread_mutex_t`(`Private/Posix/Platform.cpp` 35–68행). 생성자가 constexpr가 아니고 파괴자가 있다.
  - `LockGuard`(47행): Mutex 전용 비템플릿 가드.
  - `ConditionVariable`(69행): 64 byte 저장소. Windows `CONDITION_VARIABLE`+`SleepConditionVariableSRW`(`Private/Win64/Concurrency.cpp` 11–32행), POSIX `pthread_cond_t`(`Private/Posix/Concurrency.cpp` 16–51행). Android는 `CLOCK_MONOTONIC` 조건 변수, Apple은 상대 시간 대기다(`Private/Android/AndroidPosix.cpp` 83–108행, `Private/Apple/ApplePosix.cpp` 79–89행).
  - `ManualResetEvent`(86행), `Semaphore`(105행): Mutex+CV+값의 공용 구현(`Private/Common/Sync.cpp` 1–77행). 유한 대기는 최초 monotonic deadline의 남은 시간만 쓴다(`Private/Common/WaitDeadline.h`).
- **`Platform/Atomics.h`(179행)** — `MemoryOrder`와 `AtomicLoad`·`AtomicStore`·`AtomicExchange`·`AtomicCompareExchangeWeak/Strong`·`AtomicFetchAdd/Sub/And/Or/Xor`·`AtomicThreadFence` 자유 함수다. 순서는 템플릿 인자이고 잘못된 조합은 static_assert로 막는다. 구현은 Clang `__atomic_*` builtin이다. 대상은 1·2·4·8 byte 정수·열거형·포인터이고 `__atomic_always_lock_free`를 요구한다. `std::atomic`은 쓰지 않는다.
  - 정책 검사 `compiler-builtin`이 `__atomic_*`·`__ATOMIC_*`를 이 헤더에만 허용한다(`Programs/Rocks.Tooling.Checks/PolicyChecks.Builtins.cs` 9–28행).
- **없는 것.** CPU pause(spin 힌트)가 없다. spin 자리에서는 OS 양보 `YieldThread()`(Windows `SwitchToThread`, `Private/Win64/Concurrency.cpp`)를 쓴다. 주소 대기(`WaitOnAddress`·futex)도 없다. `Atomic<T>` 클래스도 없다(`Base/TypeTraits.h` 566–577행의 `RemoveAtomic`은 Clang `_Atomic` 한정을 벗기는 형질일 뿐이다).
- **PlatformBase 자신이 잠금을 쓴다.** 공개 헤더 `Platform/File.h` 285행, `Platform/VirtualMemory.h` 70행이 `Mutex`를 멤버로 갖고, `Private/Common/ThreadInternal.h`·`AsyncFileInternal.h`, `Private/Mac/DirectoryWatch.cpp`, `Private/Android/Path.cpp`가 쓴다.
- 구현 문서는 RWLock·affinity·취소 토큰을 후속으로 적었다(`Docs/Implementation/PlatformConcurrency.ko.md` 55행, `Docs/Design/PlatformLayer.ko.md` 68·270행).

### 4.2 Task 모듈

- `WorkerScheduler`는 상태 하나(`WorkerState`)에 `Mutex gate`+`ConditionVariable changed`를 두고, 제출·완료·대기 상태는 모두 그 잠금 아래에서 바꾸고, 잠금을 푼 뒤 `NotifyAll`로 깨운다(223→227·317→322·584→592·701→724행)(`Task/Private/WorkerScheduler.cpp` 17–18·223–227·388–395·875행). 수명 제어는 `lifecycleGate_`(`Task/Public/Task/WorkerScheduler.h` 238행)다. 큐는 잠금 아래 원형 배열이다(534–553행).
- `DedicatedLane`은 `Mutex`+`ConditionVariable`+`ManualResetEvent` 두 개(시작·정지)다(`Task/Public/Task/DedicatedLane.h` 153–160행).
- `SerialTaskChain`은 `Mutex gate_`(`SerialTaskChain.h` 60행), `ParallelFor`는 실패 기록용 `Mutex`와 원자 카운터(`ParallelFor.cpp` 24행)다.
- lock-free 경로는 없다. 설계 원칙이 "초기 Task queue는 잠금 기반으로 정확성을 확보하고, work-stealing deque와 lock-free free list는 ABA·메모리 회수 검증 후 교체"다(`Docs/Design/EngineImplementationPlan.ko.md` 423행).

### 4.3 BoundedMpscQueue

- `Core/Public/Core/Containers/BoundedMpscQueue.h`(196행). Vyukov 유한 MPSC, 칸마다 64-bit 순번, 용량은 2의 거듭제곱, `Initialize`에서 한 번 할당하고 push·pop은 할당하지 않는다. 생산자는 lock-free, 소비자는 예약 후 미공개 칸에서 멈출 수 있다. T는 trivially copyable이다. 칸과 head·tail은 `alignas(CacheLineSize)`다(178–185행).
- 사용처는 로그 큐 하나다(`Core/Public/Core/Diagnostics/LogSystem.h` 294행). CVar 쓰기 큐 후보로도 적혀 있다(`CVarSystem.ko.md` 353행).
- 시험은 순서·가득·감김, 4생산자×20000건 stress, 멈춘 생산자를 이벤트로 재현하는 시험이다(`Core/Tests/BoundedMpscQueueTests.cpp` 64·92·141행).

### 4.4 잠금 사용 현황

`platform::Mutex`를 쓰는 비시험 파일은 Core 13, Task 5, RenderBase 4, ObjectRuntime·TraceCapture 각 3, VulkanGraphics·Graphics·Localization·Crypto 각 2, Compression·D3D11·D3D12 각 1이다. 읽기가 많은 표(형 등록 `ObjectRuntime/Public/Object/TypeRegistry.h` 241행, 이름 풀 `Core/Public/Core/Text/NamePool.h` 130행, 지역화 목록 `Localization/Public/Localization/Catalog.h` 85행)는 읽기쓰기 잠금 후보다. 바꿀지는 측정으로 정한다(D7).

### 4.5 중복·자체 구현 목록

공용 기능 원칙(범용 기능은 공용 모듈 하나에)에 걸리는 것이다. 각 항목의 대체 장치는 이 문서의 절이다.

| # | 패턴 | 위치 | 대체 |
|---|---|---|---|
| 1 | exchange + `YieldThread` spin 잠금 | `TraceCapture/Private/TraceThreadWrite.cpp` 28–44행(스레드 칸 목록), `Core/Private/Diagnostics/LogSystem.cpp` 327–332·460행(`pumping_`: TryLock + Stop 쪽 spin 대기) | constinit `Mutex`(8.1절) |
| 2 | 시한 TryLock(TryLock + 양보 반복) | `TraceCapture/Private/TraceSnapshot.cpp` 24–35행 | `Mutex::TryLockFor`(8.1절) |
| 3 | 태그 색인 Treiber free list | `Core/Private/Diagnostics/LogBlockPool.cpp` 72–126행, `TraceCapture/Private/TraceBuffers.cpp` 68–100행 | `IndexedFreeList`(12.2절) |
| 4 | 원자 상한·포화·조건부 증가·함수 갱신 CAS 반복 | 최댓값 `TraceBuffers.cpp` 102–110행, 한도 안 예약 `TraceBuffers.cpp` 189–200행, 포화 더하기 `Serialization/Private/Archive.cpp` 242–262행, 상한 있는 참조 증가 `PlatformBase/Private/Common/DynamicLibrary.cpp` 185–194행, 0이 아닐 때만 증가 `Core/Public/Core/Memory/SharedPtr.h` 52–62행, double 값 더하기(bit 표현을 CAS로 갱신) `TraceCapture/Private/TraceEvents.cpp` 133–139행 | `Atomic<T>` 보조 연산(11.3절) |
| 5 | 독자 세대 대기(grace period) | 두 세대 카운터 SRCU `LogSystem.cpp` 77–118행, 스레드별 busy 표시 + 전체 대기 `TraceThreadWrite.cpp` 525–534행 | `ReclaimDomain::Synchronize`(12.3절) |
| 6 | Dekker 잠듦·깨움(표시 → fence → 재확인 + 이벤트) | `LogSystem.cpp` 409–445행(`sleeping_`·`wake_`) | `EventCount`(9.2절) |
| 7 | 잠금으로 감싼 고유 번호 발급 + 고갈 Fatal | `Core/Private/Memory/StackAllocator.cpp` 13–20행, `ObjectRuntime/Private/TypeRegistry.cpp` 18–25행, `Core/Private/Text/Name.cpp` 9–20행 | 원자 CAS 발급 함수(11.3절) |
| 8 | 함수 지역 static Mutex(동적 초기화·파괴자) | `LogSystem.cpp` 87–90행, 위 7번의 두 곳 | constinit `Mutex`(8.1절) |
| 9 | 수작업 Once(CAS 상태 + 수동 리셋 이벤트, 실패 상태 포함) | `Crypto/Private/AsyncCrypto.cpp` 286–300행 | `OnceFlag`(9.4절) |
| 10 | 원자 참조 계수 | `SharedPtr.h` 40–70행, `DynamicLibrary.cpp` 185–194행 | 별도 타입 없이 `Atomic<T>`와 보조 연산(11.3절, D16) |
| 11 | 통째로 가져가는 침투 스택(CAS push + exchange로 머리 가져가기) | `Core/Private/Memory/FrameArena.cpp` 268–276행(소멸자 기록 push), 73행(전부 가져가기) | `TakeAllStack`(12.2절) |
| 12 | 미공개 칸 앞의 `YieldThread` 반복(소비자) | `LogSystem.cpp` 434–437행 | `SpinBackoff`(8.5·11.2절) |
| 13 | 공간이 날 때까지 `Wake` + `YieldThread` 바쁜 대기(생산자, `LogOverflow::WaitForSpace`) | `LogSystem.cpp` 781–786행 | 소비자가 알리는 "공간 생김" `EventCount`(9.2절) |

한 번만 기록하는 표시(`LogSystem.cpp` 883행, `D3D11Graphics/Private/Win64/D3D11Device.cpp` 481행, `D3D12Graphics/Private/Win64/D3D12Device.cpp` 439행의 exchange(1) == 0)는 `Atomic<uint32>::Exchange`로 충분해 따로 묶지 않는다. 장치 소유 스레드 표지(`Engine/Source/Runtime/Graphics/Public/Graphics/DeviceOwnership.h` 50–85행)는 참조 계수가 아니라 "소유 스레드 ID를 CAS로 잡고 놓는" 토큰이다. `Atomic<uint64>::CompareExchangeStrong`으로 그대로 두고 따로 묶지 않는다.

시험 코드에도 조건이 설 때까지 `YieldThread`로 도는 반복이 있다(`Core/Tests/BoundedMpscQueueTests.cpp` 38·112·188·197행, `Core/Tests/SharedPtrTests.cpp` 268행, `Task/Tests/WorkerSchedulerTests.cpp` 205·368행 등). 결과 단언은 결정적이지만 기다림이 CPU를 쓴다. SY6에서 `TestGate`(14.2절)로 바꿀 후보다. 제품 코드의 중복은 아니라 표에서는 뺐다.

PlatformBase 밖에서 OS 잠금·Interlocked·`std::atomic`을 직접 부르는 곳은 없다. D3D11·D3D12의 `CreateEventW`(fence 완료 통지)는 OS 핸들이 필요한 GPU 동기화라 범위 밖이다(2.3절).

### 4.6 관련 설계 문서의 요구

| 문서 | 요구 | 이 문서의 답 |
|---|---|---|
| GarbageCollectionScale GC-S1 | `mark` 접근을 `Atomic`(relaxed)으로(156행) | `Atomic<T>`(11절) |
| GarbageCollectionScale GC-S4 | 512칸 블록 work-stealing, 첫 구현은 잠금 풀, CAS 큐는 측정 뒤(184행) | 블록 풀은 `IndexedFreeList`+`FixedBlockPool`(12.2절), deque는 Task 설계 |
| GarbageCollectionScale 64·71행 | 간선마다 전역 SRWLOCK, 할당자 전역 잠금 | 잠금 제거는 GC 쪽 일. 남는 잠금은 1 byte `Mutex`·`IntrusiveMutex`로 슬롯에 넣을 수 있다(8.2절) |
| CVarSystem 294·354행 | snapshot 포인터를 짧은 잠금 안에서 바꾸고 참조 계수를 올려 준다 | `Mutex` + `Atomic<T>` 참조 계수. 잠금 없는 게시가 필요해지면 `ReclaimDomain` |
| CVarSystem 353행 | 다중 생산자·단일 소비자 쓰기 큐, 꽉 차면 `Busy` | `BoundedMpscQueue` 그대로 |
| AppInfoCommandLine D4 | 프레임마다 게시하는 refcount 묶음 | 같은 장치(CVar 묶음)와 함께. 이 문서는 참조 계수 연산만 제공 |
| RenderBase 5.2절 | 완료 통지 큐, lane FIFO | lane 큐는 `DedicatedLane` 잠금 큐 유지. 통지 큐는 `TakeAllStack` 후보(12.2절) |
| AssetSystem 337행 | lease 해제를 미리 할당한 항목으로 큐에 | `IntrusiveMpscQueue`(호출자 소유 노드, 12.2절) |
| TraceSystem 4.1절 | 블록 풀 lock-free, 새 페이지만 잠금 | `IndexedFreeList`로 교체(12.2절) |
| LoggingSystem 8.2·8.4절 | MPSC 큐, Dekker 깨움, `WaitForSpace` | 큐 유지, 깨움은 `EventCount`, 공간 대기도 `EventCount` |
| EngineImplementationPlan 716행 | AudioRT: 동적 할당·lock·IO·일반 scheduler wait 금지 | AudioRT는 잠금을 쓰지 않는다. 입력은 `BoundedSpscQueue`로 받는다(D27) |
| EngineImplementationPlan 719행 | lane별 lock rank를 정하고 debug에서 검사, lock 보유 상태의 task wait 금지 | 순위에 lane 순위를 넣고, task 대기를 "쥔 채 대기" 검사에 넣는다(13.2절, D21) |
