# WorkerScheduler·점진 GC 독립 교차 검토

이 검토는 WorkerScheduler 작성자와 GC 작성자가 아닌 Core 담당자가 공개 계약, 구현과 실패 경계를 대조한 결과다. 코드 수정 전 새로운 복합 호출 순서를 구성하였으며, 기존 테스트 성공 결과만으로 안전성을 판단하지 않았다. 검토 대상은 `WorkerScheduler.cpp/.h`, `ObjectRegistry.cpp/.h`, 관련 구현 문서와 기존 점진 GC 회귀다.

## WG01 — 중첩 Submit의 바깥 scheduler 종료 교착

**심각도: P1. 수정 완료, 새 회귀의 통합 실행 확인 대기.**

수정 전 WorkerScheduler는 활성 Submit을 `thread_local WorkerState* submittingState` 하나로 기록하였다. SubmitScope는 이전 포인터를 저장·복구하였지만 현재 중첩 중인 바깥 호출을 조회하지 못하였다. Shutdown의 재진입 검사는 해당 포인터가 자기 state와 같은지만 검사하였다.

다음 순서는 공개 API와 allocator callback 재진입 계약을 위반하지 않으면서 교착한다.

1. 같은 owner thread에서 시작한 scheduler A와 B를 준비한다.
2. A.Submit이 용량과 control 수명을 예약한 뒤 A allocator를 호출한다.
3. A allocator callback이 B.Submit을 호출한다. 현재 TLS 포인터는 B가 된다.
4. B allocator callback이 A.Shutdown을 호출한다.
5. 기존 Shutdown은 현재 포인터가 B이므로 A의 활성 제출을 찾지 못하고 A worker를 Join한다.
6. A worker는 아직 반환되지 않은 A Submit reservation이 0이 되길 기다린다. owner는 Join 중이므로 A allocator와 바깥 Submit이 복귀하지 못한다.

이 문제는 문서의 “Submit/lifecycle allocator 콜백에서 재진입한 Shutdown은 Busy” 계약과 충돌한다. 단순한 동일 scheduler 재진입 검사나 기존 포인터의 복구만으로는 해결되지 않는다.

현재 수정 위치는 `Engine/Task/Private/WorkerScheduler.cpp:53`의 TLS 프레임 포인터, `:104`의 SubmitScope, `:122`의 전체 프레임 검색과 `:591`의 Shutdown 검사다.

parent 승인 후 SubmitScope를 할당 없는 TLS 연결 프레임 스택으로 변경하였다. `Engine/Task/Private/WorkerScheduler.cpp`의 `SubmitScope`, `IsSubmitting`과 Shutdown이 모든 활성 제출 프레임을 검색한다. 프레임 수명은 실제 C++ 호출 스택에 묶이고 복사·대입을 금지한다. 파괴 시 최상단 프레임 여부를 검사하고 이전 프레임을 복원한다.

`Tests/Foundation/WorkerSchedulerReentryReviewTests.cpp`에 두 회귀를 추가하였다.

- A→B→A.Shutdown과 B.Shutdown은 Busy이며 활성 Submit이 없는 C.Shutdown은 성공한다. A와 B의 body는 이후 한 번씩 정상 실행되고 최상위 Shutdown도 성공해야 한다.
- 같은 중첩 이후 B record 할당에 OOM을 주입한다. B의 caller Function은 유지되고 B body는 실행되지 않아야 한다. 양쪽 reservation은 0, TLS 프레임은 복구되고 최종 allocator ledger는 0이어야 한다.

두 소스 파일은 clang-format을 적용하였다. 이 검토 기록을 작성한 시점에는 새 회귀의 컴파일·실행을 수행하지 않았으며, parent가 관리하는 통합 빌드에서 확인해야 한다. 기존 버전을 일부러 교착시키는 실행은 수행하지 않았다.

## GC 대조 결과

아래 경계에는 새 확정 결함을 찾지 못하였다. 이는 지정된 경로의 소스 대조 결과이며 전체 GC의 보편적 정확성 증명이나 새로운 실행 결과가 아니다.

| 대입한 경합·실패 | 확인한 코드 근거와 판정 |
| --- | --- |
| 루트 감소와 Marking 사이의 외부 RootRef Clone | RetainRoot는 gate 안에서 processing/closed와 기존 roots를 확인하고 ShadeLocked 후 계수를 올린다. slice 동안 복제를 거부하고 slice 사이의 새 보존은 큐에 게시한다. 서로 다른 wrapper 반환은 계수만 변경하며 객체 필드를 바꾸지 않는다. |
| 검은 객체에서 흰 대상으로 새 간선 게시 | SetEdge는 동일 gate 안에서 owner·target과 native type을 확인하고 대상 shade 이후 field를 게시한다. 큐 부족 등 실패는 field 게시 전에 반환한다. |
| Marking 중 생성·롤백·슬롯 재사용 | CreatePending이 gate 밖 큐 확장 전에 processing을 켜며, Commit은 모든 newborn을 grey 상태로 큐에 삽입한다. 이미 게시되어 큐에 들어간 객체는 Marking 중 retire하지 않는다. 이전 cycle의 retired 저장소나 아직 게시되지 않은 rollback 슬롯 회수는 이번 큐의 게시된 객체를 재사용하는 경로가 아니다. |
| cutoff 직후 아직 sweep cursor가 방문하지 않은 흰 객체 | 큐 소진 확인과 Sweeping 전환은 gate 안에서 수행한다. CheckLive, SetEdge, staged edge 검증과 backend acquisition에 cutoff 검사가 연결되어 새 접근·보존을 차단한다. 기존 root가 있던 객체는 root snapshot 또는 삽입 장벽으로 이미 표식된다. |
| Sweeping 중 새 chunk와 신규 객체 생성 | 새 chunk는 기존 sweep cursor 앞에 추가되더라도 newborn이 현재 epoch로 표식된다. Commit 전 간선 검증이 cutoff 대상을 거부하므로 기존 흰 그래프를 부활시키지 않는다. Reclaiming에는 미분류 흰 Alive 객체가 남지 않는다. |
| 병렬 marker 일부 수락 후 OOM·종료·취소 | StepCollection은 오류를 기록한 뒤 수락한 handle 전부를 대기한다. terminal은 worker capture 정리 이후 게시된다. 수락 작업이 남아 있는 상태로 stack MarkState를 파괴하지 않으며, native Wait 실패는 fatal이다. |
| Trace의 잘못된 간선과 동시에 실행 중인 다른 Trace | MarkEdge의 error·mark·work queue 처리는 Registry gate로 보호한다. batch drain 후 오류가 있으면 cycle을 Idle로 돌리고 sweep을 수행하지 않는다. LastGcReport는 마지막 성공 결과로 유지한다. |
| backend 보유분과 cycle 완료의 경합 | Retired 저장소는 roots/borrows/backendHolds가 모두 0일 때만 Reclaiming으로 분리한다. backend 보유로 당장 회수할 대상이 없으면 cycle을 끝내고 이후 DrainRetired가 회수할 수 있다. 이는 문서에 명시되어 있다. |

같은 scheduler worker에서 Step을 실행하는 반례는 IsCurrentWorker 검사로 제출 전에 차단한다. TypeRegistry callback lease는 각 slot이 보존하고, 병렬 Trace 동안 mutator 재개·객체 생성·slot 회수를 processing으로 차단한다. owner가 다음 slice로 복귀하기 전에 해당 batch를 모두 기다린다는 범위에서 일반 필드의 동시 변경을 허용하지 않는다.

## Worker 수명·실패 경계의 추가 대조

WG01 외에 다음 경로에서 새 확정 결함은 찾지 못하였다.

- Submit은 입력과 dependency를 검증하고 capacity·dependency·state 수명을 예약한 뒤 외부 allocator를 호출한다. 승인 전 Function을 이동하지 않는다. 종료와 경합하여 승인을 잃으면 준비한 dependency·record를 정리하고 reservation을 반환한다.
- worker는 Function capture와 dependency를 gate 밖에서 정리하고 terminal을 게시한다. inFlight는 cleanup이 끝날 때까지 포함한다. 완료 handle이 남아 있어도 capture는 유지하지 않으며 record/control storage와 주입 allocator 수명은 별도로 보존한다.
- 부분 thread 시작 실패는 성공적으로 시작된 worker를 정지·Join한 후 worker 배열을 정리한다. 성공한 Start 세대에서만 dependency를 수락하며 이전 세대 결과 handle은 조회만 유지한다.
- worker 문맥에서 같은 scheduler의 Wait와 Shutdown은 거부한다. pending 취소는 body를 생략하고 running 취소는 협력적 요청이며, 명시 오류·dependency 실패가 최종 취소보다 우선한다.

## 문서 일치와 검증 한계

`Docs/Implementation/IncrementalGarbageCollection.ko.md`의 단일 mutator, slice별 worker drain, cutoff, soft deadline, 실패 시 sweep 금지와 backend 지연 회수 설명은 검토한 코드와 일치한다. `WorkerScheduler.ko.md`의 Submit 재진입 Shutdown 규약은 WG01 수정으로 중첩된 다른 scheduler 경로까지 구현하였다.

이 회차는 코드 대조와 두 회귀 작성이다. 새 회귀의 Windows 실행 수치와 결과는 별도 통합 증거에서 확인해야 한다. macOS 네이티브 실행, TSan, scheduler 간 일반 사용자 작업의 순환 대기 방지, 무한 body의 강제 종료, hard real-time GC는 검증·보장하지 않았다.

## 후속 통합 실행 확인

위 기록 이후 최종 소스 `a321125b22a4ca628f5bbc15b3bb0f74b9ab1bfda003e46bb33ffa015e75b27a`로 Windows Debug·Release·ASan 전체 179개와 CTest 10/10/11개가 통과하였다. 이 보고서의 새 독립 회귀도 실제 실행 목록과 PASS에 포함된다. 같은 소스의 Release 100회 반복 17,900회는 실패 0이다. 원기록의 실행 대기는 검토 당시 상태이며, 최종 로컬 실행 근거는 [5차 검증 기록](ImplementationRound5.FoundationValidation.ko.md)을 따른다. macOS 결과는 별도 native CI 증거로 확정한다.
