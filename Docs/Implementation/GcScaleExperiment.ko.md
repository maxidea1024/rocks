# GC 대규모 처리 실험 결과 (GC-S0·GC-S1·GC-S2)

기준일: 2026-11-16. 브랜치 `experiment/gc-scale`(기준 `795761a`). 설계는 [GC 대규모 처리 보강](../Design/GarbageCollectionScale.ko.md)이다.

이 문서는 설계 6절의 단계 중 **GC-S0(측정 기준과 장기 smoke)**, **GC-S1(잠금 없는 직렬 marker·배치 visitor·prefetch)**, **GC-S2(시간 기준 예산)** 의 실행 결과다. 사용자 지시(2026-11-16)로 범위를 이 세 단계로 줄였으며 **GC-S3·GC-S4(이 실험의 Phase 2·3)는 착수하지 않았다.** GC-S5–S7과 M1–M7도 범위 밖이다.

## 1. 측정 환경

- Windows 11 Pro, 논리 코어 20개, clang(저장소 고정 도구 체인) **Development(-O2)** 구성.
- 실행 우선순위는 보통(normal)이다. 기준(Phase 0)과 Phase 1 벤치는 각각 다른 빌드·벤치·soak와 겹치지 않게 단독으로 돌렸다.
- 측정 도구는 `Programs/GcBench`(`GcBench.exe`, 인자 없이 기본 표)다. 할당자는 벤치 전용 `BenchAllocator`(플랫폼 정렬 할당을 직접 쓰는 O(1) 해제, 범주별 바이트 계수)를 써서 할당자 비용을 GC 비용에서 떼었다. 할당자 비교는 `--allocators`로 따로 쟀다.
- scheduler 경로는 worker 8개로 쟀다.
- 수치는 실행마다 ±10–20% 흔들린다. 정지형 Collect는 반복 측정의 중앙값(100K 5회, 1M 3회)이고 증분 수집은 cycle 1회다.

## 2. 시나리오

| 항목 | 내용 |
|---|---|
| 그래프 | 256개 Outer 묶음(머리 객체 + 이름 있는 inner 255개). 노드마다 0–2K개(평균 K) 무작위 간선을 `ObjectRefArray<Object>`에 `AddReference`로 넣는다 |
| 규모 | 살아 있는 객체 100K·1M, 평균 간선 4·8. 추가로 1M·간선 4·잎 객체 50%(강한 참조 없는 타입) |
| 루트 | 묶음 머리 전부 + 무작위 1000개(1M에서 약 4.9천 개) |
| 첫 Collect | 도달하지 못한 객체(약 e^-K)를 회수한 뒤부터 잰다 |
| Collect | Begin(작업 큐 예약·epoch·루트 수집)·표식·sweep·회수·전체 |
| 증분 | 기본 `GcBudget`, 시간 한도만 둔 2ms·0.5ms 예산. Step 횟수와 Step 한 번의 시간 분포(p50·p95·p99·최대) |
| scheduler | `StepCollection(..., scheduler)` worker 8개, 예산 무제한 |
| churn | 살아 있는 객체의 10%만큼 쓰레기 묶음(간선은 산 그래프로)을 만든 뒤 Collect — 회수 시간 |
| 메모리 | 범주별 바이트(Slot chunk·객체·참조 배열·이름·기타)와 최고치 |

## 3. 결과: 기준(Phase 0) 대 Phase 1

### 3.1 정지형 Collect (중앙값, ms)

| 시나리오 | 단계 | Begin | 표식 | sweep | 전체 | churn 회수 |
|---|---|---:|---:|---:|---:|---:|
| 100K · 간선 4 | 기준 | 0.88 | 72.1 | 0.88 | 74.1 | 4.39 (9.8K개) |
| | Phase 1 | 0.61 | **20.0** | 0.64 | 21.4 | 1.78 |
| 100K · 간선 8 | 기준 | 0.91 | 132.0 | 1.03 | 133.6 | 3.22 (10K개) |
| | Phase 1 | 0.49 | **22.4** | 0.50 | 23.5 | 1.51 |
| 1M · 간선 4 | 기준 | 8.46 | 779.0 | 7.86 | 795.5 | 17.8 (98K개) |
| | Phase 1 | 8.18 | **259.4** | 8.21 | 275.9 | 20.1 |
| 1M · 간선 8 | 기준 | 7.96 | 706.7 | 6.46 | 719.7 | 13.8 (100K개) |
| | Phase 1 | 7.43 | **320.8** | 6.88 | 335.9 | 14.0 |
| 1M · 간선 4 · 잎 50% | 기준 | 7.05 | 273.7 | 8.62 | 289.4 | 14.6 (80K개) |
| | Phase 1 | 8.28 | **134.9** | 7.96 | 151.4 | 15.2 |

표식 단축은 100K에서 3.6·5.9배, 1M에서 3.0·2.2배다. 1M·간선 8의 기준 표식(707ms)이 간선 4(779ms)보다 짧은 것은 실행 흔들림이다(첫 Collect에서는 725 대 818ms). Begin·sweep은 Phase 1에서 바꾸지 않은 전체 Slot 순회라 그대로다(GC-S3 몫).

### 3.2 증분 수집 (cycle 1회)

기준의 기본 예산은 Step당 객체 64개·soft 1ms였고 Phase 1의 기본 예산은 시간 1ms(객체 상한 2^20)다.

| 시나리오 | 예산 | 단계 | Step 수 | p50 (us) | p95 | p99 | 최대 | cycle 합 (ms) |
|---|---|---|---:|---:|---:|---:|---:|---:|
| 100K · 4 | 기본 | 기준 | 3,098 | 3.2 | 91.8 | 258.3 | 1,659 | 115.0 |
| | | Phase 1 | 20 | 1,006 | 1,021 | 1,031 | 1,031 | 19.1 |
| | 2ms | 기준 | 50 | 2,001 | 2,030 | 2,364 | 2,364 | 99.6 |
| | | Phase 1 | 10 | 2,011 | 2,024 | 2,024 | 2,024 | 19.0 |
| 100K · 8 | 기본 | 기준 | 3,126 | 18.5 | 115.8 | 157.2 | 1,057 | 142.4 |
| | | Phase 1 | 27 | 1,018 | 1,032 | 1,033 | 1,033 | 26.8 |
| | 2ms | 기준 | 70 | 2,001 | 2,003 | 2,007 | 2,007 | 139.1 |
| | | Phase 1 | 16 | 2,012 | 2,037 | 2,037 | 2,037 | 29.7 |
| 1M · 4 | 기본 | 기준 | 30,941 | 9.3 | 67.5 | 95.7 | 267 | 790.6 |
| | | Phase 1 | 279 | 1,016 | 1,041 | 1,069 | 1,300 | 292.4 |
| | 2ms | 기준 | 331 | 2,001 | 2,001 | 2,022 | 2,074 | 668.7 |
| | | Phase 1 | 128 | 2,014 | 2,036 | 2,045 | 2,046 | 263.1 |
| 1M · 8 | 기본 | 기준 | 31,246 | 21.1 | 64.0 | 84.9 | 261 | 923.6 |
| | | Phase 1 | 323 | 1,021 | 1,048 | 1,057 | 1,094 | 338.0 |
| | 2ms | 기준 | 598 | 2,001 | 2,008 | 2,059 | 2,329 | 1,203.4 |
| | | Phase 1 | 166 | 2,021 | 2,048 | 2,063 | 2,070 | 340.5 |

- 기준의 기본 예산은 Step이 짧지만(p99 0.1–0.26ms) 1M cycle 하나에 Step 3.1만 번이 걸렸다(60fps에서 약 8.6분 — 설계 3.2절 7번과 일치한다).
- Phase 1의 기본 예산(1ms)은 1M cycle을 279–323 Step에 끝낸다. 2ms 예산은 128–166 Step이다.
- soft deadline 초과는 2ms 예산에서 p99 2–3%(최대 2.07ms), 1ms 예산에서 최대 1.30ms였다. 시계를 객체 128개마다 보므로 생기는 초과와 Begin이 들어간 첫 Step이 원인이다.
- 시간 예산 cycle의 Begin(1M 6.5–9.3ms)은 첫 Step 전 `BeginCollection` 한 번에 들고 예산 밖이다.

### 3.3 scheduler 경로 (worker 8개, 표식 ms)

| 시나리오 | 기준 | Phase 1 |
|---|---:|---:|
| 100K · 4 | 490.8 | 394.6 |
| 1M · 4 | 4,838.8 | 3,811.5 |
| 1M · 8 | 4,833.6 | 4,062.4 |

직렬 Phase 1(259ms)보다 15배 느리다. 객체마다 `Function`을 할당·제출하고 64개마다 기다리는 구조(설계 3.2절 6번)를 Phase 1에서 바꾸지 않았기 때문이다(잠금만 간선마다에서 배치마다로 줄었다). GC-S4 몫이다.

### 3.4 메모리 (1M · 간선 4, 첫 Collect 전)

| 범주 | MiB | 객체당 |
|---|---:|---:|
| Slot chunk(표 포함) | 183.4 | 192.3 B/slot |
| 객체 본체 | 45.8 | |
| 참조 배열 버퍼 | 81.4 | |
| 이름 기록·색인 | 54.0 | |
| 기타(작업 큐 등) | 55.2 | |
| 합계 | 419.7 | 440 B/object |

Phase 1은 메모리 배치를 바꾸지 않았다(같은 수치). Slot 정보 192B는 4절 목표(≤ 64B)의 3배다.

### 3.5 할당자 비교 (기준, 살아 있는 객체 20K, churn 10% × 5회)

| 할당자 | 생성+연결 (ns/객체) | 회수 (ns/객체) | 종료 회수 (ms) |
|---|---:|---:|---:|
| 플랫폼(BenchAllocator) | 637 | 121 | 3.7 |
| `SystemAllocator` | 839 | 4,173 | 976 |
| `BinnedAllocator`(System 위) | 702,409 | 3,710 | 835 |

`SystemAllocator`·`BinnedAllocator`는 해제(그리고 Binned는 할당)에서 원장을 선형 검색하므로 객체 수에 비례해 느려진다. 20K에서도 회수가 플랫폼의 30–35배이고 Binned의 할당은 객체당 0.7ms다. 100K 이상은 끝나지 않을 만큼 느려 재지 않았다. 설계 3.2절 8번과 GC-S6의 근거다.

## 4. 장기 smoke 시뮬레이터 (GcSoak)

### 4.1 설계

`Core/Source/Developer/GcSoak`(도구 모듈)의 `RunGcSoak`이 결정적 seed로 tick을 돌린다. tick마다 2–9개 변경을 고르고 끝에 수집 한 걸음을 둔다.

- 변경: 생성(노드 75%·잎 25%, 40%는 무작위 Outer 아래, 이름 있음/없음, 45%는 루트 유지), 참조 필드 쓰기, `ObjectRefArray` 추가·삽입·교체·위치 제거·handle 제거·비우기, `TryPin`·루트 반환, `RequestDestroy`, `Discard`, backend lease 획득·반환, encode 고정 획득·반환, Borrow를 쥔 채 Begin·Step·Collect가 Busy인지 확인.
- 수집: Idle이면 12% Begin, 2% Collect, 3% DrainRetired. 진행 중이면 Step(객체 1–64개 예산 또는 시간 100us), 0.25% 확률로 Cancel(Marking이면 성공, cutoff 뒤면 Busy여야 한다).
- cross-thread 반환: RootRef·BackendLease·EncodePin 반환의 일부를 helper 스레드에 넘긴다. 넘긴 반환은 다음 수집 걸음에서 루트로 둔 탐침 객체(수동 Trace 타입)가 trace될 때 **표식 slice 안에서** 실행되거나(Trace가 helper를 깨우고 끝날 때까지 기다린다), 탐침이 trace되지 않으면 걸음 뒤에 실행된다. 순서는 semaphore hand-off로만 정하며 시간·경쟁 운에 기대지 않는다.
- 모델: 객체마다 간선(first·second·items)·Outer·루트·보유 계수·파괴 요청·Discard 여부를 따로 들고 다닌다. API 결과는 Registry가 알려 준 상태(`InspectLifecycle`)와 모델로 예측한 성공·오류 코드와 대조한다.

16 tick마다와 cycle이 끝날 때마다 전체를 대조한다.

| 불변식 | 내용 |
|---|---|
| (a) 조기 회수 없음 | 모델에서 루트로 닿는 객체는 Alive 또는 DestroyRequested다(cutoff·Retired·회수면 실패). 파괴 요청 상태도 모델과 같다 |
| (a') 내용 일치 | Alive 노드의 필드·배열 handle이 모델과 같다. 살아 있는 객체의 간선이 회수된 객체를 가리키지 않는다 |
| (b) 누수 없음 | 모델에서 닿지 않고 보유 계수가 없다고 관찰된 객체는, 그 관찰 뒤 시작한 cycle 하나가 끝나면 회수되어 있다. 장벽 shade·루트·새 inner가 닿은 객체와 그 하위는 관찰을 다시 센다(floating garbage는 다음 cycle) |
| (c) 통계 일치 | `Stats`의 루트·Borrow·backend 보유·encode 고정·Reserved, Alive+DestroyRequested+Retired 수가 모델과 같고, 소멸자 수가 관찰한 회수 수와 같다(Debug `Stats`는 Registry 내부 회계도 대조한다) |
| (d) 예상 밖 오류 없음 | 모든 API 결과가 예측과 같다 |
| (e) 종료 0 | 모든 보유를 놓고 Collect 두 번 뒤 모든 객체가 회수되고 통계가 0, 소멸자 수 = 생성 수, Shutdown 성공, allocator 살아 있는 할당 0 |

음성 대조로 검사가 실제로 결함을 잡는지 확인했다(코드는 되돌렸다). 표식 진행 뒤의 삽입 장벽을 끄면 첫 cycle 안에 (a) "reachable object was cut off"로 멈추고, 지연 목록의 회수 가능 전이(`WakeDeferredLocked`)를 끄면 (b) "garbage survived a complete cycle"로 멈춘다.

### 4.2 실행

| 실행 | 설정 | tick | 생성 | 회수 | cycle(시작/완료) | slice 안 cross-thread | 결과 |
|---|---|---:|---:|---:|---:|---:|---|
| 시험 `GcSoak_SerialCollectionKeepsModelInvariants` | 직렬, 객체 예산, seed 3개 × 3000 tick, 객체 600 | 9,000 | | | | | PASS(Debug·Development) |
| 시험 `GcSoak_SchedulerCollectionKeepsModelInvariants` | worker 3개, 2000 tick | 2,000 | | | | | PASS(Debug·Development) |
| 기준 seed 11 | 직렬, 객체 3000 | 300,000 | 180,078 | 177,080 | 2,338 / 1,980 | 437 | PASS |
| 기준 seed 12 | 직렬, 객체 3000 | 300,000 | 181,801 | 178,866 | 2,299 / 1,978 | 442 | PASS |
| 기준 seed 13 | scheduler worker 4개 | 200,000 | 118,544 | 115,575 | 1,557 / 1,333 | 294 | PASS |
| 기준 seed 14 | 시간 예산 100us | 200,000 | 121,509 | 118,532 | 17,026 / 16,875 | 2,974 | PASS |
| Phase 1 seed 11 | 직렬, 객체 3000 | 300,000 | 181,047 | 179,831 | 4,350 / 3,699 | 815 | PASS |
| Phase 1 seed 12 | 직렬, 객체 3000 | 300,000 | 179,673 | 176,728 | 4,281 / 3,632 | 782 | PASS |
| Phase 1 seed 13 | scheduler worker 4개 | 200,000 | 122,603 | 120,928 | 2,958 / 2,523 | 575 | PASS |
| Phase 1 seed 14 | 시간 예산 100us | 200,000 | 123,240 | 120,235 | 22,193 / 22,137 | 3,609 | PASS |

기준과 Phase 1 모두 1M tick(직렬 600K, scheduler 200K, 시간 예산 200K)씩 통과했다. Phase 1은 cutoff slice가 sweep 없이 끝나 cycle이 짧아졌고 같은 tick에 cycle 수가 약 2배다. 생성 수와 회수 수의 차이는 끝에 남은 추적 객체(최대 3008개)이고 종료 대조에서 0이 됐다.

**기준 GC에서 찾은 결함은 없다.** 시뮬레이터를 만드는 동안의 실패는 모두 모델 쪽 오류였다(비동기 반환의 계수 반영 시점, Discard의 무동작 반복과 부분 트리 판정).

World의 `StepGarbageCollection`을 쓰는 World 단위 변형은 만들지 않았다(EngineTests 대상이 필요하다. 4.3절).

### 4.3 남은 시험 구멍

- World 단위 변형(Actor·Component 수명과 `World::StepGarbageCollection`)이 없다.
- 패키지 정책 Registry(관찰자 포함)는 시뮬레이터 범위 밖이다(`ReferenceObserverModelTests`가 따로 덮는다).
- `ObjectRefMap`·`StructRefArray` 편집은 넣지 않았다.
- slice 안 cross-thread 반환은 전체 반환의 약 1–10%다(탐침이 trace될 때만 실행된다).

## 5. Phase 1 구현 (GC-S1·GC-S2)

### 5.1 7.1절 감사 결과

직렬 marker가 `gate_` 없이 읽고 쓰는 칸은 Slot의 generation·state·mark·object·type·outer와 자식 고리(firstChild·nextSibling), chunk 표, `gcWork_`, `markEpoch_`다. slice와 겹칠 수 있는 진입점을 확인한 결과는 다음과 같다.

| 진입점 | 스레드 | slice 중 동작 | marker 칸과의 겹침 |
|---|---|---|---|
| `ReleaseRoot`(`RootRef::Reset`) | 아무 스레드 | gate_ 아래 roots·`tally_.roots`, `WakeDeferredLocked` | 없음(roots는 marker가 읽지 않는다) |
| `ReleaseBackend`·`ReleaseEncodePin` | 아무 스레드 | gate_ 아래 보유 계수·tally, `WakeDeferredLocked` | 없음 |
| `WakeDeferredLocked` | 위 경로 | state·queue를 읽고 회수 목록 고리(nextInList·prevInList)와 목록 머리만 바꾼다 | 없음(state는 읽기만 한다) |
| `RetainRoot`(`RootRef::Clone`) | 아무 스레드 | `processing_`이면 Busy | 없음 |
| `ReleaseBorrow` | 아무 스레드 | slice 진입 조건이 Borrow 0이라 slice와 겹치지 않는다 | 없음 |
| 생성·간선 쓰기·장벽(`ShadeLocked`)·Discard·Commit | 소유 스레드 | Trace 콜백 재진입은 `processing_`으로 Busy | 없음 |
| `IsCutoff`·`CheckLive`·`InspectLifecycle` | 소유 스레드(gate 아래) | Sweeping에서만 mark를 읽는다. cutoff 게시는 gate 아래다 | 표식 종료 뒤에만 읽는다 |

그래서 `mark`만 relaxed atomic(`Slot::Mark`·`SetMark`)으로 바꾸고 나머지 칸은 그대로 두었다. `GcConcurrencyTests.cpp`가 증분 slice와 Collect 각각에서 탐침 Trace 안 helper 스레드의 RootRef·BackendLease·EncodePin 반환과 RootRef 복제(Busy)를 명시 hand-off로 실행하고 결과(루트를 slice 안에서 놓은 객체는 이번 cycle에 남고 다음 수집에서 회수, lease를 slice 안에서 놓은 쓰레기는 이번 cycle에 회수, 통계)를 단언한다. Asan 실행은 6절을 본다.

### 5.2 바꾼 것

- **배치 visitor.** `TraceVisitor`에 배치 방식(FlushFn)을 더했다. null이 아닌 handle을 256개 버퍼에 방문 순서대로 쌓고 가득 차거나 `Flush()`할 때 넘긴다. 간선마다 방식(VisitFn)은 그대로 둔다.
- **잠금 없는 직렬 marker(`MarkSerial`).** `Collect`와 scheduler 없는 `StepCollection`이 쓴다. 객체마다 Trace → Outer(패키지 정책이면 자식 고리) → `Flush`다. 배치 처리(`MarkHandles`)는 대상 Slot의 첫 cache line과 mark 칸을 먼저 모두 prefetch한 뒤 방문 순서대로 검사·표식·enqueue한다. 작업 큐는 16칸 앞 Slot, 8칸 앞 객체 본체를 prefetch한다. 큐 순서는 간선마다 처리할 때와 같다.
- **prefetch 도우미.** `platform::PrefetchRead`(Platform/Memory.h)를 두고 builtin 정책의 Memory.h 허용 목록에 `__builtin_prefetch`를 더했다.
- **slice 루프.** 객체마다 잡던 잠금(`hasError`, 큐 pop)과 시계 호출을 없앴다. 시계는 객체 128개마다 본다. cutoff 게시와 Reclaiming 전이만 잠근다.
- **sweep.** 작업 단위를 chunk(64칸) 하나로 바꾸고 chunk마다 잠금·시계 한 번으로 분류한다. 슬롯 순서(새 chunk부터, chunk 안은 낮은 번호부터)의 Retire 순서는 그대로다.
- **cutoff slice.** 큐가 빈 것을 예산 안에서 본 slice가 cutoff를 게시하고 sweep 없이 끝난다.
- **scheduler 경로.** 구조(객체마다 task, 64개 batch)는 그대로이고 worker의 표식만 간선마다 잠금에서 flush(최대 256 간선)마다 잠금(`MarkBatchLocked`)으로 바꿨다.
- **GC-S2 예산.** `GcBudget` 기본값을 시간 1ms·객체 상한 2^20(`GcBudget::DefaultMaxObjects`)으로 바꿨다. `maxObjects`는 trace 객체·sweep chunk·회수 객체의 합이다.
- **보고.** `GcReport`에 phase별 시간(Begin·표식·sweep·회수)을 더했다(Phase 0 커밋).
- `FindSlot`·`SlotAt`을 `ObjectRegistryInternal.h`의 inline 정의로 옮겼다(간선마다 부르는 비인라인 호출 제거).

### 5.3 설계와 다르게 한 것

| 항목 | 설계 | 실제 | 이유 |
|---|---|---|---|
| `ValidateStagedEdge`·`TallyEdge` | 배치 visitor의 flush에서 순서대로 처리 | 간선마다 방식을 그대로 쓴다 | Commit은 객체마다 `state.observe`를 바꾸므로 배치로 미루면 Outer 방문이 잘못된 관찰 상태로 집계된다. 간선마다 방식은 순서·집계가 자명하게 같다 |
| 3단 prefetch | Slot·본체·참조 배열(16·8·4칸) | Slot·본체 2단 + 배치의 대상 Slot | 참조 배열 버퍼 위치는 타입 배치를 알아야 한다(M3 몫). 수동 Trace 계약으로는 알 수 없다 |
| sweep | 슬롯마다 잠금 제거 | chunk마다 잠금 한 번 | Retire가 이름 색인·자식 고리·회수 목록·tally를 바꾸며 이것은 cross-thread 반환과 공유한다. 잠금 횟수를 1/64로 줄였다 |
| cutoff slice | 언급 없음 | cutoff를 게시한 slice는 sweep 없이 끝낸다 | chunk 단위 sweep에서는 작은 그래프가 cutoff slice 안에서 모두 분류되어 "cutoff 뒤 미분류 white" 구간이 사라진다. 그 구간을 쓰는 시험 11개(cutoff 차단·Sweeping 중 생성·Discard)의 계약을 유지했다 |
| 시험 기대값 | | `IncrementalGcTests`의 cutoff slice 뒤 Retired 수(1 → 0), `ObjectIdentityGc_RetiredParentWaitsForChildrenBeforeReclaim`는 부모·자식·손자를 서로 다른 chunk에 두도록 재구성 | 위 두 변경의 결과다. 단언하는 계약(부모가 자식보다 먼저 Retire되어도 회수를 미룬다)은 같다 |
| scheduler 경로 | GC-S4 전까지 잠금 edge 함수 유지 | 잠금을 배치마다로 줄였다 | 같은 잠금 아래 같은 처리다 |
| 증분 Begin | | 그대로(예산 밖, 1M 8ms) | GC-S3의 루트 목록 몫이다 |

## 6. 검증

| 항목 | 결과 |
|---|---|
| Debug Foundation 전체 시험 | Phase 1에서 1734개 전부 통과 |
| Development Foundation 전체 시험 | Phase 1에서 1734개 중 GC 무관 2개 실패(`Process_RunTimeoutTerminatesChild`·`SharedPtr_LockRacesWithLastRelease` — 동시 빌드·벤치 부하에서 나는 시간·경쟁 의존 시험), GC·Object 시험(`--filter Object` 158개, `--filter Gc` 39개) 전부 통과 |
| GcSoak 시험(직렬·scheduler) | Debug·Development 통과 |
| 장기 soak | 4.2절(기준·Phase 1 각 1M tick 통과) |
| Development 벤치 | 3절 |
| clang-format 21.1.6 | 통과(`Rocks.ToolTests format`) |
| policy 검사 | 통과(`Rocks.ToolTests policy`, 정책 자체 시험 `self` 113개 통과) |
| Asan Foundation 전체 시험 | Phase 1에서 1734개 전부 통과(GcSoak·GcConcurrency 포함, AddressSanitizer 보고 없음) |
| MetaCompiler·C# 도구 시험 | 해당 없음(MetaCompiler를 바꾸지 않았다). 정책 검사 C# 한 줄만 바꿨다 |

## 7. 실행 기록

- 기준(Phase 0, 커밋 `b5b42a4`): Debug `--filter Gc` 36개 통과, policy·format 통과, Development 벤치·장기 soak(4.2절).
- Phase 1(커밋 `63da487`): Debug·Asan Foundation 전체 1734개 통과, Development `--filter Object`·`--filter Gc` 통과, policy·format·`self` 통과, Development 벤치·장기 soak(4.2절). Development 전체 실행의 GC 무관 실패 2개는 6절에 적었다.
- 실행하지 않은 것: Release 구성 벤치, macOS·Android, EngineTests(World·Engine 시험 — `World::StepGarbageCollection`이 쓰는 `GcBudget` 기본값이 바뀌었으므로 다음 단계 착수 전에 돌려야 한다), World 단위 soak.

## 8. 4절 성능 gate와의 차이 (Phase 1 시점)

| gate | 목표 | Phase 1 (1M) | 상태 |
|---|---|---|---|
| STW 병렬 전체 표식 | ≤ 15ms | scheduler 3,812ms | 미달(GC-S4) |
| 단일 스레드 전체 표식 | ≤ 60ms | 259ms(간선 4)·321ms(간선 8) | 미달(GC-S3 SoA·M2) |
| Begin | ≤ 1ms | 8.2ms | 미달(GC-S3 루트 목록) |
| sweep 후보 판정 | ≤ 1ms | 8.2ms | 미달(GC-S3 bitmap) |
| 증분 Step p99 | ≤ 2ms | 2ms 예산에서 2.04–2.06ms, 1ms 예산에서 1.07ms | 시간 예산으로는 거의 충족. soft 초과 2–3% |
| 증분 cycle | 2ms 예산에서 60프레임 이내 | 128–166 Step | 미달(표식 속도) |
| Slot 정보 | ≤ 64B | 192B | 미달(GC-S3) |
| 기본 `maxSlots` | ≥ 2,097,152 | 1,048,576 | 미달(GC-S3) |

설계 GC-S1 완료 기준 "1M 표식 약 3.5배"는 100K(3.6·5.9배)에서는 넘었고 1M(3.0·2.2배)에서는 못 미쳤다. 1M에서는 객체 본체 → 참조 배열 버퍼 → 대상 Slot(192B, 2–3 cache line)의 미스가 남는다. 모델(3.4절)의 GC-S1 변형이 참조 배열까지 prefetch한 것과 다르다.

## 9. 남은 단계가 더할 것 (추정)

모델(설계 3.4절)과 이번 측정을 근거로 한 해석이며 측정 전 수치다.

- **GC-S3(SoA·루트 목록·bitmap sweep·상한) + M2.** 16B hot 레코드와 mark 바이트로 대상 검사 미스가 192B Slot의 2–3줄에서 1줄로 줄고, u32 큐로 큐 대역폭이 1/4이 된다. 모델은 GC-S1 → GC-S3에서 약 4배(199 → 45ms)였다. 이번 1M 259ms에 같은 비율이면 60–80ms로 단일 스레드 gate(60ms) 근처다. Begin·sweep은 루트 목록·bitmap으로 각각 1ms 아래(모델 0.05·0.03ms)가 된다. M2(잎 표시)는 잎 50% 시나리오(135ms)에서 잎의 trace 호출·큐 왕복을 없앤다. `maxSlots` 기본값 상향과 Slot 64B 목표의 cold 분리도 이 단계다.
- **GC-S4(병렬 표식).** worker별 큐·512칸 블록·atomic 표식으로 모델 기준 단일의 1/4–1/5(8–16스레드 10–13ms)다. 지금 scheduler 경로(3.8s)의 객체당 task 제출이 사라진다. World가 scheduler를 넘기는 변경과 1·2·N worker 종료 시험이 함께 필요하다. 장벽 전용 큐가 있어야 증분 slice 안 병렬 표식과 장벽이 공존한다.
- **GC-S5.** STW 병렬 수집 일정과 파괴·회수의 시간 예산(지금 회수는 객체마다 잠금 두 번, 1M churn 98K 회수 20ms).
- **GC-S6.** O(1) 소유권 판정·스레드 캐시·크기별 slab 할당자. `SystemAllocator`·`BinnedAllocator`로는 1M 규모의 생성·회수를 측정할 수 없었다(3.5절).
- **GC-S7·M1·M3–M7.** 영구 영역·클러스터로 표식 대상 수를 줄이고, schema Trace로 큰 배열을 나눈다. M3(참조 위치 상수)는 이번에 빠진 세 번째 prefetch 단계를 채운다. M1(marker 전용 정적 특수화)은 배치 visitor가 이미 간선당 간접 호출을 없앴으므로 추가 이득은 작을 것으로 본다.
