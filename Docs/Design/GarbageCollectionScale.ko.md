# Rocks GC 대규모 처리 보강 설계

기준일: 2026-11-16, 개정 1(초안). 개정 2(2026-11-17): 클러스터·영구 영역 상세 설계(13절), 열린 결정 4 확정, 열린 결정 6 해소, UE 클러스터 사실 정정.

**이 문서의 작업은 반드시 해야 하는 작업이다.** 현재 `ObjectRegistry` GC는 AAA급 규모(살아 있는 객체 100만 개 이상)의 게임에서 쓸 수 없는 상태다(2026-11-16 사용자 판단). 이 문서의 GC-S 단계는 선택 최적화가 아니며 성능 미달을 "측정 결과로 관리"하는 대상도 아니다. 단계를 모두 마치고 4절의 성능 gate를 통과해야 GC를 대규모 콘텐츠에 쓸 수 있다고 본다.

착수 시점은 사용자가 정한다(2026-11-16 "작업은 나중에 할 것"). 이 문서는 고정 구현 순서(A0-B → A0-O → 리플렉션 B9 → Curve → A0-G → A1–A6 → 화면 마일스톤)를 바꾸지 않는다. 완료 시한은 열린 결정이다(11절).

상태 표기는 다음과 같다. **설계**는 이 문서가 정한 계약이고 코드는 없다. **제안**은 측정 전 수치이며 GC-S0 벤치로 확정한다. **결정 필요**는 사용자가 정할 항목이다. 문서 전체가 설계이며 구현된 항목은 없다.

참조 기준은 Unreal Engine 5.8.4-release(commit `f45729c0b6f0`, 로컬 클론 `C:\github\UnrealEngine`)로 고정한다. 약칭은 다음과 같다. 경로는 `Engine/Source/Runtime/CoreUObject/` 기준이다.

- GC = `Private/UObject/GarbageCollection.cpp`
- FRC = `Public/UObject/FastReferenceCollector.h`
- UOA = `Public/UObject/UObjectArray.h`
- FLG = `Private/UObject/GarbageCollectionInternalFlags.h`

UE 소스는 설계 사실의 대조에만 쓰고 코드를 옮기지 않는다.

## 1. 결정 요약

| 항목 | 결정 | 근거 |
|---|---|---|
| 작업의 지위 | 필수. 선택 최적화가 아니다 | 서문, 3절 |
| 규모 책임 | 객체 규모는 엔진이 감당한다. 사용자에게 "객체를 적게 만들라"를 GC 문제의 1차 해법으로 요구하지 않는다 | 2절 |
| 성능 gate | 1M 객체 STW 병렬 표식 ≤ 15ms 등 4절의 수치(제안). GC-S0 벤치로 확정하고 이후 회귀 gate로 유지한다 | 4절 |
| 표식 동기화 | 표식에 Registry `gate_`를 쓰지 않는다. 표식은 atomic(바이트 교환 또는 bit `fetch_or`)으로 하고 이긴 쪽만 큐에 넣는다 | 3.2절, 5절, 계획 §6.2 2번 |
| GC 자료 배치 | GC가 읽는 값을 Slot에서 떼어 slot index로 접근하는 밀집 배열(SoA)로 둔다. Slot의 나머지는 그대로 둔다 | 3.2절 |
| 루트 | 루트 목록을 증분 관리한다. Begin은 O(루트 수)다 | UE `GRoots`, 3.2절 |
| sweep | live·mark bitmap의 word 연산으로 white 후보만 찾는다. Retire 순서는 슬롯 순서를 유지한다 | 3.2절, 7절 |
| 병렬 | worker별 큐와 512칸 블록, 블록 단위 훔치기. World가 scheduler를 넘긴다 | UE `FWorkBlock`, 3.3절 |
| 예산 | 증분 Step 예산의 기본을 시간으로 바꾼다. `maxObjects`는 상한 장치로만 남긴다 | 3.2절 7번 |
| 수집 일정 | STW 병렬 전체 수집을 정식 경로로 둔다. 증분은 선택지로 유지한다. 파괴·회수는 시간 예산으로 여러 프레임에 나눈다 | UE 기본값, 5절 |
| 할당자 | O(1) 소유권 판정, 스레드별 캐시, 크기별 slab을 갖춘 할당자를 Core Memory 공용으로 둔다. ObjectRuntime 전용 구현은 두지 않는다 | 3.2절 8번 |
| MetaCompiler 활용 | 생성 코드로 marker 전용 정적 특수화, 잎 타입 표시, 참조 위치 상수, 정적 schema를 낸다(M1–M7, 제안) | 6절 끝 "메타 컴파일러로 얻는 추가 여지" |
| 클러스터 | 패키지 정책 Registry의 패키지, World의 Level을 엔진이 자동으로 클러스터로 둔다. 모든 강한 간선 쓰기에서 장부를 정확히 갱신하므로 UE와 달리 런타임 변경으로 깨지지 않고 해체가 필요 없다 | 13절 |
| 영구 영역 | 특수 클러스터로 둔다. 영구 객체는 영구 객체만 가리킬 수 있다(결정 4 확정, 2026-11-17) | 13.7절 |
| 세대별 GC | 이 문서 범위 밖이다. 측정 전에는 보류한다. 클러스터·영구 영역과는 다른 기법이다 | UE도 쓰지 않는다 |

## 2. 원칙: 규모는 엔진의 책임

게임 엔진은 콘텐츠의 규모가 커져도 문제없이 구동되어야 한다. 객체 수를 줄여 GC를 피하는 것은 GC가 약한 엔진에서 사용자가 떠안는 회피책이다. 대표적인 예가 Unity다. Rocks는 이 부담을 사용자에게 넘기지 않는다.

- 객체 수 줄이기는 사용자의 선택적 최적화로만 다룬다. GC 비용을 이유로 사용자 설계를 강제하는 안내를 1차 처방으로 쓰지 않는다. 지금 [GC 안내](../Implementation/GarbageCollectionGuide.ko.md) 7.2절의 "값 배열·덩어리 데이터로 둘 수 있는지 검토"는 GC-S 완료 뒤 고친다(12절).
- [계획](EngineImplementationPlan.ko.md) §5.1의 "매 정점·입자·physics body·render instance를 Object로 생성하지 않는다"는 이 원칙과 충돌하지 않는다. 그 항목은 원래 GC 대상이 될 이유가 없는 대량 데이터의 표현 규칙이다. Entity·Component·에셋 객체처럼 정체성과 수명이 필요한 객체는 많아도 된다.
- 정확성이 먼저다. 오수거를 허용해 시간을 맞추지 않는다(계획 §6.4). 기존 정지형 `Collect`를 결과 대조용 oracle로 유지한다.

## 3. 현재 상태와 진단

### 3.1 현재 구현의 경로

기준 코드는 feature/engine-runtime `b2e87d3`이다. 경로는 `Core/Source/Runtime/ObjectRuntime/` 기준이다.

표식 하나의 경로는 다음 순서로 진행된다.

1. 작업 큐에서 16B `ObjectHandle`을 꺼낸다.
2. `FindSlot`이 chunk 표를 거쳐 Slot을 찾고 세대를 검사한다.
3. `slot->type.Ops().trace`가 객체 본체를 읽는다.
4. 참조 배열 버퍼를 읽으며 간선마다 `TraceVisitor`의 함수 포인터를 호출한다.
5. `MarkEdge`가 `gate_`를 잡고 대상 Slot을 다시 찾아 `mark`를 확인하고 기록한다.

각 단계가 앞 단계 결과의 주소를 필요로 하므로 캐시 미스가 겹치지 않고 직렬로 일어난다.

### 3.2 원인 (영향이 큰 순서)

1. **간선마다 전역 잠금.** `MarkEdge`가 간선 하나마다 `gate_`(Windows SRWLOCK)를 잡는다(`Private/ObjectRegistryGc.cpp` 35행). `StepCollection`은 객체마다 `hasError()`와 큐 pop에서 잠금을 잡고 시계를 읽는다(295–341행). sweep은 Slot 하나마다 잠금을 잡는다(427–447행). 병렬 경로에서는 모든 worker가 이 잠금에서 직렬화된다.
2. **192B Slot에 GC 필드와 비GC 필드가 섞임.** `Slot`(`Private/ObjectRegistryInternal.h` 48–102행)은 세대·상태·object·type·roots·mark·outer와 함께 이름, 부모·자식 고리, 회수 목록 포인터, 각종 계수를 담는다. 크기는 192B(cache line 3개)이고 `mark`는 오프셋 104다. 1M Slot이면 183 MiB를 차지한다. UE는 같은 역할의 `FUObjectItem`을 약 24B의 밀집 배열로 둔다(UOA 41–60행).
3. **간선마다 간접 호출, 객체마다 비인라인 호출.** `TraceVisitor::Visit`는 간선마다 함수 포인터를 호출한다(`Public/Object/RuntimeTypeOps.h` 429–448행). `TypeLease::Ops()`(`Private/TypeLease.cpp` 284행)와 `FindSlot`(`Private/ObjectRegistry.cpp` 100–107행)은 다른 번역 단위에 있다.
4. **prefetch와 배치 없음.** Slot → 본체 → 참조 배열 → 대상 Slot으로 이어지는 미스 사슬을 앞당겨 읽지 않는다.
5. **루트 수집과 sweep의 전체 Slot 순회.** `BeginCollection`은 루트를 찾으려고 모든 Slot을 훑는다(140–150행). 이 순회는 Step 예산 밖이다. `Collect`의 루트 수집과 sweep도 전체 Slot을 훑는다(507–519, 541–549행).
6. **병렬 경로의 구조.** 객체 하나마다 `Function`을 할당·제출하고 64개마다 모두 기다린다(355–413행). `World::StepGarbageCollection`은 scheduler를 넘기지 않으므로 게임 경로는 언제나 직렬이다(`Engine/Source/Runtime/World/Private/World.cpp` 1651행).
7. **객체 수 기준 예산.** 기본 `GcBudget`은 Step당 작업 64개, soft 1ms다(`Public/Object/ObjectRegistryOptions.h` 26–29행). 1M 객체에서는 표식 약 15,600 Step과 sweep 약 16,400 Step이 필요하다. 60fps에서 cycle 하나가 약 9분 걸리고, 그동안 쓰레기가 회수되지 않는다.
8. **회수와 할당자.** `ReclaimEligible`은 객체마다 잠금을 두 번 잡는다(624–647행). 샘플 프로그램의 `SystemAllocator`는 `Deallocate`에서 살아 있는 할당의 연결 목록을 잠금 아래에서 선형 탐색한다(`Core/Source/Runtime/Core/Private/Memory/SystemAllocator.cpp` 76–78행). `BinnedAllocator`도 전역 잠금과 원장 선형 검색을 쓰며 "성능 보장 없음"이라고 명시한다(`BinnedAllocator.h`).

### 3.3 계획과의 차이

[계획](EngineImplementationPlan.ko.md) §6.2는 이미 세 가지를 정했다: epoch bitmap의 test-and-set으로 중복 enqueue 억제, worker의 청크·회색 객체 블록 분담, 큰 참조 배열의 bounded batch(256개 등) 분할. 지금은 객체 하나 단위의 task 분담만 있다. 현재 구현은 정확성 기준 구현(계획 §1.1 "정확한 정지형 GC → … 병렬·점진화")에서 이 세 가지로 넘어가지 못한 상태다. 이 문서는 계획의 방향을 바꾸지 않는다. 계획이 정한 병렬·점진화 단계를 구체화하고 필수로 못박는다.

### 3.4 모델 측정 (2026-11-16, 리포 밖 독립 모델)

리포 코드를 고치지 않고 위 경로를 본뜬 독립 C++ 모델로 쟀다. 모델이 재현한 요소는 다음과 같다.

- Slot 192B와 필드 순서
- 16B handle 큐, 비인라인 `FindSlot`과 `Ops()`
- 간선마다 함수 포인터 호출과 SRWLOCK

측정 환경은 Windows 11, 논리 코어 20개, clang -O2다. 그래프는 무작위 간선 평균 4개, 256개 단위 Outer 묶음, 루트 약 4.9천 개다. 수치는 표식 시간의 중앙값(ms)이며 실행마다 ±20% 안팎으로 차이가 났다.

| 변형 | 1M · 본체 섞임 | 1M · 본체 순차 | 100K · 본체 순차 | 1M · 간선 8 |
|---|---:|---:|---:|---:|
| 현재 `Collect` | 721.5 | 645.9 | 57.3 | 939.6 |
| 현재 `StepCollection` 직렬(예산 무제한) | 772.2 | 747.7 | 61.5 | 1306.9 |
| 간선별 잠금만 제거 | 514.0 | 452.1 | 39.7 | 796.4 |
| Slot 유지 + 잠금 제거 + 배치 visitor + prefetch (GC-S1) | 199.2 | 173.2 | 15.7 | 347.6 |
| SoA + 배치 visitor + 3단 prefetch + bitmap (GC-S3) | 44.7 | 54.9 | 3.10 | 87.4 |
| 병렬 4스레드, 512칸 블록, atomic 표식 (GC-S4) | 22.8 | 21.1 | 1.74 | 35.0 |
| 병렬 16스레드 | 10.6 | 12.0 | 1.33 | 18.4 |

1M에서 현재 구조의 루트 수집과 sweep은 각각 7–10ms였다. 루트 목록과 bitmap을 쓰면 각각 0.05ms와 0.01–0.03ms였다.

512칸 블록 병렬 구조에서 간선마다 전역 잠금을 그대로 두면 1·4·16스레드가 각각 108·186·406ms로, **스레드를 늘릴수록 느려졌다**. 잠금을 atomic 표식으로 바꾸면 같은 실행에서 48.5(단일)·21.7·9.5ms였다. 이 비교는 위 표와 다른 실행이므로 16스레드 값(9.5와 10.6)이 다르다. task를 묶는 것만으로는 병렬 이득이 없고, 표식 동기화를 바꾸는 것이 전제다.

모델에는 한계가 있다. 합성 그래프이고 실제 리플렉션 Trace보다 객체당 일이 적으며, 회수와 장벽 비용은 재지 않았다. 따라서 비율과 순위를 근거로 쓰고, 절대값은 GC-S0의 실제 `ObjectRegistry` 벤치로 다시 잰다.

## 4. 성능 gate (제안)

기준 환경은 8코어 데스크톱 Release 빌드다. 살아 있는 객체 1M, 강한 간선 평균 4–8개, Outer 묶음이 있는 그래프를 쓴다. GC-S0이 이 수치를 확정한다. 확정 뒤에는 회귀 gate로 계속 잰다.

| 항목 | 목표 | 비고 |
|---|---|---|
| STW 병렬 전체 표식 | ≤ 15ms | 모델 16스레드 10.6ms, 8스레드 13.2ms |
| 단일 스레드 전체 표식 | ≤ 60ms | 병렬을 쓸 수 없는 환경의 하한. 모델 45ms |
| Begin(루트 수집) | ≤ 1ms | O(루트 수) |
| sweep 후보 판정 | ≤ 1ms | bitmap. Retire 처리 비용은 white 수에 비례하며 별도로 잰다 |
| 증분 Step 한 번 | ≤ 2ms(p99) | GC-S의 통과 조건(gate)이다. 계획 §6.3의 mark slice 0.5ms·safepoint 1.0ms는 그보다 엄격한 soft 목표로 따로 기록하며 gate가 아니다 |
| 증분 cycle 완료 | 1M에서 60프레임 이내 | 시간 예산이 2ms일 때 |
| 대량 파괴(레벨 언로드 등) | 프레임당 시간 예산 안에서 나눠 처리 | UE 기본 2ms/프레임 |
| 객체당 GC·관리 메모리 | Slot 정보 합계 ≤ 64B | 지금 192B |
| 기본 상한 | `maxSlots` ≥ 2,097,152 | UE `MaxUObjects` 기본 2M(`Private/UObject/UObjectBase.cpp` 1363행) |
| 3M 객체 | 위 수치의 선형 비례 이내 | 대규모 회귀 시나리오 |

## 5. UE 5.8 대조

1M 단위를 감당하는 UE의 구성이다. 굵은 줄번호는 이 문서 작성 때 직접 다시 확인했다. 나머지는 조사 때 읽은 위치다.

| 항목 | UE 5.8 | Rocks에서의 대응 |
|---|---|---|
| 표식 저장 | `FUObjectItem::FlagsAndRefCount` 상위 32비트, chunk(64K) 배열(UOA 41–60, 711) | GC-S3 밀집 배열 |
| 표식 초기화 | Reachable·MaybeUnreachable flag 맞바꾸기, O(1)(FLG 116–123) | epoch 증가로 이미 동등 |
| 표식 동기화 | 일반 read 뒤 `InterlockedAnd`, 이긴 스레드만 enqueue(FLG 99–109) | GC-S1(단일)·GC-S4(병렬) |
| 참조 방문 | 클래스별 schema(멤버 16비트 압축)를 `switch`로 해석, ARO만 함수 포인터(FRC 683–740) | GC-S1 배치 visitor, GC-S7 schema |
| 배치·prefetch | `TReferenceBatcher` 3단 큐(GC 1478–1481, 1758–1837), 객체 반복자 2·6·16칸 앞 prefetch(FRC 85–119) | GC-S1 |
| 병렬 | 최대 16 worker(**`Private/UObject/ReachabilityAnalysisState.h` 27**), 512칸 `FWorkBlock`(**FRC 127–136**), CAS work-stealing(GC 2264–2435) | GC-S4 |
| 루트 | `GRoots`와 dirty 큐로 증분 관리(GC 630–697, 4382–4419) | GC-S3 루트 목록 |
| 영구 객체 | DisregardForGC(UOA 1215–1217). cooked 빌드 전용, 시작 때 열린 창에서 만든 객체(`Launch/Private/LaunchEngineLoop.cpp` 3909–3917) | GC-S7, 13.7절 |
| 클러스터 | 루트 하나 + 멤버, 멤버 내부 간선은 따라가지 않음(GC 4295–4380, 3130–3158). 생성 때의 스냅샷이고 런타임 쓰기 추적은 없다(검증 경고 `GarbageCollectionVerification.cpp` 267, 오류 누적 시 386에서 Fatal). **에셋·액터 클러스터는 기본 꺼짐**(`gc.AssetClustreringEnabled=False`·`gc.ActorClusteringEnabled=False`, `Engine/Config/BaseEngine.ini` 1752–1755) | GC-S7, 13절 |
| 증분 표식 | `gc.AllowIncrementalReachability=0`, 실험 기능, 켜면 5ms(**GC 302–316**). 증분 반복은 work-stealing을 쓰지 않음(FRC 1175) | GC-S5에서 STW 병렬을 정식 경로로 |
| 미도달 수집 | item 배열 전체를 `ParallelFor`로 선형 스캔(GC 5378–5410) | GC-S3 bitmap |
| 파괴 | BeginDestroy·FinishDestroy를 프레임에 나눔, `gc.IncrementalGCTimePerFrame` 2ms(**`Runtime/Engine/Private/UnrealEngine.cpp` 1826**) | GC-S5 |
| 실행 주기 | `gc.TimeBetweenPurgingPendingKillObjects=61.1`(**`Engine/Config/BaseEngine.ini` 1749**), 레벨 전환 시점 | GC-S5 일정 정책 |

UE가 1M을 감당하는 이유는 세 가지가 함께 작동하기 때문이다. 표식 비용을 낮추는 기법, 표식 대상 수를 줄이는 기법, 자주 돌리지 않는 운영 정책이다. 세대별 GC는 쓰지 않는다. 대상 수 줄이기 가운데 기본으로 켜진 것은 DisregardForGC이고, 에셋·액터 클러스터는 기본 설정에서 꺼져 있다(개정 2 정정). 따라서 UE 기본 경로의 1M 처리는 주로 표식 비용·병렬·운영 정책에서 나온다.

## 6. 단계

단계 번호는 의존 순서다. GC-S2는 작아서 GC-S1과 같은 묶음으로 진행할 수 있다. 단계마다 동작 불변 이동 커밋과 성능 커밋을 나눈다. 수천 줄 규모의 이동은 별도 커밋으로 둔다.

### GC-S0 · 측정 기준

- `Programs/GcBench`를 `CompressionBench`와 같은 형태로 둔다. 실제 `ObjectRegistry`와 생성 타입을 쓴다.
- 시나리오는 100K·1M·3M 객체, 간선 분포(평균 4·8, 큰 `ObjectRefArray` 하나), Outer 묶음, 생성·파괴 섞기, 증분 Step 분포(p50·p95·p99·최대), 회수와 대량 파괴, 할당자별 비교다.
- 출력은 phase별 시간(Begin·표식·sweep·회수), 메모리 피크, Step 횟수다.
- 완료 기준: 4절 수치의 기준값을 기록하고 gate 수치를 확정한다.

### GC-S1 · 잠금 제거, 배치 visitor, prefetch

- 단일 marker의 `MarkEdge`에서 `gate_`를 뺀다. 오류 상태는 marker 지역 변수로 둔다. 전제 확인은 7.1절에 따른다.
- 지금의 scheduler 경로(`ObjectRegistryGc.cpp` 355–413행)는 같은 `MarkEdge`를 여러 worker에서 동시에 부른다. 잠금만 빼면 `gcWork_` push와 오류 상태가 경합한다. 그래서 GC-S4 전까지 scheduler 경로는 잠금을 쥐는 별도 edge 함수를 유지한다. 잠금 없는 함수는 직렬 marker 전용이다.
- `mark` 접근은 `Atomic`(relaxed)으로 바꿔 잠금 밖 접근이 데이터 경합이 되지 않게 한다.
- `TraceVisitor::Visit`를 인라인으로 지역 버퍼(예: handle 256개)에 쌓고 가득 차면 flush 콜백을 부르는 형태로 바꾼다. 사용자·생성 Trace 코드는 그대로 둔다. 같은 visitor를 쓰는 `ValidateStagedEdge`·`TallyEdge`는 flush 콜백에서 순서대로 처리해 기존 방문 순서와 집계 결과를 보존한다.
- 작업 큐 기준 16·8·4칸 앞의 Slot, 본체, 참조 배열을 prefetch한다. 거리는 GC-S0으로 조정한다.
- `StepCollection` 루프에서 객체마다 잡던 잠금과 시계 호출을 없앤다. 시계는 N개(예: 256)마다 확인한다.
- 완료 기준: 결과 집합이 oracle(`Collect` 기준 구현)과 같다. GC-S0 1M 표식이 모델 기준 약 3.5배 이상 빨라진다.

### GC-S2 · 시간 기준 예산

- `GcBudget`의 기본을 시간 한도로 바꾼다. `maxObjects`의 기본은 상한 장치 수준으로 크게 둔다. 하위호환은 고려하지 않는다.
- sweep은 Slot 하나가 아니라 bitmap word 하나(GC-S3 뒤) 또는 chunk 하나를 작업 단위로 센다.
- 완료 기준: 1M 증분 cycle이 4절 목표 안에 끝난다(GC-S3 전에는 중간 수치를 기록).

### GC-S3 · GC 필드 분리(SoA), 루트 목록, bitmap sweep, 상한

- GC가 읽는 값만 slot index로 접근하는 밀집 배열로 옮긴다.
  - hot 레코드 16B: object 포인터, 타입 index(trace 함수표), outer slot index
  - 표식: epoch 바이트 또는 mark bitmap
  - 상태 바이트, live bitmap
- 밀집 배열은 chunk처럼 페이지 단위로 늘려 기존 포인터를 옮기지 않는다. 부모·자식 고리(`Slot*`)와 cold 필드는 Slot에 남긴다.
- 작업 큐는 u32 slot index로 바꾼다(1M 기준 16 MiB → 4 MiB). GC 중 세대 검사는 Debug 검증으로만 한다. "강한 간선은 언제나 살아 있다"는 불변식을 Debug에서 확인한다.
- `roots`가 0↔1로 바뀔 때 루트 목록을 갱신한다. 바뀌는 곳은 `AddRoot`(`Private/ObjectRegistry.cpp` 158행 부근), Commit의 root 증가(`Private/ObjectRegistryCreate.cpp` 554행 부근), `detail::ReleaseRoot`(`Private/ObjectRegistry.cpp` 228–238행)이며 모두 `gate_` 아래다.
- 패키지 정책 Registry에서는 `VisitIdentityEdges`가 자식 고리(`firstChild`·`nextSibling`)를 따라간다(`Private/ObjectRegistryGc.cpp` 22–26행). 이 고리를 cold로 남기면 marker가 cold Slot을 계속 읽게 된다. 방법은 둘 중 하나다. hot 쪽에 자식 index 목록을 두거나, cold 접근을 허용하고 그 비용을 GC-S0으로 잰다. 이 단계에서 둘 중 하나로 정한다.
- sweep은 `live & ~mark`를 word 단위로 계산해 white 후보만 `RetireLocked`로 보낸다. 순서는 슬롯 순서(새 chunk부터)를 유지한다.
- `maxSlots` 기본값을 4절 상한으로 올린다. Slot 정보 합계를 4절 목표(≤ 64B)로 줄이는 방안(cold 필드의 이름·고리 정보 분리 포함)을 이 단계에서 정한다.
- 완료 기준: Begin·sweep 후보 판정이 각 1ms 이하이고, 단일 스레드 표식이 4절 목표를 만족한다.

### GC-S4 · 병렬 표식

- worker별 지역 큐와 512칸 블록을 쓰고 블록 단위로 훔친다. 첫 구현은 블록 풀 하나를 잠금으로 보호하는 단순한 방식으로 시작한다(모델이 이 방식으로 10.6ms였다). 그 뒤 CAS 기반 큐로 옮길지는 GC-S0 측정으로 정한다.
- 표식은 atomic 교환(바이트) 또는 `fetch_or`(bit)로 하고 이긴 쪽만 enqueue한다.
- `parallelTraceSafe`가 아닌 수동 Trace 타입은 별도 큐에 모아 소유 스레드에서 처리한다. 생성 타입은 이미 opt-in한다(`Programs/Rocks.MetaCompiler/CppEmitter.cs` 338행).
- 삽입 장벽은 `gcWork_` 직접 push 대신 장벽 전용 큐에 넣고 marker가 다음 slice 시작이나 종결 확인 때 소비한다. UE의 `GReachableObjects`에 해당한다.
- `World`가 `WorkerScheduler`를 넘긴다. 1·2·N worker 모두에서 진행과 종료를 검증한다(계획 §6.4).
- 완료 기준: STW 병렬 전체 표식이 4절 목표를 만족한다. 결과 집합이 oracle과 같다.

### GC-S5 · 수집 일정과 파괴 예산

- STW 병렬 전체 수집을 정식 경로로 둔다. 레벨 전환·로딩 화면, 메모리 압박, 주기 실행(간격은 설정)에서 쓴다. 이 단계를 마치면 [계획](EngineImplementationPlan.ko.md) §6.1의 "점진 marking을 기본 구현으로 채택"은 선택 경로로 바뀐다. 계획 6.6절이 이 변경을 예고한다.
- 증분 표식은 선택지로 유지한다. 증분 slice는 직렬 또는 소수 worker로 시작한다. 증분 반복의 work-stealing은 UE도 지원하지 않으므로 필요성을 측정한 뒤 정한다.
- 회수·파괴(BeginDestroy 대응 hook, 소멸자, 해제)는 시간 예산으로 여러 프레임에 나눈다. 회수는 N개씩 한 번의 잠금으로 떼어 내고 소멸자와 해제는 잠금 밖에서 묶어 처리한다.
- 완료 기준: 대량 파괴 시나리오에서 프레임당 예산을 지킨다. World 수명 정책과 시험을 새 일정에 맞춘다.

### GC-S6 · 할당자

- 필요한 성질은 세 가지다.
  - 원장 탐색 대신 주소 정렬 마스크로 페이지 헤더를 찾는 O(1) 소유권 판정
  - 스레드별 캐시와, 다른 스레드가 해제한 블록을 받는 atomic 목록
  - 같은 크기 객체를 모으는 크기별 slab
- 위치는 Core Memory 공용 모듈이다. ObjectRuntime 전용 할당자는 두지 않는다. 계획 C28의 slab/thread cache를 이 단계의 필수 항목으로 끌어올린다.
- 먼저 기존 `ExternalAllocator`와 jemalloc·TCMalloc 바인딩(`VendorAllocatorBackend.h`)으로 할당자 몫을 GC-S0에서 분리해 잰다.
- 완료 기준: 1M 생성·회수 시간이 GC-S0 기준에서 할당자 원인 비중을 제거한 수준이 된다. 수치는 GC-S0이 정한다.

### GC-S7 · 표식 대상 줄이기와 큰 Trace 분할

- **영구 영역.** 시작 때 만들어 끝까지 사는 객체를 표식 대상에서 뺀다(UE DisregardForGC에 해당). 영구 객체는 영구 객체만 가리킬 수 있다(결정 4 확정). 상세는 13.7절이다.
- **클러스터.** 함께 살고 함께 죽는 객체 묶음을 노드 하나로 표식한다. 패키지 정책 Registry의 패키지([ObjectFoundations](ObjectFoundations.ko.md) 1.7절)와 World의 Level이 클러스터다. 엔진이 생성 Commit 때 자동으로 가입시킨다. 사용자가 묶는 장치로 두지 않는다. 상세는 13절이다.
- **schema 기반 Trace.** MetaCompiler가 참조 멤버의 offset·종류 표를 생성하고 marker가 그 표를 해석한다. 큰 `ObjectRefArray`·`ObjectRefMap`은 블록 단위 작업으로 쪼갠다(계획 §6.2 2번의 "큰 참조 배열 bounded batch 분할"). 수동 Trace는 기존 함수 경로로 남긴다.
- 완료 기준: 큰 배열 하나(예: 1M 참조)가 Step 예산을 넘기지 않는다. 영구·클러스터 객체가 표식 비용에서 빠진다.

### 메타 컴파일러로 얻는 추가 여지 (제안)

UE는 리플렉션으로 만든 schema를 실행 중에 `switch`로 해석한다(FRC 683–740). Rocks에는 MetaCompiler가 있으므로 같은 정보로 **빌드 때 타입별 특수화 코드와 상수를 생성**할 수 있다. 현재 생성 Trace는 `static void Trace(const T&, TraceVisitor&)` 고정 시그니처라 이 여지를 쓰지 않는다(`Programs/Rocks.MetaCompiler/CppEmitter.cs` 255–262·339행). 아래 항목은 모두 **제안**이며 GC-S0으로 효과를 잰 뒤 채택한다. 붙는 단계를 괄호에 적었다.

| 번호 | 내용 | 기대 효과 |
|---|---|---|
| M1 (GC-S1) | **marker 전용 정적 특수화.** 생성 Trace를 visitor 타입에 대한 템플릿으로 내고, `RuntimeTypeOps`에 GC marker 전용 진입점을 따로 둔다. marker는 객체당 간접 호출 1회만 하고 간선당 간접 호출은 0회다. 필드 수가 고정이면 컴파일러가 펼친다. 관찰자 집계·Commit 검증은 기존 `TraceVisitor` 경로를 그대로 쓴다. 수동 Trace 타입은 배치 visitor 경로로 간다 | 간선당 호출 비용 제거. 공개 `TraceVisitor` 계약은 바꾸지 않는다 |
| M2 (GC-S3) | **잎 타입 표시.** 생성기는 강한 참조가 없는 타입을 이미 안다(빈 Trace를 낸다, 255행). 이를 타입 정보의 상수로 내보낸다. marker는 잎 객체를 표식만 하고 큐에 넣지 않으며, outer 간선만 hot 레코드에서 바로 따라간다 | 잎 객체의 큐 왕복·Slot 조회·trace 호출 제거. 효과는 잎 객체 비율에 비례 |
| M3 (GC-S1·S3) | **참조 위치 상수.** 생성기가 강한 참조 필드가 놓인 byte 범위와 컨테이너 버퍼 포인터의 offset을 상수로 낸다. prefetch가 객체 전체가 아니라 필요한 cache line과 컨테이너 버퍼를 정확히 가져온다 | 미스 사슬 단축. 큰 객체일수록 크다 |
| M4 (GC-S7) | **정적 schema.** offset·종류 표를 constexpr로 생성한다. M1의 특수화 코드와 같은 원천에서 만든다. 큰 `ObjectRefArray`·`ObjectRefMap`의 블록 분할 지점과 Step 예산 안에서 Trace를 끊었다 이어 가는 데 쓴다 | 거대 Trace 분할의 기반 |
| M5 (GC-S4) | **병렬 안전의 정적 판정.** 생성 타입은 이미 `parallelTraceSafe`를 opt-in한다(338행). 수동 Trace만 소유 스레드 큐로 보낸다 | 이미 있는 정보의 활용 |
| M6 (GC-S7) | **소유 관계에서 자동 클러스터.** "배타적으로 소유한 하위 객체"를 리플렉션 메타데이터로 표시하면 클러스터 경계를 빌드·로드 때 정할 수 있다. 지금 리플렉션에는 이 표시가 없으므로 새 메타데이터가 필요하다 | **개정 2에서 불필요.** 클러스터 경계를 패키지 소속과 World 소유 사슬이 이미 정하므로 새 메타데이터가 없어도 엔진이 클러스터를 만든다(13.3절) |
| M7 (GC-S7) | **정적 대상 타입에 따른 장벽·표식 생략.** 필드의 대상 타입이 영구 영역 전용 타입으로 정적으로 정해지면 장벽 shade와 표식 enqueue를 생략하는 경로를 생성한다 | Registry 단위 분리가 이미 있으므로 효과가 제한적일 수 있다. 측정 뒤 결정 |

한계는 다음과 같다.

- 손으로 쓴 `Trace`(`NativeClass`)는 이 혜택을 받지 못하고 배치 visitor 경로에 남는다.
- 컨테이너 길이는 실행 시에만 안다.
- 템플릿 인스턴스화로 코드 크기가 늘어난다.
- 생성 Trace의 시그니처가 바뀌므로 MetaCompiler 출력과 `MetaTests` 기대값을 함께 고친다. 하위호환은 고려하지 않는다.
- 3.4절 모델의 B1(인라인 배치 visitor)이 M1의 효과를 이미 상당 부분 근사한다. 그래서 M1의 추가 이득은 크지 않을 수 있다. 추가 이득은 M2·M3 쪽이 클 것으로 본다(해석, 측정 전).

## 7. 정확성과 불변식

### 7.1 잠금 제거의 전제

GC slice는 `processing_` 아래에서 실행되며, 그동안 생성·간선 변경·Discard는 `Busy`다. 이것이 marker가 `gate_` 없이 그래프와 Slot 배치를 읽어도 된다는 근거다. GC-S1 착수 때 아래를 모든 진입점에서 감사한다.

- **slice와 겹칠 수 있는 cross-thread 경로.** 다음 함수들은 접근 문맥 검사와 `processing_` 검사 없이 어느 스레드에서나 `gate_`를 잡는다. 이 경로들은 `mark`·`state`를 쓰지 않는다(`WakeDeferredLocked`는 `state`를 읽고 회수 목록 고리만 바꾼다 — `Private/ObjectRegistryGc.cpp` 173–178·264–269행). GC-S1에서 이를 시험으로 고정한다.
  - `ReleaseBackend`·`ReleaseEncodePin`(`Private/ObjectRegistry.cpp` 297–320행)
  - `detail::ReleaseRoot`(228–238행). `roots`와 통계를 바꾸므로 GC-S3 루트 목록과 직접 관련된다. slice 중 루트 해제를 루트 목록에 어떻게 반영할지(즉시 반영 또는 다음 Begin까지 지연)를 GC-S3에서 정한다.
  - `ReleaseBorrow`(285–295행)는 slice 시작 때 `totalBorrows_ == 0`을 요구하므로(`Private/ObjectRegistryGc.cpp` 300행) 실제로는 slice와 겹치지 않는다.
- **marker 밖에서 `mark`를 읽고 쓰는 곳.**
  - `ShadeLocked`(`Private/ObjectRegistryGc.cpp` 65–92행)
  - Commit의 shade(`Private/ObjectRegistryCreate.cpp` 548행 부근)
  - Begin·`Collect`의 epoch wrap 초기화
  - `IsCutoff`(59–63행)와 그것을 거치는 `CheckLive`. 이들은 `gcPhase_ == Sweeping`일 때만 `mark`를 읽는다.
  
  Sweeping 전환이 모든 marker 종료 뒤 `gate_` 아래에서 일어나는 한, 이 읽기는 marker의 쓰기와 겹치지 않는다. 장벽 shade와 marker는 둘 다 slice 밖과 안으로 나뉜다. 그래도 `mark`는 `Atomic`으로 두어 잠금 밖 접근을 정의된 동작으로 만든다. `CaptureDiagnostics`는 `mark`를 읽지 않는다.
- allocator 콜백 재진입은 `Busy`로 막혀 있다. 소멸자 콜백은 회수 단계에서만 실행되며 표식과 겹치지 않음을 확인한다.

### 7.2 결정성

- 표식 결과 집합은 순서와 무관하게 같다. 병렬 표식의 시험은 sleep이나 경쟁 운에 기대지 않고, 결과 집합·계수와 oracle 대조로 단언한다.
- Retire 순서는 sweep이 슬롯 순서(새 chunk부터)를 지켜 유지한다. 장벽 shading 순서나 `gcWork_` 순서에 기대는 기존 시험은 결과 집합 단언으로 바꾼다.
- 관찰자 집계(`TallyEdge`)는 배치 visitor의 flush가 방문 순서를 보존하므로 기존과 같은 결과를 낸다. GC-S1에서 시험으로 고정한다.

### 7.3 oracle

계획 §6.3에 따라 정지형 직렬 `Collect`를 oracle로 유지한다. 고정 그래프와 무작위 변경에서 live set 누락 0건, 보수적 잔존 객체가 무변경 full cycle 두 번 뒤 해제되는지를 매 단계 확인한다.

## 8. 현실적으로 어려운 부분

| 항목 | 내용 | 대응 |
|---|---|---|
| 잠금 제거의 전제 증명 | `gate_`가 표식 외의 cross-thread 경로도 보호한다. 감사 범위가 실제 비용이다 | 7.1절. GC-S1 첫 커밋을 감사와 Debug 검증으로 둔다 |
| 증분 장벽과 병렬 표식의 공존 | 장벽이 `gate_` 아래에서 `gcWork_`에 넣는다. UE도 증분 반복의 work-stealing을 지원하지 않는다 | 장벽 전용 큐(GC-S4). 증분은 직렬로 시작(GC-S5) |
| 중단할 수 없는 거대 Trace | 사용자 Trace 함수 하나가 1M 간선을 방문하면 쪼갤 수 없다 | schema 기반 Trace와 컨테이너 블록 분할(GC-S7). Trace 계약 변경이 따른다 |
| SoA 전환의 변경 면적 | Slot 필드 접근이 `ObjectRegistry*.cpp` 열 개 남짓에 퍼져 있다 | hot 필드만 분리하고 이동 커밋을 먼저 둔다 |
| 하드웨어 한계 | 표식은 메모리 지연에 묶인 작업이라 모델에서 8스레드 이후 확장이 둔화됐다. 3.4절 표의 실행에서 13.2 → 10.6ms, 잠금 비교 실행에서 13.4 → 9.5ms였다. 코어가 적은 기기에서는 병렬 효과가 작다 | 대상 수 줄이기(GC-S7)와 일정 정책(GC-S5)의 비중을 높인다 |
| handle 모델의 비용 | 간선이 16B handle이라 참조 배열이 포인터의 두 배다. 대신 본체를 읽지 않고 slot index를 얻는다 | GC 중 세대 검사 생략(GC-S3)과 Debug 불변식 검증 |
| 할당자 교체 | Core 공용 모듈 작업이라 다른 세션과 겹친다 | GC-S6을 Core Memory 작업으로 조율한다 |

## 9. 시험과 벤치

- 기존 GC 시험(`Tests/GcTests.cpp`, `IncrementalGcTests.cpp`, `IncrementalGcReviewTests.cpp`, `ObjectIdentityGcTests.cpp`)은 단계마다 전부 통과해야 한다. 순서 의존 단언은 7.2절 기준으로 바꾼다.
- 단계별 추가 시험은 다음과 같다.
  - oracle 대조(무작위 그래프 100K, seed 고정)
  - 잠금 밖 표식 중 cross-thread release 경합(명시 gate로 순서를 만들어 결정적으로 단언)
  - 1·2·N worker 종료
  - 장벽 큐 overflow
  - 큰 배열 분할
  - 영구·클러스터 경계
- 벤치는 GC-S0 시나리오를 단계마다 다시 돌려 4절 gate와 대조하고 결과를 구현 문서에 기록한다.
- Asan과 전체 체인은 단계 묶음 끝에 한 번 돌린다.

## 10. 범위 밖

- 세대별 GC. remembered set과 쓰기 장벽 비용이 따르고 UE도 쓰지 않는다. GC-S 완료 뒤 측정으로 필요성을 다시 본다.
- 완전 concurrent marker(mutator와 동시 표식). 계획 §6.1대로 P3 연구로 둔다.
- 이동·압축 GC. 계획의 주소 비이동 원칙을 유지한다.

## 11. 열린 결정

| 번호 | 항목 | 선택지 |
|---|---|---|
| 1 | 완료 시한 | 화면 마일스톤 뒤 대규모 콘텐츠 작업 전 / 특정 gate 전 / 사용자가 따로 정함. **결정 필요** |
| 2 | 표식 단위 | epoch 바이트(1M → 1 MiB, 단순) 또는 bit(128 KiB, `fetch_or`). GC-S0 측정으로 정한다 |
| 3 | STW 전체 수집 주기 | 시간 간격·메모리 임계·레벨 전환 조합. 기본값은 GC-S5에서 정한다 |
| 4 | 영구 객체가 일반 객체를 가리킬 때의 규칙 | **확정(2026-11-17): 금지.** 위반 쓰기·Commit은 `Unsupported`다. 일반 객체를 붙잡아야 하면 RootRef를 쓴다(13.7절) |
| 5 | Slot 정보 64B 목표의 달성 방법 | cold 필드 분리·이름 정보 외부화 등. GC-S3에서 정한다 |
| 6 | 배타 소유 하위 객체의 표시 방식(M6) | **해소(개정 2).** 패키지·Level 소속으로 경계를 정하므로 표시가 필요 없다(13.3절) |
| 7 | slice 중 루트 해제의 루트 목록 반영 시점 | 즉시 반영 / 다음 Begin까지 지연. GC-S3에서 정한다 |

## 12. 다른 문서 정정

- [EngineImplementationPlan](EngineImplementationPlan.ko.md) §6에 6.6절 "대규모 처리 필수 보강"을 두고 이 문서를 가리킨다. 1.1절 F1 행·1.1절의 C28 문장·4.1절의 "C28 이후 측정 기반 최적화안" 문장에 GC-S6 필수를 덧붙이고, §6.1에 6.6절 참조를 단다. 개정 1.10(33.15절)으로 이번에 반영한다.
- [Core·Object 입력 상세 계획](CoreObjectInputDetailedPlan.ko.md)의 C28은 "선택 R3"으로 남아 있다. 이 표는 `CoreObjectInputTasks.ko.csv`·`CoreObjectInputData.json`과 짝이므로 이번에는 고치지 않는다. GC-S6 착수 때 세 곳을 함께 필수로 바꾼다.
- [GC 안내](../Implementation/GarbageCollectionGuide.ko.md) 7절과 8절은 GC-S 단계를 마칠 때마다 현재 동작과 수치로 고친다. 7.2절의 객체 수 줄이기 권고는 GC-S 완료 뒤 사용자 선택 최적화로 낮춘다.
- [증분 GC 구현 계약](../Implementation/IncrementalGarbageCollection.ko.md)은 GC-S1(잠금), GC-S2(예산), GC-S4(장벽 큐) 때 계약을 고친다.

## 13. 클러스터와 영구 영역 (GC-S7 상세, 개정 2)

상태: **설계**. 실험 브랜치(experiment/gc-scale) Phase 4에서 구현·측정한다([실험 결과](../Implementation/GcScaleExperiment.ko.md) 12절).

### 13.1 출발점과 사실 정정

- Rocks의 타입 정보는 Registry 객체가 아니다. `TypeRegistry`가 따로 소유하고 GC 대상이 아니다. 그래서 UE의 DisregardForGC에 들어가는 네이티브 클래스·CDO에 해당하는 몫이 Rocks에는 원래 없다. 지금 영구 영역에 들어갈 객체는 Engine Registry를 초기화할 때 만드는 객체(서브시스템)와 앞으로 생길 시작 콘텐츠다. 효과는 그 수에 비례하며 측정으로 기록한다.
- UE 5.8의 에셋·액터 클러스터는 기본 설정에서 꺼져 있고 cooked 빌드에서만 만들어진다(5절 표). UE 클러스터는 생성 때의 스냅샷이다. 실행 중 멤버가 클러스터 밖의 새 객체를 가리키게 되어도 장부를 고치지 않고, 검증 모드에서만 경고한다. 멤버나 참조 대상이 Garbage가 되면 클러스터를 해체한다.
- Rocks는 UE와 조건이 다르다. 세 가지를 쓸 수 있다.
  - 모든 강한 간선 쓰기가 Registry API(`SetEdge`·컨테이너 편집·Commit)를 지난다. 관찰자([ObjectFoundations](ObjectFoundations.ko.md) 3절)가 이미 이 지점에서 패키지 사이 간선을 센다.
  - 패키지 정책 Registry에서는 소속 객체가 Discard로만 죽고, 패키지가 살아 있으면 소속 객체가 모두 산다(1.7절).
  - World에서는 Level → Actor → Component 소유 사슬이 World API로만 바뀐다(`World.h` 소유·수명 규약).
- 그래서 Rocks 클러스터는 UE를 옮기지 않는다. 간선 쓰기마다 장부를 정확히 고쳐 런타임 변경에도 깨지지 않고, 해체 없이 개별 이탈을 처리하는 형태로 설계한다.

### 13.2 의미

- 클러스터는 루트 객체 하나와 소속 객체 집합이다. 루트도 소속이다.
- 소속은 **루트 → 소속 객체의 암묵 강한 간선**으로 정의한다. 패키지 고리 간선(1.7절)과 같은 뜻이다. 정지형 oracle `Collect`도 이 간선을 따라간다. 그러므로 클러스터 표식의 결과 집합은 oracle과 정확히 같아야 한다(보수적 잔존을 허용하지 않는다).
- 표식은 소속 객체 사이의 간선을 따라가지 않는다. 루트가 살면 소속 전체가 살기 때문이다. 클러스터 하나의 표식 비용은 O(1 + 경계 소속 객체의 간선 수 + 이탈 대상 수)다.
- 가입은 생성 Commit 때만 한다. 이탈은 되돌릴 수 없다(재가입 없음). 해체는 루트의 RequestDestroy·Discard와 Registry 종료에서만 일어난다.

### 13.3 누가 클러스터를 만드나

엔진이 자동으로 만든다. 사용자가 객체를 묶는 공개 장치는 두지 않는다.

| Registry | 클러스터 | 루트 | 소속 | 가입 | 이탈 |
|---|---|---|---|---|---|
| 패키지 정책(에셋) | 패키지 하나 | 패키지 객체 | outermost가 그 패키지인 객체 | 생성 Commit(자동) | Discard(부분 트리) |
| World | Level 하나 | Level | World API(`SpawnActor`·`AddComponent`)로 만든 Actor·Component | 그 생성 Commit(World가 지정) | `DestroyActor`·`DestroyComponent`의 RequestDestroy |
| 영구 영역을 연 Registry | 영구 영역(특수 클러스터) | 없음(언제나 생존) | 영구 창이 열린 동안 Commit한 객체 | Commit | 없음(Shutdown만) |

- 소속 조건은 `RuntimeTypeOps::completeFieldLayout`인 타입이다. 모든 강한 간선이 metadata에 있고 Registry API로 쓰인다는 선언이다. 이 조건을 못 맞추는 객체(수동 Trace, 리플렉션 base를 둔 native 파생)는 클러스터 밖 일반 객체로 남는다. 정확성은 그대로이고 최적화만 받지 못한다. 패키지 정책 Registry는 이미 이 조건을 강제한다.
- World는 Level을 만드는 트랜잭션에서 그 Level을 클러스터 루트로 지정하고, Spawn·AddComponent 트랜잭션에서 가입할 클러스터를 지정한다. 이 API는 World·AssetManager 같은 엔진 계층용이다(Discard와 같은 위치).
- M6(배타 소유 표시 메타데이터)과 열린 결정 6은 필요 없어졌다.

### 13.4 장부

클러스터 기능을 켠 Registry만 객체별 칸을 slot index 밀집 배열로 둔다. chunk와 함께 할당하므로 잠금 안에서 할당하지 않는다.

| 칸 | 크기 | 뜻 |
|---|---|---|
| `clusterState` | u32 | 소속 클러스터, 또는 "클러스터 C에서 이탈한 대상(detached)", 또는 없음 |
| `inCount` | u32 | 소속이면 같은 클러스터 소속 객체에서 오는 간선 수. detached면 C 소속 객체에서 오는 간선 수 |
| `outCount` | u32 | 소속 객체의 간선 가운데 대상이 같은 클러스터의 소속도 detached도 아닌 간선 수 |
| 목록 고리 | u32 ×2 | 소속 목록 또는 detached 목록 |
| 경계 고리 | u32 ×2 | `outCount > 0`인 소속 객체 목록 |

객체당 28B다. 4절의 Slot 64B 목표와는 따로 계산하고 측정 뒤 줄일 방안을 정한다. 클러스터 레코드는 루트 slot, 소속 수, 경계 목록 머리, detached 목록 머리, 표식 epoch를 갖는다.

**간선 분류.** 소유자 X가 클러스터 C의 소속이고 대상이 t일 때 다음과 같이 센다. 소유자가 클러스터 밖이면 장부가 없다.

- t가 C의 소속이면 `inCount[t]`
- t가 C에서 이탈한 detached면 `inCount[t]`
- 그 밖이면 `outCount[X]`. 0 → 1이면 X를 경계 목록에 넣고, 1 → 0이면 뺀다.

outer 간선(생성 때 고정)도 같은 규칙으로 센다. 패키지 고리 간선과 소속 간선 자체는 세지 않는다.

**갱신 지점.** 관찰자의 기록 지점과 같다. Commit(가입과 staged 간선 일괄), `SetEdge`·`ObjectRefArray`·`ObjectRefMap`·`StructRefArray` 편집(제거와 추가 각각), Discard·RequestDestroy(이탈), 루트의 Discard·RequestDestroy(해체), Shutdown이다.

- **이탈**(객체 B, 클러스터 C): 먼저 B의 Trace와 outer로 B가 C 장부에 남긴 몫을 뺀다(대상이 C 소속이거나 C의 detached면 `inCount[t]` 감소, 0이 된 detached는 목록에서 뺌, 그 밖이면 `outCount[B]`를 비우고 경계에서 뺌). 그다음 `inCount[B] > 0`이면 B를 C의 detached로 옮기고, 아니면 클러스터 없음으로 둔다. 비용은 O(B의 간선)이다. 부분 트리 Discard는 객체마다 차례로 이탈한다.
- **가입**: 새 객체만 가입한다. 공개 전 객체는 같은 트랜잭션의 staged 간선만 받으므로(Alive 소유자의 편집은 Reserved 대상을 거부한다 — `CheckTargetLocked`), 가입 때문에 분류가 바뀌는 기존 간선이 없다. 트랜잭션의 새 객체 소속을 모두 정한 뒤 staged 간선을 센다.
- **해체**(C): 소속과 detached의 `clusterState`를 모두 비운다. 비용은 O(소속 + detached)다.
- **분류가 어긋나지 않는 근거**: 대상의 상태 전이는 네 가지뿐이다. 생성 → 소속, 소속 → detached(계수를 그대로 옮김), detached → 없음(계수가 0일 때만), 소속 → 없음(해체, 그 클러스터 장부는 버림). 어느 전이도 다른 클러스터 장부의 분류를 바꾸지 않는다. 다른 클러스터 D의 장부에서 C의 소속은 언제나 "그 밖"이기 때문이다.

### 13.5 표식과 sweep

- 객체 x에 닿으면: x가 클러스터 C의 소속이면 C를 atomic으로 표식하고 이긴 쪽이 C를 큐에 넣는다. detached이거나 클러스터 밖이면 지금처럼 개별 표식한다.
- 클러스터 C 처리: 경계 목록의 소속 객체마다 Trace와 outer를 돌리되 C 소속 대상은 `clusterState` 비교로 건너뛴다. detached 목록의 객체는 개별 표식한다.
- 루트 목록에 소속 객체가 있으면 그 클러스터를 표식한다. 영구 영역은 Begin에서 표식된 상태로 시작하고 큐에 넣지 않는다(경계·detached가 없다).
- sweep 후보는 `live & ~mark & ~clustered`다(`clustered`는 소속 bitmap). 표식되지 않은 클러스터는 sweep 시작 전에 해체해 소속을 white 후보로 내보낸다. 패키지 RootRef를 Discard 없이 놓은 버그 경로이며 `retiredWithoutDiscard`는 지금처럼 센다.
- cutoff 판정(`IsCutoff`·`CheckLive`·`ValidateStagedEdge`)은 소속 객체를 클러스터 표식으로 판정한다.
- 병렬: 클러스터 하나는 worker 하나가 처리한다. 경계가 매우 큰 클러스터의 블록 분할은 측정 뒤 정한다.

### 13.6 증분 장벽

- 삽입 장벽(`ShadeLocked`)은 대상이 소속이면 그 클러스터를 shade한다.
- Marking 중 C가 이미 표식된 뒤 경계가 늘거나 detached 계수가 늘면, 새 간선의 대상은 삽입 장벽이 이미 shade했으므로 추가 조치가 없다.
- Marking 중 이탈: C가 표식됐으면 B를 shade한다(보수적, 다음 cycle에 회수).
- Marking 중 해체: C가 표식됐으면 소속을 모두 black으로 찍고 큐에는 넣지 않는다. 밖으로 나가는 간선은 C를 처리할 때 이미 따라갔고, 안쪽 간선의 대상은 모두 같이 black이 된다.
- Sweeping 중 이탈·해체: 소속의 mark를 그 클러스터의 표식 상태로 맞춘다.

### 13.7 영구 영역

- 영구 영역은 특수 클러스터다. 언제나 표식된 상태이고 해체는 Shutdown 때만 한다.
- 들어가는 법: `RegistryOptions`의 영구 창 칸을 켠 Registry는 `ClosePermanentRegion(access)` 전까지 Commit되는 객체가 모두 영구다. 닫은 뒤에는 다시 열 수 없다. Engine은 Engine Registry 초기화(서브시스템 생성) 뒤 닫는다. World·에셋 Registry는 열지 않는다.
- 규칙(결정 4 확정, 2026-11-17): 영구 객체의 강한 간선 대상(outer 포함)은 영구 객체뿐이다. 위반하는 쓰기·Commit은 Release에서도 `Unsupported`로 거부한다(비교 하나). 일반 객체를 붙잡아야 하면 RootRef를 쥔다. 영구 객체의 RequestDestroy·Discard도 `Unsupported`다.
- 표식과 sweep에서 비용이 0이다. 영구 객체를 가리키는 간선은 언제나 표식된 클러스터에 닿아 끝난다.

### 13.8 검증

- Debug 장부 검증(옵션, 비용 큼): Begin마다 모든 클러스터 장부를 처음부터 다시 계산해 비교한다. soak에서 켠다.
- oracle(`Collect`, 소속 간선 포함)과 결과 집합이 같아야 한다. 무작위 그래프와 soak 모두에서 확인한다.
- 음성 대조: 이탈 때 `inCount` 이동을 빼거나, 이탈 shade를 빼거나, `outCount` 갱신 하나를 빼면 검출되어야 한다.
- soak 시나리오: 패키지 생성·부분 Discard·전체 Discard, 패키지 사이 간선 편집, Level·Actor·Component 생성·파괴, 같은 Level 안 Actor 사이 참조, 증분 cycle 도중의 모든 변경.

### 13.9 비용과 한계

- 간선 쓰기마다 `gate_` 아래에서 밀집 배열 갱신 몇 개가 늘어난다(무할당). 벤치로 잰다.
- 클러스터 기능을 켠 Registry에서 객체당 28B가 늘어난다.
- 경계 소속 비율이 높으면 효과가 줄어든다. 경계 비율별 벤치로 잰다.
- 소속 조건을 못 맞추는 타입은 혜택이 없다. World의 게임 Actor가 native 파생이면 클러스터 밖이다.
- 큰 클러스터 하나의 경계 처리가 병렬 작업 하나에 묶인다.

### 13.10 UE 5.8과의 차이

| 항목 | UE 5.8 | Rocks |
|---|---|---|
| 기본 사용 | 에셋·액터 클러스터 꺼짐, cooked 전용 | 패키지 정책 Registry와 World에서 항상 |
| 경계 | 생성 때의 스냅샷과 MutableObjects. 런타임 변경은 검증 경고뿐 | 모든 강한 간선 쓰기에서 정확히 갱신 |
| 멤버 이탈 | 루트나 참조 대상이 Garbage가 되면 해체 | 개별 이탈 O(그 객체의 간선), 해체 불필요 |
| 생성 시점 | 로딩 PostLoad 뒤, Level Finalize | 생성 Commit |
| 영구 객체 → 일반 객체 | 따라가지 않고 검증 경고 | 쓰기 때 거부 |
