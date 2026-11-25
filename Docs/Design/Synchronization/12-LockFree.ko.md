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
| `IntrusiveMpscQueue<Node>` | Vyukov 침투형 무한 MPSC. 노드는 호출자 소유이고 `next` 칸을 노드가 갖는다. push는 exchange 하나, pop은 소비자 하나 | 노드 수명은 호출자. 소비자가 하나라 ABA 없음 | 에셋 lease 해제 큐(`AssetSystem` 337행), 지연 정리 목록 |
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
