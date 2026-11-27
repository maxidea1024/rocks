#pragma once

#include <Base/Guid.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Meta/Annotations.h>

// Set Attributes 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionSetMaterialAttributes —
// RE/Public/Materials/MaterialExpressionSetMaterialAttributes.h). 이름·기본값은 UE와 같다(b 접두를
// 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct SetAttributesProperties {
    ROCKS_GENERATED_BODY()

    explicit SetAttributesProperties(Allocator& allocator) noexcept : attributeSetTypes(allocator) {
    }

    ROCKS_PROPERTY() Array<Guid> attributeSetTypes;
};

} // namespace rocks::material
