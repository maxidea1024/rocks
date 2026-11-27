#pragma once

#include <Base/Guid.h>
#include <Core/Text/Name.h>
#include <MaterialSchema/Nodes/Functions/FunctionsNodeEnums.h>
#include <Meta/Annotations.h>

// 노드 속성 안의 보조 값이다(Docs/Design/MaterialSystem 19.13.2절, UE FFunctionCallInput). UE의
// 입력 칸 (FExpressionInput)은 그래프 간선이 대신하므로 두지 않는다.
namespace rocks::material {

ROCKS_STRUCT()
struct FunctionCallInput {
    ROCKS_GENERATED_BODY()

    // 함수 입력 노드 id다(동적 핀 키 P_<id>).
    ROCKS_PROPERTY() Guid id {};
    // 표시 이름이다(함수 입력 노드의 inputName).
    ROCKS_PROPERTY() Name inputName;
    // 입력 값 종류다.
    ROCKS_PROPERTY() FunctionInputType inputType = FunctionInputType::Vector3;
    // 연결이 없으면 함수 입력의 미리보기 값을 쓴다(아니면 필수 입력).
    ROCKS_PROPERTY() bool usePreviewValueAsDefault = false;
};

} // namespace rocks::material
