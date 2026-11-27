# 작업 진행 보고와 취소 — 5. 작업

[목차](README.ko.md)

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
