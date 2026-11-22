# Sparks 실행 모델(LC6)

이 문서는 Sparks 언어 코어의 LC6(실행 모델)을 정한다. 언어 의미는 SparksLanguage(SL) §4.3·§5·§6·§7·§8.3·§9가 정본이고, 이 문서는 그 의미를 VM 쪽에서 어떻게 실현하는지(자료 구조·순서·host interface 2차·bytecode 형식 v3·시험)를 정한다. LC5(SparksVm.ko.md)의 링크·host interface 1차 위에 쌓는다.

- 개정 1(2026-11-22): 첫 판. 하위 단계 LC6a–LC6e, 결정 R1–R12.

## 1. 하위 단계

| 단계 | 내용 | 완료 기준 |
|---|---|---|
| LC6a session·instance·hook | `VmSession`(실행 단위 — SL §9.1 소유 범위), bytecode 형식 v3의 CLASS 구역(클래스 필드 배치·기본값 생성 함수·hook 표·handler 표·이벤트 표), instance 생성(필드 저장소는 session — Sparks 클래스 필드의 `GETF`·`SETF`는 VM이 직접), 수명 hook 실행(Awake·Enable·Start·Tick 계열·Disable·Destroy — 기반 먼저 누적), Detach, 스크립트 단계 열기·닫기 | `.scenario` golden(아래 §6)의 hook 순서·필드 값 |
| LC6b 발행·전달 | `emit`·발행 큐(FIFO)·연결 표(slot·generation)·`on 경로.이벤트` 연결(자기 필드·객체 필드·배열 원천·native 이벤트는 host 2차)·Changed(조각 값 기준)·연쇄 깊이 상한 16·전달 상한·이월 재검증 | 전달 순서 golden, CascadeLimit·BudgetExceeded 시험 |
| LC6c async 실행 | 실행 조각과 정지(`AWAIT`·`AWAITEV` — frame 보관), 대기(`Delay`·`NextTick`·`NextFixedTick`·이벤트·`All`·`Race`), 재개 큐(순서 key — SL §6.1), 취소(await 지점), 재진입 정책(`@skipWhileBusy`·`@restart`), Execution 트리·EndExec, Start 준비 대기 | 재개 순서·취소 golden |
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

## 3. 순서

- **스크립트 단계(SL §5.4):** host가 `OpenPhase(kind)`로 열고, 그 단계의 실행 조각(hook·handler·재개)을 VM이 돌린 뒤 발행 큐를 비우고(연쇄 상한·전달 상한), 활성 전환 적용 → 파괴 적용 요청을 host에 넘기고(적용 루프 64회), `ClosePhase`로 닫는다. 단계 밖의 실행은 없다(엔진이 연 진입도 단계 하나를 연다).
- **조각 끝 처리:** 조각이 반환·정지·fault로 끝나면 그 조각이 처음 쓴 필드의 Changed 자리를 시작 값과 비교해 확정·취소하고(SL §5.3), 필드·배열 원천 연결을 갱신한다(SL §5.5).
- **재개 순서(SL §6.1):** (대기를 등록한 tick, instance 안정 key, 등록 순번). 이벤트 대기는 전달 순회 안에서 연결 순서로.

## 4. 시험

- `.scenario` golden(LC6a부터): 사례 소스(클래스들)와 시나리오 줄(`spawn Door #1`, `enable #1`, `phase Tick`, `emit #1.Opened(#2)`, `advance 0.5`, `destroy #1` …)을 가짜 host가 해석해 실행하고, hook·handler 실행 순서·필드 값·host 호출·fault를 기록한 줄과 비교한다(`.run`과 같은 다시 쓰기 흐름). 형식은 LC6a에서 확정해 §6에 적는다.
- 모든 단계는 할당 실패 전수 시험과 ledger 0(CloseSession 뒤)을 본다.

## 5. 결정(권장안 — 2026-11-22 확정)

- **R1 session이 실행 단위:** 대안(instance마다 실행 문맥)은 버퍼가 instance 사이를 오갈 때 ledger를 나눌 수 없다. session 하나의 ledger가 SL §9.3 session 예산과 맞는다.
- **R2 Sparks 필드 저장은 VM:** 대안(host가 저장 — LC5d1 가짜 host)은 필드마다 host 왕복이 들고 Changed 비교·reload 이전(R3 핫 리로드)을 VM이 할 수 없다. 엔진 필드만 host.
- **R3 handle 그대로:** 대안(VM 고유 instance 번호를 Ref에 둠)은 엔진 객체와 Sparks instance의 참조가 두 종류가 되어 `==`·`as?`가 복잡해진다.
- **R4 CLASS 구역:** 대안(함수 이름 규칙 `Door.on Enable`로 hook·handler를 찾음)은 경로·재진입 정책·await 여부를 담을 수 없다. 형식 버전을 3으로 올린다(이전 형식은 받지 않는다 — 초기 단계, 하위호환 없음).
- **R5 host interface는 VmHost 파생:** LC5 링크·묶기를 그대로 쓰고 실행 모델에 필요한 것만 더한다.
- **R6 기본값 생성 함수:** 컴파일러가 클래스마다 만든다(struct 기본값을 생성 자리에서 내리는 LC5 V10과 같은 방향). 대안(CLASS 구역에 상수 원형)은 식 초기값을 담지 못한다.
- R7–R12(전달 상한 이월·재검증, 정지 frame 보관 형식, 취소 시 자원 해제 순서, 격리 시 연결 삭제 순서, reload 경계, 결정적 순서 key)는 각 하위 단계 착수 때 이 절에 확정한다.
