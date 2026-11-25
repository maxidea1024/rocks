#pragma once

#include <Base/Guid.h>
#include <Core/Text/StringView.h>
#include <MaterialSchema/MaterialNodeDefinition.h>
#include <MaterialSchema/MaterialPinType.h>

// 머티리얼 속성 정의다(Docs/Design/MaterialSystem 19.12절, UE FMaterialAttributeDefintion —
// RE/Public/Materials/MaterialAttributeDefinitionMap.h:83–160). 속성 집합(MaterialAttributes 값)의
// 한 칸이다. Make·Break·Get·Set·Blend Attributes·레이어 함수가 Guid로 가리킨다. 내장 속성의 Guid는
// UE와 같아 UE 변환이 Guid로 잇는다.
//
// 수명: name이 가리키는 data는 등록 module lease가 지킨다.
namespace rocks::material {

struct MaterialAttributeDefinition {
    Guid id;
    // 영어 이름이다(진단·표시).
    StringView name;
    MaterialPinType type = MaterialPinType::Float1;
    MaterialPinDefault defaultValue;
    // 계산 단계다(정점 또는 픽셀 하나).
    MaterialStageMask frequency = MaterialPixelStage;
    // 팔레트·Break 노드에 보이지 않는다(UE bHideAttribute — 조명 굽기 유도 값 등).
    bool hidden = false;
};

} // namespace rocks::material
