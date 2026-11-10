# Rocks Round 4 — 렌더 소비 경계 최종 closure

검토일: 2026-11-10. 최신 `Docs/Design/EngineImplementationPlan.ko.md`, `Docs/Design/CoreObjectInputDetailedPlan.ko.md`, `work/create_detailed_data.py`, `Docs/Design/CoreObjectInputData.json`, v1.1 작업 CSV를 다시 읽었다. 검토 범위는 R2-RC01–03과 R3-RC01의 수정 및 그 상호작용이다. 원본·생성물을 수정하지 않았다.

**최종 판정: 네 항목 모두 문서 계약 기준 PASS. 이번 범위에서 잔존하는 구체 모순이나 새 필수 수정 사항을 발견하지 않았다.** 구현 코드·엔진 실행·성능·sanitizer·GPU 시험의 통과를 뜻하지 않는다.

## 1. 항목별 확인

| 항목 | 판정 | 최신 위치와 확인 내용 |
|---|---|---|
| R2-RC01 — task payload와 완료 observer 분리 | PASS | 전체 §8.1 589줄, 상세 §8.1 386줄. 실행 lease 종료 뒤 callable/capture를 해제하고 그 완료 후 terminal을 release 게시한다. 오래 보관한 TaskHandle은 실행 capture를 보존하지 않는다. observer/queue가 남은 node는 계속 재사용 금지다. |
| R2-RC02 — 타입 metadata의 저장소 소유권 | PASS | 전체 §7.2 538줄, 상세 §9.1 434줄. 중첩 metadata 깊은 복사·실패 rollback·완성 revision 원자 게시·code/data revision lease·old reader 중 unregister Busy를 유지한다. R1은 Core byte buffer/고정 Guid subset을 사용하여 String/Name·Clang·동적 loader를 선행 조건으로 만들지 않는다. |
| R2-RC03 — 입력 소비 scope와 clock key | PASS | 전체 §22.6 1646줄, 상세 §17.2 827–828줄 및 §18.1 874–895줄. Advance/Query/Copy가 scope/generation·user·domain·clock epoch·tick을 같은 history key로 사용한다. 종료 cancel batch의 소비/폐기 기록, ScopeExpired/HistoryExpired 및 타 scope 최신값 대체 금지가 유지된다. |
| R3-RC01 — cleanup 전달의 포화 진행 보장 | PASS | 전체 §8.1 591줄, 상세 §8.1 388줄. Submit은 capture 이동 전에 cleanup link/state 또는 slot까지 확보하고 실패 시 caller ownership을 보존한다. 승인 후 기존 node의 intrusive cleanup 경로는 추가 allocation·일반 Submit·다른 node·일반 queue 공간을 요구하지 않는다. destructor는 gate 밖에서 실행하며 cleanup queue와 진행 중 destructor를 drain한 뒤 lane을 닫는다. |

## 2. 새로 조합한 실행 반례

### RF01 — node pool 완전 포화 + normal admission 종료 + observer 장기 보유

N개 task가 모든 node를 차지하고 외부 observer가 N개 handle을 보관한다. 정상 Submit을 닫고, task 일부는 body 반환·일부는 queued cancel로 cleanup이 필요하게 한다. 이후 일반 allocator/일반 queue 여유가 전혀 없다고 가정했다.

**PASS.** cleanup 자원은 각 task 승인 전에 확보되고 실행 queue와 구별된 내장 cleanup link/state를 사용한다. 기존 node 이관에 새 node나 일반 Submit이 필요 없으므로 observer의 handle 해제를 기다리는 순환 의존이 없다. 각 capture가 해제된 뒤 terminal은 게시될 수 있다. observer가 계속 존재하므로 완료 node 재사용이 지연되는 것은 허용된 용량 제한이며 capture root가 남는 누수와 구분된다. 추가 새 task의 Capacity 오류는 허용되지만 이미 승인된 cleanup의 실패는 허용되지 않는다.

### RF02 — 이미 실행 중인 body 취소 + queued 취소 경합 + cleanup lane 지연

실행권을 획득한 body를 중지점에서 멈추고 cancel을 요청한다. 다른 thread의 queued-cancel 처리와 body 반환을 경합시키며 cleanup lane 실행을 일시 지연한다.

**PASS.** 취소 요청 자체는 terminal이 아니고, body 반환 또는 실행 불가 선형화와 실행 lease 0을 모두 만족한 뒤에만 payload를 해제한다. cleanup·terminal은 정확히 1회라는 계약이 있으므로 두 경로가 같은 capture를 중복 소멸시킬 수 없다. cleanup lane은 마지막 drain까지 살아 있으므로 일시 지연이 조기 자원 반환이나 lane 선종료를 정당화하지 않는다. 이 계약은 미협조 body의 강제 중단을 제공하지 않으며 기존 timeout 실패/진단 정책과 일치한다.

### RF03 — cleanup 중 마지막 RootRef Reset + GC root scan + ModuleLease 파괴

capture destructor가 마지막 RootRef를 Reset하는 순간 GC가 root snapshot을 취득하고, capture 안의 ModuleLease도 마지막 사용자 lease가 된다. 동시에 task completion observer와 모듈 종료 요청이 존재한다고 가정했다.

**PASS.** destructor 진입 전 scheduler/queue/registry gate를 모두 해제하므로 Reset이 root/GC gate를 획득할 때 상위 gate를 잡은 역순 재진입을 만들지 않는다. 상세 §11.1 514줄의 root unlink/snapshot 선형화 및 scan lease는 RootRecord 조기 반환을 막는다. 이는 RootRef 구현을 lock-free로 바꾸라는 요구가 아니다. callable 실행 코드의 별도 ModuleLease는 destructor가 반환할 때까지 유지하므로 capture 안의 사용자 lease가 먼저 사라져도 현재 destructor 코드를 unload하지 않는다. object/type revision의 lease는 객체 destructor·visitor가 끝날 때까지 별도로 유지된다.

비동기 backend 해제가 필요한 자원은 payload destructor에서 GPU/IO 완료를 기다리지 않고 미리 독립 owner로 lease를 이관한다. 따라서 task terminal이 backend 완료를 위장하지 않으며 cleanup lane이 자기 후속 task/fence를 동기 대기할 필요도 없다. nonblocking·noexcept destructor 규약과 정상 종료 drain 순서는 서로 충돌하지 않는다.

### RF04 — admission 실패 후 caller retry + 같은 capture 소유권

cleanup slot/node 확보를 실패시키고 같은 TaskDesc를 재시도하거나 caller scope에서 파괴한다.

**PASS.** callable 이동 전에 admission 자원을 확보하고 실패 시 caller ownership을 유지한다는 문구가 추가되어, 승인되지 않은 task가 capture를 숨겨 보유하거나 caller와 중복 파괴하는 것을 금지한다. 실패에는 유효 TaskHandle이 없다. 성공 뒤에는 capture가 task 소유가 되므로 caller가 같은 payload를 재사용할 수 없다. R0 inline executor에도 동일 Submit 계약을 적용할 수 있다.

### RF05 — metadata N-fail + old reader + input scope 재시작 회귀

metadata 복사 중 N-fail을 주입하고 old reader를 유지한 채 unregister를 시도한다. 별도로 동일 user/tick의 PIE A/B에서 A를 pause/종료/재시작하고 old cancel query를 지연한다.

**PASS.** metadata는 완성 snapshot만 게시하고 old revision은 Busy/lease로 보존된다. 입력은 새 scope/clock key를 사용하며 old key에 새 결과를 반환하지 않는다. 종료 cancel은 소비 또는 명시 폐기 기록 후 회수하고, 회수 후 지연 요청은 expired로 끝난다. 이번 Task 수정은 두 시스템의 데이터 identity·scope 수명에 개입하지 않는다.

## 3. 기존 복합 시나리오와 작업 데이터

Round 2의 병렬 GC+barrier/dirty rescan, weak pin+cutoff/retire, constructor/private root+commit 전후 실패 규약은 변경되지 않았다. Task capture 해제는 실행 lease 0 뒤에만 수행하고 GC root unlink 규약을 그대로 사용하므로 이 세 시나리오에 새로운 회수 경로를 만들지 않는다. hotplug/PIE/text ingress generation과 입력 history scope의 역할도 계속 구분된다.

생성기 `fault_schedule['C17']` 130줄과 재생성 JSON/CSV는 `node/queue 포화·독립 cleanup 진척·capture root 소멸`을 포함한다. 상세 CT08 742줄 및 §8.1의 추가 포화 fixture와 연결된다. C07의 metadata poison/Busy, I04/I15의 동일 tick scope/expired fixture도 유지된다. v1.1 작업 CSV 52행과 JSON 52개 task를 읽기 전용으로 모든 필드 대조한 결과 불일치는 0건이었다.

이 검토의 문서 범위 closure를 종료한다. 다음 완료 증거는 실제 구현에서 CT08/CT10/CT14/CT22/IT19와 연결된 fault/concurrency fixture를 실행한 결과이며, 본 보고서 자체를 그 시험 결과로 표시하지 않는다.
