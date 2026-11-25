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
- "경쟁에서 진 쪽이 버리는" 지연 생성(렌더 셰이더, `Docs/Design/RenderBase` 9.2절·9.9절 SX10)은 Once가 아니라 `Atomic<T*>::CompareExchangeStrong`이다. 생성이 비싸거나 부작용이 있으면 Once를 쓴다.

### 9.5 대기 시간과 결과

- 시간 인자는 지금처럼 millisecond `uint32`이고 `InfiniteTimeout`이 무한, 0이 즉시 검사다. ParkingLot 안쪽은 monotonic tick 시한으로 바꿔 spurious wake 때 시간을 늘리지 않는다(`WaitDeadline.h`와 같은 규칙).
- `WaitResult`는 `Signaled`·`TimedOut` 둘로 줄인다. OS 대기 수단의 예상 밖 실패는 이제 `WaiterSignal` 한 곳뿐이고, 거기서 다시 시도할 수 없는 오류는 Fatal이다(D14). 하위호환 없이 `Failed`를 지운다.

### 9.6 RealtimeWakeSignal(개정 3)

실시간 스레드와 그 스레드를 깨우는 쪽 사이의 신호다. 첫 사용처는 [오디오 시스템](../AudioSystem.ko.md) 3.2절의 장치 콜백 → AudioRT 깨우기다. 장치 콜백은 OS·오디오 라이브러리가 소유한 높은 우선순위 스레드이고, AudioRT는 잠금을 잡지 않는다(D27). 9.1절 `AutoResetEvent`는 parking lot 위에 있어 Signal이 bucket 잠금을 잡을 수 있고, `WaiterSignal`의 macOS판은 `pthread_mutex_t`를 잡는다(7.3절). 그래서 둘 다 이 자리에 쓸 수 없다. 지금의 `ManualResetEvent`·`Semaphore`도 Signal·Release 안에서 `Mutex`를 잡는다(4.1절).

- **모양.** 대기자는 한 번에 하나다(고정 스레드는 아니다). 둘이 동시에 기다리면 계약 위반이고 Debug·Development에서 Fatal로 드러낸다. 자동 리셋이다. 상태는 `Atomic<uint32>` 하나다: 0 = 비어 있음, 1 = 신호, 2 = 대기 중.
- **`Signal() noexcept`.** 상태를 1로 교환하고, 이전 값이 2였을 때만 OS 깨우기를 부른다. 잠금·할당이 없고, 대기자가 없으면 시스템 호출도 없다. 연속 Signal은 신호 하나로 합쳐진다.
- **`Wait(timeoutMs) noexcept -> WaitResult`.** 상태가 1이면 0으로 바꾸고 바로 돌아간다. 0이면 2로 바꾸고 OS 대기(값이 2인 동안 잠듦)에 들어가며, 깬 뒤 다시 확인한다. 시한·spurious wake 규칙은 9.5절과 같다.
- **OS 수단.** Windows `WaitOnAddress`·`WakeByAddressSingle`, Android futex(7.3절과 같다). macOS는 신호 객체마다 Mach semaphore 하나(`semaphore_create`·`semaphore_signal`·`semaphore_timedwait`)다. `semaphore_signal`은 사용자 공간 잠금을 잡지 않는 커널 호출이라 Core Audio 콜백에서 쓰는 관례적 수단이다. `os_sync_wait_on_address`는 최소 버전 13.0에서 쓸 수 없다(7.3절). Mach semaphore는 커널 객체라 만들기가 실패할 수 있으므로 생성은 `Result<RealtimeWakeSignal> Create() noexcept`다(Windows·Android는 실패하지 않는다).
- **위치.** PlatformBase `Platform/Sync/RealtimeWakeSignal.h`, OS 구현은 `Private/<OS>/`. 가짜 OS 대기 seam(14.2절)을 같이 쓴다.
- **시험.** Signal 뒤 Wait, 대기 중 Signal, 연속 Signal 합침, 시한, 두 대기자 검출을 가짜 OS 대기로 결정적으로 확인한다.
- **UE와 다른 점.** UE 오디오 렌더 스레드는 `FEvent`(Apple은 pthread 조건 변수)를 기다린다(`Engine/Source/Runtime/AudioMixerCore/Private/AudioMixer.cpp`). Rocks는 D27 때문에 잠금 없는 신호로 둔다.
