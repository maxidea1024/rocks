#include <Core/Hash.h>
#include <Core/Move.h>
#include <DeterministicMath/DeterministicMath.h>
#include <MaterialCompiler/MaterialIrBuilder.h>
#include <Platform/Memory.h>

// 머티리얼 IR 빌더다(Docs/Design/MaterialSystem 6.5·6.8절).
namespace rocks::material {

namespace {

// Swizzle 즉시값의 성분 하나가 차지하는 bit 수다.
constexpr uint32 SwizzleBitsPerComponent = 2;
// Swizzle 즉시값의 성분 번호 마스크다.
constexpr uint64 SwizzleComponentMask = (1u << SwizzleBitsPerComponent) - 1;

float FloatOf(uint64 bits) noexcept {
    const uint32 narrow = static_cast<uint32>(bits);
    float value = 0.0f;
    platform::MemoryCopy(&value, &narrow, sizeof(value));
    return value;
}

uint64 BitsOf(float value) noexcept {
    uint32 narrow = 0;
    platform::MemoryCopy(&narrow, &value, sizeof(value));
    return narrow;
}

uint64 BitsOf(double value) noexcept {
    uint64 bits = 0;
    platform::MemoryCopy(&bits, &value, sizeof(value));
    return bits;
}

int32 IntOf(uint64 bits) noexcept {
    return static_cast<int32>(static_cast<uint32>(bits));
}

uint64 BitsOf(int32 value) noexcept {
    return static_cast<uint32>(value);
}

// NaN·Inf가 아닌가(지수 bit가 모두 1이 아니다).
bool IsFiniteBits(uint64 bits) noexcept {
    constexpr uint32 ExponentMask = 0x7F800000u;
    return (static_cast<uint32>(bits) & ExponentMask) != ExponentMask;
}

bool IsNumeric(IrScalar scalar) noexcept {
    return scalar != IrScalar::Bool;
}

bool IsInteger(IrScalar scalar) noexcept {
    return scalar == IrScalar::Int || scalar == IrScalar::UInt;
}

bool IsFloating(IrScalar scalar) noexcept {
    return scalar == IrScalar::Float || scalar == IrScalar::Double;
}

// 결과 타입이다. 규칙을 어기면 false다(6.8절 타입 규칙).
bool UnaryType(IrOp op, IrType operand, IrType& out) noexcept {
    out = operand;
    if (operand.IsMatrix()) {
        if (op == IrOp::Transpose) {
            out = IrFloatMatrix(operand.components, operand.rows);
            return true;
        }

        return op == IrOp::Negate;
    }

    switch (op) {
    case IrOp::BitwiseNot:
        return IsInteger(operand.scalar);
    case IrOp::Negate:
    case IrOp::Abs:
    case IrOp::Sign:
        return IsNumeric(operand.scalar) && operand.scalar != IrScalar::UInt;
    case IrOp::Not:
        return operand.scalar == IrScalar::Bool;
    case IrOp::IsFinite:
    case IrOp::IsInf:
    case IrOp::IsNan:
        out = IrBool(operand.components);
        return operand.scalar == IrScalar::Float;
    case IrOp::Length:
        out = IrFloat(1);
        return operand.scalar == IrScalar::Float;
    case IrOp::WorldPositionTile:
        out = IrFloat(operand.components);
        return operand.scalar == IrScalar::Double;
    case IrOp::Floor:
    case IrOp::Ceil:
    case IrOp::Round:
    case IrOp::Truncate:
    case IrOp::Frac:
    case IrOp::Saturate:
        return IsFloating(operand.scalar);
    case IrOp::Transpose:
        return false;
    default:
        // 초월·미분·Normalize·Rsqrt·Reciprocal은 float만 받는다.
        return IrOpArity(op) == 1 && op != IrOp::Splat && op != IrOp::Swizzle &&
               op != IrOp::Convert && operand.scalar == IrScalar::Float;
    }
}

bool BinaryType(IrOp op, IrType left, IrType right, IrType& out) noexcept {
    out = left;
    if (op == IrOp::MatrixMultiply) {
        if (left.scalar != IrScalar::Float || right.scalar != IrScalar::Float ||
            !right.IsMatrix()) {
            return false;
        }

        if (!left.IsMatrix()) {
            // 행 벡터 × 행렬(mul(v, M) — 7.4절 6항).
            out = IrFloat(right.components);
            return left.components == right.rows;
        }

        out = IrFloatMatrix(left.rows, right.components);
        return left.components == right.rows;
    }

    if (left != right || left.IsMatrix()) {
        return false;
    }

    switch (op) {
    case IrOp::Equals:
    case IrOp::NotEquals:
        out = IrBool(left.components);
        return true;
    case IrOp::GreaterThan:
    case IrOp::GreaterThanOrEquals:
    case IrOp::LessThan:
    case IrOp::LessThanOrEquals:
        out = IrBool(left.components);
        return IsNumeric(left.scalar);
    case IrOp::And:
    case IrOp::Or:
        return left.scalar == IrScalar::Bool;
    case IrOp::Add:
    case IrOp::Subtract:
    case IrOp::Multiply:
    case IrOp::Divide:
    case IrOp::Max:
    case IrOp::Min:
        return IsNumeric(left.scalar);
    case IrOp::Modulo:
        return IsNumeric(left.scalar) && left.scalar != IrScalar::Double;
    case IrOp::BitwiseAnd:
    case IrOp::BitwiseOr:
    case IrOp::BitwiseXor:
    case IrOp::BitShiftLeft:
    case IrOp::BitShiftRight:
        return IsInteger(left.scalar);
    case IrOp::Dot:
    case IrOp::Distance:
        out = IrFloat(1);
        return left.scalar == IrScalar::Float;
    case IrOp::Cross:
        return left == IrFloat(3);
    case IrOp::ATan2:
    case IrOp::ATan2Fast:
    case IrOp::Fmod:
    case IrOp::Pow:
    case IrOp::Step:
        return left.scalar == IrScalar::Float;
    default:
        return false;
    }
}

bool TernaryType(IrOp op, IrType first, IrType second, IrType third, IrType& out) noexcept {
    out = first;
    if (first != second || second != third || first.IsMatrix()) {
        return false;
    }

    switch (op) {
    case IrOp::Clamp:
        return IsNumeric(first.scalar) && first.scalar != IrScalar::Double;
    case IrOp::Lerp:
    case IrOp::Smoothstep:
        return first.scalar == IrScalar::Float;
    default:
        return false;
    }
}

// 접는 단항 연산 하나(float 성분)다. 접지 않으면 false다.
bool FoldFloatUnary(IrOp op, float x, float& out) noexcept {
    switch (op) {
    case IrOp::Negate:
        out = -x;
        return true;
    case IrOp::Abs:
        out = x < 0.0f ? -x : (x == 0.0f ? 0.0f : x);
        return true;
    case IrOp::Floor:
        out = static_cast<float>(deterministic::Floor(x));
        return true;
    case IrOp::Ceil:
        out = static_cast<float>(-deterministic::Floor(-static_cast<double>(x)));
        return true;
    case IrOp::Round:
        out = static_cast<float>(deterministic::RoundHalfEven(x));
        return true;
    case IrOp::Truncate:
        out = static_cast<float>(deterministic::Trunc(x));
        return true;
    case IrOp::Frac:
        out = x - static_cast<float>(deterministic::Floor(x));
        return true;
    case IrOp::Saturate:
        out = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
        return true;
    case IrOp::Sign:
        out = x > 0.0f ? 1.0f : (x < 0.0f ? -1.0f : 0.0f);
        return true;
    default:
        return false;
    }
}

bool FoldFloatBinary(IrOp op, float a, float b, uint64& out, bool& boolean) noexcept {
    boolean = false;
    switch (op) {
    case IrOp::Add:
        out = BitsOf(a + b);
        return true;
    case IrOp::Subtract:
        out = BitsOf(a - b);
        return true;
    case IrOp::Multiply:
        out = BitsOf(a * b);
        return true;
    case IrOp::Divide:
        out = BitsOf(a / b);
        return b != 0.0f;
    case IrOp::Max:
        out = BitsOf(a > b ? a : b);
        return true;
    case IrOp::Min:
        out = BitsOf(a < b ? a : b);
        return true;
    case IrOp::Step:
        // HLSL step(a, b) = b >= a ? 1 : 0.
        out = BitsOf(b >= a ? 1.0f : 0.0f);
        return true;
    case IrOp::Equals:
        boolean = true;
        out = a == b ? 1 : 0;
        return true;
    case IrOp::NotEquals:
        boolean = true;
        out = a != b ? 1 : 0;
        return true;
    case IrOp::GreaterThan:
        boolean = true;
        out = a > b ? 1 : 0;
        return true;
    case IrOp::GreaterThanOrEquals:
        boolean = true;
        out = a >= b ? 1 : 0;
        return true;
    case IrOp::LessThan:
        boolean = true;
        out = a < b ? 1 : 0;
        return true;
    case IrOp::LessThanOrEquals:
        boolean = true;
        out = a <= b ? 1 : 0;
        return true;
    default:
        return false;
    }
}

// 정수 이항 하나다(int·uint — HLSL처럼 2의 보수 wrap, shift는 하위 5 bit). 0 나누기·최소값 / −1은
// 접지 않는다.
bool FoldIntegerBinary(IrOp op, bool isSigned, uint32 a, uint32 b, uint64& out,
                       bool& boolean) noexcept {
    constexpr uint32 ShiftMask = 31;
    constexpr uint32 SignedMinimum = 0x80000000u;
    boolean = false;
    const int32 sa = static_cast<int32>(a);
    const int32 sb = static_cast<int32>(b);
    switch (op) {
    case IrOp::Add:
        out = static_cast<uint32>(a + b);
        return true;
    case IrOp::Subtract:
        out = static_cast<uint32>(a - b);
        return true;
    case IrOp::Multiply:
        out = static_cast<uint32>(a * b);
        return true;
    case IrOp::Divide:
    case IrOp::Modulo:
        if (b == 0 || (isSigned && a == SignedMinimum && sb == -1)) {
            return false;
        }

        if (isSigned) {
            out = static_cast<uint32>(op == IrOp::Divide ? sa / sb : sa % sb);
        } else {
            out = op == IrOp::Divide ? a / b : a % b;
        }

        return true;
    case IrOp::Max:
        out = isSigned ? static_cast<uint32>(sa > sb ? sa : sb) : (a > b ? a : b);
        return true;
    case IrOp::Min:
        out = isSigned ? static_cast<uint32>(sa < sb ? sa : sb) : (a < b ? a : b);
        return true;
    case IrOp::BitwiseAnd:
        out = a & b;
        return true;
    case IrOp::BitwiseOr:
        out = a | b;
        return true;
    case IrOp::BitwiseXor:
        out = a ^ b;
        return true;
    case IrOp::BitShiftLeft:
        out = static_cast<uint32>(a << (b & ShiftMask));
        return true;
    case IrOp::BitShiftRight:
        out = isSigned ? static_cast<uint32>(sa >> (b & ShiftMask)) : (a >> (b & ShiftMask));
        return true;
    case IrOp::Equals:
        boolean = true;
        out = a == b ? 1 : 0;
        return true;
    case IrOp::NotEquals:
        boolean = true;
        out = a != b ? 1 : 0;
        return true;
    case IrOp::GreaterThan:
        boolean = true;
        out = (isSigned ? sa > sb : a > b) ? 1 : 0;
        return true;
    case IrOp::GreaterThanOrEquals:
        boolean = true;
        out = (isSigned ? sa >= sb : a >= b) ? 1 : 0;
        return true;
    case IrOp::LessThan:
        boolean = true;
        out = (isSigned ? sa < sb : a < b) ? 1 : 0;
        return true;
    case IrOp::LessThanOrEquals:
        boolean = true;
        out = (isSigned ? sa <= sb : a <= b) ? 1 : 0;
        return true;
    default:
        return false;
    }
}

} // namespace

bool IrBuilder::KeyEqual::operator()(const Key& left, const Key& right) const noexcept {
    const IrValue& a = left.value;
    const IrValue& b = right.value;
    if (a.type != b.type || a.op != b.op || a.operandCount != b.operandCount ||
        a.frequency != b.frequency) {
        return false;
    }

    for (uint32 index = 0; index < MaxIrOperands; ++index) {
        if (a.operands[index] != b.operands[index]) {
            return false;
        }
    }

    for (uint32 index = 0; index < IrImmediateSlots; ++index) {
        if (a.immediate[index] != b.immediate[index]) {
            return false;
        }
    }

    return true;
}

uint64 IrBuilder::KeyHash::operator()(const Key& key) const noexcept {
    const IrValue& value = key.value;
    uint64 hash = MixHash(
        static_cast<uint64>(value.type.scalar) | (uint64{value.type.components} << 8) |
        (uint64{value.type.rows} << 16) | (uint64{static_cast<uint8>(value.op)} << 24) |
        (uint64{value.operandCount} << 32) | (uint64{static_cast<uint8>(value.frequency)} << 40));
    for (uint32 index = 0; index < MaxIrOperands; ++index) {
        hash = HashCombine(hash, value.operands[index]);
    }

    for (uint32 index = 0; index < IrImmediateSlots; ++index) {
        hash = HashCombine(hash, value.immediate[index]);
    }

    return hash;
}

IrBuilder::IrBuilder(Allocator& allocator) noexcept : values_(allocator), lookup_(allocator) {
}

const IrValue& IrBuilder::Value(IrValueId id) const noexcept {
    return values_[id];
}

bool IrBuilder::IsValid(IrValueId id) const noexcept {
    return id < values_.Size();
}

bool IrBuilder::IsConstant(IrValueId id) const noexcept {
    return IsValid(id) && values_[id].op == IrOp::Constant;
}

bool IrBuilder::IsConstantEqualTo(IrValueId id, int32 value) const noexcept {
    if (!IsConstant(id)) {
        return false;
    }

    const IrValue& constant = values_[id];
    for (uint8 index = 0; index < constant.type.components; ++index) {
        const uint64 bits = constant.immediate[index];
        switch (constant.type.scalar) {
        case IrScalar::Float:
            // 비트로 비교한다: −0은 0이 아니다(x − (−0)은 x가 아니다).
            if (bits != BitsOf(static_cast<float>(value))) {
                return false;
            }

            break;
        case IrScalar::Double:
            if (bits != BitsOf(static_cast<double>(value))) {
                return false;
            }

            break;
        case IrScalar::Int:
        case IrScalar::UInt:
            if (static_cast<uint32>(bits) != static_cast<uint32>(value)) {
                return false;
            }

            break;
        case IrScalar::Bool:
            if (bits != static_cast<uint64>(value != 0)) {
                return false;
            }

            break;
        }
    }

    return true;
}

Result<IrValueId> IrBuilder::Intern(IrValue value) noexcept {
    // 빈도는 피연산자 빈도의 최대다(6.2절). 상수는 Constant다. 미분은 Pixel이다. 문맥 값은 표가
    // 정한 빈도에서 시작한다(6.9절).
    IrFrequency frequency = value.op == IrOp::Extern ? value.frequency : IrFrequency::Constant;
    for (uint8 index = 0; index < value.operandCount; ++index) {
        const IrFrequency operand = values_[value.operands[index]].frequency;
        frequency =
            static_cast<uint8>(operand) > static_cast<uint8>(frequency) ? operand : frequency;
    }

    if (value.op == IrOp::Ddx || value.op == IrOp::Ddy || value.op == IrOp::Interpolate) {
        frequency = IrFrequency::Pixel;
    }

    value.frequency = value.op == IrOp::Constant ? value.frequency : frequency;
    Key key{value};
    if (const IrValueId* found = lookup_.Find(key)) {
        // 같은 값을 다른 노드가 다시 만들면 원인 위치는 NodeId가 가장 작은 쪽이다. 번역 순서(간선
        // 저장 순서)와 상관없이 원문 위치 지도가 같다(7.7절). site는 hash-consing 키·IR hash에 들지
        // 않는다.
        IrSite& site = values_[*found].site;
        const bool smaller = site_.node.high < site.node.high ||
                             (site_.node.high == site.node.high && site_.node.low < site.node.low);
        if (!(site_.node == Guid{}) && (site.node == Guid{} || smaller)) {
            site = site_;
        }

        return Result<IrValueId>::Success(*found);
    }

    value.site = site_;
    const IrValueId id = static_cast<IrValueId>(values_.Size());
    if (auto pushed = values_.TryPushBack(value); !pushed) {
        return Result<IrValueId>::Failure(pushed.Error());
    }

    if (auto inserted = lookup_.Insert(key, id); !inserted) {
        (void)values_.TryPopBack();
        return Result<IrValueId>::Failure(inserted.Error());
    }

    return Result<IrValueId>::Success(id);
}

namespace {

IrValue ConstantValue(IrType type) noexcept {
    IrValue value;
    value.type = type;
    value.op = IrOp::Constant;
    value.frequency = IrFrequency::Constant;
    return value;
}

} // namespace

Result<IrValueId> IrBuilder::FloatConstant(Span<const float> components) noexcept {
    if (components.Size() == 0 || components.Size() > MaxIrComponents) {
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    IrValue value = ConstantValue(IrFloat(static_cast<uint8>(components.Size())));
    for (usize index = 0; index < components.Size(); ++index) {
        value.immediate[index] = BitsOf(components[index]);
    }

    return Intern(value);
}

Result<IrValueId> IrBuilder::IntConstant(Span<const int32> components) noexcept {
    if (components.Size() == 0 || components.Size() > MaxIrComponents) {
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    IrValue value = ConstantValue(IrInt(static_cast<uint8>(components.Size())));
    for (usize index = 0; index < components.Size(); ++index) {
        value.immediate[index] = BitsOf(components[index]);
    }

    return Intern(value);
}

Result<IrValueId> IrBuilder::UIntConstant(Span<const uint32> components) noexcept {
    if (components.Size() == 0 || components.Size() > MaxIrComponents) {
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    IrValue value = ConstantValue(IrUInt(static_cast<uint8>(components.Size())));
    for (usize index = 0; index < components.Size(); ++index) {
        value.immediate[index] = components[index];
    }

    return Intern(value);
}

Result<IrValueId> IrBuilder::BoolConstant(Span<const bool> components) noexcept {
    if (components.Size() == 0 || components.Size() > MaxIrComponents) {
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    IrValue value = ConstantValue(IrBool(static_cast<uint8>(components.Size())));
    for (usize index = 0; index < components.Size(); ++index) {
        value.immediate[index] = components[index] ? 1 : 0;
    }

    return Intern(value);
}

Result<IrValueId> IrBuilder::DoubleConstant(Span<const double> components) noexcept {
    if (components.Size() == 0 || components.Size() > MaxIrComponents) {
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    IrValue value = ConstantValue(IrDouble(static_cast<uint8>(components.Size())));
    for (usize index = 0; index < components.Size(); ++index) {
        value.immediate[index] = BitsOf(components[index]);
    }

    return Intern(value);
}

Result<IrValueId> IrBuilder::Fold(const IrValue& value) noexcept {
    // 접을 수 있으면 상수를, 아니면 NoIrValue를 돌려준다(오류는 할당 실패뿐).
    const auto none = []() noexcept { return Result<IrValueId>::Success(NoIrValue); };
    for (uint8 index = 0; index < value.operandCount; ++index) {
        if (!IsConstant(value.operands[index])) {
            return none();
        }
    }

    const IrValue* operands[MaxIrOperands] = {};
    for (uint8 index = 0; index < value.operandCount; ++index) {
        operands[index] = &values_[value.operands[index]];
    }

    IrValue folded = ConstantValue(value.type);
    const uint8 components = value.type.components;
    const IrScalar scalar = value.operandCount > 0 ? operands[0]->type.scalar : value.type.scalar;
    if (value.type.IsMatrix() || scalar == IrScalar::Double ||
        (value.operandCount > 0 && operands[0]->type.IsMatrix())) {
        return none();
    }

    switch (value.op) {
    case IrOp::Splat:
        for (uint8 index = 0; index < components; ++index) {
            folded.immediate[index] = operands[0]->immediate[0];
        }

        return Intern(folded);
    case IrOp::Swizzle:
        for (uint8 index = 0; index < components; ++index) {
            const uint64 source =
                (value.immediate[0] >> (index * SwizzleBitsPerComponent)) & SwizzleComponentMask;
            folded.immediate[index] = operands[0]->immediate[source];
        }

        return Intern(folded);
    case IrOp::Append: {
        uint8 at = 0;
        for (uint8 side = 0; side < 2; ++side) {
            for (uint8 index = 0; index < operands[side]->type.components; ++index) {
                folded.immediate[at++] = operands[side]->immediate[index];
            }
        }

        return Intern(folded);
    }
    case IrOp::Select:
    case IrOp::Branch:
        for (uint8 index = 0; index < components; ++index) {
            const uint8 conditionIndex = operands[0]->type.components == 1 ? 0 : index;
            folded.immediate[index] = operands[0]->immediate[conditionIndex] != 0
                                          ? operands[1]->immediate[index]
                                          : operands[2]->immediate[index];
        }

        return Intern(folded);
    case IrOp::Convert:
        for (uint8 index = 0; index < components; ++index) {
            const uint64 bits = operands[0]->immediate[index];
            switch (value.type.scalar) {
            case IrScalar::Float:
                if (scalar == IrScalar::Int) {
                    folded.immediate[index] = BitsOf(static_cast<float>(IntOf(bits)));
                } else if (scalar == IrScalar::UInt) {
                    folded.immediate[index] = BitsOf(static_cast<float>(static_cast<uint32>(bits)));
                } else {
                    folded.immediate[index] = BitsOf(bits != 0 ? 1.0f : 0.0f);
                }

                break;
            case IrScalar::Int:
            case IrScalar::UInt: {
                if (scalar == IrScalar::Float) {
                    // 0 쪽으로 자른다(HLSL). 범위 밖·음수의 uint 변환은 정의되지 않으므로 접지
                    // 않는다.
                    const double truncated = deterministic::Trunc(FloatOf(bits));
                    constexpr double IntLimit = 2147483648.0;
                    constexpr double UIntLimit = 4294967296.0;
                    if (value.type.scalar == IrScalar::Int
                            ? (truncated < -IntLimit || truncated >= IntLimit)
                            : (truncated < 0.0 || truncated >= UIntLimit)) {
                        return none();
                    }

                    folded.immediate[index] =
                        value.type.scalar == IrScalar::Int
                            ? BitsOf(static_cast<int32>(truncated))
                            : static_cast<uint64>(static_cast<uint32>(truncated));
                } else if (scalar == IrScalar::Bool) {
                    folded.immediate[index] = bits != 0 ? 1 : 0;
                } else {
                    // int ↔ uint는 비트 그대로(HLSL).
                    folded.immediate[index] = static_cast<uint32>(bits);
                }

                break;
            }
            case IrScalar::Bool:
                folded.immediate[index] = scalar == IrScalar::Float
                                              ? (FloatOf(bits) != 0.0f ? 1 : 0)
                                              : (bits != 0 ? 1 : 0);
                break;
            case IrScalar::Double:
                return none();
            }
        }

        return Intern(folded);
    default:
        break;
    }

    const uint8 arity = IrOpArity(value.op);
    for (uint8 index = 0; index < components; ++index) {
        uint64 result = 0;
        if (arity == 1) {
            const uint64 bits = operands[0]->immediate[index];
            if (scalar == IrScalar::Float) {
                float out = 0.0f;
                if (!FoldFloatUnary(value.op, FloatOf(bits), out)) {
                    return none();
                }

                result = BitsOf(out);
            } else if (scalar == IrScalar::Bool && value.op == IrOp::Not) {
                result = bits != 0 ? 0 : 1;
            } else if (IsInteger(scalar)) {
                const uint32 x = static_cast<uint32>(bits);
                if (value.op == IrOp::BitwiseNot) {
                    result = static_cast<uint32>(~x);
                } else if (value.op == IrOp::Negate) {
                    result = static_cast<uint32>(0u - x);
                } else if (value.op == IrOp::Abs && scalar == IrScalar::Int) {
                    const int32 signedValue = static_cast<int32>(x);
                    result = static_cast<uint32>(signedValue < 0 ? 0u - x : x);
                } else {
                    return none();
                }
            } else {
                return none();
            }
        } else if (arity == 2) {
            const uint64 a = operands[0]->immediate[index];
            const uint64 b = operands[1]->immediate[index];
            bool boolean = false;
            if (scalar == IrScalar::Float) {
                if (!FoldFloatBinary(value.op, FloatOf(a), FloatOf(b), result, boolean)) {
                    return none();
                }
            } else if (IsInteger(scalar)) {
                if (!FoldIntegerBinary(value.op, scalar == IrScalar::Int, static_cast<uint32>(a),
                                       static_cast<uint32>(b), result, boolean)) {
                    return none();
                }
            } else if (scalar == IrScalar::Bool) {
                if (value.op == IrOp::And) {
                    result = (a != 0 && b != 0) ? 1 : 0;
                } else if (value.op == IrOp::Or) {
                    result = (a != 0 || b != 0) ? 1 : 0;
                } else if (value.op == IrOp::Equals) {
                    result = (a != 0) == (b != 0) ? 1 : 0;
                } else if (value.op == IrOp::NotEquals) {
                    result = (a != 0) != (b != 0) ? 1 : 0;
                } else {
                    return none();
                }
            } else {
                return none();
            }
        } else if (arity == 3 && value.op == IrOp::Clamp) {
            // clamp(x, a, b) = min(max(x, a), b)(HLSL 정의).
            const uint64 x = operands[0]->immediate[index];
            const uint64 low = operands[1]->immediate[index];
            const uint64 high = operands[2]->immediate[index];
            uint64 raised = 0;
            bool boolean = false;
            if (scalar == IrScalar::Float) {
                if (!FoldFloatBinary(IrOp::Max, FloatOf(x), FloatOf(low), raised, boolean) ||
                    !FoldFloatBinary(IrOp::Min, FloatOf(raised), FloatOf(high), result, boolean)) {
                    return none();
                }
            } else {
                const bool isSigned = scalar == IrScalar::Int;
                if (!FoldIntegerBinary(IrOp::Max, isSigned, static_cast<uint32>(x),
                                       static_cast<uint32>(low), raised, boolean) ||
                    !FoldIntegerBinary(IrOp::Min, isSigned, static_cast<uint32>(raised),
                                       static_cast<uint32>(high), result, boolean)) {
                    return none();
                }
            }
        } else {
            return none();
        }

        // 접은 float 결과가 NaN·Inf면 접지 않는다(GPU와 같은 값을 보장하지 못한다 — 6.8절).
        if (value.type.scalar == IrScalar::Float && !IsFiniteBits(result)) {
            return none();
        }

        folded.immediate[index] = result;
    }

    return Intern(folded);
}

Result<IrValueId> IrBuilder::Unary(IrOp op, IrValueId operand) noexcept {
    if (!IsValid(operand) || IrOpArity(op) != 1 || op == IrOp::Splat || op == IrOp::Swizzle ||
        op == IrOp::Convert) {
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    IrValue value;
    if (!UnaryType(op, values_[operand].type, value.type)) {
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    value.op = op;
    value.operandCount = 1;
    value.operands[0] = operand;
    auto folded = Fold(value);
    if (!folded || folded.Value() != NoIrValue) {
        return Move(folded);
    }

    return Intern(value);
}

Result<IrValueId> IrBuilder::Binary(IrOp op, IrValueId left, IrValueId right) noexcept {
    if (!IsValid(left) || !IsValid(right) || IrOpArity(op) != 2 || op == IrOp::Append) {
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    IrValue value;
    if (!BinaryType(op, values_[left].type, values_[right].type, value.type)) {
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    // 대수 단순화(6.5·6.8절): x*1, 1*x, x/1, x-0, 정수 x+0·0+x. float x+0·x*0은 하지 않는다.
    const bool integer = IsInteger(values_[left].type.scalar);
    if ((op == IrOp::Multiply || op == IrOp::Divide) && IsConstantEqualTo(right, 1)) {
        return Result<IrValueId>::Success(left);
    }

    if (op == IrOp::Multiply && IsConstantEqualTo(left, 1)) {
        return Result<IrValueId>::Success(right);
    }

    if (op == IrOp::Subtract && IsConstantEqualTo(right, 0) && !IsConstant(left)) {
        return Result<IrValueId>::Success(left);
    }

    if (op == IrOp::Add && integer) {
        if (IsConstantEqualTo(right, 0)) {
            return Result<IrValueId>::Success(left);
        }

        if (IsConstantEqualTo(left, 0)) {
            return Result<IrValueId>::Success(right);
        }
    }

    value.op = op;
    value.operandCount = 2;
    value.operands[0] = left;
    value.operands[1] = right;
    auto folded = Fold(value);
    if (!folded || folded.Value() != NoIrValue) {
        return Move(folded);
    }

    return Intern(value);
}

Result<IrValueId> IrBuilder::Ternary(IrOp op, IrValueId first, IrValueId second,
                                     IrValueId third) noexcept {
    if (!IsValid(first) || !IsValid(second) || !IsValid(third) ||
        (op != IrOp::Clamp && op != IrOp::Lerp && op != IrOp::Smoothstep)) {
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    IrValue value;
    if (!TernaryType(op, values_[first].type, values_[second].type, values_[third].type,
                     value.type)) {
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    value.op = op;
    value.operandCount = 3;
    value.operands[0] = first;
    value.operands[1] = second;
    value.operands[2] = third;
    auto folded = Fold(value);
    if (!folded || folded.Value() != NoIrValue) {
        return Move(folded);
    }

    return Intern(value);
}

Result<IrValueId> IrBuilder::Splat(IrValueId operand, uint8 components) noexcept {
    if (!IsValid(operand) || !values_[operand].type.IsScalar() || components == 0 ||
        components > MaxIrComponents) {
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    if (components == 1) {
        return Result<IrValueId>::Success(operand);
    }

    IrValue value;
    value.type = WithComponents(values_[operand].type, components);
    value.op = IrOp::Splat;
    value.operandCount = 1;
    value.operands[0] = operand;
    auto folded = Fold(value);
    if (!folded || folded.Value() != NoIrValue) {
        return Move(folded);
    }

    return Intern(value);
}

Result<IrValueId> IrBuilder::Swizzle(IrValueId operand, Span<const uint8> components) noexcept {
    if (!IsValid(operand) || values_[operand].type.IsMatrix() || components.Size() == 0 ||
        components.Size() > MaxIrComponents) {
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    const IrType source = values_[operand].type;
    uint8 indices[MaxIrComponents] = {};
    bool identity = components.Size() == source.components;
    for (usize index = 0; index < components.Size(); ++index) {
        if (components[index] >= source.components) {
            return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
        }

        indices[index] = components[index];
        identity = identity && components[index] == index;
    }

    if (identity) {
        return Result<IrValueId>::Success(operand);
    }

    // swizzle의 swizzle은 하나로 합친다.
    IrValueId base = operand;
    if (values_[operand].op == IrOp::Swizzle) {
        const IrValue& inner = values_[operand];
        for (usize index = 0; index < components.Size(); ++index) {
            indices[index] = static_cast<uint8>(
                (inner.immediate[0] >> (indices[index] * SwizzleBitsPerComponent)) &
                SwizzleComponentMask);
        }

        base = inner.operands[0];
        const IrType baseType = values_[base].type;
        bool baseIdentity = components.Size() == baseType.components;
        for (usize index = 0; index < components.Size(); ++index) {
            baseIdentity = baseIdentity && indices[index] == index;
        }

        if (baseIdentity) {
            return Result<IrValueId>::Success(base);
        }
    }

    IrValue value;
    value.type = WithComponents(source, static_cast<uint8>(components.Size()));
    value.op = IrOp::Swizzle;
    value.operandCount = 1;
    value.operands[0] = base;
    for (usize index = 0; index < components.Size(); ++index) {
        value.immediate[0] |= uint64{indices[index]} << (index * SwizzleBitsPerComponent);
    }

    auto folded = Fold(value);
    if (!folded || folded.Value() != NoIrValue) {
        return Move(folded);
    }

    return Intern(value);
}

Result<IrValueId> IrBuilder::Append(IrValueId left, IrValueId right) noexcept {
    if (!IsValid(left) || !IsValid(right)) {
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    const IrType a = values_[left].type;
    const IrType b = values_[right].type;
    if (a.IsMatrix() || b.IsMatrix() || a.scalar != b.scalar ||
        a.components + b.components > MaxIrComponents) {
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    IrValue value;
    value.type = WithComponents(a, static_cast<uint8>(a.components + b.components));
    value.op = IrOp::Append;
    value.operandCount = 2;
    value.operands[0] = left;
    value.operands[1] = right;
    auto folded = Fold(value);
    if (!folded || folded.Value() != NoIrValue) {
        return Move(folded);
    }

    return Intern(value);
}

Result<IrValueId> IrBuilder::Convert(IrValueId operand, IrScalar target) noexcept {
    if (!IsValid(operand) || values_[operand].type.IsMatrix() ||
        static_cast<uint8>(target) > static_cast<uint8>(IrScalar::Double)) {
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    if (values_[operand].type.scalar == target) {
        return Result<IrValueId>::Success(operand);
    }

    IrValue value;
    value.type = IrType{target, values_[operand].type.components, 1};
    value.op = IrOp::Convert;
    value.operandCount = 1;
    value.operands[0] = operand;
    auto folded = Fold(value);
    if (!folded || folded.Value() != NoIrValue) {
        return Move(folded);
    }

    return Intern(value);
}

Result<IrValueId> IrBuilder::Select(IrValueId condition, IrValueId whenTrue,
                                    IrValueId whenFalse) noexcept {
    if (!IsValid(condition) || !IsValid(whenTrue) || !IsValid(whenFalse)) {
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    const IrType conditionType = values_[condition].type;
    const IrType type = values_[whenTrue].type;
    if (conditionType.scalar != IrScalar::Bool || conditionType.IsMatrix() ||
        type != values_[whenFalse].type ||
        (conditionType.components != 1 && conditionType.components != type.components)) {
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    if (whenTrue == whenFalse) {
        return Result<IrValueId>::Success(whenTrue);
    }

    IrValue value;
    value.type = type;
    value.op = IrOp::Select;
    value.operandCount = 3;
    value.operands[0] = condition;
    value.operands[1] = whenTrue;
    value.operands[2] = whenFalse;
    auto folded = Fold(value);
    if (!folded || folded.Value() != NoIrValue) {
        return Move(folded);
    }

    return Intern(value);
}

Result<IrValueId> IrBuilder::Branch(IrValueId condition, IrValueId whenTrue,
                                    IrValueId whenFalse) noexcept {
    if (!IsValid(condition) || !IsValid(whenTrue) || !IsValid(whenFalse) ||
        values_[condition].type != IrBool(1) || values_[whenTrue].type != values_[whenFalse].type) {
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    if (whenTrue == whenFalse) {
        return Result<IrValueId>::Success(whenTrue);
    }

    IrValue value;
    value.type = values_[whenTrue].type;
    value.op = IrOp::Branch;
    value.operandCount = 3;
    value.operands[0] = condition;
    value.operands[1] = whenTrue;
    value.operands[2] = whenFalse;
    auto folded = Fold(value);
    if (!folded || folded.Value() != NoIrValue) {
        return Move(folded);
    }

    return Intern(value);
}

Result<IrValueId> IrBuilder::Extern(uint32 index, IrExternVariant variant, IrType type,
                                    IrFrequency frequency,
                                    Span<const IrValueId> arguments) noexcept {
    if (!IsValidIrType(type) || arguments.Size() > MaxIrOperands ||
        static_cast<uint8>(variant) > static_cast<uint8>(IrExternVariant::Previous)) {
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    IrValue value;
    value.type = type;
    value.op = IrOp::Extern;
    value.frequency = frequency;
    value.operandCount = static_cast<uint8>(arguments.Size());
    for (usize at = 0; at < arguments.Size(); ++at) {
        if (!IsValid(arguments[at])) {
            return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
        }

        value.operands[at] = arguments[at];
    }

    value.immediate[0] = index;
    value.immediate[1] = static_cast<uint64>(variant);
    return Intern(value);
}

Result<IrValueId> IrBuilder::Pending(IrType type, IrPendingKind kind, uint8 prerequisite,
                                     Guid node) noexcept {
    if (!IsValidIrType(type)) {
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    IrValue value;
    value.type = type;
    value.op = IrOp::Pending;
    value.frequency = IrFrequency::Pixel;
    value.immediate[0] = static_cast<uint64>(kind);
    value.immediate[1] = prerequisite;
    value.immediate[2] = node.high;
    value.immediate[3] = node.low;
    return Intern(value);
}

Result<IrValueId> IrBuilder::Interpolate(IrValueId vertex) noexcept {
    if (!IsValid(vertex) || values_[vertex].type.scalar != IrScalar::Float ||
        values_[vertex].type.IsMatrix()) {
        return Result<IrValueId>::Failure(ErrorCode::InvalidArgument);
    }

    if (IsConstant(vertex)) {
        // 상수는 보간해도 같은 값이다.
        return Result<IrValueId>::Success(vertex);
    }

    IrValue value;
    value.type = values_[vertex].type;
    value.op = IrOp::Interpolate;
    value.frequency = IrFrequency::Pixel;
    value.operandCount = 1;
    value.operands[0] = vertex;
    return Intern(value);
}

} // namespace rocks::material
