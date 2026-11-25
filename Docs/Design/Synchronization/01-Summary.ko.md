## 1. 요약

- **지금 Rocks에 있는 것은 OS 잠금 wrapper와 그 위의 이벤트 두 개다.** `Platform/Sync.h`의 `Mutex`는 Windows SRWLOCK·POSIX `pthread_mutex_t`를 64 byte 저장소에 감싼다. `ManualResetEvent`·`Semaphore`는 그 Mutex와 `ConditionVariable` 위의 공용 구현이다(4.1절). 읽기쓰기 잠금·재진입 잠금·EventCount·Once·`Atomic<T>`·범용 lock-free 컨테이너·메모리 회수는 없다.
- **없어서 모듈마다 따로 만들었다.** spin 잠금 2곳, 시한 TryLock 1곳, 태그 색인 free list 2곳, 독자 세대 대기(SRCU류) 2곳, Dekker 잠듦·깨움 1곳, 원자 상한·포화·갱신 연산 6곳, 잠금으로 감싼 고유 번호 발급 3곳, 수작업 Once 1곳, 통째로 가져가는 침투 스택 1곳, 양보로 도는 바쁜 대기 2곳이다(4.5절). 공용 기능 원칙에 따라 이 문서의 장치로 모은다.
- **UE 5.8은 parking lot 위에 작은 잠금을 쌓는다.** 1 byte `FMutex`, 4 byte `FSharedMutex`, 8 byte `FRecursiveMutex`, `FEventCount`, `FManualResetEvent`가 모두 주소 키 대기열(`ParkingLot`)을 쓴다. 불공정(barging)이고, 대기자가 없을 때만 CPU pause를 40번까지 돈 뒤 잠든다(3.2·3.3절). HAL의 `FEvent`·`FCriticalSection`·`FRWLock`은 OS 객체 wrapper로 남아 있고 사용 수는 아직 더 많다(3.11절).
- **Rocks는 UE 구조를 따르되 자리는 PlatformBase다.** 동기화 객체는 지금처럼 PlatformBase에 두고, OS 잠금 wrapper를 parking lot 기반 공용 구현으로 바꾼다. OS별 코드는 "한 스레드가 주소를 기다리고 다른 스레드가 깨우는" 수단 하나로 줄인다. PlatformBase 자신(`File.h` 285행, `VirtualMemory.h` 70행)도 잠금이 필요하므로 Core에 두면 Mutex가 두 종류가 된다(7절, D2).
- **lock-free 컨테이너와 회수는 Core다.** 우선 "회수가 필요 없는" 설계(미리 잡은 고정 풀 + 색인 + 태그)를 쓴다. 회수가 꼭 필요할 때만 SRCU 방식(도메인별 두 세대 카운터)의 회수 영역을 쓴다. 독자 경로는 RMW 두 번뿐이고 해제 쪽 대기를 위한 비용을 더하지 않는다. hazard pointer는 두지 않는다(12.3절, D17).
- **대기 표시는 한 규칙으로 다룬다.** bucket 밖에서 세운 "대기자 있음" 표시는 CanWait에서 다시 확인하고, 지우기는 bucket 잠금 아래 콜백에서만 한다. 모든 잠금·신호 타입이 이 규칙을 따른다(10.3절, D29).
- **원자 접근은 `Atomic<T>`·`AtomicRef<T>` 한 계열이다.** UE는 자체 원자 타입을 버리고 `std::atomic`으로 갔다. Rocks는 STL을 쓰지 않으므로 같은 역할의 `platform::Atomic<T>`와 기존 객체를 원자로 보는 `AtomicRef<T>`를 둔다. 연산자 overload도 순서 기본값도 없이 순서를 늘 이름으로 적는다. 지금의 자유 함수는 이 둘의 내부 구현(`platform::detail`)으로 내린다(11절, D15).
- **시험은 sleep 없이 순서를 만든다.** parking lot에 "잠들었다"를 알리는 시험 훅을 두고, lock-free 알고리즘에는 단계 지점을 두어 interleaving을 정해진 순서로 돌린다. 시한은 OS 대기까지 가짜로 바꿔 실제 시간을 쓰지 않는다. 오래 도는 stress는 기본 실행에서 뺀다. 정적 검사는 Clang thread safety analysis를 쓴다(13·14절).
