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
