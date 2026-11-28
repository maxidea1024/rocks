#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// 상수 블록 칸의 내용이다(Docs/Design/MaterialSystem 9.4절, 12.8절 MS3d2 — Double 출력은
// tile·offset 두 칸, 5.3절 LWC 표현). 컴파일러 상수 layout(MaterialConstantMember)과 쿠킹 artifact
// 상수 칸(MaterialConstantSlot)이 같은 값을 쓴다.
namespace rocks::material {

ROCKS_ENUM()
enum class MaterialConstantSlotKind : uint8 {
    Value = 0,
    DoubleTile = 1,
    DoubleOffset = 2
};

} // namespace rocks::material
