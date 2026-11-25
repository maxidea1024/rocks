#include <Material/Material.h>
#include <Material/MaterialInstance.h>
#include <Material/MaterialPrerequisiteCheck.h>

// 에셋 TODO 필드 검사다(Docs/Design/MaterialSystem 19.4.1·19.9·19.11절).
namespace rocks {

namespace {

using material::MaterialPrerequisite;

Result<void> Add(Array<MaterialPrerequisiteUse>& out, MaterialPrerequisite prerequisite,
                 StringView field) noexcept {
    return out.TryPushBack(MaterialPrerequisiteUse{prerequisite, field});
}

Result<void> AddIf(Array<MaterialPrerequisiteUse>& out, bool used,
                   MaterialPrerequisite prerequisite, StringView field) noexcept {
    return used ? Add(out, prerequisite, field) : Result<void>::Success();
}

bool SameDisplacement(const material::MaterialDisplacementTraits& left,
                      const material::MaterialDisplacementTraits& right) noexcept {
    return left.enableTessellation == right.enableTessellation &&
           left.magnitude == right.magnitude && left.center == right.center &&
           left.enableFade == right.enableFade &&
           left.fadeStartSizePixels == right.fadeStartSizePixels &&
           left.fadeEndSizePixels == right.fadeEndSizePixels;
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

Result<void> AddBlendMode(Array<MaterialPrerequisiteUse>& out, BlendMode mode,
                          StringView field) noexcept {
    const MaterialPrerequisite prerequisite = BlendModePrerequisite(mode);
    return AddIf(out, prerequisite != MaterialPrerequisite::None, prerequisite, field);
}

} // namespace

Result<void> CollectUsedPrerequisites(const MaterialBase& base,
                                      Array<MaterialPrerequisiteUse>& out) noexcept {
    if (auto added = AddIf(out, !base.subsurfaceProfiles.IsEmpty(),
                           MaterialPrerequisite::SubsurfaceProfile, "subsurfaceProfiles");
        !added) {
        return added;
    }

    if (auto added = AddIf(out, !base.specularProfiles.IsEmpty(),
                           MaterialPrerequisite::SpecularProfile, "specularProfiles");
        !added) {
        return added;
    }

    if (auto added = AddIf(out, !base.toonProfiles.IsEmpty(), MaterialPrerequisite::ToonShading,
                           "toonProfiles");
        !added) {
        return added;
    }

    return AddIf(out, !base.neuralProfile.IsNull(), MaterialPrerequisite::NeuralProfile,
                 "neuralProfile");
}

Result<void> CollectUsedPrerequisites(const Material& material,
                                      Array<MaterialPrerequisiteUse>& out) noexcept {
    if (auto base = CollectUsedPrerequisites(static_cast<const MaterialBase&>(material), out);
        !base) {
        return base;
    }

    const bool physical = !material.physicalMaterial.IsNull() ||
                          !material.physicalMaterialMask.IsNull() ||
                          !material.renderTracePhysicalMaterials.IsEmpty();
    if (auto added =
            AddIf(out, physical, MaterialPrerequisite::PhysicalMaterial, "physicalMaterial");
        !added) {
        return added;
    }

    if (auto added =
            AddIf(out, !material.virtualizedGeometryOverride.IsNull(),
                  MaterialPrerequisite::VirtualizedGeometry, "virtualizedGeometryOverride");
        !added) {
        return added;
    }

    if (auto added = AddIf(
            out,
            !SameDisplacement(material.traits.displacement, material::MaterialDisplacementTraits{}),
            MaterialPrerequisite::VirtualizedGeometry, "traits.displacement");
        !added) {
        return added;
    }

    if (auto added =
            AddIf(out, material.traits.relaxVirtualTextureRestrictions,
                  MaterialPrerequisite::VirtualTexture, "traits.relaxVirtualTextureRestrictions");
        !added) {
        return added;
    }

    return AddBlendMode(out, material.traits.blendMode, "traits.blendMode");
}

Result<void> CollectUsedPrerequisites(const MaterialInstance& instance,
                                      Array<MaterialPrerequisiteUse>& out) noexcept {
    if (auto base = CollectUsedPrerequisites(static_cast<const MaterialBase&>(instance), out);
        !base) {
        return base;
    }

    struct ArrayUse {
        bool used;
        MaterialPrerequisite prerequisite;
        StringView field;
    };

    const material::MaterialTraitOverrides& traits = instance.traitOverrides;
    const bool displacement =
        traits.enableTessellation.HasValue() || traits.displacementMagnitude.HasValue() ||
        traits.displacementCenter.HasValue() || traits.enableDisplacementFade.HasValue() ||
        traits.displacementFadeStartSizePixels.HasValue() ||
        traits.displacementFadeEndSizePixels.HasValue();
    const ArrayUse uses[] = {
        {!instance.textureCollectionOverrides.IsEmpty(), MaterialPrerequisite::TextureCollection,
         "textureCollectionOverrides"},
        {!instance.virtualTextureOverrides.IsEmpty(), MaterialPrerequisite::VirtualTexture,
         "virtualTextureOverrides"},
        {!instance.sparseVolumeTextureOverrides.IsEmpty(),
         MaterialPrerequisite::SparseVolumeTexture, "sparseVolumeTextureOverrides"},
        {!instance.fontOverrides.IsEmpty(), MaterialPrerequisite::Font, "fontOverrides"},
        {!instance.physicalMaterialOverride.IsNull() ||
             !instance.physicalMaterialMaskOverride.IsNull(),
         MaterialPrerequisite::PhysicalMaterial, "physicalMaterialOverride"},
        {!instance.virtualizedGeometryOverride.IsNull(), MaterialPrerequisite::VirtualizedGeometry,
         "virtualizedGeometryOverride"},
        {displacement, MaterialPrerequisite::VirtualizedGeometry, "traitOverrides.displacement"},
        {!instance.subsurfaceProfileOverride.IsNull(), MaterialPrerequisite::SubsurfaceProfile,
         "subsurfaceProfileOverride"},
        {!instance.specularProfileOverride.IsNull(), MaterialPrerequisite::SpecularProfile,
         "specularProfileOverride"},
        {!instance.toonProfileOverride.IsNull(), MaterialPrerequisite::ToonShading,
         "toonProfileOverride"},
    };

    for (const ArrayUse& use : uses) {
        if (auto added = AddIf(out, use.used, use.prerequisite, use.field); !added) {
            return added;
        }
    }

    if (traits.blendMode.HasValue()) {
        return AddBlendMode(out, traits.blendMode.Value(), "traitOverrides.blendMode");
    }

    return Result<void>::Success();
}

} // namespace rocks
