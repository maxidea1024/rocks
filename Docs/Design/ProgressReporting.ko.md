# 작업 진행 보고와 취소 설계

기준일: 2026-11-23, 개정 1(초안, 교차 검증 1차 반영). 사용자 지시("시스템 전반에 걸쳐 에디터에서 프로그레스 보여주는 체계가 있어야 함. 셰이더만의 문제가 아님", "스펙만 잡아놓고 다른 세션에서 작업")로 쓴 설계다. 이 문서를 쓴 렌더 세션은 구현하지 않는다. 결정은 권장안으로 확정했다(사용자 기준 "결정사항들은 권장안대로", 애매하면 UE 5.8 방식).

**범위.** 오래 걸리는 작업(임포트·쿠킹·에셋 너겟 일괄 요청·셰이더 컴파일·텍스처/오디오/메시 빌드·언리얼 교환·Sparks 컴파일·저장)이 진행과 취소를 한 가지 방식으로 알리게 한다. 받는 쪽은 나중에 만들 에디터(진행 창·알림·상태 표시줄), 명령줄 도구(사람이 읽는 줄), 자동화(기계 판독 줄), trace다. 이 문서는 데이터·스레드·프로세스 경계 계약과 API를 정한다. 에디터 화면 모양은 계약의 소비자로만 적는다(UiToolkit·에디터 단계에서 그린다).

**착수 시점.** 고정 구현 순서(에셋 A4–A6 → 화면 마일스톤)를 바꾸지 않는다. 단계(PG — 13절)의 착수와 담당 세션은 사용자가 정한다. 지금 진행 중인 작업은 12절 "지금 지킬 것"으로 이 계약과 어긋나지 않게 한다.

UE 근거는 UE 5.8.4(`release`, 로컬 클론 `C:\github\UnrealEngine`)다. 경로는 `Engine/Source/` 기준이다. 레거시·deprecated API(`BeginSlowTask` 등)는 근거로 쓰지 않았다.

| 약자 | 파일 |
|---|---|
| `ST` / `STC` | `Runtime/Core/Public/Misc/SlowTask.h` / `Runtime/Core/Private/Misc/SlowTask.cpp` |
| `SST` | `Runtime/Core/Public/Misc/ScopedSlowTask.h` |
| `STS` / `STSC` | `Runtime/Core/Public/Misc/SlowTaskStack.h` / `Runtime/Core/Private/Misc/SlowTaskStack.cpp` |
| `FC` / `FCC` | `Runtime/Core/Public/Misc/FeedbackContext.h` / `Runtime/Core/Private/Misc/FeedbackContext.cpp` |
| `ATN` / `CATN` | `Runtime/Core/Public/Misc/AsyncTaskNotification.h` / `Runtime/Core/Public/Misc/CoreAsyncTaskNotificationImpl.h` |
| `NM` | `Runtime/Slate/Public/Framework/Notifications/NotificationManager.h` |
| `IACM` / `ACM` / `ACMC` | `Runtime/Engine/Public/IAssetCompilingManager.h` / `Runtime/Engine/Public/AssetCompilingManager.h` / `Runtime/Engine/Private/AssetCompilingManager.cpp` |
| `ACH` | `Runtime/Engine/Public/AsyncCompilationHelpers.h` |
| `SCM` | `Runtime/Engine/Private/ShaderCompiler/ShaderCompiler.cpp` |

## 1. UE 5.8의 구조

UE는 진행 보고를 세 층으로 나눈다.

1. **막는 작업의 중첩 진행 범위.** `FScopedSlowTask`(`SST`)가 `FSlowTask`(`ST`)를 감싼다. 범위마다 전체 작업량(`TotalAmountOfWork`, 임의 단위 float, 0이면 1로 바꿈 — `STC` 55–58행)과 기본 문구를 갖는다. `EnterProgressFrame(work, text)`가 앞 구간을 끝내고 다음 구간에 들어간다. 총량을 넘으면 잘라 내고 ensure를 낸다(`STC` 196–197행). 안쪽 범위가 끝나면 바깥의 현재 구간이 채워진다(`STC` 159–166행). 범위는 `FFeedbackContext`(`FC`, 전역 `GWarn`)의 `FSlowTaskStack`(`STS`)에 쌓이고, 전체 비율은 스택을 따라 곱해 낸다(`STSC` 6–23행). 창은 `MakeDialog`·`MakeDialogDelayed(threshold)`로 열고, 취소 단추를 줄 수 있으며 `ShouldCancel()`로 묻는다. 표시 정책은 `ESlowTaskVisibility`(Default·ForceVisible·Important·Invisible)다. 지연 창 임계값은 호출자가 고르며 `Engine/Source` 안 호출은 1.0 s(23곳)·3.0 s(14곳)·0.5 s(5곳)이다(플러그인까지 세면 0.5 s가 33곳으로 둘째).
   - **게임 스레드 전용이다**(`STC` 48행 `bEnabled(... && IsInGameThread())`, 252행 `check(IsInGameThread())`). 이유는 표시 방식에 있다. 범위가 진행할 때마다 `FFeedbackContext::RequestUpdateUI`가 초당 최대 5번(`FCC` 187행 `MinUpdateTimeS = 0.2`) **작업 도중에 UI를 직접 펌프**한다(`FCC` 184–207행). 막는 작업이 게임 스레드에서 돌아도 창이 갱신되는 것은 이 때문이다.
   - 범위별 trace 영역을 깊이 2까지 남긴다(`STC` 18·105행 `RegionId`).
2. **백그라운드 작업 알림.** `FAsyncTaskNotification`(`ATN`)이 제목·진행 문구·상태(`EAsyncTaskNotificationState` None·Pending·Success·Failure·Prompt)·취소 가능·성공/실패 뒤 열어 둠(`bKeepOpenOnFailure`)을 갖는다. 실제 표시는 factory로 고른 구현이 한다(`CATN` 162행 `RegisterFactory`). Core 기본 구현은 로그 범주로 적고(headless), 에디터는 Slate 알림을 띄운다. 비율 막대가 있는 알림은 Slate `StartProgressNotification(text, totalWork)`·`UpdateProgressNotification(handle, done, total)`(`NM` 112행)이고 여러 알림의 작업량은 합산된다. 총량이 늘면 막대가 뒤로 갈 수 있다고 주석이 인정한다(`NM` 118행).
3. **비동기 컴파일 관리자 등록소.** 셰이더·텍스처·정적 메시 등 비동기 빌드마다 `IAssetCompilingManager`(`IACM`)를 구현한다: 남은 수(`GetNumRemainingAssets`), 대상별 끝내기(`FinishCompilationForObjects`, 51행), 대상별 취소 힌트(`MarkCompilationAsCanceled`, 59행), 전부 끝내기(`FinishAllCompilation`), 남은 일 취소 후 안전할 때까지 대기(`Shutdown`, 69행), 프레임마다 결과 적용(`ProcessAsyncTasks(FProcessAsyncTaskParams)` — `bLimitExecutionTime`·`bPlayInEditorAssetsOnly`, 14–21행), 먼저 처리할 다른 종류(`GetDependentTypeNames`). `FAssetCompilingManager`(`ACM`)가 등록받아 의존 순서로 정렬하고(순환이면 등록은 받고 의존만 무시하며 Error 로그 — `ACMC` 636–640행), 저장·쿠킹·PIE 전에 전부 끝낸다. 상태 표시줄의 "Compiling Shaders (123)"은 `FAsyncCompilationNotification::Update(NumJobs)`(`ACH`)이고, 막는 대기에는 `FScopedSlowTask`로 "Compiling Shaders ({0})"을 띄운다(`SCM` 1856·1985행).

셰이더 컴파일 워커는 묶음이 끝날 때 결과를 한꺼번에 돌려주므로 진행은 묶음 단위로 오른다. 묶음 크기 기본값은 10이다(`Config/BaseEngine.ini` 2191행 `MaxShaderJobBatchSize=10`).

## 2. 요구

1. **어느 스레드에서나 보고.** Rocks의 오래 걸리는 작업은 대부분 작업 스케줄러 worker·전용 스레드·자식 프로세스에서 돈다. UE처럼 게임 스레드 전용이면 대부분의 진행이 보이지 않는다.
2. **UI 스레드에서 막는 작업도 표시.** UE처럼 UI 스레드가 직접 막는 작업(예: 저장 전 대기 작업 전부 끝내기)을 돌려도 진행 창이 갱신돼야 한다.
3. **중첩 구간의 가중 합산.** 호출자는 자기 작업만 알고 바깥 비율을 모른다(UE `FSlowTask` 모델).
4. **병렬 작업 집계.** `RunParallelFor`·job 묶음처럼 여러 스레드가 같은 작업의 조각을 끝낸다.
5. **늘어나는 총량.** 셰이더 job·쿠킹 패키지처럼 진행 중에 총량이 커질 수 있다. 보이는 막대는 뒤로 가지 않는다.
6. **협조적 취소.** 작업은 단계 경계에서 취소를 보고 `Cancelled`로 끝난다. 강제 스레드 중단은 없다.
7. **프로세스 경계 통과.** 자식 프로세스의 진행이 부모 작업 아래로 이어지고, 부모의 취소가 자식에 닿는다.
8. **받는 쪽 독립.** 에디터·명령줄·자동화·trace가 같은 데이터를 다르게 그린다. 보고하는 코드는 받는 쪽을 모른다. 받는 쪽은 여럿이 동시에 있을 수 있다.
9. **싸야 한다.** 바인딩이 없을 때 구간 보고는 모듈 경계 호출 한 번과 thread-local 읽기 한 번이다. 바인딩이 있어도 문구 없는 보고는 할당·잠금이 없다(원자 연산과 seqlock 쓰기).
10. **결정적 시험.** 시험은 시간 운에 기대지 않는다. 시계는 주입하고, 스레드 순서는 명시 gate로 만든다.

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
- `Progress` 모듈은 `Core/Source/Runtime/Progress`에 둔다. 의존은 `Core`·`PlatformBase`(Process·잠금·thread-local)·`Localization`(`Text`)·`TraceCapture`(span)·`Task`(`ProgressParallelFor`)다. 객체 시스템·렌더러·UI에 의존하지 않으므로 Developer 도구·명령·런타임이 모두 쓴다. UE도 `FSlowTask`·`FFeedbackContext`·`FAsyncTaskNotification`을 Core에 둔다. Rocks의 Task·LogHost·TraceCapture도 Core 층의 별도 모듈이다.
- `PendingWorkRegistry`는 `Engine` 모듈(`Engine/Source/Runtime/Engine`)에 둔다. 대상이 객체(`Object*`)이고 결과 적용이 게임 스레드 프레임에 묶이기 때문이다(UE도 Engine 모듈). Developer 모듈(셰이더 컴파일 관리자 등)은 Engine에 의존하지 않고 남은 수·끝내기·취소 API만 내놓는다. Engine 쪽 어댑터가 그것을 `PendingWorkSource`로 감싸 등록한다.
- 공개 헤더에 쓰기 가능한 정적 저장소·`thread_local`을 두지 않는다(BuildTargets BS8c). hub 저장소와 스레드별 바인딩 스택은 `Progress` cpp 안에 있고, `ProgressScope` 생성·`EnterStep`은 export된 out-of-line 함수다.

## 4. 취소 표지(`CancelToken`)를 Core로

`CancelToken`·`CancelSource`(`Engine/Source/Runtime/AssetRuntime/Public/AssetRuntime/CancelToken.h`)를 `Core/Public/Core/CancelToken.h`로 옮기고 export 매크로를 `ROCKS_CORE_API`로 바꾼다. 기존 계약(아무 스레드에서 Cancel·조회, 공유 상태는 원자 계수, `Create(allocator)`)은 그대로다. 하위호환 별칭 없이 호출처를 모두 바꾸고, 참조하는 문서(AssetSystem.ko.md 11.1절, CoreObjectInputDetailedPlan.ko.md, EngineImplementationPlan.ko.md의 `CancelToken` 언급)도 갱신한다. 범용 기능을 한 곳에만 두는 규칙을 따른다.

더할 것:
- `Result<void> CancelToken::CheckCancelled() const noexcept` — 취소면 `ErrorCode::Cancelled`.
- **연결 취소.** `static Result<CancelSource> CancelSource::CreateLinked(Allocator&, const CancelToken& parent) noexcept`.
  - 자식 공유 상태가 부모 공유 상태를 참조 계수로 붙잡는다. 조회(`IsCancellationRequested` — Token·Source 둘 다)는 자기 표시를 보고, 아니면 부모 사슬을 따라 읽는다. 콜백 등록은 두지 않는다(임의 스레드 재진입을 부른다).
  - 사슬 깊이 상한 `kMaxCancelChainDepth` = 8. 넘으면 `CapacityExceeded`.
  - 부모가 기본(취소될 수 없는) token이면 일반 source를 만든다.
- Task 모듈의 스케줄러 작업 취소(`WorkerScheduler.h`의 `RequestCancel`·`IsCancellationRequested`)는 스케줄러 내부 작업 단위의 취소이고 이 표지와 합치지 않는다. 진행 작업의 취소를 스케줄러 작업으로 옮기고 싶으면 호출자가 token을 보고 스케줄러 취소를 부른다.

## 5. 작업

### 5.1 API

```cpp
using ProgressTaskId = uint64;                  // 0은 "없음"(kNoProgressTask), 1부터 늘고 재사용하지 않는다
enum class ProgressPresentation : uint8 { Blocking, Background, Hidden };
enum class ProgressCancel : uint8 { NotCancellable, UserCancellable };
enum class ProgressOutcome : uint8 { Succeeded, Failed, Cancelled, Abandoned };
enum class ProgressShape : uint8 { Indeterminate, Fraction, Count };

struct ProgressTaskDesc {
    const Text* title = nullptr;                 // 필수, 비어 있으면 안 됨. Begin이 Clone한다
    ProgressTaskId parent = kNoProgressTask;
    ProgressPresentation presentation = ProgressPresentation::Background;
    ProgressCancel cancel = ProgressCancel::NotCancellable;
    CancelToken externalCancel;                  // 호출자 token — 있으면 연결 취소로 묶는다
    double weightInParent = 1.0;                 // 부모 안 몫, 유한한 양수
    double selfWeight = 1.0;                     // 하위 작업과 섞일 때 자기 구간·계수의 몫, 유한한 0 이상
    double modalDelaySeconds = kDefaultModalProgressDelay;   // Blocking만, 0 이상
};

class ROCKS_PROGRESS_API ProgressTaskHandle {    // 이동만
public:
    ProgressTaskId Id() const noexcept;
    CancelToken Token() const noexcept;          // 취소 불가 작업은 외부 token 또는 기본 token
    Result<void> SetMessage(const Text& message) noexcept;   // 작업 문구(구간 문구가 없을 때 쓰임)
    Result<void> Finish(ProgressOutcome outcome) noexcept;
    Result<void> Finish(ProgressOutcome outcome, const Text& detail) noexcept;   // 실패·취소 사유
    ~ProgressTaskHandle();                       // Finish 없이 소멸하면 Abandoned + Warning
};

class ROCKS_PROGRESS_API ProgressHub {
public:
    static ProgressHub& Get() noexcept;
    Result<void> Initialize(Allocator&, const ProgressHubSettings&) noexcept;   // 시계 주입 포함
    void Shutdown() noexcept;
    Result<ProgressTaskHandle> Begin(const ProgressTaskDesc&) noexcept;
    Result<void> RequestCancel(ProgressTaskId) noexcept;
    void RequestCancelAllTopLevel() noexcept;    // Ctrl-C·부모 cancel 줄
    Result<void> Snapshot(ProgressSnapshot& out, uint64& eventCursor) noexcept;
    Result<void> SetUiPump(const ProgressUiPump& pump) noexcept;   // 호출한 스레드가 UI 스레드
    void ClearUiPump() noexcept;
};
```

상수: `kDefaultModalProgressDelay` = 1.0 s(UE `Engine/Source` 지연 창 호출의 최빈값), `kMaxLiveProgressTasks` = 4096.

### 5.2 오류 코드(`Core/Error.h`의 기존 값만 쓴다)

| 상황 | 코드 |
|---|---|
| `Initialize` 전 `Begin`·`Snapshot` | `NotReady` |
| `Shutdown` 뒤 `Begin`·`SetMessage`·`Finish` | `ShuttingDown` |
| `title`이 null이거나 빈 Text, 가중치가 음수·0(`weightInParent`)·NaN·무한, 지연이 음수 | `InvalidArgument` |
| `parent`가 발급된 적 없는 id | `NotFound` |
| `parent`가 이미 끝난 작업 | `StaleHandle` |
| 살아 있는 작업 수 초과 | `CapacityExceeded` |
| 이미 끝난 작업에 `SetMessage`·`Finish` | `ScopeClosed` |
| 취소 불가 작업에 `RequestCancel` | `Unsupported` |
| `Text::Clone` 실패 | 그 오류 그대로(`Overflow` 등) |

### 5.3 끝내기와 수명

- `Finish`는 한 번이다. `Succeeded`로 끝나면 보이는 비율은 1이 되고, 다른 결과는 마지막 값을 유지한다.
- 부모가 끝날 때 살아 있는 하위 작업은 먼저 `Abandoned`로 닫는다(사건 기록에 하위 작업 끝이 부모 끝보다 앞선다 — 7.3절).
- 작업 기록은 참조 계수를 갖는다. handle·`ProgressCounter`·바인딩이 기록을 붙잡으므로, hub `Shutdown` 뒤나 작업이 끝난 뒤의 보고는 기록에 표시만 보고 아무 일도 하지 않는다(no-op). 기록은 마지막 참조가 놓일 때 `Initialize`에 준 allocator로 돌려준다. allocator는 다른 스레드 해제를 지원해야 한다.
- `Shutdown`은 살아 있는 작업을 `Abandoned`로 닫고 로그를 남긴 뒤 새 `Begin`을 `ShuttingDown`으로 거부한다.

### 5.4 진행 모양

- **불확정(`Indeterminate`).** 구간·계수·하위 작업이 없다. 회전 표시와 문구만.
- **비율(`Fraction`).** 구간(6절)이나 하위 작업이 있다.
- **계수(`Count`).** 작업 수준 계수(6.4절)가 있다. 화면은 "셰이더 123 / 4,096"처럼 수를 보이고, 막대는 완료/총량이다.

모양은 스냅샷 때 정한다: 작업 수준 계수가 있으면 계수, 아니면 구간·하위 작업이 있으면 비율, 아니면 불확정.

### 5.5 비율 합성

hub가 스냅샷 때 hub 잠금 안에서 아래부터 위로 계산한다.

1. **자기 진행 `S`.** 바인딩 Primary 스택(6.1절)이 발행한 값이다. 발행 상태는 `{stepSerial, base, width}`(현재 구간이 시작하는 비율과 그 폭, 전체 기준)이고 seqlock으로 쓴다(Primary 스레드만 쓰고, 스냅샷은 일관된 쌍을 읽을 때까지 다시 읽는다). 현재 구간에 구간 계수(6.4절)가 붙어 있고 그 `stepSerial`이 같으면 `S = base + width × min(done, total) / total`(total 0이면 0), 아니면 `S = base`.
   - 작업 수준 계수만 있으면 `S = min(done, total) / total`.
   - 가장 바깥 구간이 닫히면 `S`는 그 범위의 끝 값(그 범위 몫 전부)이다. 같은 작업에서 다음 가장 바깥 구간을 열면 `S`는 0부터 다시 오른다(UE와 같다 — 스택이 비면 0). 보이는 값은 아래 4로 고정되므로 작업 하나에 가장 바깥 구간을 하나만 여는 것을 권한다.
2. **하위 작업이 없으면** `F = S`.
3. **하위 작업이 있으면** `F = (w₀·S + Σ wᵢ·Cᵢ) / (w₀ + Σ wᵢ)`. `w₀`는 `selfWeight`이되 자기 구간·계수를 한 번도 보고하지 않았으면 0, `wᵢ`는 하위 작업의 `weightInParent`, `Cᵢ`는 끝난 하위 작업(결과 무관)이면 1, 아니면 그 하위 작업의 `F`. 분모가 0이면 `F = 0`. 하위 작업이 늦게 붙으면 분모가 커져 `F`가 줄 수 있다.
4. **보이는 값 `D`.** hub가 작업마다 `D = clamp(max(이전 D, F), 0, 1)`로 고정한다. 모든 모양·모든 소비자에 같은 값이 나간다(hub 쪽 값이므로 소비자마다 스냅샷 주기가 달라도 일관된다). UE Slate 알림의 "막대가 뒤로 감"을 피한다.
5. **계수 표시.** `done`·`total`은 따로 읽히므로 스냅샷은 `done = min(done, total)`로 자른다.

## 6. 바인딩·구간·계수

### 6.1 바인딩

```cpp
enum class ProgressBindingRole : uint8 { Primary, Secondary };
class ROCKS_PROGRESS_API ProgressBinding {      // 이동 불가 RAII
public:
    static Result<ProgressBinding> Bind(ProgressTaskId, ProgressBindingRole) noexcept;
    ~ProgressBinding();
};
class ROCKS_PROGRESS_API ProgressBindingSuspend { // 이동 불가 RAII — 이 스레드의 바인딩을 잠시 뗀다
public:
    ProgressBindingSuspend() noexcept;
    ~ProgressBindingSuspend();
};
```

- **자동 바인딩은 없다.** `Begin`한 스레드도 명시 `Bind`를 해야 한다. handle을 다른 스레드로 넘겨 거기서 `Finish`해도 매달린 바인딩이 남지 않게 하려는 것이다.
- 스레드별 바인딩은 스택이다(깊이 상한 `kMaxProgressBindingDepth` = 16, 넘으면 `CapacityExceeded`). 맨 위 바인딩이 그 스레드의 "현재 작업"이다. 소멸은 생성의 역순이어야 한다(어기면 Fatal — 스택 손상은 코드 결함이다).
- 한 작업의 Primary 바인딩은 동시에 하나다. 다른 스레드가 Primary로 또 붙으면 `AlreadyExists`. Primary가 풀린 뒤에는 다른 스레드가 Primary가 될 수 있고, 새 Primary의 구간 스택은 비어서 시작한다.
- 작업이 없거나 끝났으면 `Bind`는 `NotFound`·`StaleHandle`이다.
- **Primary 스택**의 구간만 자기 진행과 문구를 발행한다. **Secondary 스택**의 구간은 진행·문구를 무시하고 취소 조회만 한다(병렬 조각이 라이브러리 구간을 열어도 Primary를 흐리지 않게).
- `ProgressBindingSuspend`는 그 스레드의 현재 작업을 "없음"으로 덮는다. 바인딩된 스레드가 병렬 본문을 직접 실행할 때 쓴다(6.5절).

### 6.2 구간

```cpp
class ROCKS_PROGRESS_API ProgressScope {        // 이동 불가 RAII
public:
    explicit ProgressScope(double totalWork) noexcept;
    ProgressScope(double totalWork, const Text& defaultMessage) noexcept;
    void EnterStep(double work = 1.0) noexcept;
    void EnterStep(double work, const Text& message) noexcept;
    void Tick() noexcept;                       // 진행 없이 살아 있음 + UI 펌프 기회
    bool IsCancellationRequested() const noexcept;
    Result<void> CheckCancelled() const noexcept;
    Result<ProgressCounter> MakeCounter(uint64 total) noexcept;   // 현재 구간을 채우는 계수
    ~ProgressScope();
};
```

```cpp
ProgressScope scope(3.0, buildMeshText);        // 문구는 미리 만든 Text
scope.EnterStep(1.0, weldVerticesText);
Weld();
scope.EnterStep(2.0);                           // 문구 없음 — 범위 기본 문구
BuildLods();                                    // 안에서 다시 ProgressScope를 열면 이 2.0이 그 범위 전체
```

- UE `FScopedSlowTask` 모델을 따른다: 총량 0이면 1로 보고 범위 전체가 한 구간, `EnterStep`이 앞 구간을 끝냄, 안쪽 범위가 바깥 현재 구간을 채움, 범위 소멸 시 남은 양을 채움.
- 입력 검사: 작업량이 음수·NaN·무한이면 0으로 보고, 총량을 넘는 `EnterStep`은 잘라 낸다. 둘 다 처음 한 번 Warning 로그(UE ensure 대응).
- 깊이 상한 `kMaxProgressScopeDepth` = 64. 넘는 구간은 아무 일도 하지 않고(진행 계산은 바깥 구간에 묻힘) 처음 한 번 Warning.
- 바인딩이 없으면 구간은 아무 일도 하지 않는다. 그래서 하위 라이브러리는 작업을 몰라도 구간을 열고, 진행을 보고 싶은 호출자만 바인딩한다.
- 문구가 있는 `EnterStep`·범위 기본 문구는 `Text::Clone`을 한다(노드 잠금 + 참조 계수, 실패하면 문구만 버리고 진행은 계속 — 실패는 처음 한 번 Warning). 이전 문구를 놓을 때 해제가 일어날 수 있다. 할당·잠금 없는 경로는 문구 없는 `EnterStep`·`Tick`·계수 `Add`다.
- **문구.** Primary 스택은 깊이별 현재 문구를 바깥부터 `kMaxReportedProgressDepth` = 4개까지 발행한다(작은 잠금). 스냅샷은 깊이별 문구 목록과 작업 문구(`SetMessage`)를 함께 준다. 한 줄로 보일 때는 가장 안쪽의 비어 있지 않은 구간 문구, 없으면 작업 문구, 없으면 제목이다. UE 진행 창이 여러 깊이를 줄로 보이는 것과 맞춘다.
- **trace.** Primary 스택 깊이 2까지의 구간은 `TraceBeginSpan`·`TraceEndSpan`(TraceCapture `TraceMark.h`)으로 남긴다(UE `RegionId`와 같은 깊이). 이름은 문구의 원문(9.2절과 같은 방법)이다. span은 스레드에 묶이므로 작업 시작·끝은 span이 아니라 hub 사건 기록(7.3절)으로 남긴다.

### 6.3 UI 스레드의 막는 작업 — UI 펌프 hook

UE가 슬로 태스크를 게임 스레드로 제한한 이유(1절)를 Rocks는 이렇게 푼다.

```cpp
struct ProgressUiPump {
    void (*pump)(void* context) noexcept = nullptr;
    void* context = nullptr;
};
```

- UI(에디터) 시작 코드가 UI 스레드에서 `hub.SetUiPump(pump)`를 부른다. 그 스레드가 "UI 스레드"로 등록된다.
- 그 스레드에서 `EnterStep`·`Tick`·계수 `Add`·`SetMessage`가 불리면, hub는 주입 시계로 마지막 펌프 뒤 `kUiPumpInterval` = 0.2 s(UE `MinUpdateTimeS`와 같음)가 지났을 때 `pump(context)`를 부른다. 펌프는 스냅샷을 당겨 진행 창을 그리고 취소 단추 입력을 처리한다. 펌프 안에서 다시 펌프하지 않는다(재진입 표시).
- 다른 스레드에서는 펌프를 부르지 않는다. 그 스레드의 작업은 UI 스레드가 자기 프레임에서 스냅샷을 당겨 그린다.
- 그래서 막는 작업을 UI 스레드에서 돌리는 코드는 긴 외부 호출 사이사이 `Tick`을 불러야 한다(UE `TickProgress`와 같은 의무).

### 6.4 계수

```cpp
class ROCKS_PROGRESS_API ProgressCounter {      // 이동만, 아무 스레드에서 Add
public:
    static Result<ProgressCounter> ForTask(const ProgressTaskHandle&, uint64 total) noexcept;
    void Add(uint64 done = 1) noexcept;
    void AddTotal(uint64 more) noexcept;
};
```

- **작업 수준 계수**(`ForTask`)는 작업의 계수 모양을 만든다. 작업마다 하나다. 이미 있으면 `AlreadyExists`. 작업이 끝났으면 `ScopeClosed`.
- **구간 계수**(`scope.MakeCounter`)는 만들 때의 현재 구간(`stepSerial`)을 채운다. 작업마다 동시에 하나이고, 이미 살아 있는 구간 계수가 있으면 `AlreadyExists`. 바인딩이 없거나 Secondary면 아무 일도 하지 않는 계수를 돌려준다.
  - 그 구간이 끝난 뒤(`EnterStep`·범위 소멸)의 `Add`는 무시된다. 스냅샷은 `stepSerial`이 다르면 계수를 쓰지 않는다.
- `Add`·`AddTotal`은 원자 더하기이고 UI 스레드 펌프 조건(6.3절) 말고는 다른 일을 하지 않는다.

### 6.5 병렬 작업

`RunParallelFor`는 호출 스레드도 조각을 실행한다(`Task/Public/Task/ParallelFor.h` 12–13행). Primary로 바인딩된 스레드가 조각 안에서 라이브러리 구간을 만나면 Primary 스택이 흐려진다. 그래서:

```cpp
// Progress 모듈이 준다: 바인딩을 잠시 떼고, 조각마다 계수를 올린다
Result<void> ProgressParallelFor(TaskScheduler&, usize count, ProgressCounter& counter,
                                 FunctionRef<void(usize)> body) noexcept;
```

- 안에서 `ProgressBindingSuspend`로 호출 스레드 바인딩을 떼고 `RunParallelFor`를 부르며, 조각이 끝날 때마다 `counter.Add(1)`.
- 직접 `RunParallelFor`를 부르는 코드는 바인딩된 스레드에서 `ProgressBindingSuspend`를 걸어야 한다(규칙). 조각 안의 진행은 계수로만 보고한다.

## 7. 중계(`ProgressHub`)

### 7.1 초기화·시계

- 프로세스에 하나다. 저장소는 `Progress` cpp 안에 있다. 엔진/도구 시작이 `Initialize`, 종료가 `Shutdown`을 부른다.
- `ProgressHubSettings`: 시계 `uint64 (*nowNanoseconds)(void*)`·context(기본은 단조 시계, 시험은 가짜 시계), 사건 기록 용량.

### 7.2 스냅샷(pull)

- 받는 쪽은 자기 빈도로 `Snapshot(out, cursor)`를 부른다. 보고 스레드에서 받는 쪽 코드를 부르지 않는다(UI 펌프 hook은 UI 스레드 자신의 호출이다).
- `ProgressSnapshot`은 살아 있는 작업의 트리 복사본이다. 작업마다 id·부모·제목·깊이별 문구·작업 문구·표시 방식·모양·보이는 비율 `D`·완료/총량·시작 시각·마지막 진행 시각·취소 가능·취소 요청됨. 문구는 `Text::Clone`으로 담는다(스냅샷은 소비자 소유, 해제도 소비자 스레드).
- 스냅샷의 할당은 소비자가 준 allocator에서 한다. 같은 `ProgressSnapshot` 객체를 다시 쓰면 용량을 재사용한다.

### 7.3 사건 기록과 소비자별 cursor

- 시작·끝 사건은 hub 잠금 안에서 일련 번호(64-bit, 1부터)를 붙여 고리 버퍼(`kProgressEventLogCapacity` = 1024)에 쌓는다. 잠금 안에서 쌓으므로 순서는 전체 순서다: 하위 작업의 시작은 부모 시작 뒤이고(부모가 살아 있어야 `Begin`이 성공), 부모가 끝날 때 하위 작업의 끝을 먼저 쌓는다.
- 사건: `Begin{id, parent, title, presentation, cancel}`, `End{id, outcome, detail, 마지막 D}`.
- `Snapshot(out, cursor)`는 `cursor` 뒤의 사건을 `out.events`에 담고 `cursor`를 마지막 번호로 옮긴다. 소비자마다 자기 cursor를 가지므로 여러 소비자가 서로의 끝 기록을 빼앗지 않는다. cursor 0은 "지금부터"다(처음 호출은 살아 있는 트리만 받고 cursor를 현재 번호로 맞춘다).
- 고리가 넘쳐 놓친 사건 수는 `out.lostEvents`에 담는다(소비자는 "N개 사건 누락"을 한 번 보고).
- push 관찰자는 두지 않는다(15절 버린 대안 (나)).

### 7.4 취소

- `RequestCancel(id)`는 그 작업의 hub 소유 `CancelSource`를 취소한다. 하위 작업은 연결 취소로 함께 취소된다(하위 작업의 source는 부모 token에 연결해 만든다).
- `RequestCancelAllTopLevel()`은 취소 가능한 최상위 작업을 모두 취소한다. 신호 처리기에서 부르지 않는다(잠금을 쓴다). Ctrl-C는 Launch의 원자 표시(`Launch/Application.h` `IsTerminationRequested`)를 펌프 주인이 보고 이 함수를 부른다(9.1절).

## 8. 대기 작업 등록소(`PendingWorkRegistry`, Engine 모듈)

UE `FAssetCompilingManager`·`IAssetCompilingManager` 구조를 따른다.

```cpp
struct PendingWorkPumpParams {
    uint64 timeBudgetNanoseconds = 0;            // 0이면 제한 없음(UE bLimitExecutionTime)
    bool tryoutAssetsOnly = false;               // Tryout에 필요한 것만(UE bPlayInEditorAssetsOnly)
};

class PendingWorkSource {
public:
    virtual StringView Name() const noexcept = 0;                   // 고유 종류 이름("Shaders")
    virtual const Text& DisplayName() const noexcept = 0;           // 상태 표시줄 문구
    virtual Span<const StringView> RunBefore() const noexcept = 0;  // 먼저 끝낼 종류(UE GetDependentTypeNames)
    virtual uint64 RemainingCount() const noexcept = 0;             // 아무 스레드
    virtual Result<void> FinishFor(Span<Object* const> targets) noexcept = 0;  // 막고 그 대상만
    virtual void CancelFor(Span<Object* const> targets) noexcept = 0;          // 힌트
    virtual Result<void> FinishAll() noexcept = 0;                  // 막고 전부
    virtual void Shutdown() noexcept = 0;                           // 남은 일 취소 + 안전할 때까지 대기
    virtual void ApplyCompleted(const PendingWorkPumpParams&) noexcept = 0;  // 소유 스레드
    virtual ~PendingWorkSource() = default;
};
```

- **소유 스레드.** 등록소는 만든 스레드가 소유한다. 엔진에서는 `Engine::Initialize`를 부른 게임 스레드이고(엔진이 등록소를 갖는다), 엔진 없이 도는 명령(쿠킹 등)은 자기 주 스레드에서 등록소를 만들고 자기 반복에서 `ApplyCompleted`를 펌프한다. `RemainingCount` 말고는 다른 스레드에서 부르면 `WrongThread`.
- `Register(PendingWorkSource&)`: 빈 이름은 `InvalidArgument`, 같은 이름은 `AlreadyRegistered`. `RunBefore`에 없는 이름은 무시한다. 순환이 생기면 등록은 받고 그 의존만 무시하며 Error 로그를 남긴다(UE `ACMC` 636–640행과 같다).
- `Unregister`: `FinishAll`·`ApplyCompleted` 도중(등록소 호출 안에서)이면 `Busy`. 모듈을 내리기 전에 그 source의 `Shutdown`을 부르고 `Unregister`한다.
- `FinishAll()`: `RunBefore` 순서대로 각 source를 끝내며, 사이사이 소유 스레드에서 `ApplyCompleted`를 끼워 넣는다(UE와 같다). 이때 `Blocking` 작업 "Finishing background work"를 열고 종류별 하위 작업에 시작 시 남은 수를 작업 수준 계수로 보인다(UE "Compiling Shaders ({0})" 창). 등록소의 소유 스레드가 UI 스레드면 UI 펌프 hook(6.3절)이 창을 갱신한다.
- `FinishFor(targets)`·`CancelFor(targets)`는 모든 source에 같은 목록을 넘긴다. source는 자기 대상만 처리한다(UE와 같다).
- 에디터 상태 표시줄은 종류별 `RemainingCount`를 보인다("셰이더 123 · 텍스처 4"). UE `FAsyncCompilationNotification::Update`와 같은 역할이다.
- 저장·쿠킹·Tryout 시작·종료 전에 `FinishAll`(또는 `FinishFor`)을 부르는 지점은 각 기능 스펙이 정한다.

## 9. 받는 쪽

### 9.1 명령줄 보고기(`ConsoleProgressReporter`)

- 사람이 읽는 줄을 Log API로 낸다. 로그 범주 이름은 `"Progress"`(변수 `LogProgress`), 문구는 원문(9.2절). 로그는 stderr로 가므로 stdout JSONL과 섞이지 않는다.
- `Pump(uint64 nowNanoseconds)`를 펌프 주인이 명시 호출한다. 시작·끝 사건은 Pump 때 바로, 진행 줄은 `kDefaultConsoleProgressInterval` = 2 s마다 최상위 작업별 한 줄: `Cooking packages: 1,234 / 5,000 (24%) - Maps/Arena`. 실패·취소·Abandoned 끝은 Warning, 나머지는 Display 수준. `Hidden` 작업은 시작·끝만.
- **펌프 주인.** 명령의 주 스레드 반복이 부른다. 한 호출로 오래 막는 명령은 `ProgressPumpThread`(Progress 모듈 — 전용 스레드가 간격 대기 후 `Pump`·종료 요청 확인)를 쓴다. 펌프 주인은 매 Pump에 `IsTerminationRequested()`(Launch)를 보고 처음 보았을 때 `RequestCancelAllTopLevel()`을 부른다. Progress 모듈은 Launch에 의존하지 않으므로 이 확인은 명령 쪽 glue(`ProgressPumpThread`에 함수로 주입)가 한다.
- 명령은 기본으로 켜고 `--no-progress`로 끈다(AppInfoCommandLine `--kebab` 형식).

### 9.2 원문과 지역화 id

- 원문: `ResolveText`에 invalid `CatalogSnapshot`을 넘겨 fallback 원문을 얻는다(`Localization/Text.h` — invalid snapshot은 원문으로 fallback). 이 원문은 source culture 원문이다.
- 지역화 id: `TryGetPersistentForm`이 Invariant·Localized 형태에서 `TextId`(Guid)와 `sourceRevision`을 준다. Format·Integer 이력은 `Unsupported`이므로 id 없이 원문만 보낸다 — 이런 문구는 부모 쪽에서 다시 지역화할 수 없다(한계 15.6).

### 9.3 기계 판독 줄(`ProgressJsonlWriter`)

EngineImplementationPlan 28.4절의 "stdout은 선택 JSONL/progress protocol"을 이 절이 정한다. `--progress=jsonl`이면 stdout에 한 줄 JSON을 쓴다. 이 모드에서 명령의 다른 stdout 출력(예: `shader-compile`의 결과 이름)은 stderr 로그나 파일로 보낸다.

```text
{"v":1,"e":"begin","id":7,"parent":0,"title":"Cooking packages","titleId":"<guid>","titleRev":3,"pres":"background","cancel":true}
{"v":1,"e":"progress","id":7,"shape":"count","frac":0.2468,"done":1234,"total":5000,"msg":"Maps/Arena","msgId":"","msgRev":0,"cancelRequested":false}
{"v":1,"e":"progress","id":9,"shape":"fraction","frac":0.4125,"msg":"Welding vertices","msgId":"<guid>","msgRev":1,"cancelRequested":false}
{"v":1,"e":"end","id":7,"outcome":"succeeded","detail":"","detailId":"","detailRev":0}
{"v":1,"e":"lost","count":12}
```

- `outcome`: `succeeded`·`failed`·`cancelled`·`abandoned`. `shape`: `indeterminate`·`fraction`·`count`(`done`·`total`은 count만). id 필드가 빈 문자열이면 원문만 있다.
- `begin`·`end`·`lost`는 Pump 때 바로 쓰고, `progress`는 `kDefaultJsonlProgressInterval` = 0.2 s마다 쓰되 직전과 같은 작업은 건너뛴다.
- 줄 최대 길이 `kMaxProgressJsonlLineBytes` = 64 KiB. 넘는 문구는 잘라 쓴다(UTF-8 경계에서).
- 읽는 쪽은 `v`가 다르면 `Unsupported`로 거부하고 그 자식의 진행을 끊는다(자식 프로세스 자체는 계속).

### 9.4 에디터(나중 — UiToolkit·에디터 단계)

계약만 정한다.
- `Blocking` 작업이 `modalDelaySeconds` 뒤에도 살아 있으면 진행 창(깊이별 문구 줄, 최상위 막대, 취소 단추). 여러 막는 작업은 하나의 창에 쌓는다(UE와 같다).
- `Background` 작업은 알림(제목·막대·취소, 끝나면 결과 표시 후 사라짐, 실패는 남김 — UE `bKeepOpenOnFailure`).
- 대기 작업 등록소 합계는 상태 표시줄.
- UI 스레드는 프레임마다 스냅샷을 당기고, 막는 작업 중에는 UI 펌프 hook으로 그린다.

## 10. 프로세스 경계

### 10.1 자식이 보내는 진행

- 자식 프로세스(쿠킹 프로세스·`shader-compile` 명령·외부 도구 어댑터)는 `--progress=jsonl`로 9.3절 줄을 stdout에 쓴다.
- 부모는 `ChildProgressReader`로 읽어 자기 작업 아래 하위 작업으로 다시 만든다. 자식 id → 부모 쪽 id 대응표 상한은 `kMaxChildProgressTasks` = 4096이고, 넘는 하위 작업은 만들지 않고(그 진행은 부모 쪽에서 안 보임) 처음 한 번 Warning.
- 깨진 줄은 Warning을 한 번 남기고 무시한다. 자식이 끝나면 남은 하위 작업을 받은 결과로, 결과가 없으면 `Failed`(비정상 종료) 또는 `Abandoned`(정상 종료인데 end가 없음)로 닫는다.
- **pipe 소유.** P1 `Process`는 한 객체를 한 번에 한 스레드만 다루고 교차 스레드 깨우기가 없다(`PlatformBase/Process.h` 스레드 계약). 그래서 `ChildProgressReader`는 Process를 소유한 한 스레드가 돌린다: `Read(timeout = kChildControlPollInterval)`(0.1 s)를 반복하고, 반복 사이에 부모 token을 보아 취소면 stdin에 `cancel` 줄을 한 번 쓴다(10.2절). 셰이더 워커 채널과 같은 방식이다.
- **stderr.** 자식 stderr는 부모 stderr를 상속하거나 파일로 보낸다. stdout 하나만 오래 읽으므로 stderr를 pipe로 열어 두면 교착이 난다(`Process.h` 경고). `mergeErrorIntoOutput`은 쓰지 않는다(JSONL과 섞인다).

### 10.2 부모 제어 모드와 취소

- 자식은 `--parent-control`일 때만 stdin을 제어 채널로 쓴다. `ParentControlListener`(전용 스레드)가 줄을 읽고:
  - `cancel` → `RequestCancelAllTopLevel()`. 자식은 정상 경로로 `Cancelled`를 내고 종료 코드 130(EngineImplementationPlan 28.4절 "사용자 중단")으로 끝난다.
  - EOF(부모가 사라짐) → 같은 취소. 셰이더 워커의 "입력 EOF면 자식을 정리하고 끝남"과 같은 규칙이다.
  - 모르는 줄 → Warning 후 무시.
- `--parent-control` 없이 실행한 명령(CI가 stdin을 닫고 띄우는 경우 등)은 stdin을 보지 않는다. EOF로 취소되지 않는다.
- 부모는 취소 뒤 호출 기능이 정한 제한 시간 안에 자식이 안 끝나면 강제 종료한다. 시험은 hook으로 자식이 취소를 무시하게 만들어 강제 종료 경로를 결정적으로 검사한다.

### 10.3 셰이더 워커(ShaderSystem SH1d가 따를 것)

- 셰이더 워커는 JSONL을 쓰지 않는다. 기존 제어 pipe 줄 프로토콜에 job 하나가 끝날 때마다 `job <seq> <index>` 줄을 더한다. 워커 채널은 지금 모르는 줄을 받으면 워커를 끝내므로(ShaderSystem 11.1절 SC4·SC6 채널 규칙), **ShaderSystem 스펙을 먼저 개정**해 메시지 버전을 올리고 채널 파서가 이 줄을 받게 한다.
- 관리자의 계수: `job` 줄이 오면 그 묶음의 잠정 완료 수를 올린다. 묶음이 끝나면 잠정 값을 실제 결과(성공·실패·재시도로 돌림)로 바꾼다. 묶음이 실패해 이미 받은 `job` 줄의 결과가 사라지면 그 job은 재시도나 실패로 다시 세고, 잠정 값이 줄어도 보이는 비율은 hub의 고정 규칙(5.5절 4)으로 뒤로 가지 않는다.
- 이 방식으로 진행은 묶음이 아니라 job 단위로 오른다 — UE(묶음 단위)보다 촘촘하다. 결과 자체는 지금처럼 묶음 출력 파일로 받는다.

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

## 12. 적용 대상과 지금 지킬 것

| 기능 | 담당 | 모양 | 지금 지킬 것(PG 전) |
|---|---|---|---|
| 셰이더 컴파일 관리자(SH1d) | 렌더 세션 | 작업 수준 계수 + 대기 작업 종류 "Shaders" | 관리자가 전체·완료·실패·캐시 적중 job 수를 원자 계수로 내놓는다. 워커 `job` 줄은 10.3절대로 ShaderSystem 스펙을 먼저 개정해 더한다. PG5에서 `ProgressCounter`·`PendingWorkSource` 어댑터로 잇는다 |
| `shader-compile` 명령 | 렌더 세션 | 작업 수준 계수 | 진행 줄을 직접 찍지 말고 관리자 계수를 읽는 자리만 둔다(PG5에서 보고기·JSONL로) |
| 임포트(A2f) | 에셋 세션 | 파일별 하위 작업 + 구간 | 임포터 단계 경계에서 `CancelToken`을 확인하는 자리를 둔다 |
| 쿠킹(A4–A5) | 에셋 세션 | 작업 수준 계수(패키지) + 하위 작업 | 같음. 쿠킹 진행 로그 줄은 9.1절 형식을 따른다 |
| 너겟 일괄 요청(A3) | 에셋 세션 | 구간 계수 | 요청 객체가 완료 수를 내놓는다 |
| 텍스처·오디오·메시 빌드 | 에셋·오디오 세션 | 구간, 비동기면 대기 작업 종류 | — |
| 언리얼 교환·Sparks 컴파일·저장 | 각 세션 | 작업 + 구간 | — |

## 13. 단계

| 단계 | 내용 | 완료 조건 |
|---|---|---|
| PG1 | `CancelToken`·`CancelSource` Core 이동, `CheckCancelled`, 연결 취소, 문서 참조 갱신 | 호출처 전부 이동, 시험 1–2, 정책 통과 |
| PG2 | `Progress` 모듈: hub·작업·바인딩·구간·계수·스냅샷·사건 기록·UI 펌프 hook·`ProgressParallelFor` | 시험 3–14, 요구 9 비용 측정치 기록 |
| PG3 | `ConsoleProgressReporter`·`ProgressPumpThread`·`ProgressJsonlWriter`·`ChildProgressReader`·`ParentControlListener` | 시험 15–21 |
| PG4 | `PendingWorkRegistry`·`PendingWorkSource`(Engine) | 시험 22–26 |
| PG5 | 첫 적용: 셰이더 컴파일 관리자·`shader-compile`(렌더 세션), 쿠킹·임포트(에셋 세션) | 실제 명령의 진행 줄·JSONL·취소 확인 |
| PG6 | 에디터 표시(진행 창·알림·상태 표시줄) | 에디터 단계에서 — UiToolkit 스펙에 연결 |

## 14. 시험

시간 운에 기대지 않는다. 시계는 주입하고(가짜 시계를 시험이 직접 전진), 스레드 순서는 명시 gate로 만든다. 단언은 결정적인 최종 상태와 사건 순서에 한다.

1. 연결 취소: 부모·자식·다단, 깊이 상한, 기본 부모 token, Source 쪽 조회도 사슬을 봄.
2. `CheckCancelled`가 `Cancelled`를 돌려줌.
3. 중첩 구간 비율: 3단 중첩, 가중치 다른 구간, 총량 0 범위, 남은 양 채움 — UE `FSlowTaskStack` 곱셈 합성과 같은 값(손 계산 표).
4. 입력 검사: 음수·NaN 작업량, 총량 초과 `EnterStep`, 깊이 상한 — 값과 Warning 1회.
5. 바인딩 없는 스레드의 구간은 아무 일도 안 하고 할당이 없다(할당 계수기).
6. 바인딩: Primary 둘 → `AlreadyExists`, 스택 순서, Secondary 구간 무시, `ProgressBindingSuspend`, 끝난 작업 → `StaleHandle`, handle을 다른 스레드에서 `Finish`해도 바인딩이 남지 않음.
7. 계수: 여러 스레드 `Add`(gate로 순서 고정), `AddTotal`로 총량 증가 시 `D` 단조, `done > total` 순간 값 자르기.
8. 구간 계수: 그 구간 몫만 채움, 구간이 끝난 뒤 `Add` 무시, 둘째 계수 → `AlreadyExists`.
9. 하위 작업 합성: `selfWeight`·`weightInParent`, 끝난 하위 작업 1, 늦게 붙는 하위 작업에도 `D` 단조, 자기 보고 없는 부모의 `w₀ = 0`.
10. 수명: `Finish` 없이 소멸 → `Abandoned` + Warning, 끝난 뒤 보고 no-op, `Shutdown` 뒤 handle·계수 no-op, 부모 끝 → 하위 `Abandoned`가 먼저 기록됨.
11. 오류 코드 표(5.2절) 각 줄.
12. 사건 기록: 두 소비자가 각자 cursor로 같은 사건을 받음, 고리 넘침 → `lostEvents`, 첫 호출 cursor 0.
13. 취소: `RequestCancel` → 작업·하위 작업 token, `RequestCancelAllTopLevel`, 취소 불가 → `Unsupported`.
14. UI 펌프 hook: 가짜 시계로 0.2 s 경계(직전·정확히·직후), 등록 스레드만 펌프, 펌프 안 재진입 없음.
15. 명령줄 보고기: 가짜 시계로 2 s 경계, 시작·끝 줄, `Hidden`, 실패·취소 줄 수준, 종료 요청 표시 → `RequestCancelAllTopLevel` 한 번.
16. JSONL golden(각 사건·모양·id 있음/없음·`lost`), 변화 없는 작업 건너뜀, 긴 문구 UTF-8 경계 자르기.
17. 원문 추출: Localized·Invariant·Format 문구.
18. 자식 읽기: 정상 왕복, 깨진 줄 무시, 버전 불일치 `Unsupported`, 대응표 상한, 비정상 종료 → `Failed`, end 없는 정상 종료 → `Abandoned`.
19. 자식 취소: 부모 취소 → `cancel` 줄 → 자식 `Cancelled`·종료 코드 130.
20. `--parent-control`에서 stdin EOF → 취소, `--parent-control` 없으면 닫힌 stdin이어도 계속.
21. 강제 종료 경로(hook로 자식이 취소를 무시).
22. 등록소: 합계, `RunBefore` 순서, 순환 → 등록 수락·의존 무시·Error 로그, 없는 이름 무시.
23. 소유 스레드 밖 호출 → `WrongThread`(`RemainingCount` 제외).
24. `FinishAll`이 사이사이 `ApplyCompleted`를 끼우고 끝에 남은 수 0, 진행 작업과 하위 계수.
25. `FinishFor`·`CancelFor`가 모든 source에 같은 목록, `Shutdown` 뒤 `Unregister`, 호출 중 `Unregister` → `Busy`.
26. `ApplyCompleted` 매개변수 전달(시간 예산·Tryout 전용).

## 15. 한계와 후속

1. **사용자 확인(Prompt).** UE `EAsyncTaskNotificationState::Prompt`(계속/취소 묻기)는 두지 않았다. 필요한 기능이 생기면 더한다.
2. **남은 시간 추정.** 두지 않았다. 받는 쪽이 스냅샷 이력으로 할 수 있다.
3. **멈춤 감지.** 스냅샷의 마지막 진행 시각으로 받는 쪽이 "응답 없음"을 보일 수 있다. 기준 시간은 에디터 단계에서 정한다.
4. **여러 자식 프로세스.** 하위 작업 트리는 자식마다 따로 붙는다. 프로세스 사이 합계 표시는 받는 쪽 몫이다.
5. **비용 수치.** 요구 9의 비용은 PG2에서 측정해 숫자로 적는다.
6. **Format 문구 재지역화.** 9.2절대로 Format·Integer 이력 문구는 자식에서 부모로 원문만 넘어간다.
7. **가장 바깥 구간 여럿.** 5.5절대로 둘째 가장 바깥 구간의 진행은 보이는 값 고정 때문에 앞 구간 끝 값을 넘기 전까지 보이지 않는다.
8. **취소 전달 지연.** 자식 취소는 최대 `kChildControlPollInterval`만큼 늦다. P1 `Process`에 교차 스레드 깨우기가 생기면 polling을 없앤다.

## 16. 버린 대안

- (가) UE처럼 게임 스레드 전용 슬로 태스크 + 별도 백그라운드 알림 API — 두 API로 같은 일을 하고 worker의 중첩 진행이 보이지 않는다. 대신 어느 스레드에서나 보고하고, UE가 게임 스레드에 묶인 이유(작업 도중 UI 펌프)는 UI 펌프 hook(6.3절)으로 따로 푼다.
- (나) 진행·사건마다 관찰자 콜백 push — 보고 스레드에서 받는 쪽 코드가 돌아 UI 스레드 계약과 잠금 순서가 깨지고, 스레드마다 통지하면 하위 작업 끝이 시작보다 먼저 도착할 수 있다. 스냅샷과 소비자별 cursor의 사건 기록으로 바꿨다.
- (다) 셰이더 관리자만의 진행 카운터 — 사용자 지적("셰이더만의 문제가 아님")으로 공용 기반으로 올렸다.
- (라) 문구를 영어 문자열로만 — 에디터 화면은 지역화해야 하므로 `Text`로 받고, 로그·JSONL에는 원문과 id를 쓴다.
- (마) 취소를 작업 객체 안의 bool로 — 이미 있는 `CancelToken`과 중복이고 부모·외부 취소 연결이 안 된다.
- (바) `Begin`한 스레드 자동 바인딩 — handle을 다른 스레드로 넘기면 바인딩이 매달린다. 명시 `Bind`만 둔다.
- (사) 작업 비율을 원자 값 하나로 — 다른 스레드의 구간 계수 `Add`를 반영할 수 없다. 발행 상태(구간 시작·폭·계수)를 두고 스냅샷 때 합성한다.
- (아) 대기 작업 등록소를 Core 층에 — 대상이 객체이고 결과 적용이 게임 스레드 프레임에 묶인다. UE처럼 Engine에 둔다.
- (자) 등록 순환을 거부 — UE는 받고 의존만 무시한다. 애매하면 UE 방식을 따른다.

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

## 개정 이력

- 개정 1(2026-11-23): 초안. UE 5.8 세 층 구조 조사, Rocks 구조(어느 스레드에서나 보고·스냅샷 pull·소비자별 사건 cursor·UI 펌프 hook·프로세스 경계), API·오류 코드·한도 상수, 단계 PG1–PG6. 교차 검증 1차(리뷰어 1명, 필수 15·권장 8) 반영: UI 스레드 막는 작업(UI 펌프 hook), 발행 상태 seqlock 합성, 비율 합성 규칙, 자동 바인딩 제거, `RunParallelFor` 호출 스레드 처리(`ProgressBindingSuspend`·`ProgressParallelFor`), `Text` Clone 계약, 원문·id 추출, 기존 `ErrorCode`만 사용, 등록소 UE 정합(대상별·`Shutdown`·매개변수·순환 수락)과 Engine 배치·소유 스레드, 자식 pipe 소유·stderr·`--parent-control`, 셰이더 `job` 줄의 ShaderSystem 선개정, 소비자별 cursor, push 관찰자 제거.
