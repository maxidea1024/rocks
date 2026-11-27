#pragma once

#include <Base/Types.h>
#include <Core/Text/StringView.h>

// 생성 원문 정의 하나다(Docs/Design/MaterialSystem 6.11절 MS2d3 — 노드 속성이 정하는 정의, 예:
// MATERIAL_VOLUMETRIC_ADVANCED). name은 정적 원문이고 value는 정수 값이다. IR hash에 들어간다.
namespace rocks::material {

struct IrDefine {
    StringView name;
    uint32 value = 0;
};

} // namespace rocks::material
