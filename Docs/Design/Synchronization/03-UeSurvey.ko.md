## 3. UE 5.8 조사

### 3.1 ParkingLot

- **뜻.** 주소를 키로 하는 대기 스레드 큐의 전역 표다(`PL` 13–16행). 동기화 객체는 자기 상태 word만 갖고, 기다릴 때 자기 주소로 큐에 들어간다.
- **API.** `Wait`·`WaitFor`·`WaitUntil(address, CanWait, BeforeWait)`, `WakeOne(address, OnWakeState)`, `WakeMultiple`, `WakeAll`(`PL` 38–125행).
  - `CanWait`는 그 주소의 bucket을 잠근 채 부른다. false면 기다리지 않는다. 안에서 ParkingLot을 다시 부르면 안 된다.
  - `BeforeWait`는 bucket을 푼 뒤, 잠들기 직전에 부른다. 깨우기 함수는 불러도 된다. 조건 변수처럼 "잠그고 있던 잠금을 풀고 잔다"를 만들 때 쓴다(`PL` 46–58행).
  - `WakeOne`의 `OnWakeState`는 bucket을 잠근 채 "깨웠는지, 남은 대기자가 있을 수 있는지"를 받고, 깨운 스레드에 넘길 64-bit token을 돌려준다(`PL` 20–34행, `PLC` 917–942행).
- **구현.**
  - 스레드마다 레코드 하나(`FThread`, cache line 정렬)가 있고, 그 안에 플랫폼 수동 리셋 이벤트 하나가 있다(`PLC` 239–276행). 레코드는 `thread_local`로 처음 기다릴 때 만든다(`PLC` 278–310행).
  - 스레드의 `thread_local` 칸이 이미 파괴된 뒤(스레드 종료 중 다른 TLS 소멸자 안)에 기다리면, 칸에 다시 만들지 않고 대기 한 번마다 임시 레코드를 새로 할당해 쓴다(`bDestroyed`, `PLC` 289–292·730–735행). 대기가 끝나면 참조 계수로 반환된다.
  - 표는 bucket 포인터 배열이고 bucket은 cache line 정렬, 잠금은 `FWordMutex`다(`PLC` 326–375행). 주소는 곱셈 해시로 bucket을 고른다(`PLC` 714–720행).
  - 스레드가 처음 ParkingLot을 쓸 때(스레드 레코드 TLS 생성자) `Reserve(살아 있는 사용 스레드 수)`로 bucket 수를 그 수의 2의 거듭제곱 이상(최소 32)으로 키운다. 표를 키울 때는 모든 bucket을 주소 순으로 잠그고 옮긴다. 옛 표는 해제하지 않는다(누수 크기가 유한하다, `PLC` 448–457·607–712·725–728행).
  - 메모리는 4 MiB + 64 KiB 가상 메모리 선형 할당기 두 개에서 받는다. 영구 할당기는 표·bucket·스레드 레코드용이다. 주석은 "16,384 스레드여도 2.25 MiB"라고 계산한다(`PLC` 56–65행, 61행). scratch 할당기는 `Reserve`가 대기 스레드를 모으는 임시 배열과 `WakeMultiple`이 깨울 스레드를 모으는 임시 배열용이다(`PLC` 113–128·610–633·974–976행). 두 할당기의 잠금도 `FWordMutex`다. ParkingLot이 자기 할당기 안에서 ParkingLot을 다시 부르는 재진입을 막기 위해서다. 재진입은 대기·깨우기 함수 머리의 `checkf`로 검사한다(`PLC` 744–747·805·913·971행).
- **크기.** 헤더 133행, 구현 1005행.

### 3.2 WordMutex

- 포인터 크기 잠금이다. ParkingLot에 기대지 않는다. 불공정·비재진입이다(`WM` 14–20행).
- 상태 word의 하위 2 bit가 잠김·큐 잠김 표시이고, 나머지가 대기 스레드 스택 노드(꼬리) 포인터다(`WM` 73–79행). 노드는 대기 스레드의 스택에 있고 노드마다 플랫폼 이벤트가 있다(`WMC` 12–20·50–75행).
- 대기 큐가 없을 때만 `FPlatformProcess::Yield()`로 40번까지 돈다(`WMC` 28–49행).
- 주석이 "될 수 있으면 `FMutex`를 쓰라"고 한다. ParkingLot bucket과 그 할당기 잠금처럼 ParkingLot 아래에서 필요한 곳을 위한 것이다.

### 3.3 Mutex와 IntrusiveMutex

- `FMutex`는 1 byte, 불공정, 비재진입이다(`MX` 16–18행). bit 0이 잠김, bit 1이 "대기자가 있을 수 있음"이다(`MX` 75–76행). `FAcquireLock` 생성자로 잠긴 상태로 만들 수 있다(`MX` 24–27행). 작업 선행 목록이 CAS 없이 잠긴 채 시작하는 데 쓴다(`Public/Tasks/TaskPrivate.h` 746행).
- 구현은 `TIntrusiveMutex`다(`Private/Async/Mutex.cpp`). `TIntrusiveMutex`는 호출자의 원자 정수 안 2 bit로 잠금을 만든다(`IM` 54행).
  - 매개 타입이 잠김 bit, 대기 bit, 선택적 잠김 mask·spin 한도(기본 40, `IM` 41행)·대기 주소를 정한다.
  - 잠금 경로: 풀려 있으면 대기자가 있어도 바로 잡는다. 그래서 FIFO가 아니고 공정하지 않다(`IM` 141행). 대기 bit가 없을 때만 spin한다. 그 뒤 대기 bit를 세우고 `ParkingLot::Wait`로 잔다(`IM` 175행).
  - 풀기: `fetch_sub`로 먼저 풀고, 대기 bit가 있었으면 `WakeOne`한다. 깨운 스레드가 없을 때만 대기 bit를 bucket 잠금 아래의 `OnWakeState` 콜백에서 지운다. 공유 잠금이 배타 잠금보다 먼저 이기지 않게 하기 위해서다(`IM` 197–205·201·216행). 그래서 마지막 대기자를 깨운 뒤에도 bit는 남고, 그다음 `Unlock`이 큐가 빈 것을 보고 지운다(`IM` 199–204행).
  - 대기 bit는 bucket 밖에서 CAS로 세운다(`IM` 161–168행). 그 사이 다른 스레드의 `Unlock`이 bit를 지울 수 있으므로, `CanWait`가 "잠김이고 대기 bit가 있다"를 bucket 잠금 아래에서 다시 확인한다(`IM` 175–179행). 아니면 자지 않고 처음부터 다시 시도한다. 10.3절 공통 규칙의 근거다.
  - `WakeMultiple`·`WakeAll`에는 bucket 잠금 아래 콜백이 없다(`PLC` 965–1003행). 콜백은 `WakeOne`에만 있다.
  - 잠금 대기 중에는 작업 scheduler의 oversubscription(대기 중 worker 보충)을 끈다. 잠금 대기는 짧아서 보충이 오히려 해롭다는 이유다(`IM` 172행, `WMC` 72–74행).
- `FPlatformProcess::Yield()`는 OS 양보가 아니라 CPU pause다. x86은 `_mm_pause`, ARM은 `isb`다(`GPP` 960–978행). OS 양보는 따로 `YieldThread`(Windows `SwitchToThread`, `WPP` 1895–1898행)다.

### 3.4 RecursiveMutex와 RecursiveWordMutex

- `FRecursiveMutex`는 8 byte다. 상태 32 bit(대기 bit + 31 bit 잠금 횟수)와 소유 스레드 ID 32 bit다(`RM` 19–47행). 같은 스레드의 재잠금은 횟수만 올린다(`RMC` 41·70행). spin 40, 불공정이다(`RMC` 79–102행).
- `FRecursiveWordMutex`는 `FWordMutex` + 횟수 + 스레드 ID다. 정적 초기화 전후에도 쓸 수 있어야 하는 곳을 위한 것이다(`Public/Async/RecursiveWordMutex.h`).

### 3.5 SharedMutex와 SharedRecursiveMutex

- `FSharedMutex`는 4 byte, 불공정, 비재진입이다(`SM` 16–22행). bit 0 배타 잠김, bit 1 배타 대기, bit 2 공유 대기, bit 3부터 공유 잠금 수다(`SM` 129–133행).
  - **쓰기 우선.** 배타 잠금을 기다리는 스레드가 있으면 새 공유 잠금은 기다린다(`SM` 19행, `TryLockShared`가 `MayHaveWaitingLockFlag`를 본다).
  - 같은 스레드가 배타와 공유를 함께 쥘 수 없다(`SM` 20행).
  - 공유 대기자는 배타 대기자와 다른 주소에서 잔다. 배타 대기자만 골라 깨울 수 있게 하기 위해서다(`SMC` 27행).
  - 마지막 공유 해제가 배타 대기자 하나를 깨운다(`SM` 103–117행).
- `FSharedRecursiveMutex`는 8 byte다. 스레드별 소유 목록(`FSharedRecursiveMutexLink`, thread_local 연결)으로 같은 스레드의 공유 재진입을 판단한다(`SRM` 8–60행, 구현 387행).
- 승격(공유 → 배타)은 없다. 옛 `FRWScopeLock::ReleaseReadOnlyLockAndAcquireWriteLock_USE_WITH_CAUTION`은 풀고 다시 잡으므로 그 사이에 다른 쓰기가 끼어든다고 주석이 적는다(`Public/Misc/ScopeRWLock.h` 193–219행).

### 3.6 가드와 표지

- `TUniqueLock`(생성 때 잠그고 소멸 때 푼다), `TDynamicUniqueLock`(미룸·이른 해제·이동), `TSharedLock`·`TDynamicSharedLock`(공유판), `TIntrusiveUniqueLock`이 있다(`Public/Async/UniqueLock.h` 12–48행, `SharedLock.h` 14–50행, `IntrusiveUniqueLock.h` 13–17행).
- 표지는 `AcquireLock`(생성 때 잠금), `DeferLock`(미룸) 두 개다(`Public/Async/LockTags.h`).
- 옛 이름 가드 `FScopeLock`·`FScopeUnlock`(`Public/Misc/ScopeLock.h` 140·171행), `FReadScopeLock`·`FWriteScopeLock`(`ScopeRWLock.h` 92·113행), `FScopeTryLock`(`ScopeTryLock.h` 89행)은 `FCriticalSection`·`FRWLock`용이다.

### 3.7 ManualResetEvent와 EventCount

- `UE::FManualResetEvent`는 1 byte다. bit 0 신호, bit 1 대기자 있을 수 있음이다(`MRE` 101–104행). `Notify`는 `exchange`로 신호를 세우고 대기자 bit가 있었으면 `WakeAll`한다(`MRE` 85–88행). `Reset`은 신호 bit만 지운다(`MRE` 97행). 시간 제한은 monotonic 시점(`WaitUntil`)이고 상대 시간은 그 위에 만든다(`MRE` 35–46행).
- `TEventCount<CounterType>`은 "알림 횟수"로 놓친 알림을 막는 이벤트다(`EC` 60행).
  - 기다리는 쪽: `PrepareWait()`로 token을 받고 → 조건을 검사하고 → 아직이면 `Wait(token)`. token 뒤에 알림이 있었으면 바로 돌아온다(`EC` 30–58행 사용 예).
  - 카운터 bit 0은 "기다리려는 스레드 있음"이고 위 bit가 알림 횟수다. `PrepareWait`가 bit 0을 세운다(`EC` 77–90행).
  - `Notify`는 `fetch_add(0, acq_rel)`로 StoreLoad 순서를 만든 뒤, bit 0이 있을 때만 횟수를 올리고 `WakeAll`한다(`EC` 151–169행). 주석이 relaxed load였을 때 알림을 놓쳐 교착한 사례를 적는다(`EC` 154–164행).
  - `NotifyWeak`은 x86에서 StoreLoad 장벽을 생략한다(`EC` 180–205행). 호출자가 순서를 책임진다.
- 쓰는 곳: 작업 상태 변경 대기(`Public/Tasks/TaskPrivate.h` 692행), 파이프 비움 대기(`Public/Tasks/Pipe.h` 36·138행, `Private/Tasks/Pipe.cpp` 80행).

### 3.8 HAL 이벤트와 semaphore, 플랫폼 구현

- **`FEvent`.** 가상 인터페이스이고 자동·수동 리셋을 고른다(`EV` 18–90행). `FEventRef`·`FSharedEventRef`가 풀에서 빌리고 돌려준다(`EV` 108–140행). Windows는 `CreateEvent`+`WaitForSingleObject`다(`Private/Windows/WindowsEvent.h` 13–50행, `WPP` 1931–1941행). 대기 때 통계·CSV·스레드 유휴 통계·oversubscription 범위를 건다(`WPP` 1933–1940행). PThread판은 mutex+condition이다(`Public/HAL/PThreadEvent.h` 29–141행).
- **플랫폼 수동 리셋 이벤트(ParkingLot 스레드 레코드용).**
  - Windows: `WaitOnAddress`·`WakeByAddressSingle`(`Private/Microsoft/MicrosoftPlatformManualResetEvent.cpp` 23·41·67·75행).
  - Android: `futex` syscall `FUTEX_WAIT_PRIVATE`·`FUTEX_WAIT_BITSET_PRIVATE`·`FUTEX_WAKE_PRIVATE`(`Private/Android/AndroidPlatformManualResetEvent.cpp` 38–95행).
  - Apple: `std::mutex`+`std::condition_variable` 공용판을 그대로 쓴다. 주석에 "macOS 14.4+에서 `os_sync_wait_on_address`로 바꿀 것"이라는 TODO가 있다(`Private/Apple/ApplePlatformManualResetEvent.h`, `Private/GenericPlatform/GenericPlatformManualResetEvent.h` 62–123행).
- **semaphore.** `FWindowsSemaphore`는 `CreateSemaphore`(최대값 있음), `FPThreadSemaphore`는 `sem_t`(최대값 없음), `FMacSemaphore`는 `dispatch_semaphore_t`이고 최대값 인자를 무시한다(`Public/Windows/WindowsSemaphore.h` 7–34행, `Public/HAL/PThreadSemaphore.h` 14–61행, `Public/Mac/MacSemaphore.h` 13–21행). 즉 최대값의 뜻이 플랫폼마다 다르다.
- **`FCriticalSection`은 재진입 잠금이다.** Windows는 `CRITICAL_SECTION`(spin 4000), POSIX는 `PTHREAD_MUTEX_RECURSIVE`다(`CS` 53행, `WPM` 19–52행, `Public/HAL/PThreadsRecursiveMutex.h` 22–49행).
- **`FRWLock`은 OS 공유 잠금 어댑터다.** Windows SRWLOCK, POSIX `pthread_rwlock_t`(`CS` 56행, `WPM` 62–116행, `Public/HAL/PThreadsSharedMutex.h` 23–78행).

### 3.9 spin 잠금과 원자 타입

- `UE::FSpinLock`은 `std::atomic<bool>` exchange + 풀릴 때까지 `Yield()`다(`Public/Misc/SpinLock.h` 11–39행).
- `TRWSpinLock`은 쓰기에 `NumericLimits::Max()`, 읽기에 +1을 CAS한다(`Public/Misc/RWSpinLock.h` 15–63행). 쓰기 대기 표시가 없어 읽기가 계속 들어오면 쓰기가 굶는다.
- 원자 타입: 3절 머리의 deprecated 목록처럼 UE는 자체 원자 타입·함수를 버리고 `std::atomic`을 쓰라고 한다.

### 3.10 lock-free 컨테이너, 할당기, 회수

| UE 타입 | 뜻과 구현 | 메모리 회수·ABA |
|---|---|---|
| `TSpscQueue` | 생산자·소비자 하나의 무한 큐. 1024cores 방식 노드 연결, 소비한 노드를 재사용(`Public/Containers/SpscQueue.h` 10–60행) | 파괴 때까지 노드를 해제하지 않는다 |
| `TMpscQueue` | 생산자 여럿·소비자 하나의 무한 큐. 1024cores 비침투 노드 방식(`Public/Containers/MpscQueue.h`) | push마다 노드를 할당한다. 소비자 하나라 ABA가 없다 |
| `TClosableMpscQueue` | 한 번만 비울 수 있는 MPSC. `Close(consumer)`가 head를 `nullptr`로 바꿔 닫고 FIFO로 소비한다. 닫힌 뒤 `Enqueue`는 false(`Public/Containers/ClosableMpscQueue.h` 44–133행) | 닫는 쪽이 노드를 모두 가져간다 |
| `TConsumeAllMpmcQueue` | 스택으로 구현한 MPMC. `ConsumeAllLifo`·`ConsumeAllFifo`가 head를 exchange로 통째로 가져간다. 주석에 "atomic race detector로 검증"(`Public/Containers/ConsumeAllMpmcQueue.h`) | 통째로 가져가므로 pop 경합이 없다 |
| `TLockFreePointerList*`(LIFO·FIFO·Unordered·Closable) | 색인 연결 리스트. 링크는 26-bit 색인(최대 2^26개), 나머지 38 bit가 ABA 태그·상태다(`LFL` 55–56·132–223행). 링크 풀은 16384개씩 페이지로 늘리고(`LFL` 58–128·242행) 스레드별 묶음 캐시가 있다(`LFLC` 75–150행) | **링크를 해제하지 않는다.** 태그 2^23 이상 여유를 static_assert한다(`LFL` 274–277행). 링크 소진은 Fatal(`LFLC` 47–49행), 태그 넘침은 로그만(`LFLC` 41–43행) |
| `TLockFreeFixedSizeAllocator`(·`_TLSCache`) | 고정 크기 블록 풀. 비면 `FMemory::Malloc`, 돌려받은 블록은 lock-free 목록에 쌓는다(`LFA` 189–335행) | 파괴 때만(또는 끝까지) 해제한다(`LFA` 191·340·384행) |
| `FHazardPointerCollection`·`THazardPointer`(Experimental) | 스레드가 읽는 포인터를 hazard 칸에 게시하고(`seq_cst` fence, `HP` 105행) 해제는 스레드별 목록에 미룬다. 1초가 지났거나 목록이 hazard 칸 수의 5/4 이상이면 모든 hazard를 정렬해 대조하고 안 쓰이는 것을 지운다(`HPC` 52–137행) | 유일한 사용처는 `FAAArrayQueue`(`Public/Experimental/Containers/FAAArrayQueue.h` 122–233행)다. 이것이 작업 scheduler의 overflow 큐(`LQ` 193행), `TLocalWorkQueue`(`Public/Async/LocalWorkQueue.h` 51행), `QueuedThreadPoolWrapper`(`Public/Misc/QueuedThreadPoolWrapper.h` 560행)에 쓰인다 |

정리하면 UE의 lock-free 쪽 기본 전략은 "노드를 해제하지 않는다 + 색인 태그로 ABA를 막는다"이다. 실제 해제가 필요한 곳(무한 MPMC 큐) 하나만 hazard pointer를 쓴다.

### 3.11 쓰는 곳

사용 수는 `git grep -c -F '<이름>' -- Engine/Source/Runtime`의 파일별 줄 수 합이다(개정 2에서 다시 셌다). namespace 안에서 접두 없이 쓴 것은 빠지므로 하한이다. 예를 들어 `UE::` 없이 단어 `FMutex`만 쓴 줄이 46줄 더 있다.

| 타입 | 수(하한) | 대표 사용처 |
|---|---|---|
| `FCriticalSection` | 936 | 렌더 정리 목록 잠금 `RenderCore/Private/RenderingThread.cpp` 1336행, GC schema 조립 `CoreUObject/Private/UObject/GarbageCollection.cpp` 7013행, 비동기 로딩 요청 `CoreUObject/Private/Serialization/AsyncLoading2.cpp` 4894행 |
| `FRWLock` | 230 | 셰이더 hash 표 `RenderCore/Private/ShaderCore.cpp` 562행, 텍스처 스트리밍 보호 에셋 `Engine/Private/Streaming/StreamingManagerTexture.h` 471행, `AsyncLoading2.cpp` 9182행 |
| `UE::FMutex` | 137 | 작업 선행 목록(잠긴 채 생성) `Core/Public/Tasks/TaskPrivate.h` 746행, scheduler TLS 목록 `Core/Private/Async/Fundamental/Scheduler.cpp` 66행, GC root `GarbageCollection.cpp` 632행, 전역 렌더 자원 `RenderCore/Private/GlobalRenderResources.cpp` 879행, 레벨 텍스처 `Engine/Private/Streaming/LevelTextureManager.h` 93행 |
| `UE::FSharedMutex` | 19 | RDG pass 이름 표(`TSharedLock`·`TUniqueLock`) `RenderCore/Private/RenderGraphTrace.cpp` 552·605·616행, `AsyncLoading2.cpp` 938행 |
| `FEventCount` | 25 | 작업 상태 변경 `TaskPrivate.h` 692행, 파이프 `Pipe.cpp` 80행 |
| `UE::FManualResetEvent` | 44 | 로딩 완료·주 스레드 깨움 `AsyncLoading2.cpp` 4719·4769행 |
| `TMpscQueue` | 71 | 로딩 요청·재우선순위·지연 정리 `AsyncLoading2.cpp` 3768–3769·4781·4944행 |
| `TConsumeAllMpmcQueue` | 44 | GC dirty root `GarbageCollection.cpp` 630·693·717행, scheduler TLS 삽입·삭제 `Scheduler.cpp` 69–70행 |
| `TLockFreePointerList*` | 65 | 렌더 지연 정리 목록 `RenderingThread.cpp` 1380행, 이벤트 풀 `Private/Misc/EventPool.h` |
| `FWordMutex` | 41 | ParkingLot 내부(3.1절) |
| `UE::FRecursiveMutex` | 23 | |
| `FSharedRecursiveMutex` | 66(정의 파일 49 포함) | |
| `FSpinLock` / `TRWSpinLock` | 5 / 4 | |

작업 scheduler의 worker 잠듦은 `FWaitEvent`가 HAL 수동 리셋 이벤트(`FEventRef`)를 쥐는 방식이다(`Public/Async/Fundamental/WaitingQueue.h` 33–40행).

### 3.12 UE의 진단과 시험

- 잠금 순서 검사기는 `Async`·`HAL`에서 찾지 못했다(문자열 검색 기준, 16절 미확인).
- 경합 기록: `FEventWin::Wait`가 통계·CSV 대기·스레드 유휴 통계를 건다(`WPP` 1933–1940행). ParkingLot 경로에는 trace·통계 호출이 없다(`PLC` 검색).
- 경쟁 검출: 자체 instrumentation 기반 race detector가 있다(`Public/Sanitizer/RaceDetector.h`, `Private/Sanitizer/RaceDetectorWindows.cpp`, `USING_INSTRUMENTATION`). ThreadSanitizer 빌드 표시 `USING_THREAD_SANITISER`도 있다(`Public/HAL/Platform.h` 172행).
- 시험: `Engine/Source/Runtime/Core/Tests/Async/`에 ParkingLot·Mutex·SharedMutex·ManualResetEvent·Semaphore 시험이 있다. `SharedMutexTest.cpp` 244행은 "배타 잠금이 기다리고 있기를" `Sleep(0.001f)`로 기대한다("Hopefully reliable enough"). Rocks는 이 방식을 쓰지 않는다(14.2절).
