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
