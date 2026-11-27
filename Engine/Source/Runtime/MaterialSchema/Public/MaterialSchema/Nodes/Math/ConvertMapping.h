#pragma once

#include <Meta/Annotations.h>

// 노드 속성 안의 보조 값이다(Docs/Design/MaterialSystem 19.13.2절, UE
// FMaterialExpressionConvertMapping). UE의 입력 칸 (FExpressionInput)은 그래프 간선이 대신하므로
// 두지 않는다.
namespace rocks::material {

ROCKS_STRUCT()
struct ConvertMapping {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int32 inputIndex = 0;
    ROCKS_PROPERTY() int32 inputComponentIndex = 0;
    ROCKS_PROPERTY() int32 outputIndex = 0;
    ROCKS_PROPERTY() int32 outputComponentIndex = 0;
};

} // namespace rocks::material
