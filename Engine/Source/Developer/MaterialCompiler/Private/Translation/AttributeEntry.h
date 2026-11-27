#pragma once

#include <Base/Guid.h>
#include <MaterialCompiler/MaterialIrValue.h>

// 속성 집합 기록의 설정된 칸 하나다(Docs/Design/MaterialSystem 6.12절 MS2e1 — 속성 Guid → IR 값).
// 설정되지 않은 칸은 기록에 없고 쓰는 곳이 기본값을 넣는다(UE INDEX_NONE).
namespace rocks::material::detail {

struct AttributeEntry {
    Guid id;
    IrValueId value = NoIrValue;
};

} // namespace rocks::material::detail
