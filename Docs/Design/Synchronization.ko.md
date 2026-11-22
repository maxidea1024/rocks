# 동기화 객체와 lock-free 계층 설계

기준일: 2026-11-22, 개정 3(9.6절 `RealtimeWakeSignal` — [오디오 시스템](AudioSystem.ko.md) 요구). 개정 2(2026-11-18): 교차검증 1차 반영, 결정 확정. 사용자 지시("설계문서만 작성해놔")로 쓴 설계이고 구현하지 않는다. 결정은 권장안으로 확정했다(16절). 원칙과 부딪히는 항목과 범위가 큰 항목은 16절에 따로 표시했다. 개정 2의 반영 내용은 18절에 있다.

**착수 시점.** 고정 구현 순서(Curve → A0-G → A1–A6 → 화면 마일스톤)를 바꾸지 않는다. 설계만 한다. 이 문서의 단계(SY — 15절)는 그 순서 안에 끼워 넣지 않으며 착수 시점은 사용자가 정한다.

이 문서는 세 가지를 정한다.
- **동기화 객체.** 잠금(Mutex·재진입·읽기쓰기), 조건 변수, 이벤트, EventCount, Semaphore, Once와 그 바탕인 parking lot을 어디에 어떤 모양으로 두는지.
- **lock-free 계층.** 원자 값 래퍼, 잠금 없는 컨테이너·고정 크기 할당기, 메모리 회수 방식, ABA 대책.
- **진단과 시험.** 잠금 순서 검사, 경합 trace, 교착 진단, 시간 운에 기대지 않는 시험.

UE 근거는 UE 5.8.4(`release`, commit `f45729c0b6f097038e72b52d366a62baeca3275b`, 로컬 클론 `C:\github\UnrealEngine`)다. 경로는 `Engine/Source/Runtime/Core/` 기준이고, 다른 모듈은 `Engine/Source/Runtime/`부터 쓴다. 아래 약자를 쓴다.

| 약자 | 파일 |
|---|---|
| `PL` / `PLC` | `Public/Async/ParkingLot.h` / `Private/Async/ParkingLot.cpp` |
| `WM` / `WMC` | `Public/Async/WordMutex.h` / `Private/Async/WordMutex.cpp` |
| `MX` | `Public/Async/Mutex.h`(구현 `Private/Async/Mutex.cpp`) |
| `IM` | `Public/Async/IntrusiveMutex.h` |
| `RM` / `RMC` | `Public/Async/RecursiveMutex.h` / `Private/Async/RecursiveMutex.cpp` |
| `SM` / `SMC` | `Public/Async/SharedMutex.h` / `Private/Async/SharedMutex.cpp` |
| `SRM` | `Public/Async/SharedRecursiveMutex.h` |
| `EC` | `Public/Async/EventCount.h` |
| `MRE` | `Public/Async/ManualResetEvent.h` |
| `EV` | `Public/HAL/Event.h` |
| `WPP` | `Private/Windows/WindowsPlatformProcess.cpp` |
| `GPP` | `Public/GenericPlatform/GenericPlatformProcess.h` |
| `WPM` | `Public/Windows/WindowsPlatformMutex.h` |
| `CS` | `Public/HAL/CriticalSection.h` |
| `LFL` / `LFLC` | `Public/Containers/LockFreeList.h` / `Private/Containers/LockFreeList.cpp` |
| `LFA` | `Public/Containers/LockFreeFixedSizeAllocator.h` |
| `HP` / `HPC` | `Public/Experimental/Containers/HazardPointer.h` / `Private/Experimental/Containers/HazardPointer.cpp` |
| `LQ` | `Public/Async/Fundamental/LocalQueue.h` |

레거시·deprecated 경로는 기준으로 삼지 않는다. 구체적으로 다음을 뺐다.
- `TExternalMutex`(`Public/Async/ExternalMutex.h` 23행 `UE_DEPRECATED(5.8, "Use TIntrusiveMutex.")`).
- `DepletableMpscQueue.h`(`UE_DEPRECATED_HEADER(5.2)`). 이 헤더가 가리키는 `DepletableMpmcQueue.h`는 5.8.4 트리에 없다.
- `FEvent` 직접 생성(`EV` 36행 `UE_DEPRECATED(5.0)`)과 `CreateSynchEvent`(`Public/Windows/WindowsPlatformProcess.h` 199행).
- `TAtomic`(`Public/Templates/Atomic.h` 13·528행), `FThreadSafeCounter`·`FThreadSafeCounter64`·`FThreadSafeBool`(`Public/HAL/ThreadSafeCounter*.h`, `ThreadSafeBool.h` 9행), `FPlatformAtomics`(`Public/GenericPlatform/GenericPlatformAtomics.h` 20행). 모두 "새 코드는 `std::atomic`"이라고 적혀 있다.
- `TQueue`(`Public/Containers/Queue.h` 11행 "planned for deprecation in favor of TSpscQueue or TMpscQueue", 사용 163곳)와 `TCircularQueue`(유한 SPSC, `Public/Containers/CircularQueue.h` 9행 "planned for deprecation in favor of TSpscQueue").

UE 이름은 3절 근거 문장과 6절 대응표에만 쓴다.

## 1. 요약

- **지금 Rocks에 있는 것은 OS 잠금 wrapper와 그 위의 이벤트 두 개다.** `Platform/Sync.h`의 `Mutex`는 Windows SRWLOCK·POSIX `pthread_mutex_t`를 64 byte 저장소에 감싼다. `ManualResetEvent`·`Semaphore`는 그 Mutex와 `ConditionVariable` 위의 공용 구현이다(4.1절). 읽기쓰기 잠금·재진입 잠금·EventCount·Once·`Atomic<T>`·범용 lock-free 컨테이너·메모리 회수는 없다.
- **없어서 모듈마다 따로 만들었다.** spin 잠금 2곳, 시한 TryLock 1곳, 태그 색인 free list 2곳, 독자 세대 대기(SRCU류) 2곳, Dekker 잠듦·깨움 1곳, 원자 상한·포화·갱신 연산 6곳, 잠금으로 감싼 고유 번호 발급 3곳, 수작업 Once 1곳, 통째로 가져가는 침투 스택 1곳, 양보로 도는 바쁜 대기 2곳이다(4.5절). 공용 기능 원칙에 따라 이 문서의 장치로 모은다.
- **UE 5.8은 parking lot 위에 작은 잠금을 쌓는다.** 1 byte `FMutex`, 4 byte `FSharedMutex`, 8 byte `FRecursiveMutex`, `FEventCount`, `FManualResetEvent`가 모두 주소 키 대기열(`ParkingLot`)을 쓴다. 불공정(barging)이고, 대기자가 없을 때만 CPU pause를 40번까지 돈 뒤 잠든다(3.2·3.3절). HAL의 `FEvent`·`FCriticalSection`·`FRWLock`은 OS 객체 wrapper로 남아 있고 사용 수는 아직 더 많다(3.11절).
- **Rocks는 UE 구조를 따르되 자리는 PlatformBase다.** 동기화 객체는 지금처럼 PlatformBase에 두고, OS 잠금 wrapper를 parking lot 기반 공용 구현으로 바꾼다. OS별 코드는 "한 스레드가 주소를 기다리고 다른 스레드가 깨우는" 수단 하나로 줄인다. PlatformBase 자신(`File.h` 285행, `VirtualMemory.h` 70행)도 잠금이 필요하므로 Core에 두면 Mutex가 두 종류가 된다(7절, D2).
- **lock-free 컨테이너와 회수는 Core다.** 우선 "회수가 필요 없는" 설계(미리 잡은 고정 풀 + 색인 + 태그)를 쓴다. 회수가 꼭 필요할 때만 SRCU 방식(도메인별 두 세대 카운터)의 회수 영역을 쓴다. 독자 경로는 RMW 두 번뿐이고 해제 쪽 대기를 위한 비용을 더하지 않는다. hazard pointer는 두지 않는다(12.3절, D17).
- **대기 표시는 한 규칙으로 다룬다.** bucket 밖에서 세운 "대기자 있음" 표시는 CanWait에서 다시 확인하고, 지우기는 bucket 잠금 아래 콜백에서만 한다. 모든 잠금·신호 타입이 이 규칙을 따른다(10.3절, D29).
- **원자 접근은 `Atomic<T>`·`AtomicRef<T>` 한 계열이다.** UE는 자체 원자 타입을 버리고 `std::atomic`으로 갔다. Rocks는 STL을 쓰지 않으므로 같은 역할의 `platform::Atomic<T>`와 기존 객체를 원자로 보는 `AtomicRef<T>`를 둔다. 연산자 overload도 순서 기본값도 없이 순서를 늘 이름으로 적는다. 지금의 자유 함수는 이 둘의 내부 구현(`platform::detail`)으로 내린다(11절, D15).
- **시험은 sleep 없이 순서를 만든다.** parking lot에 "잠들었다"를 알리는 시험 훅을 두고, lock-free 알고리즘에는 단계 지점을 두어 interleaving을 정해진 순서로 돌린다. 시한은 OS 대기까지 가짜로 바꿔 실제 시간을 쓰지 않는다. 오래 도는 stress는 기본 실행에서 뺀다. 정적 검사는 Clang thread safety analysis를 쓴다(13·14절).

## 2. 목적과 범위

### 2.1 목적

- 엔진·도구 코드가 필요로 하는 프로세스 내부 동기화를 한 곳에서 제공한다. 사용자 말 "코어에 Event, Semaphore 같은 동기화 객체들이 없는데 나중에라도 필요하지 않나? ReadWriteLock이라던가", "락프리 지원도 없는 것 같고"에 답한다.
- 이미 계획된 작업이 기대는 기반을 미리 정한다.
  - GC 규모 작업 GC-S1·GC-S4는 `Atomic` 표식과 블록 work-stealing을 요구한다(`Docs/Design/GarbageCollectionScale.ko.md` 152–195행).
  - CVar·Config는 snapshot 포인터 게시와 다중 생산자 쓰기 큐를 쓴다(`CVarSystem.ko.md` 294·353–354행).
  - 앱 정보 D4는 프레임마다 게시하는 refcount 묶음을 worker에 넘긴다(`AppInfoCommandLine.ko.md` 1307행).
  - 렌더 기반은 완료 통지 큐와 lane 간 전달을 쓴다(`RenderBase.ko.md` 47·52·160행).
  - 에셋 lease 해제 큐는 "미리 할당한 큐 항목"을 요구한다(`AssetSystem.ko.md` 337행). 이것은 호출자 소유 노드의 MPSC 큐다.
  - 텍스처 스트리밍은 IO 완료 worker가 단계를 넘긴다(`TextureStreaming.ko.md` 274–275행).
- 모듈이 각자 만든 spin·free list·세대 대기를 공용 장치로 바꿀 근거를 둔다(4.5절).

### 2.2 범위

- 프로세스 내부 잠금: `Mutex`, `IntrusiveMutex`, `RecursiveMutex`, `ReadWriteMutex`, `ConditionVariable`, 가드.
- 신호: `ManualResetEvent`, `AutoResetEvent`, `EventCount`, `Semaphore`, `OnceFlag`.
- 대기 기반: `ParkingLot`, 스레드 대기 신호, 주소 대기, CPU pause.
- 원자 값: `Atomic<T>`·`AtomicRef<T>`와 보조 연산(상한 갱신, 포화 더하기, 0이 아닐 때만 증가, 함수 갱신), `CpuPause`·`SpinBackoff`.
- lock-free 컨테이너와 할당기, 메모리 회수, ABA 대책.
- 진단: 정적 검사, 잠금 순위, 경합 trace, 교착 진단.
- 시험 방식과 구현 단계.

### 2.3 범위 밖

- **프로세스 간 동기화.** UE `FSystemWideCriticalSection`(`CS` 16행, `WPM` 120행)과 이름 있는 프로세스 간 semaphore(`WPP` 2111–2130행)는 대응하지 않는다. 에셋 쓰기 잠금은 이미 파일 잠금(`FileLock`, `AssetSystem.ko.md` 270행)으로 한다.
- **AutoRTFM 전용 계열.** `TransactionallySafeMutex`·`TransactionallySafeSharedMutex`·`TransactionallySafeRecursiveMutex`·`TransactionallySafeSharedRecursiveMutex`·`TransactionallySafeMutexPool`(`Public/Async/`)과 `TransactionallySafeMpscQueue`·`TransactionallySafeSpscQueue`(`Public/Containers/`)는 다루지 않는다. 이 타입들은 `AutoRTFM::IsClosed()`·`AutoRTFM::Close`로 트랜잭션 안의 잠금을 미루거나 되돌리는 장치다(`Public/Async/TransactionallySafeMutex.h` 45–119행). Rocks에는 AutoRTFM(트랜잭션 메모리 기반 스크립트 롤백)이 없고 도입 계획도 없다. `TAtomicRefCount`(`Public/Async/AtomicRefCount.h`)도 같은 이유로 AutoRTFM 부분은 빼고 "원자 참조 계수"라는 뜻만 11절에서 다룬다.
- **작업 scheduler 내부 큐와 worker 보충.** work-stealing deque(UE `TWorkStealingQueue2`, `LQ` 33–157행), overflow 큐(UE `FAAArrayQueue`, `LQ` 193행), 대기 중 worker 보충(UE oversubscription)은 **Task 설계(후속 문서)** 의 몫이다. 이 문서는 그 큐가 쓸 원자·회수 기반과 훅 자리만 정한다(10.5·12.5절). Task 설계 문서는 아직 없다. `Docs/Design`에는 Task 문서가 없고, 지금 있는 것은 `EngineImplementationPlan.ko.md` 423·719행의 원칙 문장뿐이다(D33).
- **GPU 동기화.** fence·timeline은 RenderBase(`Graphics/FenceTimeline.h`)다.
- **이벤트 풀과 가상 인터페이스.** UE는 OS 이벤트 생성 비용 때문에 `FEvent`를 풀에서 빌려 준다(`Private/Misc/EventPool.h` 71–115행, `Private/HAL/ThreadingBase.cpp` 795–863행). Rocks 이벤트는 1 byte 값이라 풀이 필요 없다(9.1절).

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

## 5. 원칙

- **잠금이 기본이다.** 정확성이 먼저다. lock-free는 측정으로 이득이 보이고, 12절의 회수·ABA 규칙과 14절의 interleaving 시험을 통과한 곳에만 쓴다(`EngineImplementationPlan.ko.md` 423·768행과 같다).
- **작고 상수 초기화되는 동기화 객체.** 모든 잠금·이벤트는 constexpr 생성자와 trivially destructible이다. 전역 생성자 없이 `constinit` 전역으로 둘 수 있어야 한다. 그래야 spin 잠금을 따로 만들 이유가 없다(4.5절 1·8번).
- **OS별 코드는 대기 수단 하나로 줄인다.** 잠금·이벤트 알고리즘은 공용 코드 한 벌이다. 플랫폼마다 다른 것은 "한 스레드가 잠들고 다른 스레드가 깨운다"뿐이다(7.1절).
- **push 경로에서 할당하지 않는다.** lock-free 컨테이너는 초기화 때 용량을 잡거나 호출자가 노드를 준다. 실패는 `Result`·`false`로 드러내고 버리지 않는다.
- **최대값과 "없음"은 이름으로.** 시간 무한은 `InfiniteTimeout`(= `NumericLimits<uint32>::Max()`), 색인 없음은 `IndexNone` 계열, 선택 값은 `Optional`이다.
- **시간 운에 기대지 않는다.** 구현에는 시간으로 순서를 맞추는 코드가 없다(spin 한도는 횟수다). 시험은 sleep 없이 gate·훅으로 순서를 만들고, 시한은 시계와 OS 대기를 함께 가짜로 바꿔 확인한다(14절).
- **도는 반복은 끝이 있다.** 값이 바뀔 때까지 도는 곳은 `CpuPause`를 정해진 횟수만 하고 그 뒤 `YieldThread`나 대기로 넘어간다(`SpinBackoff`, 8.5절). 상대가 선점되어 멈춘 경우에도 CPU를 태우지 않는다.
- **순서는 RMW로 만든다.** StoreLoad 순서가 필요한 곳은 단독 fence 대신 RMW(`FetchAdd(0)`·`Exchange`)로 만든다. ThreadSanitizer가 단독 fence를 모델하지 못하기 때문이다(14.5절).
- **STL·외부 라이브러리 없음.** 원자는 `Atomics.h`의 Clang builtin, OS 대기는 OS API다. `std::atomic`·`std::mutex`는 쓰지 않는다.

## 6. 이름과 대응표

Rocks 이름은 서술형이다. `Mutex`·`Semaphore`·`ParkingLot`·`EventCount`처럼 업계 공용어는 그대로 쓴다(ParkingLot은 WebKit WTF·Rust parking_lot에서 쓰는 이름이다). PlatformBase 타입은 namespace `rocks::platform`, Core 타입(동시 컨테이너·`ReclaimDomain`)은 `rocks`다(7절).

| Rocks | UE 5.8 | 비고 |
|---|---|---|
| `ParkingLot` | `UE::ParkingLot` | 주소 키 대기 큐 |
| `detail::QueueMutex`(비공개) | `FWordMutex` | ParkingLot 내부 전용 |
| `WaiterSignal` | `FPlatformManualResetEvent` | 스레드 하나가 기다리는 수동 리셋 신호(OS 대기) |
| `RealtimeWakeSignal` | (없음 — UE 오디오 렌더는 `FEvent`) | 대기자 하나, 자동 리셋, Signal에 잠금 없음(9.6절, 개정 3) |
| `CpuPause()` | `FPlatformProcess::Yield()` | spin 힌트(x86 `pause`, ARM64 `isb`) |
| `YieldThread()`(기존) | `FPlatformProcess::YieldThread()` | OS 양보 |
| `Mutex` | `UE::FMutex` | 1 byte |
| `IntrusiveMutex<Params>` | `UE::TIntrusiveMutex` | 호출자 원자 정수 안 2 bit |
| `RecursiveMutex` | `UE::FRecursiveMutex`, `FCriticalSection` | 재진입 |
| `ReadWriteMutex` | `UE::FSharedMutex`, `FRWLock` | 4 byte, 쓰기 우선 |
| (두지 않음) | `UE::FSharedRecursiveMutex` | 16절 D6 |
| (두지 않음) | `UE::FSpinLock`, `TRWSpinLock` | 8.5절 |
| `ConditionVariable` | (없음) | Rocks 기존 타입을 parking lot 위로 |
| `LockGuard<Lock>` | `UE::TUniqueLock`, `FScopeLock` | |
| `ReadLockGuard<Lock>` | `UE::TSharedLock`, `FReadScopeLock` | |
| `LockOwner<Lock>` | `UE::TDynamicUniqueLock`, `FScopeTryLock` | 미룸·시도·이른 해제·이동 |
| `ReadLockOwner<Lock>` | `UE::TDynamicSharedLock` | |
| `UnlockGuard<Lock>` | `UE::TScopeUnlock`, `FScopeUnlock` | 범위 동안 풀어 둔다 |
| `LockStart::Deferred`·`LockStart::Try` | `UE::DeferLock` | `LockOwner` 생성 방식 |
| `Mutex(InitiallyLocked)` | `UE::AcquireLock` | 잠긴 채 생성 |
| `ManualResetEvent` | `UE::FManualResetEvent`, `FEvent`(수동) | |
| `AutoResetEvent` | `FEvent`(자동), `FEventRef(EEventMode::AutoReset)` | |
| `EventCount`·`EventCountToken` | `UE::TEventCount`·`TEventCountToken` | |
| `Semaphore` | `UE::FSemaphore`(HAL) | 최대값을 모든 플랫폼에서 지킨다 |
| `OnceFlag`·`CallOnce`·`CallOnceUntilSuccess` | `UE_CALL_ONCE` | `Public/Misc/CoreMiscDefines.h` 181행. UE는 C++ 함수 지역 static 초기화에 맡긴다 |
| `Atomic<T>` | `std::atomic<T>`(UE가 권하는 것), `TAtomic`(deprecated) | |
| `AtomicRef<T>` | `std::atomic_ref<T>` | 기존 객체를 원자로 본다 |
| `SpinBackoff` | (없음, UE는 각 잠금 안에서 `Yield()` 횟수를 센다) | `CpuPause` N번 뒤 양보 |
| `BoundedMpscQueue<T>`(기존) | (대응 없음) | Core |
| `BoundedSpscQueue<T>` | `TSpscQueue`(무한), `TCircularQueue`(유한, deprecated 예정) | Core, 유한 |
| `IntrusiveMpscQueue<Node>` | `TMpscQueue` | Core, 노드는 호출자 소유 |
| `TakeAllStack<Node>` | `TConsumeAllMpmcQueue`, `TClosableMpscQueue` | Core, 닫기 선택 |
| `IndexedFreeList` | `TLockFreePointerListLIFO`(색인 링크) | Core |
| `FixedBlockPool` | `TLockFreeFixedSizeAllocator` | Core |
| `ReclaimDomain` | `FHazardPointerCollection`(방식은 다름) | Core, SRCU 방식(도메인별 두 세대 카운터) |
| `LockRank`·`RankedMutex<Rank>` | (없음) | Debug 잠금 순위 |

## 7. 계층과 모듈 배치

### 7.1 층

| 층 | 모듈·헤더 | 내용 |
|---|---|---|
| OS 대기 | PlatformBase `Private/<OS>/` | `WaiterSignal`의 OS 구현 하나. Windows `WaitOnAddress`·`WakeByAddressSingle`, Android `futex`(`FUTEX_WAIT_BITSET_PRIVATE`·`FUTEX_WAKE_PRIVATE`), macOS `pthread_mutex_t`+`pthread_cond_t`(7.3절). Debug·Development에는 시험이 OS 대기를 가짜로 바꾸는 seam이 있다(14.2절) |
| 원자·spin | PlatformBase `Platform/Atomics.h` | `Atomic<T>`·`AtomicRef<T>`·보조 연산·`CpuPause()`·`SpinBackoff`. builtin을 감싼 함수는 `platform::detail`로 내린다(11절) |
| 대기 기반 | PlatformBase `Platform/Sync/ParkingLot.h`, `Private/Common/Sync/` | `ParkingLot`, `detail::QueueMutex` |
| 잠금·신호 | PlatformBase `Platform/Sync/*.h`, `Private/Common/Sync/` | `Mutex`·`IntrusiveMutex`·`RecursiveMutex`·`ReadWriteMutex`·`ConditionVariable`·가드·이벤트·`EventCount`·`Semaphore`·`OnceFlag` |
| 진단 훅 | PlatformBase `Platform/Sync/SyncHooks.h`, 시험 설치점 `Private/Common/Sync/SyncTestAccess.h` | 경합 trace·잠금 순위 위반 보고의 함수 표(Core·TraceCapture가 설치한다), 시험 훅·단계 훅·가짜 OS 대기(시험이 `*TestAccess.h`로 설치한다, 13·14절) |
| 동시 컨테이너·회수 | Core `Core/Containers/`, `Core/Memory/` | `BoundedMpscQueue`(기존)·`BoundedSpscQueue`·`IntrusiveMpscQueue`·`TakeAllStack`·`IndexedFreeList`·`FixedBlockPool`·`ReclaimDomain` |
| 사용 | Task·TraceCapture·ObjectRuntime·Engine | Engine/Source/Runtime에는 동기화 기반을 두지 않는다 |

### 7.2 Sync.h를 PlatformBase에 두는 이유

UE는 OS wrapper를 `HAL/`, parking lot 계열을 `Async/`에 두지만 둘 다 한 모듈 `Core` 안이다. Rocks의 Base → PlatformBase → Core 셋이 UE `Core` 하나에 해당한다. 그래서 "UE Core 아래"라는 조건은 PlatformBase·Core 어느 쪽이든 맞는다. 자리를 가르는 것은 의존 방향이다.

- PlatformBase 자신이 잠금을 쓴다(4.1절). 잠금 계열을 Core에 두면 PlatformBase에는 OS 잠금이 따로 남아 Mutex가 두 종류가 된다. 어느 것을 쓸지 매번 골라야 하고 공용 기능 원칙에도 어긋난다.
- ParkingLot이 필요한 것(thread_local, 가상 메모리 예약, `Fatal`, 단조 시계)은 모두 PlatformBase에 있다. Core의 할당기·로그를 쓰지 않는다.
- 진단(잠금 순위 위반 로그, 경합 trace)은 위 모듈 기능이 필요하다. PlatformBase는 함수 표만 두고 Core(로그)·TraceCapture(trace)가 설치한다. 설치 전에는 Debug 위반이 `Fatal` 메시지로, trace는 없음으로 동작한다.

그래서 **잠금·신호는 PlatformBase에 남기고 구현을 공용 코드로 바꾼다.** OS 고유 코드는 `WaiterSignal` 하나로 줄인다. lock-free 컨테이너·회수는 할당기·`Result`가 필요하므로 Core다.

### 7.3 OS 대기 수단

| 플랫폼 | 수단 | 근거·이유 |
|---|---|---|
| Windows | `WaitOnAddress`·`WakeByAddressSingle`(Synchronization.lib) | UE와 같다. 시간 제한은 남은 millisecond로 다시 부른다. 링크는 `PlatformBase.Build.cs`의 Windows 분기에 `SystemLibraries = ["Synchronization"]`을 넣어 한다. 빌드 도구가 이 칸을 검사하고 `.lib`로 넘긴다(`Programs/Rocks.Build/InputValidation.cs` 136행, `ActionGraph.cs` 280–281행). 기본 링크 목록(`ActionGraph.cs` 318행)에는 없다(D32) |
| Android(API 26 이상) | `syscall(SYS_futex, FUTEX_WAIT_BITSET_PRIVATE)`(절대 monotonic 시한), `FUTEX_WAKE_PRIVATE` | UE와 같다. `EINTR`·`EAGAIN`은 다시 검사한다 |
| macOS(최소 13.0, `Programs/Rocks.Build/Toolchain.cs` 131–134행) | `pthread_mutex_t`+`pthread_cond_t`(스레드 레코드마다 하나) | `os_sync_wait_on_address`는 14.4부터라 최소 버전 13.0에서 늘 쓸 수 없다. `__ulock_wait`는 비공개 API라 쓰지 않는다. UE도 Apple은 조건 변수판이다. 14.4 이상에서 `__builtin_available`로 바꾸는 것은 측정 뒤(D4) |

`WaiterSignal`은 스레드 레코드와 `detail::QueueMutex` 대기 노드 안에만 있고 대기자는 늘 그 스레드 하나다. 그래서 OS 쪽 요구는 "값이 0인 동안 잠들기, 1로 바꾸고 깨우기, 시한"뿐이다. 모든 잠금의 OS 의존은 이 타입 하나다.

- Windows·Android의 깨우기는 주소를 키로만 쓴다. 대기자가 먼저 깨어 노드가 사라진 뒤에 깨우기가 불려도 메모리를 건드리지 않는다.
- macOS판은 깨우는 쪽이 노드의 `pthread_mutex_t`를 쥔 채 값을 바꾸고 `pthread_cond_signal`한 뒤 푼다. 대기자는 그 mutex를 다시 잡아 값을 본 뒤에야 돌아가므로, 깨우는 쪽의 마지막 접근은 `pthread_mutex_unlock`이다. 그 뒤의 파괴는 POSIX가 허용하는 "풀린 mutex의 파괴"에 기댄다(17절 미확인). 스택 노드(`QueueMutex`)도 이 계약을 지킨다.

### 7.4 헤더 구성

`Platform/Sync.h`는 SY6에서 지운다(하위호환 없음). 쓰는 것만 include하도록 나눈다.

- `Platform/Sync/ParkingLot.h`, `Mutex.h`(`Mutex`·`IntrusiveMutex`·`InitiallyLocked`), `RecursiveMutex.h`, `ReadWriteMutex.h`, `ConditionVariable.h`, `LockGuard.h`(가드 다섯과 `LockStart`), `Events.h`(`ManualResetEvent`·`AutoResetEvent`·`EventCount`), `Semaphore.h`, `Once.h`, `WaitTypes.h`(`InfiniteTimeout`·`WaitResult`), `LockRank.h`, `SyncHooks.h`.
- 시험 설치점은 공개 헤더가 아니라 모듈 Private의 `*TestAccess.h`다(정책 `testing-macro`·TestAccess 규칙, 14.3절).
- PlatformBase 내부 OS 잠금(SRWLOCK·`pthread_mutex_t`·`pthread_cond_t` wrapper)은 없앤다. macOS `WaiterSignal` 구현 안의 조건 변수만 남는다.
- 헤더 분할은 SY6에서 한다. SY2는 기존 `Platform/Sync.h` 안의 같은 이름 타입을 제자리에서 새 구현으로 바꾼다(15절, D28).

## 8. 잠금

### 8.1 Mutex

```cpp
namespace rocks::platform {

// 1 byte 비재진입 잠금이다. 상수 초기화되고 파괴자가 없어 constinit 전역으로 둘 수 있다.
// 공정하지 않다: 풀린 순간 먼저 CAS한 스레드가 잡는다(기다리던 스레드가 밀릴 수 있다).
//
// 사용 예:
//   constinit Mutex registryMutex;
//   { LockGuard lock(registryMutex); Insert(entry); }
//   if (registryMutex.TryLockFor(1000)) { ...; registryMutex.Unlock(); }
class Mutex {
public:
    constexpr Mutex() noexcept = default;
    explicit constexpr Mutex(InitiallyLockedTag) noexcept;
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void Lock() noexcept;
    bool TryLock() noexcept;
    // timeoutMs 안에 잡으면 true다. 0은 TryLock과 같다. InfiniteTimeout은 Lock과 같다.
    bool TryLockFor(uint32 timeoutMs) noexcept;
    void Unlock() noexcept;
    bool IsLocked() const noexcept;   // 진단용. 판단 근거로 쓰지 않는다

private:
    Atomic<uint8> state_{};           // bit 0 잠김, bit 1 대기자 있을 수 있음
};

} // namespace rocks::platform
```

- **알고리즘.** UE `TIntrusiveMutex`와 같다(3.3절). 빠른 경로는 CAS 하나(잠금)·`fetch_sub` 하나(풀기)다. 대기 bit가 없을 때만 `CpuPause()`로 최대 40번 돈다. 그 뒤 대기 bit를 세우고 `ParkingLot::Wait(&state_, CanWait = "잠김이고 대기 bit가 있다")`로 잔다. 풀기는 대기 bit가 있었을 때만 `WakeOne`하고, 깨운 스레드가 없을 때만 그 `WakeOne`의 콜백(bucket 잠금 아래)에서 대기 bit를 지운다. 대기 표시 규칙(10.3절)의 기준 사례다.
- **공정성.** 불공정(D3). 굶주림은 경합 trace로 관찰한다(13.3절). WebKit식 "가끔 직접 넘겨주기"는 두지 않는다.
- **시한 잠금.** `TryLockFor`는 `ParkingLot::WaitUntil`로 만든다. 4.5절 2번의 반복 TryLock을 대체한다.
- **계약.** 잠근 스레드가 푼다. 예외는 `InitiallyLocked`로 만든 잠금의 첫 해제다. 잠긴 채 만들어 다른 스레드에 넘기는 용도(UE 작업 선행 목록, 3.3절)라 그 해제는 보유 스택 검사에서 뺀다(13.2절). 같은 스레드의 재잠금은 교착이다(Debug는 보유 스택으로 잡아 Fatal, 13.2절). 잠긴 채 파괴하는 것은 계약 위반이다(Debug는 파괴 대신 쓰는 `AssertUnlocked()`를 소유자가 부를 수 있게 둔다. 파괴자는 trivially destructible을 위해 비운다).
- **`IsLocked`.** 진단·assert용이다. 다른 스레드에서 읽은 값은 곧 낡는다.

### 8.2 IntrusiveMutex

```cpp
// 호출자의 Atomic<StateType> 안 2 bit로 만드는 잠금이다. 슬롯 상태 word, 해시 칸처럼 이미 원자 정수가
// 있는 자료에 잠금을 따로 두지 않고 넣는다. Params는 LockedBit·WaitingBit(각 1 bit), 선택으로
// LockedMask·SpinLimit·WaitAddress를 정한다.
//
// 사용 예:
//   struct SlotBits { static constexpr uint32 LockedBit = 1u << 30; static constexpr uint32 WaitingBit = 1u << 31; };
//   IntrusiveMutex<SlotBits>::Lock(slot.state);
template <class Params>
class IntrusiveMutex {
public:
    static void Lock(Atomic<StateType>& state) noexcept;
    static bool TryLock(Atomic<StateType>& state) noexcept;
    static void Unlock(Atomic<StateType>& state) noexcept;
    static bool IsLocked(const Atomic<StateType>& state) noexcept;
};
```

- `Mutex`·`ReadWriteMutex` 내부 구현도 이것이다(UE와 같다).
- 쓰임: 1M 객체 규모에서 객체마다 잠금 byte를 더하지 않고 기존 상태 word에 넣는다. GC 규모 작업의 "잠금 제거"는 GC 쪽 결정이지만, 남는 슬롯 단위 잠금이 생기면 이 형태로 둔다.
- `IntrusiveMutex`를 쓰는 상태 word의 나머지 bit를 다른 CAS로 바꾸는 코드는 잠김·대기 bit를 보존해야 한다. Params 주석과 static_assert(bit 겹침 없음)로 막는다.

### 8.3 RecursiveMutex

- 16 byte: 상태 32 bit(대기 bit + 잠금 횟수 31 bit) + 소유 스레드 ID 64 bit. UE는 8 byte지만 Rocks `CurrentThreadId()`가 64-bit다(`Platform/Clock.h` 48행).
- 같은 스레드의 `Lock`은 횟수만 올린다. 첫 잠금·마지막 풀기는 `Mutex`와 같은 경로다. 횟수 넘침은 Fatal이다.
- `IsLockedByCurrentThread()`를 둔다.
- **언제 쓰는가.** Rocks 코드는 지금까지 비재진입으로 설계됐다. 재진입 잠금은 "콜백이 같은 객체를 다시 부를 수 있고 콜백 계약을 바꿀 수 없는" 경우에만 쓴다. 쓰는 곳은 선언 옆 주석에 이유를 적는다(D6). UE `FCriticalSection`(760곳)을 옮길 때 기계적으로 재진입 잠금으로 옮기지 않는다.
- `ConditionVariable`과 짝짓지 않는다(재진입 횟수를 풀 수 없다).

### 8.4 ReadWriteMutex

```cpp
// 4 byte 읽기쓰기 잠금이다. 읽기는 여럿이 함께, 쓰기는 혼자 쥔다. 쓰기 우선이다: 쓰기를 기다리는
// 스레드가 있으면 새 읽기는 기다린다(읽기가 끊이지 않아도 쓰기가 굶지 않는다).
// 재진입하지 않는다. 읽기를 쥔 스레드가 다시 LockRead하면 안 된다: 그 사이 쓰기 대기자가 생기면
// 두 번째 읽기가 쓰기를 기다리고 쓰기는 첫 읽기를 기다려 교착한다(쓰기 대기가 없을 때는 지나가므로
// 드물게만 드러난다. Debug는 늘 Fatal로 잡는다). 한 스레드가 읽기와 쓰기를 함께 쥐어도 교착이다.
// 읽기 → 쓰기 승격은 없다: 풀고 다시 잡은 뒤 상태를 다시 검사한다.
//
// 사용 예:
//   ReadWriteMutex tableMutex;
//   { ReadLockGuard read(tableMutex); value = Find(key); }
//   { LockGuard write(tableMutex); Insert(key, value); }
class ReadWriteMutex {
public:
    constexpr ReadWriteMutex() noexcept = default;
    void Lock() noexcept;                // 쓰기
    bool TryLock() noexcept;
    bool TryLockFor(uint32 timeoutMs) noexcept;
    void Unlock() noexcept;
    void LockRead() noexcept;
    bool TryLockRead() noexcept;
    bool TryLockReadFor(uint32 timeoutMs) noexcept;
    void UnlockRead() noexcept;

private:
    Atomic<uint32> state_{};  // bit 0 쓰기 잠김, bit 1 쓰기 대기, bit 2 읽기 대기, bit 3~ 읽기 수
};
```

- **알고리즘.** UE `FSharedMutex`와 같다(3.5절). 읽기 대기자는 `&state_`와 다른 키 주소(`&state_`에서 1 byte 떨어진 주소를 키로만 쓴다. 그 메모리를 읽지 않는다)에서 잔다. 쓰기 대기자만 골라 깨운다. 마지막 읽기 해제가 쓰기 대기자 하나를 깨운다. 쓰기 해제는 쓰기 대기자가 있으면 하나, 없으면 읽기 대기자 전부를 깨운다. 읽기 대기 bit의 지우기는 콜백 있는 `WakeAll`(10.1절)의 bucket 잠금 아래에서 한다(10.3절).
- **읽기 재진입 금지.** 계약에 적고 Debug 보유 스택이 같은 `ReadWriteMutex`의 읽기를 같은 스레드가 두 번 잡으면 쓰기 대기 여부와 관계없이 Fatal한다(13.2절, D7). 4.4절 후보(형 등록·이름 풀·지역화 목록)는 옮기기 전에 보호 구간 안의 콜백이 같은 표를 다시 읽는지 확인한다.
- **읽기 수 상한.** 29 bit(약 5억)다. 넘침은 Fatal이다.
- **이름.** 사용자 말("ReadWriteLock")과 용도를 그대로 드러내도록 `ReadWriteMutex`, 읽기 연산은 `LockRead`·`UnlockRead`다. 쓰기는 `Lock`·`Unlock`이라 `LockGuard`·`LockOwner`를 그대로 쓴다(D7).
- **언제 쓰는가.** 읽기가 대부분이고 보호 구간이 수 μs 이상인 표다. 짧은 구간에서는 읽기 수 카운터 하나에 모든 읽기 스레드가 몰려 `Mutex`보다 느릴 수 있다. 4.4절의 후보는 측정 뒤 바꾼다.
- **재진입 읽기잠금(UE `FSharedRecursiveMutex`)은 두지 않는다.** 스레드별 소유 목록이 필요하고(3.5절) 쓰기 대기 중 재진입 읽기를 따로 허용해야 해서 복잡하다. 필요한 곳이 생기면 그때 더한다(D6).

### 8.5 spin 잠금을 두지 않는 이유

- UE `FSpinLock`·`TRWSpinLock`의 쓰임(5·4곳)은 "정적 초기화 전에도 쓸 수 있는 작은 잠금"이다. Rocks `Mutex`는 constinit이고 1 byte이며 짧은 경합에는 어차피 40번 spin한다. 같은 역할을 하면서 잠든 스레드가 CPU를 태우지 않는다.
- `TRWSpinLock`은 쓰기가 굶는다(3.9절).
- 그래서 공개 spin 잠금은 두지 않는다(D8). 4.5절 1번의 두 곳은 `Mutex`로 바꾼다. 예외는 ParkingLot 내부(`detail::QueueMutex`)뿐이다.
- 잠금이 아닌 "값이 바뀔 때까지 짧게 도는" 반복(예: `BoundedMpscQueue` 소비자의 미공개 칸, 4.5절 12번)은 `SpinBackoff`(11.2절)를 쓴다. `CpuPause()`를 정해진 횟수(기본 40, 잠금 spin과 같다)만 하고, 그 뒤로는 반복마다 `YieldThread()`로 넘어간다. `CpuPause`만으로 돌면 상대(예: 예약 뒤 공개 전에 선점된 생산자, `Core/Tests/BoundedMpscQueueTests.cpp` 141행 시험의 상황)가 같은 코어에서 멈춰 있을 때 CPU를 태우며 상대의 실행도 막는다. 오래 걸릴 수 있는 기다림(공간이 날 때까지 등, 4.5절 13번)은 반복이 아니라 `EventCount` 대기로 한다.

### 8.6 ConditionVariable

- UE 5.8 `Async`에는 조건 변수가 없다(UE는 `FEventCount`를 쓴다). Rocks는 기존 사용처 9곳(4.4절, `WorkerScheduler`·`LogSystem`·`BudgetAllocator`·`ExternalAllocator`·`KeyRing`·`FenceTimeline`·`RenderLane`·`DedicatedLane`·`TraceSessionInternal`)이 "잠금 아래 상태 + 기다림"으로 짜여 있어 유지한다(D9).
- 1 byte(대기자 있음 bit)다. `Mutex`와만 짝짓는다.
- `Wait(mutex, timeoutMs)`: `ParkingLot::Wait(&state_, CanWait = { 대기 bit를 FetchOr로 세운다; true }, BeforeWait = mutex.Unlock())`로 잔다. 깨면 `mutex.Lock()`한다.
  - 대기 bit는 bucket 잠금 아래의 `CanWait` 안에서 세운다. 그래서 "bit를 세움"과 "큐에 들어감"이 bucket 잠금 하나 안에서 함께 일어나고, 깨우는 쪽 콜백이 본 큐 상태와 bit가 어긋나지 않는다(10.3절 규칙 1을 "밖에서 세우지 않음"으로 지킨다).
  - `mutex`를 푸는 것은 대기 스레드가 bucket 큐에 들어간 뒤(`BeforeWait`)다. 그래서 푼 뒤 `mutex`를 잡고 predicate를 바꾼 스레드는 그 뒤 bit를 반드시 본다(bit 설정 → `mutex` 해제 → 상대의 `mutex` 획득으로 이어지는 happens-before). 알림을 놓치지 않는다. 이것이 UE가 `BeforeWait`를 둔 이유다(`PL` 52–54행).
- `NotifyOne`·`NotifyAll`: 대기 bit를 relaxed로 읽어 있을 때만 콜백 있는 `WakeOne`·`WakeAll`(10.1절)을 부른다. 콜백(bucket 잠금 아래)이 "이 bucket에 남은 대기자가 없을 수 있다"(`mayHaveMoreWaiters == false`)를 받으면 bit를 지운다. bucket 안에 다른 주소의 대기자가 있으면 bit가 남는다. 그것은 다음 `Notify`가 빈 깨우기를 한 번 더 하는 비용일 뿐이다.
- **고친 경쟁(개정 1의 결함).** 개정 1은 bit를 bucket 밖에서 세우고 `CanWait = true`로 두었다. 그러면 이런 경로가 생긴다. W1이 잔다. N이 `mutex` 아래에서 W1의 조건을 바꾸고 푼다. W2가 `mutex`를 잡고 bit를 세운 뒤 bucket에 들어가기 직전이다. N의 `NotifyAll`이 W1만 깨우고 bucket이 비었다고 bit를 지운다. W2가 들어가 잔다. 다음 `Notify`는 bit가 없어 건너뛰고 W2는 영구히 잔다. Rocks는 `mutex`를 푼 뒤 알린다(`Task/Private/WorkerScheduler.cpp` 223→227·317→322·584→592·701→724행). 그래서 이 경로가 실제로 생긴다.
- **대기자 수 방식과 비교.** 4 byte 대기자 수(대기 전 `CanWait`에서 올리고 깬 뒤 대기자가 내린다)도 맞다. 지우기 규약이 없어 더 단순하지만 크기가 4배이고, 내리기가 bucket 밖이라 "수 > 0인데 큐가 빔" 상태가 생겨 빈 깨우기는 같이 생긴다. bit를 `CanWait` 안에서 세우면 1 byte로 같은 정확성을 얻고 다른 타입과 같은 규칙(10.3절)을 쓴다. 그래서 bit 방식으로 정한다(D9).
- 계약: predicate는 짝 `mutex`를 쥔 채 바꾼다. `Notify`는 `mutex` 밖에서 불러도 된다. spurious wake가 있을 수 있어 predicate를 반복 검사한다. 유한 대기는 최초 deadline 기준이다.

### 8.7 가드

| 가드 | 뜻 |
|---|---|
| `LockGuard<L>` | 생성 때 `Lock`, 소멸 때 `Unlock`. CTAD로 `LockGuard lock(mutex);` |
| `ReadLockGuard<L>` | `LockRead`·`UnlockRead` |
| `LockOwner<L>` | `LockStart::Now`·`Deferred`·`Try`로 만든다. `OwnsLock()`, `Lock()`, `Unlock()`(이른 해제), 이동 가능. 복사 불가 |
| `ReadLockOwner<L>` | 읽기판 |
| `UnlockGuard<L>` | 생성 때 `Unlock`, 소멸 때 `Lock`. 보호 구간 안에서 사용자 콜백을 잠금 없이 부를 때 쓴다 |

- 가드는 잠금을 소유하지 않는다. 잠금이 가드보다 오래 살아야 하고 같은 스레드에서 소멸한다.
- 기존 `platform::LockGuard`(비템플릿)는 템플릿으로 바꾼다. CTAD 덕분에 호출 모양은 같다.

## 9. 신호

### 9.1 ManualResetEvent와 AutoResetEvent

| | `ManualResetEvent` | `AutoResetEvent` |
|---|---|---|
| 크기 | 1 byte(신호·대기자 bit) | 4 byte(신호·대기자 bit) |
| `Signal()` | 신호를 세우고 대기자 전부를 깨운다 | 대기자가 있으면 하나를 깨우고 신호를 넘긴다. 없으면 신호를 세운다(최대 1) |
| `Wait(timeoutMs)` | 신호가 서 있으면 바로 통과. 신호를 지우지 않는다 | 신호 하나를 소비한다 |
| `Reset()` | 신호를 지운다 | 신호를 지운다 |
| `IsSignaled()` | 있음 | 없음(읽는 순간 소비 여부가 갈리므로) |

- 둘 다 constexpr·trivially destructible이다. 풀·가상 인터페이스·OS 핸들이 없다(2.3절).
- `AutoResetEvent`는 Windows auto-reset 이벤트와 같은 뜻이다. 신호는 쌓이지 않는다(쌓여야 하면 `Semaphore`).
- **대기 표시(10.3절).**
  - `ManualResetEvent`: 대기 쪽은 UE와 같다(3.7절). 대기자가 bit를 bucket 밖에서 세우고 `CanWait`가 "신호 없음이고 대기 bit 있음"을 다시 확인한다. `Signal`은 UE와 조금 다르다. UE는 exchange로 신호를 세우며 대기 bit를 함께 지운다(`MRE` 85–88행). Rocks는 `FetchOr`로 신호만 세우고, 대기 bit가 있었으면 콜백 있는 `WakeAll`을 부르고 그 콜백에서 대기 bit를 지운다. 비용은 같고 지우기 자리가 규칙 2와 맞는다.
  - `AutoResetEvent`: 대기자는 신호가 있으면 CAS로 소비하고 돌아간다. 없으면 대기 bit를 bucket 밖에서 세우고 `CanWait`가 "신호 없음이고 대기 bit 있음"을 다시 확인한다. `Signal`은 대기 bit가 있으면 콜백 있는 `WakeOne`을 부른다. 콜백(bucket 잠금 아래)은 깨운 스레드가 있으면 wake token으로 신호를 넘기고(신호 bit는 세우지 않는다), 남은 대기자가 없으면 대기 bit를 지운다. 깨운 스레드가 없으면 신호 bit를 세우고 대기 bit를 지운다.
- 쓰임: `ManualResetEvent`는 시작·정지 표시(`DedicatedLane`), 시험 gate(14.2절). `AutoResetEvent`는 "깨울 일이 있으면 한 번 깨운다"는 단일 소비자 깨움이다. 여러 생산자가 조건과 함께 깨우는 곳은 `EventCount`를 쓴다.

### 9.2 EventCount

```cpp
// 알림 횟수로 놓친 알림을 막는 이벤트다. 기다리는 쪽은 token을 먼저 받고 조건을 검사한다.
// token 뒤에 Notify가 있었으면 Wait가 바로 돌아온다. Notify는 기다리려는 스레드가 있을 때만
// 비싼 경로를 탄다.
//
// 사용 예(로그 출력 스레드):
//   for (;;) {
//       EventCountToken token = wake.PrepareWait();
//       if (!queue.IsEmpty() || stopping.Load<MemoryOrder::Acquire>()) break;
//       wake.Wait(token);
//   }
//   // 생산자: queue.TryPush(...); wake.Notify();
class EventCount {
public:
    EventCountToken PrepareWait() noexcept;
    void Wait(EventCountToken token) noexcept;
    bool WaitFor(EventCountToken token, uint32 timeoutMs) noexcept;  // 알림이면 true
    void Notify() noexcept;      // StoreLoad 순서 포함
private:
    Atomic<uint32> count_{};     // bit 0 기다리려는 스레드 있음, 나머지 알림 횟수
};
```

- UE `TEventCount<uint32>`와 같은 알고리즘이다(3.7절). `Notify`의 StoreLoad 순서는 UE와 같이 `FetchAdd<MemoryOrder::AcquireRelease>(0)`로 만든다(UE 주석의 교착 사례 때문에 relaxed load로 줄이지 않는다). 대기자의 `PrepareWait`도 RMW(acquire 포함)이므로 두 RMW가 `count_`의 수정 순서 안에서 서로를 본다. 그래서 순차 일관 순서까지는 필요 없다.
- 대기 표시: bit 0은 `PrepareWait`가 bucket 밖에서 세우고 `Notify`가 횟수를 올리는 CAS로 지운다. `Wait`의 `CanWait`가 "횟수가 token과 같다"를 다시 확인하므로, 지워진 뒤 들어온 대기자는 자지 않는다. 횟수 변화 자체가 `CanWait`를 거짓으로 만드는 경우라 10.3절 규칙 2의 예외(2b)다.
- `NotifyWeak`(장벽 생략판)는 두지 않는다. 호출자가 순서를 책임지는 API는 틀리기 쉽고, 이득은 x86 한 명령이다(D11).
- 32-bit 횟수는 PrepareWait와 Wait 사이에 2^31번 알림이 있어야 같은 값으로 돌아온다. 현실적으로 일어나지 않는다(UE와 같은 판단).
- 쓰임: `LogSystem`의 `sleeping_`+fence+`ManualResetEvent`(4.5절 6번)를 대체한다. `LogOverflow::WaitForSpace` 생산자의 바쁜 대기(4.5절 13번)도 대체한다. 소비자가 칸을 비운 뒤 "공간 생김" `EventCount`를 `Notify`하고, 생산자는 `PrepareWait` → `TryPush` 재시도 → `Wait`로 잔다. worker 잠듦·작업 완료 대기·파이프 비움 대기에 쓴다.

### 9.3 Semaphore

- 8 byte다. 상태 word 4 byte(bit 0 대기자 있음, bit 1–31 허가 수)와 최대값 4 byte(생성 뒤 불변)다. `Semaphore(initialCount, maximumCount)`, `Release(count = 1)`, `Wait(timeoutMs)`.
- **최대값 범위.** 허가 수가 31 bit라 `maximumCount`는 1 이상 2^31 − 1(`NumericLimits<int32>::Max()`) 이하다. 지금 구현(`uint32` 전체 허용)보다 좁아진다. 넘는 값은 생성 전제 위반으로 Fatal이다(하위호환 없음).
- 그 밖의 뜻은 지금대로다. `initial <= maximum`이 전제(위반 Fatal). `Release(0)`과 최대값 초과는 false이고 아무것도 바꾸지 않는다. UE는 최대값의 뜻이 플랫폼마다 다르지만(3.8절) Rocks는 공용 구현이라 같다.
- **대기 표시(10.3절).** 대기자는 허가가 없으면 대기 bit를 bucket 밖에서 세우고, `CanWait`가 "허가 0이고 대기 bit 있음"을 다시 확인한다. `Release(n)`은 허가를 CAS로 더한 뒤 대기 bit가 있었으면 콜백 있는 `WakeMultiple(n)`을 부르고, 콜백(bucket 잠금 아래)이 남은 대기자가 없을 때 대기 bit를 지운다. 깬 대기자는 허가를 CAS로 다시 가져간다. 공정성은 보장하지 않는다.

### 9.4 Once

```cpp
// 한 번만 실행하는 초기화다. 여럿이 동시에 불러도 함수는 한 번만 돌고, 나머지는 그 결과를 기다린다.
//
// CallOnce는 실패도 기록한다: 함수가 false를 돌려주면 이후 호출은 다시 실행하지 않고 false를
// 돌려받는다. 한 작업(job) 수명의 flag처럼 "실패를 모두가 같은 결과로 봐야 하는" 곳에 쓴다.
// CallOnceUntilSuccess는 실패를 기록하지 않는다: 함수가 false면 flag를 미실행으로 되돌리고 기다리던
// 스레드 중 하나가 다시 실행한다. 일시적일 수 있는 실패(할당·파일 열기)를 가진 constinit 전역에 쓴다.
// 한 flag에는 한 함수만 쓴다(둘을 섞어 부르면 Debug Fatal).
//
// 사용 예:
//   OnceFlag readerOpened;   // 작업 레코드 안
//   if (!CallOnce(readerOpened, [&]() noexcept { return OpenReader(job) == ErrorCode::None; })) { ... }
//   constinit OnceFlag tableReady;
//   if (!CallOnceUntilSuccess(tableReady, [&]() noexcept { return BuildTable(); })) { ... }
class OnceFlag { Atomic<uint8> state_{}; };   // 0 미실행, 1 실행 중, 2 성공, 3 실패, bit 7 대기자
template <class Function>
bool CallOnce(OnceFlag& flag, Function&& function) noexcept;
template <class Function>
bool CallOnceUntilSuccess(OnceFlag& flag, Function&& function) noexcept;
// 성공으로 끝났으면 true다(실패로 끝났거나 실행 전·중이면 false).
bool IsDone(const OnceFlag& flag) noexcept;
```

- `Reset`은 두지 않는다. 동시 호출자가 없음을 타입이 보장할 수 없어서다. 다시 시도는 `CallOnceUntilSuccess`가 맡는다.
- 실행 중인 스레드가 같은 flag로 다시 부르면 교착이다. Debug는 실행하는 동안 그 flag 주소를 보유 스택(13.2절)에 종류 `Once`로 올려 두고, 같은 주소를 다시 만나면 Fatal로 잡는다. 1 byte flag 안에 스레드 ID를 둘 자리는 쓰지 않는다.
- 대기 표시(10.3절): 대기자는 bit 7을 bucket 밖에서 세우고 `CanWait`가 "실행 중이고 bit 7 있음"을 다시 확인한다. 실행한 스레드는 bit 7을 보존한 채 결과 상태를 release CAS로 쓴 뒤 bit 7이 있었으면 콜백 있는 `WakeAll`을 부르고, 콜백에서 bit 7을 지운다. `CallOnceUntilSuccess`의 실패는 상태를 0으로 되돌린 뒤 같은 방식으로 깨운다.
- 실패를 기억하는 이유: 4.5절 9번(`AsyncCrypto`)처럼 "열기 실패"를 모든 대기자가 같은 결과로 봐야 하는 곳이 있다. 재시도가 필요한 곳은 `CallOnceUntilSuccess`를 고른다(D13).
- "경쟁에서 진 쪽이 버리는" 지연 생성(렌더 셰이더, `RenderBase.ko.md` 694행 SX10·1101행)은 Once가 아니라 `Atomic<T*>::CompareExchangeStrong`이다. 생성이 비싸거나 부작용이 있으면 Once를 쓴다.

### 9.5 대기 시간과 결과

- 시간 인자는 지금처럼 millisecond `uint32`이고 `InfiniteTimeout`이 무한, 0이 즉시 검사다. ParkingLot 안쪽은 monotonic tick 시한으로 바꿔 spurious wake 때 시간을 늘리지 않는다(`WaitDeadline.h`와 같은 규칙).
- `WaitResult`는 `Signaled`·`TimedOut` 둘로 줄인다. OS 대기 수단의 예상 밖 실패는 이제 `WaiterSignal` 한 곳뿐이고, 거기서 다시 시도할 수 없는 오류는 Fatal이다(D14). 하위호환 없이 `Failed`를 지운다.

### 9.6 RealtimeWakeSignal(개정 3)

실시간 스레드와 그 스레드를 깨우는 쪽 사이의 신호다. 첫 사용처는 [오디오 시스템](AudioSystem.ko.md) 3.2절의 장치 콜백 → AudioRT 깨우기다. 장치 콜백은 OS·오디오 라이브러리가 소유한 높은 우선순위 스레드이고, AudioRT는 잠금을 잡지 않는다(D27). 9.1절 `AutoResetEvent`는 parking lot 위에 있어 Signal이 bucket 잠금을 잡을 수 있고, `WaiterSignal`의 macOS판은 `pthread_mutex_t`를 잡는다(7.3절). 그래서 둘 다 이 자리에 쓸 수 없다. 지금의 `ManualResetEvent`·`Semaphore`도 Signal·Release 안에서 `Mutex`를 잡는다(4.1절).

- **모양.** 대기자는 한 번에 하나다(고정 스레드는 아니다). 둘이 동시에 기다리면 계약 위반이고 Debug·Development에서 Fatal로 드러낸다. 자동 리셋이다. 상태는 `Atomic<uint32>` 하나다: 0 = 비어 있음, 1 = 신호, 2 = 대기 중.
- **`Signal() noexcept`.** 상태를 1로 교환하고, 이전 값이 2였을 때만 OS 깨우기를 부른다. 잠금·할당이 없고, 대기자가 없으면 시스템 호출도 없다. 연속 Signal은 신호 하나로 합쳐진다.
- **`Wait(timeoutMs) noexcept -> WaitResult`.** 상태가 1이면 0으로 바꾸고 바로 돌아간다. 0이면 2로 바꾸고 OS 대기(값이 2인 동안 잠듦)에 들어가며, 깬 뒤 다시 확인한다. 시한·spurious wake 규칙은 9.5절과 같다.
- **OS 수단.** Windows `WaitOnAddress`·`WakeByAddressSingle`, Android futex(7.3절과 같다). macOS는 신호 객체마다 Mach semaphore 하나(`semaphore_create`·`semaphore_signal`·`semaphore_timedwait`)다. `semaphore_signal`은 사용자 공간 잠금을 잡지 않는 커널 호출이라 Core Audio 콜백에서 쓰는 관례적 수단이다. `os_sync_wait_on_address`는 최소 버전 13.0에서 쓸 수 없다(7.3절). Mach semaphore는 커널 객체라 만들기가 실패할 수 있으므로 생성은 `Result<RealtimeWakeSignal> Create() noexcept`다(Windows·Android는 실패하지 않는다).
- **위치.** PlatformBase `Platform/Sync/RealtimeWakeSignal.h`, OS 구현은 `Private/<OS>/`. 가짜 OS 대기 seam(14.2절)을 같이 쓴다.
- **시험.** Signal 뒤 Wait, 대기 중 Signal, 연속 Signal 합침, 시한, 두 대기자 검출을 가짜 OS 대기로 결정적으로 확인한다.
- **UE와 다른 점.** UE 오디오 렌더 스레드는 `FEvent`(Apple은 pthread 조건 변수)를 기다린다(`Engine/Source/Runtime/AudioMixerCore/Private/AudioMixer.cpp`). Rocks는 D27 때문에 잠금 없는 신호로 둔다.

## 10. ParkingLot

### 10.1 API

```cpp
class ParkingLot {
public:
    struct WaitOutcome { bool waited; bool woken; uint64 wakeToken; };
    struct WakeOutcome { bool wokeThread; bool mayHaveMoreWaiters; };

    // address 큐에 들어가 잔다. canWait는 bucket을 잠근 채 부르고 false면 자지 않는다.
    // beforeWait는 bucket을 푼 뒤 잠들기 직전에 부른다(깨우기 함수만 부를 수 있다).
    template <class CanWait, class BeforeWait>
    static WaitOutcome Wait(const void* address, CanWait&& canWait, BeforeWait&& beforeWait,
                            uint32 timeoutMs = InfiniteTimeout) noexcept;
    template <class OnWake>   // uint64 (WakeOutcome), bucket을 잠근 채 부른다
    static void WakeOne(const void* address, OnWake&& onWake) noexcept;
    static WakeOutcome WakeOne(const void* address) noexcept;
    // 최대 count개를 깨운다. onWake는 꺼내기를 마친 뒤 bucket을 잠근 채 한 번 부른다
    // (void (WakeOutcome), wokeThread는 하나라도 깨웠는지). 대기 표시를 지우는 자리다(10.3절).
    template <class OnWake>
    static uint32 WakeMultiple(const void* address, uint32 count, OnWake&& onWake) noexcept;
    template <class OnWake>
    static uint32 WakeAll(const void* address, OnWake&& onWake) noexcept;
    static uint32 WakeMultiple(const void* address, uint32 count) noexcept;
    static uint32 WakeAll(const void* address) noexcept;
};
```

- 콜백은 템플릿으로 받아 함수 포인터 + context로 낮춘다(PlatformBase는 Core `Function`을 쓰지 못한다).
- 콜백 있는 `WakeMultiple`·`WakeAll`은 UE에 없다(3.3절). 대기 표시를 bucket 잠금 아래에서 지우려고 더한다(D29).
- 콜백 안에서 ParkingLot 함수를 부르면 Debug Fatal이다. 대기 함수는 `BeforeWait`에서도 금지이고, 깨우기 함수는 `BeforeWait`에서 허용한다(UE `PLC` 744–747·913·971행과 같다).

### 10.2 구현

- **스레드 레코드.** cache line 정렬, `WaiterSignal` 하나, 대기 주소, 다음 포인터, wake token, 참조 계수. 처음 기다릴 때 `thread_local` 칸에 만든다. 깨우는 쪽이 레코드를 잡은 동안 그 스레드가 끝나도 되도록 참조 계수로 수명을 잡는다. 끝난 스레드의 레코드는 영구 풀의 free list로 돌아가 다음 스레드가 쓴다.
- **TLS 파괴 뒤의 대기(D31).** 스레드 종료 중 다른 `thread_local` 소멸자가 잠금에서 경합할 수 있다. 예를 들어 `TraceThreadWrite.cpp`의 `thread_local currentSlot` 소멸 → `RetireThread` → slot registry 잠금이 있고, 이 잠금은 SY6 뒤 `Mutex`가 된다. TU 사이 `thread_local` 파괴 순서는 정해져 있지 않다. 그래서 UE와 같이 레코드 칸에 "파괴됨" 표시를 두고(`PLC` 289–292·730–735행), 그 뒤의 대기는 한 번마다 영구 풀에서 임시 레코드를 받아 쓰고 대기가 끝나면 참조 계수로 돌려준다. 칸을 다시 만들지 않는다(다시 만들면 소멸자가 돌지 않아 레코드가 샌다).
- **표.** bucket 포인터 배열, bucket은 cache line 정렬 + `detail::QueueMutex` + 대기열 머리·꼬리. 처음 ParkingLot을 쓰는 스레드가 살아 있는 사용 스레드 수로 표 크기를 확인하고, 넘으면 2의 거듭제곱으로 키운다(최소 32, 모든 bucket을 주소 순으로 잠근 뒤 옮긴다). 옛 표는 해제하지 않는다. 스레드 수 N이면 표 메모리 합은 약 2N 포인터로 유한하다.
- **메모리(D5).** 가상 메모리 예약(`Platform/VirtualMemory.h`의 OS 함수)에서 선형으로 잘라 쓰는 영구 할당 하나뿐이다. `VirtualMemory` 클래스(멤버에 `Mutex`가 있다)를 거치지 않는다. 할당기 잠금도 `QueueMutex`라 ParkingLot이 자기 할당 중에 다시 불리지 않는다.
  - 예약 크기는 UE와 같은 4 MiB + 64 KiB다. UE 주석의 계산(bucket·레코드 64 byte씩, 표 약 2N 포인터, 16,384 스레드여도 2.25 MiB, `PLC` 59–61행)이 그대로 맞으므로 넘침은 Fatal로 둔다.
  - UE의 scratch 할당기(3.1절)는 두지 않는다. 깨울 스레드와 표를 키울 때 옮길 스레드는 레코드의 다음 포인터로 지역 침투 목록에 엮어 모은다. 레코드는 큐에서 꺼낸 순간 그 목록 하나에만 있으므로 다음 포인터를 다시 써도 된다. 임시 배열이 없으니 할당도 없다.
- **`detail::QueueMutex`.** UE `FWordMutex`와 같은 포인터 크기 잠금이다(3.2절). 대기 노드는 대기 스레드 스택에 있고 노드마다 `WaiterSignal`이 있다. ParkingLot 밖에서는 쓰지 않는다.
- **해시.** 주소를 64-bit 곱셈 해시로 섞는다. 같은 cache line의 서로 다른 잠금이 다른 bucket으로 가야 한다.
- **진단 훅 자리.** 10.4절에서 정한다. 빠른 경로(경합 없음)에는 들어가지 않는다.

### 10.3 대기 표시 규칙(D29)

잠금·신호 타입은 상태 word에 "대기자가 있을 수 있음" 표시를 둔다. 깨우는 쪽은 이 표시가 있을 때만 ParkingLot을 부른다. 표시를 세우는 자리와 지우는 자리가 어긋나면 깨움을 잃는다(8.6절의 개정 1 결함). 그래서 모든 타입이 다음 규칙을 따른다.

1. **밖에서 세운 표시는 `CanWait`에서 다시 확인한다.** bucket 밖에서 표시를 세운 대기자는 `CanWait`(bucket 잠금 아래)에서 "표시가 아직 있고 기다릴 조건이 아직 참"을 다시 본다. 아니면 자지 않고 처음부터 다시 시도한다. 표시를 `CanWait` 안에서 세우면(조건 변수) 이 규칙은 저절로 지켜진다.
2. **지우기는 bucket 잠금 아래 콜백에서만 한다.** 깨우는 쪽은 `WakeOne`·`WakeMultiple`·`WakeAll`의 콜백에서 "남은 대기자가 없을 수 있음"을 받았을 때만 지운다. 그래야 "큐가 비었다"는 판단과 지우기 사이에 새 대기자가 끼지 못한다.
   - 2b(예외). 지우는 원자 연산이 같은 순간 모든 대기자의 `CanWait`를 거짓으로 만드는 값 변화를 함께 하면 밖에서 지워도 된다. 지운 뒤 들어온 대기자는 규칙 1로 자지 않기 때문이다. UE 그대로인 `EventCount`(횟수 증가와 함께 bit 0을 지운다)만 이 경우다.
3. **"있을 수 있음"이다.** 콜백의 "남은 대기자"는 bucket 단위라 다른 주소의 대기자 때문에 표시가 남을 수 있다. 남은 표시는 다음 깨우기가 한 번 빈 bucket 확인을 하는 비용일 뿐 정확성 문제가 아니다.

| 타입 | 표시 | 세우는 곳 | `CanWait` 확인 | 지우는 곳 |
|---|---|---|---|---|
| `Mutex`·`IntrusiveMutex`·`RecursiveMutex` | 대기 bit | 밖(CAS) | 잠김이고 bit 있음 | `WakeOne` 콜백(깨운 스레드 없음) |
| `ReadWriteMutex` | 쓰기 대기·읽기 대기 bit | 밖(CAS) | 쓰기: 잠김(쓰기 또는 읽기 수 > 0)이고 bit 있음. 읽기: 상태가 읽은 값과 같음(`SMC` 77–80행) | 쓰기 bit는 `WakeOne` 콜백, 읽기 bit는 `WakeAll` 콜백 |
| `ConditionVariable` | 대기 bit | `CanWait` 안 | (안에서 세움) | `WakeOne`·`WakeAll` 콜백 |
| `ManualResetEvent` | 대기 bit | 밖(CAS) | 신호 없고 bit 있음 | `WakeAll` 콜백 |
| `AutoResetEvent` | 대기 bit | 밖(CAS) | 신호 없고 bit 있음 | `WakeOne` 콜백 |
| `Semaphore` | 대기 bit | 밖(CAS) | 허가 0이고 bit 있음 | `WakeMultiple` 콜백 |
| `OnceFlag` | bit 7 | 밖(CAS) | 실행 중이고 bit 있음 | `WakeAll` 콜백 |
| `EventCount` | bit 0 | 밖(`PrepareWait`) | 횟수가 token과 같음 | `Notify`의 횟수 증가 CAS(규칙 2b) |
| `ReclaimDomain` | 동기화 대기 bit | 밖(CAS) | 옛 세대 독자 수 > 0이고 bit 있음 | `WakeAll` 콜백 |

경쟁 시험은 14.2절에 있다.

### 10.4 훅 호출 지점(D30)

ParkingLot 대기 경로에는 세 종류의 훅이 있다. 같은 스레드가 enqueue한 뒤 다시 대기하면 대기 상태가 깨진다(UE `PLC` 751행 "WaitAddress must be null… if Wait is called by BeforeWait"). 그래서 호출 지점과 할 수 있는 일을 나눈다.

| 훅 | 부르는 곳 | 할 수 있는 일 |
|---|---|---|
| `onWaitBegin`·`onWaitEnd`(경합 trace·통계, 13.3절) | `onWaitBegin`은 bucket을 잡기 전, `onWaitEnd`는 깨어 레코드의 대기 주소가 비워진 뒤 | 잠금 없는 TLS 버퍼에 쓰기만. trace 세션으로 내보내기는 다음 slow path 밖 지점(trace 블록 공개)에서 한다 |
| `onParked`·`onUnparked`(시험, 14.2절) | `onParked`는 enqueue와 `BeforeWait`가 끝난 뒤 잠들기 직전, `onUnparked`는 깬 직후 | `WaiterSignal` 기반 `TestGate`만. ParkingLot 계열 타입은 쓰지 못한다 |
| 단계 훅(14.3절) | `ROCKS_SYNC_STEP` 지점 | 시험 스케줄러의 `TestGate`만 |

- **재진입 guard.** 스레드 지역 표시 하나를 두고, 훅 안에서 다시 훅이 불리면 훅을 건너뛴다. Debug는 훅 안의 ParkingLot 대기를 Fatal로 잡는다(UE의 재진입 검사와 같은 자리).
- 훅이 설치되지 않았으면 비용은 relaxed load 하나와 분기 하나다.

### 10.5 Task scheduler와의 관계

UE는 잠금 대기에서 oversubscription을 끄고 이벤트 대기에서 켠다(3.3·3.8절). Rocks Task 모듈에는 이 장치가 아직 없다. `onWaitBegin`·`onWaitEnd` 자리가 "이 worker가 잔다"를 알릴 자리가 된다. 켤지는 **Task 설계(후속 문서)** 에서 정한다(D26·D33).

## 11. 원자 값

### 11.1 Atomic<T>

```cpp
namespace rocks::platform {

// 다른 스레드와 공유하는 값 하나다. 모든 접근은 원자적이고 메모리 순서를 늘 템플릿 인자로 적는다
// (기본값이 없다). 연산자 overload(=, ++, 암시적 변환)는 두지 않는다: 순서가 코드에 드러나야 하고,
// 실수로 일반 읽기·쓰기를 섞지 않게 한다.
//
// 사용 예:
//   Atomic<uint64> dropped;
//   dropped.FetchAdd<MemoryOrder::Relaxed>(1);
//   if (refs.FetchSub<MemoryOrder::AcquireRelease>(1) == 1) Destroy();
//   while (ready.Load<MemoryOrder::Acquire>() == 0) { backoff.Pause(); }
template <class T>
class Atomic {
public:
    constexpr Atomic() noexcept = default;          // 0 초기화
    constexpr explicit Atomic(T initial) noexcept;
    Atomic(const Atomic&) = delete;
    Atomic& operator=(const Atomic&) = delete;

    template <MemoryOrder Order> T Load() const noexcept;
    template <MemoryOrder Order> void Store(T value) noexcept;
    template <MemoryOrder Order> T Exchange(T value) noexcept;
    template <MemoryOrder Success, MemoryOrder Failure>
    bool CompareExchangeWeak(T& expected, T desired) noexcept;
    template <MemoryOrder Success, MemoryOrder Failure>
    bool CompareExchangeStrong(T& expected, T desired) noexcept;
    template <MemoryOrder Order> T FetchAdd(T value) noexcept;  // 정수만, 이하 같음
    template <MemoryOrder Order> T FetchSub(T value) noexcept;
    template <MemoryOrder Order> T FetchAnd(T value) noexcept;
    template <MemoryOrder Order> T FetchOr(T value) noexcept;
    template <MemoryOrder Order> T FetchXor(T value) noexcept;

private:
    alignas(sizeof(T)) T value_{};
};

// 이미 있는 T 객체 하나를 원자로 본다. 객체는 AtomicRef보다 오래 살고 T의 자연 정렬을 지켜야 한다
// (Debug는 정렬을 검사한다). 같은 객체를 AtomicRef로 보는 동안 다른 스레드가 일반 접근을 하면 안
// 된다. 멤버와 순서 규칙은 Atomic<T>와 같다.
//
// 사용 예:
//   AtomicRef<uint64>(cell.sequence).Store<MemoryOrder::Release>(next);   // trivially copyable 칸
template <class T>
class AtomicRef {
public:
    explicit AtomicRef(T& object) noexcept;
    // Atomic<T>와 같은 멤버
};

} // namespace rocks::platform
```

- 대상 타입·순서 제약은 지금 자유 함수와 같다(1·2·4·8 byte 정수·열거형·포인터·bool, lock-free 필수, 포인터 산술 금지, 잘못된 순서 조합은 static_assert).
- 크기·정렬은 T와 같다. `alignas(sizeof(T))`로 8 byte 값이 구조체 안에서 4 byte 정렬로 떨어지는 일을 막는다.
- **순서 기본값 없음.** 개정 1은 "순서를 늘 이름으로 적는다"고 하면서 기본값(SequentiallyConsistent)을 두어 모순이었다. 기본값을 없앤다. 순서를 빠뜨리면 컴파일 오류다.
- **한 계열(D15).** 공개 원자 API는 `Atomic<T>`(값을 소유)와 `AtomicRef<T>`(기존 객체를 봄) 둘이다. 지금의 자유 함수(`AtomicLoad` 등)는 `platform::detail`로 내려 이 두 타입의 구현으로만 쓴다. `__atomic_*` builtin은 여전히 `Atomics.h` 한 곳이다.
  - `AtomicRef`가 자유 함수 자리를 맡는다. trivially copyable이어야 하는 칸(`BoundedMpscQueue::Cell`), 외부·공유 메모리, 원자 접근과 일반 접근이 시기별로 나뉘는 값(초기화 때 일반 쓰기, 게시 뒤 원자 읽기)이 그 경우다.
  - 늘 원자로만 접근하는 필드(통계 카운터, 상태 word, 참조 계수, 게시 포인터)는 `Atomic<T>`로 둔다.
  - 정책 검사 `atomic-detail`을 더한다. `platform::detail::Atomic*`을 `Platform/Atomics.h` 밖에서 부르면 위반이다. 기존 호출은 SY6에서 모두 `Atomic<T>`·`AtomicRef<T>`로 옮기고, 그 뒤 검사를 켠다.
- 이름 확인: 리플렉션의 struct 표시 `Atomic`(`ObjectRuntime/Tests/Reflection/AtomicStructTests.cpp` 7–10행)은 enum 값·지정자라 C++ 타입 이름과 겹치지 않는다. MetaCompiler 지정자 파서가 필드 형식 `Atomic<...>`·`AtomicRef<...>`를 지정자로 오인하지 않는지는 SY0의 통과 조건으로 확인한다(15절).

### 11.2 CpuPause와 SpinBackoff

- `Atomics.h`에 `void CpuPause() noexcept`를 둔다. x86-64는 `__builtin_ia32_pause()`, ARM64는 `__builtin_arm_isb(15)`(UE와 같은 `isb`)다. 정책 `compiler-builtin`의 `Atomics.h` 허용 목록에 이 두 builtin을 더한다.
- `YieldThread()`(OS 양보)와 이름으로 구분한다.
- `SpinBackoff`(D8): 반복 하나의 상태(횟수)다. `Pause()`는 처음 `PauseLimit`번(기본 40)은 `CpuPause()`, 그 뒤로는 `YieldThread()`다. 시간이 아니라 횟수로 넘어간다. 잠금 밖에서 "값이 바뀔 때까지 짧게 도는" 모든 반복이 이것을 쓴다(8.5절).

```cpp
// 사용 예(소비자가 미공개 칸을 기다린다):
//   SpinBackoff backoff;
//   while (!cell.IsPublished()) { backoff.Pause(); }
class SpinBackoff {
public:
    static constexpr uint32 PauseLimit = 40;
    void Pause() noexcept;
private:
    uint32 count_ = 0;
};
```

### 11.3 보조 연산

4.5절 4·7·10번을 모은다. `Atomics.h`의 `Atomic<T>`·`AtomicRef<T>` 멤버다. 순서는 다른 멤버처럼 템플릿 인자로 적는다.

| 연산 | 뜻 | 대체하는 곳 |
|---|---|---|
| `FetchMax(value)` | 현재 값보다 크면 바꾼다. 이전 값을 돌려준다 | `TraceBuffers.cpp` 102–110행 |
| `TryFetchAddBounded(value, limit)` | 더해도 `limit` 이하일 때만 더한다. 성공이면 `Optional<T>`(이전 값), 아니면 빈 값 | `TraceBuffers.cpp` 189–200행, `DynamicLibrary.cpp` 185–194행 |
| `FetchAddSaturating(value, limit)` | 넘으면 `limit`에 멈춘다 | `Archive.cpp` 242–262행 |
| `TryIncrementIfNonZero()` | 0이 아닐 때만 1 올린다(약한 참조 승격) | `SharedPtr.h` 52–62행 |
| `TakeUniqueNumber()` | 0을 쓰지 않는 단조 번호를 CAS로 하나 받는다. 소진되면 빈 `Optional`(호출자가 Fatal 또는 오류) | `StackAllocator.cpp` 13–20행, `TypeRegistry.cpp` 18–25행, `Name.cpp` 9–20행 |
| `FetchUpdate(function)` | 현재 값에 함수(`T → T`, noexcept)를 적용한 결과를 CAS 반복으로 쓴다. 이전 값을 돌려준다. 함수는 부작용이 없어야 한다(여러 번 불릴 수 있다) | `TraceEvents.cpp` 133–139행(double을 `uint64` bit 표현으로 더하기) |

- 원자 참조 계수 전용 타입(UE `TAtomicRefCount`)은 두지 않는다. UE 타입의 실질은 AutoRTFM 처리이고(2.3절), Rocks에서는 `Atomic<uint32>` + `FetchSub<AcquireRelease>` + 위 보조 연산이면 된다(D16).

## 12. lock-free 컨테이너와 할당기

### 12.1 규칙

- **push 경로에서 할당하지 않는다.** 용량은 `Initialize`에서 잡거나 노드를 호출자가 준다. UE `TMpscQueue`처럼 push마다 노드를 할당하면 실패를 push 결과로 돌려줘야 하고 할당기가 hot path에 들어온다.
- **진척 성질을 주석에 적는다.** 생산자·소비자 각각 lock-free인지, 어느 쪽이 멈추면 누가 막히는지(`BoundedMpscQueue` 주석 방식).
- **스레드 계약을 타입 이름에 적는다.** Spsc·Mpsc·Mpmc.
- **ABA와 회수 방식을 주석에 적는다**(12.3·12.4절 중 무엇인지).
- **단계 정책을 템플릿 인자로 받는다.** 모든 Core 동시 컨테이너는 마지막 템플릿 인자로 `StepPolicy`(기본 `NoSteps`)를 받고 원자 연산 사이에서 `StepPolicy::Step("name")`을 부른다. 기본 정책은 빈 inline 함수라 제품 비용이 0이다. 시험은 다른 정책 타입으로 별도 instantiation을 만든다(14.3절, D24).

### 12.2 목록

| 타입 | 뜻 | 회수·ABA | 사용 후보 |
|---|---|---|---|
| `BoundedMpscQueue<T>`(기존) | Vyukov 유한 MPSC | 칸 재사용, 순번으로 ABA 없음 | 로그, CVar 쓰기 큐 |
| `BoundedSpscQueue<T>` | 고정 원형 버퍼, 생산자·소비자 하나. head·tail을 각자 cache line에 두고 상대 값을 지역에 캐시한다 | 칸 재사용 | lane 간 일방 전달, 오디오 lane 입력 |
| `IntrusiveMpscQueue<Node>` | Vyukov 침투형 무한 MPSC. 노드는 호출자 소유이고 `next` 칸을 노드가 갖는다. push는 exchange 하나, pop은 소비자 하나 | 노드 수명은 호출자. 소비자가 하나라 ABA 없음 | 에셋 lease 해제 큐(`AssetSystem.ko.md` 337행), 지연 정리 목록 |
| `TakeAllStack<Node>` | 침투형 스택. push는 CAS, 소비는 `TakeAll()`이 머리를 exchange로 통째로 가져간다(LIFO, 뒤집기 함수로 FIFO). `Close()`를 부르면 이후 push가 false다 | pop 경합이 없어 ABA 없음 | 완료 통지 묶음, GC dirty root(UE `GarbageCollection.cpp` 630행과 같은 용도), 작업 후속 목록, `FrameArena` 소멸자 기록(4.5절 11번) |
| `IndexedFreeList<IndexBits = 24>` | 미리 잡은 칸 배열 위의 Treiber 스택. 머리는 64-bit `[태그 64−IndexBits \| 색인 IndexBits]`(기본 태그 40 bit, 색인 24 bit = 칸 약 1,677만 개), 다음 색인은 칸마다 `Atomic<uint32>` | 칸은 배열 수명 동안 해제하지 않는다. 태그로 ABA를 막는다(12.4절) | `LogBlockPool`·`TraceBlockPool` 통합(4.5절 3번), GC 512칸 블록 풀 |
| `FixedBlockPool` | 고정 크기 블록 할당기. 페이지 단위로 늘리고(늘리기만 `Mutex`) 블록은 `IndexedFreeList`로 돌린다. 스레드 지역 캐시는 두지 않는다(측정 뒤) | 파괴 때만 페이지를 돌려준다 | 작업 레코드, trace 블록, 객체 블록 |

두지 않는 것과 이유:
- **무한 MPMC 큐**(UE `FAAArrayQueue`): 노드 회수가 필요해 hazard pointer·epoch가 따라온다. 지금 필요한 곳은 Task overflow 큐뿐이고 잠금 큐로 시작한다(`EngineImplementationPlan.ko.md` 423행). Task 설계(후속 문서)에서 다시 본다(D33).
- **유한 MPMC 큐**: 지금 쓰임이 없다. 생기면 Vyukov 유한 MPMC를 `BoundedMpscQueue` 옆에 둔다.
- **할당하는 MPSC**(UE `TMpscQueue`): 12.1절.
- **work-stealing deque**: Task 설계(후속 문서)의 몫(2.3절, D33). 이 문서의 `Atomic<T>`·`SpinBackoff`·`IndexedFreeList`·`StepPolicy` 위에 만든다.

### 12.3 메모리 회수: SRCU, epoch, hazard pointer, 회수 없음

| 방식 | 읽기 비용 | 회수 지연 | 메모리 상한 | 잘 맞는 곳 |
|---|---|---|---|---|
| 회수 없음(고정 풀 + 색인 + 태그) | 0 | 없음(칸 재사용) | 풀 크기 | 블록·노드 풀, free list. UE `LockFreeList`·`LockFreeFixedSizeAllocator`의 방식 |
| SRCU(도메인별 공유 두 세대 카운터) | 구간 진입·이탈에 공유 카운터 RMW 하나씩(seq_cst `FetchAdd`·release `FetchSub`). 해제 쪽 대기를 위한 추가 비용 없음 | 진행 중인 옛 세대 구간이 모두 끝날 때까지 | 멈춘 독자 하나가 그 도메인의 회수를 막는다(상한 없음) | 쓰기가 드문 게시 포인터(로그 설치, trace 세션, CVar snapshot) |
| epoch(EBR, 스레드별 표시) | 구간 진입·이탈에 스레드 지역 쓰기 + fence | 모든 스레드가 구간을 한 번 벗어날 때까지 | SRCU와 같다 | 독자가 매우 많아 공유 카운터 경합이 문제가 되는 곳. 지금은 두지 않는다 |
| hazard pointer | 포인터를 읽을 때마다 게시 + `seq_cst` fence(`HP` 105행) | 짧다 | 스레드 수 × hazard 칸으로 유한 | 오래 쥐는 포인터, 독자가 멈출 수 있는 곳 |

개정 1은 표에 EBR 비용(스레드 지역 쓰기)을 적고 구현은 공유 카운터(SRCU)로 적어 서로 어긋났다. 실제 지금 코드(`LogSystem.cpp` 77–122행)와 이 문서의 구현은 SRCU다. 공유 카운터 RMW는 독자가 많은 도메인에서 cache line 경합을 만든다. 지금 쓰는 곳(로그 설치·trace 세션)은 독자가 스레드 수만큼이고 그 비용은 지금 코드와 같다. EBR은 측정으로 경합이 보일 때 더한다.

권장(D17):
1. **먼저 회수가 필요 없게 만든다.** 노드를 고정 풀에서 색인으로 주고 컨테이너 수명 동안 해제하지 않는다. 지금 Rocks lock-free 코드(로그·trace 블록 풀)가 이미 이 방식이고 UE도 대부분 그렇다.
2. **회수가 필요하면 SRCU 방식의 `ReclaimDomain`.** 엔진은 lane·worker 구조라 "짧은 읽기 구간"을 강제할 수 있다.
   - 도메인 단위다. 전역 하나가 아니다. 로그 설치, trace 세션, CVar snapshot이 각자 도메인을 갖는다. 한 도메인의 느린 독자가 다른 도메인 회수를 막지 않는다.
   - API: `ReclaimDomain::ReadScope`(구간), `Synchronize()`(지금까지 시작한 구간이 모두 끝나기를 기다린다, 동기 grace period), `Retire(pointer, deleter)`(나중에 회수), `Collect()`(기다리지 않고 회수 진행, 소유 lane이 프레임마다 부른다).
   - **상태.** 세대 번호 word 하나와 세대 카운터 word 두 개다. 세대 카운터 word의 최상위 bit는 "동기화 대기자 있음"이고 나머지가 그 세대의 독자 수다.
   - **독자.** 진입: 세대 번호를 seq_cst로 읽고 그 세대 카운터를 seq_cst `FetchAdd(1)` 한 뒤 보호 포인터를 seq_cst로 다시 읽는다(지금 `LogReadScope`와 같다). 이탈: 그 카운터를 release `FetchSub(1)` 한다. 반환값이 "대기 bit 있음, 독자 수 1"일 때만 그 카운터 주소에 콜백 있는 `WakeAll`을 부른다. 독자는 `FetchSub` 반환값만 보므로 대기자가 없을 때 독자 비용은 지금 코드와 같다(추가 RMW 0).
   - **`Synchronize`.** 도메인 잠금 아래에서 세대 번호를 seq_cst `FetchAdd`로 뒤집는다. 옛 세대 카운터를 `SpinBackoff`로 잠시 본 뒤에도 0이 아니면 대기 bit를 CAS로 세우고 `ParkingLot::Wait(카운터 주소, CanWait = "독자 수 > 0이고 대기 bit 있음")`로 잔다. 독자 쪽 `WakeAll` 콜백이 bucket 잠금 아래에서 대기 bit를 지운다(10.3절 규칙). 지금의 `YieldThread` 반복(4.5절 5번)을 대신한다.
   - **`Retire`·`Collect`.** `Retire` 목록은 세대별로 쌓는다. `Collect`는 옛 세대 카운터가 이미 0일 때만 세대를 뒤집고, 두 번 뒤집힌 세대의 목록을 회수한다. 0이 아니면 이번 프레임은 아무것도 하지 않는다(기다리지 않는다).
   - 독자가 구간 안에서 기다리거나 잠그는 것은 계약 위반이다. Debug는 구간 안의 `ParkingLot::Wait`를 보유 스택 검사로 잡는다(13.2절).
   - `Retire` 목록이 한도(설정값, 기본 4096)를 넘으면 경고 로그·trace 표지를 남긴다. 회수를 강제하지는 않는다. 로그 설치 도메인 자신의 경고는 재진입을 피해 trace 표지만 남긴다.
3. **hazard pointer는 두지 않는다.** UE에서도 Experimental이고 쓰임이 한 곳이다(3.10절). 독자가 오래 쥐는 곳이 생기면 참조 계수로 푼다.

`LogSystem` 설치·해제와 trace 생산자 대기(4.5절 5번)는 `ReclaimDomain::Synchronize`로 옮긴다(SY9). 옮기기 전에 지금 방식과 같은 시험(설치 중 기록 계속, 해제가 굶지 않음)을 통과해야 한다.

### 12.4 ABA

- **64-bit 단일 CAS에 `[태그 40 | 색인 24]`를 담는다.** 분할은 템플릿 인자 `IndexBits`(기본 24)로 받고 `static_assert(64 - IndexBits >= 38, "...")`로 태그 폭을 UE 이상으로 강제한다. 태그는 push·pop마다 1 올린다.
  - 한 스레드가 머리를 읽고 CAS하기 전에 같은 머리에서 2^태그폭번 연산이 일어나야 ABA가 된다. 연산 하나가 5–20 ns면 2^32번은 약 21–86초라, 그만큼 밀린 스레드 하나(과부하·저우선순위)로 ABA가 날 수 있다. UE는 태그 38 bit(색인 26 bit, `LFL` 55–56·132행)이고 여유 2^23 이상을 static_assert한다(`LFL` 274–277행). 2^38번은 약 23–95분, 2^40번은 약 1.5–6시간이다.
  - 색인 24 bit(칸 약 1,677만 개)는 블록·노드 풀에 충분하다. 더 큰 풀이 필요하면 `IndexBits = 26`까지 쓸 수 있다(태그 38 bit).
  - 지금 두 풀(`LogBlockPool.cpp` 72–126행, `TraceBuffers.cpp` 17–18·68–100행)은 `[태그 32 | 색인 32]`다. SY7 통합 때 바뀐다.
- **128-bit CAS는 쓰지 않는다.** x86-64 `cmpxchg16b`는 있으나 ARM64는 LSE `casp` 또는 LL/SC 쌍이 필요하고, `Atomics.h`의 lock-free 요구(`__atomic_always_lock_free`)를 16 byte에서 플랫폼마다 보장하지 못한다.
- **포인터 상위 bit 태그는 쓰지 않는다.** ARM64 TBI·MTE, 플랫폼별 가상 주소 폭에 기대게 된다.
- 색인은 `uint32`, "없음"은 `IndexNone` 계열 상수로 이름 붙인다(지금 `NoLogBlock`처럼 따로 정의하지 않는다).
- 태그 넘침은 정상 동작이다(감기는 것이 설계). UE처럼 로그를 남기지 않는다.

### 12.5 메모리 순서 규칙

- 게시: 데이터를 쓴 뒤 Release 쓰기, 소비: Acquire 읽기 뒤 데이터 읽기.
- CAS 실패 순서는 다음 시도에 읽은 값을 쓰면 Acquire, 아니면 Relaxed.
- 잠듦·깨움 짝(Dekker)은 직접 쓰지 않고 `EventCount`를 쓴다.
- StoreLoad 순서는 단독 fence(`AtomicThreadFence`) 대신 같은 변수의 RMW로 만든다(5절). ThreadSanitizer는 단독 fence를 모델하지 못해 fence 기반 코드(지금 `LogSystem.cpp` 411·440행)에 오탐을 낸다(14.5절). fence가 꼭 필요한 곳은 이유를 주석에 적는다.
- 각 알고리즘 주석에 "어느 Release가 어느 Acquire와 짝인지"를 적는다(`BoundedMpscQueue` 주석 방식).

## 13. 디버그 지원

### 13.1 정적 검사: Clang thread safety analysis

- `Base/Compiler.h`에 `ROCKS_CAPABILITY(name)`, `ROCKS_SCOPED_CAPABILITY`, `ROCKS_GUARDED_BY(lock)`, `ROCKS_PT_GUARDED_BY(lock)`, `ROCKS_REQUIRES(...)`, `ROCKS_REQUIRES_SHARED(...)`, `ROCKS_ACQUIRE(...)`, `ROCKS_RELEASE(...)`, `ROCKS_TRY_ACQUIRE(...)`, `ROCKS_EXCLUDES(...)`, `ROCKS_NO_THREAD_SAFETY_ANALYSIS`를 둔다(Clang `__attribute__((capability))` 계열).
- `Mutex`·`RecursiveMutex`·`ReadWriteMutex`는 capability, 가드는 scoped capability다.
- 빌드에 `-Wthread-safety`를 켜고 경고를 오류로 둔다. Windows(clang-cl)를 포함한 모든 대상에서 컴파일 때 돈다. 실행 시간 운에 기대지 않는 유일한 경쟁 검사다.
  - **실측(2026-11-18, clang-cl 21.1.6).** clang-cl이 `-Wthread-safety`를 접두 그대로 받는다. `/W4 /WX`와 함께 주면 보호 변수 쓰기가 `error: writing variable 'v' requires holding mutex 'm' exclusively [-Werror,-Wthread-safety-analysis]`로 막힌다. `/W4`만으로는 켜지지 않는다.
  - **빌드 변경(SY0).** `Programs/Rocks.Build/ActionGraph.cs`의 Windows 인자(430행 `/W4`·`/WX` 옆)와 POSIX 언어 인자(471–472행)에 `-Wthread-safety`를 더한다. MetaCompiler는 네이티브 프로필 인자만 `--clang-arg`로 받는다. 허용 목록을 늘릴 때 AST 동등성 검토와 회귀 검사를 먼저 둔다는 규칙이 있다(`Build/README.md` 140행). 그래서 이 경고 인자를 허용 목록에 더하고 회귀 검사를 함께 둔다. 경고 인자라 AST는 바뀌지 않는다.
  - **범위(D32).** 외부 라이브러리(ThirdParty manifest로 빌드하는 모듈)에는 걸지 않는다. Rocks 모듈만이다.
- 매크로는 SY0에서 `Base/Compiler.h`에 둔다. 주석 달기는 SY2부터 새 타입·새 코드에 하고, 기존 필드는 SY6에서 옮기며 단다. 분석이 표현하지 못하는 곳(조건부 잠금, 잠금 넘겨주기, 상태 word 안의 `IntrusiveMutex`)은 `ROCKS_NO_THREAD_SAFETY_ANALYSIS`와 이유 주석이다(D20).

### 13.2 잠금 순위와 보유 스택(Debug·Development)

- **보유 스택.** 스레드마다 고정 16칸 배열(잠금 주소, 종류, 순위, 호출 위치)이다. 할당하지 않는다. 넘치면 Fatal이다.
- **검사.**
  - 같은 비재진입 잠금을 다시 잡음 → Fatal(자기 교착). `ReadWriteMutex`는 같은 스레드의 읽기 두 번, 읽기 뒤 쓰기, 쓰기 뒤 읽기를 모두 포함한다. 쓰기 대기자가 있을 때만 실제로 교착하는 읽기 재진입(8.4절)도 대기자 유무와 관계없이 결정적으로 잡는다.
  - `OnceFlag` 실행 중 같은 flag로 다시 부름 → Fatal(실행하는 동안 flag 주소를 종류 `Once`로 올린다, 9.4절).
  - 쥐지 않은 잠금을 풂, 다른 스레드가 쥔 잠금을 풂 → Fatal. 예외: `InitiallyLocked`로 만든 잠금의 첫 해제는 보유 스택에 없는 것이 정상이라 검사하지 않는다(8.1절).
  - 잠금을 쥔 채 기다림 → Fatal. 대상은 `ManualResetEvent`·`AutoResetEvent`·`EventCount`·`Semaphore`·`OnceFlag`(다른 스레드 실행을 기다림)·`Thread::Join`·`ReclaimDomain::Synchronize`, 그리고 task 대기(`WorkerScheduler`의 작업 완료 대기·`WorkerTaskEvent` 대기·`DedicatedLane` 완료 대기)다. task 대기는 `EngineImplementationPlan.ko.md` 719행 "lock 보유 상태의 task wait는 금지"를 따른다. `ConditionVariable::Wait`의 짝 잠금은 예외다. 의도된 경우는 `AllowWaitWhileLocked` 범위로 표시한다.
  - 순위 위반: 쥐고 있는 순위 이하의 순위를 잡음 → Fatal.
- **순위.** `enum class LockRank : uint16`을 PlatformBase `LockRank.h`에 한 곳으로 모은다(값 사이에 간격을 둔다). `RankedMutex<LockRank::Log>`·`RankedReadWriteMutex<...>`가 순위를 타입에 싣는다. 크기는 같은 1·4 byte다.
  - 계획 719행 "lane별 lock rank를 정하고 debug에서 검사한다"에 맞춘다. 순위 값은 lane 순서로 구간을 나눈다. 바깥부터 Game → Render → RHI·장치 → IO → worker 공용 순이고, 각 구간 안에 그 lane 소유 잠금을 둔다. 엔진 전역 핵심 잠금(로그 설치, trace 등록, 객체 레지스트리, 형 등록, 이름 풀, 할당기)은 모든 lane 구간보다 안쪽(가장 나중에 잡는 쪽)에 둔다. AudioRT는 잠금을 쓰지 않으므로 구간이 없다(D27).
  - lane 소유 잠금과 전역 핵심 잠금에 순위를 붙인다. 순위 없는 잠금은 순위 검사에서 빠지고 위의 다른 검사만 받는다.
  - 계획 719행의 문장과 이 구간 정의를 맞추는 문서 반영은 SY11에 넣는다.
- 동적 순서 그래프(Abseil 방식: 실제로 잡은 순서로 그래프를 만들고 순환을 찾는다)는 후속이다. 순위 없는 잠금 사이의 교착을 잡지만 메모리·비용이 크다(D21).
- Release 빌드는 이 검사를 모두 뺀다(빈 함수).
- 보고: Core가 설치한 훅이 로그(`Log::Fatal`, 영어 메시지)로 낸다. 설치 전이면 `platform::Fatal`이다. 메시지는 영어(로그·Fatal 문구 규칙)다.

### 13.3 경합 trace

- TraceSystem에 채널 `lock`을 더한다. 기록은 slow path에서만 낸다.
  - `Sync.ContendedWait`: 잠금 주소, 종류(Mutex·ReadWrite 읽기/쓰기·Recursive·Event·EventCount·Semaphore·Condition), 순위(있으면), 시작·끝 tick.
  - spin만으로 잡은 경우는 기록하지 않는다(빈도가 너무 높다). 통계 카운터(`lock.spins`·`lock.parks`)만 올린다.
- 구현: `SyncHooks.onWaitBegin/onWaitEnd` 함수 포인터다. 호출 지점은 10.4절이다. `onWaitBegin`은 bucket을 잡기 전에 부르고, 둘 다 스레드 지역 버퍼에만 쓴다. trace 세션으로 내보내기는 그 스레드가 ParkingLot 밖에 있을 때(다음 trace 블록 공개) 한다. trace 기록 경로가 잠금을 잡을 수 있기 때문이다(`TraceSnapshot.cpp`의 writer 잠금, slot registry, 이름 표 삽입 잠금). TraceCapture가 세션 시작 때 설치한다. Core·PlatformBase가 TraceCapture에 의존하지 않는다(`TraceCapture`는 Core에 의존, `TraceCapture/TraceCapture.Build.cs` 9행).
- 재진입 guard: 훅 안에서 다시 경합 대기가 생기면 그 대기는 기록하지 않는다(10.4절).
- 비용: 경합 없는 잠금은 0이다. 잠드는 경로에서 훅 포인터 relaxed load 하나와 분기 하나다. 잠드는 비용(μs 단위)에 비해 무시할 수 있다.
- 분석기(TraceAnalysis)는 주소별 대기 시간 합·최대, 순위별 집계를 보인다.

### 13.4 교착 진단

- **`sync.dump` 콘솔 명령**(CVar 콘솔 영역 `sync`): ParkingLot bucket을 하나씩 잠그며 잠든 스레드(스레드 이름·ID·대기 주소·대기 시작 tick)를 모으고, Debug면 각 스레드의 보유 스택을 붙여 로그로 낸다. 진단 출력이며 판정에 쓰지 않는다.
- **Fatal·crash 때**: 같은 내용을 TryLock으로만 모은다(못 잡은 bucket은 "잠김"으로 적는다). 멈춘 쪽을 기다리지 않는다.
- **watchdog은 두지 않는다.** "N초 이상 기다리면 교착"은 시간에 기대는 판정이다. 오래 기다림은 trace로 보고, 교착 판정은 사람이 `sync.dump`로 한다(D23).

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

## 15. 구현 단계

단계 기호는 SY다. 크기 S·M·L은 상대값이다.

| 단계 | 내용 | 선행 | 크기 |
|---|---|---|---|
| SY0 | PlatformBase: `CpuPause`·`SpinBackoff`, `WaiterSignal`(Windows `WaitOnAddress`·Android futex·macOS 조건 변수)과 가짜 OS 대기 seam, `PlatformBase.Build.cs` Windows `SystemLibraries = ["Synchronization"]`, 정책 허용 목록에 pause builtin, `Atomic<T>`·`AtomicRef<T>`(순서 기본값 없음)와 보조 연산(11절), `Base/Compiler.h`의 thread safety 매크로, `ActionGraph.cs` 430·471–472행에 `-Wthread-safety`, MetaCompiler `--clang-arg` 허용 목록과 회귀 검사, 시험. **통과 조건**: MetaCompiler 지정자 파서가 필드 형식 `Atomic<...>`·`AtomicRef<...>`를 리플렉션 struct 표시 `Atomic`과 혼동하지 않음을 시험으로 확인 | 없음 | M |
| SY1 | `ParkingLot`(콜백 있는 `WakeMultiple`·`WakeAll` 포함)·`detail::QueueMutex`·영구 선형 할당·침투 목록 깨우기·스레드 레코드 수명과 TLS 파괴 뒤 임시 레코드(D31), `SyncHooks` 자리와 훅 호출 지점(10.4절), `SyncTestAccess.h`, 결정적 시험(14.2절), 단계 스케줄러 하네스(14.3절) | SY0 | L |
| SY2 | **제자리 교체**: 기존 `Platform/Sync.h`의 `Mutex`·`ConditionVariable`·`LockGuard`를 parking lot판으로 바꾼다(이름·호출 모양 그대로, constexpr·trivially destructible). `IntrusiveMutex`·`InitiallyLocked`·가드 다섯, `WaitResult::Failed` 제거와 그 사용처 정리, 대기 표시 경쟁 시험. 새 타입에 thread safety 주석 | SY1 | M |
| SY3 | `RecursiveMutex`·`ReadWriteMutex`·`ReadLockGuard`·`ReadLockOwner` | SY2 | M |
| SY4 | `ManualResetEvent`·`Semaphore`(기존 타입 제자리 교체), `AutoResetEvent`·`EventCount`·`OnceFlag`·`CallOnce`·`CallOnceUntilSuccess` | SY1 | M |
| SY5 | 진단: 보유 스택·순위(`LockRank`·`RankedMutex`, lane 구간), 읽기 재진입·잠금 쥔 채 대기·task 대기 검사, `InitiallyLocked` 예외, Core 로그 훅 설치, `sync.dump` | SY2–SY4 | M |
| SY6 | 정리: `Platform/Sync.h`를 `Platform/Sync/*.h`로 헤더 분할하고 사용처 include 갱신, OS 잠금 wrapper 제거, 4.5절 1·2·4·6·7·8·9·12·13번 대체, 자유 원자 함수 호출을 `Atomic<T>`·`AtomicRef<T>`로 옮기고 정책 `atomic-detail` 켜기, 기존 필드 thread safety 주석, 전역 핵심 잠금·lane 잠금에 순위, 시험의 `YieldThread` 폴링을 `TestGate`로 | SY2–SY5 | L |
| SY7 | Core 동시 컨테이너: `IndexedFreeList`(`[태그 40 \| 색인 24]`, 로그·trace 블록 풀 통합), `FixedBlockPool`, `BoundedSpscQueue`, `IntrusiveMpscQueue`, `TakeAllStack`(`FrameArena` 소멸자 기록 포함), 모든 컨테이너의 `StepPolicy` 인자, interleaving 시험 | SY0, SY1(하네스), SY2(`FixedBlockPool`의 페이지 잠금) | M |
| SY8 | trace 채널 `lock`과 `SyncHooks` 설치(TraceCapture), TLS 버퍼 내보내기, 분석기 집계 | SY1, TraceSystem TR 단계 | S |
| SY9 | `ReclaimDomain`(SRCU, 동기화 대기 bit), 로그 설치·trace 생산자 대기 옮기기(4.5절 5번), 시험 | SY4, SY5(구간 안 대기 검사), SY7 | M |
| SY10 | `STRESS_TEST`·`--stress`, macOS `Tsan` 구성과 `__tsan_mutex_*` 주석, ARM64 stress 실행, 성능 측정 | SY2–SY9 | M |
| SY11 | 문서 반영: PlatformLayer.ko.md 68·270행, Docs/Implementation/PlatformConcurrency.ko.md 55행, UnrealCoreFeatureAudit.ko.md UE-C17 44행, LoggingSystem 8.4절(깨움·`WaitForSpace`), TraceSystem 4.1·채널 표, CVarSystem 콘솔 영역 `sync`, EngineImplementationPlan.ko.md 719행(lane 순위 구간과 task 대기 검사)·423행(Task 설계 후속 문서), GarbageCollectionScale GC-S4 블록 풀. 구현 문서, 묶음 끝 리뷰 | 전부 | S |
| SY12 | `RealtimeWakeSignal`(9.6절, 개정 3): 상태 원자·OS 수단 셋(Windows·Android·macOS Mach semaphore)·가짜 OS 대기 seam 연결·시험 | SY0 | S |

- 임시 이름공간을 두지 않는다(D28). 새 `Mutex`·`ConditionVariable`의 API는 기존 API의 상위집합이다. 생성자가 constexpr이 되고 파괴자가 없어지며 `TryLockFor`가 더해진다. 그래서 SY2에서 같은 이름으로 제자리 교체하고, 호출 코드는 바뀌지 않는다. 바뀌는 것은 `WaitResult::Failed` 사용처(비시험 28곳)뿐이다. 두 종류의 Mutex가 공존하는 기간이 없다.
- SY3·SY4의 새 타입은 처음부터 `Platform/Sync/*.h`에 둔다. SY6은 기존 `Platform/Sync.h`의 헤더 분할과 중복 제거만 맡는다.
- SY2와 SY6은 Core·Task·RenderBase·ObjectRuntime·Crypto·TraceCapture·Localization·Compression·Graphics backend를 건드린다(SY2는 재컴파일과 `WaitResult` 정리, SY6은 include·원자 호출 옮기기). 메인 트리 구현 에이전트 한 번에 하나 규칙에 따라 다른 구현과 겹치지 않는 시점에 한다.

## 16. 결정 목록

권장안으로 확정했다(2026-11-18, 개정 2). **[범위 큼]**은 범위가 커서 착수 전에 사용자가 다시 볼 만한 항목, **[원칙 확인]**은 프로젝트 원칙과 맞닿아 판단 근거를 적은 항목이다. D29–D33은 개정 2에서 새로 정한 결정이다.

- **D1 범위** — 프로세스 내부 동기화만 한다. 프로세스 간 잠금·semaphore, AutoRTFM 계열(`TransactionallySafe*`), GPU 동기화, 이벤트 풀은 하지 않는다(2.3절).
- **D2 배치 [범위 큼]** — 잠금·신호·ParkingLot·`Atomic<T>`·`AtomicRef<T>`는 PlatformBase에 둔다(`Platform/Sync/*.h`, 구현은 `Private/Common/Sync/`). OS 고유 코드는 `WaiterSignal` 하나다. lock-free 컨테이너·회수는 Core(`Core/Containers/`, `Core/Memory/`)다. OS 잠금 wrapper는 지운다. Engine/Source/Runtime에는 두지 않는다(7절).
- **D3 Mutex 구현 [범위 큼]** — parking lot 기반 1 byte, constexpr·trivially destructible, 불공정, 대기자 없을 때만 `CpuPause` 40번 spin 뒤 잠든다. `TryLockFor`를 둔다. OS 잠금(SRWLOCK·`pthread_mutex_t`)을 감싸지 않는다. 모든 사용처가 다시 컴파일되는 변경이다(8.1절).
- **D4 OS 대기 수단** — Windows `WaitOnAddress`, Android futex, macOS는 스레드 레코드·대기 노드마다 `pthread_mutex_t`+`pthread_cond_t`(깨우는 쪽은 mutex를 쥔 채 signal). `__ulock_wait`(비공개 API)는 쓰지 않는다. macOS 14.4 이상 `os_sync_wait_on_address` 분기는 측정 뒤에 정한다(7.3절).
- **D5 ParkingLot 메모리** — 가상 메모리 예약에서 영구 선형 할당 하나(4 MiB + 64 KiB). UE 주석의 계산(16,384 스레드 2.25 MiB, `PLC` 61행)대로 넘침은 Fatal이다. UE의 scratch 할당기는 두지 않고, 깨울 스레드와 옮길 스레드는 레코드의 다음 포인터로 지역 침투 목록에 엮는다. bucket 수는 사용 스레드 수의 2의 거듭제곱 이상(최소 32), 옛 표는 해제하지 않는다(유한). bucket 잠금은 비공개 `detail::QueueMutex`. `VirtualMemory` 클래스를 거치지 않아 재진입이 없다(10.2절).
- **D6 재진입 잠금** — `RecursiveMutex`(16 byte)는 두되, 쓰는 곳은 선언 옆에 이유를 적는다. UE `FCriticalSection`을 기계적으로 옮기지 않는다. 재진입 읽기쓰기 잠금은 두지 않는다(8.3·8.4절).
- **D7 읽기쓰기 잠금** — 이름 `ReadWriteMutex`, 4 byte, 쓰기 우선, 비재진입, 승격 없음, 읽기 연산은 `LockRead`·`UnlockRead`. **읽기 재진입은 금지**다(쓰기 대기자가 있으면 교착). 계약에 적고 Debug 보유 스택이 같은 `ReadWriteMutex` 읽기의 중복 획득을 쓰기 대기 여부와 관계없이 결정적으로 Fatal한다. 기존 읽기 많은 표(형 등록·이름 풀·지역화 목록)는 콜백 재진입을 확인하고 측정한 뒤에 바꾼다(8.4·13.2절).
- **D8 spin [원칙 확인]** — 공개 spin 잠금·읽기쓰기 spin 잠금은 두지 않는다. constinit `Mutex`가 같은 일을 하고 잠든 스레드가 CPU를 태우지 않는다. 기존 spin 두 곳은 `Mutex`로 바꾼다. UE에는 있으나 공용 기능 하나 원칙을 따랐다. 잠금이 아닌 도는 반복은 `SpinBackoff`(`CpuPause` 40번 뒤 `YieldThread`)를 쓰고, 오래 걸릴 수 있는 기다림(`LogSystem` `WaitForSpace`)은 `EventCount` 대기로 바꾼다(8.5·11.2절).
- **D9 조건 변수** — `ConditionVariable`을 parking lot 위 1 byte로 유지한다(UE에는 없음). `Mutex`와만 짝짓는다. 대기 bit는 `CanWait` 안(bucket 잠금 아래)에서 세우고, 지우기는 콜백 있는 `WakeOne`·`WakeAll`의 콜백에서만 한다. 대기자 수(4 byte) 방식도 맞지만, 같은 정확성을 1 byte로 얻고 다른 타입과 같은 규칙을 쓰는 bit 방식으로 정한다. predicate는 짝 `mutex` 아래에서 바꾸고 `Notify`는 `mutex` 밖에서 불러도 된다(8.6절).
- **D10 이벤트** — `ManualResetEvent`(1 byte)·`AutoResetEvent`(4 byte)를 둔다. 가상 인터페이스·풀·OS 핸들이 없다. 둘 다 D29 규칙을 따른다(`ManualResetEvent::Signal`은 `FetchOr` + 콜백 있는 `WakeAll`, `AutoResetEvent::Signal`은 콜백 있는 `WakeOne`으로 신호 넘김, 9.1절).
- **D11 EventCount** — UE와 같은 알고리즘의 `EventCount`(32-bit)를 둔다. `Notify`의 순서는 UE와 같은 `FetchAdd<AcquireRelease>(0)`다. 장벽 생략판(`NotifyWeak`)은 두지 않는다. 로그의 Dekker 깨움과 `WaitForSpace` 바쁜 대기를 이것으로 바꾼다(9.2절).
- **D12 Semaphore** — parking lot 위 공용 구현, 8 byte(상태 4 byte: 대기 bit + 허가 31 bit, 최대값 4 byte). `maximumCount`는 1 이상 2^31 − 1 이하이고 넘으면 Fatal이다(지금보다 범위가 좁아진다, 하위호환 없음). `Release` 실패 뜻은 지금대로 모든 플랫폼에서 지킨다(9.3절).
- **D13 Once** — `OnceFlag`(1 byte)와 두 함수다. `CallOnce`는 실패를 기록하고 다시 실행하지 않는다. `CallOnceUntilSuccess`는 실패를 기록하지 않고 대기자 하나가 다시 실행한다(constinit 전역의 일시적 실패용). `Reset`은 두지 않는다. 실행 중 재진입은 Debug 보유 스택(종류 `Once`)으로 Fatal(9.4절).
- **D14 대기 표기** — 시간은 millisecond `uint32` + `InfiniteTimeout`을 유지하고 안쪽은 monotonic 시한이다. `WaitResult::Failed`를 지우고 회복할 수 없는 OS 대기 오류는 Fatal로 한다(9.5절).
- **D15 원자 API [범위 큼] [원칙 확인]** — 공개 원자 API는 `Atomic<T>`(값 소유)와 `AtomicRef<T>`(기존 객체를 봄) 한 계열이다. 메모리 순서는 기본값 없이 늘 템플릿 인자로 적는다. 지금의 자유 함수는 `platform::detail`로 내려 두 타입의 구현으로만 쓰고, SY6에서 모든 호출을 옮긴 뒤 정책 `atomic-detail`로 `Atomics.h` 밖의 사용을 막는다(공용 기능 하나 원칙, 11.1절).
- **D16 보조 연산** — `FetchMax`·`TryFetchAddBounded`·`FetchAddSaturating`·`TryIncrementIfNonZero`·`TakeUniqueNumber`·`FetchUpdate`를 둔다. 원자 참조 계수 전용 타입은 두지 않는다. 장치 소유 스레드 표지는 참조 계수가 아니라 CAS 토큰으로 그대로 둔다(4.5·11.3절).
- **D17 메모리 회수** — 1순위는 회수가 필요 없는 설계(고정 풀 + 색인 + 태그). 필요하면 도메인 단위 **SRCU** `ReclaimDomain`(공유 두 세대 카운터). 세대 카운터 word에 동기화 대기자 bit를 두고, 독자는 이탈 때 `FetchSub` 반환값만 보며 대기자가 있을 때만 깨운다(대기자 없을 때 추가 비용 0). `Synchronize`는 `SpinBackoff` 뒤 ParkingLot으로 잔다. EBR(스레드별 표시)은 공유 카운터 경합이 측정으로 보일 때 더한다. hazard pointer는 두지 않는다. 로그·trace의 세대 대기를 `ReclaimDomain::Synchronize`로 모은다(12.3절).
- **D18 ABA** — 64-bit 단일 CAS의 `[태그 40 | 색인 24]`. 분할은 템플릿 인자 `IndexBits`로 받고 `static_assert(64 - IndexBits >= 38)`로 태그 폭을 UE(38 bit) 이상으로 강제한다. 128-bit CAS·포인터 상위 bit 태그는 쓰지 않는다. 태그 감김은 정상이다(12.4절).
- **D19 컨테이너 목록** — `BoundedMpscQueue`(유지)·`BoundedSpscQueue`·`IntrusiveMpscQueue`·`TakeAllStack`·`IndexedFreeList`·`FixedBlockPool`. 모두 `StepPolicy` 템플릿 인자를 받는다. push 경로에서 할당하지 않는다. 무한 MPMC·할당 MPSC·work-stealing deque는 두지 않는다(deque는 Task 설계 후속 문서, 12.2절).
- **D20 정적 검사 [범위 큼]** — Clang thread safety analysis 매크로를 SY0에서 `Base/Compiler.h`에 두고 `-Wthread-safety`를 오류로 켠다(clang-cl 21.1.6에서 `/W4 /WX`와 함께 동작함을 실측). `ActionGraph.cs` 430·471–472행과 MetaCompiler `--clang-arg` 허용 목록(회귀 검사 포함)을 SY0에서 바꾼다. 새 코드부터, 기존 필드는 옮기며 단다(13.1절).
- **D21 잠금 순위** — Debug·Development에서 스레드별 고정 16칸 보유 스택. 자기 교착(읽기 재진입 포함)·남의 잠금 해제·잠금 쥔 채 대기(task 대기 포함)·순위 위반·`OnceFlag` 재진입을 Fatal. `InitiallyLocked` 잠금의 첫 해제는 예외. 순위는 `EngineImplementationPlan.ko.md` 719행에 맞춰 lane 구간(Game → Render → RHI·장치 → IO → worker 공용, 그 안쪽에 전역 핵심 잠금)으로 나누고 `RankedMutex<LockRank>`로 싣는다. 계획 문서 반영은 SY11. 동적 순서 그래프는 후속(13.2절).
- **D22 경합 trace** — TraceSystem 채널 `lock`, slow path(잠듦)에서만 기록, spin은 카운터만. `onWaitBegin`은 bucket 진입 전에 부르고 스레드 지역 버퍼에만 쓴다. 내보내기는 ParkingLot 밖에서 한다. 재진입 guard를 둔다. `SyncHooks` 함수 표는 TraceCapture가 설치한다. 경합 없는 잠금 비용 0(10.4·13.3절).
- **D23 교착 진단 [원칙 확인]** — `sync.dump` 콘솔 명령과 Fatal 때의 TryLock 수집. 시간 기준 watchdog은 두지 않는다(시간 의존 판정 금지 원칙, 13.4절).
- **D24 시험 [범위 큼] [원칙 확인]** — ParkingLot 시험 훅으로 sleep 없는 순서를 만든다. 단계 지점은 Core 컨테이너에서는 `StepPolicy` 템플릿 인자(제품 비용 0, ODR 문제 없음)이고, ParkingLot·잠금 .cpp에서는 Debug·Development의 TLS 함수 포인터(`ROCKS_SYNC_STEP`, slow path에만, Release는 빈 매크로)다. 설치는 `*TestAccess.h`로 해서 `ROCKS_TESTING` 정책(`testing-macro`)과 충돌하지 않는다. 시한은 단조 시계와 `WaiterSignal` 대기를 함께 가짜로 바꿔 실제 시간 없이 확인한다. `STRESS_TEST`·`--stress`로 stress를 분리한다. 외부 모델 검사기는 쓰지 않는다(14절).
- **D25 TSan** — macOS 전용 `Tsan` 구성을 두고 수동으로 돈다. 기본 검증에 넣지 않는다. Windows는 clang-cl이 `-fsanitize=thread`를 거부함을 실측했고 정적 분석에 기댄다. TSan은 단독 fence를 모델하지 못하므로 StoreLoad 순서는 RMW로 만든다는 규칙을 둔다(5·12.5·14.5절).
- **D26 Task 연계** — `onWaitBegin`·`onWaitEnd` 자리를 worker 보충(UE oversubscription)의 자리로 둔다. 켤지는 Task 설계(후속 문서)에서 정한다(10.5절, D33).
- **D27 우선순위 역전** — parking lot 잠금은 OS 우선순위 상속(Apple `os_unfair_lock`·pthread priority inheritance)을 받지 못한다. 지금은 받아들인다. AudioRT는 계획 716행대로 잠금을 쓰지 않는다. 다른 lane과의 입력은 `BoundedSpscQueue`로 받는다. 그래서 실시간 스레드가 잠금 때문에 역전되는 경로가 없다(4.6절).
- **D28 옮기기 순서** — 임시 이름공간을 두지 않는다. SY2에서 기존 `Mutex`·`ConditionVariable`(SY4에서 `ManualResetEvent`·`Semaphore`)을 같은 이름으로 제자리 교체한다. SY6은 헤더 분할과 중복 제거만 맡는다. 하위호환 별칭을 두지 않는다(15절).
- **D29 대기 표시 규칙(새)** — bucket 밖에서 세운 대기 표시는 `CanWait`에서 다시 확인한다. 지우기는 bucket 잠금 아래 콜백에서만 한다(예외 2b: 지우는 연산이 동시에 `CanWait`를 거짓으로 만드는 `EventCount` 횟수 증가). ParkingLot에 콜백 있는 `WakeMultiple`·`WakeAll`을 둔다. ConditionVariable·AutoResetEvent·Semaphore·OnceFlag·ManualResetEvent·ReadWriteMutex·ReclaimDomain에 모두 적용하고, 타입마다 "표시를 세운 뒤 enqueue 전에 다른 대기자의 알림이 끼어드는" 경쟁 시험을 둔다(10.3·14.2절).
- **D30 훅 호출 지점(새)** — `onWaitBegin`은 bucket 진입 전, `onWaitEnd`는 깬 뒤에 부르고 스레드 지역 버퍼에만 쓴다. `onParked`는 enqueue와 `BeforeWait` 뒤에 부르고 `WaiterSignal` 기반 `TestGate`만 쓴다. 훅 안의 ParkingLot 대기는 금지(Debug Fatal), 재진입 guard를 둔다(10.4절).
- **D31 TLS 파괴 뒤 대기(새)** — 스레드 레코드의 `thread_local` 칸이 파괴된 뒤의 대기는 UE처럼 대기마다 영구 풀의 임시 레코드를 쓰고 참조 계수로 돌려준다. 시험을 둔다(10.2·14.2절).
- **D32 빌드 연결(새)** — `WaitOnAddress` 링크는 `PlatformBase.Build.cs`의 Windows `SystemLibraries`에 `"Synchronization"`을 넣어 한다. thread safety 분석은 ThirdParty 모듈에 걸지 않는다. `ActionGraph.cs` 430·471–472행 플래그와 MetaCompiler `--clang-arg` 허용 목록 변경은 SY0에 넣는다(7.3·13.1절).
- **D33 Task 설계 후속 문서(새)** — Task 설계 문서는 아직 없다. work-stealing deque, overflow 큐, worker 보충(oversubscription)은 **Task 설계(후속 문서)** 항목으로 남긴다. 이 문서는 그 바탕(`Atomic<T>`·`SpinBackoff`·`IndexedFreeList`·`StepPolicy`·대기 훅 자리)만 정한다(2.3·10.5·12.2절).
- **D34 실시간 깨우기(개정 3)** — `RealtimeWakeSignal`을 둔다: 대기자 하나, 자동 리셋, Signal은 잠금·할당이 없고 대기자가 있을 때만 OS 깨우기를 부른다. macOS는 Mach semaphore. 첫 사용처는 오디오 장치 콜백 → AudioRT([오디오 시스템](AudioSystem.ko.md) AD26·PR21). 단계 SY12.

## 17. 미확인 사실

UE 쪽:
- `FSharedRecursiveMutex`의 재진입 공유 잠금 알고리즘 세부(`Private/Async/SharedRecursiveMutex.cpp` 387행)를 읽지 않았다.
- `Private/Unix/`·`Private/Linux/` 수동 리셋 이벤트 구현(futex 여부)을 읽지 않았다. Rocks는 Linux를 대상으로 하지 않는다.
- 작업 scheduler `FWaitingQueue`의 `Park`·`CommitWait`(spin·대기 cycle 수)를 읽지 않았다(`Public/Async/Fundamental/WaitingQueue.h` 75·102행).
- 잠금 순서 검사기·교착 검출기는 `Public/Async`·`Private/Async`·`Public/HAL`·`Private/HAL`에서 문자열 검색으로 찾지 못했다. 다른 모듈(예: 진단 도구)에 있을 수 있다.
- UE race detector(`Private/Sanitizer/RaceDetectorWindows.cpp`)가 어떤 컴파일러 계측을 요구하는지 읽지 않았다.
- 3.11절 사용 수는 문자열 하한이다. namespace 안 무접두 사용은 빠졌다.
- 렌더 스레드 "지연 기록 명령 목록"(`RenderCore/Public/RenderingThread.h` 725–740행)의 lock-free 구현 방식은 다시 읽지 않았다(`RenderBase.ko.md` 270행 인용만).

Rocks·도구 쪽:
- Android NDK의 ThreadSanitizer 지원 범위를 확인하지 못했다(확신 낮음, 공식 지원이 아닌 것으로 안다).
- macOS `WaiterSignal`의 "깨우는 쪽 `pthread_mutex_unlock` 직후 대기자가 노드를 파괴" 경로가 Apple libpthread에서 안전하다는 것은 POSIX의 "풀린 mutex 파괴 허용" 규정에 기댄 판단이고 실기에서 시험하지 않았다(7.3절, 확신 낮음).
- `thread_local` 객체의 파괴자 사용(ParkingLot 스레드 레코드 반환)이 Rocks 스레드 종료 규약(`Docs/Implementation/PlatformConcurrency.ko.md`의 TLS 소멸자 관련 시험)과 어긋나지 않는지 SY1에서 시험으로 확인한다(D31).
- parking lot `Mutex`와 SRWLOCK·`pthread_mutex_t`의 성능 비교는 측정하지 않았다. UE가 바꾼 근거를 따른 것이다.
- Android futex 대기의 `FUTEX_WAIT_BITSET_PRIVATE` 절대 시한은 `FUTEX_CLOCK_REALTIME`이 없으면 `CLOCK_MONOTONIC` 기준이다(futex(2) 규정, UE `AndroidPlatformManualResetEvent.cpp` 83–84행도 그렇게 쓴다). 대상 API 26 실기에서는 시험하지 않았다.
- lane 순위 구간의 순서(D21)는 계획 문서의 lane 정의에 맞춘 권장이다. 실제 lane 사이 잠금 순서는 SY5·SY11에서 사용처를 보며 확정한다.

해소된 항목(개정 2): ParkingLot 영구 할당 크기 근거(`PLC` 61행), `Synchronization.lib` 링크 자리(`SystemLibraries`), clang-cl의 `-Wthread-safety` 동작과 `-fsanitize=thread` 거부(로컬 실측), MetaCompiler `Atomic<...>` 확인(SY0 통과 조건으로 옮김).

## 18. 개정 이력

- 개정 1(2026-11-18): 초안. UE 5.8.4 조사, Rocks 현황과 중복 10건, 배치·잠금·신호·원자·lock-free·회수·진단·시험 설계, 단계 SY0–SY11, 결정 D1–D28.
- 개정 2(2026-11-18): 교차검증 1차 반영, 결정 확정.
  - 정확성: ConditionVariable 대기 bit 경쟁(알림 유실)을 고치고 대기 표시 공통 규칙과 콜백 있는 `WakeMultiple`·`WakeAll`을 정했다(8.6·10.3절, D9·D29). AutoResetEvent·Semaphore·OnceFlag·ManualResetEvent·ReadWriteMutex·ReclaimDomain에 같은 규칙을 적용했다.
  - 계약: ReadWriteMutex 읽기 재진입 금지와 Debug 검출(D7), `InitiallyLocked` 해제 예외, task 대기 검사와 lane 순위(D21), AudioRT 잠금 금지(D27).
  - lock-free: ABA를 `[태그 40 | 색인 24]`와 태그 폭 static_assert로(D18), ReclaimDomain을 SRCU로 명시하고 동기화 대기 bit로 독자 추가 비용을 없앴다(D17).
  - 기반: TLS 파괴 뒤 임시 레코드(D31), scratch 대신 침투 목록(D5), `SpinBackoff`(D8), 훅 호출 지점과 재진입 guard(D22·D30), `Atomic<T>`·`AtomicRef<T>` 한 계열과 순서 기본값 제거(D15), Semaphore 크기·최대값 범위(D12), `CallOnceUntilSuccess`(D13), `FetchUpdate`.
  - 시험: `StepPolicy`·TestAccess 설치로 `ROCKS_TESTING` 정책과 맞춤(D24), 가짜 OS 대기로 시한 시험, 대기 표시 경쟁 시험, 대기 bit 해제 시점 기대값, TLS 파괴 뒤 대기 시험.
  - 빌드·도구: `Synchronization` 시스템 라이브러리, thread safety 플래그 자리와 ThirdParty 제외, MetaCompiler 허용 목록(D20·D32), TSan의 Windows 거부 실측과 fence 한계(D25).
  - 단계: 임시 이름공간 없이 SY2 제자리 교체(D28), thread safety 매크로를 SY0으로, SY7 선행에 SY2, MetaCompiler `Atomic<...>` 확인을 SY0 통과 조건으로.
  - 사실 정정: UE ParkingLot `Reserve` 시점·재진입 검사 줄·scratch 할당기·TLS 파괴 경로, `FAAArrayQueue` 사용처(`LocalWorkQueue.h`), 사용 수 재계산, `EV` 36행, `TQueue`·`TCircularQueue` 제외 목록, IntrusiveMutex 대기 bit 해제 시점. Rocks 쪽은 D3D11·D3D12 줄 번호, TraceCapture Mutex 파일 수, `DeviceOwnership` 분류, RenderBase 인용 줄, 빠진 중복(`FrameArena`, `LogSystem` 소비자 spin·`WaitForSpace`, `TraceEvents` double 더하기, 시험의 폴링 반복).
  - Task 설계 문서가 없음을 적고 work-stealing·overflow·oversubscription을 "Task 설계(후속 문서)"로 남겼다(D33).
- 개정 3(2026-11-22): 실시간 깨우기 `RealtimeWakeSignal`(9.6절, D34, 단계 SY12)과 6절 대응표 한 줄. [오디오 시스템](AudioSystem.ko.md) AD26·PR21의 요구다.
