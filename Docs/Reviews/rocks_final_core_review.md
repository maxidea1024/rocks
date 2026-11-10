# Rocks 통합 계획 최종 Core 교차 검토

검토일: 2026-11-10. 대상: `Docs/Design/EngineImplementationPlan.ko.md` 34장, `Docs/Design/CoreObjectInputDetailedPlan.ko.md` 26장, `Docs/Design/CoreObjectInputData.json`. 원문은 수정하지 않았다. 아래 행 번호는 검토 snapshot 기준이며 후속 편집 시 달라질 수 있다.

**판정: 문서 간 계약 불일치 4건 수정 필요.** 구현 코드의 결함이나 실행 시험 실패를 주장하는 결과가 아니다. 객체 조회·생성 실패·종료 순서·검증 ID에서 같은 구현 담당자가 서로 다른 계약을 선택할 수 있는 반례를 확인하였다. P0는 확인되지 않았다.

## 1. 필수 정합성 수정

### FC01 · P1 · 객체 Resolve의 명시 접근 Context 누락

- **위치·근거:** master §5.1, 383행은 `Borrow<T> Resolve(ObjectHandle, GcReadScope&)`이다. 상세 §10.2, 449–451행은 `Result<Borrow<T>> Resolve(ObjectHandle, GcReadScope&, const ObjectAccessContext&)`이며, 상세 §11.3, 499행은 current World의 암묵 조회를 금지한다. master §5.3, 409행과 §6.5, 474행도 WorldId/Epoch 검사를 요구한다.
- **반례:** PIE A의 여전히 Published인 handle을 PIE B의 코드가 보관한다. master 예제의 두 인자만 받은 Registry는 호출자가 B인지, 의도된 Unscoped 접근인지 구분할 입력이 없다. generation·type·상태가 정상인 A 객체를 resolve하면 §22.6의 session 격리를 위반한다. GcReadScope가 회수 지연 장치라는 현재 계약만으로 Context 권한을 대체할 수 없다.
- **최소 수정:** master 예제를 상세 signature와 동일하게 변경한다. Unscoped 접근도 명시 Context 값으로 전달하며 scoped 객체 검사를 우회하지 못한다. Resolve/TryPin/신규 edge 설정·backend 결과 commit의 공통 availability 판정을 연결한다. 오류/빈 결과의 표현도 Result로 통일한다.
- **Acceptance:** CT13/14와 PI03에 A/B 두 context·서로 다른 epoch·Unscoped 조합을 추가한다. cross-session 및 stale epoch 조회·pin·store의 거부와 기존 owner 값 보존을 확인한다. Context 없는 조회 overload는 compile fixture에서 제공되지 않아야 한다.

### FC02 · P1 · publication 이후 등록 실패의 rollback 오해

- **위치·근거:** master §5.3, 411행은 Initialize/Register 실패를 모두 역순 rollback으로 서술한다. 413행의 공통 instancing 경로에는 publication 경계 없이 Register→BeginPlay가 연결되고, 생성 실패를 미공개 graph 전체 정리로 표현한다. 상세 §12.1, 513–521행은 publication 전 journal rollback과 publication 후 정상 RequestDestroy/retire를 구분한다. master §20.2, 1338행의 Register rollback 문장도 동일 경계가 필요하다.
- **반례:** Register가 backend에 instance를 게시하고 callback context를 등록한 뒤 두 번째 등록 단계에서 실패한다. 이 시점을 미공개 생성 실패로 분류하여 graph storage를 역순 해제하면, 이미 승인된 callback/backend completion이 반환 시 해제된 객체·등록 상태에 접근한다. 반대로 강한 root만 보존한 채 rollback을 포기하면 종료 회계가 닫히지 않는다.
- **최소 수정:** master 공통 경로에 AtomicGraphPublication을 명시한다. Initialize는 context journal의 가역 값/자원만 준비하고 외부 공개 등록은 publication 후 수행한다. 이후 실패는 PublishedGraph owner가 논리 admission 차단·정상 retire·completion drain을 수행한다. Register 내부의 부분 등록 취소 자체는 허용하되, 이를 graph storage의 즉시 미공개 rollback과 구분한다.
- **Acceptance:** CT15·NL01/03에 publication 직전 실패, 첫 외부 등록 성공 후 실패, 늦은/중복 callback을 각각 주입한다. 전자는 외부 노출 0건과 역순 journal 정리, 후자는 public Resolve 차단·모든 lease 종료 전 storage 보존·terminal 1회·최종 owner/heap 0을 검증한다.

### FC03 · P1 · 전체 종료 요약의 final GC·추가 cleanup 순서 누락

- **위치·근거:** master §3.2, 226행은 backend 완료→scheduler 종료→모듈 역순 종료다. §28.3, 1972행도 backend 완료 후 output commit·process exit로 이어진다. 상세 §14.2, 587–598행과 master PIE §22.6, 1520행은 root 해제·final GC가 생성하는 추가 cleanup을 다시 drain한 후 scheduler/metadata를 반환하도록 명시한다.
- **반례:** module/ClassDefaults가 마지막 root를 보유한 객체는 첫 backend drain 때 살아 있다. master §3.2 순서대로 scheduler를 join한 뒤 module 종료에서 root를 해제하면 final GC의 BeginDestroy가 새 backend 해제를 요청한다. 완료 task를 실행할 scheduler가 이미 없으므로 정상 종료가 교착·누수로 끝나거나 강제 해제로 UAF가 발생한다.
- **최소 수정:** 모든 종료 요약을 하나의 규약으로 연결한다. 일반 admission 차단→논리 종료/취소→첫 drain→session/module 소유 root 해제→final GC→새 cleanup/backend 완료 drain→runtime use lease·retire·reader closure→객체/타입 코드 반환→IO/Task join→trace writer drain/join→allocator/platform 종료다. closure의 대상은 객체 사용·task/backend/reader lease이며, 아직 마지막 정리에 필요한 registry/type-table/writer의 소유 lease를 먼저 0으로 요구하지 않는다. 이 infrastructure owner는 마지막 사용자 종료 후 순서대로 해제한다.
- **Acceptance:** CT22·NL06·PI05/06에 첫 drain 이후 마지막 root가 해제되는 객체, BeginDestroy가 지연 backend completion을 만드는 객체, 마지막 destructor가 module code를 사용하는 객체를 둔다. 종료 전후 root/task/retire/backend/metadata 회계를 기록한다. 정상 종료는 closure 후에만 성공, timeout은 미완료 사유를 가진 실패이며 강제 free로 숫자를 0으로 만들지 않아야 한다.

### FC04 · P1 · 같은 Foundation Gate ID의 서로 다른 의미

- **위치·근거:** master §1.2, 52–63행과 상세 §16.1, 679–690행에서 CF01–CF12를 재사용하지만 의미가 다르다. 예: master CF05는 Task, 상세 CF05는 schema/Archive다. master CF09는 기본값/복제, 상세 CF09는 파괴/backend 지연이다. master CF11은 진단, 상세 CF11은 확장/headless다. JSON의 C26은 구분 없이 CF01–12를 완료 산출물로 사용한다.
- **반례:** 담당자가 상세 CF11의 새 타입/allocator·headless 검사를 통과하고 공통 추적표 CF11에 PASS를 등록한다. master 검토자는 이를 CPU/Task/GC/memory 진단·root chain·crash report 통과로 해석한다. 실제로는 해당 관측 시험이 미실행인데 Foundation 완료로 집계할 수 있다. 같은 문제가 Archive·Task·clone·shutdown에 반복된다.
- **최소 수정:** master CF 번호를 정본으로 유지하고 상세 표를 같은 의미로 재정렬한 뒤 CT 테스트 묶음을 매핑한다. 또는 상세를 DCF처럼 별도 namespace로 변경하고 양방향 매핑표를 제공한다. 전자가 기존 C26/전체 추적표 변경량이 적다. 문서마다 의미가 다른 ID를 설명 문장만으로 구분하는 상태는 유지하지 않는다.
- **Acceptance:** 하나의 Gate registry에서 ID→의미→CT/NL/OB/HT 실행 항목을 생성한다. master·상세·JSON/CSV의 CF 정의가 동일한지 검사하고, 모든 필수 하위 시험의 PASS/FAIL/NOT RUN을 집계한다. CF11 진단 시험 하나를 의도적으로 NOT RUN으로 두었을 때 Foundation 전체 완료를 거부해야 한다.

## 2. 단계 표기의 보완

### FC05 · P2 · Foundation 순서 표와 선택 최적화의 관계

master §1.1, 36–44행은 F1의 page/slab/pool/arena와 F2/F3의 전체 library·Archive·worker/IO를 F4 객체보다 앞선 순서·선행 조건으로 표시한다. master §4.1, 293행도 slab/thread cache를 기본 구성으로 단정한다. 상세 R0/R1은 simple heap·최소 Array·inline executor로 객체를 먼저 완성하고 slab/thread cache는 C28 선택 R3다.

이 차이는 memory safety 결함으로 판정하지 않는다. 다만 구현 담당자가 master 표를 순차 필수 gate로 해석하면 최소 객체 vertical slice 이전에 선택 allocator/Task 최적화를 완료해야 하는 것으로 읽힌다. 사용자의 Core/Object 최우선·안정성 우선과 상세의 실행 순서가 달라진다.

**최소 수정:** F 표를 최종 Foundation 책임 묶음으로 명명하고 실제 착수 순서는 상세 R0→R1→R2, C28 최적화는 선택임을 명시하거나, 표 자체를 해당 순서로 정리한다. §4.1의 slab/thread cache는 후속 최적화안으로 한정한다. **Acceptance:** C07–C12의 선행 dependency closure에 C13–C20/C28이 들어가지 않음을 JSON DAG로 검사하고, minimal runner가 full math/IO/worker 구현 없이 R1 fixture를 구성하도록 계획한다.

## 3. 정합성이 유지된 항목

| 검토 항목 | 확인 결과·한계 |
|---|---|
| GC cutoff/epoch | master §6.2/6.5와 상세 §13.2가 acquisition gate·단일 분류 cycle·bitmap 재사용 금지·born epoch·retire native retention에 일치한다. 기존 RootRef/edge의 저장소 보존과 신규 승격을 구분한다. |
| edge relocation | master §6.5와 상세 §11.2/13.3에 same-owner noexcept 이전, 다른 owner TrySet, mutation version/rescan이 존재한다. raw element pointer를 slice에 보존하지 않는 계약도 일치한다. |
| Borrow·명시 destroy | safepoint/yield/frame/async 경계 금지, admission 차단 후 teardown hook, managed 이웃에 의존하지 않는 BeginDestroy가 일치한다. Resolve의 공개 예제만 FC01로 정정이 필요하다. |
| 모듈 방향 | Base→PlatformBase→Core 분리, ObjectRuntime의 Asset/World/VM 역의존 금지, ObjectSerialization bridge가 일치한다. 진단 provider는 Host가 주입하며 Core→viewer 역의존을 요구하지 않는다. |
| 수학 | master §3.3·상세 §7.4·C15는 검증된 backend 채택, Rocks API, f32/f64 구분, 자체 ScalarMath 구현 제외에 일치한다. DirectXMath 품질 배제와 무비용 wrapper 보장을 하지 않는다. 후보 benchmark는 미실행 상태다. |
| 시간·병렬화 | Core 시간 4개 계획 파일, owner commit·immutable TickContext, 고정 h·scaled accumulator, pause 중 unscaled pump, exact-one fixed single-step, 직렬/병렬 대조가 연결된다. clock 제어 명령은 paused 상태에도 처리되는 boundary로 구체 구현해야 한다. |
| 관측·하네스 | C06 초기 log/scope, R1 객체·GC ledger, C18 수집/분석/viewer, C26 필수 보고서, C30 선택 미세 최적화가 일치한다. 모든 52개 task에 장애주입·동시성·완료증거 열이 존재한다. |
| 누수 기준 | master §29.6·상세 §6.3은 leak/UAF/race를 성능으로 상쇄하지 않으며 정상 closure·shared baseline·SDK 관찰 범위·crash incomplete를 구분한다. 유한한 시험을 모든 환경의 누수 부재 증명으로 표현하지 않는다. |
| PIE | private VM/clock/input/network scope, immutable asset revision 공유, 명시 simulation-change transaction, 2개 이상 session, 비동기 Stop·최종 GC 후 추가 cleanup·1,000회 시험이 존재한다. 별도 구조 확대는 필요하지 않다. |
| Rocks 명명 | 검토 대상 본문의 내부 타입에서 UObject/Actor/SceneComponent/IAllocator/IInputBackend를 새 API로 노출한 잔존 사례는 확인되지 않았다. Unreal·foreign API의 고유 명칭과 테스트 ID의 I/CF 접두사는 내부 타입 분류 접두사와 구분하였다. |

## 4. 계획 파일 수·산출물·링크

검토 snapshot의 JSON은 task 52개, 예정 파일 293개다. 상세 9행은 미구현·미측정, 128행은 아직 작성되지 않은 제안 파일, 1202행은 경로가 생성 예정이며 CSV를 자동 완료로 보지 않는다고 명시한다. 따라서 **293개 엔진 소스가 생성되었다는 표현은 확인되지 않았다.** 최근 요청에 따른 .editorconfig/.clang-format/CodingStyle 계획 경로가 추가되면 생성기 집계가 증가하는 것은 정상이며 293을 고정 사양으로 사용하지 않는다.

현재 새 CSV는 다음 명칭과 일치한다.

- `261110_Rocks_Core_Object_Input_Task_Backlog_KO_v1_1.csv`
- `261110_Rocks_Core_Object_Input_File_Map_KO_v1_1.csv`

검토 대상 Markdown/JSON에는 구 CSV명·`outputs/...` 상대 파일 링크가 없었다. 새 Rocks HTML 2개에서도 구 CSV 링크는 확인되지 않았다. 다만 **기존 배포 helper와 ZIP은 이전 산출물**이다. `work/check_detailed_delivery.py` 7·21–23·44·53행은 v1_0·218개·구 CSV명·구 master를 사용하며, `outputs/261110_Engine_Core_Object_Input_Plans_KO.zip`도 이전 배포물이다. 이를 새 Rocks bundle의 검증/패키징에 그대로 재사용하면 stale 파일 전달 또는 assertion 실패가 발생한다.

**배포 Acceptance:** 최종 배포 manifest는 새 Rocks master/detail/CSV명만 명시하고 파일 수를 현재 JSON에서 계산한다. ZIP을 새로 만들 경우 entry 목록·각 파일 hash·본문 요구 개수·상대 링크 존재 여부를 검사한다. 보존된 이전 버전은 새 최종 다운로드 링크로 사용하지 않는다. 사용자에게 전달하는 로컬 링크는 절대 outputs 경로를 사용한다.

## 5. 수정 후 1차 종료 확인

FC01–05의 지적 반례만 재확인하였다. 새 전체 검토를 시작하지 않았다.

| 항목 | 수정 확인 | 종료 판정 |
|---|---|---|
| FC01 | master §5.1 Resolve가 Result+명시 const ObjectAccessContext로 변경되어 상세와 일치 | 해결 |
| FC02 | master §5.3에 pre-commit Initialize→Commit→Register, 공개 이후 RequestDestroy/정상 retire가 명시됨 | 핵심 해결. §20.2의 ‘실패한 Register는 역순 rollback’ 잔존 문장에 같은 경계 연결 필요 |
| FC03 | master §3.2가 final GC→추가 cleanup drain→closure→scheduler 종료로 변경됨 | 핵심 해결. §28.3의 backend 완료→output→exit 요약에 §3.2 공통 종료 계약 참조 필요 |
| FC04 | master CF 표를 정본으로 선언하고 상세 §16.1을 같은 의미의 CT mapping으로 재정렬 | 표 해결. 상세 §7.3 Transform 시험의 CF03/CT06 참조를 CF04/CT06으로 정정 필요 |
| FC05 | F 표를 책임 묶음으로 바꾸고 실제 R0→R1→R2·C28 선택을 명시 | 해결 |

추가 요청의 계획 반영도 확인하였다. 최신 JSON은 52 task·297개 고유 예정 경로이며 Core 시간 경로 4개를 유지한다. C00에 .editorconfig·.clang-format·CodingStyle·FormatAudit 4개가 추가되었다. 두 Markdown의 C++ block 합계는 37개이고 실제 tab 문자는 0개다. 상세 §4.3은 ‘사람이 알아보기 좋은 코드 스타일 유지’, 의미 있는 빈 줄, spaces 4와 formatter/리뷰의 역할을 명시한다. 이 결과는 문서·계획 데이터 확인이며 엔진 코드 생성 또는 C++ 컴파일 통과 의미가 아니다.

## 6. 최종 종료 판정

**FC01–FC05 모두 해결 — 문서 계약 closure PASS.** 1차 재확인에서 남은 문장 3곳을 현재 파일에서 다시 확인하였다.

- master §20.2, 1456행: Register 실패는 backend 부분 등록 해제와 Commit된 객체의 RequestDestroy/정상 retire로 구분하며 §5.3을 참조한다.
- master §28.3, 2113행: root 반환·최종 GC·추가 cleanup/backend drain·owner closure 이후 scheduler/type/writer 종료를 명시하고 §3.2의 반복 drain·수명 종료 규약을 적용한다.
- 상세 §7.3, 333행: Transform 검증 참조가 정본 의미에 맞는 CF04/CT06으로 변경되었다.

이 수정 범위에서 지적한 반례를 다시 허용하는 새 모순은 확인되지 않았다. 새 전체 검토나 원본 변경은 수행하지 않았다. PASS는 문서 계약의 정합성 판정이며 엔진 구현·unit/stress·실장비·성능 시험의 통과를 의미하지 않는다.

EOD
