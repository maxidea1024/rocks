# Rocks Round 2 — Core/Object/Input를 소비하는 렌더 경계 교차 검증

검토일: 2026-11-10. 권위 원본은 `Docs/Design/EngineImplementationPlan.ko.md`, `Docs/Design/CoreObjectInputDetailedPlan.ko.md`, `Docs/Design/CoreObjectInputData.json`이다. 렌더·에디터 작업이 객체·schema·입력 결과를 소비하는 실행 순서로 검토했다. 원본은 수정하지 않았다. 이전 FR01–FR06 closure를 재판정하지 않았으며 신규 시스템·SDK 지원 범위를 추가하지 않았다.

**결과: P0 발견 없음, P1 계약 보완 후보 3건.** 병렬 GC+barrier, weak 승격+retire, constructor/commit/rollback, task 취소+root 해제, 입력 hotplug+PIE capture, schema 재등록/재로딩의 여섯 복합 반례를 구성했다. 아래 PASS는 문서가 해당 실행을 금지하거나 안전한 순서를 요구한다는 판정이다. 실제 코드·sanitizer·GPU 시험은 실행하지 않았다.

## 1. 신규 보완 후보

### R2-RC01 · P1 — task node 수명과 callable capture 해제 시점이 분리되지 않음

**위치.** 상세 §8.1 384줄: “node는 queue·실행·결과 observer의 모든 참조가 종료된 뒤 재사용한다.” 386줄: “기본 dependency failure는 body 미실행과 terminal 전파”. 전체 §8.1 585/599줄은 owning capture와 cooperative cancellation을 규정한다. 상세 §14.1 611줄은 최종 객체 회수에 task 완료와 강한 도달성 제거를 요구한다. 작업 JSON `tasks[ID=C17]` 310줄과 `C24` 436줄은 terminal/shutdown·destroy/task drain을 검증하지만 **완료 handle을 보관한 상태의 capture 해제**는 명시하지 않는다.

**실행 순서.**

1. 게임 lane이 render extraction을 위한 task에 객체 A의 RootRef와 모듈 lease를 capture한다. task body는 A를 직접 worker에서 수정하지 않고 이미 만든 값 데이터를 처리한다.
2. prerequisite가 실패하거나 실행 전 취소되어 body는 실행되지 않고 Cancelled를 게시한다. 다른 경우에는 body가 정상 반환한다.
3. 진단·DAG·완료 결과 조회자가 TaskHandle을 오래 보관한다. 문서대로 task node 재사용은 지연된다.
4. 단순한 구현이 node 안의 Function을 node destructor까지 유지하면 capture의 RootRef도 계속 남는다. PIE Stop의 task terminal drain은 끝났지만 A는 strong root 때문에 수거되지 않고 session 종료가 멈춘다.
5. 반대로 취소 요청을 terminal로 간주하여 callable을 즉시 파괴하면, 이미 실행 중인 body가 사용하는 capture나 모듈 코드가 사라질 수 있다.

**문제의 범위.** owning capture·node 재사용 금지는 필요하지만, 실행 payload와 결과 관찰용 node가 언제 각각 끝나는지 결정하지 않는다. 완료 TaskHandle을 보관하는 정상 사용이 task-owned Object graph 전체를 보존해서는 안 된다. 호출자가 의도적으로 result 안에 RootRef를 반환한 경우는 별도 소유권이며 이 지적에 포함하지 않는다.

**최소 수정.** task state machine에 실행 권한/실행 lease와 callable payload의 종료 지점을 명시한다. body가 반환했거나 queued task의 실행 불가가 선형화되고 실행 lease가 0일 때 callable/capture·불필요한 prerequisite hold를 정확히 한 번 해제한다. **초기 계약은 payload 해제 뒤 terminal을 release 게시하며 observer는 acquire로 관찰하는 방식으로 고정한다.** queue tombstone·TaskHandle·terminal/result record는 이후에도 유지될 수 있다. 실행 중 취소 요청은 terminal이 아니며 body의 안전한 종료를 기다린다. callable destructor에 필요한 ModuleLease는 destructor 반환까지 유지한다. OS IO·GPU·독립 callback이 사용하는 lease는 해당 owner로 명시 이관하고 task 취소만으로 해제하지 않는다. 기존 Submit/Then signature는 유지할 수 있다.

**수용 시험.** CT08/NL03와 C24 `TaskLifetimeInterleaving.cpp`에서 TaskHandle 1,000개를 의도적으로 보관한 채 successful/pre-run-cancel/dependency-failed task의 capture root 수가 payload drain 뒤 0이 되는지 확인한다. running body를 barrier에서 멈춘 후 cancel을 경합시켜 body 반환 전 capture 파괴가 0건인지 확인한다. callable destructor·terminal은 각각 1회, completed node는 observer 종료 전 재사용 0건이어야 한다. TestInlineExecutor와 1/N-worker executor에 같은 의미를 적용한다.

### R2-RC02 · P1 — TypeSchema/FieldInfo 데이터 소유권이 ModuleLease로 대체될 수 있음

**위치.** 상세 §9.1 426줄은 `RegisterObjectType(const TypeSchema&, RuntimeTypeOps)`이며 414줄의 schema에는 field/type metadata가 들어간다. 전체 §7.2 551줄은 `Span<const FieldInfo> fields`이다. 전체 §5.3 435줄과 상세 §14.1 611줄은 TypeInfo/module code lease를 유지한다. 그러나 등록 시 schema·field 배열·이름/속성의 저장소를 복사하는지, static storage 또는 별도 metadata owner를 요구하는지 정해지지 않았다. JSON `C07` 130줄은 등록 부분 실패·중복 TypeId·scan lease를, `C25`는 migration registry/code lease를 시험한다.

**실행 순서.**

1. 수동 등록 helper가 scratch arena의 field 배열을 참조하는 TypeSchema를 생성하고 RegisterObjectType에 전달한다. 함수 포인터 코드는 살아 있는 TestObjectModule 안에 있다.
2. 등록 성공 후 helper가 반환하거나 arena를 reset한다. module code는 ModuleLease로 계속 살아 있지만 schema 배열은 사라진다.
3. GC visitor 선택·Archive field lookup·에디터/render snapshot용 property 추출이 등록된 metadata를 읽는다. registry가 const 참조/Span을 그대로 보관한 구현이면 해제된 데이터를 읽는다.
4. 이후 같은 TypeId의 schema/processor 자료를 새 arena에 생성하면 이전 pointer가 유효한 새 데이터처럼 보일 수도 있다. code lease만 검사하면 schema revision 혼용을 검출하지 못한다.

**최소 수정.** 초기 RegisterObjectType은 기존 signature를 유지하되 **TypeSchema와 모든 field/name/attribute 저장소를 registration-owned immutable snapshot으로 deep copy한 뒤 등록 성공을 게시한다.** allocation/검증 실패는 부분 registration을 남기지 않는다. const 참조를 장기 보관하는 API로 구현하지 않는다. static generated table의 무복사 등록은 후속의 명시 MetadataLease overload로만 최적화한다. RuntimeTypeOps/code lease와 metadata storage lease를 구분하고 object·visitor·Archive reader·비동기 schema 소비자가 필요한 registration revision을 보존한다. 초기 native registration은 같은 TypeId의 비호환 active 재등록을 거부하고 기존 객체의 ops/table을 제자리 교체하지 않는다. native C++ hot reload를 구현하라는 요구가 아니며 기존 process-restart 기본값은 유지한다.

**수용 시험.** CT10/CT22의 manual 등록 fixture에 registration 직후 source arena reset/poison과 helper scope 종료를 추가한다. 초기 deep-copy 등록은 계속 올바르게 조회되어야 하고 N-fail의 각 부분 복사 실패 뒤 등록/할당 잔존은 0이어야 한다. 후속 borrowed-static overload를 추가하면 잘못된 lifetime 등록을 별도 계약 위반으로 검출한다. reader/visitor를 멈춘 채 unregister·새 revision 등록을 시도하여 Busy 또는 독립 revision lease를 확인한다. 마지막 destructor/visitor와 metadata reader 종료 전 old code/data의 반환 0건, 종료 후 잔존 registration/lease 0건을 검사한다. C07·C24·C25의 기존 파일에 추가하면 되며 새 대형 subsystem은 필요 없다.

### R2-RC03 · P1 — 입력 조회 API의 route/clock scope가 여러 PIE 세션을 주소화하지 못함

**위치.** 상세 §17.1 786줄은 InputSystem이 device·clock·queue·tick snapshot을 맡는다고 정의한다. §17.2 821–822줄은 `QueryAction(LocalUserId, ActionId)`와 `CopyActionEvents(LocalUserId, uint64 tick, Allocator&)`이다. 830줄은 반환 snapshot의 route/profile version과 UI 독립 pump를 규정한다. §18.1 863–867줄의 InputTick은 index·구간·route를 갖는다. 전체 §22.6 1634–1636줄은 session별 clock·입력 context·pause·focus token을 요구한다. **InputSystem instance가 단일 소비 scope에 묶이는지, 조회가 어느 route/clock epoch를 요구하는지**는 없다.

**실행 순서.**

1. 한 Host가 동일 LocalUser U와 물리 장치를 사용하면서 Editor preview, PIE A, PIE B를 관리한다. A는 paused tick 120, B는 tick 120에서 진행한다.
2. A에서 생성된 ActionEventBatch 소비 요청을 지연한다. focus/capture는 B로 전환되고 장치가 disconnect/reconnect하여 새 device generation을 받는다. event ingress의 route/generation 검사는 올바르게 수행된다.
3. B가 새 route에서 tick 120의 action을 생성한다. 또는 A를 재시작해 tick 번호를 재사용한다. UIRealTime pump도 gameplay pause와 독립적으로 결과를 생성한다.
4. 지연된 A 소비자가 `CopyActionEvents(U, 120, ...)`를 호출한다. 전역 InputSystem과 `(user,tick)` 저장 키를 사용하면 B/새 session의 결과를 선택하거나 A 결과를 덮어쓴다. QueryAction도 expected route/clock 없이 현재 다른 session 상태를 받을 수 있다.
5. ingress에서 구 event를 차단하는 것과 이미 정상 생성된 여러 소비 stream의 조회를 격리하는 것은 별개다. 반환 값의 route version만으로 잘못된 결과를 사후 폐기하면 오입력은 줄일 수 있지만 필요한 A의 terminal cancel/event batch를 주소화할 수 없다.

**최소 수정.** 초기 API는 Query/Copy에 불투명 InputConsumerScope와 sample domain/clock epoch를 명시 전달하는 방식으로 고정한다. 예: `QueryAction(InputConsumerScope, LocalUserId, ActionId)` 및 `CopyActionEvents(InputConsumerScope, LocalUserId, InputSampleKey, Allocator&)`. InputSampleKey는 update domain·clock epoch·tick/sequence를 담고, scope는 Host가 route/session identity를 연결한 id+generation이다. AdvanceInput에도 같은 scope/key를 적용한다. InputCore는 World 타입을 include하지 않는다. event history key는 최소 consumer scope+user+update domain+clock epoch+tick/sequence를 구분한다. UIRealTime 결과는 gameplay tick과 같은 숫자 키로 합치지 않는다. 종료 scope의 조회는 ScopeExpired/HistoryExpired를 반환하고 새 scope의 결과를 대신 돌려주지 않는다. 공유 물리 DeviceHub와 각 action 소비 scope의 수명을 구분한다. 후속 편의 API는 이 scope에 bind한 reader facade로 감쌀 수 있다.

**수용 시험.** IT03/IT19와 I04/I06/I15에 동일 LocalUser·동일 tick 번호·다른 clock/route의 Editor+A+B fake fixture를 추가한다. A pause, B 진행, A Stop/restart, capture 전환, delayed query, hotplug를 조합한다. 잘못된 action 반환·구 terminal cancel 누락·타 scope history 덮어쓰기 0건 또는 명시 expired 오류를 요구한다. UI pump가 pause 중 계속되는 것도 확인한다. 실제 콘솔 SDK 없이 synthetic Host scope로 먼저 검증할 수 있다.

## 2. 여섯 복합 반례의 검증 기록

### CE01 — 병렬 mark + 구조 변경 barrier + queue 포화 + 렌더 snapshot

**실행.** root owner A의 GcArray를 두 marker가 분할 방문한다. 첫 slice 후 mutator가 재개되어 아직 방문하지 않은 edge를 swap-remove로 cursor 앞쪽으로 옮기고 새 target C를 삽입한다. barrier queue 용량도 포화시킨다. 렌더 worker는 이전 값 snapshot을 계속 처리한다. 다음 safepoint에서 marker가 재개되고 cutoff를 시도한다.

**판정: 문서상 차단.** 상세 §13.3 593–597줄은 cursor에 owner/FieldId/mutation version/index만 보관하고 구조 변경 시 dirty rescan을 강제한다. 이미 marked owner라도 rescan을 버리지 않는다. §13.2 583/589줄은 신규 edge shade 및 spill/admission으로 무손실 barrier를 요구한다. 전체 §6.3 486줄과 상세 §8.2 390줄은 renderer가 Object 필드를 직접 읽는 것을 허용하지 않는다. 따라서 재배치된 edge 누락과 renderer/GC 필드 data race를 정상 구현으로 인정할 수 없다.

**시험 가능성.** C21/C22와 CT18/19에 이미 allocation/barrier 실패·rescan·cutoff 강제 schedule이 연결되어 있다. 구현 시 mark test-and-set 직후 enqueue 실패를 negative control로 넣고, outstanding work/dirty rescan이 남은 cutoff를 실패로 판정해야 한다. 원자 mark flag만 세운 뒤 outgoing work를 잃는 구현은 기존 ‘barrier 유실 없음’ 계약 위반이다. 새 계약 누락으로 중복 집계하지 않는다.

### CE02 — weak 승격 + 명시 destroy + 마지막 root 해제 + backend retire

**실행.** W가 weak A의 TryPin 검사를 시작한다. Game lane이 RequestDestroy 또는 GC cutoff를 시도한다. pin 성공 뒤 다른 owner가 마지막 RootRef를 해제하고 렌더 쪽 old snapshot/fence는 아직 남아 있다. 동일 slot을 재사용하려 한다.

**판정: 문서상 차단.** 상세 §11.1 508/518줄, 전체 §6.5 502–506줄은 검사·root 등록·cutoff를 같은 gate로 선형화한다. pin이 먼저 승인되면 storage root가 남지만 destroy 이후 Resolve는 실패하고, cutoff가 먼저이면 신규 pin 자체가 실패한다. backend hold는 GC self-root가 아니며 상세 §14.1 611줄의 논리곱으로 storage 반환을 늦춘다. slot generation 재사용/격리는 상세 §10.2 491줄에 있다.

**시험 가능성.** C09/C12/C24, CT14/20/21의 강제 pause 지점을 pin 검사 이전/registry insert 이전/cutoff publish 직후/root Reset/old backend ACK 사이로 분리하면 된다. old render packet은 native resource lease만 갖고 관리 Object 주소를 사용하지 않아야 한다. RootRef의 존재를 논리적 호출 권한으로 오인하는 구현을 negative control로 둔다.

### CE03 — constructor/private root + 단계별 강제 GC + commit 뒤 integration 실패

**실행.** graph A/B를 reserve한다. A 생성 중 메모리 부족/GC 요청이 발생하고, 완전히 생성된 B를 defaults/remap 전에 보존한다. 단계 경계에서 강제 GC를 수행한다. Initialize 실패 경로와 Commit 성공 후 Register 실패 경로를 각각 실행하고, 지연 cleanup을 겹친다.

**판정: 문서상 차단.** 상세 §12.1 548–552줄은 ctor no-safepoint, 부분 객체 visitor 금지, 완성 구성원·dependency private root, precommit 가역 journal, postcommit 정상 RequestDestroy/retire를 구분한다. §12.2 565줄은 Commit 실패 시 caller context에 ownership이 남도록 한다. 전체 §5.2/§5.4의 private root·release/acquire publication이 같은 방향이다.

**시험 가능성.** C10 CT15의 생성 각 단계 N-fail/GC fixture와 C24 정상 retire를 함께 사용한다. pending GC는 fully-constructed member의 private-root 등록이 완료될 때까지 안전 경계 밖에서 실행되지 않게 해야 한다. 이 등록 handoff를 포함한 ctor no-safepoint guard 경계는 구현 리뷰에서 확인한다. 상세 §12.1 552줄은 postcommit 실패 시험을 명시하지만 CT15 표 743줄의 짧은 oracle은 precommit “publish 0건” 중심이므로 보고서는 `precommit`과 `published-integration` 하위 사례를 분리해야 한다. 이는 이미 본문에 요구된 시험의 표현 정리이며 신규 기능 요구는 아니다.

### CE04 — task 취소 + 완료 observer 유지 + GC/root 해제

**실행·판정.** R2-RC01의 두 분기(pre-run cancel/실행 중 cancel)를 사용한다. node 재사용 방지는 충분하지만 callable payload 종료 지점은 불명확하여 **수정 필요**다. C17/CT08과 C24/NL03에 completed handle 보관 fixture를 추가해야 결과가 판정 가능하다.

### CE05 — hotplug + PIE capture 전환 + 지연 text/action + 독립 clock

**실행.** A PIE에서 key-down과 IME composition을 시작하고 action query를 지연한다. 같은 창에서 B로 focus/capture를 전환한다. 장치를 재연결하고 A를 Stop/restart한 뒤 old text commit·old key-up·A의 지연 query를 전달한다.

**판정.** 상세 §18.1 874줄의 route admission/TextSession generation, §18.3 890줄의 stream epoch·lossVersion·watermark, §19.3 924줄의 capture token, §20.2 942줄의 reconnect require-release는 **event ingress의 반례를 차단**한다. old text를 새 widget에 commit하거나 old key-up을 새 gameplay release로 해석하는 구현은 허용되지 않는다. 다만 정상 생성된 action batch의 소비 scope를 query가 구분하는지는 R2-RC03의 **수정 필요** 항목이다. provider capability가 없는 transition을 복원했다고 주장하지 않는 규약은 유지된다.

**시험 가능성.** I02/I03/I06/I15와 IT04/05/19의 기존 fake provider에 독립 scope/clock을 추가한다. 물리 device generation, routing generation, TextSession generation, clock epoch를 서로 다른 값으로 유지하여 하나의 generation으로 모든 경계를 통과시키는 구현을 검출한다.

### CE06 — schema 생성/등록 + old consumer + 재등록/코드 재로딩

**실행.** manual/generated schema를 등록하고 Archive/GC/property reader가 그 revision을 사용한다. source scratch storage를 reset하고 새 schema revision을 준비한다. old reader가 남은 상태에서 unregister/재등록·script reload·native module 해제를 시도한다.

**판정.** 전체 §7.2 564줄의 native process restart 기본값, 상세 §14.2 629줄의 task/callback/object/archetype/TypeInfo/script-frame lease drain은 **native code의 조기 unload를 차단**한다. 전체 §23의 script reload revision lease와 coroutine root map 규약도 native layout 무조건 교체를 허용하지 않는다. 그러나 source metadata 저장소 자체는 R2-RC02의 **수정 필요** 항목이다. 모듈 코드가 살아 있다는 사실을 데이터 arena 수명의 증거로 사용하면 안 된다.

**시험 가능성.** C07의 duplicate registration/부분 실패, C20의 수동/생성 schema 대조, C24/CT22의 unregister Busy, C25의 migration/stale publish에 같은 source-poison fixture를 연결한다. 동적 native loader 구현 없이 static module lease fake로 이 계약을 검증할 수 있다.

## 3. 우선순위와 다음 검증

1. R2-RC01은 C06 TestInlineExecutor 계약과 C17/C24 실제 scheduler 수명에 함께 반영한다. 작업 구현 후 terminal 수만 세는 검증으로 끝내지 않는다.
2. R2-RC02는 R1 C07 등록 API부터 정한다. 이 경계가 불명확하면 수동 타입을 먼저 만드는 Core-first 순서에서 이미 dangling metadata가 가능하다.
3. R2-RC03은 I0의 action/history scope를 먼저 고정하고 I2 PIE/VM bridge가 그 계약을 사용하도록 한다. InputCore에 World·Widget 의존성을 추가할 필요가 없다.

새 작업 ID·전문 기능·콘솔 SDK 요구는 필요 없다. 기존 작업 파일과 CT/IT fixture에 위 경계를 추가하면 된다. 현재 소스가 구현 계획이므로 이 검토는 실제 C++ memory-order·data race 부재, backend 완료, sanitizer 결과를 증명하지 않는다. Round 3에서는 세 수정의 API/수명 연결과 여섯 실행 순서가 서로 충돌하지 않는지만 재확인하면 된다.
