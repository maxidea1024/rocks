#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <InputActions/InputTrigger.h>
#include <InputActions/InputValue.h>

namespace rocks::input {

// 여러 트리거의 결합 상태다(UE FTriggerStateTracker — Docs/Design/InputActions 3.3절). 바인딩
// 트리거와 액션 트리거를 같은 결합기에 이어서 평가할 수 있다(4.3절).
//
// 사용 예:
//   TriggerCombiner combiner;
//   combiner.Evaluate(triggers, states, value, context);
//   if (combiner.State() == TriggerState::Triggered) { ... }
//
// 스레드: 값 타입이다.
class ROCKS_INPUTACTIONS_API TriggerCombiner {
public:
    // triggers와 states(같은 길이여야 한다 — 다르면 assert)를 모두 평가하고 결합 상태를 돌려준다.
    // 일찍 멈추지 않는다(모든 트리거가 누적 시간을 갱신해야 한다 — UE InputAction.cpp:77). 각
    // 트리거의 직전 값을 value로 갱신한다.
    TriggerState Evaluate(Span<const InputTrigger> triggers, Span<InputTriggerState> states,
                          const InputValue& value, const TriggerContext& context) noexcept;

    // 결합 상태다(UE GetState).
    TriggerState State() const noexcept;

    // 트리거를 하나도 평가하지 않았을 때의 상태다(평가기가 최종 값 nonzero로 정한다 — UE
    // SetStateForNoTriggers).
    void SetStateForNoTriggers(TriggerState state) noexcept {
        noTriggerState_ = state;
    }

    bool EvaluatedTriggers() const noexcept {
        return evaluatedTriggers_;
    }

    // 바인딩에 트리거가 있었다는 표지다(UE SetMappingTriggerApplied — 액션 마무리가 결과를 바인딩
    // 상태로 제한한다, 4.3절 4).
    void SetMappingTriggerApplied(bool applied) noexcept {
        mappingTriggerApplied_ = applied;
    }

    bool MappingTriggerApplied() const noexcept {
        return mappingTriggerApplied_;
    }

    // (상태, 트리거 평가 여부) 순서 비교다(UE operator<) — 바인딩 중 가장 진전된 것을 고른다.
    bool operator<(const TriggerCombiner& other) const noexcept;

private:
    TriggerState noTriggerState_ = TriggerState::None;
    bool evaluatedTriggers_ = false;
    bool foundActive_ = false;
    bool anyExplicitTriggered_ = false;
    bool foundExplicit_ = false;
    bool allImplicitsTriggered_ = true;
    bool blocking_ = false;
    bool mappingTriggerApplied_ = false;
};

} // namespace rocks::input
