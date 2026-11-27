#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <MaterialSchema/MaterialAssetReference.h>
#include <MaterialSchema/Nodes/Functions/FunctionCallInput.h>
#include <MaterialSchema/Nodes/Functions/FunctionCallOutput.h>
#include <Meta/Annotations.h>

// Function Call 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionMaterialFunctionCall —
// RE/Public/Materials/MaterialExpressionMaterialFunctionCall.h). 이름·기본값은 UE와 같다(b 접두를
// 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct FunctionCallProperties {
    ROCKS_GENERATED_BODY()

    explicit FunctionCallProperties(Allocator& allocator) noexcept
        : inputs(allocator), outputs(allocator) {
    }

    // 기대 클래스는 MaterialFunctionBase다(19.4.3절).
    ROCKS_PROPERTY() MaterialAssetReference materialFunction;
    // 함수 입력 노드의 사본이다(UE FunctionInputs). 함수가 바뀌면 에디터가 다시 채운다.
    ROCKS_PROPERTY() Array<FunctionCallInput> inputs;
    // 함수 출력 노드의 사본이다(UE FunctionOutputs).
    ROCKS_PROPERTY() Array<FunctionCallOutput> outputs;
};

} // namespace rocks::material
