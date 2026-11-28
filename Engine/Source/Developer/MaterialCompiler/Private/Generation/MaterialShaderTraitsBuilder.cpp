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

// UE IsTranslucentOnlyBlendMode(Translucent·색 투과 — Rocks에 회색 투과 모드는 없다).
bool IsTranslucentOnlyBlend(BlendMode mode) noexcept {
    return mode == BlendMode::Translucent || mode == BlendMode::TranslucentColoredTransmittance;
}

// SingleLayerWater shading model 비트다(Renderer 14.2절 번호 — RenderData ShadingModel).
constexpr uint32 SingleLayerWaterModels = 1u << static_cast<uint32>(ShadingModel::SingleLayerWater);
// first person custom 출력 이름이다(6.11절 MS2d3 표 — UE HasFirstPersonOutput).
constexpr StringView FirstPersonOutputName = "FirstPersonOutput";

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
    // 메시 위치를 바꿀 수 있는가다(UE MaterialMayModifyMeshPosition — WPO·PDO·first person 출력,
    // displacement는 아직 lowering하지 않는다).
    bool firstPerson = false;
    for (StringView name : translation.customOutputs) {
        firstPerson = firstPerson || name == FirstPersonOutputName;
    }

    const bool movesMesh = out.usesWpo || out.usesPdo || firstPerson;
    out.writesVelocity =
        translucent ? desc.outputTranslucentVelocity : movesMesh || desc.hasPixelAnimation;
    // UE ShouldCastDynamicShadows다.
    out.castsShadow = translation.shadingModels != SingleLayerWaterModels &&
                      (!translucent ||
                       (IsTranslucentOnlyBlend(desc.blendMode) && desc.castDynamicShadowAsMasked));
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
    // 품질 칸을 읽지 않은 번역은 수준마다 같은 결과다(8.3절 — 셰이더 집합 id 공유).
    out.quality = (translation.consultedTarget & ToMask(MaterialTargetField::Quality)) != 0
                      ? quality
                      : MaterialQualityLevel::None;
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
