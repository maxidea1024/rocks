#pragma once

#include <Base/Types.h>
#include <MaterialCompiler/MaterialIrValue.h>

// 머티리얼 상수 블록 멤버 하나다(Docs/Design/MaterialSystem 6.10절 MS2c2, 9.4절 —
// MaterialConstants가 담는다).
//
// 사용 예:
//   for (const MaterialConstantMember& member : constants.members) {
//       Write(member.vector, member.firstComponent, member.components);
//   }
namespace rocks::material {

// 상수 블록 칸의 내용이다(Double 출력은 tile·offset 두 칸 — 5.3절 LWC 표현).
enum class MaterialConstantSlotKind : uint8 {
    Value = 0,
    DoubleTile = 1,
    DoubleOffset = 2
};

// 상수 블록 멤버 하나(출력 하나의 칸)다.
struct MaterialConstantMember {
    // 모듈 값 번호다.
    IrValueId value = NoIrValue;
    // 그 값을 계산하는 프로그램 명령 번호다.
    uint32 step = 0;
    MaterialConstantSlotKind kind = MaterialConstantSlotKind::Value;
    // float4 칸 번호와 그 안의 첫 성분·성분 수다(float4 경계를 넘지 않는다 — 7.4절 5항).
    uint32 vector = 0;
    uint8 firstComponent = 0;
    uint8 components = 0;
};

} // namespace rocks::material
