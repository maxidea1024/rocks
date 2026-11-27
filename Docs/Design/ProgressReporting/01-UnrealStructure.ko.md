# 작업 진행 보고와 취소 — 1. UE 5.8의 구조

[목차](README.ko.md)

## 1. UE 5.8의 구조

UE는 진행 보고를 세 층으로 나눈다.

1. **막는 작업의 중첩 진행 범위.** `FScopedSlowTask`(`SST`)가 `FSlowTask`(`ST`)를 감싼다. 범위마다 전체 작업량(`TotalAmountOfWork`, 임의 단위 float, 0이면 1로 바꿈 — `STC` 55–58행)과 기본 문구를 갖는다. `EnterProgressFrame(work, text)`가 앞 구간을 끝내고 다음 구간에 들어간다. 총량을 넘으면 잘라 내고 ensure를 낸다(`STC` 196–197행). 안쪽 범위가 끝나면 바깥의 현재 구간이 채워진다(`STC` 159–166행). 범위는 `FFeedbackContext`(`FC`, 전역 `GWarn`)의 `FSlowTaskStack`(`STS`)에 쌓이고, 전체 비율은 스택을 따라 곱해 낸다(`STSC` 6–23행). 창은 `MakeDialog`·`MakeDialogDelayed(threshold)`로 열고, 취소 단추를 줄 수 있으며 `ShouldCancel()`로 묻는다. 표시 정책은 `ESlowTaskVisibility`(Default·ForceVisible·Important·Invisible)다. 지연 창 임계값은 호출자가 고르며 `Engine/Source` 안 호출은 1.0 s(23곳)·3.0 s(14곳)·0.5 s(5곳)이다(플러그인까지 세면 0.5 s가 33곳으로 둘째).
   - **게임 스레드 전용이다**(`STC` 48행 `bEnabled(... && IsInGameThread())`, 252행 `check(IsInGameThread())`). 이유는 표시 방식에 있다. 범위가 진행할 때마다 `FFeedbackContext::RequestUpdateUI`가 초당 최대 5번(`FCC` 187행 `MinUpdateTimeS = 0.2`) **작업 도중에 UI를 직접 펌프**한다(`FCC` 184–207행). 막는 작업이 게임 스레드에서 돌아도 창이 갱신되는 것은 이 때문이다.
   - 범위별 trace 영역을 깊이 2까지 남긴다(`STC` 18·105행 `RegionId`).
2. **백그라운드 작업 알림.** `FAsyncTaskNotification`(`ATN`)이 제목·진행 문구·상태(`EAsyncTaskNotificationState` None·Pending·Success·Failure·Prompt)·취소 가능·성공/실패 뒤 열어 둠(`bKeepOpenOnFailure`)을 갖는다. 실제 표시는 factory로 고른 구현이 한다(`CATN` 162행 `RegisterFactory`). Core 기본 구현은 로그 범주로 적고(headless), 에디터는 Slate 알림을 띄운다. 비율 막대가 있는 알림은 Slate `StartProgressNotification(text, totalWork)`·`UpdateProgressNotification(handle, done, total)`(`NM` 112행)이고 여러 알림의 작업량은 합산된다. 총량이 늘면 막대가 뒤로 갈 수 있다고 주석이 인정한다(`NM` 118행).
3. **비동기 컴파일 관리자 등록소.** 셰이더·텍스처·정적 메시 등 비동기 빌드마다 `IAssetCompilingManager`(`IACM`)를 구현한다: 남은 수(`GetNumRemainingAssets`), 대상별 끝내기(`FinishCompilationForObjects`, 51행), 대상별 취소 힌트(`MarkCompilationAsCanceled`, 59행), 전부 끝내기(`FinishAllCompilation`), 남은 일 취소 후 안전할 때까지 대기(`Shutdown`, 69행), 프레임마다 결과 적용(`ProcessAsyncTasks(FProcessAsyncTaskParams)` — `bLimitExecutionTime`·`bPlayInEditorAssetsOnly`, 14–21행), 먼저 처리할 다른 종류(`GetDependentTypeNames`). `FAssetCompilingManager`(`ACM`)가 등록받아 의존 순서로 정렬하고(순환이면 등록은 받고 의존만 무시하며 Error 로그 — `ACMC` 636–640행), 저장·쿠킹·PIE 전에 전부 끝낸다. 상태 표시줄의 "Compiling Shaders (123)"은 `FAsyncCompilationNotification::Update(NumJobs)`(`ACH`)이고, 막는 대기에는 `FScopedSlowTask`로 "Compiling Shaders ({0})"을 띄운다(`SCM` 1856·1985행).

셰이더 컴파일 워커는 묶음이 끝날 때 결과를 한꺼번에 돌려주므로 진행은 묶음 단위로 오른다. 묶음 크기 기본값은 10이다(`Config/BaseEngine.ini` 2191행 `MaxShaderJobBatchSize=10`).
