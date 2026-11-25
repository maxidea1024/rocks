## 2. 목적과 범위

### 2.1 목적

- 엔진·도구 코드가 필요로 하는 프로세스 내부 동기화를 한 곳에서 제공한다. 사용자 말 "코어에 Event, Semaphore 같은 동기화 객체들이 없는데 나중에라도 필요하지 않나? ReadWriteLock이라던가", "락프리 지원도 없는 것 같고"에 답한다.
- 이미 계획된 작업이 기대는 기반을 미리 정한다.
  - GC 규모 작업 GC-S1·GC-S4는 `Atomic` 표식과 블록 work-stealing을 요구한다(`Docs/Design/GarbageCollectionScale.ko.md` 152–195행).
  - CVar·Config는 snapshot 포인터 게시와 다중 생산자 쓰기 큐를 쓴다(`CVarSystem.ko.md` 294·353–354행).
  - 앱 정보 D4는 프레임마다 게시하는 refcount 묶음을 worker에 넘긴다(`AppInfoCommandLine.ko.md` 1307행).
  - 렌더 기반은 완료 통지 큐와 lane 간 전달을 쓴다(`Docs/Design/RenderBase` 2·5.2절).
  - 에셋 lease 해제 큐는 "미리 할당한 큐 항목"을 요구한다(`AssetSystem` 337행). 이것은 호출자 소유 노드의 MPSC 큐다.
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

- **프로세스 간 동기화.** UE `FSystemWideCriticalSection`(`CS` 16행, `WPM` 120행)과 이름 있는 프로세스 간 semaphore(`WPP` 2111–2130행)는 대응하지 않는다. 에셋 쓰기 잠금은 이미 파일 잠금(`FileLock`, `AssetSystem` 270행)으로 한다.
- **AutoRTFM 전용 계열.** `TransactionallySafeMutex`·`TransactionallySafeSharedMutex`·`TransactionallySafeRecursiveMutex`·`TransactionallySafeSharedRecursiveMutex`·`TransactionallySafeMutexPool`(`Public/Async/`)과 `TransactionallySafeMpscQueue`·`TransactionallySafeSpscQueue`(`Public/Containers/`)는 다루지 않는다. 이 타입들은 `AutoRTFM::IsClosed()`·`AutoRTFM::Close`로 트랜잭션 안의 잠금을 미루거나 되돌리는 장치다(`Public/Async/TransactionallySafeMutex.h` 45–119행). Rocks에는 AutoRTFM(트랜잭션 메모리 기반 스크립트 롤백)이 없고 도입 계획도 없다. `TAtomicRefCount`(`Public/Async/AtomicRefCount.h`)도 같은 이유로 AutoRTFM 부분은 빼고 "원자 참조 계수"라는 뜻만 11절에서 다룬다.
- **작업 scheduler 내부 큐와 worker 보충.** work-stealing deque(UE `TWorkStealingQueue2`, `LQ` 33–157행), overflow 큐(UE `FAAArrayQueue`, `LQ` 193행), 대기 중 worker 보충(UE oversubscription)은 **Task 설계(후속 문서)** 의 몫이다. 이 문서는 그 큐가 쓸 원자·회수 기반과 훅 자리만 정한다(10.5·12.5절). Task 설계 문서는 아직 없다. `Docs/Design`에는 Task 문서가 없고, 지금 있는 것은 `EngineImplementationPlan.ko.md` 423·719행의 원칙 문장뿐이다(D33).
- **GPU 동기화.** fence·timeline은 RenderBase(`Graphics/FenceTimeline.h`)다.
- **이벤트 풀과 가상 인터페이스.** UE는 OS 이벤트 생성 비용 때문에 `FEvent`를 풀에서 빌려 준다(`Private/Misc/EventPool.h` 71–115행, `Private/HAL/ThreadingBase.cpp` 795–863행). Rocks 이벤트는 1 byte 값이라 풀이 필요 없다(9.1절).
