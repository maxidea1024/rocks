#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/Name.h>
#include <MaterialSchema/MaterialAssetReference.h>
#include <MaterialSchema/Nodes/MaterialAttributes/MaterialAttributesNodeEnums.h>
#include <Meta/Annotations.h>

// Aggregate 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionAggregate — RE/Public/Materials/MaterialExpressionAggregate.h). 이름·기본값은
// UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct AggregateProperties {
    ROCKS_GENERATED_BODY()

    explicit AggregateProperties(Allocator& allocator) noexcept : attributeNames(allocator) {
    }

    ROCKS_PROPERTY() AggregateKind kind = AggregateKind::MaterialAttributes;
    // TODO(MPQ-MaterialAggregate): 대상 에셋 클래스가 생기면 그 클래스로 검사한다(19.4.3절).
    // 기대 클래스는 MaterialAggregate다(19.4.3절).
    ROCKS_PROPERTY() MaterialAssetReference userAggregate;
    ROCKS_PROPERTY() Array<Name> attributeNames;
};

} // namespace rocks::material
