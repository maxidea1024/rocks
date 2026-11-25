#include <Material/Material.h>
#include <Material/MaterialFunctionInstance.h>
#include <Material/MaterialInstance.h>
#include <Material/MaterialPrerequisiteCheck.h>

// 에셋 TODO 필드 검사다(Docs/Design/MaterialSystem 4.6.1·19.4.1·19.9·19.11절).
namespace rocks {

namespace {

using material::MaterialAssetReference;
using material::MaterialPrerequisite;

// 검사 한 줄이다(쓰였는가·선행·필드 경로).
struct Use {
    bool used;
    MaterialPrerequisite prerequisite;
    StringView field;
};

Result<void> AddUses(Array<MaterialPrerequisiteUse>& out, Span<const Use> uses) noexcept {
    for (const Use& use : uses) {
        if (!use.used) {
            continue;
        }

        if (auto added = out.TryPushBack(MaterialPrerequisiteUse{use.prerequisite, use.field});
            !added) {
            return added;
        }
    }

    return Result<void>::Success();
}

bool AnySet(Span<const MaterialAssetReference> references) noexcept {
    for (const MaterialAssetReference& reference : references) {
        if (!reference.IsNull()) {
            return true;
        }
    }

    return false;
}

// Optional override는 값이 있으면(없음으로 덮기 포함) 쓰인 것이다.
bool HasOverride(const Optional<MaterialAssetReference>& reference) noexcept {
    return reference.HasValue();
}

bool SameDisplacement(const material::MaterialDisplacementTraits& left,
                      const material::MaterialDisplacementTraits& right) noexcept {
    return left.enableTessellation == right.enableTessellation &&
           left.magnitude == right.magnitude && left.center == right.center &&
           left.enableFade == right.enableFade &&
           left.fadeStartSizePixels == right.fadeStartSizePixels &&
           left.fadeEndSizePixels == right.fadeEndSizePixels;
}

bool SameLightBake(const MaterialLightBakeSettings& left,
                   const MaterialLightBakeSettings& right) noexcept {
    return left.castShadowAsMasked == right.castShadowAsMasked &&
           left.emissiveBoost == right.emissiveBoost && left.diffuseBoost == right.diffuseBoost &&
           left.exportResolutionScale == right.exportResolutionScale;
}

// 블렌드 모드 값의 선행이다(Renderer 14.2절 후속 값).
MaterialPrerequisite BlendModePrerequisite(BlendMode mode) noexcept {
    switch (mode) {
    case BlendMode::AlphaHoldout:
        return MaterialPrerequisite::AlphaHoldout;
    case BlendMode::TranslucentColoredTransmittance:
        return MaterialPrerequisite::ColoredTransmittance;
    default:
        return MaterialPrerequisite::None;
    }
}

} // namespace

Result<void> CollectUsedPrerequisites(const MaterialBase& base,
                                      Array<MaterialPrerequisiteUse>& out) noexcept {
    const Use uses[] = {
        {!base.subsurfaceProfiles.IsEmpty(), MaterialPrerequisite::SubsurfaceProfile,
         "subsurfaceProfiles"},
        {!base.specularProfiles.IsEmpty(), MaterialPrerequisite::SpecularProfile,
         "specularProfiles"},
        {!base.toonProfiles.IsEmpty(), MaterialPrerequisite::ToonShading, "toonProfiles"},
        {!base.neuralProfile.IsNull(), MaterialPrerequisite::NeuralProfile, "neuralProfile"},
        {!SameLightBake(base.lightBakeSettings, MaterialLightBakeSettings{}),
         MaterialPrerequisite::LightBake, "lightBakeSettings"},
    };
    return AddUses(out, uses);
}

Result<void> CollectUsedPrerequisites(const Material& material,
                                      Array<MaterialPrerequisiteUse>& out) noexcept {
    if (auto base = CollectUsedPrerequisites(static_cast<const MaterialBase&>(material), out);
        !base) {
        return base;
    }

    const BlendMode blend = material.traits.blendMode;
    const MaterialPrerequisite blendPrerequisite = BlendModePrerequisite(blend);
    const Use uses[] = {
        {!material.physicalMaterial.IsNull(), MaterialPrerequisite::PhysicalMaterial,
         "physicalMaterial"},
        {!material.physicalMaterialMask.IsNull(), MaterialPrerequisite::PhysicalMaterial,
         "physicalMaterialMask"},
        {!material.renderTracePhysicalMaterials.IsEmpty(), MaterialPrerequisite::PhysicalMaterial,
         "renderTracePhysicalMaterials"},
        {AnySet(material.physicalMaterialMap), MaterialPrerequisite::PhysicalMaterial,
         "physicalMaterialMap"},
        {!material.virtualizedGeometryOverride.IsNull(), MaterialPrerequisite::VirtualizedGeometry,
         "virtualizedGeometryOverride"},
        {!SameDisplacement(material.traits.displacement, material::MaterialDisplacementTraits{}),
         MaterialPrerequisite::VirtualizedGeometry, "traits.displacement"},
        {material.traits.relaxVirtualTextureRestrictions, MaterialPrerequisite::VirtualTexture,
         "traits.relaxVirtualTextureRestrictions"},
        {blendPrerequisite != MaterialPrerequisite::None, blendPrerequisite, "traits.blendMode"},
    };
    return AddUses(out, uses);
}

Result<void> CollectUsedPrerequisites(const MaterialInstance& instance,
                                      Array<MaterialPrerequisiteUse>& out) noexcept {
    if (auto base = CollectUsedPrerequisites(static_cast<const MaterialBase&>(instance), out);
        !base) {
        return base;
    }

    const material::MaterialTraitOverrides& traits = instance.traitOverrides;
    const bool displacement =
        traits.enableTessellation.HasValue() || traits.displacementMagnitude.HasValue() ||
        traits.displacementCenter.HasValue() || traits.enableDisplacementFade.HasValue() ||
        traits.displacementFadeStartSizePixels.HasValue() ||
        traits.displacementFadeEndSizePixels.HasValue();
    const MaterialLightBakeOverrides& bake = instance.lightBakeOverrides;
    const bool lightBake = bake.castShadowAsMasked.HasValue() || bake.emissiveBoost.HasValue() ||
                           bake.diffuseBoost.HasValue() || bake.exportResolutionScale.HasValue();
    const MaterialPrerequisite blendPrerequisite =
        traits.blendMode.HasValue() ? BlendModePrerequisite(traits.blendMode.Value())
                                    : MaterialPrerequisite::None;
    const Use uses[] = {
        {!instance.textureCollectionOverrides.IsEmpty(), MaterialPrerequisite::TextureCollection,
         "textureCollectionOverrides"},
        {!instance.virtualTextureOverrides.IsEmpty(), MaterialPrerequisite::VirtualTexture,
         "virtualTextureOverrides"},
        {!instance.sparseVolumeTextureOverrides.IsEmpty(),
         MaterialPrerequisite::SparseVolumeTexture, "sparseVolumeTextureOverrides"},
        {!instance.fontOverrides.IsEmpty(), MaterialPrerequisite::Font, "fontOverrides"},
        {HasOverride(instance.physicalMaterialOverride), MaterialPrerequisite::PhysicalMaterial,
         "physicalMaterialOverride"},
        {HasOverride(instance.physicalMaterialMaskOverride), MaterialPrerequisite::PhysicalMaterial,
         "physicalMaterialMaskOverride"},
        {AnySet(instance.physicalMaterialMapOverride), MaterialPrerequisite::PhysicalMaterial,
         "physicalMaterialMapOverride"},
        {HasOverride(instance.virtualizedGeometryOverride),
         MaterialPrerequisite::VirtualizedGeometry, "virtualizedGeometryOverride"},
        {displacement, MaterialPrerequisite::VirtualizedGeometry, "traitOverrides.displacement"},
        {HasOverride(instance.subsurfaceProfileOverride), MaterialPrerequisite::SubsurfaceProfile,
         "subsurfaceProfileOverride"},
        {HasOverride(instance.specularProfileOverride), MaterialPrerequisite::SpecularProfile,
         "specularProfileOverride"},
        {HasOverride(instance.toonProfileOverride), MaterialPrerequisite::ToonShading,
         "toonProfileOverride"},
        {lightBake, MaterialPrerequisite::LightBake, "lightBakeOverrides"},
        {blendPrerequisite != MaterialPrerequisite::None, blendPrerequisite,
         "traitOverrides.blendMode"},
    };
    return AddUses(out, uses);
}

Result<void> CollectUsedPrerequisites(const MaterialFunctionInstance& instance,
                                      Array<MaterialPrerequisiteUse>& out) noexcept {
    const Use uses[] = {
        {!instance.textureCollectionOverrides.IsEmpty(), MaterialPrerequisite::TextureCollection,
         "textureCollectionOverrides"},
        {!instance.virtualTextureOverrides.IsEmpty(), MaterialPrerequisite::VirtualTexture,
         "virtualTextureOverrides"},
        {!instance.sparseVolumeTextureOverrides.IsEmpty(),
         MaterialPrerequisite::SparseVolumeTexture, "sparseVolumeTextureOverrides"},
        {!instance.fontOverrides.IsEmpty(), MaterialPrerequisite::Font, "fontOverrides"},
    };
    return AddUses(out, uses);
}

} // namespace rocks
