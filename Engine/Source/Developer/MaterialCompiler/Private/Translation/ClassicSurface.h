#pragma once

#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <MaterialCompiler/MaterialIrValue.h>

#include "AttributeEntry.h"

// 속성 집합에서 고전 preset 표면을 만든다(Docs/Design/MaterialSystem 6.12절 MS2e1 — 출력 노드
// useMaterialAttributes의 표면 변환, UE SubstrateConvertMaterialAttributes 숨은 변환). shading
// model 칸이 없으면 DefaultLit, 물 계수는 기본값이다. 위쪽 오류면 NoIrValue다.
namespace rocks::material::detail {

class LoweringContext;

Result<IrValueId> ClassicSurfaceFromAttributes(LoweringContext& context,
                                               Span<const AttributeEntry> entries) noexcept;

} // namespace rocks::material::detail
