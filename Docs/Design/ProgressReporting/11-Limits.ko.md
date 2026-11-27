# 작업 진행 보고와 취소 — 11. 한도 상수

[목차](README.ko.md)

## 11. 한도 상수

| 이름 | 값 | 근거 |
|---|---|---|
| `DefaultModalProgressDelay` | 1.0 s | UE `Engine/Source` 지연 창 최빈값 |
| `ProgressUiPumpInterval` | 0.2 s | UE `MinUpdateTimeS` |
| `DefaultConsoleProgressIntervalNanoseconds` | 2 s | 로그 양 |
| `DefaultJsonlProgressIntervalNanoseconds` | 0.2 s | UI 갱신 상한과 같음 |
| `ChildControlPollMilliseconds` | 0.1 s | 취소 전달 지연 상한 |
| `DefaultChildCancelGraceMilliseconds` | 10 s | cancel 줄 뒤 강제 종료까지 |
| `ProgressCancelledExitCode` | 130 | 취소로 끝난 명령의 종료 코드 |
| `ProgressJsonlVersion` | 1 | JSONL 형식 판 |
| `MaxLiveProgressTasks` | 4096 | 셰이더 종류별·패키지별 하위 작업 여유 |
| `MaxProgressBindingDepth` | 16 | 중첩 작업 |
| `MaxProgressBindingSuspendDepth` | 16 | `ProgressBindingSuspend` 겹침(넘으면 Fatal — 코드 결함) |
| `MaxProgressScopeDepth` | 64 | UE 실사용 깊이보다 넉넉 |
| `MaxReportedProgressDepth` | 4 | 진행 창 줄 수 |
| `MaxTracedProgressDepth` | 2 | trace span 깊이(UE `RegionId`와 같음) |
| `DefaultProgressEventLogCapacity` | 1024 | 소비자 Pump 간격 동안의 시작·끝 |
| `MaxProgressEventLogCapacity` | 2²⁰ | 설정 검사 상한 |
| `ProgressWorkTolerance` | 1e-9(총량 대비) | 부동소수 합(0.1 + 0.2)의 작은 넘침은 `EnterStep` 경고에서 뺀다 |
| `MaxProgressJsonlLineBytes` | 64 KiB | 줄 버퍼 |
| `MaxProgressJsonlTextBytes` | 8 KiB | 문구 칸 하나(이스케이프 6배여도 줄 상한 안) |
| `MaxChildProgressTasks` | 4096 | `MaxLiveProgressTasks`와 같음 |
| `MaxCancelChainDepth` | 8 | 연결 취소 사슬 |
