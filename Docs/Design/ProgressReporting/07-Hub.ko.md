# 작업 진행 보고와 취소 — 7. 중계(`ProgressHub`)

[목차](README.ko.md)

## 7. 중계(`ProgressHub`)

### 7.1 초기화·시계

- 프로세스에 하나다. 저장소는 `Progress` cpp 안에 있다. 엔진/도구 시작이 `Initialize`, 종료가 `Shutdown`을 부른다.
- `ProgressHubSettings`: 시계 `uint64 (*nowNanoseconds)(void*)`·context(기본은 단조 시계, 시험은 가짜 시계), 사건 기록 용량(1–`MaxProgressEventLogCapacity`).
- `Shutdown` 뒤 다시 `Initialize`할 수 있다(켜짐마다 세대가 바뀌고, 앞 세대의 handle·계수는 계속 `ShuttingDown`·no-op이다). 작업 id와 사건 번호는 프로세스 전체에서 이어 간다.
- `Stats()`: 이번 켜짐의 입력 검사 Warning 수(처음 한 번만 남기므로 0 또는 1)·Finish 없이 사라진 handle 수·살아 있는 작업을 남긴 채 닫힌 횟수. 로그 sink 없이 시험·진단이 본다.

### 7.2 스냅샷(pull)

- 받는 쪽은 자기 빈도로 `Snapshot(out, cursor)`를 부른다. 보고 스레드에서 받는 쪽 코드를 부르지 않는다(UI 펌프 hook은 UI 스레드 자신의 호출이다).
- `ProgressSnapshot`은 살아 있는 작업의 트리 복사본이다. 작업마다 id·부모·제목·깊이별 문구·작업 문구·표시 방식·모양·보이는 비율 `D`·자기 진행 `selfFraction`(하위 작업을 섞지 않은 `S` — 뒤로 갈 수 있다. 자기 단계만 보이는 받는 쪽, 예: 시작 화면 7.2절)·완료/총량·시작 시각·마지막 진행 시각·취소 가능·취소 요청됨. 문구는 `Text::Clone`으로 담는다(스냅샷은 소비자 소유, 해제도 소비자 스레드).
- **마지막 진행 시각**은 hub가 보고를 받을 때 재지 않는다(보고 경로는 잠금·시계 없음). 스냅샷이 합성 값 `F`의 증가나 완료 수 변화를 본 때의 시각이다. 그래서 해상도는 가장 잦은 소비자의 스냅샷 주기다.
- 스냅샷의 할당은 소비자가 준 allocator에서 한다. 같은 `ProgressSnapshot` 객체를 다시 쓰면 용량을 재사용한다.

### 7.3 사건 기록과 소비자별 cursor

- 시작·끝 사건은 hub 잠금 안에서 일련 번호(64-bit, 1부터)를 붙여 고리 버퍼(`DefaultProgressEventLogCapacity` = 1024)에 쌓는다. 잠금 안에서 쌓으므로 순서는 전체 순서다: 하위 작업의 시작은 부모 시작 뒤이고(부모가 살아 있어야 `Begin`이 성공), 부모가 끝날 때 하위 작업의 끝을 먼저 쌓는다.
- 사건: `Begin{id, parent, title, presentation, cancel}`, `End{id, outcome, detail, 마지막 D}`.
- `Snapshot(out, cursor)`는 `cursor` 뒤의 사건을 `out.events`에 담고 `cursor`를 옮긴다. `cursor`는 소비자가 그대로 보관하는 불투명 값이다(구현: 다음에 읽을 사건 번호 — 0을 "지금부터"로 쓰므로 마지막 번호가 아니라 다음 번호를 둔다. 사건이 하나도 없을 때의 0과 섞이지 않는다). 소비자마다 자기 cursor를 가지므로 여러 소비자가 서로의 끝 기록을 빼앗지 않는다. cursor 0은 "지금부터"다(처음 호출은 살아 있는 트리만 받고 cursor를 현재 번호로 맞춘다).
- 고리가 넘쳐 놓친 사건 수는 `out.lostEvents`에 담는다(소비자는 "N개 사건 누락"을 한 번 보고).
- push 관찰자는 두지 않는다(15절 버린 대안 (나)).

### 7.4 취소

- `RequestCancel(id)`는 그 작업의 hub 소유 `CancelSource`를 취소한다. 하위 작업은 연결 취소로 함께 취소된다(하위 작업의 source는 부모 token에 연결해 만든다).
- `RequestCancelAllTopLevel()`은 취소 가능한 최상위 작업을 모두 취소한다. 신호 처리기에서 부르지 않는다(잠금을 쓴다). Ctrl-C는 Launch의 원자 표시(`Launch/Application.h` `IsTerminationRequested`)를 펌프 주인이 보고 이 함수를 부른다(9.1절).
