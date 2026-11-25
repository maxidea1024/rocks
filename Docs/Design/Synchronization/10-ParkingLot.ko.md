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
