#pragma once

#include <Core/Containers/Array.h>
#include <Core/Math/Color.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>

// Dynamic Parameter 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionDynamicParameter — RE/Public/Materials/MaterialExpressionDynamicParameter.h).
// 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct DynamicParameterProperties {
    ROCKS_GENERATED_BODY()

    explicit DynamicParameterProperties(Allocator& allocator) noexcept : paramNames(allocator) {
    }

    ROCKS_PROPERTY() Array<String> paramNames;
    ROCKS_PROPERTY() LinearColor defaultValue = LinearColor(1.0f, 1.0f, 1.0f, 1.0f);
    ROCKS_PROPERTY() uint32 parameterIndex = 0;
};

} // namespace rocks::material
