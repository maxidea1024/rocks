#pragma once

#include <Core/Text/StringView.h>
#include <MaterialCompiler/MaterialIrModule.h>

// 모듈 결과 조회다(Docs/Design/MaterialSystem 7.3절 MATERIAL_USES_* 판정 — 생성 원문과 셰이더 특성
// MakeMaterialShaderTraits(8.3절)가 같은 판정을 쓴다).
namespace rocks::material::detail {

// (entry, slot) 결과다(없으면 nullptr).
const IrResult* FindIrResult(const IrModule& module, IrEntry entry, StringView slot) noexcept;

// 결과가 없거나 Float 상수 0(부호 무시 — +0·-0)인가다.
bool IsZeroIrResult(const IrModule& module, const IrResult* result) noexcept;

} // namespace rocks::material::detail
