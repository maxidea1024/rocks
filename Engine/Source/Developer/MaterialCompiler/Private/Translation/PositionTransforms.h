#pragma once

#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <MaterialCompiler/MaterialIrValue.h>

// 다른 노드가 쓰는 위치 공간 변환이다(Docs/Design/MaterialSystem 6.9절 MS2b2b — 구현은
// LowerTransforms.cpp, UE TransformPosition과 같은 검증·직접 쌍).
namespace rocks::material::detail {

class LoweringContext;

// TranslatedWorld 위치 value를 World(Double3)로 바꾼다(UE TransformPosition(MCB_TranslatedWorld,
// MCB_World) — Double(A) − PreViewTranslation). 3성분 미만이면 UE 문구 "input must be a
// 3-component vector …"를 pin 위치에 진단하고 NoIrValue다(Float4는 앞 셋). value가 NoIrValue면
// 진단 없이 NoIrValue다.
Result<IrValueId> TranslatedWorldPositionToWorld(LoweringContext& context, IrValueId value,
                                                 StringView pin) noexcept;

} // namespace rocks::material::detail
