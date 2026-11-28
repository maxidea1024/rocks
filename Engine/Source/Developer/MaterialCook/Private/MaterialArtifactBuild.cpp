#include <MaterialCompiler/MaterialIrOp.h>
#include <MaterialCook/MaterialArtifactBuild.h>
#include <MaterialSchema/MaterialReferenceIndex.h>
#include <MaterialSchema/MaterialShaderTraits.h>

// 번역 값 → 런타임 artifact 변환이다(Docs/Design/MaterialSystem 12.8절 MS3d2).
namespace rocks {

namespace {

using material::IrOp;

// IR 텍스처 enum과 런타임 enum은 같은 값이다(변환이 값을 그대로 옮긴다).
static_assert(static_cast<uint8>(material::IrTextureSource::Asset) ==
                      static_cast<uint8>(material::MaterialTextureSource::Asset) &&
                  static_cast<uint8>(material::IrTextureSource::Parameter) ==
                      static_cast<uint8>(material::MaterialTextureSource::Parameter),
              "MaterialTextureSource must match IrTextureSource");
static_assert(static_cast<uint8>(material::IrTextureKind::None) ==
                      static_cast<uint8>(material::MaterialTextureKind::None) &&
                  static_cast<uint8>(material::IrTextureKind::Texture2D) ==
                      static_cast<uint8>(material::MaterialTextureKind::Texture2D) &&
                  static_cast<uint8>(material::IrTextureKind::TextureCube) ==
                      static_cast<uint8>(material::MaterialTextureKind::TextureCube) &&
                  static_cast<uint8>(material::IrTextureKind::Texture2DArray) ==
                      static_cast<uint8>(material::MaterialTextureKind::Texture2DArray) &&
                  static_cast<uint8>(material::IrTextureKind::TextureCubeArray) ==
                      static_cast<uint8>(material::MaterialTextureKind::TextureCubeArray) &&
                  static_cast<uint8>(material::IrTextureKind::Texture3D) ==
                      static_cast<uint8>(material::MaterialTextureKind::Texture3D) &&
                  static_cast<uint8>(material::IrTextureKind::External) ==
                      static_cast<uint8>(material::MaterialTextureKind::External),
              "MaterialTextureKind must match IrTextureKind");
static_assert(static_cast<uint8>(material::IrTextureSampler::None) ==
                      static_cast<uint8>(material::MaterialSamplerSource::None) &&
                  static_cast<uint8>(material::IrTextureSampler::Own) ==
                      static_cast<uint8>(material::MaterialSamplerSource::Own) &&
                  static_cast<uint8>(material::IrTextureSampler::SharedWrap) ==
                      static_cast<uint8>(material::MaterialSamplerSource::SharedWrap) &&
                  static_cast<uint8>(material::IrTextureSampler::SharedClamp) ==
                      static_cast<uint8>(material::MaterialSamplerSource::SharedClamp),
              "MaterialSamplerSource must match IrTextureSampler");
static_assert(material::MaterialConstantOperands == material::MaxIrOperands &&
                  material::MaterialConstantImmediates == material::IrImmediateSlots,
              "Constant instruction slots must match the IR value slots");
using material::MaterialConstantOp;

// IR 연산 → 상수 식 연산이다(상수 빌더 집합 밖이면 false).
bool ConstantOpOf(IrOp op, MaterialConstantOp& out) noexcept {
    switch (op) {
    case IrOp::Constant:
        out = MaterialConstantOp::Constant;
        return true;
    case IrOp::Parameter:
        out = MaterialConstantOp::Parameter;
        return true;
    case IrOp::Texture:
        out = MaterialConstantOp::Texture;
        return true;
    case IrOp::TextureSize:
        out = MaterialConstantOp::TextureSize;
        return true;
    case IrOp::Add:
        out = MaterialConstantOp::Add;
        return true;
    case IrOp::Subtract:
        out = MaterialConstantOp::Subtract;
        return true;
    case IrOp::Multiply:
        out = MaterialConstantOp::Multiply;
        return true;
    case IrOp::Divide:
        out = MaterialConstantOp::Divide;
        return true;
    case IrOp::Min:
        out = MaterialConstantOp::Min;
        return true;
    case IrOp::Max:
        out = MaterialConstantOp::Max;
        return true;
    case IrOp::Abs:
        out = MaterialConstantOp::Abs;
        return true;
    case IrOp::Floor:
        out = MaterialConstantOp::Floor;
        return true;
    case IrOp::Ceil:
        out = MaterialConstantOp::Ceil;
        return true;
    case IrOp::Round:
        out = MaterialConstantOp::Round;
        return true;
    case IrOp::Truncate:
        out = MaterialConstantOp::Truncate;
        return true;
    case IrOp::Frac:
        out = MaterialConstantOp::Frac;
        return true;
    case IrOp::Saturate:
        out = MaterialConstantOp::Saturate;
        return true;
    case IrOp::Sign:
        out = MaterialConstantOp::Sign;
        return true;
    case IrOp::Sqrt:
        out = MaterialConstantOp::Sqrt;
        return true;
    case IrOp::Reciprocal:
        out = MaterialConstantOp::Reciprocal;
        return true;
    case IrOp::Length:
        out = MaterialConstantOp::Length;
        return true;
    case IrOp::Normalize:
        out = MaterialConstantOp::Normalize;
        return true;
    case IrOp::Dot:
        out = MaterialConstantOp::Dot;
        return true;
    case IrOp::Cross:
        out = MaterialConstantOp::Cross;
        return true;
    case IrOp::Sin:
        out = MaterialConstantOp::Sin;
        return true;
    case IrOp::Cos:
        out = MaterialConstantOp::Cos;
        return true;
    case IrOp::Tan:
        out = MaterialConstantOp::Tan;
        return true;
    case IrOp::ASin:
        out = MaterialConstantOp::ASin;
        return true;
    case IrOp::ACos:
        out = MaterialConstantOp::ACos;
        return true;
    case IrOp::ATan:
        out = MaterialConstantOp::ATan;
        return true;
    case IrOp::ATan2:
        out = MaterialConstantOp::ATan2;
        return true;
    case IrOp::Exponential:
        out = MaterialConstantOp::Exponential;
        return true;
    case IrOp::Exponential2:
        out = MaterialConstantOp::Exponential2;
        return true;
    case IrOp::Logarithm:
        out = MaterialConstantOp::Logarithm;
        return true;
    case IrOp::Logarithm2:
        out = MaterialConstantOp::Logarithm2;
        return true;
    case IrOp::Logarithm10:
        out = MaterialConstantOp::Logarithm10;
        return true;
    case IrOp::Fmod:
        out = MaterialConstantOp::Fmod;
        return true;
    case IrOp::Modulo:
        out = MaterialConstantOp::Modulo;
        return true;
    case IrOp::Negate:
        out = MaterialConstantOp::Negate;
        return true;
    case IrOp::Clamp:
        out = MaterialConstantOp::Clamp;
        return true;
    case IrOp::Splat:
        out = MaterialConstantOp::Splat;
        return true;
    case IrOp::Swizzle:
        out = MaterialConstantOp::Swizzle;
        return true;
    case IrOp::Append:
        out = MaterialConstantOp::Append;
        return true;
    case IrOp::Convert:
        out = MaterialConstantOp::Convert;
        return true;
    default:
        return false;
    }
}

// IR 값 형 → 명령 스칼라 형이다(Float·Double·Texture 밖이면 false).
bool ConstantScalarOf(const material::IrType& type, material::MaterialConstantScalar& out,
                      uint8& components) noexcept {
    switch (type.scalar) {
    case material::IrScalar::Float:
        out = material::MaterialConstantScalar::Float;
        break;
    case material::IrScalar::Double:
        out = material::MaterialConstantScalar::Double;
        break;
    case material::IrScalar::Texture:
        out = material::MaterialConstantScalar::Texture;
        components = 0;
        return true;
    default:
        return false;
    }

    components = type.components;
    return type.rows <= 1;
}

} // namespace

Result<void> MakeMaterialArtifact(const MaterialTranslationValue& value, NamePool& names,
                                  const MaterialArtifactLinks& links,
                                  material::MaterialArtifact& out) noexcept {
    if (links.parameters.Size() != value.parameters.Size() ||
        links.textures.Size() != value.textures.Size() ||
        links.textureParameters.Size() != value.textures.Size() ||
        links.collections.Size() != value.collections.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 번호는 모두 있어야 하고(없음 표지는 처리기의 잘못), 텍스처 파라미터 번호는 파라미터 slot에만
    // 있다.
    const auto present = [](Span<const uint32> list) noexcept {
        for (uint32 index : list) {
            if (index == material::NoMaterialReference) {
                return false;
            }
        }

        return true;
    };

    if (!present(links.parameters) || !present(links.textures) || !present(links.collections)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (usize index = 0; index < value.textures.Size(); ++index) {
        const bool parameterSlot =
            value.textures[index].source == material::IrTextureSource::Parameter;
        if (parameterSlot != (links.textureParameters[index] != material::NoMaterialReference)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    out.shaderSetId = Sha256Digest{};
    out.layoutSignature = Sha256Digest{};
    out.floatVectors = 0;
    out.traits.Clear();
    out.constants.Clear();
    out.program.Clear();
    out.parameters.Clear();
    out.textures.Clear();
    out.collections.Clear();
    out.userSceneTextures.Clear();
    auto setId = MaterialShaderSetId(value);
    if (!setId) {
        return Result<void>::Failure(setId.Error());
    }

    auto layout = MaterialLayoutSignature(value);
    if (!layout) {
        return Result<void>::Failure(layout.Error());
    }

    out.shaderSetId = setId.Value();
    out.layoutSignature = layout.Value();
    if (auto written = material::WriteMaterialShaderTraits(value.traits, out.traits); !written) {
        return written;
    }

    out.floatVectors = value.floatVectors;
    for (const material::MaterialConstantMember& member : value.members) {
        material::MaterialConstantSlot slot;
        slot.kind = member.kind;
        slot.vector = member.vector;
        slot.firstComponent = member.firstComponent;
        slot.components = member.components;
        slot.instruction = member.step;
        if (auto pushed = out.constants.TryPushBack(slot); !pushed) {
            return pushed;
        }
    }

    for (const material::IrValue& step : value.steps) {
        material::MaterialConstantInstruction instruction;
        if (!ConstantOpOf(step.op, instruction.op) ||
            !ConstantScalarOf(step.type, instruction.scalar, instruction.components)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        // operandCount 밖 칸은 0이다(IR의 NoIrValue를 옮기지 않는다 — MaterialConstantProgram.h).
        instruction.operandCount = step.operandCount;
        for (usize index = 0; index < material::MaterialConstantOperands; ++index) {
            instruction.operands[index] = index < step.operandCount ? step.operands[index] : 0;
        }

        for (usize index = 0; index < material::MaterialConstantImmediates; ++index) {
            instruction.immediates[index] = step.immediate[index];
        }

        // Parameter·Texture의 즉시값 [3]은 artifact 파라미터·텍스처 slot 번호 그대로다(값의 표와
        // 같은 순서 — artifact.parameters가 Material::parameters로 잇는다).
        const uint64 table = step.immediate[material::MaterialConstantTableImmediate];
        if ((step.op == IrOp::Parameter && table >= value.parameters.Size()) ||
            (step.op == IrOp::Texture && table >= value.textures.Size())) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (auto pushed = out.program.TryPushBack(instruction); !pushed) {
            return pushed;
        }
    }

    if (auto copied = out.parameters.TryAppend(links.parameters); !copied) {
        return copied;
    }

    for (usize index = 0; index < value.textures.Size(); ++index) {
        const material::IrTexture& texture = value.textures[index];
        material::MaterialTextureSlot slot;
        slot.source = static_cast<material::MaterialTextureSource>(texture.source);
        slot.kind = static_cast<material::MaterialTextureKind>(texture.kind);
        slot.sampler = static_cast<material::MaterialSamplerSource>(texture.sampler);
        slot.parameter = links.textureParameters[index];
        slot.texture = links.textures[index];
        // 논리 크기 사용: TextureSize 명령의 피연산자(Texture 명령)가 이 slot이다.
        for (const material::IrValue& step : value.steps) {
            if (step.op != IrOp::TextureSize || step.operandCount == 0 ||
                step.operands[0] >= value.steps.Size()) {
                continue;
            }

            const material::IrValue& operand = value.steps[step.operands[0]];
            slot.logicalSize =
                slot.logicalSize || (operand.op == IrOp::Texture && operand.immediate[3] == index);
        }

        if (auto pushed = out.textures.TryPushBack(slot); !pushed) {
            return pushed;
        }
    }

    if (auto copied = out.collections.TryAppend(links.collections); !copied) {
        return copied;
    }

    for (const String& name : value.userSceneTextures) {
        auto interned = names.TryIntern(name.View());
        if (!interned) {
            return Result<void>::Failure(interned.Error());
        }

        if (auto pushed = out.userSceneTextures.TryPushBack(interned.Value()); !pushed) {
            return pushed;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks
