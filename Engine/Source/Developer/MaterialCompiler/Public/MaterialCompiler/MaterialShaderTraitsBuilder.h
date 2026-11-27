#pragma once

#include <Core/Result.h>
#include <MaterialCompiler/MaterialTranslation.h>
#include <MaterialSchema/MaterialQualityLevel.h>
#include <MaterialSchema/MaterialShaderTraits.h>
#include <MaterialSchema/MaterialTraitsDesc.h>

// 번역 결과의 머티리얼 셰이더 특성이다(Docs/Design/MaterialSystem 8.3절 "유도 규칙" — MS3c).
//
// 사용 예:
//   MaterialShaderTraits traits(allocator);
//   if (auto made = MakeMaterialShaderTraits(translation, desc, target.quality, traits); !made)
//   {...}
namespace rocks::material {

// out을 translation(성공한 번역)·desc(번역 입력의 기본 속성)·quality에서 채운다(customOutputs는
// 비우고 채운다). 오류: 성공하지 않은 번역 InvalidArgument, 할당 실패.
ROCKS_MATERIALCOMPILER_API Result<void>
MakeMaterialShaderTraits(const MaterialTranslation& translation, const MaterialTraitsDesc& desc,
                         MaterialQualityLevel quality, MaterialShaderTraits& out) noexcept;

} // namespace rocks::material
