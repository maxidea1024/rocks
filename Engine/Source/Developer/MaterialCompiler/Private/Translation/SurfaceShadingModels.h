#pragma once

#include <Core/Result.h>
#include <MaterialCompiler/MaterialIrBuilder.h>
#include <MaterialCompiler/MaterialIrValue.h>

// shading model 값에서 static으로 닿는 상수의 집합이다(Docs/Design/MaterialSystem 6.11절 MS2d1 —
// UE ShadingModelsFromCompilation). Select·Branch는 두 쪽을 따라가고, 상수가 아닌 값이 섞이면
// Renderer 14.2절 번호표 전부다.
namespace rocks::material::detail {

// 번호표의 비트 전부다.
uint32 AllShadingModelBits() noexcept;

// value에서 닿는 shading model 비트(1 << 번호)다.
Result<uint32> ReachableShadingModels(const IrBuilder& builder, IrValueId value) noexcept;

} // namespace rocks::material::detail
