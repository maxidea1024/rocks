#pragma once

#include <Core/Text/Name.h>
#include <MaterialSchema/Nodes/Custom/CustomNodeEnums.h>
#include <Meta/Annotations.h>

// 노드 속성 안의 보조 값이다(Docs/Design/MaterialSystem 19.13.2절, UE FCustomOutput). UE의 입력 칸
// (FExpressionInput)은 그래프 간선이 대신하므로 두지 않는다.
namespace rocks::material {

ROCKS_STRUCT()
struct CustomOutput {
    ROCKS_GENERATED_BODY()

    // 추가 출력 핀 이름이다(HLSL out 인자 이름).
    ROCKS_PROPERTY() Name outputName;
    ROCKS_PROPERTY() CustomOutputType outputType = CustomOutputType::Float1;
};

} // namespace rocks::material
