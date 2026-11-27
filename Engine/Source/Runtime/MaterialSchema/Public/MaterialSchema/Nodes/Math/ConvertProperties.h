#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/String.h>
#include <MaterialSchema/Nodes/Math/ConvertInput.h>
#include <MaterialSchema/Nodes/Math/ConvertMapping.h>
#include <MaterialSchema/Nodes/Math/ConvertOutput.h>
#include <Meta/Annotations.h>

// Convert 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE UMaterialExpressionConvert —
// RE/Public/Materials/MaterialExpressionConvert.h). 이름·기본값은 UE와 같다(b 접두를 떼고
// camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct ConvertProperties {
    ROCKS_GENERATED_BODY()

    explicit ConvertProperties(Allocator& allocator) noexcept
        : convertInputs(allocator), convertOutputs(allocator), nodeName(allocator),
          convertMappings(allocator) {
    }

    ROCKS_PROPERTY() Array<ConvertInput> convertInputs;
    ROCKS_PROPERTY() Array<ConvertOutput> convertOutputs;
    ROCKS_PROPERTY() String nodeName;
    // 입력 성분 → 출력 성분 대응이다(UE ConvertMappings — 그래프 편집이 채운다).
    ROCKS_PROPERTY() Array<ConvertMapping> convertMappings;
};

} // namespace rocks::material
