# Rocks Round 3 — 수명 계약 수정의 역방향 검증

검토일: 2026-11-10. 대상은 수정된 `Docs/Design/EngineImplementationPlan.ko.md`, `Docs/Design/CoreObjectInputDetailedPlan.ko.md`, `work/create_detailed_data.py`, 재생성된 `Docs/Design/CoreObjectInputData.json` 및 v1.1 작업/파일 CSV다. 원본과 생성물을 수정하거나 생성기를 재실행하지 않았다. 세 수정의 반대 방향 실패와 기존 여섯 복합 실행만 검토했다. 실제 엔진 구현·GPU·sanitizer 시험은 수행하지 않았다.

**결과: R2-RC02/03 PASS. R2-RC01의 기존 capture 보존 문제는 해소되었으나, 새 cleanup 전달 단계의 용량 보장에 R3-RC01(P1) 한 건이 남는다.** 신규 기능 누락이 아니라 승인된 task의 종료 진행을 보장하는 계약이다.

## 1. 역방향 반례별 판정

### RV01 — cleanup lane 종료·실행 중 취소·완료 observer 보존

**위치.** 상세 §8.1 384–388줄, §8.2 394줄, CT08 740줄. 전체 §8.1 589/603–605줄. 생성기 `fault_schedule['C17']` 130줄, JSON `tasks[ID=C17]`.

**수정된 경로의 확인.** Running body를 중지점에서 멈추고 취소 요청을 보낸다. observer가 TaskHandle을 보관한 채 cleanup lane의 실행도 잠시 늦춘다. body가 반환한 뒤에만 실행 lease가 0이 되고, callable/capture 소멸 및 ModuleLease 해제가 끝난 뒤 terminal이 release 게시된다. observer는 acquire로 이 상태를 읽는다. node/result record를 오래 보관해도 capture root가 남지 않는다. IO/GPU/독립 callback lease는 그 owner에 이관되어 task terminal만으로 조기 반환되지 않는다. 이 순서는 **R2-RC01의 기존 두 반례를 차단한다**.

상세 386줄은 terminal에 필요한 cleanup lane을 먼저 중지하지 않도록 요구하고 394줄은 terminal/callback drain 뒤 worker join을 수행한다. 단순한 shutdown 순서 역전도 문서상 금지된다. RunOnTerminal continuation은 별도 payload이므로 선행 task의 해제된 capture를 공유 저장소처럼 사용할 수 없다.

**잔존 반례: R3-RC01 · P1 — 승인된 task의 cleanup handoff가 capacity 실패할 수 있음.**

1. task node pool의 N개를 모두 승인된 task가 사용한다. 진단 observer도 N개 handle을 보유하므로 완료 후에도 node를 즉시 재사용하지 못한다.
2. N개 body가 반환하거나 queued cancel이 확정되어 지정 cleanup lane으로 전달해야 한다. 지금은 cleanup 완료 전 terminal을 게시할 수 없다.
3. cleanup을 일반 Submit 또는 같은 node pool에서 새 task로 만드는 구현은 capacity 오류를 받는다. shutdown이 동시에 진행되면 일반 admission도 닫혀 있다.
4. 기존 N개 node는 cleanup/terminal 이전이거나 observer가 보존하므로 새 node가 생기지 않는다. cleanup 전달 재시도만으로는 진행할 수 없다. lane을 켜 두는 것만으로 공간 부족은 해결되지 않는다.

현재 본문은 일반 Submit의 capacity 실패와 cleanup lane 수명을 각각 정의하지만, **승인 이후 cleanup을 전달하는 데 추가 할당/새 task node가 필요 없어야 한다는 조건**은 없다. 따라서 원래 수정의 안전한 해제 순서가 resource pressure에서 종료를 막는 새 상호작용이 발생한다.

**최소 수정안.** Submit 승인 시 현재 node 안에 cleanup link/state를 예약한다. body 종료 또는 pre-run cancel 확정 뒤 그 node를 지정 lane의 teardown queue로 이관하며 추가 allocation·일반 Submit·다른 task node를 사용하지 않는다. bounded queue를 사용한다면 Submit admission 단계에서 cleanup slot까지 확보하거나 요청을 명시 거부한다. 이미 승인된 task의 cleanup enqueue는 실패하지 않아야 한다. callable/capture destructor는 scheduler/queue/registry gate 밖에서 nonblocking·noexcept로 실행한다. 비동기 해제가 필요한 자원은 destructor에서 기다리지 않고 독립 owner에 lease를 명시 이관한다. cleanup lane 종료는 이 queue까지 drain한 뒤 수행한다.

**수용 시험.** CT08/C17/C24에서 node pool 및 일반 queue를 최소 크기로 줄이고 완료 handle을 전부 유지한다. admission 이후 모든 일반 allocation을 N-fail로 실패시키고, normal Submit 차단·pre-run cancel·running cancel·cleanup lane 지연을 조합한다. 승인 task의 payload cleanup/terminal은 각각 1회 진행하고, RootRef는 payload drain 뒤 0이며, observer node는 observer 종료 전 재사용하지 않아야 한다. destructor에 gate-reentry probe를 넣어 registry/queue lock을 보유한 채 호출하는 negative control이 검출되는지 확인한다. observer가 handle을 해제해야 cleanup이 가능해지는 구현은 실패다.

### RV02 — metadata 복사 실패·old reader unregister·R1 bootstrap

**위치.** 상세 §9.1 432줄, §15.1 639/643–649줄, CT10/CT22 742/754줄. 전체 §7.2 538줄. 생성기 `fault_schedule['C07']` 120줄, JSON `tasks[ID=C07]`.

**역방향 실행.** old registration revision R의 reader/visitor를 유지한다. 새 TypeSchema의 field/name/attribute 깊은 복사 중 각각의 allocation을 실패시킨다. source arena를 poison/reset하고 old unregister·동일 TypeId 재등록을 경합시킨다. 마지막 reader/객체 destructor가 끝난 뒤 재시도한다.

**PASS.** 완성된 snapshot만 원자 게시하고 실패 시 부분 등록을 전부 rollback하므로 old 등록이 반쯤 바뀔 수 없다. 초기 active TypeId의 비호환 재등록·제자리 table 교체는 거부되고 old reader가 남은 unregister는 Busy다. code와 metadata data를 registration revision lease로 함께 유지하여 source arena나 caller stack의 수명과 독립된다. code ModuleLease를 데이터 소유권으로 대체하는 기존 반례가 닫혔다.

R1 C07은 고정 Guid/POD와 Core 소유 byte buffer로 필요한 subset을 복사한다고 명시했다. C13 String/Name이나 C20 Clang 생성기를 기다리는 역의존은 생기지 않는다. C06의 loader 비의존 ModuleLease와 R0 Array/allocator를 사용하므로 native dynamic loader를 Foundation 선행 조건으로 요구하지 않는다. 깊은 복사 N-fail·old reader Busy·source poison이 기존 CT10/C07/C24에 연결되어 있다.

테스트 보고서는 첫 등록 실패와 active ID 충돌을 분리해야 한다. active ID 충돌을 정상 거부한 결과만으로 복사 중 모든 allocation 실패 지점이 실행되었다고 기록해서는 안 된다. 이는 테스트 입력 구성의 주의점이며 신규 설계 수정은 필요 없다.

### RV03 — UI pause·PIE 동일 tick·expired terminal cancel batch

**위치.** 상세 §17.2 825–826/834줄, §18.1 867–893줄, §18.2 901줄, IT19 1140줄. 전체 §22.6 1640줄. 생성기 `fault_schedule['I04']` 및 `['I15']`, JSON 동일 ID.

**역방향 실행.** LocalUser가 같은 Editor/PIE A/PIE B에서 같은 숫자 tick을 사용한다. A gameplay를 pause하고 UIRealTime pump와 B는 진행한다. A에 cancel batch를 만든 뒤 scope admission을 닫고, A를 재시작하여 새 generation을 발급한다. 구 batch의 소비가 지연되는 동안 B/새 A에 같은 tick 결과를 생성한다. 구 key 조회와 새 key 조회를 뒤섞는다.

**PASS.** InputReadKey에 scope id/generation·user·update domain·clock epoch·tick이 모두 포함되고 AdvanceInput과 두 조회가 같은 key를 사용한다. UIRealTime과 gameplay fixed/frame/replay의 숫자 tick을 합치지 않으므로 pause 중 UI pump가 A의 gameplay history를 덮지 않는다. 종료 scope에는 ScopeExpired, retention 범위 밖에는 HistoryExpired를 반환하며 다른 scope 최신 결과로 대체하지 않는다.

종료 cancel batch는 소비 또는 명시 폐기까지 기록한 뒤 scope를 회수하므로 무한히 지연된 observer 때문에 과거 scope를 영구 보존할 필요가 없다. 이미 회수된 scope의 terminal event는 재생성하지 않고 expired 오류로 보고한다. 이 정책은 terminal cancel의 무조건 전달 보장과 구분되며, 새 scope의 취소/입력으로 재해석하지 않는다는 계약과 일치한다. 실제 이벤트 전달이 필요한 종료 경로는 scope를 회수하기 전에 소비해야 한다.

기존 ingress의 device generation·InputStreamEpoch/lossVersion·route token·TextSession generation은 그대로 남아 있다. 소비 history scope를 추가해 이들을 대체하지 않았고, InputCore에 World/Widget 의존성도 생기지 않았다. Synthetic Host로 scope/clock을 먼저 검증한 뒤 IT19의 실제 UI/PIE 부분을 별도 실행하는 단계 구분이 유지된다.

## 2. 기존 여섯 복합 시나리오 회귀

| Round 2 시나리오 | Round 3 판정 | 확인한 연결 |
|---|---|---|
| CE01 병렬 mark+mutation/barrier+render snapshot | PASS | 상세 §13의 versioned cursor·dirty rescan·무손실 queue/admission, mutator 정지와 POD render 소비가 유지된다. metadata snapshot이 불변화되어 visitor metadata 수명도 강화되었다. |
| CE02 weak pin+destroy/cutoff+root release+retire | PASS | 신규 acquisition gate와 기존 보존 lease를 계속 구분한다. task capture root의 해제 시점이 앞당겨져도 실행 lease와 payload cleanup 뒤에만 해제하므로 실행 중 메모리를 수거하지 않는다. backend hold는 독립이다. |
| CE03 ctor/private root+GC+commit 전후 실패 | PASS | no-safepoint ctor·private ConstructionContext·precommit rollback·postcommit 정상 retire 규약이 유지된다. metadata 등록 실패는 객체 construction/publication 전에 반환할 수 있고 R1 선행 관계를 늘리지 않는다. |
| CE04 task cancel+observer 보존+root release | 기존 반례 PASS / 새 R3-RC01 잔존 | terminal이 payload cleanup 뒤로 이동하여 capture 유지·조기 소멸은 해결되었다. cleanup 전달의 capacity 보장은 추가해야 한다. |
| CE05 hotplug+PIE capture+지연 action/text | PASS | ingress generation과 소비 scope key가 분리되어 old event·old query 양쪽이 차단된다. 종료 cancel은 소비/폐기 기록 및 명시 expired 결과를 가진다. |
| CE06 schema source reset+old reader+unregister/reload | PASS | deep-copy snapshot·원자 게시·revision lease·Busy·native restart 정책을 함께 유지한다. old reader가 새 field layout을 제자리에서 읽지 않는다. |

이 표의 PASS는 해당 반례를 허용하는 문구가 없다는 설계 판정이다. 실제 memory ordering, task node 구현, GC work counter, platform event delivery의 검증을 대체하지 않는다.

## 3. 작업 데이터·문서의 일치 확인

읽기 전용 대조 결과, `detailed_task_data.json`과 `261110_Rocks_Core_Object_Input_Task_Backlog_KO_v1_1.csv`의 작업 52개 및 `261110_Rocks_Core_Object_Input_File_Map_KO_v1_1.csv`의 파일 297개가 모든 필드에서 일치했다(불일치 0건). 이전 버전 CSV는 이 확인 대상이 아니다.

생성기와 JSON의 C07은 source arena poison/deep-copy/old reader Busy를, C17은 완료 handle 1,000개와 body/cleanup/terminal을, I04/I15는 동일 user/tick 다중 scope·clock 격리·scope expired를 갖는다. 본문 CT08/CT10/IT19와 같은 방향이다. R3-RC01을 보완할 때에는 본문 외에도 생성기 C17의 fault/concurrency 항목에 **cleanup 무할당 handoff·최소 node pool·승인 후 allocation 실패**를 추가하고 JSON/CSV를 다시 생성해야 한다.

Round 4의 재검토 범위는 R3-RC01 한 건의 queue capacity/lock/정리 진행과 기존 terminal ordering 보존으로 한정할 수 있다. 다른 계약의 재설계나 새 SDK는 필요하지 않다.
