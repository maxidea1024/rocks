# Rocks Round 4 — 에셋 수명·완료 callback 최종 계약 확인

검토일: 2026-11-10. 검토 대상은 `Docs/Design/EngineImplementationPlan.ko.md`(361,015 bytes), `Docs/Design/CoreObjectInputDetailedPlan.ko.md`(141,347 bytes), `Docs/Design/CoreObjectInputData.json`(144,392 bytes)이다. 원본은 수정하지 않았다. 줄 번호는 위 revision 기준이다.

## 1. 최종 판정

**검토 범위 내 PASS. R2C01과 R3C01의 반례가 모두 닫혔으며, 이번 수정으로 발생한 추가 계약 모순은 발견하지 않았다.**

이 판정은 문서에 명시한 소유권·선형화·오류·종료 규약의 대조 결과다. 실제 엔진 구현, 하네스 실행, sanitizer, GPU 및 플랫폼 시험을 수행한 결과가 아니다. 구현 단계에서 아래 조건을 그대로 검증할 의무가 남는다.

| 기존 지적 | 반영 위치 | 최종 판단 |
|---|---|---|
| R2C01 · 정상 AssetLease와 unload 조건 분리 | 전체 §9.2 줄 703–709 | request interest·residency retention, 동일 package gate, scope revoke·tombstone·타 scope 보존이 명시되어 해결 |
| R3C01 · callback/capture destructor의 gate 재진입 | 전체 §9.2 줄 695–697 | 모든 gate 밖 실행·execution retention·자기 Stop 비동기·등록 acceptance가 명시되어 해결 |
| 수용 후 queue 포화가 cleanup/terminal을 차단할 위험 | 전체 §8.1 줄 589–591, 상세 §8.1 줄 386–388 | admission 선예약·intrusive 독립 cleanup·gate 밖 destructor·lane drain 계약과 일치 |

## 2. 재진입·실패·경합 반례의 최종 대조

### A. callback에서 같은 package 재요청·lease 복사

**순서:** success delivery admission → callback 실행 → 같은 package RequestAsset 및 lease 복사 → callback capture destructor에서 lease 반환.

**PASS 근거:** 줄 697은 scope/package gate 안의 작업을 admission·record·execution retention 확정으로 제한한다. callback과 capture destructor는 모든 gate를 해제한 뒤 지정 lane에서 실행하므로, 재요청·복사가 바깥 completion의 gate를 자기 재진입하지 않는다. 줄 705의 scope→package 순서도 유지된다. 등록 실패와 새 요청 거부는 결과로 반환하므로 재진입을 무조건 성공시킨다고 약속하지 않는다.

**구현 확인 조건:** callback과 capture destructor 진입 시 관련 held gate 수 0, 같은 package의 성공 또는 명시 오류 반환, 마지막 retention 반환 1회, lock-rank 위반 0. recursive mutex로 문제를 숨기는 구현은 수용하지 않는다.

### B. OnReady 등록 OOM·queue 포화·shutdown

**순서:** 일반 task queue와 node pool을 포화 → OnReady 등록 중 record/전달 slot 확보 실패 → 재시도에서 수용 → 일반 admission 종료 → terminal 전달·capture cleanup.

**PASS 근거:** 줄 691–695는 RequestAsset과 OnReady의 동기 acceptance 오류를 명시한다. OnReady 실패는 미등록·미실행이며 caller Function 소유권을 보존한다. 성공 전에 callback record와 지정 lane 전달 slot을 예약하고, 성공 이후 일반 Submit·추가 allocation·일반 queue 여유를 요구하지 않는다. callback은 loader가 호출과 capture cleanup까지 소유하므로 ticket 반환 때문에 유실되지 않는다. 줄 591의 Task cleanup도 기존 node를 독립 intrusive queue로 이관하며 별도 node를 확보하지 않는다.

**구현 확인 조건:** 각 예약 단계 N-fail에서 callable 이동/실행 0, 성공 등록 뒤 일반 queue 포화에서 callback과 cleanup 진척, 중복 등록 AlreadyRegistered, terminal 1회, code lease의 마지막 destructor 이전 해제 0. 실패한 등록을 AlreadyRegistered 상태로 남기지 않는다. 이 조건은 실패 시 미등록이라는 본문 계약의 직접 결과다.

### C. callback이 자기 PIE Stop을 요청

**순서:** callback admission → callback이 자기 scope Stop 요청 → revoke가 신규 사용을 차단 → callback이 반환 → capture cleanup → execution retention 반환 → session closure.

**PASS 근거:** 줄 697은 자기 Stop을 비동기 요청으로 제한하고 자신의 callback 종료에 대한 동기 wait를 금지한다. revoke는 gate 밖에서 accepted callback과 cleanup을 drain한다. 실행 retention은 callback 반환·capture cleanup 뒤 한 번만 반환한다. 전체 §22.6 줄 1658의 비동기 Stop과 scheduler·metadata 보존 순서와도 일치한다.

**구현 확인 조건:** 자기 Stop 호출이 즉시 요청 결과를 반환하고 callback은 종료 가능해야 한다. Stop 이후 새 asset 접근·gameplay commit은 거부하고, cleanup만 허용한다. callback이 아직 실행 중인 동안 Stopped를 게시하지 않는다.

### D. queued success와 scope revoke의 양방향 경합

**순서 1:** 물리 로드 Ready → revoke → delivery admission. **순서 2:** 물리 로드 Ready → delivery admission → revoke → callback 반환.

**PASS 근거:** 줄 697이 두 순서를 구분한다. 첫 순서는 ScopeClosed terminal을 한 번 전달한다. 둘째 순서는 callback 반환까지 execution retention을 보존하지만, revoke 뒤의 신규 접근·commit은 scope를 재검사하여 거부한다. 성공 결과를 받았다는 사실이 무기한 논리 사용 권한을 주지 않는다. 실행 보호와 결과 commit 권한을 별도로 판정하므로 UAF 방지와 scope 종료가 충돌하지 않는다.

**구현 확인 조건:** admission 직전·직후를 멈추는 fixture에서 결과 수와 execution retention 반환 수가 각각 1이어야 한다. 이미 진행 중인 짧은 접근/IO/GPU 사용은 완료 전 회수하지 않고, revoked scope에 새 결과를 commit하지 않는다.

## 3. R2C01 수명 회귀 확인

| 역방향 사례 | 적용 계약 | 판정 |
|---|---|---|
| callback 성공 후 마지막 ticket 반환, lease만 다음 frame까지 유지 | 줄 703: 발급 residency retention과 graph root 보존, ticket 취소가 발급 lease를 묵시 revoke하지 않음 | PASS |
| interest 0 판정과 새 lease 발급·복사 경합 | 줄 703: 동일 package gate, 먼저 성공한 admission에 따른 Closing 배제 | PASS |
| revoke 이후 외부 token 장기 보관·늦은 파괴 | 줄 705: 마지막 사용 종료 시 retention 1회 반환, token은 tombstone, 중복 반환 금지 | PASS |
| 공유 A/B 중 A만 Stop, B는 GC 중 계속 사용 | 줄 703·705: 다른 scope lease/root 유지, 공유 graph 임의 파괴 금지 | PASS |
| A의 기존 recording이 revoke 이후 늦게 Submit | §11.2 줄 901–909: 이미 승인된 recording 보호, fence 등록과 lease 이관 원자성, device domain 확인 | PASS |

본문 줄 709는 위 lifetime 사례와 callback 재요청·자기 Stop·등록 OOM/queue 포화·admission 전후 revoke를 기존 NL03/NL04·PI05에 연결한다. 마지막 lease/작업 종료 후 full GC 2회 및 backend drain의 기준선 회복도 유지된다.

외부에 보관한 정상 ticket·lease와 사용 권한을 잃은 tombstone은 같은 상태가 아니다. 정상 소유자가 의도적으로 유지하는 권한은 ledger의 보유 원인으로 표시하고, revoke 후 tombstone이 package graph나 종료된 scope를 숨겨서 보존하는 구현은 허용하지 않는다.

## 4. 기존 다섯 복합 경로의 회귀 범위

- PIE Stop·GPU recording: §11.2 줄 901–909, §11.4 줄 931, §22.6 줄 1658의 snapshot domain·fence·closure 규약을 유지한다.
- streaming cancel·GC·공유 lease: §9.2 줄 695–709로 기존 보존·callback 공백을 해소한다.
- device lost·늦은 callback: DeviceId/DeviceEpoch 및 구 recovery domain 규칙이 유지된다. 새 장치 fence로 구 자원을 회수할 수 없다.
- CPU headless material bake: §14.10 줄 1202의 BakeMaterialProfile과 GPU readback 비의존 계약을 유지하며, callback 변경이 bake의 stale publish 금지를 약화하지 않는다.
- plugin unload·render cache: §14.6 줄 1143·§14.8 줄 1165의 snapshot/module lease 보존과 Task의 destructor 종료까지 code lease 유지 규약이 일치한다.

이번 확인에서 이 다섯 경로에 새로운 모순은 발견하지 않았다. 실제 backend의 동작이나 전체 기능 구현 완료를 뜻하지 않는다.

## 5. 상세 문서·작업 데이터와 종료

상세 §8.1 줄 386–388은 전체 Task의 payload/observer 분리·할당 없는 cleanup 규약과 같다. 상세 §26.2 줄 1265는 C24 fake owner 시험과 M3 이후 실제 loader/RHI 검증을 분리한다.

`detailed_task_data.json`의 C17 줄 310–323은 node/queue 포화·독립 cleanup 진척·capture root 소멸을, C24 줄 436–449는 gate 밖 callback·자기 Stop 비동기를 검증 범위로 기록한다. 신규 task나 callback token 없이 기존 작업 체계에 연결되었다.

**이번 검토 범위의 미해결 P0/P1/P2 지적: 0건.** 전체 시스템의 무결함을 보편적으로 입증한 결과는 아니며, 각 구현 PR에서 위 반례를 실제 테스트로 실행하고 target·seed·revision·PASS/FAIL/NOT RUN과 lease/owner 종료 증거를 남겨야 한다.
