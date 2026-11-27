# 작업 진행 보고와 취소 — 8. 대기 작업 등록소(`PendingWorkRegistry`, Engine 모듈)

[목차](README.ko.md)

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
