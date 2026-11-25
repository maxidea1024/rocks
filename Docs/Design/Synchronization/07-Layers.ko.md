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
| Android(API 26 이상) | `syscall(SYS_futex, FUTEX_WAIT_PRIVATE)`(상대 시한, CLOCK_MONOTONIC 기준), `FUTEX_WAKE_PRIVATE` | UE는 절대 시한 `FUTEX_WAIT_BITSET_PRIVATE`다. Rocks는 호출자(`WaiterSignal::WaitFor`)가 깰 때마다 최초 시한의 남은 시간을 다시 계산하므로 상대 시한으로 같은 뜻이 된다(개정 4). `EINTR`·`EAGAIN`·`ETIMEDOUT`은 다시 검사한다 |
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
