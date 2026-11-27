#include <MaterialCompiler/MaterialIrOp.h>

// 머티리얼 IR 연산 표다(Docs/Design/MaterialSystem 6.8절).
namespace rocks::material {

namespace {

struct IrOpInfo {
    StringView name;
    uint8 arity;
};

// IrOp 값 순서와 같다.
constexpr IrOpInfo IrOpInfos[IrOpCount] = {
    {"Constant", 0},
    {"Splat", 1},
    {"Swizzle", 1},
    {"Append", 2},
    {"Convert", 1},
    {"Select", 3},
    {"Branch", 3},
    {"BitwiseNot", 1},
    {"Negate", 1},
    {"Not", 1},
    {"Abs", 1},
    {"ACos", 1},
    {"ACosFast", 1},
    {"ACosh", 1},
    {"ASin", 1},
    {"ASinFast", 1},
    {"ASinh", 1},
    {"ATan", 1},
    {"ATanFast", 1},
    {"ATanh", 1},
    {"Ceil", 1},
    {"Cos", 1},
    {"Cosh", 1},
    {"Exponential", 1},
    {"Exponential2", 1},
    {"Floor", 1},
    {"Frac", 1},
    {"IsFinite", 1},
    {"IsInf", 1},
    {"IsNan", 1},
    {"Length", 1},
    {"Logarithm", 1},
    {"Logarithm10", 1},
    {"Logarithm2", 1},
    {"WorldPositionTile", 1},
    {"Reciprocal", 1},
    {"Round", 1},
    {"Rsqrt", 1},
    {"Saturate", 1},
    {"Sign", 1},
    {"Sin", 1},
    {"Sinh", 1},
    {"Sqrt", 1},
    {"Tan", 1},
    {"Tanh", 1},
    {"Transpose", 1},
    {"Truncate", 1},
    {"Normalize", 1},
    {"Ddx", 1},
    {"Ddy", 1},
    {"Equals", 2},
    {"GreaterThan", 2},
    {"GreaterThanOrEquals", 2},
    {"LessThan", 2},
    {"LessThanOrEquals", 2},
    {"NotEquals", 2},
    {"And", 2},
    {"Or", 2},
    {"Add", 2},
    {"Subtract", 2},
    {"Multiply", 2},
    {"MatrixMultiply", 2},
    {"Divide", 2},
    {"Modulo", 2},
    {"BitwiseAnd", 2},
    {"BitwiseOr", 2},
    {"BitwiseXor", 2},
    {"BitShiftLeft", 2},
    {"BitShiftRight", 2},
    {"ATan2", 2},
    {"ATan2Fast", 2},
    {"Cross", 2},
    {"Distance", 2},
    {"Dot", 2},
    {"Fmod", 2},
    {"Max", 2},
    {"Min", 2},
    {"Pow", 2},
    {"Step", 2},
    {"Clamp", 3},
    {"Lerp", 3},
    {"Smoothstep", 3},
    {"Extern", 0},
    {"Interpolate", 1},
    {"Pending", 0},
    {"Parameter", 0},
    {"CollectionValue", 0},
    {"Texture", 0},
    {"TextureSample", 0},
    {"AddressDemote", 0},
    {"TextureSize", 0},
    {"SurfaceRef", 0},
    {"AttributesRef", 0},
    {"Library", 0},
    {"CustomArguments", 0},
    {"CustomCall", 0},
    {"CustomOutput", 1},
};

static_assert(static_cast<uint32>(IrOp::CustomOutput) + 1 == IrOpCount,
              "IrOpCount must follow the last IrOp value");

} // namespace

uint8 IrOpArity(IrOp op) noexcept {
    const uint32 index = static_cast<uint32>(op);
    return index < IrOpCount ? IrOpInfos[index].arity : 0;
}

StringView IrOpName(IrOp op) noexcept {
    const uint32 index = static_cast<uint32>(op);
    return index < IrOpCount ? IrOpInfos[index].name : StringView("Invalid");
}

} // namespace rocks::material
