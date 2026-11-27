#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <MaterialSchema/Nodes/Switches/FormatFactInput.h>
#include <Meta/Annotations.h>

// Format Fact Switch 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionDataDrivenShaderPlatformInfoSwitch —
// RE/Public/Materials/MaterialExpressionDataDrivenShaderPlatformInfoSwitch.h). 이름·기본값은 UE와
// 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct FormatFactSwitchProperties {
    ROCKS_GENERATED_BODY()

    explicit FormatFactSwitchProperties(Allocator& allocator) noexcept
        : ddspiPropertyNames(allocator) {
    }

    ROCKS_PROPERTY() Array<FormatFactInput> ddspiPropertyNames;
};

} // namespace rocks::material
