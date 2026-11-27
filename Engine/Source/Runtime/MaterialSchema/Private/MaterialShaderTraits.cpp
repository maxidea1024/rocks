#include <Core/Encoding/ByteReader.h>
#include <Core/Encoding/ByteWriter.h>
#include <Core/Move.h>
#include <Base/NumericLimits.h>
#include <MaterialSchema/MaterialShaderTraits.h>

// 머티리얼 셰이더 특성의 정규 바이트다(Docs/Design/MaterialSystem 8.3절 "정규 바이트").
namespace rocks::material {

namespace {

// 열거 값의 상한(마지막 값)이다 — 해석이 범위 밖 값을 손상으로 본다.
constexpr uint8 LastDomain = static_cast<uint8>(MaterialDomain::UserInterface);
constexpr uint8 LastBlendMode = static_cast<uint8>(BlendMode::TranslucentColoredTransmittance);
constexpr uint8 LastTranslucencyPass = static_cast<uint8>(TranslucencyPass::AfterMotionBlur);
constexpr uint8 LastQuality = static_cast<uint8>(MaterialQualityLevel::None);
constexpr uint8 LastTier = static_cast<uint8>(MaterialCapabilityTier::Sm6);

} // namespace

Result<void> WriteMaterialShaderTraits(const MaterialShaderTraits& traits,
                                       Array<uint8>& out) noexcept {
    ByteWriter writer(out);
    writer.WriteLittleEndian<uint32>(MaterialShaderTraitsVersion);
    writer.WriteU8(static_cast<uint8>(traits.domain));
    writer.WriteU8(static_cast<uint8>(traits.blendMode));
    writer.WriteU8(static_cast<uint8>(traits.translucencyPass));
    writer.WriteU8(static_cast<uint8>(traits.quality));
    writer.WriteU8(static_cast<uint8>(traits.minimumTier));
    const bool flags[] = {traits.twoSided,        traits.writesVelocity,       traits.castsShadow,
                          traits.usesWpo,         traits.usesPreviousWpo,      traits.usesPdo,
                          traits.usesOpacityMask, traits.engineDefaultMaterial};
    for (bool flag : flags) {
        writer.WriteU8(flag ? 1 : 0);
    }

    writer.WriteLittleEndian<uint32>(traits.shadingModels);
    writer.WriteLittleEndian<uint32>(traits.vertexFeatures);
    writer.WriteLittleEndian<uint32>(traits.pixelFeatures);
    if (traits.customOutputs.Size() > NumericLimits<uint32>::Max()) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    writer.WriteLittleEndian<uint32>(static_cast<uint32>(traits.customOutputs.Size()));
    for (const String& name : traits.customOutputs) {
        if (name.ByteCount() > NumericLimits<uint32>::Max()) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }

        writer.WriteLittleEndian<uint32>(static_cast<uint32>(name.ByteCount()));
        writer.WriteBytes(
            Span<const uint8>(reinterpret_cast<const uint8*>(name.Data()), name.ByteCount()));
    }

    return writer.Status();
}

Result<void> ReadMaterialShaderTraits(Span<const uint8> bytes, MaterialShaderTraits& out,
                                      usize& consumed) noexcept {
    ByteReader reader(bytes);
    const uint32 version = reader.ReadLittleEndian<uint32>();
    const uint8 domain = reader.ReadU8();
    const uint8 blend = reader.ReadU8();
    const uint8 pass = reader.ReadU8();
    const uint8 quality = reader.ReadU8();
    const uint8 tier = reader.ReadU8();
    bool* const flags[] = {&out.twoSided,        &out.writesVelocity,       &out.castsShadow,
                           &out.usesWpo,         &out.usesPreviousWpo,      &out.usesPdo,
                           &out.usesOpacityMask, &out.engineDefaultMaterial};
    for (bool* flag : flags) {
        const uint8 value = reader.ReadU8();
        if (value > 1) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        *flag = value == 1;
    }

    out.shadingModels = reader.ReadLittleEndian<uint32>();
    out.vertexFeatures = reader.ReadLittleEndian<uint32>();
    out.pixelFeatures = reader.ReadLittleEndian<uint32>();
    const uint32 outputs = reader.ReadLittleEndian<uint32>();
    if (reader.Failed() || version != MaterialShaderTraitsVersion || domain > LastDomain ||
        blend > LastBlendMode || pass > LastTranslucencyPass || quality > LastQuality ||
        tier > LastTier || outputs > reader.Remaining()) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    out.domain = static_cast<MaterialDomain>(domain);
    out.blendMode = static_cast<BlendMode>(blend);
    out.translucencyPass = static_cast<TranslucencyPass>(pass);
    out.quality = static_cast<MaterialQualityLevel>(quality);
    out.minimumTier = static_cast<MaterialCapabilityTier>(tier);
    out.customOutputs.Clear();
    for (uint32 index = 0; index < outputs; ++index) {
        const uint32 length = reader.ReadLittleEndian<uint32>();
        const Span<const uint8> text = reader.Take(length);
        if (reader.Failed()) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        String name(out.customOutputs.GetAllocator());
        if (auto assigned =
                name.TryAssign(StringView(reinterpret_cast<const char*>(text.Data()), text.Size()));
            !assigned) {
            return assigned;
        }

        if (auto pushed = out.customOutputs.TryPushBack(Move(name)); !pushed) {
            return pushed;
        }
    }

    consumed = reader.Position();
    return Result<void>::Success();
}

} // namespace rocks::material
