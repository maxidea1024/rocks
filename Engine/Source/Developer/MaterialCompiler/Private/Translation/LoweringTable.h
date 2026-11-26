#pragma once

#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>

// 노드 종류 → lowering 함수 표다(Docs/Design/MaterialSystem 6.8절 "lowering 표"). 함수는 노드의
// 입력 값을 요청하고 출력 핀마다 IR 값을 낸다. 표에 없는 카탈로그 종류는 NotYetLowered다.
namespace rocks::material::detail {

class LoweringContext;

// 노드 하나의 lowering이다. 그래프 오류는 context로 진단하고 성공을 돌려준다(출력을 내지 않으면
// 받는 쪽이 NoIrValue를 본다). 실패는 시스템 오류뿐이다.
using LoweringFn = Result<void> (*)(LoweringContext& context) noexcept;

struct LoweringEntry {
    StringView type;
    LoweringFn fn = nullptr;
    // 참이면 eagerInputs 밖의 입력은 번역 순서 계획에 넣지 않고 lowering 함수가 요청할 때
    // 번역한다(번역 때 고르는 스위치의 가지 — 6.9절 MS2b3).
    bool lazyInputs = false;
    Span<const StringView> eagerInputs = {};
};

// 묶음별 표다(파일마다 하나 —
// LowerConstants·LowerMath·LowerFlow·LowerContextValues·LowerCoordinates·LowerTransforms·LowerSwitches·LowerInterpolators·LowerSupply·LowerParameters).
Span<const LoweringEntry> ConstantLowerings() noexcept;
Span<const LoweringEntry> MathLowerings() noexcept;
Span<const LoweringEntry> FlowLowerings() noexcept;
Span<const LoweringEntry> ContextValueLowerings() noexcept;
Span<const LoweringEntry> CoordinateLowerings() noexcept;
Span<const LoweringEntry> TransformLowerings() noexcept;
Span<const LoweringEntry> SwitchLowerings() noexcept;
Span<const LoweringEntry> InterpolatorLowerings() noexcept;
Span<const LoweringEntry> SupplyLowerings() noexcept;
Span<const LoweringEntry> ParameterLowerings() noexcept;

} // namespace rocks::material::detail
