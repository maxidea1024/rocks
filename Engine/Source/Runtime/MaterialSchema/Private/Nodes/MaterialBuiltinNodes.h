#pragma once

#include <Core/Containers/Span.h>
#include <MaterialSchema/MaterialNodeDefinition.h>

// 엔진 노드 정의 묶음이다(Docs/Design/MaterialSystem 19.6절). 범주 파일마다 함수 하나가 정적 정의
// 배열을 돌려준다. BuiltinMaterialNodeGroups(MaterialSchemaModule.cpp)가 모은다.
namespace rocks::material::detail {

Span<const MaterialNodeDefinition> OutputNodeDefinitions() noexcept;

} // namespace rocks::material::detail
