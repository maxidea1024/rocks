#pragma once

#include <MaterialCompiler/MaterialCurveAtlasSnapshot.h>
#include <MaterialCompiler/MaterialIrType.h>
#include <MaterialSchema/MaterialParameterMetadata.h>

// 파라미터 기본값 해석이다(Docs/Design/MaterialSystem 12.8절 MS3d1). 노드 hook(ParameterValue)이
// 읽은 값을 IR 파라미터 기본값으로 바꾼다. lowering과 쿠킹 처리기가 이 함수만 써서 너겟 적중 때
// 모은 기본값이 미적중 IR 기본값과 같다. 함수 인스턴스 override(UE GetParameterOverrideValue)는
// MS6이 여기에 더한다.
//
// 사용 예:
//   double defaults[MaxIrComponents] = {};
//   if (!ResolveParameterDefaults(metadata, atlasSnapshot, defaults)) { /* 행 없음 */ }
namespace rocks::material {

// metadata의 기본값을 out에 쓴다. 커브 atlas 행이면 행 번호(atlas에서 커브의 첫 행 — UE
// GetCurveIndex, float로 둔 값)이고 atlas가 null이거나 커브가 없으면 false다(lowering이 그 오류를
// 진단한다). 그 밖은 metadata.defaults 그대로다.
ROCKS_MATERIALCOMPILER_API bool ResolveParameterDefaults(const MaterialParameterMetadata& metadata,
                                                         const MaterialCurveAtlasSnapshot* atlas,
                                                         double (&out)[MaxIrComponents]) noexcept;

} // namespace rocks::material
