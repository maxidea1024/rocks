#pragma once

#include <Base/Guid.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Meta/Annotations.h>

// Get Attributes 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionGetMaterialAttributes —
// RE/Public/Materials/MaterialExpressionGetMaterialAttributes.h). 이름·기본값은 UE와 같다(b 접두를
// 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct GetAttributesProperties {
    ROCKS_GENERATED_BODY()

    explicit GetAttributesProperties(Allocator& allocator) noexcept : attributeGetTypes(allocator) {
    }

    ROCKS_PROPERTY() Array<Guid> attributeGetTypes;
};

} // namespace rocks::material
