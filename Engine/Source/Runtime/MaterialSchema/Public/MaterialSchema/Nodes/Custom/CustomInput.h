#pragma once

#include <Core/Text/Name.h>
#include <Meta/Annotations.h>

// 노드 속성 안의 보조 값이다(Docs/Design/MaterialSystem 19.13.2절, UE FCustomInput). UE의 입력 칸
// (FExpressionInput)은 그래프 간선이 대신하므로 두지 않는다.
namespace rocks::material {

ROCKS_STRUCT()
struct CustomInput {
    ROCKS_GENERATED_BODY()

    // 입력 핀 이름이다(동적 핀 키·HLSL 인자 이름).
    ROCKS_PROPERTY() Name inputName;
};

} // namespace rocks::material
