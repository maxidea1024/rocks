# Rocks Round 3 — AssetLease 수정 및 역방향 반례 검토

검토일: 2026-11-10. 대상은 `Docs/Design/EngineImplementationPlan.ko.md`(358,064 bytes), `Docs/Design/CoreObjectInputDetailedPlan.ko.md`(140,322 bytes), `Docs/Design/CoreObjectInputData.json`(144,311 bytes)이다. 원본은 수정하지 않았다. 줄 번호는 이 검토 시점 기준이다. 문서의 실행 순서와 계약을 대조한 결과이며 실제 프로그램·장치·하네스 실행 결과가 아니다.

## 1. 결론

**기존 R2C01의 직접 반례는 해결되었다. 추가 잔여는 P1 1건, R3C01이다.** 정상 AssetLease 보존, package Closing과 발급의 선형화, 다른 scope의 공유 revision 보존, revoked token의 retention 분리와 이중 반환 방지가 반영되었다. 다만 새 scope/package gate 계약은 사용자 callback·capture destructor의 실행 경계를 정의하지 않아 재진입 교착을 허용한다.

| 판정 대상 | 결과 | 위치 |
|---|---|---|
| ticket 반환 후 정상 lease 단독 사용 | PASS | 전체 §9.2, 줄 697 |
| lease 발급·복사와 정상 unload의 경합 | PASS | 전체 §9.2, 줄 697 |
| scope revoke와 다른 scope의 공유 revision | PASS | 전체 §9.2, 줄 699 |
| 외부 revoked token 장기 보유 | PASS | 전체 §9.2, 줄 699·703 |
| success callback의 gate 재진입 | 수정 필요 R3C01 | 전체 §9.2, 줄 689–699 |
| Core fake와 실제 loader/RHI Gate 구분 | PASS | 전체 줄 703, 상세 §26.2 줄 1263 |

## 2. 역방향 반례 A — revoke 이후 외부 token을 계속 보관

**실행 순서:** PIE A의 AssetLease를 editor 측 보관함에 복사한다. A에서 snapshot 추출을 시작한 뒤 Stop/revoke한다. 외부 복사본은 수천 frame 동안 보관한다. snapshot의 마지막 GPU 작업이 종료된 뒤 A의 종료를 완료하고, 나중에 외부 복사본을 파괴한다.

**판정: PASS.** 전체 줄 699는 신규 접근·발급·commit을 ScopeClosed로 거부하고, 진행 중 사용만 storage/backend lease로 보존한다. 마지막 허용 사용 종료 후 retention을 정확히 한 번 반환하며 외부 token은 상태 조회만 가능한 tombstone이 된다. 따라서 외부 token의 늦은 파괴를 기다리느라 package graph root를 계속 유지할 필요가 없으며 늦은 파괴가 retention을 중복 감소시킬 수도 없다.

**구현 수용 조건:** 외부 token 보관 중에도 A의 graph/root/backend retention은 마지막 작업 완료 후 0이어야 한다. tombstone 자체의 작은 상태 저장소는 실제 보관 owner에 계상하고 package graph·A의 VM·type/backend 자원을 숨겨서 붙잡지 않아야 한다. 새 package slot/generation을 재사용한 뒤 구 token을 파괴하여 새 retention이 바뀌지 않는지 검사한다. 줄 703의 revoked token 보관 시험이 이 반례를 포함한다.

## 3. 역방향 반례 B — success callback 재진입과 새 lease 발급

### R3C01 · P1 — 사용자 코드의 gate 밖 실행 규칙 누락

**위치:** 전체 §9.2 줄 689–690의 callback API, 줄 697의 package gate, 줄 699의 scope→package 순서. 전체 §8.1 줄 601은 lock 보유 task wait를 금지하지만 Asset OnReady callback을 모든 gate 밖에서 호출한다는 규칙은 없다.

**실행 순서:**

1. success completion이 scope admission과 package gate를 획득하여 AssetLease를 발급한다.
2. 구현자가 completion 직렬화 구간 안에서 OnReady callback을 호출한다. 현재 문구만으로는 이 배치가 금지되지 않는다.
3. callback은 같은 package의 lease를 복사하거나 RequestAsset을 호출한다. 또는 자신이 속한 PIE의 Stop을 요청한다.
4. 복사/발급은 package gate, Stop/revoke는 scope admission에 다시 진입한다. 비재귀 mutex라면 자기 교착이 발생한다. 재귀 mutex로 회피해도 callback이 만든 상태 변경이 바깥 completion의 판단을 무효화할 수 있다.
5. callback body가 단순해도 callback capture의 소멸자가 다른 lease를 반환하거나 구독을 해제할 수 있다. capture cleanup까지 gate 안에 남겨 두면 같은 문제가 반복된다.

**범위:** 기존 R2C01의 정상 resident 수명은 수정되었다. 이 지적은 새로 명시한 gate에 사용자 코드를 포함할 수 있다는 인접 계약 공백 한 건이다. 모든 가능한 구현이 교착한다는 뜻은 아니다.

### 권장 최소 수정

> scope/package gate 안에서는 delivery 결과와 실행 retention을 확정하고 소유 payload를 안전하게 이관하는 작업만 수행한다. 모든 gate를 해제한 뒤 지정 completion lane에서 사용자 callback과 사용자 capture destructor를 실행한다. revoke는 admission만 닫고 active callback의 종료를 lock 밖에서 비동기 drain한다. callback이 자신의 scope Stop을 요청하면 nonblocking 요청만 수행하며 자신의 종료 완료를 기다리지 않는다. callback 복귀 후 추가 gameplay commit이 필요하면 scope/generation을 다시 확인한다.

추가로 callback의 수용 실패와 완료 전달을 명확히 하면 새 gate 규약의 종료 진척을 보존할 수 있다. **새 callback token을 도입하지 않는 최소안**을 권장한다.

~~~cpp
Result<LoadTicket> RequestAsset(AssetRequest);
Result<void> OnReady(LoadTicket, TaskLane,
                     Function<void(Result<AssetLease>)>&& callback);
~~~

- RequestAsset 실패는 유효 ticket·request interest를 만들지 않는다.
- OnReady는 LoadTicket당 callback 하나만 수용하고 중복은 AlreadyRegistered로 반환한다. 실패 시 미등록·미실행이며 Function 소유권은 caller에게 남는다. 필요한 저장소·전달 slot 예약이 끝나기 전에 callable을 이동하지 않는다.
- 성공 시 loader가 callback payload·필요한 code/실행 lease를 소유한다. 기존 ticket의 관찰 record와 callback payload 수명을 분리하며, ticket의 장기 보관이 완료된 callable/capture를 보존하지 않게 한다.
- 수용 전에 completion record와 전달 경로를 예약한다. 일반 task Submit 거부·queue 포화·종료 시작 때문에 이미 수용한 callback을 유실하지 않는다. 종료는 수용된 terminal callback과 capture cleanup의 drain을 포함한다.
- 물리 로드의 Ready와 callback delivery admission을 구분한다. revoke가 delivery admission보다 먼저이면 ScopeClosed 결과를 정확히 한 번 전달한다. delivery admission이 먼저이면 발급·실행 retention을 보존하되 revoke 후 신규 접근·commit은 허용하지 않는다. callback의 성공 결과 자체를 영구 scope 사용 권한으로 해석하지 않는다.
- ticket 취소는 아직 발급되지 않은 caller 결과와 request interest에 적용한다. 이미 성공 발급된 lease를 묵시 revoke하지 않는 줄 697의 규칙은 유지한다. 같은 package의 다른 caller 결과를 취소하지 않는다.

**수용 조건:** callback body와 capture destructor 각각에서 같은 package 재요청·lease 복사/반환·자기 Stop을 실행하는 fixture를 구성한다. lock-rank/debug gate 검사로 사용자 코드 실행 시 held gate 0을 확인한다. completion queue 포화와 shutdown을 동시에 주입해 OnReady 거부는 caller 소유권 유지, 수용된 callback은 정확히 한 번 결과·cleanup을 완료하는지 확인한다. callback과 scope revoke의 양 순서를 검사하고 정상 종료가 자신의 execution retention을 기다리는 교착에 빠지지 않아야 한다.

## 4. 역방향 반례 C — 다른 scope 사용 + GC + 늦은 GPU Submit

**실행 순서:** A와 B가 같은 immutable package revision을 사용한다. A의 render packet이 recording lease를 획득한 채 일시 정지한다. A만 revoke하고 full GC를 시작한다. B는 자신의 정상 AssetLease로 새 snapshot을 생성한다. 이후 A packet이 늦게 Submit되고 A의 원래 LoadTicket과 revoked token이 파괴된다.

**판정: PASS.** 줄 697은 B의 정상 residency retention과 graph root를 유지하고, 줄 699는 A revoke가 B를 무효화하지 못하게 한다. 줄 895는 이미 승인된 A recording의 늦은 Submit을 허용하며 fence 등록과 lease 이관 사이에 보호 공백이 없도록 한다. 줄 897·901은 모든 queue 완료 전 회수를 금지한다. 줄 925는 mutable render scene/history의 session domain을 분리한다. A의 token 파괴가 B의 retention이나 새 generation을 감소시키지 않는다.

**구현 수용 조건:** B의 AssetLease가 살아 있는 동안 공유 graph는 GC oracle의 reachable set에 남아야 한다. A recording/fence 완료 전 GPU free 0, B의 ReadyMask·영상·history 변화 0, A의 closure 이후 B의 계속 사용 성공을 확인한다. 정상 package Closing과 물리 회수는 B의 lease 반환 및 마지막 작업 완료 이후에만 진행한다.

## 5. 기존 다섯 시나리오의 계약 회귀 대조

| 시나리오 | 최신 위치 | 판정 및 이유 |
|---|---|---|
| S1 · PIE Stop + GPU recording | §11.2 줄 895–903, §11.4 줄 925, §22.6 줄 1644–1661 | PASS. recording→submission 이관, session domain, 최종 cleanup closure 유지 |
| S2 · streaming cancel + GC + shared lease | §9.2 줄 693–703, §9.5 줄 727 | 기존 R2C01 직접 반례 해결. callback 재진입은 R3C01 보완 필요 |
| S3 · device lost + late callback | §9.6 줄 756–760, §11.2 줄 903, §29.6 줄 2237 | PASS. 이전 DeviceEpoch의 fence/resource를 새 domain에 적용하지 않음; 미확인 완료는 실패·quarantine |
| S4 · material + CPU headless bake | §14.8 줄 1155–1163, §14.10 줄 1194–1217 | PASS. CPU BakeMaterialProfile, immutable 입력, stale/부분 결과 publish 금지 유지 |
| S5 · plugin unload + render cache | §14.6 줄 1133–1137, §14.8 줄 1159–1161, 상세 §4.2 줄 187–189 | PASS. ModuleLease 및 작업/backend drain과 Busy unload 유지 |

## 6. 상세 문서·작업 데이터와 적용 범위

상세 §26.2 줄 1263은 C24 fake owner의 retention/late completion/revoke와 실제 M3 이후 package admission·공유 revision·GPU 시험을 분리한다. 전체 줄 703도 같은 구분을 적용한다. `detailed_task_data.json` C17(줄 310)의 terminal/capture cleanup 검증, C24(줄 436)의 수명 통합, C26(줄 472)의 CPU Foundation Gate, C27(줄 490)의 선택 loader 구분과 충돌하지 않는다.

전체 줄 589의 task terminal payload 분리는 이 검토의 callback payload 권고와 일치한다. 전체 Task/Input/metadata 수정의 별도 반례 검증은 담당 검토자의 결과를 따른다. 이 보고서는 해당 영역을 새로 전수 검토한 것으로 표시하지 않는다.

R3C01 보완은 §9.2의 callback acceptance·execution 경계와 기존 NL03/NL04·PI05 사례를 구체화하는 범위다. 신규 Core 작업 ID나 즉시 dynamic loader 구현을 요구하지 않는다. 수정본 확인 전에는 전체 closure를 PASS로 선언하지 않는다.
