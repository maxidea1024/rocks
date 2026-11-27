#pragma once

#include <Base/Guid.h>
#include <Core/Text/Name.h>
#include <Meta/Annotations.h>

// 노드 속성 안의 보조 값이다(Docs/Design/MaterialSystem 19.13.2절, UE FFunctionCallOutput). UE의
// 입력 칸 (FExpressionInput)은 그래프 간선이 대신하므로 두지 않는다.
namespace rocks::material {

ROCKS_STRUCT()
struct FunctionCallOutput {
    ROCKS_GENERATED_BODY()

    // 함수 출력 노드 id다(동적 핀 키 P_<id>).
    ROCKS_PROPERTY() Guid id {};
    // 표시 이름이다(함수 출력 노드의 outputName).
    ROCKS_PROPERTY() Name outputName;
};

} // namespace rocks::material
