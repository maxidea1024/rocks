#pragma once

#include <Base/Types.h>

// 머티리얼 값의 셰이더 단계 mask다(Docs/Design/MaterialSystem 6.4절 V4). 노드 정의와 문맥 값 표가
// 쓴다.
namespace rocks::material {

// 노드 값이 계산될 수 있는 셰이더 단계다(V4). 비트 0 정점, 비트 1 픽셀.
using MaterialStageMask = uint8;

inline constexpr MaterialStageMask MaterialVertexStage = 1u << 0;
inline constexpr MaterialStageMask MaterialPixelStage = 1u << 1;
inline constexpr MaterialStageMask AllMaterialStages = MaterialVertexStage | MaterialPixelStage;

} // namespace rocks::material
