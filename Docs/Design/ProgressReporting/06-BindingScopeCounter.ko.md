# 작업 진행 보고와 취소 — 6. 바인딩·구간·계수

[목차](README.ko.md)

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
