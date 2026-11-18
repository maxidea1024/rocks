# GC 대규모 처리 실험 결과 (GC-S0–GC-S4·GC-S7·M2·M3)

기준일: 2026-11-16(Phase 4: 2026-11-17, 경계 큰 클러스터 개선: 2026-11-18). 브랜치 `experiment/gc-scale`(기준 `795761a`). 설계는 [GC 대규모 처리 보강](../Design/GarbageCollectionScale.ko.md)이다.

이 문서는 설계 6절의 단계 중 **GC-S0(측정 기준과 장기 smoke, Phase 0)**, **GC-S1(잠금 없는 직렬 marker·배치 visitor·prefetch)과 GC-S2(시간 기준 예산)(Phase 1)**, **GC-S3(GC 밀집 자료·루트 목록·비트 sweep·상한)과 M2(잎 타입)(Phase 2)**, **GC-S4(병렬 표식·장벽 큐·World scheduler)(Phase 3)** 의 실행 결과다. Phase 4(13절)는 **병렬 slice 보정(P4a)**, **GC-S7 클러스터·영구 영역(P4b–P4e)**, **M3 참조 위치 표(P4f)** 를 더한다. GC-S5·S6과 M1·M4–M7은 범위 밖이다.

## 0. 요약: 처음 → 현재 (2026-11-18, 1M · 간선 4)

### 0.1 설계 3.2절 원인별 처리

| # | 처음 원인 | 현재 | 단계 |
|---|---|---|---|
| 1 | 간선마다 전역 잠금 | 표식 중 잠금 없음 | Phase 1 |
| 2 | 192B Slot에 GC·비GC 필드 혼재 | marker는 GC 밀집 자료(슬롯당 약 21B)만 읽음. Slot 자체는 200B로 남음 | Phase 2(Slot 64B 미달) |
| 3 | 간선마다 간접 호출 | 배치 visitor, 생성 타입은 참조 위치 표(M3) | Phase 1·Phase 4 |
| 4 | prefetch 없음 | prefetch, 참조 배열 버퍼 미리 읽기 | Phase 1·Phase 4 |
| 5 | 루트 수집·sweep의 전체 Slot 순회 | 루트 목록·비트 sweep | Phase 2 |
| 6 | 병렬 경로가 객체마다 task, World는 직렬 | 블록 단위 병렬 표식, World scheduler | Phase 3 |
| 7 | 객체 수 기준 예산(Step당 64개) | 시간 기준 예산 | Phase 1 |
| 8 | 회수의 객체당 잠금 두 번, 할당자 선형 검색 | 바뀌지 않음 | GC-S5·S6 남음 |
| — | (대상 수 자체) | 패키지·World Level 클러스터, 영구 영역 | Phase 4(GC-S7) |

### 0.2 4절 gate: 처음 → 현재

| 항목 | 목표 | 처음(Phase 0) | 현재 일반 그래프 | 현재 클러스터 그래프 | 상태 |
|---|---|---:|---:|---:|---|
| 단일 스레드 표식 | ≤ 60ms | 779ms | 102–104ms(부하 아래, 13.4절) | 7–20ms(14.4절) | 일반 미달, 클러스터 충족 |
| STW 병렬 표식 | ≤ 15ms | scheduler ×8 4,839ms | ×8 35.7ms, ×16 23.9ms(3.3절) | ×8 3.2–6.5ms | 일반 미달, 클러스터 충족 |
| Begin | ≤ 1ms | 8.46ms | 0.04–0.40ms | 같음 | 충족 |
| sweep 후보 판정 | ≤ 1ms | 7.86ms | 0.01–0.19ms | 같음 | 충족 |
| 증분 cycle | ≤ 60프레임 | 기본 예산 30,941 Step(60fps 약 8.6분) | 직렬 49–51, 병렬 21 Step | 3–5 Step | 충족 |
| 증분 Step p99 | ≤ 2ms | 2ms 예산 2.02ms(cycle 669ms) | p95 2.1ms, p99는 부하 아래 판정 불가 | — | 재측정 필요 |
| Slot 정보 | ≤ 64B | 192B | 200 + 20.8B | 장부 + 24B | 미달 |
| churn 회수(약 98K) | — | 17.8ms | 15–28ms | — | 변화 없음(GC-S5) |
| 1M 생성·회수(System·Binned 할당자) | — | 측정 불가 | 측정 불가 | — | GC-S6 남음 |

처음에는 1M에서 표식 0.8초, 증분 cycle 8.6분, 병렬이 직렬보다 6배 느려 사실상 쓸 수 없었다. 지금은 cycle이 1초 안에 끝나고 Begin·sweep 판정이 1ms 아래이며, 클러스터가 맞는 그래프는 표식 gate를 모두 충족한다. Phase 4 측정은 부하 100% 아래라 병렬·p99 수치는 판정용이 아니다.

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

## 12. Phase 4 계획 (2026-11-17)

설계 [13절](../Design/GarbageCollectionScale.ko.md)의 클러스터·영구 영역과, 10절에서 남은 gate를 함께 다룬다. 순서는 아래와 같고 단계마다 따로 커밋한다.

| 단계 | 내용 | 완료 기준 |
|---|---|---|
| P4a | 병렬 증분 slice의 남은 회색 블록을 펴지 않고 넘김(`gcGrayBlocks_`), slice 시간 내역 계측 | 병렬 Step p99 ≤ 2ms(1M·간선 4, ×8). 못 미치면 원인과 남은 몫을 기록 |
| P4b | 클러스터 장부·표식·sweep·cutoff·장벽, oracle의 소속 간선, Debug 장부 검증 | 기존 GC 시험 전부와 oracle 대조 통과 |
| P4c | 패키지 정책 Registry의 자동 패키지 클러스터 | 패키지 생성·부분 Discard·전체 Discard 시험, `retiredWithoutDiscard` 경로 |
| P4d | 영구 영역(창 열기·닫기, 영구 → 일반 간선 거부, 파괴 거부), Engine Registry 초기화 뒤 닫기 | 거부 경로 시험, Engine 시험 통과 |
| P4e | World의 Level 클러스터(Level 생성 때 루트 지정, Spawn·AddComponent 가입, 파괴 때 이탈). World 레코드에 Actor별 Component handle 보관(API 밖 Actor 파괴에서도 Component 이탈), RequestDestroy 결과 확인 | EngineTests World 시험 통과, 소속 불변식(`inCount ≥ 1`) 위반 0 |
| P4f | M3 참조 위치 표 | 단일·병렬 표식 시간, 표와 Trace의 Debug 대조 |
| P4g | 측정·soak·음성 대조, 이 문서의 결과 절 | 아래 측정 항목 |

**벤치 시나리오 추가.**

- 패키지형: 패키지 정책 Registry, 1M 객체 = 패키지 10K × 100. 패키지 안 간선 평균 4. 패키지 사이 간선 비율 1%·10%·30%. 클러스터를 끈 같은 그래프와 비교한다.
- World형: Level 10개 × Actor 30K × (Actor 1 + Component 2), 같은 Level 안 참조, Level 밖 일반 객체 200K 참조 5%, 일반 객체 churn.
- 영구 영역: 영구 객체 100K + 일반 1M에서 영구 영역 있음·없음 비교.
- 간선 쓰기 비용: 클러스터 장부 갱신이 있는 `SetEdge`·배열 편집 처리량을 장부 없는 Registry와 비교한다.
- 기존 1M 시나리오는 P3와 P4를 같은 실행 안에서 번갈아 잰다.

**soak 추가.** 패키지 생성·부분 Discard·전체 Discard, 패키지 사이 간선 편집, Level·Actor·Component 생성·파괴, 같은 Level 안 Actor 사이 참조를 증분 cycle 도중에 섞는다. Begin마다 Debug 장부 검증을 켜고, oracle 대조를 주기적으로 한다.

**음성 대조 추가.** 이탈 때 `inCount` 이동 생략, Marking 중 이탈 shade 생략, `outCount` 갱신 하나 생략, M3 표에서 참조 하나 생략. 각각 시험이나 soak가 잡아야 한다.

**검증.** 커밋마다 Debug Foundation·EngineTests의 바뀐 범위, format, policy. Asan은 돌리지 않는다(2026-11-17 사용자 결정).

## 13. Phase 4 구현 (P4a–P4f)

### 13.1 바꾼 것

| 단계 | 커밋 | 내용 |
|---|---|---|
| P4a | `a7711a4`·`534431f` | 병렬 증분 slice 시간 내역(`GcSliceTimes`)과 GcBench 출력. 예산이 다해 남은 회색 블록을 펴지 않고 다음 slice로 넘김(`gcGrayBlocks_`), 앞 slice의 꼬리 시간(worker 회수·회색 넘기기)만큼 표식을 일찍 멈춤(예산의 1/4까지) |
| P4b | `edd0bec` | 클러스터 장부(슬롯마다 28 B 칸 — GC 페이지와 함께 할당, `clustered` 비트), 레코드 표, `ConstructionContext::SetClusterRoot`·`JoinCluster`, Commit의 가입과 staged 간선·outer 계수, 모든 live 간선 지점(비우기 경로 포함)의 장부 갱신, 클러스터 표식(epoch 비교 교환)·처리(경계 소속 항목·detached 표식·처리 epoch, 직렬·병렬), sweep 후보 `live & ~mark & ~clustered`, cutoff·Collect 직전 표식되지 않은 클러스터 해체, RequestDestroy·Discard의 이탈(잠금 밖 Trace)·루트 해체, 처리 전 해체의 경계 항목, Marking 중 가입의 outer shade, Debug 장부 재계산(`verifyClusterLedger`)·소속 불변식(`verifyClusterOwnership`) |
| P4c | `25e6f41` | 패키지 정책 Registry의 자동 패키지 클러스터(`packageClusters`, 기본 켬). struct 원소 제거·비우기는 원소 trace 한 번으로 관찰자 집계와 장부를 같이 셈 |
| P4d | `224a4dc` | 영구 영역(`permanentRegion`·`ClosePermanentRegion`), 창 안 완전한 필드 배치 강제, 영구 → 일반 간선(staged·live, outer 포함) Unsupported, 영구 객체 RequestDestroy·Discard Unsupported, 패키지 정책과 함께 켜면 Fatal, Shutdown 해체. Engine Registry가 서브시스템 초기화 동안 창을 열고 뒤에 닫음(실패는 별도 초기화 실패 지점) |
| P4e | `3c72bab` | World Level 클러스터(`WorldDesc::levelClusters`), SpawnActor·AddComponent의 가입, Actor 레코드의 Component handle 목록(우회 파괴 Actor의 Component도 정리), GC cycle 시작 전 우회 파괴 Actor 정리, RequestDestroy 결과 확인 |
| — | `435950f`·`b68dd3d` | 클러스터 장기 smoke(`RunGcClusterSoak`, World형·패키지형), GcBench 클러스터 시나리오(`--clusters`)·`--cluster-soak` |
| — | `9cb44e9`·`7981d11` | smoke·시험에 단일 필드 쓰기·배열 원소 교체·map 편집을 더함(음성 대조 3·10이 드러낸 구멍). smoke 도구 결함(모델 상한·O(객체²) oracle) 고침과 진행 표시(13.11절) |
| merge | `0551d92` | `origin/feature/engine-runtime`(영어 결과물 policy, 로깅 API). GcBench를 Program 타깃으로 표시(새 Release 배제 policy) |
| P4f | `0bdb66e` | 참조 위치 표(M3): `RuntimeTypeOps::referenceLayout`(MetaCompiler 생성), 타입별 표(`referenceTables`, 기본 켬), 직렬·병렬 marker의 표 표식과 4칸 앞 배열 버퍼 미리 읽기, Debug 표·Trace 대조. GC 사이클 끝 통계를 `Log::Debug`(Object.GC)로 남김 |

### 13.2 측정 환경

- Phase 3과 같은 기계·Development(-O2) 구성이다. 측정 내내 같은 기계의 에뮬레이터(qemu)가 CPU를 계속 써서 부하가 100%로 보고됐다. 그래서 비교는 같은 실행 안에서 번갈아(P3 실행 파일 ↔ P4f, 표 켬 ↔ 끔, 클러스터 켬 ↔ 끔) 쟀다. 병렬 수치와 증분 p99는 선점에 크게 흔들리고(같은 설정의 두 round가 1.5–3배 다르다), 직렬 정지형 표식은 round 사이 차이가 2–30%다.
- 원시 출력: scratchpad `p4/p4a-cmp.txt`(P4a), `p4/p4f-m3.txt`(M3·P3 비교), `p4/p4g-clusters.txt`(클러스터·영구 영역·간선 쓰기), `p4/p4g-negctl.txt`(음성 대조), `p4/p4g-soak.txt`(장기 soak).

### 13.3 P4a: 병렬 증분 slice (1M · 간선 4, worker 8개)

| 예산 | 지표 | P3 | P4a |
|---|---|---|---|
| 2ms | p50 | 2342–2375us | 2007–2022us |
| 2ms | p95 | 9.9–11.8ms | 2.12–2.18ms |
| 2ms | p99 | — | 6.1–13.7ms |
| 기본 1ms | p99 | 9.4ms | 1.24–2.64ms |

slice마다 회색 객체 약 26만 개를 펴고 다시 블록으로 묶던 비용(준비 약 195us·넘기기 약 226us)이 준비 약 40us·넘기기 25–60us로 줄었다. 2ms 예산의 p99는 표식 구간의 선점이 차지한다(slice 내역의 mark p99가 Step p99와 같다). 완료 기준(병렬 Step p99 ≤ 2ms)은 p95까지 맞고 p99는 이 기계의 부하에서 판정할 수 없다.

### 13.4 P4f: 참조 위치 표(M3) (1M · 간선 4, 정지형 중앙값 ms, 두 round)

| 항목 | P3 | P4f 표 켬 | P4f 표 끔 |
|---|---|---|---|
| 직렬 Collect 표식 | 244.4 · 170.1 | **102.3 · 104.1** | 168.1 · 226.0 |
| 병렬 ×8 표식 | 63.0 · 74.8 | 96.9 · 135.1 | 123.4 · 93.2 |
| 병렬 ×16 표식 | 41.4 · 77.7 | 67.5 · 179.6 | 75.0 · 123.1 |
| 증분 기본(직렬) cycle·Step 수 | 211.5ms·172 · 163.5ms·162 | **84.9ms·86 · 118.9ms·103** | 182.7ms·180 · 310.2ms·193 |
| 증분 2ms(직렬) Step 수 | 89 · 82 | **49 · 51** | 94 · 100 |
| 증분 기본 ×8 Step 수 | 78 · 61 | 42 · 41 | 46 · 47 |
| churn 98K Collect 전체 | 234.2 · 212.8 | **141.9 · 134.2** | 197.1 · 223.4 |

- 직렬 표식은 표로 약 **1.6–2.2배** 빨라졌다(11절의 예상 55–65ms에는 못 미친다 — 이 부하에서 102ms). 표가 없을 때보다 trace 함수 간접 호출·`ObjectRefArray::Trace` 순회가 빠지고, 4칸 앞 객체의 배열 버퍼를 미리 읽는다.
- 병렬 정지형은 이 측정에서 개선을 보이지 않는다. round 사이 차이(×16 67.5 ↔ 179.6ms)가 설정 차이보다 커서 판정할 수 없다. 병렬 marker의 지역 큐가 LIFO라 "4칸 앞"이 실제로 곧 꺼낼 객체가 아니어서 미리 읽기 효과가 작을 수 있다(부하 없는 기계에서 다시 잴 항목).
- 증분 직렬 cycle은 Step 수가 절반 가까이 줄었다(2ms 예산 89 → 49–51 Step, 60프레임 gate 충족).

### 13.5 클러스터 시나리오 (`GcBench --clusters`, 클러스터 켬 ↔ 끔 같은 실행)

정지형 표식 중앙값(ms), 증분 기본 예산 cycle(직렬 Step 수, ×8 Step 수), 간선 쓰기(`AddReference`·`RemoveReferenceAt` 각 200K번, Borrow 비용 포함, ns/op).

| 시나리오 | 클러스터 | 직렬 표식 | 병렬 ×8 표식 | 증분 직렬 | 증분 ×8 | 간선 추가 | 간선 제거 |
|---|---|---:|---:|---|---|---:|---:|
| 패키지 1M, 패키지 사이 1% | 켬 | **18.7** | **3.2** | 34.6ms · 20 | 5 | 1,041 | 986 |
| | 끔 | 90.2 | 67.2 | 175.4ms · 136 | 43 | 1,127 | 566 |
| 패키지 1M, 사이 10% | 켬 | 99.8 | **34.3** | 142.3ms · 130 | 21 | 1,581 | 819 |
| | 끔 | 110.6 | 68.6 | 106.6ms · 100 | 65 | 998 | 710 |
| 패키지 1M, 사이 30% | 켬 | 230.3 | **34.0** | 184.9ms · 176 | 33 | 1,402 | 912 |
| | 끔 | 123.9 | 53.3 | 129.3ms · 123 | 63 | 870 | 594 |
| World 10 Level × 30K Actor × 3 + 밖 200K | 켬 | **20.4** | **6.5** | 20.1ms · 21 | 8 | 774 | 609 |
| | 끔 | 51.7 | 21.3 | 64.9ms · 64 | 24 | 605 | 447 |
| 영구 100K + 일반 1M | 켬 | **98.2** | 41.4 | 107.0ms · 105 | 45 | — | — |
| | 끔 | 113.1 | 46.3 | 125.7ms · 126 | 55 | — | — |

- World형 일반 객체 churn(20K, 병렬 ×8 Collect 전체): 켬 12.6ms(표식 6.4) ↔ 끔 28.2ms(표식 21.0).
- 메모리: 클러스터를 켠 Registry의 GC 밀집 자료가 슬롯당 20.8 → 48.9 B(패키지형), 24.2 → 52.2 B(World형)다(장부 칸 28 B와 clustered 비트).
- **효과는 경계 비율에 달려 있다.** 패키지 사이 간선 1%(소속 약 4%가 경계)에서 직렬 4.8배·병렬 21배 빨라지고, World형(같은 Level 안 참조만)도 직렬 2.5배·병렬 3.3배 빨라진다. 사이 30%(경계 소속 약 76%)에서는 직렬이 **1.9배 느려진다**. 경계 소속을 거의 다 trace하면서 간선마다 대상이 소속인지 장부 칸(28 B, 대상 슬롯마다 다른 캐시 줄)과 레코드를 한 번 더 읽기 때문이다. 병렬은 클러스터 항목이 경계 소속 수백 개를 한 번에 지역 큐에 넣어 나눔이 쉬워 여전히 1.6배 빠르다.
- 간선 쓰기는 장부 갱신으로 추가가 0–60%, 제거가 30–75% 느려졌다(Borrow 조회가 포함된 수치 — 장부 갱신 자체는 잠금 안 칸 몇 개의 갱신이다). 패키지 1% 끔 ↔ 켬의 추가가 거꾸로인 것은 측정 흔들림이다.
- 영구 영역은 영구 객체 수(전체의 9%)만큼 표식이 준다(직렬 13%, 병렬 11%).

### 13.6 장기 soak

기존 GcSoak(Development, P4 실행 파일 — 표·클러스터 기능이 들어간 Registry의 일반 경로):

| 실행 | 설정 | tick | 생성 | 회수 | cycle(시작/완료) | 결과 |
|---|---|---:|---:|---:|---:|---|
| seed 11 | 직렬, 객체 3000 | 300,000 | 178,662 | 174,654 | 4,319 / 3,662 | PASS(20.3s) |
| seed 12 | 직렬, 객체 3000 | 300,000 | 181,539 | 177,639 | 4,349 / 3,688 | PASS(16.1s) |
| seed 24 | 병렬 worker 4개 | 300,000 | 184,662 | 181,391 | 5,735 / 5,140 | PASS(22.6s) |
| seed 14 | 시간 예산 100us | 200,000 | 125,297 | 122,131 | 20,258 / 20,169 | PASS(20.7s) |

클러스터 smoke(`GcBench --cluster-soak`, 13.8절 시나리오 — 소속 객체의 단일 필드 쓰기·배열 원소 교체 포함, 각 실행은 진행 표시와 상한 시간을 두었다). Debug 실행은 Begin·Collect마다 장부 재계산과(World형) 소속 불변식 검증을 한다.

| 실행 | 구성 | 형 | tick | 생성 | 회수 | cycle | 정지형 대조 | 결과 |
|---|---|---|---:|---:|---:|---:|---:|---|
| seed 31 | Development 직렬 | World | 5,000 | 1,500 | 1,407 | 4,286 | 715 | PASS(0.4s) |
| seed 35 | Development 직렬 | World | 5,000 | 1,500 | 1,404 | 4,286 | 715 | PASS(0.3s) |
| seed 33 | Development worker 4 | World | 3,000 | 1,500 | 1,369 | 2,572 | 429 | PASS(0.5s) |
| seed 32 | Development 직렬 | 패키지 | 1,000 | 1,497 | 1,479 | 858 | 143 | PASS(0.1s) |
| seed 36 | Development 직렬 | 패키지 | 1,000 | 1,497 | 1,472 | 858 | 143 | PASS(0.1s) |
| seed 34 | Development worker 4 | 패키지 | 600 | 1,497 | 1,443 | 515 | 86 | PASS(0.1s) |
| seed 41 | **Debug**(장부·소속 검증) 직렬 | World | 1,500 | 1,200 | 1,106 | 1,286 | 215 | PASS(0.4s) |
| seed 42 | **Debug**(장부 검증) 직렬 | 패키지 | 300 | 1,199 | 1,056 | 258 | 43 | PASS(0.2s) |

한계: 클러스터 smoke의 모델은 항목을 재사용하지 않아 모델 상한(1,200–1,500)에 닿은 뒤(World형 약 1,000 tick, 패키지형 약 200 tick)는 남은 객체의 편집·파괴만 한다. 그래서 tick 수보다 cycle 수(위 표)가 실제 덮은 양이다. 첫 실행 묶음에서 World형 5,000 tick이 끝나지 않은 일은 도구 결함이었다(13.11절).

### 13.7 음성 대조

코드 한 곳을 임시로 바꿔 Debug Foundation을 빌드하고 클러스터 시험(`ObjectGcCluster`)·smoke 시험(`GcSoak`, 클러스터 smoke 포함)·병렬 oracle(`ObjectGcParallel`)·증분 시험(`ObjectIncrementalGc`)을 돌린 뒤 되돌렸다(커밋하지 않음 — 원시 출력 `p4/p4g-negctl-1-2.txt`·`p4/p4g-negctl-3.txt`).

| 대조 | 바꾼 것 | 잡은 시험 | 증상 |
|---|---|---|---|
| 1 | 이탈 때 `inCount` 이동 생략 | 클러스터 시험·smoke | Debug 장부 재계산 "Cluster ledger diverged from a recount" |
| 2 | Marking 중 이탈 shade 생략 | 클러스터 smoke | "Reachable object was reclaimed"(tick 45). 결정적 시험은 잡지 못했다 |
| 3 | `outCount` 갱신 하나 생략(`ReplaceArrayEdge`) | 클러스터 smoke | 장부 재계산. 처음 실행에서는 smoke·시험 모두 배열 원소 교체를 하지 않아 **잡지 못했고**, smoke에 단일 필드 쓰기·원소 교체를 더한 뒤(`9cb44e9`) 잡았다 |
| 9 | `outCount` 갱신 하나 생략(`RemoveArrayEdge`) | 클러스터 시험·smoke | 장부 재계산 |
| 4 | 처리 전 해체에서 경계 항목 넣기 생략 | 13.8절 결정적 시험 | 경계 소속의 밖 대상이 회수됨(`Exists(outside)` 실패). smoke는 이 tick 수에서 잡지 못했다 |
| 5 | 배열 비우기 경로의 장부 갱신 생략 | 클러스터 시험·smoke | 장부 재계산 |
| 10 | map 비우기 경로의 장부 갱신 생략 | map 편집 시험(`9cb44e9`에서 추가) | 장부 재계산 |
| 6 | M3 표에서 마지막 칸 생략 | 병렬 oracle | 생존 집합·표식 수가 oracle과 다름(Debug 표·Trace 대조 Fatal) |
| 7 | 삽입 장벽 끔 | 클러스터 smoke·병렬·증분 시험 | "Reachable object was reclaimed"(tick 3) 외. 클러스터 결정적 시험(13.8절 장벽 경우)은 **잡지 못했다** — keeper가 연결 시점에 아직 trace 전이라 장벽 없이도 산다(시험이 의도보다 약하다) |
| 8 | 지연 목록의 회수 가능 전이 끔 | GcSoak·증분 시험 | 회수 수가 모델과 다름 |

검사가 비어 있던 경로 두 곳(배열 원소 교체, map 비우기)은 대조로 드러나 시험·smoke를 보강했다. 대조 2·4·7은 한 가지 시험만 잡는다.

### 13.8 설계(13절)와 다르게 구현한 점

| 항목 | 설계 | 구현 | 이유 |
|---|---|---|---|
| 소속 trace 항목 | 일반 표식 항목과 구분되는 큐 항목, 같은 클러스터 대상은 `clusterState` 비교로 건너뜀 | 경계 소속을 mark 비트를 켜고 **일반 항목**으로 넣는다. 같은 클러스터 대상은 일반 표식 경로에서 clustered 비트 → 클러스터 epoch 비교 한 번으로 끝난다 | 꺼낼 때 소속이 아니어도 그대로 일반 trace라 별도 표지가 필요 없다. mark 비트로 넣어 슬롯마다 큐 항목이 한 번이 되어 작업 큐 상한이 `슬롯 수 + 클러스터 레코드 수`로 닫힌다 |
| 클러스터 id | 루트 slot에서 얻음 | 레코드 표 번호(빈 레코드 재사용). 루트 칸의 `clusterState`가 번호를 가리키고 레코드가 루트 slot을 가진다 | 레코드 표를 slot 수만큼 두지 않으려고. Marking 중 해체한 레코드는 큐가 빌 때(cutoff·취소·오류)까지 재사용하지 않아 "해체됨" 판정이 같은 cycle 안에서 유지된다 |
| Marking 중 Create의 outer shade | 모든 새 객체 | 가입한 새 소속만 | 소속이 아닌 새 객체는 shade 뒤 trace되어 outer를 방문한다(기존 규칙). 차이가 생기는 것은 처리된 클러스터에 가입한 소속뿐이다 |
| 소속 불변식 검증 | Debug 장부 검증과 같이 | `verifyClusterOwnership`를 따로 켠다(World 시험·soak·World형 smoke) | 엔진 계층 API를 다른 용도(시험 모델)로 쓰면 소유가 클러스터 밖일 수 있다 |
| World API 밖 Actor 파괴 | World 레코드의 Component 목록으로 정리 | 목록에 더해 World가 GC cycle을 시작하기 전에 레코드를 훑어 우회 파괴된 Actor를 정리한다(`RepairBypassedActors`) | 틱 없는 Actor는 Tick이 발견하지 못해, 그 사이 Begin의 소속 불변식 검증이 고아 소속을 만난다. 훑기는 cycle마다 O(Actor 수)다 |
| World 클러스터 끄기 | 항상 | `WorldDesc::levelClusters`(기본 켬) | 측정 비교와 객체 단위 표식 순서를 보는 기존 장벽 시험 2개 |
| 패키지 클러스터 끄기 | 항상 | `RegistryOptions::packageClusters`(기본 켬) | 측정 비교 |
| struct 원소 제거·비우기 | 장부가 원소 Trace로 셈 | 관찰자 집계와 장부가 원소 trace 한 번의 결과를 같이 쓴다 | 원소마다 trace를 두 번 부르지 않는다(관찰자 시험이 trace 횟수를 센다) |
| Engine.cpp | 한 줄 | 영구 창을 연 Registry 생성(옵션 도우미)과 초기화 뒤 닫기(실패하면 초기화 실패, 서브시스템 단계와 다른 실패 지점) | 창을 열려면 생성 옵션이 필요하다 |
| M3 측정 타입 | GcBench·GcSoak 타입을 리플렉션 타입(Metadata·rocksids)으로 | GcBench 노드는 생성 함수와 같은 모양의 **수동** 참조 위치 함수를 쓴다(`ReportLayout`). 생성 경로는 World·리플렉션 시험 타입(EngineTests·Foundation 리플렉션 시험)이 Debug 대조와 함께 지난다 | 벤치·smoke 모듈에 MetaCompiler 단계를 넣지 않아도 표식 경로는 같다(표가 만들어진 뒤에는 생성·수동이 구별되지 않는다) |
| M3 표 범위 | 참조 위치 상수(컨테이너 버퍼 offset) | ObjectRef·ObjectRefArray 칸만 표로 나타내고, StructRefArray·ObjectRefMap·참조 struct 필드가 있는 타입은 trace로 남긴다. 표는 타입마다 첫 객체에서 멤버 주소 차이로 잰다(offsetof는 표준 배치가 아닌 클래스에서 오류) | 컨테이너 내부 배치를 marker가 알아야 하는 종류만 넣었다 |

### 13.9 4절 성능 gate (Phase 4 시점, 1M)

| gate | 목표 | Phase 3 | Phase 4 | 상태 |
|---|---|---|---|---|
| STW 병렬 전체 표식 | ≤ 15ms | ×16 23.9ms(부하 없는 재측정) | 일반 그래프는 부하 아래 41–180ms로 판정 불가. 클러스터가 맞는 그래프는 패키지 1% ×8 3.2ms, World형 ×8 6.5ms | 클러스터 그래프 **충족**, 일반 그래프 미정(부하 없는 재측정 필요) |
| 단일 스레드 전체 표식 | ≤ 60ms | 112–117ms | 표 켬 102–104ms(부하 100%). 패키지 1% 18.7ms, World형 20.4ms | 일반 그래프 미달, 클러스터 그래프 **충족** |
| Begin | ≤ 1ms | 0.04–0.12ms | 0.04–0.40ms(클러스터 레코드 epoch·루트 클러스터 항목) | **충족** |
| sweep 후보 판정 | ≤ 1ms | 0.012–0.03ms | 0.02–0.19ms | **충족** |
| 증분 Step p99 | ≤ 2ms | 직렬 2.0–2.1ms, 병렬 2.3–2.7ms | 직렬 표 켬 1.1–5.9ms·병렬 p95 1.04–2.1ms, p99는 선점(25–110ms) | 부하 아래 판정 불가(p95는 충족) |
| 증분 cycle | 2ms 예산에서 60프레임 이내 | 직렬 89·병렬 21–27 Step | 직렬 49–51, 병렬 21–22, 패키지 1% ×8 3, World ×8 5 | **충족** |
| Slot 정보 | ≤ 64B | 192 + 20.6B | 200 + 20.8B(클러스터 Registry는 + 28B) | 미달 |
| 기본 `maxSlots` | ≥ 2,097,152 | 2,097,152 | 같음(클러스터 Registry 상한 2^29) | **충족** |

### 13.10 검증

| 항목 | 결과 |
|---|---|
| Debug Foundation 전체 시험 | P4c 1758개, merge 뒤 P4f 2057개 전부 통과(P4b·P4d·P4e는 `--filter Object`·`GcSoak`·`Reference` 범위 통과) |
| Debug EngineTests | P4d 1827개, P4e 1832개, merge 뒤 2183개(P4f 포함) 전부 통과. World 시험 전부가 Debug 장부·소속 불변식 검증을 켠 채 돈다 |
| Debug Foundation `-t:Test`(P4f — MetaCompiler 변경) | 네이티브 2057개, 도구 self 134개, native 117개, meta 208개 통과 |
| 클러스터 시험(`ObjectGcCluster` 13개) | 기본 규칙·13.8절 장벽 경우·map 편집·패키지(정지형·증분·병렬)·영구 영역(정지형·증분·병렬)·무작위 모델(직렬·병렬, oracle에 소속 간선 포함, slice마다 안전성·정지형 뒤 정확성) — `--repeat 10` 통과(P4b 시점) |
| 병렬 oracle(`ObjectGcParallel`) | 표 타입(노드·poison)과 trace 타입(수동 노드)을 섞은 그래프, Debug 표·Trace 대조 통과 |
| clang-format 21.1.6·policy | 커밋마다 통과(merge 뒤 새 영어 결과물 policy 포함) |
| Asan | 돌리지 않았다(2026-11-17 사용자 결정) |
| 기존 시험의 기대 변경 | `ObjectDiscard_DuringMarkingAndSweeping`(패키지 클러스터가 표식된 뒤 Marking 중 Discard는 보수적으로 한 cycle 남는다 — 13.6절), `ReferenceObserver_UnregisteredElementStructIsUnsupported`(관찰자 없는 비교 Registry는 패키지 클러스터를 끈다 — 소속 소유자의 원소 제거는 원소 Trace가 필요하다), World 장벽 시험 2개(Level 클러스터를 끈다 — 객체 단위 스캔 순서를 지나야 한다), `ObjectIncrementalGcMatchesIndependentGraphOracleWithParallelMarking`(worker가 실제로 trace했는지의 경쟁 운 단언 삭제 — Phase 3에서 들어온 시간 의존 단언) |

### 13.11 발견과 남은 것

- **경계가 큰 클러스터의 간선당 비용.** 클러스터 그래프의 표식은 간선마다 대상의 `clustered` 비트를 보고, 소속이면 장부 칸(28 B — 대상마다 다른 캐시 줄)에서 클러스터 번호를 읽고 레코드 epoch를 비교한다. 경계 소속 비율이 높으면(패키지 사이 간선 30%) 이 두 번의 추가 읽기로 직렬 표식이 클러스터 없는 경우보다 1.9배 느려졌다. → 14절에서 고쳤다.
- **병렬 표식과 M3.** 직렬은 표로 1.6–2.2배 빨라졌지만 병렬 정지형은 이 부하에서 개선을 확인하지 못했다. 병렬 marker의 지역 큐는 LIFO라 "4칸 앞" 미리 읽기가 곧 꺼낼 객체를 겨누지 않는다. 부하 없는 기계에서 다시 재고, 블록 단위 미리 읽기를 검토한다.
- **부하 없는 재측정.** 이 Phase의 모든 측정은 부하 100%(같은 기계의 에뮬레이터와 다른 세션의 빌드) 아래였다. 병렬 STW gate(15ms)·증분 p99 gate(2ms)는 판정하지 못했다.
- **장부 메모리.** 클러스터 Registry는 슬롯당 28 B를 더 쓴다. 고리 칸(16 B)은 페이지 안 16비트 번호로 줄일 수 있다.
- **영구 영역의 실제 효과.** 지금 Engine 서브시스템은 초기화 동안 Registry 객체를 만들지 않아 엔진에서의 효과는 0이다(벤치의 100K 영구 객체는 시작 콘텐츠를 가정한 것).
- **World 우회 파괴 발견.** 틱 없는 Actor의 우회 파괴는 다음 GC cycle 시작 때 O(Actor 수) 훑기로 정리한다. Actor가 많은 World에서 cycle마다의 비용을 잰 적이 없다.
- **클러스터 smoke가 멈춘 것처럼 보인 일(2026-11-18, 도구 결함).** `GcBench --cluster-soak 5000 --seed 31`(World형)이 13분 넘게 단일 스레드 100%로 출력 없이 돌았다. cdb로 세 번 표본을 뜬 스택이 모두 smoke 쪽 oracle(`ClusterSoak::Reach` ← `CheckSafety`)이었고, 같은 seed의 tick 수를 늘리면 1,000 tick 1.8초 → 2,000 tick 15.4초로 초선형이었다(무한 루프가 아니라 비용 폭증). 원인 둘: (1) Level 생성이 모델 상한(`maxObjects`)을 보지 않아 Level을 파괴·재생성할 때마다 모델 항목이 끝없이 늘었고(2,000 tick에서 2,350개 — 상한 1,500), (2) oracle이 도달한 클러스터 루트·패키지 객체마다 모델 전체를 훑어 slice마다 O(루트 × 객체)였다. Registry·GC 결함은 아니다. Level 생성에 상한을 걸고, oracle을 slice마다 인접 목록을 만드는 O(객체) 방식으로 바꾸고, smoke에 진행 표시를 더했다. 시험 `GcSoak_ClusterWorldStaysWithinModelLimit`(상한 120, 600 tick, `created ≤ maxObjects`)가 상한 검사를 빼면 실패함을 확인했다.
- **시험 구멍.** 소속 소유자의 `StructRefArray` 원소 제거·비우기 장부 경로는 등록 struct가 필요해 클러스터 시험·smoke가 지나지 않는다(관찰자 시험이 같은 trace 경로의 집계만 본다). 클러스터 smoke의 패키지형은 oracle이 O(객체²)라 객체 1,500개 안팎에서만 돈다.
- **cutoff의 클러스터 순회.** cutoff와 정지형 sweep 직전에 레코드 표 전체를 훑어 표식되지 않은 클러스터를 해체한다(잠금 안, O(클러스터 수)). 패키지 10K개에서 Begin·sweep 시간에 드러나지 않았다(0.3ms 이하).

## 14. 경계 큰 클러스터의 표식·참조 쓰기 비용 (2026-11-18)

13.11절 첫 항목의 후속이다. 원인을 계측으로 나눈 뒤 구조를 바꾸고, 경계가 거의 전부인 패키지 클러스터에만 안전장치를 더했다. 설계 개정 3([13.4·13.5·13.9절](../Design/GarbageCollectionScale.ko.md))에 반영했다.

| 커밋 | 내용 |
|---|---|
| `4f6cd09` | 직렬 표식 작업 계수(`RegistryOptions::countGcWork`, `GcReport::work`)와 GcBench `--package-profile CROSS` |
| `f11a9ee` | 계수를 `--count-work`로 분리(계수의 상태 읽기가 간선마다 시간을 키운다), 반복 중 표식 최솟값, `--reps` 상한 |
| `d3f89e1` | GcBench `--write-rounds`(간선 쓰기 반복 측정) |
| `2588dc9` | 장부 배치와 표식 경로(14.2절) |
| `c37ffc5` | 경계 비율 안전장치(14.3절)와 시험 |

### 14.1 원인 분해 (패키지 1M, 사이 30%, 직렬 Collect, 바꾸기 전 `4f6cd09`)

방법: (1) 작업 계수(`--count-work`). (2) 표본 프로파일러. ETW·WPR 커널 추적은 관리자 권한이 없어 쓸 수 없어서, 자식 프로세스의 주 스레드를 약 1ms마다 멈추고 RIP를 읽는 도구를 따로 만들었다(scratchpad `sampler/` — `SuspendThread`·`GetThreadContext`, 기호는 Windows Kits의 dbghelp, 줄 단위 집계). 캐시 미스 수를 직접 세는 하드웨어 계수기는 같은 이유로 쓸 수 없어, 미스는 표본이 몰린 적재 명령과 배열 크기로 판정했다. (3) 같은 실행 안 번갈아 잰 표식 최솟값.

작업 계수(클러스터 켬 ↔ 끔): trace한 객체 637,483 ↔ 1,000,000, 방문한 간선 3,268,510 ↔ 3,956,181(켬은 모두 소속 대상 간선, 그중 같은 클러스터 2,082,362), 클러스터 항목 10,000, 경계 소속 항목 637,483, detached 0, 클러스터 처리 5.5ms. 켬이 일을 덜 하는데도 표식은 165ms ↔ 끔 약 60ms였다.

| 항목 | 표본 비율 | 시간 | 단위 비용 | 메모리 |
|---|---:|---:|---|---|
| 간선마다 대상 장부 칸(28 B)에서 클러스터 번호 읽기 | 52.4% | 약 86ms | 소속 대상 간선당 26ns | 칸 배열 28MB를 무작위로 읽음 — 간선마다 캐시 줄 1개가 더(마크 비트 줄과 별개) |
| 클러스터 레코드 epoch 비교 | 10.7% | 약 18ms | 간선당 5.4ns | 레코드 표 10K × 약 40 B |
| 경계 고리 순회와 경계 소속 항목 넣기 | 2.7% | 약 4.5ms | 경계 소속당 7ns | 고리는 같은 패키지 안이라 대부분 캐시 적중 |
| detached 처리 | 0 | — | — | 이 그래프에는 없음 |
| 큐 넣기 증가 | 0.6% | 약 1ms | — | 경계 소속 637K + 클러스터 10K 항목(끔은 1M 항목) |
| 나머지(trace·간선 방문·비트 묶음) | 약 33% | 약 55ms | — | 끔과 같은 종류의 일 |

결론: 원인은 고리 순회·큐가 아니라 **간선마다 소속 대상의 장부 칸과 레코드를 한 번씩 더 읽은 것**이다(간선당 약 31ns, 30% 그래프에서 표식의 63%).

### 14.2 구조 개선

단계별 효과(30%, 같은 실행 번갈아 잰 표식 최솟값 ms, 부하 때문에 단계 사이 절대값은 round마다 다르다).

| 단계 | 바꾼 것 | 표식 | 끔 |
|---|---|---:|---:|
| 0 | 바꾸기 전(장부 칸 28 B에 상태 포함, 레코드에 epoch) | 159.8 | 61.6 |
| 1 | 장부 상태를 4 B 밀집 배열로, 표식 epoch를 클러스터 번호로 찾는 u32 배열로 | 117.5 | |
| 2 | + 이번 cycle에 클러스터 표식을 본 소속 비트(`clusterSeen`): 같은 소속으로 가는 다음 간선은 비트 하나로 끝 | 74.6 | 61.6 |
| 3 | + chunk마다 클러스터 하나 힌트 | 71.7 | — |
| 4 | 힌트를 chunk당 클러스터 둘(번호 둘 + 고름 비트)로 | 53.5 | 57.8 |
| 5 | 클러스터 표와 `clusterSeen`을 비트 묶음(`GcBits`, 32 → 64 B 한 캐시 줄)에 넣음 | 49.3 | 51.9 |

- 3단계 프로파일: 표식의 30%가 섞인 chunk의 장부 상태 읽기였다. 패키지(100객체)가 64칸 chunk 경계를 가르면 chunk의 63%가 클러스터 둘을 담아 단일 힌트가 쓸모없었다. 4단계 뒤 그 줄은 4%가 됐다.
- 5단계는 시간상 같은 수준이었지만(49.1 ↔ 49.3, 50.5 ↔ 50.2) 자료 구조를 하나로 줄이고 클러스터를 끈 Registry의 표식도 그대로였다(끔 51.5 ↔ 51.9, 51.1 ↔ 50.6). 클러스터 표는 모든 Registry의 비트 묶음에 있다(슬롯당 +0.5 B).
- chunk 클러스터 표의 소속 번호 미리 읽기(간선 배치에서 비트 묶음과 함께)는 차이가 없어 넣지 않았다(30%: 51.5 ↔ 51.6, 51.6 ↔ 50.5).
- 바꾼 뒤 30% 프로파일: 소속 판정·클러스터 찾기 7.9%(약 4.2ms, 처음 만난 소속당 약 4ns), 클러스터 epoch 3.3%, 경계 고리 순회 5.4%. 클러스터 고유 비용이 표식의 약 17%(약 9ms)로 줄었다(전: 약 66%·108ms).

**참조 쓰기.** marker가 더는 장부 상태를 읽지 않으므로 장부는 쓰기 쪽에 맞췄다.

| 배치 | 간선 분류 | 같은 클러스터 간선 추가 | 제거 | 결과 |
|---|---|---|---|---|
| 전(28 B 칸에 상태·계수) | 소유자 칸·대상 칸 | 칸 2개(서로 독립이라 겹친다) | 칸 2개 | 기준 |
| 상태를 Slot 채움 자리로 | Slot 2개(검증으로 이미 읽음) | 대상 칸 1개 | 대상 Slot + 대상 칸 | 제거가 7–23% 느려짐(Slot 200 B 간격 배열의 무작위 읽기는 TLB 미스까지 겹친다) — 버림 |
| 상태는 칸, 분류는 chunk 표 | 소유자·대상 비트 묶음(1MB, 캐시에 남음) | 대상 칸 1개 | 대상 칸 1개 | 전과 같은 수준 |
| **상태는 칸, `inCount`는 Slot 채움 자리, 분류는 chunk 표(채택)** | 비트 묶음 | **없음**(대상 Slot은 검증이 이미 읽었다) | 대상 Slot 1개 | 추가 2–5% 빠름, 제거 같은 수준 |

장부 칸은 28 → 24 B(`inCount`가 Slot으로), Slot 크기는 그대로(200.3 B)다.

### 14.3 경계 비율 안전장치

구조 개선 뒤에도 사이 간선 100%(경계 약 88%) 그래프는 끔보다 6–8% 느렸다(55.3–55.8 ↔ 51.6–52.4ms). 경계 소속은 어차피 trace되고 나머지 소속도 처음 만날 때 클러스터 찾기를 치르기 때문이다. 그래서 **패키지 클러스터**의 경계 소속 수(`ClusterRecord::boundaryMembers`, 경계 고리 출입 때 센다)가 소속의 `ClusterFlattenBoundaryPercent`(80%)를 넘으면 cycle 끝에 해체한다.

- 시점: 증분은 cutoff에서 표식되지 않은 클러스터를 해체한 직후(남은 클러스터는 표식됐으므로 해체가 소속에 mark를 켠다), 정지형은 sweep 분류 뒤. 결과 집합은 그대로다(시험이 정지형·증분·병렬에서 확인).
- 해체된 패키지(살아 있는 패키지 객체가 소속이 아니다 — `IsFlattenedPackageLocked`)의 새 객체는 가입하지 않는다. 다시 묶지 않는다.
- 엔진이 지정한 클러스터(World Level)와 영구 영역은 대상이 아니다. 해체하면 다음 가입 지정이 `StaleHandle`이 되고, World형은 같은 Level 안 참조라 경계가 작다.
- `GcReport::flattenedClusters`가 cycle마다 해체 수를 센다. Debug 장부 검증이 경계 소속 수를 고리 길이와 대조한다.

문턱 측정(표식 최솟값 ms, 부하가 적은 round만 — 원시 출력 `p4/bits.txt`·`p4/slot.txt`):

| 사이 간선(평균 경계) | 안전장치 없음 | 70% | 80% | 90% | 끔 |
|---|---:|---:|---:|---:|---:|
| 30%(64%) | 49.3 · 50.9 | 51.0 · 54.3 | 51.7 · 49.9 | 49.9 · 50.4 | 51.9 · 53.4 |
| 40%(72%) | 50.2 · 52.1 | 52.0 · 53.5 | — | — | 50.6 · 50.6 |
| 50%(77%) | 52.3 · 52.9 | 53.2 · 53.0 | 54.6 | 53.9 | 51.5 · 53.2 |
| 70%(83%) | 53.7 | — | 54.1 | 55.1 | 54.4 |
| 100%(88%) | 55.8 · 57.1 | 53.5 · 52.8 | 53.3 | 71.1(흔들림) | 51.6 · 52.9 |
| 해체 수(30/50/70/100%) | — | 782 / 9,379 / — / 10,000 | — / 2,003 / 7,896 / 9,878 | — / 2 / 177 / 2,132 | — |

70%는 경계 70–80% 패키지 일부만 해체해 40–50% 그래프를 1–2ms 느리게 했다(해체된 패키지의 객체가 모두 trace되면서 남은 클러스터 소속으로 가는 간선은 그대로 찾기를 치른다). 90%는 100% 그래프에서도 21%만 해체해 효과가 없다. 80%는 100% 그래프를 끔 수준으로 되돌리고 70% 이하에는 차이가 없다.

### 14.4 전후 비교

패키지 직렬 표식 최솟값(ms, `--package-profile`, 높은 우선순위로 같은 실행 번갈아 두 round — `p4/final-mark-high.txt`):

| 사이 간선 | 전 | 후 | 끔 | 후 ↔ 끔 |
|---|---:|---:|---:|---|
| 1% | 9.6 · 10.3 | 7.0 · 7.2 | 48.8 · 50.6 | 7배 빠름(전보다도 27% 빠름) |
| 10% | 58.0 · 62.3 | 37.4 · 39.9 | 48.2 · 51.3 | **22% 빠름** |
| 30% | 106.4 · 113.7 | 47.5 · 48.9 | 49.8 · 52.6 | **5–7% 빠름** |
| 50% | 132.0 · 150.8 | 57.1 · 56.4 | 56.6 · 54.7 | 같은 수준(+1–3%) |
| 100% | 202.6 · 193.0 | 58.1 · 53.1 | 52.0 · 52.6 | +1–12%(안전장치가 98.8% 해체) |

`GcBench --clusters` 전체(높은 우선순위, 전 ↔ 후 두 round, 정지형 표식 중앙값 ms):

| 시나리오 | 전 켬 | 후 켬 | 끔 |
|---|---:|---:|---:|
| 패키지 1% 직렬 · 병렬 ×8 | 11.1 · 14.0 / 3.3 · 3.5 | 8.1 · 11.8 / 2.7 · 2.8 | 74.7 · 72.8 / 32.5 · 31.9 |
| 패키지 10% 직렬 · ×8 | 90.4 · 88.7 / 14.9 · 15.9 | 56.3 · 53.7 / 12.0 · 11.9 | 71.6 · 88.1 / 39.5 · 38.7 |
| 패키지 30% 직렬 · ×8 | 153.4 · 203.6 / 27.2 · 28.2 | 74.7 · 72.6 / 18.0 · 16.2 | 79.4 · 83.4 / 43.4 · 42.9 |
| World 직렬 · ×8 | 20.9 · 21.2 / 6.1 · 6.9 | 17.6 · 16.6 / 5.9 · 6.2 | 49.3 · 44.4 / 21.0 · 21.5 |

간선 쓰기(ns/op, `--write-rounds 15`(50K번씩 15회)의 최솟값, 높은 우선순위, 부하가 적은 round — `p4/writes-high.txt`):

| 사이 간선 | 전 추가 · 제거 | 후 추가 · 제거 | 끔 추가 · 제거 |
|---|---|---|---|
| 1% | 451 · 326 | 429 · 325 | 439 · 244 |
| 10% | 443 · 323 | 436 · 333 | 452 · 254 |
| 30% | 453 · 341 / 455 · 333 | 438 · 353 / 444 · 346 | 456 · 257 / 427 · 239 |

메모리(GC 밀집 자료, 슬롯당): 패키지형 켬 48.9 → 45.4 B, 끔 20.8 → 21.3 B. World형 켬 52.2 → 48.7 B, 끔 24.2 → 24.7 B. Slot 200.3 B 그대로.

- 목표 대조: 30% 켬 ≤ 끔 **충족**, 10% 켬이 끔보다 분명히 빠름 **충족**(22%), 1%·World 이득 **유지**(둘 다 오히려 빨라졌다), 참조 쓰기 **추가는 2–5% 줄고 제거는 같은 수준**(제거의 남은 몫은 아래).
- 제거에 남은 장부 비용(끔보다 약 80–100ns)은 대상의 `inCount` 하나를 고치는 무작위 메모리 읽기 하나다. 이 계수는 소속이 이탈할 때 detached 여부를 정하는 데 필요해 없앨 수 없고, 전에는 소유자 칸·대상 칸 두 읽기가 서로 겹쳐 같은 시간이 걸렸다. 추가는 대상 Slot을 검증이 이미 읽어 장부가 거의 공짜다.

### 14.5 검증

| 항목 | 결과 |
|---|---|
| Debug Foundation 전체 | 2,062개 통과(안전장치 시험 3개 포함). 클러스터 시험 `--repeat 5` 16개 통과 |
| 구조 개선만의 중간 상태(`2588dc9`) | Debug `ObjectGcCluster` 13·`GcSoak` 6·`ObjectGcParallel` 10·`ObjectIncrementalGc` 11개 통과 |
| Debug EngineTests 전체 | 2,188개 통과(World 시험은 장부·소속 불변식 검증을 켠 채 돈다) |
| 클러스터 smoke(Debug — Begin·Collect마다 장부 재계산, World형은 소속 불변식) | World형 seed 41(직렬)·43(worker 4) 1,500 tick: cycle 1,286·정지형 대조 215번씩 PASS. 패키지형 seed 42(직렬)·44(worker 4) 300 tick: cycle 258·대조 43번씩 PASS |
| clang-format·policy | 통과 |
| Asan | 돌리지 않았다(사용자 결정) |

Debug 장부 재계산은 새 배치를 본다: 칸의 `clusterState`·`outCount`, Slot의 `inCount`, 경계 소속 수를 고리와 다시 세어 대조하고, 소속마다 chunk 클러스터 표의 번호를 장부 상태와 대조한다(대조 2·3이 이 검사로 잡혔다).

음성 대조(Debug Foundation을 고쳐 빌드하고 클러스터·smoke·병렬·증분 시험을 돌린 뒤 되돌림 — `p4/negctl2.txt`):

| 대조 | 바꾼 것 | 잡은 시험 | 증상 |
|---|---|---|---|
| 1 | 증분 Begin이 `clusterSeen`을 지우지 않음 | 클러스터 무작위 모델·클러스터 smoke | "Reachable object was reclaimed"(tick 2), 모델 안전성 단언 |
| 2 | 가입이 둘째 칸 번호만 쓰고 고름 비트를 켜지 않음 | 클러스터 시험·smoke | "Chunk cluster hint diverged from the ledger", "Cluster ledger diverged from a recount" |
| 3 | 경계 고리에서 뺄 때 경계 소속 수를 줄이지 않음 | 클러스터 시험·smoke | "Cluster boundary count diverged from its boundary list" |
| 4 | 정지형이 sweep 분류 **전**에 안전장치 해체(소속이 mark 없이 풀림) | 안전장치 시험(정지형) | 해체된 패키지 객체가 회수되어 뒤 생성이 실패. smoke는 이 경로(정지형 대조 직후 해체)를 잡지 못했다 |

### 14.6 남은 것

- 50–100% 그래프는 끔과 같은 수준까지만 왔다. 남은 몫은 경계 소속 trace(끔과 같은 일)와 소속마다 처음 한 번의 클러스터 찾기(약 4ns), 경계 고리 순회다.
- 안전장치는 패키지 클러스터만 해체한다. 경계가 큰 World Level 클러스터는 엔진 계층이 가입을 정하므로 그대로 둔다. 그런 Level이 실제로 생기면 World가 Level 클러스터를 끄는 정책이 필요하다.
- 해체된 패키지는 다시 묶지 않는다. 경계가 나중에 줄어도 일반 객체로 남는다.
- 섞인 chunk(클러스터 셋 이상 — 작은 패키지가 많을 때)는 장부 상태를 읽는다. 패키지 크기가 64보다 작은 그래프를 따로 재지 않았다.
- 측정은 다른 세션의 빌드·에뮬레이터가 도는 기계에서 했다. 높은 우선순위와 최솟값으로 선점의 영향을 줄였지만 캐시 경합은 남는다.

## 15. 클러스터 시험 구멍과 작은 패키지 (2026-11-18)

### 15.1 시험 구멍

| 구멍(13.11절·13.7절) | 메운 것 | 확인(음성 대조, `p4/negctl3.txt`) |
|---|---|---|
| 소속 소유자의 `StructRefArray` 원소 제거·비우기 장부 경로를 아무 시험도 지나지 않음 | 등록 struct 원소를 둔 시험 타입으로 `ObjectGcCluster_MemberStructElementRemovalKeepsLedger`(같은 클러스터 대상 둘·밖 대상 하나를 원소로 가리키고, 원소 제거 → 밖 대상 회수, 이탈한 대상은 원소가 가리키는 동안 detached, 비우기 → 회수). 클러스터 smoke에 원소 추가·제거·비우기를 섞음(World형·패키지형 모두) | 제거 경로의 장부 갱신 생략(S1)·비우기 경로의 생략(S2) 모두 "Cluster ledger diverged from a recount"로 잡힌다. Foundation의 짧은 smoke 시험은 잡지 못한다(긴 smoke 실행은 원소 편집을 지난다) |
| 삽입 장벽을 끈 결함(음성 대조 7)을 클러스터 결정적 시험이 못 잡음 — keeper가 연결 시점에 아직 trace 전이었다 | 첫 slice가 루트(keeper 포함 다섯)를 모두 trace하고 루트 없는 사슬이 남아 표식이 끝나지 않게 한 뒤 keeper → 소속 간선을 건다(이 간선은 장벽만 지킨다) | 장벽을 끄면 `ObjectGcCluster_IncrementalBarrierCasesKeepReachableObjects`가 소속·밖 대상 회수로 실패한다(smoke·병렬·증분 시험도 함께) |
| 경계 비율 안전장치가 smoke에서 일어나지 않음(패키지 객체에 간선이 없어 경계가 75%를 넘지 못했다) | smoke가 패키지 객체에도 간선을 두고, 기존 패키지 아래 새 객체를 만들며, 해체된 클러스터를 모델에 옮긴다. 이를 위해 진단용 공개 조회 `ObjectRegistry::InspectClusterMembership`을 더했다 | 패키지형 smoke 300 tick에서 안전장치 해체 372–420건. 해체된 패키지 인식을 끄면(F1) 안전장치 시험이 새 객체 생성 실패로 잡는다 |

### 15.2 작은 패키지(64개 미만 — chunk 하나에 클러스터 셋 이상)

`GcBench --package-profile CROSS --package-size N`(1M 객체를 N개씩 패키지로)으로 쟀다. 14절 배치는 chunk 표가 클러스터 둘까지만 담아, 패키지가 10–30개면 거의 모든 chunk가 "섞임"이 되어 소속을 처음 만날 때마다 장부 칸(28MB 배열)을 읽었다(크기 10·사이 30%에서 표식의 29%가 그 한 줄).

고친 것: **2단계 chunk 표.** `GcBits`의 표(클러스터 둘)가 넘치면 클러스터를 켠 Registry만 가진 넓은 표(`ClusterChunk`, chunk당 64 B — 클러스터 8개와 소속마다 3비트 고름)로 옮기고, 그것도 넘치면 장부 상태를 읽는다. `clusterSeen`은 비트 묶음에 남겼다 — 넓은 표에 함께 두었더니 같은 소속으로 가는 반복 간선마다 줄 하나를 더 읽어 패키지 100개 그래프가 76 → 90ms로 느려졌다.

| 패키지 크기 · 사이 간선 | 14절 배치 | 2단계 표 | 끔 |
|---|---:|---:|---:|
| 10 · 1% | 9.3–13.6 | 8.6–13.9 | 47–69 |
| 10 · 10% | 50–66 | 54–60 | 60–73 |
| 10 · 30% | 101–110 | 75–87 | 52–60 |
| 30 · 1% | 11.7–13.4 | 8.0–10.1 | 62–67 |
| 30 · 10% | 66–74 | 54 | 69 |
| 30 · 30% | 102–108 | 57–77 | 50–57 |
| 100 · 10% | 38–42 | 37–45 | 49–62 |
| 100 · 30% | 50–58 | 47–64 | 50–55 |

표식 최솟값(ms), 높은 우선순위, 같은 실행 번갈아 두 round(`p4/two.txt`). 부하가 적던 round만 보면 2단계 표는 크기 10에서 38 · 56 · 58ms(끔 49 · 52 · 50), 크기 30에서 39 · 48 · 54ms(끔 50 · 56 · 74)다(`p4/smallvalve.txt`).

- 남은 차이: 크기 10·사이 30%(경계 58%)는 끔보다 15–40% 느리다. 프로파일에서 넓은 표 읽기 19%, 클러스터 epoch 비교 11%다(소속 1M을 처음 만날 때마다 — 클러스터 100K개).
- 시도하고 버린 것: (1) 간선 배치에서 넓은 표 미리 읽기 — 크기 10에서 들쭉날쭉, 크기 100에서 느려짐. (2) 작은 클러스터(소속 32개 미만)의 안전장치 문턱을 40·55%로 — 해체된 패키지와 남은 클러스터가 섞여 오히려 느려졌다(부하가 적은 round: 크기 10·30%에서 안전장치 없음 57.7, 40% 61.8, 55% 65.1ms).
- 메모리: 넓은 표가 클러스터 Registry에 슬롯당 1 B를 더한다(GC 밀집 자료 45.4 → 46.4 B/slot).

## 16. 선형 검색 점검 (2026-11-18)

범위: ObjectRuntime(Registry·GC·장부·관찰자·이름 색인), World·Actor·Level·TickScheduler, Engine 서브시스템, Core 할당자. 연산 하나나 cycle·프레임 하나의 비용이 객체·클러스터·Actor·할당 수에 비례하는 곳을 모두 찾았다(읽기 전용 조사 1회 + 직접 확인). 측정은 새 도구 `Programs/WorldBench`(Actor N개 World — 파괴 O(1) 벤치 할당자, Actor 하나의 파괴, GC cycle을 시작하는 프레임, Actor 하나를 만든 뒤의 프레임, 작은 Level 파괴)와 GcBench `--package-size`·`--incremental-reps`로 했다. 전·후는 같은 실행에서 번갈아 높은 우선순위로 쟀다(`p4/worldbench2.txt`·`p4/worldbench4.txt`·`p4/settle.txt`).

### 16.1 고친 것 (연산·cycle·프레임마다 규모에 비례하던 곳)

| 위치 | 비례 대상 | 빈도 | 전: 10K → 100K Actor | 후: 10K → 100K | 처치 |
|---|---|---|---:|---:|---|
| `World.cpp` RepairBypassedActors(구 1779행) | 모든 Actor(Actor마다 `InspectLifecycle` 잠금 1회) | GC cycle 시작마다 | cycle 시작 프레임 207 → 2,390us | 2.0 → 3.8–13.2us | Registry의 파괴 요청 목록(`TakeDestroyRequests` — DestroyRequested가 된 슬롯의 이중 연결 목록, 회수 때 뺀다)에서 그 사이 파괴만 꺼낸다. O(파괴 수) |
| `World.cpp` DestroyActorNow → `RemoveReference(Level.actors_)` | 그 Level의 Actor 수(찾기 + 뒤 원소 당기기) | Actor 파괴마다(대량 파괴는 O(A²)) | 5.0 → 43.7us/Actor | 0.71 → 0.52–1.02us/Actor | Actor 레코드가 Level 배열 위치를 기억하고 끝 원소와 바꿔 뗀다(`SetReferenceAt` + 끝 `RemoveReferenceAt`, 둘 다 O(1)). Level 배열 순서는 생성 순서가 아니게 됐다 |
| `World.cpp` DestroyLevel(구 387행) | World 전체 Actor | Level 파괴마다 | 작은 Level(16 Actor) 21 → 716us | 1.3 → 1.5us | 그 Level의 배열만 훑는다 |
| `TickScheduler.cpp` RebuildOrder | 모든 틱 등록(전 단계 훑기 + 위상 정렬) | 등록·해제·선행 추가가 있던 단계마다 프레임 경계에서 | 틱 있는 Actor 하나 생성 뒤 프레임 − 빈 프레임: 약 1.6 → 22ms | 약 0 → 0(−2 … +0.1ms 측정 흔들림) | 새 등록이 priority 최댓값 이상이면 순서 끝에 붙인다(재구성 결과와 같다). 해제는 순서에 남겨 실행이 건너뛰고 절반을 넘으면 재구성한다. 선행 추가는 지금 순서가 이미 지키면 재구성하지 않는다(그 간선이 막는 선택은 원래도 일어나지 않았다). 그 밖은 지금처럼 재구성 |
| `TickScheduler.cpp` Reaches | 모든 틱 칸(방문 표시 배열 비우기) | 선행 추가마다 | — | — | 호출 번호(epoch) 표시로 방문한 칸만 본다 |
| `TickScheduler.cpp` ApplyPendingRemovals | 모든 틱 등록 | 실행 중 해제가 있던 단계 경계 | — | — | 미룬 해제 목록만 지운다(용량은 등록 때 확보 — 해제는 할당하지 않는다) |
| `World.cpp` ProcessStarts | 시작을 기다리는 꺼진 Actor | 프레임마다 | — | — | 꺼진 Actor는 큐에서 빼고 활성화가 다시 넣는다 |
| `ObjectRegistryGc.cpp` AcquireGcType | GC 타입 수 | 객체 생성마다 | — | — | 타입 키 → 번호 개방 주소 색인(적재율 ≤ 1/2) |

"—"는 따로 재지 않은 것(구조상 비용이 방문·변경 수에 비례한다). 틱 있는 World의 "Actor 하나의 파괴" 값은 정리 프레임 하나(모든 Actor 틱)를 200개로 나눈 것이 섞여 크기에 비례해 보인다(후 10K 3.0–7.2us, 100K 57–108us — 빈 프레임 10–21ms / 200). 틱 없는 World에서는 0.5–1.0us로 같다.

### 16.2 그대로 둔 것

| 위치 | 비례 대상 | 빈도 | 1M · 10K 클러스터 · 10만 Actor 규모 | 판단 |
|---|---|---|---|---|
| `ObjectRegistryClusters.cpp` DissolveUnmarked·FlattenBoundary 레코드 표 훑기 | 레코드 표(최고 수위) | cycle마다 1회(cutoff slice 안, 정지형 sweep) | 10K·100K 클러스터 모두 cutoff slice 최댓값 1.04–1.10ms(예산 1ms의 soft 초과 범위 — 클러스터 수와 무관). 표 훑기는 100K에 약 0.1ms | 그대로. slice마다 나누는 판(Sweeping의 레코드 커서, IsCutoff의 클러스터 epoch 판정)을 만들어 쟀으나 차이가 없어 되돌렸다 |
| ProcessCluster·해체의 경계·소속·detached 고리 | 그 클러스터의 경계·소속 수 | 표식된 클러스터마다 cycle 1회, 해체마다 | 경계 소속당 약 7ns(14.1절) — 9만 소속 Level의 해체 약 0.6ms | 그대로(표식·해체할 양에 비례하는 일). slice로 나누지 못한다 |
| `ObjectRefArray::IndexOf`·`RemoveReference(handle)` | 그 배열 크기 | 호출마다 | 배열 크기만큼 | API 의미(값으로 찾기). 큰 배열은 위치로 지운다 — World는 16.1처럼 바꿨다 |
| 이름 색인 rehash | 이름 있는 객체 수 | 색인이 찰 때(분할 상환 O(1)) | 1M 이름에서 한 Create가 rehash 하나를 진다 | 그대로(분할 상환). 잠금 안에서 일어나는 단발 지연은 기록 |
| `ReferenceObserver::TakeChanges`의 색인 비우기 | 관찰 키 색인 용량 | TakeChanges마다 | 키(패키지 쌍) 수 — 객체 수가 아니다 | 그대로 |
| `World.cpp` FindLevelRecord | Level 수 | Spawn·파괴마다 | Level 수십 | 그대로 |
| `World.cpp` 우회 파괴 Component의 소유 Actor 찾기 | Actor × Component | World API 밖에서 파괴된 Component마다(드묾) | — | 그대로(드문 복구 경로) |
| `World.cpp` ProcessStarts의 준비 안 된 Actor 재검사 | 준비를 기다리는 Actor | 프레임마다 | 그런 Actor 수 | 그대로(`IsReadyToStart` 폴링 API의 의미) |
| TickScheduler RunPhase | 등록된 틱(꺼진 것 포함) | 프레임마다 | 틱 수 | 그대로(프레임마다 실행할 일). 틱 하나의 비용이 10K에서 약 86ns, 100K에서 약 210ns로 늘어난다(Actor 레코드 해시와 Borrow의 캐시 미스) — 남은 일 |
| `VerifyClusterLedger`, Debug `Stats()` 재계산 | 모든 슬롯 | Begin·Collect·Stats마다 | — | Debug 전용 검증 |
| SystemAllocator Deallocate(살아 있는 할당 목록), BinnedAllocator Allocate·Deallocate(원장), PoolAllocator Deallocate(chunk 목록) | 살아 있는 할당 수·chunk 수 | 할당·해제마다 | 1M 생성·회수가 끝나지 않는다(13.10절 이전 측정) | GC-S6(8단계)에서 같은 기준으로 고친다 |
| GcSoak·클러스터 smoke 모델의 훑기 | 모델 객체 수 | slice·파괴마다 | 객체 수천 | 도구 쪽(정확성 대조용). 그대로 |

### 16.3 검증

- 15·16절 변경을 함께 넣은 상태: Debug Foundation 전체 2,063개, Debug EngineTests 전체 2,189개 통과. 클러스터 smoke(Debug — Begin·Collect마다 장부 재계산, World형은 소속 불변식) World형 seed 41(직렬)·43(worker 4) 1,500 tick, 패키지형 seed 42·44 300 tick 모두 PASS(패키지형 안전장치 해체 372–420건). clang-format·policy 통과.
- 음성 대조(`p4/negctl4.txt` — Debug Foundation·EngineTests를 고쳐 빌드하고 클러스터·smoke·증분·World·Tick 시험을 돌린 뒤 되돌림):

| 대조 | 바꾼 것 | 잡은 시험 | 증상 |
|---|---|---|---|
| A1 | RequestDestroy가 파괴 요청 목록에 넣지 않음 | World 시험 | Debug 검증 Fatal로 중단(우회 파괴 Actor의 Component가 고아 소속으로 남는다) |
| B1 | Level 배열에서 끝 원소를 옮긴 뒤 그 Actor의 위치를 고치지 않음 | World·Tick 시험 | "Actor record position diverged from its Level array" |
| C1 | 틱 도달 검사의 방문 번호가 오르지 않음 | Tick 시험 2개 | 사이클이 될 선행 추가를 받아들인다 |
| E1 | 꺼진 채 시작 큐에서 빠진 Actor를 활성화가 다시 넣지 않음 | `World_DisabledActorDefersStartUntilEnabled` | 활성화 뒤 OnStart·틱이 없다 |
| T2 | 새 틱 등록을 priority와 관계없이 끝에 붙임 | `Tick_PriorityThenRegistrationOrderWithinPhase` | 순서가 priority를 따르지 않는다 |
| T3 | 지금 순서가 어기는 선행 추가에도 재구성하지 않음 | `Tick_PrerequisitesOverridePriorityAndCyclesAreRejected` | 선행보다 먼저 실행된다 |
| T4 | 의존받는 틱의 해제를 재구성 없이 처리(의존 계수 없음) | 같은 시험 | 해제 뒤 풀린 의존의 순서가 옛 순서로 남는다 — 처음 구현에서 이 시험이 실제로 실패해 의존 계수를 더했다 |
