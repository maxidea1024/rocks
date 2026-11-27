#pragma once

#include <Core/Text/Name.h>
#include <MaterialSchema/Nodes/Functions/FunctionsNodeEnums.h>
#include <Meta/Annotations.h>

// 노드 속성 안의 보조 값이다(Docs/Design/MaterialSystem 19.13.2절, UE FMaterialLayerInput). UE의
// 입력 칸 (FExpressionInput)은 그래프 간선이 대신하므로 두지 않는다.
namespace rocks::material {

ROCKS_STRUCT()
struct LayerStackInput {
    ROCKS_GENERATED_BODY()

    // 레이어 함수 입력 이름이다(동적 핀 키).
    ROCKS_PROPERTY() Name inputName;
    // 입력 값 종류다.
    ROCKS_PROPERTY() FunctionInputType inputType = FunctionInputType::Vector3;
};

} // namespace rocks::material
