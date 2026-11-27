#include <Core/Move.h>
#include <MaterialCompiler/MaterialShaderTraitsBuilder.h>

#include "IrResultQueries.h"

// 셰이더 특성 유도다(Docs/Design/MaterialSystem 8.3절 "유도 규칙").
namespace rocks::material {

namespace {

// 정점 문맥 진입점이다(나머지는 픽셀 문맥).
bool IsVertexEntry(IrEntry entry) noexcept {
    return entry == IrEntry::Vertex || entry == IrEntry::VertexPrevious ||
           entry == IrEntry::CustomOutputVertex || entry == IrEntry::CustomOutputVertexPrevious;
}

bool IsTranslucentBlend(BlendMode mode) noexcept {
    return mode != BlendMode::Opaque && mode != BlendMode::Masked;
}

} // namespace

Result<void> MakeMaterialShaderTraits(const MaterialTranslation& translation,
                                      const MaterialTraitsDesc& desc, MaterialQualityLevel quality,
                                      MaterialShaderTraits& out) noexcept {
    if (!translation.succeeded) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const IrModule& module = translation.module;
    const bool translucent = IsTranslucentBlend(desc.blendMode);
    out.domain = desc.domain;
    out.blendMode = desc.blendMode;
    out.twoSided = desc.twoSided;
    out.shadingModels = translation.shadingModels;
    out.translucencyPass =
        translucent ? desc.translucency.pass : TranslucencyPass::AfterDepthOfField;
    out.usesWpo = !detail::IsZeroIrResult(
        module, detail::FindIrResult(module, IrEntry::Vertex, "WorldPositionOffset"));
    out.usesPreviousWpo = !detail::IsZeroIrResult(
        module, detail::FindIrResult(module, IrEntry::VertexPrevious, "WorldPositionOffset"));
    out.usesPdo = !detail::IsZeroIrResult(
        module, detail::FindIrResult(module, IrEntry::PixelDepthOffset, "PixelDepthOffset"));
    out.usesOpacityMask = desc.blendMode == BlendMode::Masked;
    // UE 머티리얼 velocity 출력 조건(불투명은 정점 변형·픽셀 애니메이션, 반투명은 명시 표지).
    out.writesVelocity =
        translucent ? desc.outputTranslucentVelocity : out.usesWpo || desc.hasPixelAnimation;
    out.castsShadow = !translucent || desc.castDynamicShadowAsMasked;
    out.vertexFeatures = NoMaterialContextFeatures;
    out.pixelFeatures = NoMaterialContextFeatures;
    for (uint32 entry = 0; entry < IrEntryCount; ++entry) {
        const IrEntry item = static_cast<IrEntry>(entry);
        MaterialContextFeatureMask& target =
            IsVertexEntry(item) ? out.vertexFeatures : out.pixelFeatures;
        target |= module.EntryFeatures(item);
    }

    out.minimumTier = module.RequiredTier();
    out.engineDefaultMaterial = desc.engineDefaultMaterial;
    out.quality = quality;
    out.customOutputs.Clear();
    for (StringView name : translation.customOutputs) {
        String text(out.customOutputs.GetAllocator());
        if (auto assigned = text.TryAssign(name); !assigned) {
            return assigned;
        }

        if (auto pushed = out.customOutputs.TryPushBack(Move(text)); !pushed) {
            return pushed;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::material
