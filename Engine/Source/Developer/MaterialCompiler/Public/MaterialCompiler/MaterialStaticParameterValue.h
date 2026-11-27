#pragma once

#include <Base/Guid.h>
#include <Base/Types.h>

// 번역 입력의 static 파라미터 값 하나다(Docs/Design/MaterialSystem 6.10절 MS2c2, 9.5·9.6절).
// 인스턴스 사슬을 평탄화한 값이고(쿠킹 MS3이 만든다) 목록에 없는 파라미터는 노드 기본값이다(UE
// StaticParameters 정확 일치 — HLSLMaterialTranslator.cpp:8947–8991).
//
// 사용 예:
//   const MaterialStaticParameterValue values[] = {
//       {switchId, MaterialStaticParameterKind::Bool, true},
//   };
//   input.staticParameters = values;
namespace rocks::material {

// static 값 종류다(StaticBool·StaticSwitch 파라미터는 Bool, StaticComponentMask 파라미터는
// ComponentMask).
enum class MaterialStaticParameterKind : uint8 {
    Bool = 0,
    ComponentMask = 1
};

struct MaterialStaticParameterValue {
    // 파라미터 노드의 parameterId다.
    Guid parameterId;
    MaterialStaticParameterKind kind = MaterialStaticParameterKind::Bool;
    // Bool 값이다(ComponentMask면 쓰지 않는다).
    bool value = false;
    // ComponentMask의 R·G·B·A 선택이다(Bool이면 쓰지 않는다).
    bool mask[4] = {false, false, false, false};
};

} // namespace rocks::material
