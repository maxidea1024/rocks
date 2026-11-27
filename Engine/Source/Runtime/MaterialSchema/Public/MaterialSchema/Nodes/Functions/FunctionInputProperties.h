#pragma once

#include <Base/Guid.h>
#include <Core/Math/Vector4.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/Name.h>
#include <Core/Text/String.h>
#include <MaterialSchema/Nodes/Functions/FunctionsNodeEnums.h>
#include <Meta/Annotations.h>

// Function Input 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionFunctionInput — RE/Public/Materials/MaterialExpressionFunctionInput.h).
// 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct FunctionInputProperties {
    ROCKS_GENERATED_BODY()

    explicit FunctionInputProperties(Allocator& allocator) noexcept : description(allocator) {
    }

    ROCKS_PROPERTY() Name inputName;
    ROCKS_PROPERTY() String description;
    ROCKS_PROPERTY() FunctionInputType inputType = FunctionInputType::Vector3;
    ROCKS_PROPERTY() Vec4 previewValue {};
    ROCKS_PROPERTY() bool usePreviewValueAsDefault {};
    ROCKS_PROPERTY() int32 sortPriority {};
    ROCKS_PROPERTY() BlendInputRelevance blendInputRelevance = BlendInputRelevance::General;
    // 입력 id다(함수 호출 노드의 동적 핀 키 P_<id> — NodeGraph GD2).
    ROCKS_PROPERTY() Guid id {};
};

} // namespace rocks::material
