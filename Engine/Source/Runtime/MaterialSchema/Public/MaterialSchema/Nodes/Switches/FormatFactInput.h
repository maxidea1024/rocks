#pragma once

#include <Core/Text/Name.h>
#include <MaterialSchema/Nodes/Switches/SwitchesNodeEnums.h>
#include <Meta/Annotations.h>

// 노드 속성 안의 보조 값이다(Docs/Design/MaterialSystem 19.13.2절, UE
// FDataDrivenShaderPlatformInfoInput). UE의 입력 칸 (FExpressionInput)은 그래프 간선이 대신하므로
// 두지 않는다.
namespace rocks::material {

ROCKS_STRUCT()
struct FormatFactInput {
    ROCKS_GENERATED_BODY()

    // 형식 성질 이름이다.
    ROCKS_PROPERTY() Name inputName;
    ROCKS_PROPERTY() FormatFactCondition propertyCondition = FormatFactCondition::True;
};

} // namespace rocks::material
