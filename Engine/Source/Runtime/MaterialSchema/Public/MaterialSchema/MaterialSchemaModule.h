#pragma once

#include <Core/Containers/Span.h>
#include <MaterialSchema/MaterialNodeDefinition.h>

// MaterialSchema 모듈의 엔진 노드 묶음이다(Docs/Design/MaterialSystem 19장, 단계 MS1a·MS1b).
//
// 사용 예:
//   MaterialNodeCatalog catalog(allocator);
//   MaterialGraphSchema schema(MaterialGraphKind::Material, catalog, names, allocator);
//   registry.RegisterSchema(schema, lease);
//   catalog.RegisterGroups(registry, schema, BuiltinMaterialNodeGroups(), lease);
namespace rocks::material {

// 엔진 노드 정의 배열 묶음이다(범주별). 수명: 정적이다(MaterialSchema 모듈 data). 엔진 노드는 이
// 모듈의 lease로 등록한다.
ROCKS_MATERIALSCHEMA_API Span<const Span<const MaterialNodeDefinition>>
BuiltinMaterialNodeGroups() noexcept;

} // namespace rocks::material
