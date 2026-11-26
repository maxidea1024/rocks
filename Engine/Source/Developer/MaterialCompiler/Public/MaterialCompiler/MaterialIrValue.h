#pragma once

#include <Base/NumericLimits.h>
#include <Base/Types.h>
#include <MaterialCompiler/MaterialIrOp.h>
#include <MaterialCompiler/MaterialIrSite.h>
#include <MaterialCompiler/MaterialIrType.h>

// 머티리얼 IR 값 하나다(Docs/Design/MaterialSystem 6.2·6.8절).
namespace rocks::material {

// 값 번호다. 빌더 안에서는 만든 순서, 모듈 안에서는 진입점에서 다시 매긴 순서다(6.6절).
using IrValueId = uint32;

// "값 없음" 표지다.
inline constexpr IrValueId NoIrValue = NumericLimits<uint32>::Max();

// 값 하나가 받는 피연산자의 최대 수다(삼항 연산·선택·분기).
inline constexpr uint32 MaxIrOperands = 3;

// 즉시값 칸 수다(상수 성분 넷 — 행렬 상수는 두지 않는다).
inline constexpr uint32 IrImmediateSlots = 4;

// 문맥 값의 변형이다(Extern 즉시값 [1] — 6.9절, 19.3.5절).
enum class IrExternVariant : uint8 {
    Current = 0,
    // 이전 프레임 값이다(이전 정점 문맥에서 표의 hasPrevious가 참인 값).
    Previous = 1
};

// 값이 바뀌는 빈도다(6.2절 격자 — 뒤로 갈수록 자주 바뀐다).
enum class IrFrequency : uint8 {
    Constant = 0,
    Static = 1,
    Uniform = 2,
    Instance = 3,
    Vertex = 4,
    Pixel = 5
};

struct IrValue {
    IrType type;
    IrOp op = IrOp::Constant;
    uint8 operandCount = 0;
    IrFrequency frequency = IrFrequency::Constant;
    IrValueId operands[MaxIrOperands] = {NoIrValue, NoIrValue, NoIrValue};
    // 상수는 성분별 비트 패턴(float·int·uint 32 bit, double 64 bit, bool 0·1), Swizzle은 [0]에
    // 성분 번호(2 bit씩, 성분 0이 하위)다. 그 밖의 연산은 0이다.
    uint64 immediate[IrImmediateSlots] = {0, 0, 0, 0};
    IrSite site;
};

} // namespace rocks::material
