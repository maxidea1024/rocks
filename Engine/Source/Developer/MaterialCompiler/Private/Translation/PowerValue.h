#pragma once

#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <MaterialCompiler/MaterialIrValue.h>

// UE Power 규칙이다(Docs/Design/MaterialSystem 6.8절 — UE FHLSLMaterialTranslator::Power,
// HLSLMaterialTranslator.cpp:9819): 결과는 base 타입, 지수는 float 스칼라(벡터면 첫 성분), 상수
// 지수 1이면 base, 0이면 1, 상수 밑 0이면 0, 그 밖은 PositiveClampedPow(밑 ≤ 2.980233e-8이면 0)다.
// Power 노드와 Fresnel(6.13절 MS2f1)이 쓴다. pin은 진단 위치(지수 입력)다.
namespace rocks::material::detail {

class LoweringContext;

Result<IrValueId> PowerValue(LoweringContext& context, IrValueId base, IrValueId exponent,
                             StringView pin) noexcept;

} // namespace rocks::material::detail
