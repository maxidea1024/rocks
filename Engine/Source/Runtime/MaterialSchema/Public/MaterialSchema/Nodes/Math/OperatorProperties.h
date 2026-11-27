#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <MaterialSchema/Nodes/Math/MathNodeEnums.h>
#include <MaterialSchema/Nodes/Math/OperatorInput.h>
#include <Meta/Annotations.h>

// Operator 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE UMaterialExpressionOperator
// — RE/Public/Materials/MaterialExpressionOperator.h). 이름·기본값은 UE와 같다(b 접두를 떼고
// camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct OperatorProperties {
    ROCKS_GENERATED_BODY()

    explicit OperatorProperties(Allocator& allocator) noexcept : dynamicInputs(allocator) {
    }

    ROCKS_PROPERTY() Array<OperatorInput> dynamicInputs;
    ROCKS_PROPERTY() OperatorKind operatorKind = OperatorKind::Add;
};

} // namespace rocks::material
