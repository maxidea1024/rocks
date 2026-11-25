#include <MaterialSchema/MaterialPrerequisite.h>

// 머티리얼 층 선행 ID 표다(Docs/Design/MaterialSystem 19.4.1절).
namespace rocks::material {

namespace {

// MaterialPrerequisite 값 순서와 같다(None은 빈 문자열).
constexpr StringView PrerequisiteIds[MaterialPrerequisiteCount] = {
    "",
    "MPQ-TextureCollection",
    "MPQ-SparseVolumeTexture",
    "MPQ-VirtualTexture",
    "MPQ-ExternalTexture",
    "MPQ-Font",
    "MPQ-CurveAsset",
    "MPQ-MaterialAggregate",
    "MPQ-SubsurfaceProfile",
    "MPQ-SpecularProfile",
    "MPQ-ToonShading",
    "MPQ-NeuralProfile",
    "MPQ-PhysicalMaterial",
    "MPQ-VirtualizedGeometry",
    "MPQ-ColoredTransmittance",
    "MPQ-AlphaHoldout",
    "MPQ-LightBake",
};

static_assert(static_cast<uint32>(MaterialPrerequisite::LightBake) + 1 == MaterialPrerequisiteCount,
              "MaterialPrerequisiteCount must follow the last MaterialPrerequisite value");

} // namespace

StringView MaterialPrerequisiteId(MaterialPrerequisite prerequisite) noexcept {
    const auto index = static_cast<uint32>(prerequisite);
    return index < MaterialPrerequisiteCount ? PrerequisiteIds[index] : StringView();
}

} // namespace rocks::material
