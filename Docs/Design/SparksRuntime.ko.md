# Sparks 실행 모델(LC6)

이 문서는 Sparks 언어 코어의 LC6(실행 모델)을 정한다. 언어 의미는 SparksLanguage(SL) §4.3·§5·§6·§7·§8.3·§9가 정본이고, 이 문서는 그 의미를 VM 쪽에서 어떻게 실현하는지(자료 구조·순서·host interface 2차·bytecode 형식 v3·시험)를 정한다. LC5(SparksVm.ko.md)의 링크·host interface 1차 위에 쌓는다.

- 개정 1(2026-11-22): 첫 판. 하위 단계 LC6a–LC6e, 결정 R1–R12.
- 개정 7(2026-11-22): LC6c2 리뷰 반영(2.5절).
- 개정 6(2026-11-22): LC6c2 구현 메모(2.5절 "LC6c2 구현").
- 개정 5(2026-11-22): LC6c2 설계(2.5절) — `WAITEV`(이벤트 대기 handle, `AWAITEV` 대체)·전달 순회 안 재개·발행자 파괴, `CALLW`(void async 호출의 하위 실행 — child frame 경계 정지·fault 전파), `Advance(…, paused)`. 결정 R13–R17.
- 개정 4(2026-11-22): LC6c1 리뷰 반영 — 대기 번호 재사용(세대 handle)·조각 끝 정리, 재링크 뒤 재개 취소, 한 번만 기다림, 단계 시작 스냅샷 재개 순서(instance 생성 번호), 하위 묶음 취소 전파, Random 보존, Advance·FixedStep을 새 스크립트 단계로, Detach가 this frame 실행도 취소, async 함수를 대기 값으로 쓰는 호출의 임시 처리(2.4절 "LC6c1 리뷰 반영").
- 개정 3(2026-11-22): LC6b 리뷰 반영 — Changed 자리를 첫 쓰기에서 확보(순서·fault), 비활성 instance 미발행, 경로 원천 변경 시 1회 전달, 다른 모듈 기반의 원천 대조를 key로, 진입마다 루트 초기화, 실행 중 재진입 계약, Detach의 쓰기 기록 정리(2.3절 "LC6b 리뷰 반영").
- 개정 2(2026-11-22): LC6c를 LC6c1(정지·기본 대기·All/Race·시계·재개 순서·취소·재진입 — 구현)과 LC6c2(이벤트 대기·async 함수 호출의 대기 값·`paused`·Start 준비 대기)로 나눔. `await F()` 직접 호출 lowering, `.run` `advance`·`fixed`, 재개 전 실행 문맥 비우기(2.4절).

## 1. 하위 단계

| 단계 | 내용 | 완료 기준 |
|---|---|---|
| LC6a session·instance·hook | `VmSession`(실행 단위 — SL §9.1 소유 범위), bytecode 형식 v3의 CLASS 구역(클래스 필드 배치·기본값 생성 함수·hook 표·handler 표·이벤트 표), instance 생성(필드 저장소는 session — Sparks 클래스 필드의 `GETF`·`SETF`는 VM이 직접), 수명 hook 실행(Awake·Enable·Start·Tick 계열·Disable·Destroy — 기반 먼저 누적), Detach | `.run` instance 지시 golden(§4)의 hook 순서·필드 값 |
| LC6b 발행·전달 | `emit`·발행 큐(FIFO)·연결 표(slot·generation)·`on 경로.이벤트` 연결(자기 필드·객체 필드·배열 원천·native 이벤트는 host 2차)·Changed(조각 값 기준)·연쇄 깊이 상한 16·전달 상한·이월 재검증 | 전달 순서 golden, CascadeLimit·BudgetExceeded 시험 |
| LC6c1 async 실행 | 실행 조각과 정지(`AWAIT` — frame stack 보관), 기본 대기(`Delay`·`NextTick`·`NextFixedTick`·`All`·`Race`), 시계·재개 순서(SL §6.1), 취소(Disable·Detach), 재진입 정책(`@skipWhileBusy`·`@restart`), `await F()`(async 함수) 직접 호출 | `Run/Async` golden(재개 순서·취소·재진입), 할당 실패 전수 |
| LC6c2 async 나머지 | 이벤트 대기(`AWAITEV` — 전달 순회 안 재개), async 함수 호출을 대기 값으로 쓰기(`All(F(), G())` — 하위 실행·fault 전파), 시계 `paused`, Start 준비 대기, Execution 트리·EndExec | 이벤트 대기·하위 실행 golden |
| LC6d 자원·지연·격리 | 유지 자원(획득 → 해제 → 저장, 실행·instance 범위 해제), 로드 요청 handle(host 2차), 활성 전환·파괴 지연 큐(적용 루프 64회), fault 위치별 처리와 격리(SL §8.3 표), 예산(instance·session — SL §9.3), CloseSession(ledger 0) | 누수 0 시험, 격리·예산 시험 |
| LC6e 시나리오·Gate | SL §2 예시 시나리오 전달 순서 golden, Gate G1(누수 0)·G4(fault 격리) | corpus `Scenario/*` 통과 |

## 2. 구조

- **`VmSession`(R1):** SL §9.1의 소유 범위 하나(Tryout session·dedicated server session). instance 표, 연결 표, 발행 큐, 재개 큐(시계별), 활성 전환·파괴 지연 큐, 실행 표(정지한 실행의 frame 묶음), 예산 장부, fault 보고를 가진다. 실행 문맥(`VmExecution`)은 session이 조각마다 빌려 쓴다(같은 session 안의 버퍼는 한 ledger — LC5 V12의 "실행 문맥 사이 버퍼 이동"은 session ledger 하나로 푼다).
- **instance(R2):** (host 객체 handle, 클래스, 상태, 필드 저장소, 실행 목록, 유지 자원 목록, 연결 목록). 상태는 Created → Awake → Enabled ↔ Disabled → Destroyed → Detached(격리는 Isolated). 필드 저장소는 session이 가진 bank별 slot 묶음이다(CLASS 구역의 배치 — Buf slot은 참조 하나). Sparks 클래스 필드의 `GETF`·`SETF`는 링크가 Field 참조를 "자기 모듈 클래스 필드"로 묶으면 VM이 instance 저장소를 직접 쓰고(host를 부르지 않는다), 엔진 필드만 host로 간다.
- **handle(R3):** instance는 host 객체에 붙는다(SL §4 — ScriptableComponent 등). Ref bank의 값은 host handle 그대로다. VM은 handle → instance를 session 색인(handle → instance 번호, 열린 주소 해시)으로 찾는다.
- **bytecode 형식 v3(R4):** CLASS 구역을 더한다 — 클래스(이름, 기반 클래스 참조, 엔진 기반 타입 이름, 필드 범위, hook 범위, handler 범위, 이벤트 범위, bank별 slot 수, 기본값 생성 함수 번호), 필드(이름, bank, slot 수, 위치 — TYPE 필드와 같은 규칙), hook(종류, 함수 번호), handler(경로 글자, 이벤트 이름, 함수 번호, 재진입 정책, await 여부), 이벤트(이름, 인자 서술자). 컴파일러가 클래스마다 기본값 생성 함수(`<Class>.$init` — 필드 초기값 식을 선언 순으로 `SETF`)를 만든다. verifier는 번호·범위·배치를 본다.
- **host interface 2차(R5):** `VmSessionHost`(VmHost 파생): 시계(GameClock·UIClock 값, 프레임 번호), 활성 전환·파괴 적용 요청(`RequestSetEnabled`·`RequestDestroy`), native 이벤트 연결·해제(발행자 handle, 이벤트 묶음, 연결 slot), native 이벤트 항목 넣기(host → session), 유지 자원 해제(`ReleaseHeld`), 로드 요청 시작·상태·결과·lease 반환, fault·진단 보고. session 쪽 진입은 `CreateInstance`·`RunHook`·`OpenPhase`·`ClosePhase`·`Enqueue`·`Resume`·`Detach`·`CloseSession`이다(SL §11 "host가 VM에 들어오는 것").

### 2.1 CLASS 구역(형식 v3 — LC6a)

- **클래스 항목:** 이름(문자열 — 표시 이름 `Door`), 기반(같은 모듈 클래스 번호, 다른 모듈 Sparks 클래스는 참조 이름 `<경로>:Class`, 엔진 기반은 엔진 타입 이름 — 셋 중 하나), 필드 범위(이 클래스가 선언한 필드만), hook 범위, handler 범위, 이벤트 범위, bank별 자기 필드 slot 수, 초기화 함수 번호(없으면 NoIndex).
- **필드 항목:** 이름, bank, slot 수, bank 안 위치(자기 클래스 구역 안 — instance 저장소에서는 기반 클래스 구역 뒤에 놓인다: bank마다 기반 사슬의 slot 수 합이 시작 위치). 객체 필드 참조(`Field` — `Door.Open`)는 링크가 같은 모듈 클래스 필드면 (클래스, 필드)로 묶는다(host 묶음 아님).
- **hook 항목:** 종류(Awake·Enable·Start·Tick·FixedTick·LateTick·Disable·Destroy·TriggerEnter·TriggerExit·CollisionEnter·CollisionExit·Validate), 함수 번호. 클래스당 종류마다 하나(누적은 기반 사슬을 따라 기반 먼저 — SL §5.1).
- **handler 항목:** 경로 글자(`Owner`·`Target.Current`·`Widget.RetryButton` — 빈 글자는 자기 이벤트), 이벤트 이름(`Damaged`·`Changed`), 함수 번호, 재진입 정책(없음·`@skipWhileBusy`·`@restart`), await 여부.
- **이벤트 항목:** 이름, 인자 서술자(SparksVm 6a 문법 — `x,f64>void`의 피연산자 부분).
- **초기화 함수(R6):** 컴파일러가 클래스마다 `<Class>.$init`(this 하나, 반환 없음)을 만든다. 본문은 그 클래스가 선언한 필드의 초기값(SL §3.2 — 초기값 식, 없으면 0 값 중 slot 0·null로 충분하지 않은 것: 문자열 `""`·빈 컨테이너·struct 기본값·단위 quat 등)을 선언 순으로 자기 필드에 쓴다. 초기값이 모두 0·null인 클래스는 함수가 없다. session은 instance를 만들 때 기반 사슬의 초기화 함수를 기반 먼저 부른다(이 실행은 Changed를 내지 않는다 — SL §5.3 "초기값").
- **verifier:** 클래스·필드·hook·handler·이벤트 범위가 표 안, 기반 클래스 번호가 앞선 클래스(순환 없음), 필드 배치(bank 0–2, Ref·Buf 1 slot, 위치가 자기 slot 수 안), hook 종류 중복 없음, 함수 번호가 함수 표 안, 초기화 함수의 매개변수 모양(Ref 1).

### 2.2 VmSession API(LC6a)

- `VmSession(allocator, host)` — 실행 문맥 하나(`Execution()`)와 instance 표를 가진다. `AddModule(linked)`는 session 모듈 목록에 더한다(같은 host로 링크한 모듈만 — 다른 모듈 기반 클래스·필드를 찾는 범위).
- `CreateInstance(handle, module, class)` — handle(host 객체, 살아 있어야 함)에 instance를 붙인다: 필드 저장소를 기반 사슬 전체 크기로 0·null로 만들고 기반 먼저 `$init`을 부른다(Changed 없음). 같은 handle에 두 번 붙이면 InvalidArgument. 초기화 fault는 instance를 만들지 않고 그 fault를 돌려준다(LC6d가 spawn 실패로 잇는다).
- `RunHook(instance, kind, scalars, refs)` — 그 종류의 hook을 기반 사슬 순(기반 먼저 — SL §5.1 누적)으로 부른다. 첫 fault에서 멈추고 그 결과를 돌려준다(격리는 LC6d).
- `Call(instance, module, function, …)` — 메서드를 this = instance handle로 부른다(`.run`의 `Class.Method(#this, …)`).
- `Detach(instance)` — 필드 버퍼를 놓고 표에서 뺀다(handle 색인도). 소멸자는 남은 instance를 모두 Detach한다(CloseSession — ledger 0).
- **필드 로컬 묶음(R2):** 링크는 Field 참조 이름의 주인이 같은 모듈 CLASS 구역의 클래스면 host에 묻지 않고 (클래스, 필드)로 묶는다. 실행 중 `GETF`·`SETF`는 실행 문맥의 session에서 handle의 instance를 찾아 저장소를 직접 쓴다. instance가 아니거나 session 밖이면 `ContractViolation`이다. 클래스 구역의 시작은 bank마다 기반 사슬(같은 모듈 기반, `<경로>:Class` 기반은 session 모듈에서 찾음, 엔진 기반에서 멈춤)의 slot 합이다 — 파생 클래스와 무관하게 같은 위치다.
- **`.run`(LC5 corpus)의 메서드 호출:** `Class.Method(#this, …)`는 `#this`에 그 클래스 instance가 없으면 먼저 `CreateInstance`(기본값 적용)하고 session으로 부른다. 가짜 host의 Sparks 필드 흉내(LC5d1)는 없앤다.

- **LC6a 리뷰 반영(2026-11-22):** 실행 문맥은 재진입하지 않는다 — Call 실행 중(host 콜백 안)에 같은 실행 문맥의 Call·`CreateInstance`·`RunHook`은 InvalidArgument다(spawn native가 instance를 만드는 경로는 LC6d의 지연 큐가 맡는다). Detach는 저장소까지 놓고 그 번호를 뒤의 `CreateInstance`가 다시 쓴다(host는 번호가 아니라 handle로 instance를 가리킨다). `CreateInstance`의 결과 명령 수·로그는 `$init` 사슬 전체다. 같은 경로의 다른 모듈은 `AddModule`이 거부한다. verifier는 클래스 필드의 밀집 배치(선언 순, bank별 합 = 클래스 slot 수)·모듈 안 클래스 이름 하나·hook 함수 모양(종류별 인자 최대 — 앞에서부터 필요한 만큼, this 필수, 반환 없음)·재진입 표시는 await handler만을 본다. 0 값이 단위값인 수학 타입(quat·mat4·Transform) 필드도 `$init`이 쓴다. `RunHook`은 Buf 인자(Collision의 `hit`)를 받는다.
- **알려진 한계(LC6a):** 다른 모듈 Sparks 클래스의 필드 참조(`<경로>:Base.Field` — 다른 모듈 기반에서 상속한 필드)는 아직 로컬로 묶지 않는다(host로 간다 — LinkImports 단계에서 session 모듈의 CLASS로 묶는 것은 LC6b 이후). 필드 접근은 명령마다 handle 색인·기반 사슬을 다시 찾는다(instance에 사슬 시작 위치 cache는 측정 뒤). `.run` 실행기는 instance가 붙어 있는 동안 줄마다의 ledger 0 검사를 하지 않고 사례 끝에 모두 Detach한 뒤 본다. 자기 이벤트의 handler는 `on this.Event`(actor 클래스 — 경로 참조 0개)이고 이름 하나짜리 `on X`는 hook 전용이다(SL §5.1).

### 2.3 발행·전달(LC6b)

- **handler 경로의 참조(R7):** CLASS의 handler 항목은 경로 글자 대신 경로 필드 참조 목록(`Field` 참조 번호 — 경로 마디마다 하나, 자기 이벤트는 0개)을 가진다. `Changed`는 마지막 마디가 감시 필드다(`on State.Changed` → 경로 `[State]`, 발행자는 그 필드를 가진 객체 = self). 그 밖의 이벤트는 경로 끝 객체가 발행자다(`on Owner.Damaged` → `[Component.Owner]`, `on Opened` → `[]` = self). 링크가 참조를 묶으므로 실행 중 이름으로 필드를 찾지 않는다(엔진 필드 `Owner`는 host 묶음).
- **연결 표(SL §5.5):** session이 가진다. 항목은 (발행자 handle, 이벤트 key, 구독 instance, handler — 모듈·함수 번호, 활성, 추가 순번). 이벤트 key는 Sparks 이벤트면 (모듈, `Event` 참조 이름 `Class.Event`), Changed면 (필드 참조 이름 + `.Changed`), native 이벤트면 host 묶음 번호다. instance의 Enable이 반환된 뒤 그 instance의 handler 경로를 풀어 항목을 더하고(경로 마디가 `Array<객체>`면 원소마다 — 같은 객체는 하나), Disable은 비활성으로 표시, Detach는 삭제한다. 실행 조각이 끝날 때 그 조각이 쓴 경로 원천 필드가 있으면 그 instance의 연결을 다시 풀어 추가·제거한다(유지되는 항목은 순번을 지킨다).
- **발행 큐(SL §5.4):** `EMIT Bx`(Ref 창 0번 발행자, 인자는 그 뒤)는 항목(발행자, 이벤트 key, 인자 사본 — Buf는 참조 +1, 깊이)을 session 큐 끝에 넣는다. 깊이: 단계를 시작한 조각의 발행은 1, 깊이 d 항목의 handler가 낸 발행은 d+1; 17이 될 항목을 넣으려는 조각은 `CascadeLimit` fault이고 같은 루트의 미전달 항목을 폐기한다. 큐 상한 262,144를 넘기면 그 조각의 `BudgetExceeded`.
- **전달:** 실행 조각(hook·handler·메서드)이 끝날 때마다 session은 큐를 FIFO로 비운다: 항목 하나를 꺼내 그 시점의 활성 연결(발행자·key가 같은 것)을 순번대로 실행하고 다음 항목으로 간다. 단계당 전달 항목 상한 65,536을 넘는 항목은 다음 단계로 이월하고, 이월 항목은 전달 직전 인자 객체의 생존을 다시 본다(`?` 없는 인자가 죽었으면 폐기·보고, `T?`는 null).
- **Changed(SL §5.3):** 조각 안에서 instance 필드를 처음 쓸 때 시작 값을 보관하고, 조각 끝에 끝 값과 다르면(Scalar 비트, 문자열 byte, 컨테이너·struct 원소별, 객체 동일성) Changed 항목을 넣는다(깊이 규칙 같음). Awake·Enable hook 안의 쓰기, 초기화, Disable·Destroy 문맥의 쓰기는 내지 않는다.
- **단계:** `OpenPhase()` — 새 단계(단계당 전달 수 초기화 — 이월 항목 전달). 모든 진입(`RunHook`·`Call`·`Emit`·`OpenPhase`)은 끝에 큐를 비운다(단계를 닫는 별도 호출은 없다 — 활성 전환·파괴 적용은 LC6d가 더한다). `.run` 지시: `phase`(결과 `ok (N queued)`), `emit #n Class.Event(인자)`(host 진입 발행 — 깊이 1, Class가 그 모듈 클래스가 아니면 엔진 이벤트 key), 실행된 handler는 결과 줄 뒤 `  deliver #구독자 <함수> -> 결과 (N steps)` 줄이다.
- **구현 세부(LC6b 확정):** key는 링크가 정한다(`VmEventKey` — 선언 모듈 경로와 `Class.Member`, 엔진 선언은 빈 경로). Changed 항목·연결의 key는 감시 필드의 key다(이벤트 key와 이름이 겹치지 않는다). 연결은 instance의 Enable hook 뒤에 기반 사슬의 handler 전부를 풀어 만들고, 활성 instance에서 경로 첫 마디 필드가 조각 안에서 바뀌면 그 필드를 원천으로 쓰는 handler를 다시 푼다(남는 발행자의 항목은 순번 유지). handler 인자는 선언한 만큼 bank마다 앞에서부터 넘긴다(배열 원천은 발행한 원소가 첫 Ref 인자). 반환값이 있는 메서드는 반환 slot·버퍼를 보관하고 전달한 뒤 실행 문맥의 반환 자리로 되돌린다. 이월 항목의 죽은 객체 인자는 지금 null로 넘긴다 — non-null 인자의 항목 폐기·보고는 인자 타입(서술자의 `?<x>`)을 대조하는 LC6d에서 한다.
- **LC6b 리뷰 반영(개정 3):**
  - **Changed 자리:** `SETF`(자기 모듈 Sparks 필드)가 조각에서 그 필드를 처음 쓸 때 큐에 Changed 항목을 넣어 자리를 확보한다(SL §5.3 — `Count = 1; emit E`는 Changed가 먼저). 큐 상한·연쇄 상한은 그 쓰기 명령의 fault(`BudgetExceeded`·`CascadeLimit`)이고 조용히 버리지 않는다. 조각 끝에 시작 값과 같으면 자리를 취소한다(항목의 발행자를 0으로 — 전달 때 건너뛴다). 자리는 활성 instance(Enable 반환 뒤·Disable 전)이고 억제 문맥(초기화·Awake·Enable·Disable·Destroy hook)이 아닐 때만 확보한다. 억제는 hook 사슬과 그 조각 끝 정리까지이고, 뒤따르는 전달(Enable 안의 `emit`을 받은 handler 등)은 억제하지 않는다.
  - **경로 원천:** 바뀐 필드의 key를 경로 첫 마디(Field 참조)의 key와 대조해 instance 클래스 사슬(다른 모듈 기반 포함)의 handler를 다시 연결한다. `on A.B.Changed`(마디 둘 이상)는 다시 연결한 뒤 그 handler에만 한 번 전달하는 항목을 넣는다(A가 null이 된 경우 포함 — 전달 때 instance가 그대로 활성이고 생성 번호가 같아야 한다). 상한에 걸리면 이 한 번은 버린다(조각이 이미 끝나 fault를 낼 명령이 없다). 깊은 마디(`Target.Inner`의 Inner가 바뀜)는 다시 연결하지 않는다 — 원천 instance의 쓰기만 본다(한계).
  - **진입 계약:** 실행 중(handler·hook의 native 호출 안)에 host가 부를 수 있는 진입은 `Emit`뿐이고 지금 조각의 깊이 + 1로 넣기만 한다. 그 밖의 진입(`CreateInstance`·`RunHook`·`Call`·`Detach`·`OpenPhase`·`Advance`·`FixedStep`·`AddModule`)은 실행 중이면 `InvalidArgument`다(활성 전환·파괴의 지연 적용은 LC6d). 진입마다 연쇄 루트·깊이를 새로 둔다(끊긴 진입의 루트를 잇지 않는다).
  - **Detach:** 그 instance의 조각 쓰기 기록(시작 값 버퍼)을 놓고 확보한 Changed 자리를 취소한다(번호를 재사용한 새 instance에 닿지 않게). instance마다 생성 번호(serial)를 둔다.
  - **남은 위험(LC6d 이후):** 큐 저장소는 큐를 다 비웠을 때만 앞으로 당긴다(과부하가 이어지면 자란다 — OpenPhase 압축), host `Emit`의 인자 수를 이벤트 모양과 대조하지 않는다(handler 선언보다 적으면 그 진입이 `InvalidArgument`), 전달 중 새로 생긴 연결도 같은 항목을 받는다(순번이 더 크면), 연결 찾기와 `NoteWrite`는 선형(발행자 색인은 측정 뒤), `VmValuesEqual`은 Map·Set을 삽입 순서로 비교한다.


### 2.4 async 실행(LC6c)

- **실행(Execution)과 정지(R9):** `AWAIT`·`AWAITEV`가 실행을 정지하면 VM은 실행 문맥의 frame stack 전체(frame 표와 bank별 레지스터 구간 — 바닥 frame부터 정지 frame까지)를 session의 실행 표 항목으로 옮긴다(Buf 레지스터의 참조는 옮긴다 — 복사·Retain 없음). `Call`의 결과는 `VmOutcome::Suspended`(대기 handle과 실행 번호)다. 재개는 그 항목을 실행 문맥으로 되돌리고 정지 명령 다음부터 이어 간다(결과는 `AWAIT`의 A — 결과 bank·slot은 EXTRA). await 지점의 생존 구간 표(SparksBytecode)는 검증용이며 저장은 frame 전체다(정지 실행은 많지 않고 frame은 작다 — 측정 뒤 생존 구간만 저장).
- **대기 handle:** Ref bank 값이며 상위 비트 `VmWaitTag`(1 << 63)로 객체 handle과 구별한다(host handle은 이 비트를 쓰지 않는다 — VmHost 계약). 대기 표(session)의 항목: 종류(Delay·NextTick·NextFixedTick·이벤트·All·Race·로드 요청), 시계(Game·UI), 기한, 등록 tick, 자식 범위(All·Race), 완료 표지. 언어 함수 `Delay`·`NextTick`·`NextFixedTick`·`All`·`Race`(LC5c에서 묶음 없이 링크하던 것)는 session이 대기 항목을 만들어 handle을 돌려준다.
- **시계와 재개(SL §6.1):** host가 `Advance(gameDelta, uiDelta, paused)`로 시계를 진행하고 프레임 번호를 올린다. 재개 단계(`ResumeStep(kind)` — 가변 tick 재개·고정 재개)는 기한이 된 대기를 (등록 tick, instance 안정 key, 등록 순번) 순으로 재개한다. 기본 대기의 `await`는 이미 완료돼도 항상 정지한다. 이벤트 대기는 전달 순회 안에서 연결 순서로 재개한다(발행자 파괴는 대기를 끝내지 않은 채 실행 종료 — SL §6.1).
- **All·Race:** 인자 대기를 모두 시작한 뒤 판정한다. All은 모두 완료되면, Race는 첫 완료(같은 처리 시점이면 인자 순)에서 완료하고 나머지를 취소한다. 하위 작업의 fault는 부모 실행의 fault다.
- **취소:** instance Disable·Detach·CloseSession은 그 instance의 정지 실행을 취소한다(frame의 Buf를 놓는다 — 유지 자원 해제는 LC6d). `@restart`는 같은 발행자로 시작된 정지 실행을 취소하고 새로 시작하며, `@skipWhileBusy`는 그런 실행이 있으면 새 이벤트를 버리고 횟수를 센다(디버거 표시).
- **async fn 호출:** `AWAIT`가 async 함수의 대기를 기다리는 형태(`await F()`)는 정지 지점이 아니다 — 호출된 함수 안의 기본 대기가 정지하면 호출 사슬 전체가 하나의 실행으로 정지한다(frame stack 전체를 옮기므로 자연히 된다).
- **`.run` 지시:** `advance <초>`(Game·UI 시계 진행·프레임 +1·가변 재개 단계), `fixed`(고정 재개), 정지한 실행의 결과는 `-> suspended`, 재개된 실행은 `  resume #구독자 <함수> -> 결과` 줄이다.
- **LC6c1 구현(개정 2):**
  - **API:** `VmSession::Advance(gameDelta, uiDelta)`(프레임 +1 — 앞 프레임에 등록한 `NextTick`, 등록 프레임 뒤이고 기한이 된 `Delay`를 완료), `FixedStep()`(앞 고정 step에 등록한 `NextFixedTick`), `SuspendedCount()`, `SkippedWhileBusy()`. 위 `ResumeStep(kind)`는 이 두 진입으로 나눴다(가변·고정 단계를 host가 따로 부른다). `paused`는 LC6c2.
  - **기한:** `Delay(s)`의 기한은 등록 시각 + max(s, 0)이고, 등록한 프레임 안에서는 완료하지 않는다(0초도 다음 프레임 — "기본 대기는 이미 완료돼도 정지").
  - **재개:** 완료된 최상위 대기의 정지 실행을 (등록 프레임, instance 번호, 등록 순번) 순으로 하나씩 되돌려 이어 간다. 되돌리기 전에 실행 문맥의 남은 frame·레지스터(앞 조각의 것)를 비운다 — 덧붙이면 frame base가 어긋난다(구현 중 발견, `Run/Async`의 같은 단계 재개 세 개가 고정한다). 기본 대기의 결과는 없다(결과 slot은 0·null). 재개한 조각 뒤에는 발행 큐를 비운다.
  - **All·Race:** 자식 대기에 부모 번호를 둔다(한 대기는 한 묶음의 인자만). 완료는 위로 전파하고, Race는 끝나지 않은 자식을 취소한다. 묶음 인자를 직접 기다리거나 같은 대기를 두 번 기다리면 ContractViolation이다(언어가 막는다 — 안전장치).
  - **취소:** Disable hook 실행 전과 Detach에서 그 instance의 정지 실행을 취소한다(대기는 cancelled, frame의 Buf는 놓는다). 정지한 hook은 끝난 것으로 보고 기반 사슬을 잇는다.
  - **재진입:** 정지 실행은 시작한 handler(모듈·handler 번호)와 발행자를 기억한다. 전달할 때 같은 (구독 instance, handler, 발행자)의 정지 실행이 있으면 `@skipWhileBusy`는 전달을 버리고 횟수를 세며, `@restart`는 그 실행을 취소하고 새로 시작한다.
  - **`await F()`(async 함수):** 컴파일러가 호출 결과를 대기 handle이 아니라 반환 값으로 내리고 `AWAIT`를 두지 않는다(`await a?.F()`도 같다). 이전 lowering은 호출 결과를 Ref 대기로 읽어 반환 값과 bank가 어긋났다(Codegen `Flow`·`Review` golden 갱신).
  - **세션 밖:** `AWAIT`와 대기 언어 함수는 session 밖 실행에서 ContractViolation이다(컴파일된 코드는 async 함수·handler만 기다리고 이들은 session으로 부른다 — 안전장치).
- **LC6c1 리뷰 반영(개정 4):**
  - **대기 표 재사용:** handle은 `VmWaitTag | 세대 << 32 | 번호`다. 재개(기다린 대기와 그 하위 묶음)·취소에서 번호를 돌려받고 세대를 올린다(옛 handle은 무효). 조각(Call·Continue 하나)이 끝나면 그 조각이 만든 대기 중 정지 실행이 기다리지 않는 묶음을 돌려받는다(언어는 대기를 바로 기다리거나 묶음에 넣는다 — fault·손상 대비). `while (true) { await NextTick(); }`는 대기 표를 1개로 유지한다(`stats` golden). 자식 목록은 firstChild·nextSibling이고 돌려받기·취소 전파·완료 전파는 재귀 없는 반복이다(깊은 묶음 사슬에도 stack 고정).
  - **한 번만 기다림:** 대기는 `awaited` 표지를 두어 동시·순차 모두 두 번 기다릴 수 없다(ContractViolation). 묶음 인자는 살아 있고, 끝나거나 취소되지 않았고, 기다린 적 없고, 다른 묶음의 인자가 아니어야 한다(중복 인자 포함 — 언어가 막는다, 안전장치).
  - **재개 단계:** 단계 시작 때 끝난 대기의 정지 실행을 모아 (등록 프레임, instance 생성 번호, 등록 순번)으로 정렬한 뒤 차례로 재개한다. 재개된 조각이 이미 끝난 대기를 기다리면 다음 단계에 재개한다(같은 단계에서 다시 고르지 않는다 — 끝나지 않는 반복 방지). `Advance`·`FixedStep`은 새 스크립트 단계다(단계 전달 수 초기화·이월 항목 전달·끝에 큐 비우기). delta는 유한한 0 이상이어야 한다(아니면 `InvalidArgument` — `.run` `advance -1 -> invalid`). `.run` `advance <Game 초> [<UI 초>]`로 두 시계를 따로 진행한다.
  - **재링크:** 정지 실행은 frame마다 모듈의 링크 세대를 보관하고, 재개 전에 링크·import 링크·세대가 그대로가 아니면 재개하지 않고 취소한다(SL §12.4 호환되지 않는 reload와 같은 처리 — 옛 pc·함수 번호로 새 모듈을 읽지 않는다).
  - **Random:** 정지 실행이 실행 문맥의 Random stream을 보관하고 재개가 이어 쓴다(다른 실행의 stream을 잇지 않는다 — SL §6.3).
  - **취소 범위:** Disable은 그 instance가 시작한 실행, Detach는 그에 더해 그 instance를 this(frame Ref 0번)로 쓰는 frame이 있는 실행(다른 instance가 부른 이 instance의 async 메서드 안의 정지)도 취소한다. Race가 진 자식과 그 하위 묶음은 취소 표시하고 묶음과 함께 돌려받는다.
  - **async 함수를 대기 값으로(`All(F(), G())`):** 하위 실행은 LC6c2다. 그때까지 컴파일러는 함수를 부르고 대기 자리를 null로 둔다 — 묶음이 ContractViolation으로 확정적으로 멈춘다(반환 창에 남은 값을 대기로 읽지 않는다, `Run/Async` `Group`).
  - **남은 것(LC6c2·LC6d):** 정지 실행의 frame·레지스터 사본과 대기 표를 실행 메모리 예산·ledger에 넣기(LC6d), schema `overridable` async 엔진 함수가 Sparks override로 풀릴 때의 계약(연동 단계), 정지 실행 찾기(`FindBusy`·취소)의 선형 탐색(측정 뒤 색인), `VmCallResult::suspension` 번호는 재개·취소 뒤 다시 쓰인다(host가 보관하지 않는다).

### 2.5 async 나머지(LC6c2)

- **이벤트 대기 handle(`WAITEV`, R13):** `WAITEV A B + EXTRA(이벤트 참조)`는 발행자 B(Ref)의 그 이벤트를 기다리는 대기 handle을 A(Ref)에 둔다. `await X.E`는 `WAITEV` + `AWAIT`로 내리고 `All`·`Race`의 이벤트 인자(`Race(Play(seq), Skip.Triggered)` — SL §6.1 SD41)도 같은 `WAITEV`로 대기 값을 만든다. 이전 `AWAITEV`는 없앤다(대기를 만드는 길이 하나 — verifier의 await 표는 `AWAIT`만). B가 null이거나 죽은 객체면 ContractViolation이다(언어는 non-null 발행자만 기다린다).
- **이벤트 대기의 완료:** 대기 항목은 (발행자, 이벤트 key, 등록 순번 — 연결 표와 같은 순번 공간, 등록 때 큐 항목 일련번호)를 가진다. 전달 순회(SparksRuntime 2.3)는 항목 하나를 꺼낼 때 그 항목보다 먼저 등록된(항목 일련번호가 등록 때 값보다 큰) 같은 (발행자, key)의 대기를 연결과 함께 순번대로 본다(SL §6.1 "이벤트 대기는 전달 순회 안에서 다른 연결과 같이 연결 순서로 재개"). 대기를 완료하고, 그것으로 최상위 대기가 끝난 정지 실행은 그 자리에서 재개한다(재개는 전달 하나 — `  resume` 줄, 뒤 처리는 handler 전달과 같다). 같은 항목을 등록한 조각이 받지 않는다(등록 뒤 처음 전달되는 것 — SD13).
- **발행자 파괴(SD13):** 발행자가 Sparks instance면 그 Detach에서, 엔진 객체면 재개 단계 시작(`Advance`·`FixedStep`)에서 생존을 보고(같은 단계의 전달이 먼저 — 큐를 비운 뒤) 그 발행자의 이벤트 대기를 "완료되지 않은 채 끝남"으로 처리한다: 단독 대기면 실행 취소, `Race`의 인자면 그 인자를 끝나지 않는 것으로 보고(남은 인자가 모두 그렇게 되면 실행 취소), `All`의 인자면 묶음 전체가 끝나 실행 취소.
- **하위 실행(`CALLW`, R14):** `All`·`Race`의 void async 함수 인자는 `CALLW Bx`(CALL과 같은 창·참조 규칙)로 부른다. VM은 부른 frame에 하위 실행 경계 표지를 두고 그대로 실행한다. (1) 경계 frame이 반환하면 끝난 하위 대기 handle을 부르는 쪽 Ref 창 0번에 둔다. (2) 경계 위에서 `AWAIT`가 정지하면 경계 frame부터 맨 위까지(그 frame들의 레지스터 구간 — base를 0으로 옮겨)만 하위 정지 실행으로 session에 옮기고, 끝나지 않은 하위 대기 handle을 부르는 쪽 Ref 창 0번에 둔 뒤 부르는 쪽을 `CALLW` 다음부터 잇는다(옮긴 구간의 Buf 참조는 옮기고 실행 문맥 쪽 자리는 null). 묶음은 인자를 모두 시작한 뒤 판정하므로(SL §6.1) 이미 끝난 하위 대기도 인자로 받는다 — 묶음을 만들 때 이미 끝난 자식으로 판정한다(All은 모두 끝났으면, Race는 첫 끝난 인자로).
- **하위 실행의 끝:** 하위 정지 실행은 다른 정지 실행과 같은 재개 순서로 재개한다. 반환하거나 `ENDEXEC`로 끝나면 그 하위 대기가 완료된다(`ENDEXEC`는 fault가 아니다 — 끝난 것으로 본다). fault면 부모 실행의 fault다(R15): 형제를 취소하고 부모 정지 실행을 그 fault(하위 실행의 위치)로 끝내 전달 기록에 남긴다. 부모가 취소되면(Race 패배·Disable·Detach) 하위 실행도 취소한다(대기를 돌려받을 때 그 하위 실행을 취소 — 깊이는 호출 깊이 상한으로 묶인다).
- **일시 정지(R16):** `Advance(gameDelta, uiDelta, paused)` — `paused`면 프레임 번호와 시계는 진행하되(host가 Game delta 0을 준다) `Clock.Game`의 Delay와 `NextTick`은 완료하지 않는다(SL §6.1 — 일시 정지 중 재개하지 않음). `Clock.UI` Delay는 완료한다. `FixedStep`은 host가 SingleStep에서만 부른다(일시 정지와 무관 — 호출 자체가 1회 진행). `.run`: `advance <Game> <UI> paused`.
- **Start 준비 대기:** `on Start`의 정지는 다른 hook과 같다(정지한 hook은 끝난 것으로 보고 사슬을 잇는다 — LC6c1). 준비 대기의 대상(로드 요청·Sequencer 준비·DataLayer)은 host 대기이므로 로드 요청 handle과 함께 LC6d·연동에서 더한다. 문맥 검사(Start에서 준비 대기만)는 컴파일러(LC3b)가 한다.
- **LC6c2 구현(개정 6):**
  - **번호:** `WAITEV`는 이전 `AWAITEV` 번호를, `CALLW`는 `EXTRA` 뒤 번호를 쓴다(기존 명령 번호를 바꾸지 않는다).
  - **완료 순서:** 하위 실행이 끝나 부모 묶음이 끝나면 부모를 곧바로(같은 단계·같은 전달 순회 안) 재개한다. `Race(Child(), NextTick())`처럼 같은 재개 단계에서 둘이 끝날 수 있으면 시계 판정(단계 시작의 대기 훑기)이 먼저라 기본 대기 인자가 이기고 하위 실행은 취소된다(`Run/Async` `RaceChild`).
  - **ENDEXEC:** 하위 실행 경계 위의 `ENDEXEC`는 그 하위 실행만 끝내고 끝난 하위 대기를 남긴다(정지 전이든 재개 뒤든 같다).
  - **가상 async 호출:** interface·open·override 메서드를 `All`·`Race` 인자로 쓰는 하위 실행(`CALLV` 경계)은 아직 없다 — 컴파일러가 부르고 대기 자리를 null로 둬 묶음이 ContractViolation으로 멈춘다(연동 단계에서 `CALLV` 경계를 더한다).
  - **시험:** `Run/Async`의 LC6c2 절(이벤트 대기 등록 전 항목 무시·전달 순회 안 재개, 발행자 detach의 단독·Race·All 처리, 하위 실행의 동기 반환·정지·fault 전파·Race 취소·3단 중첩, `advance … paused`), bytecode verifier 음성(`WAITEV` 참조·레지스터, `CALLW` 참조).
- **LC6c2 리뷰 반영(개정 7):**
  - **누적 호출 깊이:** 하위 정지 실행은 바깥 frame 수(경계 아래 깊이 + 그 실행의 누적 깊이)를 보관하고, 재개하면 실행 문맥의 깊이 기준(`depthBase_`)으로 두어 `CALL`·`CALLW`·`CALLV`의 깊이 상한(`MaxCallDepth`)이 부모 사슬 전체를 센다. 그래서 하위 실행 중첩과 그 끝·취소 전파의 재귀 깊이가 호출 깊이 상한으로 묶인다.
  - **하위 실행 끝의 순서:** 조각 끝 정리 → 부모 묶음에 반영(완료면 부모를 곧바로 재개 — 같은 연쇄 깊이·루트) → 전달 순회 밖이면 큐 비우기. 하위 실행이 끝나기 직전에 낸 발행은 그 Race 판정 뒤에 전달된다(전달 순회 안팎이 같다).
  - **할당 실패:** 재개한 하위 실행의 기록·정리가 할당에 실패해도 부모 묶음 반영(완료·fault·취소)은 끝까지 하고 첫 오류를 돌려준다. 재개 자체가 실패하면 하위 대기를 "완료되지 않은 채 끝남"으로 처리한다(부모가 영원히 기다리지 않는다).
  - **verifier:** `CALLW`는 Ref 창 0번이 있어야 한다(하위 대기 handle 자리 — 범위 밖 쓰기 방지).
  - **`a?.F()` 묶음 인자:** 결과는 대기 handle이고 a가 null이면 null이다. 묶음은 null 인자를 바로 끝난 인자로 본다(SL §6.1 `?.`).
  - **가상 async 함수 인자:** 부르지 않고 그 자리에서 `FAULT 4`(ContractViolation)로 멈춘다(동기로 부르면 그 안의 정지가 사슬 전체를 멈춰 묶음 의미가 틀어진다). `base.F()` 같은 정적 호출도 open·override 메서드면 여기에 든다(한계).
  - **엔진 발행자 생존:** `Advance`·`FixedStep`은 이월 항목을 먼저 전달한 뒤 발행자 생존을 본다(같은 단계의 전달이 파괴보다 먼저).
  - **시험:** `Run/Async` 리뷰 절(발행 뒤 같은 조각의 대기, Race에서 끝난 하위 실행이 자기 발행보다 먼저 승리, 전달 순회 안의 하위 완료와 부모 재개, 하위 실행끼리 Race, Race 안 하위 fault, 하위 실행의 `try` 실패(정지 전·뒤), `Peer?.Child()` null·있음, `@restart`·Disable과 하위 실행, 일시 정지 묶음), verifier 음성(`CALLW` Ref 창), 할당 실패 전수 `SparksConformance_RunGoldenAllocationFailureAsync`.
  - **남은 것:** 엔진 객체 발행자 소멸 시험(`.run`에 host 객체 파괴 지시가 없다 — 연동 단계), 깊은 하위 실행 사슬의 깊이 상한 시험(256단계 — 측정 시험과 함께), Drain의 이벤트 대기 찾기와 생존 검사의 선형 탐색(측정 뒤 발행자 색인), import 모듈 async 메서드의 `CALLW`·재링크 사례(다중 모듈 `.run` 뒤).
- **결정:** R13 대안(`AWAITEV` 유지 + 묶음 인자용 별도 명령)은 대기를 만드는 길이 둘이 되어 발행자 파괴·순서 규칙이 두 번 구현된다. R14 대안(하위 실행을 처음부터 별도 실행 문맥에서 돌림)은 인자 창·반환 규약을 다시 만들어야 하고 같은 조각 안의 동기 구간(첫 await 전)이 다른 문맥이 된다 — 경계 표지는 보통 호출과 같은 경로를 쓰고 정지할 때만 나눈다. R15는 SL §6.1 "하위 작업의 fault는 부모 실행의 fault가 되어 형제를 취소한다" 그대로다. R16 대안(host가 일시 정지 중 Advance를 부르지 않음)은 UI 시계 대기(`Delay(0.0, Clock.UI)`)가 멈춘다. R17: LC6c2 시험은 `Run/Async`에 이벤트 대기·발행자 파괴·하위 실행(반환·정지·fault·취소)·일시 정지 사례를 더하고 할당 실패 전수를 본다.

## 3. 순서

- **스크립트 단계(SL §5.4):** host가 `OpenPhase(kind)`로 열고, 그 단계의 실행 조각(hook·handler·재개)을 VM이 돌린 뒤 발행 큐를 비우고(연쇄 상한·전달 상한), 활성 전환 적용 → 파괴 적용 요청을 host에 넘기고(적용 루프 64회), `ClosePhase`로 닫는다. 단계 밖의 실행은 없다(엔진이 연 진입도 단계 하나를 연다).
- **조각 끝 처리:** 조각이 반환·정지·fault로 끝나면 그 조각이 처음 쓴 필드의 Changed 자리를 시작 값과 비교해 확정·취소하고(SL §5.3), 필드·배열 원천 연결을 갱신한다(SL §5.5).
- **재개 순서(SL §6.1):** (대기를 등록한 tick, instance 안정 key, 등록 순번). 이벤트 대기는 전달 순회 안에서 연결 순서로.

## 4. 시험

- **실행 golden은 `.run`을 넓힌다(LC6a 확정 — 별도 `.scenario` 형식을 두지 않는다):** 소문자 지시 줄이 instance를 다룬다 — `new #1 Door`(가짜 host 객체 #1에 Door instance를 붙이고 `$init`), `hook #1 Enable`·`hook #1 Tick(0.5)`·`hook #1 TriggerEnter(#2, #3)`(기반 먼저 누적 실행 — 인자는 실수·`#n`), `fields #1`(기반 사슬 전체 필드 표기 `Door(Open: true, …)`), `detach #1`. 결과는 `-> ok (N steps)`·`-> fault <종류> at 줄:열 (N steps)`·필드 표기이고 log·host 줄이 뒤따른다(함수 호출 줄과 같은 다시 쓰기). LC6b 이후 `phase`·`emit`·`advance` 지시를 같은 방식으로 더한다.
- 모든 단계는 할당 실패 전수 시험과 ledger 0(CloseSession 뒤)을 본다.

## 5. 결정(권장안 — 2026-11-22 확정)

- **R1 session이 실행 단위:** 대안(instance마다 실행 문맥)은 버퍼가 instance 사이를 오갈 때 ledger를 나눌 수 없다. session 하나의 ledger가 SL §9.3 session 예산과 맞는다.
- **R2 Sparks 필드 저장은 VM:** 대안(host가 저장 — LC5d1 가짜 host)은 필드마다 host 왕복이 들고 Changed 비교·reload 이전(R3 핫 리로드)을 VM이 할 수 없다. 엔진 필드만 host.
- **R3 handle 그대로:** 대안(VM 고유 instance 번호를 Ref에 둠)은 엔진 객체와 Sparks instance의 참조가 두 종류가 되어 `==`·`as?`가 복잡해진다.
- **R4 CLASS 구역:** 대안(함수 이름 규칙 `Door.on Enable`로 hook·handler를 찾음)은 경로·재진입 정책·await 여부를 담을 수 없다. 형식 버전을 3으로 올린다(이전 형식은 받지 않는다 — 초기 단계, 하위호환 없음).
- **R5 host interface는 VmHost 파생:** LC5 링크·묶기를 그대로 쓰고 실행 모델에 필요한 것만 더한다.
- **R6 기본값 생성 함수:** 컴파일러가 클래스마다 만든다(struct 기본값을 생성 자리에서 내리는 LC5 V10과 같은 방향). 대안(CLASS 구역에 상수 원형)은 식 초기값을 담지 못한다.
- **R7 handler 경로는 필드 참조:** 대안(경로 글자를 실행 중 이름으로 풀기)은 상속 필드·엔진 필드의 주인 타입을 실행 중에 알 수 없고 매번 이름을 찾는다.
- **R8 연결 표는 명시 항목:** 대안(전달할 때마다 모든 instance의 경로를 풀기)은 연결 순서(SL §5.4 "항목이 delegate에 추가된 순서")를 지킬 수 없고 O(instance × handler)다.
- **R9 정지는 frame stack 전체를 옮긴다:** 대안(생존 구간만 저장)은 저장량이 적지만 frame 사이의 창·부르는 쪽 레지스터를 다시 맞추는 규칙이 필요하다. 정지 실행 수와 frame 크기가 작아 전체 이동이 단순하고 결정적이다(측정 뒤 바꾼다).
- R10–R12(취소 시 자원 해제 순서, 격리 시 연결 삭제 순서, reload 경계)는 각 하위 단계 착수 때 이 절에 확정한다.
