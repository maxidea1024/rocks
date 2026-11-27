# 작업 진행 보고와 취소 — 11. 한도 상수

[목차](README.ko.md)

## 11. 한도 상수

| 이름 | 값 | 근거 |
|---|---|---|
| `kDefaultModalProgressDelay` | 1.0 s | UE `Engine/Source` 지연 창 최빈값 |
| `kUiPumpInterval` | 0.2 s | UE `MinUpdateTimeS` |
| `kDefaultConsoleProgressInterval` | 2 s | 로그 양 |
| `kDefaultJsonlProgressInterval` | 0.2 s | UI 갱신 상한과 같음 |
| `kChildControlPollInterval` | 0.1 s | 취소 전달 지연 상한 |
| `kMaxLiveProgressTasks` | 4096 | 셰이더 종류별·패키지별 하위 작업 여유 |
| `kMaxProgressBindingDepth` | 16 | 중첩 작업 |
| `kMaxProgressScopeDepth` | 64 | UE 실사용 깊이보다 넉넉 |
| `kMaxReportedProgressDepth` | 4 | 진행 창 줄 수 |
| `kProgressEventLogCapacity` | 1024 | 소비자 Pump 간격 동안의 시작·끝 |
| `kMaxProgressJsonlLineBytes` | 64 KiB | 줄 버퍼 |
| `kMaxChildProgressTasks` | 4096 | `kMaxLiveProgressTasks`와 같음 |
| `kMaxCancelChainDepth` | 8 | 연결 취소 사슬 |
