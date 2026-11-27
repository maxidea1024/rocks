#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>

#include "FunctionSignaturePin.h"

// 함수 그래프의 입출력 목록이다(Docs/Design/MaterialSystem 6.12절 MS2e3 — UE
// UMaterialFunction::GetInputsAndOutputs). 목록은 정렬 순(sortPriority 오름차순, 같으면 그래프 노드
// 순)이다.
namespace rocks::material::detail {

struct FunctionSignature {
    explicit FunctionSignature(Allocator& allocator) noexcept
        : inputs(allocator), outputs(allocator) {
    }

    // MaterialAttributes 형 입력·출력 수다(UE FValidLayerUsageTracker).
    usize AttributeInputs() const noexcept {
        usize count = 0;
        for (const FunctionSignaturePin& pin : inputs) {
            count += pin.attributes ? 1 : 0;
        }

        return count;
    }

    usize AttributeOutputs() const noexcept {
        usize count = 0;
        for (const FunctionSignaturePin& pin : outputs) {
            count += pin.attributes ? 1 : 0;
        }

        return count;
    }

    Array<FunctionSignaturePin> inputs;
    Array<FunctionSignaturePin> outputs;
    // static 노드(UE IsStaticExpression)가 있는가.
    bool hasStatic = false;
};

} // namespace rocks::material::detail
