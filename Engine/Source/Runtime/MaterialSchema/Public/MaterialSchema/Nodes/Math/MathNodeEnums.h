#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// Math 범주 노드 속성의 열거다(Docs/Design/MaterialSystem 19.13.2절). 값 번호와 뜻은
// UE 5.8 열거와 같고 이름은 UE 접두를 뗀 나머지다. UE의 숨은 값과 _MAX 표지는 두지 않는다. 저장
// 형식이라 바꾸지 않는다.
namespace rocks::material {

// ClampMode 값이다(UE EClampMode — RE/Public/Materials/MaterialExpressionClamp.h).
ROCKS_ENUM()
enum class ClampMode : uint8 {
    Clamp = 0,
    ClampMin = 1,
    ClampMax = 2
};

// ConvertValueType 값이다(UE EMaterialExpressionConvertType —
// RE/Public/Materials/MaterialExpressionConvert.h).
ROCKS_ENUM()
enum class ConvertValueType : uint8 {
    Scalar = 0,
    Vector2 = 1,
    Vector3 = 2,
    Vector4 = 3
};

// FloatToIntMode 값이다(UE EFloatToIntMode — RE/Public/Materials/MaterialExpressionFloatToUInt.h).
ROCKS_ENUM()
enum class FloatToIntMode : uint8 {
    Truncate = 0,
    Floor = 1,
    Round = 2,
    Ceil = 3
};

// OperatorKind 값이다(UE EMaterialExpressionOperatorKind —
// RE/Public/Materials/MaterialExpressionOperator.h).
ROCKS_ENUM()
enum class OperatorKind : uint8 {
    BitwiseNot = 0,
    Negate = 1,
    Not = 2,
    Abs = 3,
    ACos = 4,
    ACosFast = 5,
    ACosh = 6,
    ASin = 7,
    ASinFast = 8,
    ASinh = 9,
    ATan = 10,
    ATanFast = 11,
    ATanh = 12,
    Ceil = 13,
    Cos = 14,
    Cosh = 15,
    Exponential = 16,
    Exponential2 = 17,
    Floor = 18,
    Frac = 19,
    IsFinite = 20,
    IsInf = 21,
    IsNan = 22,
    Length = 23,
    Logarithm = 24,
    Logarithm10 = 25,
    Logarithm2 = 26,
    LWCTile = 27,
    Reciprocal = 28,
    Round = 29,
    Rsqrt = 30,
    Saturate = 31,
    Sign = 32,
    Sin = 33,
    Sinh = 34,
    Sqrt = 35,
    Tan = 36,
    Tanh = 37,
    Transpose = 38,
    Truncate = 39,
    Equals = 40,
    GreaterThan = 41,
    GreaterThanOrEquals = 42,
    LessThan = 43,
    LessThanOrEquals = 44,
    NotEquals = 45,
    And = 46,
    Or = 47,
    Add = 48,
    Subtract = 49,
    Multiply = 50,
    MatrixMultiply = 51,
    Divide = 52,
    Modulo = 53,
    BitwiseAnd = 54,
    BitwiseOr = 55,
    BitwiseXor = 56,
    BitShiftLeft = 57,
    BitShiftRight = 58,
    ATan2 = 59,
    ATan2Fast = 60,
    Cross = 61,
    Distance = 62,
    Dot = 63,
    Fmod = 64,
    Max = 65,
    Min = 66,
    Pow = 67,
    Step = 68,
    Clamp = 69,
    Lerp = 70,
    Select = 71,
    Smoothstep = 72
};

} // namespace rocks::material
