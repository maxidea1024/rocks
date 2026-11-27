#pragma once

#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <MaterialSchema/MaterialNodeDefinition.h>

// MaterialSchema 모듈의 엔진 노드 묶음이다(Docs/Design/MaterialSystem 19장, 단계 MS1a·MS1b).
//
// 사용 예:
//   MaterialNodeCatalog catalog(allocator);
//   MaterialGraphSchema schema(MaterialGraphKind::Material, catalog, names, allocator);
//   registry.RegisterSchema(schema, lease);
//   catalog.RegisterGroups(registry, schema, BuiltinMaterialNodeGroups(), lease);
namespace rocks {

class ModuleRegistration;

} // namespace rocks

namespace rocks::material {

// MaterialSchema 리플렉션 타입(traits·override·파라미터 키 등)을 등록한다(생성 집계 함수
// generated::MaterialSchema::RegisterModuleTypes — 의존 모듈 타입도 함께). 이 타입을 필드로 쓰는
// 모듈 등록보다 먼저 부른다. 오류: 등록 오류를 그대로 돌려준다.
ROCKS_MATERIALSCHEMA_API Result<void>
RegisterMaterialSchemaTypes(ModuleRegistration& registration) noexcept;

// 엔진 노드 정의 배열 묶음이다(범주별). 수명: 정적이다(MaterialSchema 모듈 data). 엔진 노드는 이
// 모듈의 lease로 등록한다.
ROCKS_MATERIALSCHEMA_API Span<const Span<const MaterialNodeDefinition>>
BuiltinMaterialNodeGroups() noexcept;

} // namespace rocks::material
