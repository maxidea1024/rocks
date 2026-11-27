# 작업 진행 보고와 취소 — 17. 대응표

[목차](README.ko.md)

## 17. 대응표

| Rocks | UE 5.8 |
|---|---|
| `ProgressScope`·`EnterStep`·`Tick` | `FScopedSlowTask`·`EnterProgressFrame`·`TickProgress` |
| 스레드별 구간 스택(Primary·Secondary) | `FSlowTaskStack`(게임 스레드 하나) |
| UI 펌프 hook | `FFeedbackContext::RequestUpdateUI`·`UpdateUI` |
| `ProgressTaskDesc::presentation = Blocking` + 진행 창 | `FSlowTask::MakeDialog`·`MakeDialogDelayed` |
| `presentation = Background` | `FAsyncTaskNotification`, Slate `StartProgressNotification` |
| `presentation = Hidden` | `ESlowTaskVisibility::Invisible` |
| `ProgressHub` | `FFeedbackContext`(`GWarn`), `FAsyncTaskNotificationFactory`, `IProgressNotificationHandler` |
| `CancelToken`·`RequestCancel` | `FSlowTask::ShouldCancel`, `FFeedbackContext::ReceivedUserCancel` |
| `ProgressOutcome` | `EAsyncTaskNotificationState`(Success·Failure) |
| 구간 trace span(깊이 2) | `FSlowTask` `RegionId` |
| `PendingWorkRegistry`·`PendingWorkSource` | `FAssetCompilingManager`·`IAssetCompilingManager` |
| `RunBefore` | `GetDependentTypeNames` |
| `FinishFor`·`CancelFor`·`Shutdown` | `FinishCompilationForObjects`·`MarkCompilationAsCanceled`·`Shutdown` |
| `ApplyCompleted(PendingWorkPumpParams)` | `ProcessAsyncTasks(FProcessAsyncTaskParams)` |
| 상태 표시줄 대기 작업 합계 | `FAsyncCompilationNotification::Update` |
| `ConsoleProgressReporter` | 명령줄 `FFeedbackContext`·`FCoreAsyncTaskNotificationImpl` 로그 |
| `ProgressJsonlWriter`·`ChildProgressReader`·`ParentControlListener` | (없음) |
