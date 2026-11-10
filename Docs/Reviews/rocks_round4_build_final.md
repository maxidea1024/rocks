# Rocks 구현 계획 Round 4 최종 교차 검토 — 빌드·초기 구현·작업 추적

검토일: 2026-11-10. root의 Task cleanup 및 Asset callback 수정, JSON/CSV 재생성 완료 후 최신 파일을 다시 읽었다. 마스터·상세안·JSON·CSV 원본은 수정하지 않았다.

**판정: 담당 범위의 문서 검토 PASS.** 새 cleanup/delivery 계약은 C06의 동기 실행기와 R1의 최초 오브젝트 경로를 상위 loader 완료에 의존시키지 않는다. C24의 fake 시험과 M3 이후 실제 loader/RHI 시험도 구별되어 있다. Round 2의 빌드 관련 두 항목은 해결 상태를 유지한다. 새 확정 결함은 발견하지 못하였다.

이 판정은 설계 문구·예정 작업·파일 목록의 일관성 검토다. 실제 엔진 컴파일, node pool 포화 시험, 비동기 callback 실행, 장치 시험을 수행한 결과가 아니다.

## 1. 수정 계약과 초기 Core의 선행 조건

| 검토 경로 | 구체 반례 | 최신 근거와 판정 |
|---|---|---|
| C06 inline Submit → capture 정리 → terminal | cleanup 전달을 새 Task로 Submit하여 일반 queue/node pool이 포화될 때 초기 executor의 완료도 막힌다. 또는 cleanup 때문에 C17 worker를 먼저 구현한다. | 상세 8절 388행과 마스터 591행은 승인 전에 내장 cleanup link/state 또는 slot을 확보하고, 승인 후에는 추가 할당·일반 Submit·별도 node·일반 queue 여유를 요구하지 않는 전달 경로를 정한다. 상세 15.1의 641·651행은 C06의 CT08을 동기 executor subset으로 유지한다. C06의 같은 실행 lane에서 직접 정리하거나 이미 확보한 내장 경로로 정리하는 구현이 가능하며 C17의 worker queue를 먼저 요구하지 않는다. **PASS.** |
| R1 boot/full GC/shutdown → TaskHandle 장기 보관 | 종료 observer가 남아 있다는 이유로 capture의 RootRef가 계속 남거나 destructor가 registry gate 안에서 재진입하여 교착한다. | 상세 386–388행은 완료 관찰 record와 실행 payload를 분리하고 capture destructor 완료 뒤 terminal을 게시한다. destructor는 scheduler/queue/registry gate 밖에서 실행하며 noexcept·비차단 규약을 따른다. 보관 handle이 node 재사용을 지연할 수는 있지만 capture 정리와 terminal을 지연시키지 않는다. C06의 inline 부분 검증과 C17의 0/1/N worker·queue 포화 검증을 구별하므로 R1 착수 조건에 병렬 구현을 앞당기지 않는다. **PASS.** |
| C24 fake owner → callback acceptance/revoke | 새 OnReady 계약을 시험하려면 실제 package loader·World/PIE·RHI가 먼저 필요하여 Foundation Gate가 순환한다. | 마스터 9.2의 691–697행은 RequestAsset/OnReady의 Result 반환, 실패 시 소유권 유지, 승인 전 전달 slot 예약, 승인 후 gate 밖 callback 및 cleanup을 명시한다. 실제 public loader API는 상위 Asset 계약에 남는다. 상세 26.3의 1265행과 마스터 709행은 C24를 fake owner의 retention/late completion/revoke 시험으로 한정하고, 실제 package admission·공유 revision·GPU는 M3 이후 NL03/NL04·PI05에 둔다. JSON C24 파일은 ObjectTaskBridge·BackendLeaseTests·ReentrancyTests·FakeBackendCompletion·TaskLifetimeInterleaving이며 AssetLoader 구현을 요구하지 않는다. **PASS.** |
| accepted callback → 같은 package 재요청/자기 Stop → revoke | scope/package lock을 가진 채 사용자 callback을 호출하거나 callback이 자기 종료를 동기 대기하여 fake 및 실제 종료가 막힌다. | 마스터 697행은 gate 안에서 admission/record/execution retention만 확정하고 사용자 callback/destructor는 모든 gate 밖에서 실행한다. 자기 Stop은 비동기 요청이며 revoke도 accepted callback과 cleanup을 lock 밖에서 drain한다. revoke가 먼저인 경우 ScopeClosed terminal 1회, delivery가 먼저인 경우 execution retention 유지와 이후 commit 재검사를 구별한다. JSON C24의 동시성 검증에도 gate 밖 callback·자기 Stop 비동기를 반영하였다. **PASS.** |

C06에 없는 lane/backend를 성공 stub으로 흉내 낼 필요는 없다. 기존 단계별 subset 규칙에 따라 실제 지원하는 동기 실행 경로의 admission·정리·terminal 의미를 먼저 검증하고, C17 이후 병렬 경합을 추가하면 된다. R1은 상세 14절 633행대로 inline executor·C06 logging·R1 ledger를 조립하며 전체 비동기 IO/Trace worker/Archive를 먼저 요구하지 않는다.

## 2. Round 2 항목의 해결 상태 유지

| 항목 | 최신 확인 | 판정 |
|---|---|---|
| R2B01: 공개 Sync 계약의 늦은 최초 도입 | 상세 작업표 677행과 JSON/CSV에서 `Engine/PlatformBase/Public/Platform/Sync.h`의 최초 소유 작업은 C02 하나다. C16은 C02 Sync.h 확장으로 표시한다. C02 atomic/mutex와 C16 thread/wait의 구분도 유지한다. | **해결 유지** |
| R2B02: formatter 설정/실행 파일의 생성 캐시 입력 누락 | 마스터 27.2의 1960행과 상세 9.3의 446행에 formatter executable/version digest·실제 style 설정 digest, 단일 최종 producer, 동일 최종 바이트의 timestamp 보존을 유지한다. CT12 746행 및 C20 데이터에 config/binary 변경·stale artifact 검증이 남아 있다. | **해결 유지** |

## 3. 실제 수행한 작업·파일 데이터 검사

최신 JSON과 다음 재생성 CSV를 파싱하여 모든 행의 모든 필드를 비교하였다.

- `outputs/261110_Rocks_Core_Object_Input_Task_Backlog_KO_v1_1.csv`
- `outputs/261110_Rocks_Core_Object_Input_File_Map_KO_v1_1.csv`

| 검사 | 결과 |
|---|---|
| 작업 수 / 예정 파일 수 | 52 / 297 |
| JSON ↔ 작업 CSV 전체 행/필드 불일치 | 0 |
| JSON ↔ 파일 CSV 전체 행/필드 불일치 | 0 |
| 존재하지 않는 선행 작업 / 작업 DAG cycle | 0 / 0 |
| 예정 파일 경로 중복 | 0 |
| C06 전이적 선행 집합 | C00–C05 |
| C12 전이적 선행 집합 | C00–C11 |
| C24 직접 선행 | C18·C22·C23 |
| C24 전이적 선행 집합 | C00–C14, C16–C23 |

C24의 선행 집합에 World·실제 AssetLoader·RHI·Studio/PIE·dynamic loader 작업은 없다. 이 결과는 명시적인 작업 의존성과 파일 소유 관계의 일관성을 확인한다. 실제 C++ include/link 의존성의 부재를 입증한 것은 아니다.

## 4. 구현 단계에서 남는 검증

계획 변경으로 검증을 이미 끝낸 것으로 처리하지 않는다. C06은 실패한 Submit의 caller 소유권 유지와 동기 capture 정리/terminal 순서를 검사한다. C17은 0/1/N worker, 일반 node/queue 포화, 외부 완료 handle 보관 중 cleanup 진척을 검증한다. C24는 fake owner를 사용하여 gate 밖 재진입·자기 Stop 비동기·늦은 완료·revoke를 확인한다. M3 이후에는 실제 loader의 callback 등록 OOM/queue 포화, admission 전후 revoke, package 재요청, 공유 revision 및 RHI 완료를 별도로 검증한다. 실제 환경에서 실행하지 않은 항목은 NOT RUN으로 남긴다.

담당 범위에서 추가 문서 수정 요청은 없다. 초기 Core/Object 구현 순서와 기존 두 빌드 수정의 해결 상태를 유지한 채 다음 구현 단계로 진행할 수 있는 계획으로 판정한다.
