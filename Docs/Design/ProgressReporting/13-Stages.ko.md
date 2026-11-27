# 작업 진행 보고와 취소 — 13. 단계

[목차](README.ko.md)

## 13. 단계

| 단계 | 내용 | 완료 조건 |
|---|---|---|
| PG1 | `CancelToken`·`CancelSource` Core 이동, `CheckCancelled`, 연결 취소, 문서 참조 갱신 | 호출처 전부 이동, 시험 1–2, 정책 통과 — **완료**(2026-11-27, 시작 화면 세션. `Core/CancelToken.h`, 시험 `CancelToken_*` 6개) |
| PG2 | `Progress` 모듈: hub·작업·바인딩·구간·계수·스냅샷·사건 기록·UI 펌프 hook·`ProgressParallelFor` | 시험 3–14, 요구 9 비용 측정치 기록 — **완료**(2026-11-27, 시작 화면 세션. `Core/Source/Runtime/Progress`, 시험 `Progress_*` 21개, 비용 15.5절·`Programs/ProgressBench`) |
| PG3 | `ConsoleProgressReporter`·`ProgressPumpThread`·`ProgressJsonlWriter`·`ChildProgressReader`·`ParentControlListener` | 시험 15–21 |
| PG4 | `PendingWorkRegistry`·`PendingWorkSource`(Engine) | 시험 22–26 |
| PG5 | 첫 적용: 셰이더 컴파일 관리자·`shader-compile`(렌더 세션), 쿠킹·임포트(에셋 세션) | 실제 명령의 진행 줄·JSONL·취소 확인 |
| PG6 | 에디터 표시(진행 창·알림·상태 표시줄) | 에디터 단계에서 — UiToolkit 스펙에 연결 |
