#pragma once

#include <Base/Types.h>
#include <MaterialSchema/MaterialConstantSlotKind.h>
#include <Meta/Annotations.h>

// 머티리얼 상수 블록 칸 하나다(Docs/Design/MaterialSystem 9.4절, 12.8절 MS3d2 — 상수 식 출력 하나가
// float4 vector의 [firstComponent, firstComponent + components) 성분에 간다).
namespace rocks::material {

ROCKS_STRUCT()
struct MaterialConstantSlot {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() MaterialConstantSlotKind kind = MaterialConstantSlotKind::Value;
    // float4 번호다(materialFloats[vector]).
    ROCKS_PROPERTY() uint32 vector = 0;
    ROCKS_PROPERTY() uint8 firstComponent = 0;
    ROCKS_PROPERTY() uint8 components = 0;
    // 이 칸의 값을 내는 프로그램 명령 번호다.
    ROCKS_PROPERTY() uint32 instruction = 0;
};

} // namespace rocks::material
