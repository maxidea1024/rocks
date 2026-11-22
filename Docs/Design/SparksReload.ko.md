# Sparks 핫 리로드(LC7)

이 문서는 Sparks 언어 코어의 LC7(핫 리로드)을 정한다. 언어 의미는 SparksLanguage(SL) §12.4가 정본이고, 이 문서는 그 중 언어·VM 쪽(R1 본문 교체, R2 제자리 migration의 Sparks 필드·`on`·const, `ReloadExecutions`, 계획·판정·원자적 적용, 증분 재컴파일)을 VM session에서 어떻게 실현하는지 정한다. R3(instance 재생성)·R4(복제 schema)는 엔진 연동(L2·L3)이고, 사전·widget schema의 R2는 EI5다. 실행 모델은 SparksRuntime(LC6) 위에 쌓는다.

- 개정 1(2026-11-22): 첫 판. 하위 단계 LC7a–LC7c, 결정 K1–K10.
- 개정 10(2026-11-23): LC7c 리뷰 반영(6절 끝 "LC7c 리뷰 반영").
- 개정 9(2026-11-23): LC7c3 구현·측정(6절 "LC7c3 설계").
- 개정 8(2026-11-22): LC7c2b 구현(6절 "LC7c2b 설계").
- 개정 7(2026-11-22): LC7c2를 LC7c2a(여러 모듈 `.run`)·LC7c2b(모듈 묶음 reload)로 나누고 LC7c2a 설계(6절).
- 개정 6(2026-11-22): LC7c 하위 단계(6절 — LC7c1 Gate G5, LC7c2 모듈 묶음 reload·증분 재컴파일, LC7c3 측정)와 LC7c1 설계.
- 개정 5(2026-11-22): LC7b 리뷰 반영(5절 끝 "리뷰 반영"). K4의 필드 대응 범위를 같은 클래스로 바로잡음.
- 개정 4(2026-11-22): LC7b 구현(5절). K4에 형태 표기(형식 v6 — struct 배치·enum case가 바뀌면 형식 변경), K5 쓰기 bit를 `$init` 밖의 모든 스크립트 쓰기로(Awake·Enable은 reload 뒤 다시 돌지 않는다), K3 적용 순서를 임시 반영 → 연결 다시 풀기 → 확정으로(연결 할당 실패도 되돌린다), 새 `$init` fault는 적용 거부(SL 12.4 기본값 레코드 재합성 실패).
- 개정 3(2026-11-22): LC7a 리뷰 반영(4절 끝 "리뷰 반영").
- 개정 2(2026-11-22): LC7a 구현(4절). K4의 타입 서술자를 정규 타입 표기로(언어 함수 서술자는 객체 타입이 모두 `x`라 타입 변경을 가르지 못한다), K6 Cancel이 앞서 Keep으로 남은 더 오래된 revision의 실행에도 적용, `.run` 결과에 `previous` 더함.

## 1. 하위 단계

| 단계 | 내용 | 완료 기준 |
|---|---|---|
| LC7a revision·R1 | bytecode 형식 v5(CLAS 필드 타입 서술자·`@formerly` 옛 이름), 새 revision 링크와 `VmSession::PlanReload`·`ApplyReload`(같은 배치 — 본문만 바뀜), instance·연결의 revision 교체, `ReloadExecutions`(Keep·Cancel — 본문이 바뀐 함수를 frame에 가진 실행), 모듈별 살아 있는 revision 8, 취소된 Start 재실행, Tick 중지 해제 | `.run` `reload` golden(R1 행) — 완료(개정 2, 4절) |
| LC7b R2 migration | 필드 대응(이름·옛 이름·타입 표기·형태 표기 — 형식 v6), 새 기본값은 쓰지 않은 필드에만(필드별 쓰기 bit), 유지 자원·Buf 필드의 보존·해제, 삭제·변경 필드를 참조하는 실행 취소, 구독 연결 재구성(순서 유지 — 새 `on`은 끝), 판정 실패(클래스 삭제·기반 변경 = R3 필요) | `.run` `reload` golden(R2 행), 할당 실패 전수 — 완료(개정 4, 5절) |
| LC7c Gate·측정(6절 — LC7c1–LC7c3) | Gate G5의 R1·R2 행(대기 실행·유지 자원·연결이 있는 상태의 reload와 롤백 주입), 본문 reload 2 s 측정(컴파일 → 링크 → 적용), 증분 재컴파일(바뀐 모듈과 의존 모듈만) | G5 R1·R2 통과, 측정 시험 |

## 2. 구조

- **revision(K1):** 새 revision은 새 `VmLinkedModule` 객체다(같은 객체를 다시 링크하지 않는다 — 그러면 Keep 실행이 옛 코드를 잃는다). host는 session이 옛 revision을 참조하는 동안(`VmSession::References(linked)` — instance·정지 실행·연결) 그 객체와 bytecode를 보관한다. 모듈별 살아 있는 revision은 8개까지이고(SL §12.4), 넘으면 가장 오래된 revision을 frame에 가진 실행을 취소하고 보고한다.
- **계획(PlanReload, K2):** `PlanReload(old, fresh, plan)`은 아무것도 바꾸지 않고 계산한다: 클래스 대응(이름 — 옛 클래스가 새 revision에 없거나 기반 클래스가 바뀌었는데 그 클래스의 instance가 있으면 R3가 필요하므로 판정 실패), 필드 대응(아래), handler 대응(함수 이름 — `Door.on Owner.Damaged`), 함수 대응(이름 — 본문 hash 비교), 대기 실행별 처리(유지·취소·Start 재실행). 할당 실패 외의 판정 실패는 이유를 담아 돌려주고 session은 그대로다(SL §12.4 all-or-nothing).
- **적용(ApplyReload, K3):** 실행 중이 아닌 reload 경계에서만(`InvalidArgument`). (LC7b가 순서를 임시 반영 → 연결 다시 풀기 → 확정으로 바꾼다 — 5절.) 순서: (1) 취소할 실행 취소 → (2) instance마다 새 배치 저장소를 만들고(새 `$init`을 Changed 억제로 돌려 기본값) 대응 필드의 옛 값을 옮김(쓰지 않은 필드는 새 기본값 — K5) → (3) 삭제·형식이 바뀐 필드의 유지 자원·Buf 해제 → (4) instance의 module·class 번호 교체 → (5) 연결의 module·handler 번호 교체(삭제된 handler의 연결은 지운다 — 순서 유지) → (6) 활성 instance에 새 handler 연결을 끝에 더함 → (7) session 모듈 목록 교체, Tick 중지 해제(그 모듈의 새 revision) → (8) 취소된 Start 재실행. 적용은 실패하지 않게 할당을 (2) 앞에서 모두 마친다(계획이 저장소 크기를 안다) — 할당 실패면 아무것도 바꾸지 않고 `OutOfMemory`. instance 하나의 `$init` fault는 그 instance만 격리한다(SL §12.4 4번).
- **필드 대응(K4):** 새 클래스의 필드마다 옛 클래스(같은 이름의 클래스 — 자기 필드만, 개정 5: 기반·파생 사이로 옮긴 필드는 삭제 + 새 필드다)에서 같은 이름, 없으면 새 필드의 `@formerly` 옛 이름과 같은 필드를 찾는다. 타입 표기(CLAS 형식 v5 — 정규 표시 이름, 다른 모듈 선언은 타입 인자에서도 `<경로>:` 접두: `int`·`string`·`Door?`·`Array<int>`·`Array<Game/Other:Door>`·`SoundCue`)와 형태 표기(형식 v6 — 5절)가 같으면 값을 옮긴다(Buf는 참조를 옮긴다, 유지 자원은 그대로). 다르면 새 기본값을 두고 "손실" 보고다(무손실 변환 §15는 저장 시스템과 함께 — EI). 대응이 없는 옛 필드는 삭제: 유지 자원은 멈추고 Buf는 놓는다. FieldId(ID manifest)는 컴파일러가 이름 키로 풀어 `@formerly`와 같은 결과가 되므로(SparksBytecode 6.1) VM은 이름과 옛 이름으로 충분하다.
- **쓰기 bit(K5):** instance마다 slot별 "spawn 뒤 쓰기 있음" bit를 둔다(필드 쓰기 명령이 표시 — `$init`의 쓰기만 표시하지 않는다, 개정 4: Awake·Enable이 계산해 둔 값은 reload 뒤 그 hook이 다시 돌지 않으므로 새 기본값으로 덮으면 잃는다). 새 기본값은 이 bit가 없는 필드에만 반영하고, 있는 필드는 옛 값을 유지한다. prefab·instance override는 엔진 쪽(EI)이다.
- **실행(K6 — SL §12.4 SD30):** `ReloadExecutions` Keep이면 정지 실행은 옛 revision으로 끝까지 간다(옛 frame의 link가 옛 객체를 가리킨다). Cancel이면 새 revision과 본문이 다른(또는 삭제된) 함수를 frame에 가진 실행을 취소한다 — 바로 앞 revision의 frame은 계획의 대응표로, 앞선 Keep으로 남은 더 오래된 revision의 frame은 새 revision의 같은 이름 함수와 본문 hash를 직접 비교한다. 본문이 같은 함수만 frame에 가진 실행은 이전 revision 코드로 남는다(본문 hash가 같으면 동작이 같다 — 명령 단어의 참조 번호는 revision마다 다를 수 있어 frame을 옮기지 않는다). 설정과 무관하게 삭제된 handler에 속한 실행, 형식이 바뀌거나 삭제된 필드를 참조(GETF·SETF의 Field 참조)하는 함수를 frame에 가진 실행은 취소한다. 취소된 실행의 맨 아래 frame이 Start hook이면 적용 뒤 그 instance의 Start를 다시 부른다. 이벤트 handler는 다음 이벤트를 기다린다.
- **변경 보고(K7):** `VmReloadReport` — 적용한 클래스·instance 수, 옛 값을 옮긴 필드·새 기본값을 쓴 필드·손실(형식 변경)·삭제 필드, 취소한 실행 수, 다시 시작한 Start·Tick 수, 이전 revision으로 도는 실행 수.
- **Changed·연결(K8):** reload가 바꾼 값은 Changed를 내지 않는다. 연결 순서는 유지되고 새 `on`은 끝에 더한다(SL §12.4).
- **`.run` 지시(K9):** `reload <버전>`은 사례 옆 파일 `<사례>.<버전>.spark`를 같은 compilation 설정으로 컴파일·링크하고 `PlanReload`·`ApplyReload`(설정은 `reload <버전> keep|cancel`, 기본 cancel — Tryout 기본값)를 한다. 결과는 `ok (instances N, moved M, defaulted D, lost L, removed R, cancelled C, restarted S, previous P)` 또는 `rejected (<이유>)`. corpus 실행기는 `.run`이 있는 파일 사례 옆의 `*.<버전>.spark`를 받는다.
- **증분 재컴파일(K10):** 컴파일러의 재컴파일 범위(`FindRecompileScope` — interface hash, LC4d)로 바뀐 모듈과 interface가 바뀐 모듈에 의존하는 모듈만 다시 lowering한다. 측정은 LC7c에서 본문 하나를 바꾼 모듈의 저장 → 적용 시간(목표 2 s — SL SD8).

## 3. 결정

- K1 대안(같은 `VmLinkedModule`을 다시 링크)은 Keep 실행이 옛 bytecode를 잃는다(지금 재링크된 정지 실행은 취소된다 — LC6c1). 새 객체면 옛 revision의 수명을 host가 참조로 관리한다.
- K2·K3 계획·적용 분리는 SL §12.4 원자적 적용이다. 대안(적용하면서 판정)은 중간 실패 때 되돌릴 상태가 instance 수만큼 생긴다.
- K4 대안(slot 위치로 대응)은 필드 순서만 바꿔도 값이 섞인다. 타입 서술자는 bank·slot이 같아도 다른 타입(int ↔ float, Array<int> ↔ Array<string>)을 가른다.
- K5 대안(항상 옛 값 유지)은 새 기본값을 반영할 길이 없다(SL "새 기본값은 쓰지 않은 필드에만"). 대안(옛 기본값과 값이 같으면 새 기본값 — UE 재인스턴스의 CDO 비교)은 SL이 쓰기 bit로 정했고, 같은 값을 다시 쓴 필드(사용자가 기본값을 명시적으로 고른 경우)를 가르지 못한다. 억제 문맥(Awake·Enable)을 표시하지 않는 대안(개정 1)은 Enable이 계산한 값을 잃는다(개정 4).
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
- **리뷰 반영(개정 3):**
  - 낡은 계획: 계획은 session의 다음 instance 생성 번호를 기억하고, 적용 때 다르면(계획 뒤 instance가 생겼다 — 계획 때 instance가 없어 필드·handler 변경을 허용한 클래스일 수 있다) `InvalidArgument`로 아무것도 바꾸지 않는다.
  - 되돌리기: fresh가 아직 참조되는 이전 revision 객체(이전 revision 기록의 link·successor이거나 session이 직접 참조)면 계획 거부다(기록이 순환해 영구히 남고 상한 계산이 부푼다). 참조가 끝난 옛 객체는 다시 쓸 수 있다.
  - 취소된 Start: 취소 대상과 별도로, 맨 아래 frame이 Start hook인 루트 실행을 모두 후보로 두고 취소 단계 뒤 살아 있지 않으면(직접 취소든 하위 실행 취소가 이어서든) 다시 부른다. 지금 언어는 Start가 하위 실행을 기다리지 못하게 해(S0602 — 준비 대기만) 루트가 늘 직접 걸리지만, 판정은 문법에 기대지 않는다. `cancelled`는 이어진 취소까지 센다(취소 전후 정지 실행 수의 차).
  - 한 handler에만 갈 큐 항목(Changed 경로 원천)은 대상 handler가 없어지면 버리고(모두에게 전달되지 않게), 남으면 key를 새 revision의 글자로 바꾼다.
  - `Uses`는 Tick 중지 모듈(`tickStopModule`)도 본다. revision 상한의 old 판정은 old를 붙잡는 큐 항목도 센다(큐 항목은 만료 취소 대상이 아니다 — 전달되면 놓인다).
  - Start 다시 부르기의 할당 실패는 `VmReloadReport::restartError`(reload는 적용됐다 — 남은 Start는 부르지 않았다)이고 `ApplyReload`는 보고를 돌려준다.
  - corpus: revision 원문은 짝 파일(폴더·`.expect`·덤프·`.run`)이 없는 `.spark`이고, `.run`이 어느 `reload`에서도 쓰지 않는 원문은 사례 실패다(이름 오타).
  - 가짜 host(`FakeHost`)는 처음 compilation의 schema·클래스 관계로 `IsA`·`CAST`·가상 호출 대상을 풀기 때문에 revision이 클래스 관계를 바꾸는 시험은 연동 단계 host로 미룬다(R1·R2 golden은 클래스 관계를 바꾸지 않는다).
- **시험:** `Run/ReloadStart`(호출한 async 함수의 본문만 바뀐 Start — 실행 전체 취소·로드 요청 해제·새 revision Start), `Run/Reload`(판정 실패 넷 — 클래스 삭제·필드·handler·기반, Cancel의 취소와 Start 다시 부르기·로드 요청 해제, Tick 중지 해제, 연결의 새 handler 전달, 본문이 같은 실행의 이전 revision 실행과 필드 번역, Keep의 이전 revision 실행·Ref 필드 쓰기, 9번째 revision에서 가장 오래된 실행 취소와 남은 7개의 각 revision 실행), `SparksVmReload_RejectsBadArgumentsAndStalePlans`(인자 거부·소비한·낡은 계획·References), `SparksConformance_RunGoldenAllocationFailureReload`(revision 컴파일·계획·적용·필드 번역의 모든 할당 실패 — OutOfMemory, 누수 없음), Gate G4 주입 행렬(`Run/Reload`의 모든 `AWAIT`).
- **한계:** 의존 모듈의 import는 host가 fresh로 다시 푼다(`LinkImports` — 옛 revision을 가리키는 import는 옛 코드를 부른다). 큐 항목의 key 글자는 발행한 revision의 bytecode를 가리키므로 그 항목이 전달될 때까지 그 revision이 참조된다. `Faults()` 기록의 module은 비교용 포인터라 host가 놓은 revision을 가리킬 수 있다(역참조하지 않는다). 필드 migration·handler 연결 재구성·쓰기 bit는 LC7b(5절), G5·측정·증분 재컴파일은 LC7c다.

## 5. LC7b 구현(개정 4)

- **형식 v6 — 형태 표기:** CLAS 필드에 형태 표기(문자열 번호 u32 — verifier: 문자열 표 안)를 더한다. 타입 표기의 Sparks struct·enum 이름 뒤에 `{…}`로 배치를 붙인 글자다: struct는 선언 순 `이름:형태`(재귀), enum은 선언 순 case 이름(`Stats{Level:int,Offset:vec3}`·`Mode{Idle,Busy,Done}`·`Array<Stats{Level:int}>`). 한 형태 안에서 이미 펼친 타입은 두 번째부터 이름만 쓴다(`Tree{Kids:Array<Tree>}` — 순환, `Pair{A:Inner{X:int},B:Inner}` — 같은 struct를 여러 번 쓰는 배치가 지수로 커지지 않게, 개정 5. 펼침 순서가 결정적이라 형태는 여전히 배치를 가른다). 엔진 struct·enum은 host 배치라 이름만이다(엔진 변경은 Tryout 재시작 — SL 12.4). 형태가 타입 표기와 같으면 같은 문자열 번호다(표가 늘지 않는다). 덤프는 다를 때만 ` shape <형태>`를 쓴다. 이유: struct 필드를 더하거나 enum case 순서를 바꿔도 타입 표기(`Stats`·`Mode`)는 같지만 옛 값 버퍼·case 번호는 새 코드에서 다른 뜻이다(struct 접근은 bank 구역 검사로 메모리는 안전하지만 값이 섞인다). 대안(형태 hash)은 덤프로 원인을 읽을 수 없다. struct 필드 단위 재귀 migration·enum case 이름 대응(SL 12.4 보존 규칙)은 무손실 변환(SL §15)과 함께 EI다 — LC7b는 형태가 다르면 "손실"(새 기본값)로 둔다.
- **계획(PlanReload):**
  - 필드: 새 필드마다 원천 옛 필드 = 같은 이름(먼저 모두), 그다음 아직 원천이 아닌 옛 필드 중 `@formerly` 옛 이름과 같은 것. 원천의 타입 표기·형태 표기·bank·slot이 같으면 "옮김"(`fieldMap_` — 이전 revision 필드 번역도 이 대응만 쓴다), 다르면 "손실", 원천이 없으면 "새 필드". 어느 새 필드의 원천도 아닌 옛 필드는 "삭제".
  - handler: 함수 이름·경로·이벤트로 대응한다(재진입 정책은 전달 때 새 handler 기록을 읽으므로 대응에 넣지 않는다 — 바뀌어도 연결 순서 유지).
  - 함수: 본문 hash가 다르거나 없어진 함수는 "바뀜", 옮기지 않는(손실·삭제) 옛 필드를 필드 명령(GETF·SETF의 Field 참조)으로 참조하는 함수는 "필드 깨짐"(명령 단어를 훑는다).
  - 판정 실패는 instance가 있는 클래스의 삭제·기반 변경(R3)만 남는다. R1의 필드 배치·handler 목록 거부는 없앤다.
- **실행 처리(K6):** 정책과 무관하게 취소: "필드 깨짐" 함수를 old frame에 가진 실행, 더 오래된 revision frame의 필드 참조가 이전 revision 기록과 이번 대응으로 새 revision까지 번역되지 않는 실행, 삭제된 handler로 시작된 실행. Cancel이면 LC7a대로 "바뀜" 함수의 실행을 더한다. 취소된 Start 루트는 다시 부른다(LC7a).
- **쓰기 bit:** instance마다 slot별 bit 묶음(Scalar·Ref·Buf 순으로 이은 번호 — 64 bit 단어)을 `CreateInstance`가 할당한다(0). 필드 쓰기 명령(SETF)이 값을 둔 뒤 그 필드의 slot을 표시하고, `CreateInstance`의 `$init`과 reload 기본값 계산의 쓰기는 표시하지 않는다. 필드는 첫 slot의 bit로 판정한다.
- **기본값 레코드:** 적용 준비에서 영향받는 instance 사슬에 든 새 클래스마다 새 `$init`을 한 번 돌려 그 클래스 구역만의 임시 저장소(자기 slot 수)에 기본값을 만든다. 필드 초기값은 상수식이라(SL §3.11 — 함수 호출·필드 참조 없음) 결과가 instance마다 같다. `this`는 그 클래스를 사슬에 가진 영향 instance 하나의 handle이고(host 생존 검사), 필드 자리는 기본값 계산 중인 (새 revision, 클래스)이면 임시 저장소다(`FieldSlots` — instance 번호 없음: Changed 기록·instance 메모리 예산·쓰기 bit를 건너뛴다). `$init`의 할당 실패는 `OutOfMemory`, fault는 기본값 레코드 재합성 실패라 `InvalidArgument`이고 둘 다 아무것도 바꾸지 않는다(SL 12.4 판정).
- **새 저장소:** 영향 instance(사슬에 old 클래스가 있다)마다 새 사슬(old 클래스 → 대응 새 클래스, 나머지 그대로 — 다른 모듈 파생 구역도 뒤로 밀린다)의 bank별 크기로 새 배열·쓰기 bit를 만든다. old 밖 구역은 그대로 복사(Buf는 참조를 더한다, bit도 복사). 새 클래스 구역은 필드마다:
  - 옮김: 유지 자원 필드면 handle을 그대로 옮기고 "옮김", 쓰기 bit가 있으면 옛 값(Buf는 참조를 더한다)과 bit를 옮기고 "옮김", 없으면 기본값(Buf 참조를 더한다)이고 "기본값".
  - 손실: 기본값, "손실". 새 필드: 기본값, "기본값". 삭제된 옛 필드는 instance마다 "삭제".
  새 저장소의 Buf는 모두 자기 참조를 쥐므로 확정이 옛 배열의 Buf를 모두 놓고, 되돌리기는 새 배열의 Buf를 모두 놓는다(소유가 한쪽으로 섞이지 않는다).
- **적용 순서(K3 개정):** (1) 준비(할당 — 취소·재시작 목록, 이전 revision 기록, 기본값 레코드, 새 저장소, 연결 표 사본과 예약) → (2) 임시 반영: instance 저장소·module·class 교체(옛 배열은 준비 기록이 쥔다), session 모듈 목록 교체, 연결의 module·handler·key 교체(삭제된 handler의 연결은 지운다 — 순서 유지) → (3) 활성 instance마다 연결 다시 풀기(`Connect` — 남는 발행자는 순서를 지키고 새 `on`·새 발행자는 끝에, 원천 필드가 손실·기본값으로 바뀐 경로도 여기서 바로잡힌다). 할당 실패면 (2)를 되돌리고(연결 표 사본·거부 연결 수 복원, 저장소·모듈 되돌림, 새 저장소 해제) `OutOfMemory` → (4) 확정(실패 없음): 옛 배열의 Buf 해제, 옮기지 않은 유지 자원 필드(손실·삭제)를 선언 역순으로 멈춤, Tick 중지 해제, 큐 항목·정지 실행의 handler 교체, 취소, 이전 revision 기록, 취소된 Start 다시 부르기. 취소를 연결 뒤로 옮긴 것은 취소가 되돌릴 수 없기 때문이다(취소는 instance 저장소를 읽지 않는다 — frame 레지스터·대기만 놓는다).
- **Changed:** 기본값 계산과 저장소 교체는 필드 쓰기 명령이 아니므로 Changed 기록·쓰기 bit가 생기지 않는다(K8). `Connect`는 `on A.B.Changed`의 한 번 전달을 넣지 않는다(그것은 `Reconnect` — 원천 쓰기 경로).
- **보고:** `moved`·`defaulted`·`lost`·`removed`는 (영향 instance, 필드) 쌍의 수다(위 분류). 본문만 바뀐 reload도 쓰지 않은 필드는 "기본값"으로 센다(값은 같다).
- **`.run`:** 적용 실패는 `apply failed (<오류>)`. `fields`는 적용 뒤 새 revision 배치로 읽는다.
- **한계:** 다른 모듈 클래스 필드의 형태(그 모듈이 쓰는 struct가 reload 모듈의 struct일 때)는 그 모듈을 함께 다시 컴파일·reload해야 바뀐다(의존 모듈 묶음 적용 — LC7c 증분 재컴파일, 엔진의 묶음 reload). 손실 변환·enum case 대응·struct 재귀 migration은 EI(§15). host가 쓴 필드(prefab·override)의 쓰기 표시는 EI(엔진의 override 경로). 새 기본값 저장소가 instance 메모리 예산을 넘는지는 보지 않는다(쓰기 명령만 거부한다).
- **리뷰 반영(개정 5):**
  - "필드 깨짐"은 함수 하나가 아니라 같은 모듈 호출(CALL·CALLW의 Function 참조) 폐포다: 재개 뒤 부를 callee가 옮기지 못한 필드를 쓰면 그 실행도 취소한다(실행 중 `ContractViolation` 대신). 적용 준비가 old와 이전 revision마다 함수별 표지를 계산한다(할당 — 실패면 그대로). 다른 모듈 import·가상 호출은 host가 지금 revision으로 풀므로 폐포에 넣지 않는다.
  - 연결 다시 풀기의 되돌리기는 연결 순번(`nextOrder_`)도 복원한다. 이전 revision 번역 단계 상한은 `FieldSlots`와 같은 `MaxRevisions`다.
  - `.run`은 적용 실패(OutOfMemory·InvalidArgument) 전후 session 상태(instance별 module·class·필드 값·Buf 포인터, 연결·거부·정지 실행·대기·큐 수)를 대조한다 — 할당 실패 전수 시험이 모든 실패 자리에서 "session 그대로"를 본다. `limits`는 셋째 인자로 조각 명령 예산을 받는다(새 `$init` fault 시험).
  - 한계: 필드 초기값의 component 생성(PF1 뒤 — SL §3.11)은 기본값 레코드를 instance마다 나눌 수 없다(한 component를 모든 instance가 가리킨다). 그 단계에서 component 생성 필드는 instance별로 만들거나 R3로 보낸다. 시험 host가 파일 사례 하나의 모듈만 reload하므로 다른 모듈의 파생·기반 클래스가 섞인 사슬은 연동 단계 host 시험이다.
- **시험:** `Run/ReloadFields`(같은 모듈 기반 클래스의 필드 추가·삭제(파생 구역이 밀림, 기반 유지 자원), callee를 통한 삭제 필드 참조 취소, 두 reload에 걸쳐 위치가 옮겨진 필드를 쓰는 이전 revision 실행, 비활성 instance, 새 `$init` fault의 적용 거부, 필드 추가·삭제·`@formerly` 이름 변경·형식 변경·기본값 변경(쓴 필드·안 쓴 필드)·struct 형태 변경·enum case 변경, 유지 자원 필드 보존·삭제·형식 변경, Buf 필드, `on` 추가·삭제(연결 순서), 삭제 필드를 참조하는 Keep 실행 취소, 다른 클래스 instance는 그대로), `Run/Reload`(R1 행 — 필드·handler 거부 줄은 R2로 옮겨 없앤다), `SparksConformance_RunGoldenAllocationFailureReloadFields`(R2의 모든 할당 실패 — `OutOfMemory`이면 session 그대로·누수 없음, 연결 다시 풀기 실패의 되돌림 포함), 형식 v6 verifier 음성 시험.

## 6. LC7c(개정 6)

| 단계 | 내용 | 완료 기준 |
|---|---|---|
| LC7c1 Gate G5 | reload 롤백 주입 행렬: 통과한 `.run` 사례의 n번째 적용마다 임시 반영 뒤 되돌리기를 주입, 판정 거부·적용 실패 전후 session 상태 대조 | 저장소 corpus의 모든 `reload` 행에서 주입이 일어나고 상태 그대로·누수 0 |
| LC7c2a 여러 모듈 `.run` | 폴더 사례의 `.run`: 모든 모듈 `.sparkc` 왕복·링크·`LinkImports`·session 모듈 등록, 가짜 host의 여러 모듈 가상 호출, 지시의 클래스·함수 이름은 모든 모듈에서 찾는다 | 폴더 사례 golden(다른 모듈 기반 클래스·import 호출·다른 모듈 이벤트 구독), 할당 실패 전수 — 완료(개정 7) |
| LC7c2b 모듈 묶음 reload(완료 — 개정 8) | 바뀐 모듈과 interface가 바뀐 모듈에 의존하는 모듈을 한 계획·한 적용으로(SL 12.4 all-or-nothing), 의존 모듈 import 다시 풀기, 컴파일러 재컴파일 범위(`FindRecompileScope`)로 묶음 결정, `.run` 폴더 사례 reload | 폴더 사례 golden(의존 모듈의 struct 변경·호출 대상 변경), 할당 실패 전수 |
| LC7c3 측정(완료 — 개정 9) | 본문 하나를 바꾼 모듈의 저장 → 컴파일 → 링크 → 적용 시간(목표 2 s — SL SD8) | 측정 시험(시간은 단언하지 않고 보고 — 결정적 시험 원칙), 문서에 Release 측정값 |

- **LC7c1 설계:**
  - `VmSession::InjectReloadRollback()`(시험 전용 — G4 `InjectAwait`와 같은 자리): 다음 `ApplyReload`가 연결 다시 풀기까지 마친 뒤 연결 실패와 같은 되돌리기 경로를 밟고 `InvalidArgument`를 돌려준다(`InjectionFired`). 되돌리기는 LC7b의 할당 실패 경로와 같은 코드라 주입은 그 경로를 할당자 없이 모든 reload 상태(대기 실행·유지 자원·연결이 있는 상태 — SL G5)에서 밟게 한다.
  - `.run`은 `PlanReload` 전과 판정 거부·적용 실패 뒤 session 상태(LC7b 리뷰 반영의 대조 — instance별 module·class·필드 값·Buf 포인터, 연결·거부·정지 실행·대기·큐 수)를 비교한다(다르면 사례 실패).
  - corpus 실행기는 G4 행렬 뒤에 G5 행렬을 돈다: 통과한 사례에서 적용에 성공한 `reload`마다(앞 단계에서 스스로 거부되는 적용 — 새 `$init` fault — 에는 되돌릴 임시 반영이 없다, 사례당 64개까지) 그 적용에 주입해 다시 돌리고, 출력은 비교하지 않고 주입 발생·상태 대조·`CloseSession` 뒤 ledger·유지 자원 0을 본다.
  - 음성 대조(G3 방식 — 2026-11-22 수동, 코드에 남기지 않음): 되돌리기에서 연결 표 복원을 빼면 `Run/ReloadFields`가 "a refused or failed reload changed the session"으로, 저장소 복원을 빼면 범위 검사로 실패한다.
  - 범위: R3(instance 재생성) 실패 주입과 counter(엔진 ledger)는 연동 단계 G5다.
- **LC7c2a 설계(개정 7):**
  - 폴더 사례에 `.run`이 있으면 모든 모듈이 진단 없이 내려와야 한다(아니면 사례 실패). 모듈마다 `.sparkc` 왕복 → `LinkModule`(가짜 host) → 전체 `LinkImports` → session `AddModule`(compilation 모듈 번호 순). `.run`의 실행 단위 표(unit — compilation 모듈 번호, 모듈 경로, bytecode, 링크)가 파일 사례의 단일 모듈 포인터를 대신한다(파일 사례는 unit 하나).
  - 지시의 클래스(`new #1 Door`)와 함수(`Door.Watch(...)`)는 모든 unit에서 이름으로 찾는다. 여러 unit에 같은 이름이 있으면 `<모듈 경로>:Door`로 고른다(없으면 지시 오류 "ambiguous"). `fields`는 instance 사슬의 각 클래스를 그 클래스의 unit bytecode·compilation 심볼로 읽는다(다른 모듈 기반 클래스 포함).
  - 가짜 host: `SetModules(units)` — 가상 호출 대상은 수신자 타입부터 기반 쪽으로 모든 unit의 `<타입>.<메서드>` 함수다. 클래스 관계(`IsA`·`CAST`)는 compilation 심볼을 이름으로 본다(사례는 모듈 사이 클래스 이름을 겹치지 않게 쓴다 — 한계로 남긴다).
  - 이벤트 key·발행 대조는 기존대로 모듈 경로가 붙은 key다(다른 모듈 클래스의 이벤트 구독이 이 경로를 쓴다).
  - `fields`의 사슬은 `VmSession::ClassChain`(공개 — 시험·덤프용)으로 읽는다(전에는 같은 모듈 기반만 따라갔다).
  - `reload`는 폴더 사례에서 아직 지시 오류다(LC7c2b).
  - 시험: `Run/Modules`(폴더 사례 — 다른 모듈 기반 클래스의 필드·이벤트, import 최상위 함수 호출, 기반 모듈에서 부른 가상 호출의 파생 override, Gate G4·G5 행렬), `SparksConformance_RunGoldenAllocationFailureModules`.
- **LC7c2b 설계(개정 8):**
  - **묶음:** 원문이 바뀐 모듈(content hash) ∪ 재컴파일 범위(`FindRecompileScope` — interface가 바뀐 모듈과 그것을 전이적으로 import하는 모듈, 본문이 바뀐 함수의 모듈) ∪ 묶음 모듈을 import하는(함수 import 참조) 모듈의 전이 폐포. 묶음 모듈은 모두 새 revision 객체가 된다(원문·bytecode가 같아도 — 그 import 표가 새 revision을 가리켜야 하고, 옛 revision 객체의 import 표는 Keep 실행을 위해 옛 대상을 그대로 가리킨다: SL 12.4 Keep "그 실행이 하는 호출도 이전 revision 안에서 해석").
  - **링크:** 새 revision마다 `LinkModule` → `LinkImports(새 revision들, 묶음 밖 지금 모듈들)`(새 오버로드 — 첫 묶음의 import만 풀고 둘째 묶음은 이름 찾기 대상으로만 쓴다, 둘째 묶음을 고치지 않는다). 기존 `LinkImports(modules)`는 `LinkImports(modules, {})`다.
  - **VM:** `VmReloadModule{old, fresh}` 목록을 받는 `PlanReload(Span<const VmReloadModule>, …)`(한 쌍 판은 그 감싸기). 계획은 쌍마다 LC7a·LC7b 대응표(`VmReloadPlan::Entry`)를 갖고, `ApplyReload`는 모든 쌍을 한 준비·한 임시 반영·한 연결 다시 풀기·한 확정으로 적용한다(LC7b 원자성 그대로 — 실패면 모든 쌍이 그대로). 쌍마다 revision 상한·이전 revision 기록·Tick 중지 해제·Cancel 판정·필드 깨짐 표를 따로 둔다. 사슬의 클래스는 그 모듈이 속한 쌍의 대응으로 옮긴다(다른 모듈 기반이 같이 바뀌는 사슬).
  - **추가 판정(계획 거부):** old·fresh 중복, 쌍의 모듈 경로 다름, 새 revision이 묶음의 옛 revision을 import함, 묶음 밖 session 모듈이 묶음의 옛 revision을 import함(그 모듈도 묶음에 넣어야 한다 — host 실수 방지).
  - **`.run`:** 폴더 사례의 revision은 사례 폴더 옆 `<사례>.<버전>/`(바뀐 파일만 — 같은 상대 경로로 덮는다. 모듈 추가·삭제는 지시 오류 — 엔진 연동의 모듈 목록 변경)이다. 사례 파일 전체에 덮어 다시 컴파일(진단이 있으면 사례 실패)하고 위 규칙으로 묶음을 정해 `reload <버전> -> ok (modules N, …)`(묶음 모듈 수를 앞에) 줄을 쓴다. 파일 사례의 출력은 그대로다. corpus 실행기는 `.run`이 있는 폴더 사례 옆의 짝 파일 없는 `<사례>.<버전>` 폴더를 revision으로 받는다. revision은 언제나 사례 원본 위에 덮는다(앞 revision 위가 아니다 — 앞 revision에서 바꾼 파일을 이어 쓰려면 그 revision 폴더에도 둔다).
  - 시험: `Run/Bundle`(기반 모듈의 필드 추가·함수 본문 변경과 import 폐포로 묶음 2개 — 독립 모듈 제외, 다른 모듈 기반이 커져 밀린 파생 구역, Keep으로 남은 옛 Main 실행이 옛 import로 옛 Base 함수를 부름, 기반 변경 거부로 묶음 전체 그대로, 한 모듈만 바뀐 revision — G4·G5 행렬 포함), `SparksVmReload_BundleRulesForImports`(묶음 밖 importer·새 revision의 옛 import·중복·빈 묶음 거부, 묶음 적용, targets 불변), `SparksConformance_RunGoldenAllocationFailureBundle`.
  - 한계: 가짜 host는 처음 compilation의 클래스 관계를 쓴다(LC7a 리뷰 반영과 같다). 모듈 추가·삭제와 패키지 의존 변경은 엔진 연동(SL 12.4 — 의존 graph 변경은 Tryout 재시작).
- **LC7c3 설계(개정 9):**
  - **무엇을 재나:** 본문 하나를 바꾼 revision의 reload 경로 전체 — 컴파일(새 compilation의 Check·lowering — 모든 모듈), 묶음 결정(content hash·`FindRecompileScope`·import 폐포), 링크(묶음 모듈 `.sparkc` 왕복·`LinkModule`·`LinkImports`), 계획(`PlanReload`), 적용(`ApplyReload` — instance 이전·연결 다시 풀기). 저장 감지·파일 읽기(편집기·Rocks Build의 몫)는 넣지 않는다.
  - **어떻게:** `CaseInput::timings`(`RunTimings` — 단계별 누적 nanosecond와 적용에 성공한 reload 수)가 있으면 `.run`의 reload가 단계마다 `MonotonicNanoseconds`로 잰다(파일 사례는 컴파일과 링크를 compile에 함께 넣는다). 측정 시험은 생성한 폴더 사례(바뀌는 `Core` 모듈과 그것을 함수 import하는 모듈들, 클래스·필드·함수 수십~수백 개, instance 수백 개와 구독 연결)를 메모리에서 만들어 `RunCase`로 돌리고, 결과 줄(`reload v2 -> ok (modules N, …)`)만 단언한 뒤 측정값을 진단 출력에 쓴다. 시간은 단언하지 않는다(결정적 시험 원칙 — 기기·부하에 따라 다르다).
  - **기록:** Release 측정값을 이 절에 적는다(기기·크기와 함께). 2 s를 넘으면 가장 큰 단계를 줄이는 후속을 이 절에 적는다(컴파일러의 모듈 단위 lowering 재사용 — K10의 증분 재컴파일 — 은 측정에서 컴파일이 지배적일 때 착수한다).
  - **측정(2026-11-23, `SparksReload_MeasuresBodyReload`):** 모듈 5개(Core — 함수 20·클래스 60·클래스마다 메서드 12, 사용자 모듈 4 — 클래스 20씩), 원문 139 KB, instance 460, 구독 연결 240, 묶음 5모듈. 13th Gen Intel Core i5-13600KF, Windows 11:
    - Release 3회: 합계 23.8–30.1 ms(compile 19.8–26.1, bundle 1.0, link 0.8, plan 0.85–0.88, apply 1.26–1.44 ms).
    - Debug 1회: 합계 153 ms(compile 123, bundle 12, link 5.7, plan 8.6, apply 3.3 ms).
    - 판정: 목표 2 s(SL SD8)의 1.5 % 안팎이다. 컴파일이 80 % 이상을 차지하고 원문 크기에 비례하므로, 같은 비율이면 원문 약 9 MB에서 2 s에 닿는다. 그래서 K10의 모듈 단위 lowering 재사용은 지금 착수하지 않고, 프로젝트 원문이 MB 단위로 커지는 연동 단계(EI — Rocks Build compile action, SD6)에서 다시 잰다. 저장 감지·파일 읽기·편집기 요약은 이 측정 밖이다.
- **LC7c 리뷰 반영(개정 10):**
  - **1 — import로 닿는 옛 revision:** import는 링크 때 정적으로 묶이므로(Keep 실행의 옛 코드는 옛 대상을 부른다) 이전 revision 기록은 살아 있는 정지 실행이 frame 모듈과 그 import 폐포로 닿는 동안 남는다(`ExecutionReaches` — 따라간 모듈이 `MaxReachModules`(256)를 넘으면 닿는다고 본다). `References`는 그래서 그 revision을 참조 중으로 답한다(놓으면 옛 코드의 CALL이 놓인 객체를 부른다). revision 상한의 "old가 남는다"와 만료 취소도 같은 닿음으로 판정한다.
  - **2 — 판정의 호출 폐포:** "바뀜"(Cancel)과 "필드 깨짐"(정책 무관)을 쌍의 old와 살아 있는 모든 이전 revision의 함수에 대해 계산한다. 초기값은 그 모듈 경로의 최신 revision(묶음이면 새 revision, 아니면 지금 모듈)과의 본문 비교·필드 번역이고, 같은 모듈 호출과 정적으로 묶인 import 호출을 따라 한 번의 전역 고정점으로 넓힌다. 그래서 자기 본문은 같아도 바뀐 함수를 부르는 옛 실행이 Cancel에서 취소된다(본문 hash가 같으면 동작이 같다는 K6의 전제는 callee가 이전 revision에 묶이면 깨진다 — 개정 10에서 바로잡음). 가상 호출은 host가 지금 revision으로 푼다.
  - **3 — 새 revision의 import 대상:** 묶음의 다른 새 revision이거나 묶음 밖 지금 session 모듈이어야 한다(이전 revision·session 밖 모듈은 판정 실패).
  - **4 — G5 대조:** `VmSession::AppendGateState`(시험 전용)가 session 모듈 목록, 연결마다 (발행자, 구독자, 모듈, handler, key 글자 위치·길이, 순번, 활성), 다음 연결 순번, instance마다 쓰기 bit·Tick 중지를 더한다. 주입 실행은 그 뒤의 출력·지시 오류를 보지 않는다(상태 대조와 누수만 — 한계로 남긴다).
  - **5 — `emit`:** `<경로>:Class.Event`의 key 이름은 경로를 뗀 `Class.Event`이고, 여러 모듈에 있는 이름은 지시 오류다.
  - 시험: `Run/Bundle`에 Keep 실행이 옛 Base 메서드(`Raise` — import 호출)로 배치가 바뀐 필드를 쓰는 경우와, Cancel에서 자기 본문은 같고 import로 바뀐 함수(`Twice`)를 부르는 실행의 취소(v4)를 더했다.

## 7. 시험

- `.run` `reload` golden이 R1·R2 행을 덮는다: 본문만 바뀜(Keep·Cancel 각각 — 정지 실행이 있는 상태), 필드 추가·삭제·이름 변경(`@formerly`)·형식 변경·기본값 변경(쓴 필드·안 쓴 필드), `on` 추가·삭제(연결 순서), 유지 자원 필드 보존·삭제, 판정 실패(클래스 삭제 — R3 필요).
- 할당 실패 전수: 계획·적용의 모든 할당 실패에서 session이 그대로이고 누수가 없다.
- G5 R1·R2 행: 대기 실행·유지 자원·구독 연결이 있는 상태에서 reload하고 롤백(판정 실패) 경로에서 counter·필드 값·연결 수가 그대로인지 본다.
