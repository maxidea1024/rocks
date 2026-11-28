#include <MaterialSchema/MaterialArtifactValidation.h>
#include <MaterialSchema/MaterialReferenceIndex.h>

// 쿠킹 artifact 구조 검증이다(Docs/Design/MaterialSystem 12.8절 MS3d2).
namespace rocks::material {

namespace {

// float4 한 칸의 성분 수다.
constexpr uint32 VectorComponents = 4;

// 명령의 연산·형이 맞는가(Texture 명령만 Texture 형·성분 0, 그 밖은 Float·Double 1–4 성분).
bool ValidShape(const MaterialConstantInstruction& instruction) noexcept {
    if (static_cast<uint8>(instruction.op) > static_cast<uint8>(LastMaterialConstantOp) ||
        instruction.operandCount > MaterialConstantOperands) {
        return false;
    }

    if (instruction.scalar == MaterialConstantScalar::Texture) {
        return instruction.op == MaterialConstantOp::Texture && instruction.components == 0;
    }

    return (instruction.scalar == MaterialConstantScalar::Float ||
            instruction.scalar == MaterialConstantScalar::Double) &&
           instruction.op != MaterialConstantOp::Texture && instruction.components >= 1 &&
           instruction.components <= VectorComponents;
}

} // namespace

Result<void> ValidateMaterialArtifact(const MaterialArtifact& artifact,
                                      const MaterialArtifactTables& tables) noexcept {
    const auto corrupt = []() noexcept { return Result<void>::Failure(ErrorCode::CorruptData); };
    const Span<const MaterialConstantInstruction> program = artifact.program.View();
    for (usize index = 0; index < program.Size(); ++index) {
        const MaterialConstantInstruction& instruction = program[index];
        if (!ValidShape(instruction)) {
            return corrupt();
        }

        // 잎 명령은 피연산자가 없다.
        const bool leaf = instruction.op == MaterialConstantOp::Constant ||
                          instruction.op == MaterialConstantOp::Parameter ||
                          instruction.op == MaterialConstantOp::Texture;
        if (leaf && instruction.operandCount != 0) {
            return corrupt();
        }

        // 피연산자는 앞 명령이고, Texture 명령은 TextureSize의 피연산자로만 쓴다.
        for (uint8 operand = 0; operand < instruction.operandCount; ++operand) {
            const uint32 at = instruction.operands[operand];
            if (at >= index) {
                return corrupt();
            }

            const bool texture = program[at].op == MaterialConstantOp::Texture;
            if (texture != (instruction.op == MaterialConstantOp::TextureSize)) {
                return corrupt();
            }
        }

        const uint64 table = instruction.immediates[MaterialConstantTableImmediate];
        if ((instruction.op == MaterialConstantOp::TextureSize && instruction.operandCount != 1) ||
            (instruction.op == MaterialConstantOp::Parameter &&
             table >= artifact.parameters.Size()) ||
            (instruction.op == MaterialConstantOp::Texture && table >= artifact.textures.Size())) {
            return corrupt();
        }
    }

    for (const MaterialConstantSlot& slot : artifact.constants) {
        if (slot.instruction >= program.Size() || slot.vector >= artifact.floatVectors ||
            static_cast<uint32>(slot.firstComponent) + slot.components > VectorComponents) {
            return corrupt();
        }

        // 칸의 성분 수는 그 명령과 같고, Double 명령은 tile·offset 두 칸, Float 명령은 Value
        // 칸이다.
        const MaterialConstantInstruction& instruction = program[slot.instruction];
        const bool wide = instruction.scalar == MaterialConstantScalar::Double;
        const bool wideSlot = slot.kind == MaterialConstantSlotKind::DoubleTile ||
                              slot.kind == MaterialConstantSlotKind::DoubleOffset;
        if (instruction.scalar == MaterialConstantScalar::Texture ||
            slot.components != instruction.components || wide != wideSlot ||
            static_cast<uint8>(slot.kind) >
                static_cast<uint8>(MaterialConstantSlotKind::DoubleOffset)) {
            return corrupt();
        }
    }

    for (uint32 parameter : artifact.parameters) {
        if (parameter >= tables.parameters) {
            return corrupt();
        }
    }

    for (const MaterialTextureSlot& slot : artifact.textures) {
        const bool parameterSlot = slot.source == MaterialTextureSource::Parameter;
        if (static_cast<uint8>(slot.source) >
                static_cast<uint8>(MaterialTextureSource::Parameter) ||
            slot.kind == MaterialTextureKind::None ||
            static_cast<uint8>(slot.kind) > static_cast<uint8>(MaterialTextureKind::External) ||
            static_cast<uint8>(slot.sampler) >
                static_cast<uint8>(MaterialSamplerSource::SharedClamp) ||
            slot.texture >= tables.textures ||
            parameterSlot != (slot.parameter != NoMaterialReference) ||
            (parameterSlot && slot.parameter >= tables.parameters)) {
            return corrupt();
        }
    }

    for (uint32 collection : artifact.collections) {
        if (collection >= tables.collections) {
            return corrupt();
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::material
