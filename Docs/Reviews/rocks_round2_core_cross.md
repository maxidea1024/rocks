# Rocks Round 2 — Core 관점의 상위 시스템 경계 교차 검토

검토일: 2026-11-10. 검토 대상은 최신 `Docs/Design/EngineImplementationPlan.ko.md`(351,742 bytes), `Docs/Design/CoreObjectInputDetailedPlan.ko.md`(135,776 bytes), `Docs/Design/CoreObjectInputData.json`(144,065 bytes)이다. 원본은 수정하지 않았다. 아래 줄 번호는 검토 시점의 파일 기준이다.

## 1. 판정

**수정 필요: P1 1건.** AssetLease를 보유한 정상 사용자의 수명과 package unload의 시작 조건을 연결하는 계약이 빠져 있다. 나머지 네 복합 시나리오에서는 현재 문서의 조항을 함께 적용하면 실행 순서 반례가 차단된다. 이 PASS는 문서 계약에 대한 판정이며 구현·장치 시험의 통과를 뜻하지 않는다.

| 복합 시나리오 | 판정 | 핵심 근거 |
|---|---|---|
| S1 · PIE Stop + 미제출 GPU recording + 다른 PIE 실행 | PASS | session domain, recording lease, 늦은 Submit의 원자적 fence 이관, 비동기 Stop closure |
| S2 · streaming 취소 + 공유 load + GC + 성공 lease 보존 | 수정 필요 R2C01 | request interest와 issued resident lease가 일반 unload 조건에 어떻게 결합되는지 미정 |
| S3 · device lost + resource callback 역순 + 장치 재생성 | PASS | DeviceId/DeviceEpoch가 fence·resource·completion의 동일 domain을 정의 |
| S4 · material 변경 + CPU headless bake + 취소/늦은 결과 | PASS | BakeMaterialProfile, immutable snapshot, 명시 unsupported, stale/부분 publish 금지 |
| S5 · plugin unload + material compiler/preview + render cache | PASS | ModuleLease, admission 차단·drain·Busy, runtime과 compiler node의 분리 |

## 2. R2C01 — 정상 AssetLease와 package unload admission의 연결 누락

**위험도: P1. 위치: 전체 계획 §9.2, 줄 685–693.**

- 줄 689: “LoadTicket은 요청 관심을 나타내며 AssetLease는 실제 resident 수명을 유지한다.”
- 줄 693: “언로드는 관심 refcount 0 → 신규 사용 차단 → subsystem detach → RootRef 해제 → 논리 수거/retire → backend 완료 → 물리 회수 순으로 구분한다.”
- 같은 절은 공유 package의 caller별 취소·terminal 결과를 분리하지만, 성공으로 발급된 AssetLease가 interest 또는 별도의 residency pin에 포함되는지 정의하지 않는다.
- §22.6 줄 1632는 불변 asset을 revision lease로 공유한다고 명시한다. 따라서 기존 lease가 살아 있는 동안 요청 ticket만 종료되는 정상 사용 사례가 존재한다.

### 실행 순서 반례

1. PIE A와 EditorWorld가 같은 package revision을 요청한다. loader는 하나의 공유 작업으로 묶고 둘에게 각각 성공 AssetLease를 전달한다.
2. A는 비동기 요청이 끝났으므로 LoadTicket을 반환한다. EditorWorld도 ticket을 반환한다. 두 AssetLease는 계속 유효하며 이후 프레임의 데이터 조회와 snapshot 추출에 사용한다.
3. request interest가 0이 되어 줄 693의 unload 경로가 신규 사용을 차단하고 graph root를 반환한 뒤 논리 retire를 시작한다.
4. AssetLease가 RootRef를 내부적으로 보유한 구현이면 저장소는 남을 수 있다. 그러나 §5.2의 일반 Resolve 거부 때문에 정상 lease에서 다음 데이터 조회가 실패한다. 반대로 native resident hold만 보유하고 관리 graph root를 유지하지 않는 구현이면 GC가 asset graph를 수거할 수 있다.
5. A의 Stop과 editor의 계속 사용이 겹치면, scope 취소를 package 전체 revoke로 해석한 구현에서 editor lease까지 사용할 수 없어진다.

이 반례는 반드시 UAF가 발생한다는 주장이 아니다. **서로 다른 안전 구현을 선택해도 정상 lease의 사용 가능성, graph 보존 범위, 공유 사용자 취소 격리가 일치하지 않는 설계 공백**이다. 일반 RootRef의 저장소 보존 규칙만으로 AssetLease의 제품 계약이 결정되지 않는다.

### 최소 수정안

§9.2에서 아래 계약을 한 단락으로 확정한다. 별도 작업 ID나 새 범용 수명 체계는 필요하지 않다.

> LoadTicket의 request interest와 AssetLease의 residency retention은 별도 계수다. 성공 AssetLease 발급은 package revision의 resident record 및 필요한 관리 graph root를 보존하며, 해당 lease의 선언된 ReadyMask와 최소 residency 계약을 release 또는 명시 scope revoke까지 유지한다. texture의 추가 mip/page pin은 별도 계약이다. 정상 unload는 request interest와 발급된 residency retention이 모두 0일 때만 Resident→Closing으로 전이한다. lease 발급·승격과 이 전이는 같은 package admission gate에서 선형화하여, 발급이 먼저 성공하면 unload가 대기하고 Closing이 먼저이면 새 발급을 실패시킨다. ticket의 취소·반환은 이미 성공 발급한 lease를 묵시 취소하지 않는다.

> PIE/World 종료 등 명시 scope revoke는 일반 cache eviction과 분리한다. revoke는 owner scope/generation의 신규 사용·발급·결과 commit만 차단하고, 다른 scope가 보유한 같은 immutable revision lease를 무효화하지 않는다. revoke 이후 일반 접근은 ScopeClosed/Stale/Cancelled 중 명세한 상태를 반환한다. 진행 중 짧은 접근, callback, 추출 snapshot, upload/recording은 각 storage/backend lease로 완료까지 보존하며, 해당 scope의 root/retention은 마지막 허용 사용이 끝난 뒤 반환한다. scope revoke 또는 request cancel 자체를 GPU/IO 완료로 취급하지 않는다. package 자체 retire는 남은 다른 사용자·retention을 다시 확인하며, 종료 완료는 기존 owner ledger의 closure로 판정한다.

상태 이름은 최종 API에서 하나로 정해도 된다. 중요한 구분은 정상 유효 lease, 사용 권한을 잃었지만 완료를 기다리는 retention, 공유 revision의 다른 정상 사용자다. forced revoke가 성공 callback과 경쟁할 때도 같은 scope/package gate를 통해 발급 성공 또는 취소를 한 번만 결정한다.

### 수용 조건

- OnReady 성공 직후 마지막 LoadTicket 반환 → AssetLease만으로 다음 프레임 CPU 조회·render snapshot 생성 → lease 반환 후에만 정상 unload 시작.
- request interest 0 판정 직전/직후에 lease 발급을 중단·재개한다. 발급 성공과 Closing 성공이 동시에 성립하지 않는다.
- A/B가 같은 package를 사용한다. A 취소·Stop·강제 scope revoke와 B의 새 frame을 교차 실행하여 B의 lease·ReadyMask·결과가 유지되는지 확인한다.
- revoke와 성공 completion·GC cutoff·늦은 IO/Submit을 교차한다. revoked scope에는 stale 결과가 commit되지 않고, 이미 수락한 작업은 실제 완료 전 free되지 않는다.
- 마지막 lease/작업 종료 후 두 번의 무변경 full GC와 backend drain에서 package graph·scope root·resident retention이 기준선으로 복귀한다. 남은 retention은 owner와 원인을 출력한다.
- 기존 NL03/NL04·PI05와 asset/streaming integration fixture의 부속 사례로 추가한다. C24의 fake completion 통과와 실제 loader/RHI 검증 결과를 구분한다.

## 3. S1 — PIE 종료와 GPU 작업의 교차

**위치:** 전체 계획 §11.2 줄 885–893, §11.4 줄 915, §22.6 줄 1632–1649. 상세 계획 §14.2 줄 613–629, §26.2 줄 1232·1237.

**실행 순서:** PIE A의 render packet이 lease를 획득한 뒤 recording을 멈춘다. A의 Stop과 viewport close를 시작한다. 같은 immutable mesh를 사용하는 PIE B는 계속 실행한다. A의 recording이 늦게 끝나 Submit되고, final GC에서 추가 backend cleanup이 발생한다.

**PASS 근거:** Stop은 일반 admission을 닫지만 이미 수락한 recording을 해제하지 않는다. §11.2는 recording lease를 submission record로 원자 이관하고 late Submit의 last-use fence를 포함한다. §11.4는 mutable render scene/history/cache에 SessionId/WorldEpoch를 요구하므로 B의 데이터를 A와 함께 지우지 않는다. §22.6은 마지막 cleanup 이후 owner 기록이 닫혀야 Stopped로 전이한다. final GC가 추가 cleanup을 만들어도 scheduler·metadata가 먼저 사라지지 않는다.

**검증 조건:** PI03/05/06과 MT09에서 recording pause, 작은 frame/descriptor ring, 늦은 Submit, 반복 Stop을 조합한다. A의 fence 전 free 0, B의 영상/history 변화 0, A 종료 후 scope lease 0을 확인한다. 장치 손실로 완료를 확인하지 못하면 Stop 실패가 맞으며 Stopped로 기록하지 않는다.

**유보:** 실제 GPU·플랫폼 closure는 아직 실행되지 않았다. R2C01의 공유 AssetLease 보존 조건을 함께 보완해야 전체 경로가 닫힌다.

## 4. S3 — 장치 손실과 늦은 완료

**위치:** 전체 계획 §11.2 줄 866–893, §9.6 줄 746–750, §29.6 줄 2221–2223.

**실행 순서:** D1의 texture upload를 부분 제출한 뒤 device lost가 발생한다. D2가 생성되어 같은 slot·queue·fence value를 재사용한다. D1의 upload 완료 및 BuiltinReady callback이 뒤늦게 도착하고 D1의 자원을 Retire한다.

**PASS 근거:** fence는 숫자 단독이 아니라 device incarnation·queue·value의 튜플이다. resource·descriptor·packet·vendor context·callback에 동일 domain 검사를 요구한다. 부분 제출/미확인 완료는 기존 recovery record에 남으며 새 장치의 fence/pool로 해제하지 않는다. BuiltinReady도 새 generation의 upload 완료를 요구한다. bounded quarantine과 종료 실패 보고가 있으므로 안전을 위해 남긴 자원을 성공적인 누수 0으로 위장할 수 없다.

**검증 조건:** API04/BR04·NL05에 두 장치에서 동일 slot/value를 강제로 재사용하고 callback 순서를 뒤집는다. 구 callback의 새 BuiltinReady 변경 0, 구 자원의 새 pool 반환 0, quarantine 상한·retained reason·실패 상태를 확인한다. 실제 복구가 불가능한 backend에서는 제한된 종료 경로가 정상 정책일 수 있다.

## 5. S4 — CPU headless bake와 material 변경

**위치:** 전체 계획 §14.6 줄 1093–1129, §14.8 줄 1145–1153, §14.10 줄 1182–1207, §28.1–28.3 줄 2076–2119.

**실행 순서:** GPU 없는 commandlet에서 masked/emissive texture·material function을 포함한 BakeScene을 시작한다. 편집기는 원본 material revision을 변경한다. worker 하나가 실패하고 취소가 전달된 뒤 이전 worker 결과가 늦게 도착한다. 다른 fixture에는 CPU bake profile에서 지원하지 않는 Custom HLSL node를 포함한다.

**PASS 근거:** BakeMaterialProfile이 CPU 소비 target으로 정의되어 Shader IR을 GPU raster/readback에 의존해 평가하는 모순이 없다. 지원 불가 node는 승인 proxy 또는 위치가 있는 오류가 필요하다. BakeScene은 입력 revision·texture sampling·seed가 고정된 immutable snapshot이며 worker는 scene 주소를 보관하지 않는다. 현재 source와 다른 결과 및 취소된 부분 결과를 성공 asset으로 게시할 수 없다. CPU headless와 실제 GPU preview 검증도 구분한다.

**검증 조건:** LT01/05/06·MT08에 GPU device 생성 시 즉시 실패하는 host fixture를 사용한다. 정상 부분집합은 CPU만으로 완료하고 미지원 node는 명시 실패한다. 편집·취소·worker crash·에셋 너겟 손상 후 current lighting manifest가 바뀌지 않고, 임시 출력·module/resource lease가 drain 후 반환되는지 검사한다.

**유보:** CPU evaluator의 정확한 수치 오차·denoise 품질 기준은 구현 workload에서 확정한다. 이것은 소유권 모순으로 분류하지 않는다.

## 6. S5 — 플러그인 해제와 material/render cache

**위치:** 전체 계획 §14.6 줄 1094–1098·1123–1129, §14.8 줄 1147–1151, §11.2 줄 885–887, §27.10 줄 2064. 상세 계획 §4.2 줄 165–189·§14.2 줄 629.

**실행 순서:** plugin node lowering을 수행하는 compile worker를 멈춘다. 이전 성공 material의 PSO가 preview recording에 쓰이는 동안 plugin unregister를 시작한다. compile 취소 뒤 worker가 늦게 복귀하고 viewport도 닫힌다. DDC에는 해당 node/compiler 버전으로 만든 이전 artifact가 남아 있다.

**PASS 근거:** node descriptor 배열은 등록 시 소유 복사하고 lowering pointer는 ModuleLease로 보호한다. unregister는 compile admission 차단과 drain을 거치며, 공통 확장 계약은 instance/metadata/backend lease가 남으면 Busy로 DLL unload를 보류한다. runtime material에는 원본 node 구현을 포함하지 않으므로 검증된 shader bytecode만 남은 에셋 너겟/PSO cache를 곧바로 plugin code 사용으로 간주할 필요는 없다. 실제 함수 pointer나 plugin 소멸 연산을 보유한 cache entry는 공통 ModuleLease 계약의 사용자이며 lease 종료 전 unload할 수 없다. GPU packet이 보유한 PSO·vendor context는 recording/fence 규약을 따른다.

**검증 조건:** MT05/09/10·CT22/23에서 느린 lowering·cancel·unregister·preview close·늦은 Submit을 교차한다. 마지막 lowering/destructor 이전 code unload 0, Busy 중 신규 compile 거부, stale 결과 publish 0을 확인한다. bytecode-only cache와 plugin 함수 pointer를 가진 test cache를 별도로 만들어 후자만 unload를 지연하는지 검사한다. 무기한 cache pin을 정상 완료로 보고하지 않고 보유 owner를 출력한다.

**유보:** C27 dynamic loader는 선택 R3다. Foundation에서는 정적 module lease와 fake unregister로 계약을 검사하고, 실제 DLL unload 완료를 주장하지 않는다.

## 7. 상세 작업 데이터와의 대조

`detailed_task_data.json`의 C17(줄 310), C18(줄 328), C24(줄 436), C26(줄 472), C27(줄 490)을 확인하였다. C24는 fake backend completion·늦은/중복 completion·timeout·type lease drain을, C26은 CPU Foundation Gate를 맡는다. C27은 선택 후속으로 기록되어 있다.

상세 계획 §26.1–26.2(줄 1211–1237)는 실제 AssetRuntime·renderer·PIE·material을 상위 후속 Gate로 배치한다. 따라서 Core의 fake completion 결과를 PI/API/MT/LT의 실제 backend 통과로 잘못 합산하는 규정은 발견하지 않았다. R2C01은 §9.2의 계약 및 상위 asset integration fixture만 보완하면 되며 52개 Core/Input 작업을 늘릴 필요가 없다.

검토 과정에서 Task capture의 terminal 이후 release, TypeSchema descriptor의 깊은 복사, Input query scope, 초기 Sync 파일 배치는 다른 검토자의 담당임을 확인하였다. 이 보고서는 해당 결론을 중복 판정하지 않는다. 구현·sanitizer·실제 GPU 시험은 실행하지 않았으며, 유한한 문서 검토로 모든 오류나 누수의 부재를 증명하지 않는다.
