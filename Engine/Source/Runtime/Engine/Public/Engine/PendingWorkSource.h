#pragma once

#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>

namespace rocks {

class Object;
class Text;

// 엔진이 프레임마다 ApplyCompleted에 주는 시간 예산이다(UE FEngineLoop::Tick이
// ProcessAsyncTasks(bLimitExecutionTime=true)로 부르고 FTextureCompilingManager가
// MaxSecondsPerFrame = 0.016을 쓴다).
constexpr uint64 PendingWorkFrameBudgetNanoseconds = 16'000'000;

// ApplyCompleted 매개변수다(Docs/Design/ProgressReporting 8장, UE FAssetCompilingManager::
// ProcessAsyncTasks 매개변수 대응).
struct PendingWorkPumpParams {
    // 0이면 제한 없음(UE bLimitExecutionTime). 아니면 이 시간 안에서 결과를 적용한다.
    uint64 timeBudgetNanoseconds = 0;
    // Tryout에 필요한 것만 적용한다(UE bPlayInEditorAssetsOnly).
    bool tryoutAssetsOnly = false;
};

// 비동기로 만드는 일 한 종류(셰이더·텍스처·메시 빌드 등)다(Docs/Design/ProgressReporting 8장, UE
// IAssetCompilingManager 대응). 모듈이 만들어 PendingWorkRegistry에 등록한다. RemainingCount 말고는
// 등록소의 소유 스레드에서 불린다.
//
// 계약:
// - RemainingCount는 등록소 목록 잠금 안에서 아무 스레드에서나 불린다: 잠금 없는 원자 읽기여야 하고
//   등록소를 다시 부르면 안 된다(잠금 순서 역전·재진입 교착).
// - Name·RunBefore는 등록 동안 바뀌지 않는다.
// - Shutdown은 두 번 불려도 된다(엔진 종료 때 등록소가, 모듈이 내릴 때 모듈이 부른다). Shutdown
//   뒤에도 FinishAll·FinishFor·ApplyCompleted가 올 수 있다(남은 일이 없으니 바로 돌아가면 된다).
// - 등록소 호출 안에서 등록소의 Register·Unregister·FinishAll 등은 Busy다.
//
// 구현 예:
//   class ShaderPendingWork final : public PendingWorkSource {
//   public:
//       StringView Name() const noexcept override { return "Shaders"; }
//       const Text& DisplayName() const noexcept override { return displayName_; }
//       Span<const StringView> RunBefore() const noexcept override { return {}; }
//       uint64 RemainingCount() const noexcept override { return manager_.Pending(); }
//       ...
//   };
class PendingWorkSource {
public:
    virtual ~PendingWorkSource() = default;

    // 고유 종류 이름이다(예: "Shaders"). 비면 등록할 수 없다.
    virtual StringView Name() const noexcept = 0;
    // 상태 표시줄·진행 창 문구다.
    virtual const Text& DisplayName() const noexcept = 0;
    // 먼저 끝낼 종류 이름이다(UE GetDependentTypeNames). 없는 이름은 무시된다.
    virtual Span<const StringView> RunBefore() const noexcept = 0;
    // 남은 일 수다. 아무 스레드에서나 불린다.
    virtual uint64 RemainingCount() const noexcept = 0;
    // targets의 일만 끝날 때까지 막는다(자기 대상이 아니면 아무 일도 하지 않는다).
    virtual Result<void> FinishFor(Span<Object* const> targets) noexcept = 0;
    // targets의 일을 취소한다(힌트 — 이미 도는 일은 끝까지 갈 수 있다).
    virtual void CancelFor(Span<Object* const> targets) noexcept = 0;
    // 모든 일이 끝날 때까지 막고, 끝난 결과를 대상에 적용한 뒤 돌아간다(UE PostCompilation과
    // 같다 — 등록소는 사이사이 ApplyCompleted를 끼우지 않는다). 등록소가 하위 진행 작업을 이
    // 스레드에 바인딩해 두므로 안의 ProgressScope가 그 작업의 진행이 된다.
    virtual Result<void> FinishAll() noexcept = 0;
    // 남은 일을 취소하고 안전해질 때까지 기다린다(모듈을 내리기 전).
    virtual void Shutdown() noexcept = 0;
    // 끝난 결과를 대상에 적용한다(소유 스레드 — 엔진에서는 프레임마다 소유 쪽 ObjectAccessScope
    // 안에서, 예산 PendingWorkFrameBudgetNanoseconds로).
    virtual void ApplyCompleted(const PendingWorkPumpParams& params) noexcept = 0;
};

} // namespace rocks
