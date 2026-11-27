#pragma once

#include <Base/Types.h>
#include <MaterialCompiler/MaterialIrValue.h>
#include <MaterialCompiler/MaterialTranslation.h>
#include <Platform/Memory.h>
#include <Platform/PlatformMath.h>

// 시험용 IR 계산기다(Docs/Design/MaterialSystem 6.13절 MS2f1 시험 — 상수 입력 그래프의 결과 값을 UE
// 식과 대조한다). Float·Bool 상수와 MS2f1 노드가 쓰는 구조·수 연산만 계산한다. 문맥 값(Extern)·
// 텍스처·모르는 연산은 실패(false)다. 깊이는 IR 번호 순서가 정해 주므로 재귀로 계산한다.
namespace material_ir_evaluator {

using namespace rocks;
using namespace rocks::material;

// 계산 깊이 상한이다(시험 그래프는 작다).
inline constexpr uint32 MaxEvaluationDepth = 512;
// Swizzle 즉시값의 성분당 bit 수와 mask다(MaterialIrOp.h Swizzle).
inline constexpr uint32 SwizzleBits = 2;
inline constexpr uint64 SwizzleMask = 0x3;

struct EvaluatedIr {
    float values[MaxIrComponents] = {};
    uint8 count = 0;
};

inline float EvaluatorFloatBits(uint64 immediate) noexcept {
    const uint32 bits = static_cast<uint32>(immediate);
    float result = 0.0f;
    platform::MemoryCopy(&result, &bits, sizeof(result));
    return result;
}

inline bool EvaluateIr(const MaterialTranslation& out, IrValueId id, EvaluatedIr& result,
                       uint32 depth = 0) noexcept {
    using Math = platform::PlatformMath;
    if (id == NoIrValue || id >= out.module.Values().Size() || depth > MaxEvaluationDepth) {
        return false;
    }

    const IrValue& value = out.module.Values()[id];
    EvaluatedIr operands[MaxIrOperands];
    if (value.op != IrOp::Constant) {
        for (uint8 at = 0; at < value.operandCount; ++at) {
            if (!EvaluateIr(out, value.operands[at], operands[at], depth + 1)) {
                return false;
            }
        }
    }

    result.count = value.type.components;
    // 성분 at의 피연산자 값이다(스칼라는 복제).
    const auto lane = [&](uint8 operand, uint8 at) noexcept {
        const EvaluatedIr& item = operands[operand];
        return item.count == 1 ? item.values[0] : item.values[at];
    };

    switch (value.op) {
    case IrOp::Constant:
        for (uint8 at = 0; at < result.count; ++at) {
            result.values[at] = value.type.scalar == IrScalar::Float
                                    ? EvaluatorFloatBits(value.immediate[at])
                                : value.type.scalar == IrScalar::UInt
                                    ? static_cast<float>(static_cast<uint32>(value.immediate[at]))
                                    : static_cast<float>(value.immediate[at] != 0 ? 1 : 0);
        }

        return value.type.scalar == IrScalar::Float || value.type.scalar == IrScalar::Bool ||
               value.type.scalar == IrScalar::UInt;
    case IrOp::Splat:
        for (uint8 at = 0; at < result.count; ++at) {
            result.values[at] = operands[0].values[0];
        }

        return true;
    case IrOp::Swizzle:
        for (uint8 at = 0; at < result.count; ++at) {
            result.values[at] =
                operands[0].values[(value.immediate[0] >> (at * SwizzleBits)) & SwizzleMask];
        }

        return true;
    case IrOp::Append: {
        uint8 at = 0;
        for (uint8 operand = 0; operand < value.operandCount; ++operand) {
            for (uint8 inner = 0; inner < operands[operand].count; ++inner) {
                result.values[at++] = operands[operand].values[inner];
            }
        }

        return at == result.count;
    }
    case IrOp::Convert:
        // UInt 값은 float에 정확히 담기는 범위(2^24 미만)만 쓴다. Float → UInt는 0 쪽으로 자른다.
        result = operands[0];
        result.count = value.type.components;
        if (value.type.scalar == IrScalar::UInt) {
            for (uint8 at = 0; at < result.count; ++at) {
                result.values[at] = static_cast<float>(static_cast<uint32>(result.values[at]));
            }
        }

        return true;
    case IrOp::Floor:
        for (uint8 at = 0; at < result.count; ++at) {
            result.values[at] = Math::Floor(lane(0, at));
        }

        return true;
    case IrOp::BitwiseAnd:
    case IrOp::BitwiseXor:
    case IrOp::BitShiftLeft:
    case IrOp::BitShiftRight:
    case IrOp::NotEquals:
        for (uint8 at = 0; at < result.count; ++at) {
            const uint32 x = static_cast<uint32>(lane(0, at));
            const uint32 y = static_cast<uint32>(lane(1, at));
            const uint32 bits = value.op == IrOp::BitwiseAnd      ? x & y
                                : value.op == IrOp::BitwiseXor    ? x ^ y
                                : value.op == IrOp::BitShiftLeft  ? x << y
                                : value.op == IrOp::BitShiftRight ? x >> y
                                                                  : (x != y ? 1u : 0u);
            result.values[at] = static_cast<float>(bits);
        }

        return true;
    case IrOp::Select:
        for (uint8 at = 0; at < result.count; ++at) {
            result.values[at] = lane(0, at) != 0.0f ? lane(1, at) : lane(2, at);
        }

        return true;
    case IrOp::Abs:
    case IrOp::Sqrt:
    case IrOp::Saturate:
    case IrOp::Sin:
    case IrOp::Cos:
    case IrOp::Exponential:
    case IrOp::Logarithm:
    case IrOp::Negate:
        for (uint8 at = 0; at < result.count; ++at) {
            const float x = lane(0, at);
            switch (value.op) {
            case IrOp::Abs:
                result.values[at] = x < 0.0f ? -x : x;
                break;
            case IrOp::Sqrt:
                result.values[at] = Math::Sqrt(x);
                break;
            case IrOp::Saturate:
                result.values[at] = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
                break;
            case IrOp::Sin:
                result.values[at] = Math::Sin(x);
                break;
            case IrOp::Cos:
                result.values[at] = Math::Cos(x);
                break;
            case IrOp::Exponential:
                result.values[at] = Math::Exp(x);
                break;
            case IrOp::Logarithm:
                result.values[at] = Math::Log(x);
                break;
            default:
                result.values[at] = -x;
                break;
            }
        }

        return true;
    case IrOp::Length: {
        float sum = 0.0f;
        for (uint8 at = 0; at < operands[0].count; ++at) {
            sum += operands[0].values[at] * operands[0].values[at];
        }

        result.values[0] = Math::Sqrt(sum);
        return true;
    }
    case IrOp::Dot: {
        float sum = 0.0f;
        const uint8 width =
            operands[0].count > operands[1].count ? operands[0].count : operands[1].count;
        for (uint8 at = 0; at < width; ++at) {
            sum += lane(0, at) * lane(1, at);
        }

        result.values[0] = sum;
        return true;
    }
    case IrOp::Cross: {
        const float* a = operands[0].values;
        const float* b = operands[1].values;
        result.values[0] = a[1] * b[2] - a[2] * b[1];
        result.values[1] = a[2] * b[0] - a[0] * b[2];
        result.values[2] = a[0] * b[1] - a[1] * b[0];
        return true;
    }
    case IrOp::Add:
    case IrOp::Subtract:
    case IrOp::Multiply:
    case IrOp::Divide:
    case IrOp::Min:
    case IrOp::Max:
    case IrOp::Pow:
    case IrOp::LessThan:
    case IrOp::LessThanOrEquals:
    case IrOp::GreaterThan:
        for (uint8 at = 0; at < result.count; ++at) {
            const float x = lane(0, at);
            const float y = lane(1, at);
            switch (value.op) {
            case IrOp::Add:
                result.values[at] = x + y;
                break;
            case IrOp::Subtract:
                result.values[at] = x - y;
                break;
            case IrOp::Multiply:
                result.values[at] = x * y;
                break;
            case IrOp::Divide:
                result.values[at] = x / y;
                break;
            case IrOp::Min:
                result.values[at] = x < y ? x : y;
                break;
            case IrOp::Max:
                result.values[at] = x > y ? x : y;
                break;
            case IrOp::Pow:
                result.values[at] = Math::Pow(x, y);
                break;
            case IrOp::LessThan:
                result.values[at] = x < y ? 1.0f : 0.0f;
                break;
            case IrOp::LessThanOrEquals:
                result.values[at] = x <= y ? 1.0f : 0.0f;
                break;
            default:
                result.values[at] = x > y ? 1.0f : 0.0f;
                break;
            }
        }

        return true;
    case IrOp::Lerp:
        for (uint8 at = 0; at < result.count; ++at) {
            const float x = lane(0, at);
            result.values[at] = x + lane(2, at) * (lane(1, at) - x);
        }

        return true;
    default:
        return false;
    }
}

} // namespace material_ir_evaluator
