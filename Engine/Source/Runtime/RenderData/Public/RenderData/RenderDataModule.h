#pragma once

#include <Core/Result.h>

// RenderData 모듈의 리플렉션 타입 등록이다(Docs/Design/Renderer 3.2절).
namespace rocks {

class ModuleRegistration;

// 번호표 열거(MaterialEnums.h)를 registration에 등록한다. 이 열거를 필드로 쓰는 모듈(머티리얼
// 스키마·에셋)의 등록보다 먼저 부른다. 오류: registration의 등록 오류를 그대로 돌려준다.
ROCKS_RENDERDATA_API Result<void>
RegisterRenderDataTypes(ModuleRegistration& registration) noexcept;

} // namespace rocks
