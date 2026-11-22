# Sparks 핫 리로드(LC7)

이 문서는 Sparks 언어 코어의 LC7(핫 리로드)을 정한다. 언어 의미는 SparksLanguage(SL) §12.4가 정본이고, 이 문서는 그 중 언어·VM 쪽(R1 본문 교체, R2 제자리 migration의 Sparks 필드·`on`·const, `ReloadExecutions`, 계획·판정·원자적 적용, 증분 재컴파일)을 VM session에서 어떻게 실현하는지 정한다. R3(instance 재생성)·R4(복제 schema)는 엔진 연동(L2·L3)이고, 사전·widget schema의 R2는 EI5다. 실행 모델은 SparksRuntime(LC6) 위에 쌓는다.

- 개정 1(2026-11-22): 첫 판. 하위 단계 LC7a–LC7c, 결정 K1–K10.
- 개정 2(2026-11-22): LC7a 구현(4절). K4의 타입 서술자를 정규 타입 표기로(언어 함수 서술자는 객체 타입이 모두 `x`라 타입 변경을 가르지 못한다), K6 Cancel이 앞서 Keep으로 남은 더 오래된 revision의 실행에도 적용, `.run` 결과에 `previous` 더함.

## 1. 하위 단계

| 단계 | 내용 | 완료 기준 |
|---|---|---|
| LC7a revision·R1 | bytecode 형식 v5(CLAS 필드 타입 서술자·`@formerly` 옛 이름), 새 revision 링크와 `VmSession::PlanReload`·`ApplyReload`(같은 배치 — 본문만 바뀜), instance·연결의 revision 교체, `ReloadExecutions`(Keep·Cancel — 본문이 바뀐 함수를 frame에 가진 실행), 모듈별 살아 있는 revision 8, 취소된 Start 재실행, Tick 중지 해제 | `.run` `reload` golden(R1 행) — 완료(개정 2, 4절) |
| LC7b R2 migration | 필드 대응(이름·옛 이름·타입 서술자), 새 기본값은 쓰지 않은 필드에만(필드별 쓰기 bit), 유지 자원·Buf 필드의 보존·해제, 삭제·변경 필드를 참조하는 실행 취소, 구독 연결 재구성(순서 유지 — 새 `on`은 끝), 판정 실패(클래스 삭제·기반 변경 = R3 필요) | `.run` `reload` golden(R2 행), 할당 실패 전수 |
| LC7c Gate·측정 | Gate G5의 R1·R2 행(대기 실행·유지 자원·연결이 있는 상태의 reload와 롤백 주입), 본문 reload 2 s 측정(컴파일 → 링크 → 적용), 증분 재컴파일(바뀐 모듈과 의존 모듈만) | G5 R1·R2 통과, 측정 시험 |

## 2. 구조

- **revision(K1):** 새 revision은 새 `VmLinkedModule` 객체다(같은 객체를 다시 링크하지 않는다 — 그러면 Keep 실행이 옛 코드를 잃는다). host는 session이 옛 revision을 참조하는 동안(`VmSession::References(linked)` — instance·정지 실행·연결) 그 객체와 bytecode를 보관한다. 모듈별 살아 있는 revision은 8개까지이고(SL §12.4), 넘으면 가장 오래된 revision을 frame에 가진 실행을 취소하고 보고한다.
- **계획(PlanReload, K2):** `PlanReload(old, fresh, plan)`은 아무것도 바꾸지 않고 계산한다: 클래스 대응(이름 — 옛 클래스가 새 revision에 없거나 기반 클래스가 바뀌었는데 그 클래스의 instance가 있으면 R3가 필요하므로 판정 실패), 필드 대응(아래), handler 대응(함수 이름 — `Door.on Owner.Damaged`), 함수 대응(이름 — 본문 hash 비교), 대기 실행별 처리(유지·취소·Start 재실행). 할당 실패 외의 판정 실패는 이유를 담아 돌려주고 session은 그대로다(SL §12.4 all-or-nothing).
- **적용(ApplyReload, K3):** 실행 중이 아닌 reload 경계에서만(`InvalidArgument`). 순서: (1) 취소할 실행 취소 → (2) instance마다 새 배치 저장소를 만들고(새 `$init`을 Changed 억제로 돌려 기본값) 대응 필드의 옛 값을 옮김(쓰지 않은 필드는 새 기본값 — K5) → (3) 삭제·형식이 바뀐 필드의 유지 자원·Buf 해제 → (4) instance의 module·class 번호 교체 → (5) 연결의 module·handler 번호 교체(삭제된 handler의 연결은 지운다 — 순서 유지) → (6) 활성 instance에 새 handler 연결을 끝에 더함 → (7) session 모듈 목록 교체, Tick 중지 해제(그 모듈의 새 revision) → (8) 취소된 Start 재실행. 적용은 실패하지 않게 할당을 (2) 앞에서 모두 마친다(계획이 저장소 크기를 안다) — 할당 실패면 아무것도 바꾸지 않고 `OutOfMemory`. instance 하나의 `$init` fault는 그 instance만 격리한다(SL §12.4 4번).
- **필드 대응(K4):** 새 클래스의 필드마다 옛 클래스(같은 이름의 클래스, 기반 사슬 포함)에서 같은 이름, 없으면 새 필드의 `@formerly` 옛 이름과 같은 필드를 찾는다. 타입 표기(CLAS 형식 v5 — 정규 표시 이름, 다른 모듈 선언은 `<경로>:` 접두: `int`·`string`·`Door?`·`Array<int>`·`SoundCue`)가 같으면 값을 옮긴다(Buf는 참조를 옮긴다, 유지 자원은 그대로). 다르면 새 기본값을 두고 "손실" 보고다(무손실 변환 §15는 저장 시스템과 함께 — EI). 대응이 없는 옛 필드는 삭제: 유지 자원은 멈추고 Buf는 놓는다. FieldId(ID manifest)는 컴파일러가 이름 키로 풀어 `@formerly`와 같은 결과가 되므로(SparksBytecode 6.1) VM은 이름과 옛 이름으로 충분하다.
- **쓰기 bit(K5):** instance마다 slot별 "spawn 뒤 쓰기 있음" bit를 둔다(`NoteWrite`가 표시 — `$init`·Awake·Enable 억제 문맥의 쓰기는 표시하지 않는다). 새 기본값은 이 bit가 없는 필드에만 반영하고, 있는 필드는 옛 값을 유지한다. prefab·instance override는 엔진 쪽(EI)이다.
- **실행(K6 — SL §12.4 SD30):** `ReloadExecutions` Keep이면 정지 실행은 옛 revision으로 끝까지 간다(옛 frame의 link가 옛 객체를 가리킨다). Cancel이면 새 revision과 본문이 다른(또는 삭제된) 함수를 frame에 가진 실행을 취소한다 — 바로 앞 revision의 frame은 계획의 대응표로, 앞선 Keep으로 남은 더 오래된 revision의 frame은 새 revision의 같은 이름 함수와 본문 hash를 직접 비교한다. 본문이 같은 함수만 frame에 가진 실행은 이전 revision 코드로 남는다(본문 hash가 같으면 동작이 같다 — 명령 단어의 참조 번호는 revision마다 다를 수 있어 frame을 옮기지 않는다). 설정과 무관하게 삭제된 handler에 속한 실행, 형식이 바뀌거나 삭제된 필드를 참조(GETF·SETF의 Field 참조)하는 함수를 frame에 가진 실행은 취소한다. 취소된 실행의 맨 아래 frame이 Start hook이면 적용 뒤 그 instance의 Start를 다시 부른다. 이벤트 handler는 다음 이벤트를 기다린다.
- **변경 보고(K7):** `VmReloadReport` — 적용한 클래스·instance 수, 옛 값을 옮긴 필드·새 기본값을 쓴 필드·손실(형식 변경)·삭제 필드, 취소한 실행 수, 다시 시작한 Start·Tick 수, 이전 revision으로 도는 실행 수.
- **Changed·연결(K8):** reload가 바꾼 값은 Changed를 내지 않는다. 연결 순서는 유지되고 새 `on`은 끝에 더한다(SL §12.4).
- **`.run` 지시(K9):** `reload <버전>`은 사례 옆 파일 `<사례>.<버전>.spark`를 같은 compilation 설정으로 컴파일·링크하고 `PlanReload`·`ApplyReload`(설정은 `reload <버전> keep|cancel`, 기본 cancel — Tryout 기본값)를 한다. 결과는 `ok (instances N, moved M, defaulted D, lost L, removed R, cancelled C, restarted S, previous P)` 또는 `rejected (<이유>)`. corpus 실행기는 `.run`이 있는 파일 사례 옆의 `*.<버전>.spark`를 받는다.
- **증분 재컴파일(K10):** 컴파일러의 재컴파일 범위(`FindRecompileScope` — interface hash, LC4d)로 바뀐 모듈과 interface가 바뀐 모듈에 의존하는 모듈만 다시 lowering한다. 측정은 LC7c에서 본문 하나를 바꾼 모듈의 저장 → 적용 시간(목표 2 s — SL SD8).

## 3. 결정

- K1 대안(같은 `VmLinkedModule`을 다시 링크)은 Keep 실행이 옛 bytecode를 잃는다(지금 재링크된 정지 실행은 취소된다 — LC6c1). 새 객체면 옛 revision의 수명을 host가 참조로 관리한다.
- K2·K3 계획·적용 분리는 SL §12.4 원자적 적용이다. 대안(적용하면서 판정)은 중간 실패 때 되돌릴 상태가 instance 수만큼 생긴다.
- K4 대안(slot 위치로 대응)은 필드 순서만 바꿔도 값이 섞인다. 타입 서술자는 bank·slot이 같아도 다른 타입(int ↔ float, Array<int> ↔ Array<string>)을 가른다.
- K5 대안(항상 옛 값 유지)은 새 기본값을 반영할 길이 없다(SL "새 기본값은 쓰지 않은 필드에만").
- K6은 SL SD30 그대로다. "필드를 참조하는 실행"은 bytecode의 Field 참조로 판정한다(실행 중 값 경로를 추적하지 않는다 — 보수적).
- K9 대안(별도 `.reload` 형식)은 지시가 늘어난다. 버전 파일은 diff로 읽기 쉽다.

## 4. LC7a 구현(개정 2)

- **형식 v5:** CLAS 필드에 타입 표기·`@formerly` 옛 이름(문자열 번호 u32 둘 — verifier: 타입은 문자열 표 안, 옛 이름은 NoIndex거나 표 안). 컴파일러는 `TypeChecker::FormerName`(안정 ID 판정과 같은 `@formerly` 해석)과 표시 타입으로 채운다.
- **API:** `VmSession::PlanReload(old, fresh, VmReloadExecutions, VmReloadPlan&, reason)` · `ApplyReload(plan)` → `VmReloadReport` · `References(linked)`, 상한 `MaxRevisions`(8). 계획 거부(인자·판정 — reason에 영어 한 문장, `InvalidArgument`): session 닫힘·실행 중, old가 session 모듈 아님, fresh가 old와 같거나 이미 session 모듈, fresh가 session host로 링크·import 풀기를 마치지 않음, 모듈 경로가 다름, instance가 있는 클래스의 삭제·기반 변경(R3 필요), R1에서 instance가 있는 클래스의 필드 배치(이름·타입·bank·slot·위치·표지)나 handler 목록 변경(R2 — LC7b). 계획은 모든 옛 클래스·필드·handler·함수의 대응을 만든다(LC7b가 그대로 쓴다).
- **적용 순서:** 낡은 계획 거부(old가 지금 모듈이 아님, fresh가 이미 모듈, 계획 뒤 생긴 instance의 클래스 대응 없음 — `InvalidArgument`, session 그대로) → 이전 revision 기록 정리 → 할당(취소·재시작 목록, 이전 revision 기록) → 취소 → instance의 module·class 교체(그 모듈 Tick 중지 해제) → 연결의 module·handler·key 교체(순서 유지) → 한 handler에만 갈 큐 항목·정지 실행의 시작 handler 교체 → session 모듈 목록 교체와 이전 revision 기록 추가 → 취소된 Start 다시 부르기(instance 생성 순, 적용 기록 `hook` 전달, 그 클래스의 Start 함수 하나 — 사슬 전체가 아니다).
- **이전 revision 코드의 필드(K1):** Keep으로 남은 실행의 `GETF`·`SETF`는 옛 revision의 로컬 필드 묶음(클래스·bank·위치)을 쓴다. session은 이전 revision마다 (link → 다음 revision, 클래스 대응, 필드 대응) 기록을 두고, 필드의 클래스가 instance 사슬에 없으면 기록을 따라 지금 revision 위치로 옮긴다(최대 `MaxRevisions`단계 — 대응이 없거나 bank·slot이 다르면 그 쓰기는 instance 필드가 아니다). 기록은 그 revision이나 더 오래된 revision을 직접 참조하는 것(정지 실행 frame·시작 handler, 큐 항목, 연결, instance)이 있는 동안 남고, `ApplyReload`·`References`가 정리한다(포인터만 비교 — 놓인 revision 객체를 읽지 않는다).
- **revision 상한:** 적용 뒤 살아 있는 revision = 새 revision + (취소 뒤에도 실행이 남으면) old + old 앞의 이전 revision 기록. 8을 넘으면 가장 오래된 revision을 frame에 가진 실행을 취소한다(`cancelled`에 포함 — Start였으면 다시 부른다).
- **취소된 Start:** Start는 준비 대기(로드)만 기다릴 수 있다(SL S0602). 취소된 실행이 루트(하위 실행 아님)이고 맨 아래 frame이 어느 클래스의 Start hook이면, 그 클래스를 지금 revision으로 옮겨(이전 revision 기록·계획의 클래스 대응) 그 Start를 부른다.
- **`.run`·corpus:** `reload <버전> [keep|cancel]` — `<사례>.<버전>.spark`를 파일 사례와 같은 schema·manifest·모듈 경로로 컴파일(진단이 있으면 사례 실패)하고 `.sparkc` 왕복 뒤 링크해 계획·적용한다. 적용되면 그 뒤 지시(`new`·호출·`fields`)는 새 revision의 bytecode·compilation을 쓴다. corpus 실행기는 `.run`이 있는 파일 사례 옆의 `<사례>.<버전>.spark`(자기 `.expect` 없음, 버전에 점 없음)를 그 사례의 revision으로 받고 사례로 돌리지 않는다.
- **시험:** `Run/Reload`(판정 실패 넷 — 클래스 삭제·필드·handler·기반, Cancel의 취소와 Start 다시 부르기·로드 요청 해제, Tick 중지 해제, 연결의 새 handler 전달, 본문이 같은 실행의 이전 revision 실행과 필드 번역, Keep의 이전 revision 실행·Ref 필드 쓰기, 9번째 revision에서 가장 오래된 실행 취소와 남은 7개의 각 revision 실행), `SparksVmReload_RejectsBadArgumentsAndStalePlans`(인자 거부·소비한·낡은 계획·References), `SparksConformance_RunGoldenAllocationFailureReload`(revision 컴파일·계획·적용·필드 번역의 모든 할당 실패 — OutOfMemory, 누수 없음), Gate G4 주입 행렬(`Run/Reload`의 모든 `AWAIT`).
- **한계:** 의존 모듈의 import는 host가 fresh로 다시 푼다(`LinkImports` — 옛 revision을 가리키는 import는 옛 코드를 부른다). 큐 항목의 key 글자는 발행한 revision의 bytecode를 가리키므로 그 항목이 전달될 때까지 그 revision이 참조된다. `Faults()` 기록의 module은 비교용 포인터라 host가 놓은 revision을 가리킬 수 있다(역참조하지 않는다). 필드 migration·handler 연결 재구성·쓰기 bit는 LC7b, G5·측정·증분 재컴파일은 LC7c다.

## 5. 시험

- `.run` `reload` golden이 R1·R2 행을 덮는다: 본문만 바뀜(Keep·Cancel 각각 — 정지 실행이 있는 상태), 필드 추가·삭제·이름 변경(`@formerly`)·형식 변경·기본값 변경(쓴 필드·안 쓴 필드), `on` 추가·삭제(연결 순서), 유지 자원 필드 보존·삭제, 판정 실패(클래스 삭제 — R3 필요).
- 할당 실패 전수: 계획·적용의 모든 할당 실패에서 session이 그대로이고 누수가 없다.
- G5 R1·R2 행: 대기 실행·유지 자원·구독 연결이 있는 상태에서 reload하고 롤백(판정 실패) 경로에서 counter·필드 값·연결 수가 그대로인지 본다.
