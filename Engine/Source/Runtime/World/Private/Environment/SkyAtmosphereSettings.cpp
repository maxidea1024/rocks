#include <Core/Math/MathUtility.h>
#include <World/Environment/SkyAtmosphereSettings.h>
#include "../ComponentValueChecks.h"

// 대기 설정 범위 검사다(Docs/Design/EngineRuntime 27.2절, D5).
namespace rocks {

namespace {

// UE 범위 상수다(SkyAtmosphereComponent.h ClampMin·ClampMax — km·도).
constexpr float SkyMinBottomRadiusKm = 0.1f;
constexpr float SkyMaxBottomRadiusKm = 10000.0f;
constexpr float SkyMinAtmosphereHeightKm = 0.1f;
constexpr float SkyMaxMultiScatteringFactor = 100.0f;
constexpr float SkyMinTraceSampleCountScale = 0.25f;
constexpr float SkyMinExponentialDistributionKm = 0.001f;
constexpr float SkyMaxMieAnisotropy = 0.999f;
constexpr float SkyMaxElevationAngleDegrees = 90.0f;
constexpr float SkyMinAerialPerspectiveStartDepthKm = 0.001f;

} // namespace

bool IsValidSkyAtmosphereSettings(const SkyAtmosphereSettings& s) noexcept {
    using component_checks::AtLeast;
    using component_checks::InRange;
    return static_cast<uint8>(s.transformMode) <=
               static_cast<uint8>(SkyAtmosphereTransformMode::PlanetCenterAtComponentTransform) &&
           InRange(s.bottomRadius, SkyMinBottomRadiusKm, SkyMaxBottomRadiusKm) &&
           AtLeast(s.atmosphereHeight, SkyMinAtmosphereHeightKm) &&
           InRange(s.multiScatteringFactor, 0.0f, SkyMaxMultiScatteringFactor) &&
           AtLeast(s.traceSampleCountScale, SkyMinTraceSampleCountScale) &&
           AtLeast(s.rayleighScatteringScale, 0.0f) && s.rayleighScattering.IsFinite() &&
           AtLeast(s.rayleighExponentialDistribution, SkyMinExponentialDistributionKm) &&
           AtLeast(s.mieScatteringScale, 0.0f) && s.mieScattering.IsFinite() &&
           AtLeast(s.mieAbsorptionScale, 0.0f) && s.mieAbsorption.IsFinite() &&
           InRange(s.mieAnisotropy, 0.0f, SkyMaxMieAnisotropy) &&
           AtLeast(s.mieExponentialDistribution, SkyMinExponentialDistributionKm) &&
           AtLeast(s.otherAbsorptionScale, 0.0f) && s.otherAbsorption.IsFinite() &&
           math::IsFinite(s.otherTentDistribution.tipAltitude) &&
           AtLeast(s.otherTentDistribution.tipValue, 0.0f) &&
           AtLeast(s.otherTentDistribution.width, 0.0f) && s.skyLuminanceFactor.IsFinite() &&
           s.skyAndAerialPerspectiveLuminanceFactor.IsFinite() &&
           AtLeast(s.aerialPerspectiveViewDistanceScale, 0.0f) &&
           AtLeast(s.heightFogContribution, 0.0f) &&
           InRange(s.transmittanceMinLightElevationAngle, -SkyMaxElevationAngleDegrees,
                   SkyMaxElevationAngleDegrees) &&
           AtLeast(s.aerialPerspectiveStartDepth, SkyMinAerialPerspectiveStartDepthKm);
}

} // namespace rocks
