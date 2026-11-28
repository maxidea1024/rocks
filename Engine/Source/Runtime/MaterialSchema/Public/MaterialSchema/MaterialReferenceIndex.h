#pragma once

#include <Base/NumericLimits.h>
#include <Base/Types.h>

// 머티리얼 쿠킹 데이터가 다른 표를 가리키는 번호의 "없음" 값이다(Docs/Design/MaterialSystem 12.8절
// MS3d2 — 텍스처 slot의 파라미터 번호, 파라미터의 기본 텍스처 번호 등).
namespace rocks::material {

inline constexpr uint32 NoMaterialReference = NumericLimits<uint32>::Max();

} // namespace rocks::material
