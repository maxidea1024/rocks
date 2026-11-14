# 점진·병렬 GC의 구현 계약

이 단계는 기존 비이동 객체 저장소에 점진 수집과 병렬 표식을 추가한다. 객체 필드를 변경하는 mutator는 Registry 생성 스레드 하나이며, worker 표식은 그 스레드가 GC slice를 실행하는 동안에만 수행한다. 반환 전에 모든 worker를 합류시키므로 필드 변경과 표식의 동시 실행을 허용하는 concurrent GC는 아니다. 기존 `Collect`는 별도의 full STW 구현으로 유지하여 기준 결과를 대조한다.

## API와 단계

`BeginCollection(access)`는 수집 큐를 예약하고 epoch를 갱신한 뒤 루트를 표식한다. `StepCollection(access, GcBudget, WorkerScheduler*)`는 작업을 진행하고 `GcProgress`를 반환한다. `CollectionPhase`로 현재 단계를 조회하며, `CancelCollection`은 아직 회수 대상을 확정하지 않은 Marking 단계만 취소한다.

| 단계 | 동작과 변경 허용 범위 |
|---|---|
| Idle | 수집 중이 아니다. full Collect 또는 새 cycle을 개시할 수 있다. |
| Marking | 루트와 강한 간선을 추적한다. slice 사이의 신규 루트·간선·객체 공개에 삽입 장벽을 적용한다. |
| Sweeping | 같은 gate 안에서 도달성 마감을 확정한 뒤 흰 객체를 순차적으로 Retired로 분류한다. 마감 당시 흰 객체에 대한 새 접근·pin·간선·backend hold는 이미 거부한다. |
| Reclaiming | 보유 계수가 해소된 Retired 저장소를 예산에 맞추어 실제 파괴한다. backend 보유분은 남을 수 있으며 나중에 DrainRetired로 회수한다. |

Begin/Step/full Collect는 소유 스레드, 올바른 context, Borrow 0개, 활성 ConstructionContext 0개를 요구한다. slice 안의 사용자 callback 재진입 변경은 거부한다. slice 사이에는 생성 트랜잭션과 일반 객체 접근을 다시 사용할 수 있다. 생성 트랜잭션을 열린 채 다음 slice를 실행하면 Busy다.

Sweeping 이후 취소는 Busy다. 일부 흰 객체가 이미 Retired인 상태에서 나머지 흰 객체를 살아 있는 객체로 되돌리면 이미 회수한 저장소로 연결된 간선을 부활시킬 수 있기 때문이다. Shutdown은 신규 유입을 닫고 활성 cycle을 완료한 뒤 기준 full 수집을 실행한다. 남은 RootRef/Borrow/BackendLease를 무시하여 강제 해제하지 않는다.

## 표식 불변식과 수명

RootRef 생성·복제, SetReference, Registry 컨테이너 쓰기(ObjectRefArray의 AddReference·InsertReferenceAt·SetReferenceAt, StructRefArray 원소의 SetElementReference)는 Marking 동안 non-null 대상의 shade와 참조 게시를 같은 gate 안에서 수행한다(null 원소는 간선이 아니라 장벽이 없다). live 컨테이너 변경은 owner Borrow를 쥔 채 하므로 잠금 밖 버퍼 할당·원소 생성 중에도 수집·회수가 끼어들지 않고, 다시 잠근 뒤 그때의 size·index로 다시 판단한다(그 사이 콜백이 같은 컨테이너를 바꿨으면 Busy 또는 기존 버퍼 사용). 생성 트랜잭션의 `AssignReferences`는 생존 대상만 즉시 shade하고 같은 트랜잭션의 Reserved 대상은 Commit이 공개하며 shade한다. 검은 객체에서 아직 흰 대상에 연결한 새 간선을 놓치지 않는다. 삭제 장벽은 없으므로 cycle 중 끊어진 객체 일부는 이번 cycle에 보존되고 다음 cycle에서 수거되는 floating garbage가 될 수 있다.

Marking 중 공개하는 newborn은 모든 staged 객체를 grey로 넣어 자체 Trace가 실행되도록 한다. 검게만 표시하고 스캔을 생략하지 않는다. 생성 전에 전체 슬롯 수에 맞는 큐 용량을 확보하여 잠금 안의 shade가 할당하지 않도록 한다. 현재 cycle의 Marking 중에는 객체를 회수·슬롯 재사용하지 않는다.

Marking 큐가 비었음을 확인하고 Sweeping으로의 전환은 gate 안에서 원자적으로 수행한다. 아직 slot cursor가 방문하지 않은 흰 객체도 cutoff 검사로 접근을 거부한다. Sweeping 중 태어난 객체는 현재 epoch로 표식하고 cutoff 대상에 연결하지 못하게 하므로 이번 cycle에 보존된다.

inner → outer는 언제나 강한 간선이다([객체·그래프 보강](../Design/ObjectFoundations.ko.md) 1.5절). 타입 Trace가 아니라 Registry가 객체를 trace하는 모든 곳(full Collect, Step의 순차·병렬 trace, Commit의 staged 검증)에서 Trace 뒤에 그 객체의 Outer를 방문한다. Outer는 공개 전에 정해져 바뀌지 않으므로 worker가 `gate_` 없이 읽어도 경합이 없다. Marking 중 공개한 newborn의 trace가 Outer를 표식하고, Sweeping 중에는 cutoff white Outer 아래 생성이 StaleHandle이며 Commit 검증이 Outer도 대상처럼 검사한다. 정책 없는 Registry에서 outer는 inner를 살리지 않는다. Retire(sweep·rollback)는 같은 잠금 안에서 그 객체를 이름 색인과 부모의 자식 고리에서 뗀다. 고리는 해체하지 않는다: white 부모의 자식은 모두 white라 같은 sweep에서 스스로 떨어지므로, 부모가 먼저 Retire되어도 자식이 남은 동안(`firstChild != nullptr`)은 회수하지 않는다(sweep 사이의 `DrainRetired`·rollback 회수 포함). 회수가 이름 기록을 잠금 밖에서 반환한다.

Trace 오류나 작업 제출 실패는 수락한 모든 worker를 정리한 후 cycle을 중단한다. 마감 전 실패에서는 sweep을 하지 않는다. 마지막 성공한 `LastGcReport`도 덮어쓰지 않는다. 네이티브 대기 자체가 실패하여 worker의 stack context 사용 종료를 확인할 수 없으면, 그 context를 해제하고 계속하지 않고 fatal 계약을 적용한다.

## 병렬 실행 계약

`StepCollection`에 `WorkerScheduler`를 주면 `RuntimeTypeOps::parallelTraceSafe=true`인 타입만 worker에 제출한다. 기본값은 false이며, 생성 메타데이터의 순수 참조 방문은 true다. 사용자 Trace는 thread-safe 선언뿐 아니라 공유 상태 접근과 callback 계약을 실제로 지켜야 한다. 선언하지 않은 타입의 Trace는 소유 스레드에서 수행한다.

최대 64개 표식 작업을 한 배치로 제출하고 수락한 handle을 모두 기다린다. 수동 Trace와 병렬 배치를 혼합할 때 이전 배치를 먼저 완료한다. marker는 Registry gate 아래에서 mark epoch·큐·오류 상태를 갱신한다. scheduler allocator는 worker에서 capture를 해제할 수 있는 thread-safe allocator여야 한다.

같은 scheduler worker 안에서 Step을 호출하면 작업을 제출하기 전에 WrongThread다. 부분 제출 뒤 OOM, scheduler 종료 또는 취소가 발생해도 이미 제출한 marker가 끝나기 전 함수의 stack 상태를 파괴하지 않는다. 이는 별도 event 제어 회귀로 검증한다.

## 예산과 진단

`GcBudget.maxObjects`는 Trace, 슬롯 분류, 실제 저장소 회수의 처리량을 합산한다. 0은 InvalidArgument다. 기본값은 64이며 `maxNanoseconds` 기본값은 1,000,000ns다. 시간값 0은 시간 제한만 끈다.

이 시간 예산은 **soft deadline**이다. 사용자 Trace·소멸자, 잠금 대기, worker 배치 완료를 중간에 중단하지 않는다. Begin의 루트·슬롯 스캔과 회수 후보 탐색도 최악 시간 상한을 보장하지 않는다. 특히 ReclaimEligible와 완료 여부 검사는 현재 선형 슬롯 탐색이며, 대규모 저장소에서 cursor·인덱스 최적화와 pause percentile 벤치마크가 후속 과제다. `maxObjects=1`만으로 프레임 시간 상한을 보장한다고 해석하지 않는다.

`GcReport`는 검사·표식·retire·실제 회수 개수를 분리한다. duration은 Begin과 실제 slice의 소요 시간 합이며 slice 사이 게임 실행 시간은 제외한다. backend 보유 때문에 회수되지 않은 retired 객체가 있어도 현재 즉시 회수할 대상이 없으면 cycle이 완료될 수 있다.

## 검증과 남은 범위

`Core/Source/Runtime/ObjectRuntime/Tests/IncrementalGcTests.cpp`와 `IncrementalGcReviewTests.cpp`에서 삽입 장벽, 신규 루트, newborn의 outgoing edge, cutoff, slice 재개, safepoint, 취소, 잘못된 Trace, 큐 OOM, worker 제출 실패, 부분 제출 후 drain, 실제 worker 표식, 독립 그래프 oracle, 회수 예산과 backend 보유를 검증한다. 기존 full collector·수명·음성 대조군을 함께 유지한다.

hard real-time GC, mutator와 동시 스캔, 다중 mutator, 자동 root stack 스캔, 객체 이동/압축, 객체 cluster, 병렬 sweep, 대규모 성능 보증은 이 구현의 완료 범위가 아니다. 새로운 최적화는 현재 수명 규약과 독립 oracle을 유지하면서 별도 측정으로 승인한다. 실제 테스트 수와 native 실행 결과는 FoundationStatus 및 해당 회차 검증 보고서에서 확인한다.
