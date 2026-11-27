# 작업 진행 보고와 취소 — 3. 구조 개요

[목차](README.ko.md)

## 3. 구조 개요

| 층 | Rocks 이름 | 위치 | 하는 일 | UE 대응 |
|---|---|---|---|---|
| 취소 | `CancelSource`·`CancelToken` | Core | 요청 쪽이 걸고 작업 쪽이 본다, 연결 취소 | `FSlowTask::ShouldCancel`·`FFeedbackContext::ReceivedUserCancel`의 일반화 |
| 작업 | `ProgressTaskHandle`·`ProgressTaskDesc` | Progress | 사용자에게 보이는 작업 하나 | `FAsyncTaskNotification` + 바깥 `FSlowTask` |
| 바인딩 | `ProgressBinding`·`ProgressBindingSuspend` | Progress | 스레드를 작업에 붙임/잠시 뗌 | (UE 없음 — 게임 스레드 하나) |
| 구간 | `ProgressScope` | Progress | 바인딩된 작업 아래 중첩 가중 구간 | `FScopedSlowTask`·`FSlowTaskStack` |
| 계수 | `ProgressCounter` | Progress | 여러 스레드가 올리는 완료 수·늘어나는 총량 | (UE 없음 — 관리자 남은 수로 대신) |
| 중계 | `ProgressHub` | Progress | 작업 기록·스냅샷·사건 기록·취소 요청·UI 펌프 hook | `FFeedbackContext`(`GWarn`)·알림 factory·`IProgressNotificationHandler` |
| 받는 쪽 | `ConsoleProgressReporter`·`ProgressJsonlWriter` | Progress | 사람이 읽는 로그 줄·기계 판독 줄 | 명령줄 `FFeedbackContext`·`FCoreAsyncTaskNotificationImpl` |
| 프로세스 경계 | `ChildProgressReader`·`ParentControlListener` | Progress | 자식 진행 줄 → 부모 하위 작업, 취소 줄 전달 | (UE 없음) |
| 대기 작업 등록소 | `PendingWorkRegistry`·`PendingWorkSource` | Engine | 비동기 빌드 종류별 남은 수·끝내기·취소·결과 적용 | `FAssetCompilingManager`·`IAssetCompilingManager` |

**모듈 배치.**
- `Progress` 모듈은 `Core/Source/Runtime/Progress`에 둔다. 의존은 `Core`(`PlatformBase`를 함께 본다 — 잠금·원자·시계)·`Localization`(`Text`)·`Task`(`ProgressParallelFor` 헤더 템플릿)가 공개, `TraceCapture`(span)가 비공개다. PG3의 자식 프로세스 읽기는 `Process`(PlatformBase)를 쓴다. 객체 시스템·렌더러·UI에 의존하지 않으므로 Developer 도구·명령·런타임이 모두 쓴다. UE도 `FSlowTask`·`FFeedbackContext`·`FAsyncTaskNotification`을 Core에 둔다. Rocks의 Task·LogHost·TraceCapture도 Core 층의 별도 모듈이다.
- `PendingWorkRegistry`는 `Engine` 모듈(`Engine/Source/Runtime/Engine`)에 둔다. 대상이 객체(`Object*`)이고 결과 적용이 게임 스레드 프레임에 묶이기 때문이다(UE도 Engine 모듈). Developer 모듈(셰이더 컴파일 관리자 등)은 Engine에 의존하지 않고 남은 수·끝내기·취소 API만 내놓는다. Engine 쪽 어댑터가 그것을 `PendingWorkSource`로 감싸 등록한다.
- 공개 헤더에 쓰기 가능한 정적 저장소·`thread_local`을 두지 않는다(BuildTargets BS8c). hub 저장소와 스레드별 바인딩 스택은 `Progress` cpp 안에 있고, `ProgressScope` 생성·`EnterStep`은 export된 out-of-line 함수다.
