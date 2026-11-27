#include <Core/Memory/SystemAllocator.h>
#include <MaterialCompiler/MaterialConstants.h>

// 상수 블록 layout과 상수 식 프로그램 만들기다(Docs/Design/MaterialSystem 6.10절 MS2c2, 9.4절).
namespace rocks::material {

namespace {

// float4 칸 하나의 성분 수다.
constexpr uint32 ConstantVectorComponents = 4;

} // namespace

bool IsMaterialConstantProgramOp(IrOp op) noexcept {
    // UE uniform 식으로 접히는 연산과 같다(HLSLMaterialTranslator.cpp:5358–10907). Pow·Lerp·
    // Smoothstep·Step·Select·비교는 UE도 접지 않는다.
    switch (op) {
    case IrOp::Parameter:
    case IrOp::Add:
    case IrOp::Subtract:
    case IrOp::Multiply:
    case IrOp::Divide:
    case IrOp::Min:
    case IrOp::Max:
    case IrOp::Abs:
    case IrOp::Floor:
    case IrOp::Ceil:
    case IrOp::Round:
    case IrOp::Truncate:
    case IrOp::Frac:
    case IrOp::Saturate:
    case IrOp::Sign:
    case IrOp::Sqrt:
    case IrOp::Reciprocal:
    case IrOp::Length:
    case IrOp::Normalize:
    case IrOp::Dot:
    case IrOp::Cross:
    case IrOp::Sin:
    case IrOp::Cos:
    case IrOp::Tan:
    case IrOp::ASin:
    case IrOp::ACos:
    case IrOp::ATan:
    case IrOp::ATan2:
    case IrOp::Exponential:
    case IrOp::Exponential2:
    case IrOp::Logarithm:
    case IrOp::Logarithm2:
    case IrOp::Logarithm10:
    case IrOp::Fmod:
    case IrOp::Modulo:
    case IrOp::Negate:
    case IrOp::Clamp:
    case IrOp::Splat:
    case IrOp::Swizzle:
    case IrOp::Append:
    case IrOp::Convert:
    case IrOp::TextureSize:
        return true;
    default:
        return false;
    }
}

Result<void> BuildMaterialConstants(const IrModule& module, MaterialConstants& out) noexcept {
    out.steps.Clear();
    out.members.Clear();
    out.floatVectors = 0;
    const Span<const IrValue> values = module.Values();
    SystemAllocator scratch;
    // 프로그램이 계산할 수 있는 값이다(빈도 Uniform 이하, 프로그램 연산, 피연산자도 상수나 프로그램
    // 값). Convert는 Float·Double 사이만이다.
    Array<bool> programmable(scratch);
    Array<bool> output(scratch);
    if (auto sized = programmable.TryResize(values.Size()); !sized) {
        return sized;
    }

    if (auto sized = output.TryResize(values.Size()); !sized) {
        return sized;
    }

    // Pow 밑의 Abs는 셰이더에 남긴다(6.8절 Power 행 — 프로그램이 abs를 계산하면 셰이더에는 상수
    // 버퍼 값을 바로 pow해 FXC가 음수 밑 경고 X3571을 낸다, 7.8절).
    Array<bool> powBase(scratch);
    if (auto sized = powBase.TryResize(values.Size()); !sized) {
        return sized;
    }

    for (bool& item : powBase) {
        item = false;
    }

    for (const IrValue& value : values) {
        if (value.op == IrOp::Pow && value.operandCount > 0 && value.operands[0] < values.Size() &&
            values[value.operands[0]].op == IrOp::Abs) {
            powBase[value.operands[0]] = true;
        }
    }

    for (usize index = 0; index < values.Size(); ++index) {
        const IrValue& value = values[index];
        output[index] = false;
        bool can =
            !powBase[index] && value.op != IrOp::Constant &&
            static_cast<uint8>(value.frequency) <= static_cast<uint8>(IrFrequency::Uniform) &&
            IsMaterialConstantProgramOp(value.op) && !value.type.IsMatrix() &&
            (value.type.scalar == IrScalar::Float || value.type.scalar == IrScalar::Double);
        for (uint8 operand = 0; operand < value.operandCount; ++operand) {
            if (value.operands[operand] >= index) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }

        for (uint8 operand = 0; can && operand < value.operandCount; ++operand) {
            const IrValue& source = values[value.operands[operand]];
            // 텍스처 값은 TextureSize의 slot leaf로만 프로그램에 든다(6.10절 MS2c3b).
            can = source.op == IrOp::Constant  ? (source.type.scalar == IrScalar::Float ||
                                                 source.type.scalar == IrScalar::Double)
                  : source.op == IrOp::Texture ? value.op == IrOp::TextureSize
                                               : programmable[value.operands[operand]];
        }

        programmable[index] = can;
    }

    // 출력: 프로그램 밖 소비자가 있거나 진입점 결과인 빈도 Uniform 프로그램 값이다. 빈도 Constant인
    // 값(파라미터에 기대지 않는 식)은 셰이더 원문에 그대로 남는다(UE도 상수 uniform 식은 코드에
    // 넣는다 — HLSLMaterialTranslator AddUniformExpression의 IsConstant).
    for (usize index = 0; index < values.Size(); ++index) {
        const IrValue& value = values[index];
        if (programmable[index]) {
            continue;
        }

        for (uint8 operand = 0; operand < value.operandCount; ++operand) {
            if (programmable[value.operands[operand]] &&
                values[value.operands[operand]].frequency == IrFrequency::Uniform) {
                output[value.operands[operand]] = true;
            }
        }
    }

    for (const IrResult& result : module.Results()) {
        if (programmable[result.value] && values[result.value].frequency == IrFrequency::Uniform) {
            output[result.value] = true;
        }
    }

    // 명령: 출력까지 닿는 프로그램 값과 상수를 모듈 번호 순으로 낸다.
    Array<bool> needed(scratch);
    Array<uint32> stepOf(scratch);
    if (auto sized = needed.TryResize(values.Size()); !sized) {
        return sized;
    }

    if (auto sized = stepOf.TryResize(values.Size()); !sized) {
        return sized;
    }

    for (usize index = 0; index < values.Size(); ++index) {
        needed[index] = output[index];
    }

    for (usize index = values.Size(); index > 0; --index) {
        if (!needed[index - 1] || values[index - 1].op == IrOp::Constant) {
            continue;
        }

        const IrValue& value = values[index - 1];
        for (uint8 operand = 0; operand < value.operandCount; ++operand) {
            needed[value.operands[operand]] = true;
        }
    }

    for (usize index = 0; index < values.Size(); ++index) {
        if (!needed[index]) {
            continue;
        }

        IrValue step = values[index];
        for (uint8 operand = 0; operand < step.operandCount; ++operand) {
            step.operands[operand] = stepOf[step.operands[operand]];
        }

        stepOf[index] = static_cast<uint32>(out.steps.Size());
        if (auto added = out.steps.TryPushBack(step); !added) {
            return added;
        }
    }

    // layout: 크기 큰 것부터(같은 크기는 값 번호 순) float4 경계를 넘지 않게 앞에서부터 채운다.
    Array<uint8> used(scratch);
    for (uint32 size = ConstantVectorComponents; size > 0; --size) {
        for (usize index = 0; index < values.Size(); ++index) {
            if (!output[index] || values[index].type.components != size) {
                continue;
            }

            const bool wide = values[index].type.scalar == IrScalar::Double;
            const MaterialConstantSlotKind kinds[] = {wide ? MaterialConstantSlotKind::DoubleTile
                                                           : MaterialConstantSlotKind::Value,
                                                      MaterialConstantSlotKind::DoubleOffset};
            for (uint32 part = 0; part < (wide ? 2u : 1u); ++part) {
                usize vector = 0;
                while (vector < used.Size() && used[vector] + size > ConstantVectorComponents) {
                    ++vector;
                }

                if (vector == used.Size()) {
                    if (auto added = used.TryPushBack(0); !added) {
                        return added;
                    }
                }

                MaterialConstantMember member;
                member.value = static_cast<IrValueId>(index);
                member.step = stepOf[index];
                member.kind = kinds[part];
                member.vector = static_cast<uint32>(vector);
                member.firstComponent = used[vector];
                member.components = static_cast<uint8>(size);
                used[vector] = static_cast<uint8>(used[vector] + size);
                if (auto added = out.members.TryPushBack(member); !added) {
                    return added;
                }
            }
        }
    }

    out.floatVectors = static_cast<uint32>(used.Size());
    return Result<void>::Success();
}

} // namespace rocks::material
