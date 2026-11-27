#pragma once

#include <Core/Math/Color.h>
#include <MaterialSchema/Nodes/Math/MathNodeEnums.h>
#include <Meta/Annotations.h>

// 노드 속성 안의 보조 값이다(Docs/Design/MaterialSystem 19.13.2절, UE
// FMaterialExpressionConvertInput). UE의 입력 칸 (FExpressionInput)은 그래프 간선이 대신하므로 두지
// 않는다.
namespace rocks::material {

ROCKS_STRUCT()
struct ConvertInput {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() ConvertValueType type = ConvertValueType::Scalar;
    // 연결이 없을 때의 값이다.
    ROCKS_PROPERTY() LinearColor defaultValue = LinearColor(0.0f, 0.0f, 0.0f, 1.0f);
};

} // namespace rocks::material
