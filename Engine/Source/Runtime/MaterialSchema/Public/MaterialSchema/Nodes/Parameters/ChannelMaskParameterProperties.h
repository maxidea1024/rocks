#pragma once

#include <Base/Guid.h>
#include <Core/Math/Color.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/Name.h>
#include <MaterialSchema/Nodes/Parameters/ParameterChannelNames.h>
#include <MaterialSchema/Nodes/Parameters/ParametersNodeEnums.h>
#include <Meta/Annotations.h>

// Channel Mask Parameter 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionChannelMaskParameter —
// RE/Public/Materials/MaterialExpressionChannelMaskParameter.h). 이름·기본값은 UE와 같다(b 접두를
// 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct ChannelMaskParameterProperties {
    ROCKS_GENERATED_BODY()

    explicit ChannelMaskParameterProperties(Allocator& allocator) noexcept
        : channelNames(allocator) {
    }

    ROCKS_PROPERTY() Name parameterName;
    ROCKS_PROPERTY() Guid parameterId {};
    ROCKS_PROPERTY() Name group;
    ROCKS_PROPERTY() int32 sortPriority = 32;
    ROCKS_PROPERTY() LinearColor defaultValue = LinearColor(1.0f, 0.0f, 0.0f, 0.0f);
    ROCKS_PROPERTY() bool useCustomPrimitiveData = false;
    ROCKS_PROPERTY() uint8 primitiveDataIndex = 0;
    ROCKS_PROPERTY() ParameterChannelNames channelNames;
    ROCKS_PROPERTY() ChannelMaskParameterColor maskChannel = ChannelMaskParameterColor::Red;
};

} // namespace rocks::material
