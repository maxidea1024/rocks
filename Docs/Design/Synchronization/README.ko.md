# 동기화 객체와 lock-free 계층 설계

기준일: 2026-11-25, 개정 4(착수 — 사용자 결정으로 오디오 세션이 SY0·SY1·SY2·SY7·SY12를 구현한다, 15절). 개정 3(2026-11-22, 9.6절 `RealtimeWakeSignal` — [오디오 시스템](../AudioSystem/README.ko.md) 요구). 개정 2(2026-11-18): 교차검증 1차 반영, 결정 확정. 사용자 지시("설계문서만 작성해놔")로 쓴 설계이고 구현하지 않는다. 결정은 권장안으로 확정했다(16절). 원칙과 부딪히는 항목과 범위가 큰 항목은 16절에 따로 표시했다. 개정 2의 반영 내용은 18절에 있다.

**착수 시점.** 사용자가 2026-11-25에 착수를 정했다. 오디오 장치 출력(오디오 AU2)이 `BoundedSpscQueue`·`RealtimeWakeSignal`을 기다리므로 오디오 세션이 SY0 → SY1 → SY2 → SY7 → SY12를 이 순서로 구현한다. Mutex 제자리 교체(SY2)가 엔진 전체에 닿으므로 작업은 별도 브랜치 `feature/synchronization`(별도 worktree)에서 하고 단계마다 빌드·시험한 뒤 병합한다. SY3–SY6·SY8–SY11은 아직 착수하지 않았다(처음 설계 지시는 "설계문서만 작성해놔"였다).

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

## 장 목차

- [1. 요약](01-Summary.ko.md)
- [2. 목적과 범위](02-PurposeAndScope.ko.md)
- [3. UE 5.8 조사](03-UeSurvey.ko.md)
- [4. Rocks 현황](04-RocksState.ko.md)
- [5. 원칙](05-Principles.ko.md)
- [6. 이름과 대응표](06-Names.ko.md)
- [7. 계층과 모듈 배치](07-Layers.ko.md)
- [8. 잠금](08-Locks.ko.md)
- [9. 신호](09-Signals.ko.md)
- [10. ParkingLot](10-ParkingLot.ko.md)
- [11. 원자 값](11-Atomics.ko.md)
- [12. lock-free 컨테이너와 할당기](12-LockFree.ko.md)
- [13. 디버그 지원](13-Diagnostics.ko.md)
- [14. 시험 계획](14-Tests.ko.md)
- [15. 구현 단계](15-Phases.ko.md)
- [16. 결정 목록](16-Decisions.ko.md)
- [17. 미확인 사실](17-Unverified.ko.md)
- [18. 개정 이력](18-Revisions.ko.md)
