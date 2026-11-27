#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/String.h>
#include <MaterialSchema/Nodes/Switches/SwitchInput.h>
#include <Meta/Annotations.h>

// Switch 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE UMaterialExpressionSwitch —
// RE/Public/Materials/MaterialExpressionSwitch.h). 이름·기본값은 UE와 같다(b 접두를 떼고
// camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct SwitchProperties {
    ROCKS_GENERATED_BODY()

    explicit SwitchProperties(Allocator& allocator) noexcept
        : description(allocator), inputs(allocator) {
    }

    ROCKS_PROPERTY() String description;
    ROCKS_PROPERTY() Array<SwitchInput> inputs;
};

} // namespace rocks::material
