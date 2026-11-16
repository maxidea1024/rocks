# GC 대규모 처리 실험 결과 (GC-S0–GC-S4·M2)

기준일: 2026-11-16. 브랜치 `experiment/gc-scale`(기준 `795761a`). 설계는 [GC 대규모 처리 보강](../Design/GarbageCollectionScale.ko.md)이다.

이 문서는 설계 6절의 단계 중 **GC-S0(측정 기준과 장기 smoke, Phase 0)**, **GC-S1(잠금 없는 직렬 marker·배치 visitor·prefetch)과 GC-S2(시간 기준 예산)(Phase 1)**, **GC-S3(GC 밀집 자료·루트 목록·비트 sweep·상한)과 M2(잎 타입)(Phase 2)**, **GC-S4(병렬 표식·장벽 큐·World scheduler)(Phase 3)** 의 실행 결과다. GC-S5–S7과 M1·M3–M7은 범위 밖이다.

## 1. 측정 환경

- Windows 11 Pro, 논리 코어 20개, clang(저장소 고정 도구 체인) **Development(-O2)** 구성.
- 실행 우선순위는 보통(normal)이다. 각 단계의 벤치는 이 작업의 다른 빌드·벤치·soak와 겹치지 않게 단독으로 돌렸다.
- Phase 3 측정 때는 같은 기계의 다른 프로세스(장시간 실행 중인 에뮬레이터 등)가 CPU를 많이 써서 첫 Phase 3 전체 벤치가 1.5–3배 느리게 나왔다. 그래서 1M 시나리오는 Phase 2와 Phase 3 실행 파일을 같은 시점에 번갈아 다시 재고(Phase 2 직렬 수치가 앞 측정과 2–7% 안으로 같았다), 100K는 그 뒤 단독으로 다시 쟀다. 이 문서의 Phase 3 수치는 그 재측정이다.
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
| scheduler | Phase 0–2: `StepCollection(..., scheduler)` worker 8개, 예산 무제한. Phase 3: worker 2·4·8·16개의 `Collect(access, scheduler)`(STW 병렬 전체 수집, 소유 스레드도 참여)와 worker 8개의 증분 수집(기본·2ms 예산) |
| churn | 살아 있는 객체의 10%만큼 쓰레기 묶음(간선은 산 그래프로)을 만든 뒤 Collect — 회수 시간 |
| 메모리 | 범주별 바이트(Slot chunk·객체·참조 배열·이름·GC 밀집 자료·기타)와 최고치 |

## 3. 결과: 기준(Phase 0) → Phase 1 → Phase 2 → Phase 3

### 3.1 정지형 Collect (중앙값, ms)

| 시나리오 | 단계 | Begin | 표식 | sweep | 전체 | churn 회수 |
|---|---|---:|---:|---:|---:|---:|
| 100K · 간선 4 | 기준 | 0.88 | 72.1 | 0.88 | 74.1 | 4.39 (9.8K개) |
| | Phase 1 | 0.61 | 20.0 | 0.64 | 21.4 | 1.78 |
| | Phase 2 | 0.006 | 7.4 | 0.002 | 7.4 | 1.56 |
| | Phase 3 직렬 | 0.005 | 8.3 | 0.002 | 8.3 | 1.58 |
| | Phase 3 병렬 ×8 | 0.006 | **2.26** | 0.002 | 2.27 | — |
| 100K · 간선 8 | 기준 | 0.91 | 132.0 | 1.03 | 133.6 | 3.22 (10K개) |
| | Phase 1 | 0.49 | 22.4 | 0.50 | 23.5 | 1.51 |
| | Phase 2 | 0.006 | 11.6 | 0.002 | 11.6 | 1.37 |
| | Phase 3 직렬 | 0.006 | 12.2 | 0.002 | 12.2 | 1.29 |
| | Phase 3 병렬 ×8 | 0.007 | **2.82** | 0.002 | 2.83 | — |
| 1M · 간선 4 | 기준 | 8.46 | 779.0 | 7.86 | 795.5 | 17.8 (98K개) |
| | Phase 1 | 8.18 | 259.4 | 8.21 | 275.9 | 20.1 |
| | Phase 2 | 0.035 | 112.3 | 0.012 | 112.4 | 17.0 |
| | Phase 3 직렬 | 0.049 | 116.7 | 0.012 | 116.8 | 28.1 |
| | Phase 3 병렬 ×8 | 0.038 | **35.7** | 0.017 | 35.7 | — |
| | Phase 3 병렬 ×16 | 0.037 | **23.9** | 0.029 | 24.0 | — |
| 1M · 간선 8 | 기준 | 7.96 | 706.7 | 6.46 | 719.7 | 13.8 (100K개) |
| | Phase 1 | 7.43 | 320.8 | 6.88 | 335.9 | 14.0 |
| | Phase 2 | 0.054 | 142.4 | 0.012 | 142.4 | 14.6 |
| | Phase 3 직렬 | 0.059 | 145.0 | 0.014 | 145.0 | 16.3 |
| | Phase 3 병렬 ×8 | 0.053 | **39.1** | 0.030 | 39.1 | — |
| | Phase 3 병렬 ×16 | 0.055 | **23.7** | 0.020 | 23.8 | — |
| 1M · 간선 4 · 잎 50% | 기준 | 7.05 | 273.7 | 8.62 | 289.4 | 14.6 (80K개) |
| | Phase 1 | 8.28 | 134.9 | 7.96 | 151.4 | 15.2 |
| | Phase 2 | 0.072 | 57.3 | 0.012 | 57.4 | 15.2 |
| | Phase 3 직렬 | 0.122 | 66.2 | 0.014 | 66.4 | 16.3 |
| | Phase 3 병렬 ×8 | 0.074 | **17.2** | 0.028 | 17.3 | — |
| | Phase 3 병렬 ×16 | 0.076 | **11.4** | 0.028 | 11.5 | — |

- 표식: Phase 1이 기준 대비 100K에서 3.6·5.9배, 1M에서 3.0·2.2배 줄였고, Phase 2가 다시 1M에서 2.3배(간선 4)·2.3배(간선 8), 잎 50%에서 2.4배 줄였다. 기준 대비 1M은 6.9배(간선 4)·5.0배(간선 8)·4.8배(잎 50%)다. 1M·간선 8의 기준 표식(707ms)이 간선 4(779ms)보다 짧은 것은 실행 흔들림이다(첫 Collect에서는 725 대 818ms).
- Begin·sweep 후보 판정: Phase 2의 루트 목록과 비트 sweep으로 1M에서 Begin 8.2 → 0.035–0.07ms, sweep 8.2 → 0.012ms(white가 없을 때)다. white가 있는 첫 Collect·churn의 sweep(1M 1.9–5ms)은 대부분 Retire 처리(이름 색인·고리·회수 목록)이고 white 수에 비례한다.
- 회수(churn)는 모든 단계에서 1M 객체당 140–290ns로 비슷하다(Phase 3의 28ms는 부하 아래 재측정의 흔들림이다). 객체마다 잠금 두 번과 소멸자·해제가 남는다(GC-S5).
- Phase 3의 직렬 경로는 Phase 2와 같다. 같은 시점에 번갈아 잰 Phase 2는 113.4·150.5·63.0ms, Phase 3은 116.7·145.0·66.2ms다(1M 간선 4·간선 8·잎 50%).

### 3.2 증분 수집 (cycle 1회)

기준의 기본 예산은 Step당 객체 64개·soft 1ms였고 Phase 1의 기본 예산은 시간 1ms(객체 상한 2^20)다.

| 시나리오 | 예산 | 단계 | Step 수 | p50 (us) | p95 | p99 | 최대 | cycle 합 (ms) |
|---|---|---|---:|---:|---:|---:|---:|---:|
| 100K · 4 | 기본 | 기준 | 3,098 | 3.2 | 91.8 | 258.3 | 1,659 | 115.0 |
| | | Phase 1 | 20 | 1,006 | 1,021 | 1,031 | 1,031 | 19.1 |
| | | Phase 2 | 8 | 1,005 | 1,008 | 1,008 | 1,008 | 7.0 |
| | 2ms | 기준 | 50 | 2,001 | 2,030 | 2,364 | 2,364 | 99.6 |
| | | Phase 1 | 10 | 2,011 | 2,024 | 2,024 | 2,024 | 19.0 |
| | | Phase 2 | 5 | 2,002 | 2,010 | 2,010 | 2,010 | 7.0 |
| 100K · 8 | 기본 | 기준 | 3,126 | 18.5 | 115.8 | 157.2 | 1,057 | 142.4 |
| | | Phase 1 | 27 | 1,018 | 1,032 | 1,033 | 1,033 | 26.8 |
| | | Phase 2 | 12 | 1,006 | 1,016 | 1,016 | 1,016 | 10.9 |
| | 2ms | 기준 | 70 | 2,001 | 2,003 | 2,007 | 2,007 | 139.1 |
| | | Phase 1 | 16 | 2,012 | 2,037 | 2,037 | 2,037 | 29.7 |
| | | Phase 2 | 8 | 2,005 | 2,042 | 2,042 | 2,042 | 13.0 |
| 1M · 4 | 기본 | 기준 | 30,941 | 9.3 | 67.5 | 95.7 | 267 | 790.6 |
| | | Phase 1 | 279 | 1,016 | 1,041 | 1,069 | 1,300 | 292.4 |
| | | Phase 2 | 124 | 1,009 | 1,021 | 1,059 | 1,062 | 124.3 |
| | 2ms | 기준 | 331 | 2,001 | 2,001 | 2,022 | 2,074 | 668.7 |
| | | Phase 1 | 128 | 2,014 | 2,036 | 2,045 | 2,046 | 263.1 |
| | | Phase 2 | **57** | 2,008 | 2,015 | 2,019 | 2,019 | 111.6 |
| 1M · 8 | 기본 | 기준 | 31,246 | 21.1 | 64.0 | 84.9 | 261 | 923.6 |
| | | Phase 1 | 323 | 1,021 | 1,048 | 1,057 | 1,094 | 338.0 |
| | | Phase 2 | 144 | 1,011 | 1,020 | 1,023 | 1,028 | 144.3 |
| | 2ms | 기준 | 598 | 2,001 | 2,008 | 2,059 | 2,329 | 1,203.4 |
| | | Phase 1 | 166 | 2,021 | 2,048 | 2,063 | 2,070 | 340.5 |
| | | Phase 2 | 73 | 2,010 | 2,017 | 2,023 | 2,023 | 144.5 |
| 1M · 4 · 잎 50% | 기본 | Phase 2 | 62 | 1,008 | 1,021 | 1,095 | 1,095 | 61.4 |
| | 2ms | Phase 2 | 33 | 2,010 | 2,062 | 2,090 | 2,090 | 63.6 |
| **Phase 3 병렬(worker 8)** | | | | | | | | |
| 100K · 4 | 기본 | Phase 3 ×8 | 4 | 528 | 1,058 | 1,058 | 1,058 | 2.6 |
| | 2ms | Phase 3 ×8 | 3 | 376 | 2,031 | 2,031 | 2,031 | 2.4 |
| 1M · 4 | 기본 | Phase 3 ×8 | 44 | 1,231 | 1,421 | 1,488 | 1,488 | 53.0 |
| | 2ms | Phase 3 ×8 | **21** | 2,244 | 2,511 | 2,669 | 2,669 | 45.4 |
| 1M · 8 | 기본 | Phase 3 ×8 | 62 | 1,456 | 1,721 | 1,887 | 1,887 | 88.4 |
| | 2ms | Phase 3 ×8 | **25** | 2,497 | 2,730 | 2,732 | 2,732 | 59.1 |
| 1M · 4 · 잎 50% | 기본 | Phase 3 ×8 | 21 | 1,067 | 1,092 | 1,097 | 1,097 | 20.9 |
| | 2ms | Phase 3 ×8 | 13 | 2,095 | 2,260 | 2,260 | 2,260 | 24.4 |

- 기준의 기본 예산은 Step이 짧지만(p99 0.1–0.26ms) 1M cycle 하나에 Step 3.1만 번이 걸렸다(60fps에서 약 8.6분 — 설계 3.2절 7번과 일치한다).
- Phase 1의 기본 예산(1ms)은 1M cycle을 279–323 Step에 끝낸다. 2ms 예산은 128–166 Step이다.
- soft deadline 초과는 2ms 예산에서 p99 2–3%(최대 2.07ms), 1ms 예산에서 최대 1.30ms였다. 시계를 객체 128개마다 보므로 생기는 초과와 Begin이 들어간 첫 Step이 원인이다.
- 시간 예산 cycle의 Begin은 첫 Step 전 `BeginCollection` 한 번에 들고 예산 밖이다. 1M에서 Phase 1까지 6.5–9.3ms, Phase 2에서 0.02–0.08ms다.
- Phase 2의 2ms 예산은 1M·간선 4 cycle을 57 Step(60fps 기준 0.95초)에 끝낸다. 간선 8은 73 Step이다.
- Phase 3의 직렬 증분은 Phase 2와 같다(같은 시점 재측정 1M·4: 기본 117 Step p99 1.02ms, 2ms 59 Step p99 2.04ms).
- Phase 3의 병렬 증분(worker 8)은 1M·4를 2ms 예산 21 Step, 1M·8을 25 Step에 끝낸다. 대신 Step이 예산을 더 넘는다(p99가 1ms 예산에서 1.49–1.89ms, 2ms 예산에서 2.67–2.73ms). 멈춤은 참여자마다 객체 64개마다 보는 예산 검사, task 제출·worker 깨우기, 남은 회색 객체 회수가 slice마다 0.2–0.7ms를 더한다. 실제 예산을 지키려면 시작·끝 비용만큼 deadline을 앞당겨야 한다(10절).

### 3.3 scheduler·병렬 표식 (표식 ms)

| 시나리오 | 기준 ×8 | Phase 1 ×8 | Phase 2 ×8 | Phase 3 ×2 | ×4 | ×8 | ×16 |
|---|---:|---:|---:|---:|---:|---:|---:|
| 100K · 4 | 490.8 | 394.6 | 425.0 | 4.47 | 3.23 | 2.26 | 1.56 |
| 100K · 8 | 399.3 | 382.0 | 450.1 | 5.83 | 3.94 | 2.82 | 1.99 |
| 1M · 4 | 4,838.8 | 3,811.5 | 4,350.7 | 83.6 | 54.3 | 35.7 | **23.9** |
| 1M · 8 | 4,833.6 | 4,062.4 | 4,427.8 | 102.3 | 82.5 | 39.1 | **23.7** |
| 1M · 4 · 잎 50% | 3,385.6 | 2,807.2 | 1,800.8 | 47.9 | 28.0 | 17.2 | **11.4** |

- Phase 0–2의 scheduler 경로는 객체마다 task를 제출해 직렬보다 15–39배 느렸다. Phase 3은 worker 수만큼 task를 한 번 제출하고 블록을 나눠 쓴다.
- Phase 3의 1M·간선 4는 직렬 116.7ms 대비 ×2 1.4배, ×4 2.2배, ×8 3.3배, ×16 4.9배다(참여자는 worker + 소유 스레드). 모델(설계 3.4절)은 단일 대비 16스레드에서 약 4.2배였다.
- ×4와 ×8 사이(간선 8: 82.5 → 39.1ms)처럼 단계가 고르지 않은 것은 다른 프로세스의 부하와 공유 풀 나눔의 흔들림이다. 객체마다 `Function`을 할당·제출하고 64개마다 기다리는 구조(설계 3.2절 6번)를 Phase 1에서 바꾸지 않았기 때문이다(잠금만 간선마다에서 배치마다로 줄었다). GC-S4 몫이다.

### 3.4 메모리 (1M · 간선 4, 첫 Collect 전)

| 범주 | MiB | 객체당 |
|---|---:|---:|
| Slot chunk(표 포함) | 183.4 | 192.3 B/slot |
| 객체 본체 | 45.8 | |
| 참조 배열 버퍼 | 81.4 | |
| 이름 기록·색인 | 54.0 | |
| GC 밀집 자료(Phase 2: GC 페이지·타입 표·루트 목록) | 19.7 | 20.6 B/slot |
| 기타(작업 큐 등) | 55.2 | |
| 합계 | 419.7(기준·Phase 1) → 439.4(Phase 2) | 440 → 461 B/object |

- Phase 1은 메모리 배치를 바꾸지 않았다.
- Phase 2는 Slot(192B)을 그대로 두고 GC 밀집 자료를 더했다. 슬롯당 hot 레코드 16B, 비트 묶음 0.375B(live·mark·leaf 3비트), 루트 목록 용량 4B(두 배씩 늘려 최대 8B)다. marker가 슬롯마다 읽는 자료는 Slot 192B(cache line 3개)에서 16.4B로 줄었다.
- Slot 정보 합계(192 + 20.6B)는 4절 목표(≤ 64B)의 3.3배다. cold Slot을 줄이는 방안은 6.4절이다.
- Phase 3은 슬롯당 자료를 더하지 않았다. 장벽 큐(슬롯당 4B 용량, 표식 큐와 같다)와 병렬 표식의 큐 블록(2KB, 회색 객체 512개마다 하나와 참여자마다 몇 개 — 재사용 목록에 남는다)이 더해진다.

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
| Phase 2 seed 11 | 직렬, 객체 3000 | 300,000 | 183,427 | 180,455 | 5,379 / 4,753 | 1,042 | PASS |
| Phase 2 seed 12 | 직렬, 객체 3000 | 300,000 | 182,872 | 181,922 | 5,361 / 4,745 | 981 | PASS |
| Phase 2 seed 13 | scheduler worker 4개 | 300,000 | 182,996 | 179,988 | 5,325 / 4,698 | 952 | PASS |
| Phase 2 seed 14 | 시간 예산 100us | 200,000 | 124,146 | 121,160 | 22,414 / 22,370 | 3,714 | PASS |
| Phase 3 seed 11 | 직렬, 객체 3000 | 300,000 | 183,427 | 180,455 | 5,379 / 4,753 | 1,042 | PASS |
| Phase 3 seed 12 | 직렬, 객체 3000 | 300,000 | 182,872 | 181,922 | 5,361 / 4,745 | 981 | PASS |
| Phase 3 seed 22 | 병렬 worker 2개 | 300,000 | 184,070 | 181,062 | 6,281 / 5,665 | 1,201 | PASS |
| Phase 3 seed 24 | 병렬 worker 4개 | 300,000 | 184,182 | 181,594 | 5,827 / 5,201 | 1,036 | PASS |
| Phase 3 seed 28 | 병렬 worker 8개 | 300,000 | 182,152 | 180,378 | 5,395 / 4,761 | 850 | PASS |
| Phase 3 seed 14 | 시간 예산 100us | 200,000 | 120,130 | 117,135 | 22,187 / 22,130 | 3,748 | PASS |

기준과 Phase 1은 각각 1M tick(직렬 600K, scheduler 200K, 시간 예산 200K), Phase 2는 1.1M tick(직렬 600K, scheduler 300K, 시간 예산 200K)을 통과했다. Phase 2의 시뮬레이터 잎 타입(SoakLeaf)은 `hasStrongReferences = false`라 잎 경로(M2)를 지난다. Phase 3은 1.7M tick(직렬 600K, 병렬 worker 2·4·8 각 300K, 시간 예산 200K)을 통과했다. 병렬 변형은 증분 Step과 Collect 모두 새 병렬 marker를 쓴다. 시뮬레이터의 노드·잎은 병렬 안전 타입이고 탐침은 수동 Trace라, 병렬 slice 안에서 소유 스레드 큐·cross-thread 반환이 함께 돈다. 직렬 seed 11·12의 결과가 Phase 2와 같은 것은 직렬 경로와 시뮬레이터가 결정적이기 때문이다. Phase 1은 cutoff slice가 sweep 없이 끝나 cycle이 짧아졌고 같은 tick에 cycle 수가 약 2배다. 생성 수와 회수 수의 차이는 끝에 남은 추적 객체(최대 3008개)이고 종료 대조에서 0이 됐다.

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

그래서 `mark`만 relaxed atomic(`Slot::Mark`·`SetMark`)으로 바꾸고 나머지 칸은 그대로 두었다. `GcConcurrencyTests.cpp`가 증분 slice와 Collect 각각에서 탐침 Trace 안 helper 스레드의 RootRef·BackendLease·EncodePin 반환과 RootRef 복제(Busy)를 명시 hand-off로 실행하고 결과(루트를 slice 안에서 놓은 객체는 이번 cycle에 남고 다음 수집에서 회수, lease를 slice 안에서 놓은 쓰레기는 이번 cycle에 회수, 통계)를 단언한다. Asan 실행은 8절을 본다.

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

## 6. Phase 2 구현 (GC-S3·M2)

### 6.1 바꾼 것

- **GC 밀집 자료.** 슬롯 4096개(chunk 64개)마다 GC 페이지 하나를 둔다. 페이지는 16B hot 레코드(object 포인터·Outer 슬롯 번호·GC 타입 번호) 4096개와 chunk마다의 비트 묶음(live·mark·leaf 각 64비트) 64개를 담는다. 페이지는 이동하지 않고 페이지 표만 두 배로 늘린다. 늘리는 일은 생성의 처리 표시 아래 잠금 밖에서 준비하고 잠금 안에서 게시한다(marker와 겹치지 않는다).
- **GC 타입 표.** hot 레코드의 16비트 번호가 가리키는 항목(trace 함수·병렬 안전·잎)이다. 생성이 타입 레코드의 `RuntimeTypeOps` 주소로 찾거나 단다.
- **marker.** 큐는 슬롯 번호(uint32)다. 꺼낸 객체는 hot 레코드로 trace하고(Slot·`TypeLease::Ops()`를 읽지 않는다) Outer는 hot 레코드의 슬롯 번호로 바로 표식한다. 간선은 domain·슬롯 범위·live 비트·mark 비트만 본다. 세대·상태 검사는 Debug만 한다(설계 GC-S3 "GC 중 세대 검사는 Debug 검증으로만"). 큐 16칸 앞 hot 레코드, 8칸 앞 객체 본체, 배치마다 대상의 비트 묶음을 prefetch한다.
- **루트 목록.** roots가 0 ↔ 1로 바뀌는 `AddRoot`·Commit의 root 증가·`detail::ReleaseRoot`에서 gate 아래 갱신한다. 목록 용량은 언제나 슬롯 수 이상이라 gate 아래 추가가 할당하지 않는다(성장은 chunk 생성 때 잠금 밖 할당·잠금 안 교체). Begin·Collect는 목록만 걷는다.
- **표식 초기화.** epoch를 없애고 Begin·Collect가 chunk마다 mark word 하나를 지운다(1M에서 1.56만 word).
- **비트 sweep.** chunk마다 `live & ~mark`를 잠금 없이 계산하고 white가 있는 chunk만 잠가 낮은 번호부터 Retire한다(슬롯 순서 유지). live 비트는 Commit이 켜고 Retire가 끈다(소유 스레드, 잠금 안).
- **M2 잎 타입.** `RuntimeTypeOps::hasStrongReferences`(기본 true)를 더했다. MetaCompiler는 방문 필드가 없는 클래스에 false를 낸다. `RegisterNativeClass`는 사슬의 어느 native 클래스도 Trace를 선언하지 않고 리플렉션 조상도 잎일 때 false로 둔다. `MakeRuntimeTypeOps`는 추론하지 않는다(호출자가 trace를 바꿀 수 있어 true가 안전하다). 잎 객체는 표식만 하고 큐에 넣지 않으며 hot 레코드의 Outer 사슬을 바로 따라간다. Debug는 Commit 검증에서 잎 타입이 간선을 방문하면 Fatal로 멈춘다.
- **기본 상한.** `RegistryOptions::maxSlots` 기본값을 2,097,152로 올렸다.
- Debug `Stats`가 live 비트와 루트 목록 위치를 슬롯과 대조한다.
- 7.1절 감사 갱신: marker가 잠금 없이 읽고 쓰는 자료가 hot 레코드·비트 묶음·GC 타입 표·작업 큐로 바뀌었다. 다른 스레드의 `ReleaseRoot`·`ReleaseBackend`·`ReleaseEncodePin`은 이 자료를 건드리지 않는다(`ReleaseRoot`는 루트 목록을 바꾸지만 marker는 목록을 읽지 않는다). live 비트를 바꾸는 Commit·Retire와 페이지·타입 표·루트 목록 용량을 바꾸는 생성은 소유 스레드이고 slice 중에는 Busy다.

### 6.2 결정 (설계 11절 열린 결정)

| 번호 | 항목 | 결정 | 근거 |
|---|---|---|---|
| 2 | 표식 단위 | **bit**(chunk마다 64비트 word, live·leaf와 같은 24B 묶음) | 1M에서 표식 비트 전체가 375KB(live·leaf 포함)로 L2에 가깝게 들어간다. Begin의 지우기가 chunk마다 word 하나다. GC-S4는 `fetch_or`로 옮긴다(지금은 relaxed load·store) |
| 7 | slice 중 루트 해제의 루트 목록 반영 시점 | **즉시 반영** | 목록은 Begin·Collect의 gate 구간만 읽는다. slice 중 `ReleaseRoot`는 gate 아래에서 목록을 바꾸지만 marker는 목록을 읽지 않는다. 이미 표식된 객체는 이번 cycle에 남는다(`GcConcurrencyTests`) |
| — | 패키지 정책 자식 고리 | **cold Slot 접근 유지** | 자식 고리는 패키지 정책 Registry만 읽는다(정책 없는 Registry는 비용 0). 패키지 정책 Registry는 잎 처리도 하지 않는다(자식을 따라야 한다). 패키지 Registry의 비용은 이번 벤치에서 재지 않았다 |
| 5 | Slot 정보 64B 목표 | **이번 단계에서 하지 않음, 방안만** | 6.4절 |

### 6.3 설계와 다르게 한 것

| 항목 | 설계 | 실제 | 이유 |
|---|---|---|---|
| hot 레코드의 상태 바이트 | 상태 바이트·live bitmap | live 비트만 둔다 | marker·sweep이 쓰는 상태는 "후보(Alive·DestroyRequested)인가"뿐이다. 나머지 상태 판정은 소유 스레드의 잠금 구간이 Slot에서 한다 |
| 밀집 배열 단위 | chunk처럼 페이지 단위 | 4096슬롯 페이지(약 66KB) | chunk(64칸)마다 두면 비트가 1KB씩 흩어져 대상 비트 미스가 다시 생긴다. 시험의 작은 Registry도 페이지 하나(66KB)를 받는다 |
| 세대 검사 | Debug만 | Debug만 + Release도 domain·슬롯 범위·live 비트 | 잘못된 Trace가 다른 Registry handle이나 빈 슬롯을 방문하면 지금처럼 StaleHandle로 cycle을 멈춘다(`ObjectIncrementalGcTraceFailureCannotPublishCutoffOrReclaimGarbage`). 회수 뒤 재사용된 슬롯의 옛 handle은 Release에서 새 객체를 표식할 수 있다(floating, 메모리 안전) |
| 잎 표시의 출처 | 생성기가 내는 상수 | 생성기 + `RegisterNativeClass` 추론, 수동 함수표는 명시 | native 클래스도 사슬로 판정할 수 있다 |
| 3단 prefetch | 16·8·4칸 | 16·8칸 + 대상 비트 | 참조 배열 버퍼는 M3 몫이다. 32·16칸으로 늘려 봤으나 1M 표식이 135 → 141ms로 차이가 없었다 |
| 시험 | | 잎 Item을 padding으로 써서 Marking을 유지하던 두 시험(`ConstructionContext_Assign*ShadesAliveTargets*`)을 trace되는 Holder로 바꿨다 | 잎은 큐에 들지 않아 첫 slice에 cutoff가 났다. 단언하는 계약(slice 사이 staged 대상 shade)은 같다 |

### 6.4 Slot 64B 목표의 방안 (결정 5, 미구현)

marker는 이제 Slot을 읽지 않으므로 Slot 크기는 GC 시간이 아니라 메모리와 생성·회수 경로의 문제다. 지금 Slot 192B의 구성은 대략 수명·상태 24B, 회수 목록 고리 16B, object·storage 16B, TypeLease 16B, stableType 16B, 보유 계수 16B, transaction 8B, Outer handle 16B, 이름·자식 고리 56B, 이름 기록 8B, 나머지다. 64B로 가려면 다음이 필요하다(추정).

- 이름·자식 고리(outermost·parent·firstChild·lastChild·nextSibling·prevSibling·name·pathBytes, 약 66B)를 이름 있는/Outer 있는 객체만 갖는 별도 identity 레코드로 뺀다.
- Outer handle(16B)은 hot 레코드의 Outer 슬롯 번호와 세대로 대신한다.
- stableType(16B)·transaction(8B)·회수 목록 고리(16B)는 생성·회수 중에만 쓰므로 cold 보조 표로 옮긴다.
- object(hot에 있음)와 storage를 합친다.

## 7. Phase 3 구현 (GC-S4)

### 7.1 바꾼 것

- **병렬 marker(`ParallelMark`).** 객체마다 task를 제출하던 경로를 없앴다. slice(또는 `Collect(access, scheduler)`)마다 worker 수(최대 64)만큼 task를 한 번 제출하고 소유 스레드도 참여자로 함께 돈다.
  - 참여자마다 512칸 블록 더미로 된 지역 큐(LIFO)를 둔다. 맨 위 블록이 차고 공유 풀이 참여자 수보다 적으면 그 블록을 공유 풀에 내놓는다. 객체 64개마다 idle 참여자가 있으면 가득 찬 블록 하나를 더 내놓는다.
  - 공유 풀은 잠금 하나(설계의 "블록 풀 하나를 잠금으로 보호하는 단순한 방식")와 조건 변수로 지킨다.
  - 표식은 mark 비트를 먼저 일반 읽기로 보고 `fetch_or`(relaxed)로 세운다. 이긴 참여자만 지역 큐에 넣는다. 잎은 표식만 하고 hot 레코드의 Outer 사슬을 같은 방식으로 따라간다.
  - `parallelTraceSafe`가 아닌 수동 Trace 객체는 worker가 소유 스레드 큐로 넘기고(idle 전에 반드시 넘긴다) 소유 스레드만 trace한다.
  - 끝: 모든 참여자가 idle이고 공유 풀과 소유 스레드 큐가 비었을 때다. worker가 소유 스레드 큐를 남긴 채 idle이 되어도(소유 스레드가 아직 깨어나지 않음) 끝으로 보지 않는다.
  - 예산: 참여자마다 객체 64개(객체 예산이 더 작으면 그 수)마다 공유 trace 수에 더하고 시간을 본다. 넘으면 멈춤을 걸고 모든 참여자가 남은 블록을 내놓으며, 소유 스레드가 task를 모두 회수한 뒤 남은 회색 객체를 표식 큐에 다시 담아 다음 slice로 넘긴다.
  - 오류(Trace 간선 검증 실패·블록 할당 실패·task 제출 실패)는 첫 오류를 남기고 멈춘다. 받아들여진 task를 모두 회수한 뒤 cycle을 버린다.
  - 큐 블록은 Registry의 재사용 목록에 남겨 다음 slice가 다시 쓴다.
- **장벽 큐.** 삽입 장벽·새 루트·Marking 중 출생의 shade는 장벽 큐(`gcShade_`)에만 넣고 slice 시작에 표식 큐로 옮긴다(UE `GReachableObjects` 대응). cutoff는 표식이 끝나고 장벽 큐가 빈 slice만 게시한다. 직렬 경로도 같은 장벽 큐를 쓴다.
- **World.** `WorldDesc::gcScheduler`(엔진은 `WorldCreateDesc::gcScheduler`, 빌림)를 받으면 프레임 끝 slice와 종료 수집(Step·Collect)에 넘긴다. 엔진은 아직 scheduler를 소유하지 않으므로 기본은 직렬이다.
- **Debug 불변식 검증.** Release 표식은 세대를 보지 않으므로(GC-S3), Debug는 직렬·병렬 모두에서 강한 간선의 세대·상태를 확인한다(`IsMarkableEdge`). 회수된 객체의 옛 handle이 강한 간선에 남으면 Debug는 cycle을 StaleHandle로 버리고 GcSoak가 실패로 보고한다(`ObjectGcParallel_DebugCatchesStaleHandleInStrongEdge`).
- **시험.** `GcParallelTests.cpp`: worker 1·2·8의 정지형·증분 병렬 표식이 seed 고정 무작위 그래프(6000개, 병렬 노드·수동 Trace 노드·잎·Outer 사슬 혼합)에서 독립 oracle·직렬 Collect와 같은 생존 집합·표식 수를 낸다(Debug에서 30회, Development에서 20회 반복 실행 통과). 병렬 slice 사이 장벽, Debug의 옛 handle 검출도 본다. `GcConcurrencyTests`에 병렬 slice·병렬 Collect 안 cross-thread 반환 변형을 더했다.

### 7.2 설계와 다르게 한 것

| 항목 | 설계 | 실제 | 이유 |
|---|---|---|---|
| 블록 나눔 | 블록 단위 훔치기, 잠금 풀로 시작 | 남는 블록을 내놓는(push) 공유 풀 하나. 다른 참여자의 지역 큐에서 직접 훔치지는 않는다 | 지역 큐를 잠금 없이 쓰려면 내놓기 방식이 단순하다. 굶는 참여자가 있으면 64개마다 가득 찬 블록을 더 내놓아 같은 효과를 낸다. CAS 큐는 잠금 경합이 측정에 보이지 않아(×16에서도 선형에 가깝게 줄었다) 두지 않았다 |
| 소유 스레드 | 수동 Trace 큐를 소유 스레드에서 처리 | 소유 스레드도 일반 참여자로 병렬 안전 객체를 trace한다 | worker 1개일 때도 참여자가 둘이다. 대신 "병렬 안전 객체를 반드시 worker가 trace한다"는 가정이 없어져, 그 가정을 단언하던 시험(`ObjectLifecycleParallelTraceRejectsForeignThreadBeforeReentry`)은 trace한 스레드에 맞춰 WrongThread(worker) 또는 Busy(소유 스레드)를 단언하도록 바꿨다 |
| 객체 예산 | 객체 수 | 참여자마다 64개 단위로 확인(예산이 작으면 그 수) | 참여자 수 × 간격만큼 넘을 수 있다 |
| 제출 실패 시험 | | `ObjectIncrementalGcDrainsAcceptedMarkerBeforeReturningLaterSubmissionFailure`를 "두 번째 task 제출 실패 → 받아들인 task 회수(`inFlight == 0`) → OutOfMemory → cycle 버림 → 다음 cycle 정상"으로 바꿨다 | 객체마다 제출하던 구조의 Trace 차단 순서에 기대던 시험이다 |
| worker 없는 scheduler | 제출 실패 | 소유 스레드 혼자 표식한다(`ObjectIncrementalGcSchedulerWithoutWorkersMarksOnOwnerThread`) | task를 제출하지 않으므로 실패할 것이 없다 |
| 증분 병렬 slice의 예산 | ≤ 2ms(p99) | p99 2.67–2.73ms(2ms 예산) | 시작·끝 비용을 deadline에 반영하지 않았다(10절) |

## 8. 검증

| 항목 | 결과 |
|---|---|
| Debug Foundation 전체 시험 | Phase 1 1734개, Phase 2 1737개, Phase 3 1747개 전부 통과 |
| Development Foundation 전체 시험 | Phase 1에서 1734개 중 GC 무관 2개 실패(`Process_RunTimeoutTerminatesChild`·`SharedPtr_LockRacesWithLastRelease` — 동시 빌드·벤치 부하에서 나는 시간·경쟁 의존 시험), GC·Object 시험(`--filter Object` 158개, `--filter Gc` 39개) 전부 통과 |
| GcSoak 시험(직렬·scheduler) | Debug·Development 통과 |
| 장기 soak | 4.2절(기준·Phase 1 각 1M tick, Phase 2 1.1M tick, Phase 3 1.7M tick 통과) |
| Development 벤치 | 3절 |
| clang-format 21.1.6 | 통과(`Rocks.ToolTests format`) |
| policy 검사 | 통과(`Rocks.ToolTests policy`, 정책 자체 시험 `self` 113개 통과) |
| Asan Foundation 전체 시험 | Phase 1에서 1734개 전부 통과(GcSoak·GcConcurrency 포함, AddressSanitizer 보고 없음). Phase 2 1737개, Phase 3 1747개 통과 |
| MetaCompiler·C# 도구 시험 | Phase 2(MetaCompiler가 `hasStrongReferences`를 낸다): Debug Foundation `-t:Test` 전체(네이티브 1737개, 도구 self 113개, native 112개, meta 195개) 통과 |
| Asan EngineTests | Phase 2 1803개, Phase 3 1813개 통과 |
| EngineTests(World·Engine) | Phase 1(Debug) 1800개 통과 — `GcBudget` 기본값 변경(64객체 → 1ms)으로 실패한 시험이 없어 World 쪽 수정은 하지 않았다. Phase 2(Debug) 1803개, Phase 3(Debug) 1813개 통과 |
| 병렬 표식 반복 실행 | Phase 3 `--filter Parallel`(병렬 GC 시험 17개): Debug 30회·Development 20회 반복 모두 통과 |

## 9. 실행 기록

- 기준(Phase 0, 커밋 `b5b42a4`): Debug `--filter Gc` 36개 통과, policy·format 통과, Development 벤치·장기 soak(4.2절).
- Phase 1(커밋 `63da487`): Debug·Asan Foundation 전체 1734개 통과, Development `--filter Object`·`--filter Gc` 통과, policy·format·`self` 통과, Development 벤치·장기 soak(4.2절). Development 전체 실행의 GC 무관 실패 2개는 8절에 적었다.
- Phase 1 추가: Debug EngineTests 1800개 통과. 부하 아래 실패했던 Development 시험 2개(`Process_RunTimeoutTerminatesChild`·`SharedPtr_LockRacesWithLastRelease`)는 단독 실행에서 통과했다(`Process_*` 21개·`SharedPtr*` 12개 전부 통과). 뒤의 것은 경쟁이 실제로 났는지(`observedAlive > 0 && observedDead > 0`)를 단언하므로 부하에 따라 흔들리는 기존 시험이다.
- Phase 2(커밋 `d90a769`): Debug Foundation 1734개·Debug EngineTests 1803개 통과, Development `--filter Object`·`--filter Gc` 통과, policy·format 통과, Development 벤치·장기 soak(4.2절). Asan Foundation 1737개·Asan EngineTests 1803개 통과, Debug Foundation `-t:Test`(meta 195개 포함) 통과.
- Phase 3(커밋 `b8c23b6`): Debug Foundation 1747개·Debug EngineTests 1813개, Asan Foundation 1747개·Asan EngineTests 1813개 통과, Development `--filter Gc`(52개)·`--filter Object`(171개) 통과, `--filter Parallel` 반복(Debug 30회·Development 20회) 통과, policy·format 통과, Development 벤치(1절의 재측정)·장기 soak(4.2절). MetaCompiler는 바꾸지 않아 meta 시험은 다시 돌리지 않았다.
- 실행하지 않은 것: Release 구성 벤치, macOS·Android, World 단위 soak, 패키지 정책 Registry 벤치, 부하 없는 기계에서의 Phase 3 재측정.

## 10. 4절 성능 gate와의 차이 (Phase 3 시점)

| gate | 목표 | Phase 2 (1M) | Phase 3 (1M) | 상태 |
|---|---|---|---|---|
| STW 병렬 전체 표식 | ≤ 15ms | scheduler 4,351ms | ×8 35.7ms·×16 23.9ms(간선 4), ×16 23.7ms(간선 8), ×16 11.4ms(잎 50%) | 잎 50%만 충족. 간선 4·8은 16 worker에서 1.6배 남는다 |
| 단일 스레드 전체 표식 | ≤ 60ms | 112ms(간선 4)·142ms(간선 8)·57ms(잎 50%) | 같음(직렬 경로 불변) | 미달(잎 50%만 충족) |
| Begin | ≤ 1ms | 0.035–0.07ms | 0.04–0.12ms | **충족** |
| sweep 후보 판정 | ≤ 1ms | 0.012ms | 0.012–0.03ms | **충족** |
| 증분 Step p99 | ≤ 2ms | 직렬 2ms 예산 2.02–2.09ms | 직렬은 같음. 병렬 ×8은 2ms 예산 2.26–2.73ms, 1ms 예산 1.10–1.89ms | 직렬은 거의 충족, 병렬은 미달(slice 시작·끝 비용) |
| 증분 cycle | 2ms 예산에서 60프레임 이내 | 57·73·33 Step | 병렬 ×8: 21·25·13 Step | **충족**(병렬). 직렬은 간선 8만 미달 |
| Slot 정보 | ≤ 64B | 192 + 20.6B | 같음 | 미달(6.4절) |
| 기본 `maxSlots` | ≥ 2,097,152 | 2,097,152 | 같음 | **충족** |

GC-S4 완료 기준 "STW 병렬 전체 표식이 4절 목표를 만족한다"는 1M·간선 4·8에서 못 미쳤다(16 worker 24ms). 남은 몫은 단일 marker의 객체당 비용이다(11절 M3). 병렬 증분 slice는 cycle을 빨리 끝내지만 p99 gate(2ms)를 넘는다.

## 11. 남은 단계가 더할 것 (추정)

모델(설계 3.4절)과 이번 측정을 근거로 한 해석이며 측정 전 수치다.

- **M3(참조 위치 상수).** 생성 타입의 컨테이너 버퍼 offset을 상수로 내어 marker가 참조 배열 버퍼를 큐 몇 칸 앞에서 미리 읽게 한다. 모델에서 이 단계가 있는 GC-S3 변형은 45–55ms였고 실제 Phase 2·3 직렬은 112–117ms다. 차이를 대부분 이 미스로 보면 단일 표식이 약 2배 줄어 55–65ms(단일 gate 60ms 근처)가 되고, 병렬은 같은 비율로 ×16 약 12ms·×8 약 18ms(병렬 gate 15ms 근처)로 예상한다. 효과는 생성(리플렉션) 타입에만 있고 손 Trace 타입은 그대로다.
- **병렬 증분 slice의 예산 지키기.** task 제출·worker 깨우기·멈춤 전파·남은 회색 객체 회수가 slice마다 0.2–0.7ms 든다. 이 시작·끝 비용을 재어 deadline을 그만큼 앞당기거나, 작은 slice는 직렬로 돌리는 선택이 필요하다(GC-S5 일정 정책과 함께).
- **GC-S5.** STW 병렬 전체 수집을 정식 경로로 두는 일정 정책(레벨 전환·메모리 압박·주기)과 파괴·회수의 시간 예산이다. 지금 회수는 객체마다 잠금 두 번이고 1M churn 98K 회수에 15–28ms다. 엔진이 scheduler를 소유해 World에 넘기는 일도 여기다.
- **GC-S6.** O(1) 소유권 판정·스레드 캐시·크기별 slab 할당자다. `SystemAllocator`·`BinnedAllocator`로는 1M 규모의 생성·회수를 측정할 수 없었다(3.5절). 병렬 marker의 큐 블록도 Registry allocator의 다중 스레드 할당에 기댄다.
- **GC-S7.** 영구 영역·클러스터로 표식 대상 수를 줄이고 schema Trace(M4)로 큰 배열을 나눈다. 시작 때부터 끝까지 사는 객체가 많은 게임에서는 표식 시간이 대상 수에 비례해 준다.
- **M1.** 배치 visitor가 이미 간선당 간접 호출을 없앴으므로 추가 이득은 작을 것으로 본다(객체당 간접 호출 1회만 남는다).
- **Slot 64B.** 6.4절의 cold 분리다. 표식 시간에는 영향이 작고 메모리(1M에서 약 130MB)를 줄인다.
