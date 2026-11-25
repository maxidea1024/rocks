#pragma once

#include <Base/Guid.h>
#include <Core/Math/Color.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/Name.h>
#include <MaterialSchema/Nodes/Parameters/ParameterChannelNames.h>
#include <Meta/Annotations.h>

// Vector Parameter 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionVectorParameter — RE/Public/Materials/MaterialExpressionVectorParameter.h).
// 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct VectorParameterProperties {
    ROCKS_GENERATED_BODY()

    explicit VectorParameterProperties(Allocator& allocator) noexcept : channelNames(allocator) {
    }

    ROCKS_PROPERTY() Name parameterName;
    ROCKS_PROPERTY() Guid parameterId {};
    ROCKS_PROPERTY() Name group;
    ROCKS_PROPERTY() int32 sortPriority = 32;
    ROCKS_PROPERTY() LinearColor defaultValue {};
    ROCKS_PROPERTY() bool useCustomPrimitiveData = false;
    ROCKS_PROPERTY() uint8 primitiveDataIndex = 0;
    ROCKS_PROPERTY() ParameterChannelNames channelNames;
};

} // namespace rocks::material
