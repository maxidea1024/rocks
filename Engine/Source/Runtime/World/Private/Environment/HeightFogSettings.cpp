#include <Core/Math/MathUtility.h>
#include <World/Environment/HeightFogSettings.h>
#include "../ComponentValueChecks.h"

// 높이 안개 설정 범위 검사다(Docs/Design/EngineRuntime 27.3절, D5).
namespace rocks {

namespace {

// UE 범위 상수다(ExponentialHeightFogComponent.cpp:243–252의 PostEditChangeProperty clamp와
// 헤더 ClampMin·ClampMax). 범위가 UIMin뿐인 값은 유한이기만 하면 된다.
constexpr float FogMaxDensity = 10.0f;
constexpr float FogMaxHeightFalloff = 2.0f;
constexpr float FogMaxCubemapAngleDegrees = 360.0f;
// UE WORLD_MAX(UE5 큰 월드 — EngineDefines.h:41 UE_LARGE_WORLD_MAX, cm)와 끝·잘림 거리
// 배율(10)이다.
constexpr float FogWorldMax = 8796093022208.0f;
constexpr float FogFarDistanceScale = 10.0f;

// UE FExponentialHeightFogData::ClampToValidRanges — 첫 겹과 같은 범위다.
bool ValidHeightFogData(const HeightFogData& data) noexcept {
    return component_checks::InRange(data.fogDensity, 0.0f, FogMaxDensity) &&
           component_checks::InRange(data.fogHeightFalloff, 0.0f, FogMaxHeightFalloff) &&
           math::IsFinite(data.fogHeightOffset);
}

} // namespace

bool IsValidHeightFogSettings(const HeightFogSettings& s) noexcept {
    using component_checks::AtLeast;
    using component_checks::InRange;
    return InRange(s.fogDensity, 0.0f, FogMaxDensity) &&
           InRange(s.fogHeightFalloff, 0.0f, FogMaxHeightFalloff) &&
           ValidHeightFogData(s.secondFogData) && s.fogInscatteringLuminance.IsFinite() &&
           s.skyAtmosphereAmbientContributionColorScale.IsFinite() &&
           AtLeast(s.skyLightCaptureAffectsHeightFogStrength, 0.0f) &&
           InRange(s.skyLightCaptureAffectsHeightFogRoughness, 0.0f, 1.0f) &&
           InRange(s.inscatteringColorCubemapAngle, 0.0f, FogMaxCubemapAngleDegrees) &&
           s.inscatteringTextureTint.IsFinite() &&
           InRange(s.fullyDirectionalInscatteringColorDistance, 0.0f, FogWorldMax) &&
           InRange(s.nonDirectionalInscatteringColorDistance, 0.0f,
                   s.fullyDirectionalInscatteringColorDistance) &&
           math::IsFinite(s.directionalInscatteringExponent) &&
           math::IsFinite(s.directionalInscatteringStartDistance) &&
           s.directionalInscatteringLuminance.IsFinite() && InRange(s.fogMaxOpacity, 0.0f, 1.0f) &&
           InRange(s.startDistance, 0.0f, FogWorldMax) &&
           InRange(s.endDistance, 0.0f, FogFarDistanceScale * FogWorldMax) &&
           InRange(s.fogCutoffDistance, 0.0f, FogFarDistanceScale * FogWorldMax) &&
           // 볼륨 안개 값은 UE에 UIMin뿐이다.
           math::IsFinite(s.volumetricFogScatteringDistribution) &&
           s.volumetricFogEmissive.IsFinite() && math::IsFinite(s.volumetricFogExtinctionScale) &&
           math::IsFinite(s.volumetricFogDistance) &&
           math::IsFinite(s.volumetricFogStartDistance) &&
           math::IsFinite(s.volumetricFogNearFadeInDistance) &&
           math::IsFinite(s.volumetricFogStaticLightingScatteringIntensity);
}

} // namespace rocks
