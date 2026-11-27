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
- `Register(PendingWorkSource&)`: 빈 이름은 `InvalidArgument`, 같은 이름·같은 source는 `AlreadyExists`(`Core/Error.h` 기존 값 — 처음 안의 `AlreadyRegistered`는 없다), `Shutdown` 뒤는 `ShuttingDown`, 등록소 호출 도중은 `Busy`. `RunBefore`에 없는 이름은 무시한다. 순환이 생기면 등록은 받고, 앞서 등록한 source가 새 source를 가리키는 의존을 무시하며(새 source 자신의 `RunBefore`는 그대로) Error 로그를 남긴다(UE `AssetCompilingManager.cpp` 636–640행 — 새 manager를 `AssetCompilingManagersWithValidDependencies`에서 뺀다).
- `Unregister`: `FinishAll`·`ApplyCompleted` 도중(등록소 호출 안에서)이면 `Busy`. 모듈을 내리기 전에 그 source의 `Shutdown`을 부르고 `Unregister`한다.
- `FinishAll()`: `RunBefore` 순서대로 모든 source의 `FinishAll`을 부른다(남은 수와 상관없이 — UE `FinishAllCompilation`과 같다). source의 `FinishAll`은 끝난 결과를 대상에 적용한 뒤 돌아간다(UE manager의 `PostCompilation`과 같다 — 등록소가 사이사이 `ApplyCompleted`를 끼우지 않는다). 남은 일이 있으면 `Blocking` 작업 "Finishing background work"를 열고 종류별 하위 작업을 이 스레드에 바인딩해 source가 그 위에서 `ProgressScope`로 진행을 보고한다(UE manager가 "Compiling Shaders ({0})" 창 문구를 스스로 갱신하는 것과 같다). 등록소의 소유 스레드가 UI 스레드면 UI 펌프 hook(6.3절)이 창을 갱신한다.
- `FinishFor(targets)`·`CancelFor(targets)`는 모든 source에 같은 목록을 넘긴다. source는 자기 대상만 처리한다. 목록이 비면 아무 일도 하지 않는다(UE와 같다).
- 에디터 상태 표시줄은 종류별 `RemainingCount`를 보인다("셰이더 123 · 텍스처 4"). UE `FAsyncCompilationNotification::Update`와 같은 역할이다.
- 저장·쿠킹·Tryout 시작·종료 전에 `FinishAll`(또는 `FinishFor`)을 부르는 지점은 각 기능 스펙이 정한다.

**PG4 구현(2026-11-27, 리뷰 반영 같은 날).** `Engine/PendingWorkSource.h`·`Engine/PendingWorkRegistry.h`(Engine 모듈).
- 소유 스레드는 만든 스레드이고 `AdoptCurrentThread()`로 바꾼다(등록이 없을 때만 — `Engine::Initialize`가 부르고 실패하면 Error 로그). `RemainingCount` 말고 모든 함수(`Shutdown`·`SourceCount`·`SourceNameAt` 포함)가 소유 스레드 전용이다. `Shutdown`은 결과를 돌려주지 않으므로 소유 스레드 밖·등록소 호출 도중이면 Error 로그를 남기고 아무 일도 하지 않는다.
- **source 계약.** `RemainingCount`는 아무 스레드에서 불리며 등록소 잠금 안에서 불린다: 잠금 없는 원자 읽기여야 하고 등록소를 다시 부르면 안 된다(잠금 순서 역전·재진입 교착). `Name`·`RunBefore`는 등록 동안 바뀌지 않는다. `Shutdown`은 두 번 불려도 된다(엔진 종료 때 등록소가, 모듈이 내릴 때 모듈이 부른다). `Shutdown` 뒤에도 `FinishAll`·`FinishFor`·`ApplyCompleted`가 올 수 있다(남은 일이 없으니 바로 돌아가면 된다). `ApplyCompleted`는 엔진 프레임에서 소유 쪽 객체 접근 scope(`ObjectAccessScope`) 안에서 불린다.
- 순서: `RunBefore` 위상 정렬(같은 순위는 등록 순서). 순환 처리는 위(UE와 같다). UE는 그 등록 호출에서는 다시 정렬하지 않고 다음 등록 때 정렬하지만, Rocks는 바로 다시 정렬한다(결과는 UE 다음 정렬과 같다). 순환 판정과 할당 실패를 구분한다(할당 실패는 등록을 되돌리고 `OutOfMemory`). `Unregister`는 할당 없이 순서 목록에서 그 칸만 빼고 뒤 색인을 당긴다(위상 순서는 그대로). Error 로그는 목록 잠금 밖에서 남긴다. `SourceCount`·`SourceNameAt`이 순서를 보인다(진단·시험).
- `FinishAll`: 모든 source를 부르고, 처음으로 남은 수가 0이 아닌 source에서 막는 진행 작업을 연다(할 일이 없으면 진행 작업을 만들지 않는다 — 저장마다 빈 작업이 기록에 남지 않게). 하위 작업 제목은 `DisplayName`, 유효하지 않으면 `Name`. 진행 작업은 `ProgressHub`가 켜져 있을 때만이다. source 오류는 처음 것을 돌려주고 나머지는 계속 끝낸다.
- `Shutdown`은 `RunBefore` 순서(정방향 — UE와 같다)로 source의 `Shutdown`을 부르고(두 번째부터 아무 일도 하지 않음) 그 뒤 `Register`는 `ShuttingDown`, `Unregister`는 된다. 등록이 남은 채 소멸하면 Warning.
- **Engine 연결.** `Engine::PendingWork()`. 엔진은 프레임 시작 hook(`BeginFrame`) 바로 뒤, 소유 쪽 객체 접근 scope 안에서 Subsystem `Tick`보다 먼저 `ApplyCompleted`를 부른다. 시간 예산은 `PendingWorkFrameBudgetNanoseconds`(16 ms — UE `FEngineLoop::Tick`이 `ProcessAsyncTasks(bLimitExecutionTime=true)`로 부르고 `FTextureCompilingManager`가 `MaxSecondsPerFrame = 0.016`을 쓴다)이다. `Shutdown` 처음(Subsystem이 내려가기 전)에 등록소 `Shutdown`을 부른다(UE `FEngineLoop::Exit`가 `PreExit` 전에 부르는 것과 같다). 등록소는 Subsystem보다 오래 산다(Subsystem이 `Deinitialize`에서 `Unregister`한다).
- **버린 것.** `FinishAll`에서 남은 수 0인 source 건너뛰기(적용 전 결과를 `RemainingCount`가 세지 않으면 저장 전에 낡은 상태가 남는다), 사이사이 `ApplyCompleted` 끼우기(UE에 없고 source가 자기 결과를 적용하면 필요 없다), `Shutdown` 역순(UE와 다르고 근거가 없다), 하위 작업에 등록소가 시작·끝 계수만 싣기(0/N → N/N 두 값뿐이고 source의 구간이 하위 작업 아래 붙지 않는다).
- **한계.** UE `FinishCompilationForObjects`는 끝낸 뒤에도 컴파일 중인 객체가 있으면 `FinishAll`로 넘어간다(`IInterface_AsyncCompilation::IsCompiling`). Rocks에는 객체 쪽 "빌드 중" 인터페이스가 아직 없어 그 보정이 없다(첫 적용 PG5에서 필요하면 넣는다).
