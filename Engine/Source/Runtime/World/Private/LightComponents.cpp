#include <Core/Math/MathUtility.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/DirectionalLightComponent.generated.h>
#include <World/DirectionalLightComponent.h>
#include <World/LightComponent.generated.h>
#include <World/LightComponent.h>
#include <World/LightingChannels.generated.h>
#include <World/PointLightComponent.generated.h>
#include <World/PointLightComponent.h>
#include <World/RectLightComponent.generated.h>
#include <World/RectLightComponent.h>
#include <World/SceneComponent.generated.h>
#include <World/SkyLightComponent.generated.h>
#include <World/SkyLightComponent.h>

// 광원 컴포넌트다(Docs/Design/EngineRuntime 24.2절, D1). 데이터·순수 계산만 있고 렌더 연결은 RS
// 단계다.
namespace rocks {

namespace {

// UE 단위 변환 상수다(LocalLightComponent.cpp:146 — cm²↔m² 1e4, 옛 Unitless 배율 16).
constexpr float SquareCentimetersPerSquareMeter = 100.0f * 100.0f;
constexpr float LegacyUnitlessScale = 16.0f;
// 원뿔 cos 상한 여유(UE KINDA_SMALL_NUMBER)다.
constexpr float ConeCosineMargin = 1.0e-4f;

Result<void> Invalid() noexcept {
    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

bool InRange(float value, float minimum, float maximum) noexcept {
    return math::IsFinite(value) && value >= minimum && value <= maximum;
}

bool NonNegative(float value) noexcept {
    return math::IsFinite(value) && value >= 0.0f;
}

bool IsKnownUnits(LightUnits units) noexcept {
    return static_cast<uint8>(units) <= static_cast<uint8>(LightUnits::Nits);
}

// UE EV100ToLuminance(EV100) = 2^EV100(LuminanceMax 1 — RenderUtils.h:744).
float Ev100ToLuminance(float ev100) noexcept {
    return platform::PlatformMath::Pow(2.0f, ev100);
}

// 1/(2π(1 − cos))다(원뿔 입체각의 역수). cos는 이미 잘린 값이다.
float InverseConeSolidAngle(float cosHalfConeAngle) noexcept {
    return 1.0f / (2.0f * math::Pi<float> * (1.0f - cosHalfConeAngle));
}

} // namespace

// --- LightComponentBase ---

Result<void> LightComponentBase::SetIntensity(float intensity) noexcept {
    // UE PostEditChangeProperty(LightComponent.cpp:781): EV가 아니면 0 이상.
    if (!math::IsFinite(intensity) || (intensity < 0.0f && IntensityUnits() != LightUnits::EV)) {
        return Invalid();
    }

    return ChangeLightData(intensity_, intensity, "intensity");
}

LinearColor LightComponentBase::LinearLightColor() const noexcept {
    return LinearColor::FromSRGB(lightColor_);
}

Result<void> LightComponentBase::SetLightColor(Color color) noexcept {
    return ChangeLightData(lightColor_, color, "lightColor");
}

Result<void> LightComponentBase::SetLightColor(const LinearColor& color, bool srgb) noexcept {
    if (!color.IsFinite()) {
        return Invalid();
    }

    const Color encoded = srgb ? color.ToSRGB() : color.QuantizeRound();
    return ChangeLightData(lightColor_, encoded, "lightColor");
}

Result<void> LightComponentBase::SetAffectsWorld(bool affects) noexcept {
    if (auto allowed = CheckChangeBeforeGameInitialization("affectsWorld"); !allowed) {
        return allowed;
    }

    if (affectsWorld_ != affects) {
        affectsWorld_ = affects;
        MarkRenderStateDirtyIfPresent();
    }

    return Result<void>::Success();
}

Result<void> LightComponentBase::SetCastShadows(bool cast) noexcept {
    return ChangeLightData(castShadows_, cast, "castShadows");
}

Result<void> LightComponentBase::SetCastStaticShadows(bool cast) noexcept {
    return ChangeLightData(castStaticShadows_, cast, "castStaticShadows");
}

Result<void> LightComponentBase::SetCastDynamicShadows(bool cast) noexcept {
    return ChangeLightData(castDynamicShadows_, cast, "castDynamicShadows");
}

Result<void> LightComponentBase::SetAffectTranslucentLighting(bool affects) noexcept {
    return ChangeLightData(affectTranslucentLighting_, affects, "affectTranslucentLighting");
}

Result<void> LightComponentBase::SetTransmission(bool transmission) noexcept {
    return ChangeLightData(transmission_, transmission, "transmission");
}

Result<void> LightComponentBase::SetCastVolumetricShadow(bool cast) noexcept {
    return ChangeLightData(castVolumetricShadow_, cast, "castVolumetricShadow");
}

Result<void> LightComponentBase::SetIndirectLightingIntensity(float intensity) noexcept {
    if (!math::IsFinite(intensity)) {
        return Invalid();
    }

    return ChangeLightData(indirectLightingIntensity_, intensity, "indirectLightingIntensity");
}

Result<void> LightComponentBase::SetVolumetricScatteringIntensity(float intensity) noexcept {
    if (!math::IsFinite(intensity)) {
        return Invalid();
    }

    return ChangeLightData(volumetricScatteringIntensity_, intensity,
                           "volumetricScatteringIntensity");
}

bool LightComponentBase::HasStaticLighting() const noexcept {
    return Mobility() == ComponentMobility::Static && !GetOwner().IsNull();
}

bool LightComponentBase::HasStaticShadowing() const noexcept {
    return Mobility() != ComponentMobility::Movable && !GetOwner().IsNull();
}

// --- LightComponent ---

Vec3d LightComponent::Direction() const noexcept {
    // UE GetComponentTransform().GetUnitAxis(X): 월드 행렬 0행(scale 포함)을 정규화한다.
    auto axis = WorldMatrix().GetScaledAxis(0).TryNormalize();
    return axis ? axis.Value() : Vec3d(1.0, 0.0, 0.0);
}

Result<void> LightComponent::SetTemperature(float kelvin) noexcept {
    if (!math::IsFinite(kelvin)) {
        return Invalid();
    }

    return ChangeLightData(temperature_, kelvin, "temperature");
}

Result<void> LightComponent::SetUseTemperature(bool use) noexcept {
    return ChangeLightData(useTemperature_, use, "useTemperature");
}

Result<void> LightComponent::SetMaxDrawDistance(float distance) noexcept {
    if (!math::IsFinite(distance)) {
        return Invalid();
    }

    return ChangeLightData(maxDrawDistance_, distance, "maxDrawDistance");
}

Result<void> LightComponent::SetMaxDistanceFadeRange(float range) noexcept {
    if (!math::IsFinite(range)) {
        return Invalid();
    }

    return ChangeLightData(maxDistanceFadeRange_, range, "maxDistanceFadeRange");
}

Result<void> LightComponent::SetSpecularScale(float scale) noexcept {
    // UE PostEditChangeProperty clamp [0, 1](LightComponent.cpp:785).
    if (!InRange(scale, 0.0f, 1.0f)) {
        return Invalid();
    }

    return ChangeLightData(specularScale_, scale, "specularScale");
}

Result<void> LightComponent::SetDiffuseScale(float scale) noexcept {
    if (!InRange(scale, 0.0f, 1.0f)) {
        return Invalid();
    }

    return ChangeLightData(diffuseScale_, scale, "diffuseScale");
}

Result<void> LightComponent::SetShadowResolutionScale(float scale) noexcept {
    if (!NonNegative(scale)) {
        return Invalid();
    }

    return ChangeLightData(shadowResolutionScale_, scale, "shadowResolutionScale");
}

Result<void> LightComponent::SetShadowBias(float bias) noexcept {
    if (!math::IsFinite(bias) || (LimitsShadowBias() && !InRange(bias, 0.0f, MaxShadowBias))) {
        return Invalid();
    }

    return ChangeLightData(shadowBias_, bias, "shadowBias");
}

Result<void> LightComponent::SetShadowSlopeBias(float bias) noexcept {
    if (!math::IsFinite(bias) || (LimitsShadowBias() && !InRange(bias, 0.0f, MaxShadowBias))) {
        return Invalid();
    }

    return ChangeLightData(shadowSlopeBias_, bias, "shadowSlopeBias");
}

Result<void> LightComponent::SetShadowSharpen(float sharpen) noexcept {
    if (!math::IsFinite(sharpen)) {
        return Invalid();
    }

    return ChangeLightData(shadowSharpen_, sharpen, "shadowSharpen");
}

Result<void> LightComponent::SetContactShadowLength(float length) noexcept {
    if (!NonNegative(length)) {
        return Invalid();
    }

    return ChangeLightData(contactShadowLength_, length, "contactShadowLength");
}

Result<void> LightComponent::SetContactShadowLengthInWorldSpace(bool worldSpace) noexcept {
    return ChangeLightData(contactShadowLengthInWorldSpace_, worldSpace,
                           "contactShadowLengthInWorldSpace");
}

Result<void> LightComponent::SetCastTranslucentShadows(bool cast) noexcept {
    return ChangeLightData(castTranslucentShadows_, cast, "castTranslucentShadows");
}

Result<void> LightComponent::SetLightingChannels(const rocks::LightingChannels& channels) noexcept {
    // UE SetLightingChannels는 Mobility를 보지 않는다(LightComponent.cpp:1366).
    if (!(lightingChannels_ == channels)) {
        lightingChannels_ = channels;
        MarkRenderStateDirtyIfPresent();
    }

    return Result<void>::Success();
}

// --- LocalLightComponent ---

Result<void> LocalLightComponent::SetIntensityUnits(LightUnits units) noexcept {
    if (!IsKnownUnits(units) || !AcceptsIntensityUnits(units)) {
        return Invalid();
    }

    if (auto changed = ChangeLightData(intensityUnits_, units, "intensityUnits"); !changed) {
        return changed;
    }

    ClampIntensityForUnits();
    return Result<void>::Success();
}

void LocalLightComponent::ClampIntensityForUnits() noexcept {
    // UE PostEditChangeProperty(LightComponent.cpp:781): EV가 아니면 세기를 0 이상으로 둔다.
    if (IntensityUnits() != LightUnits::EV && intensity_ < 0.0f) {
        intensity_ = 0.0f;
    }
}

Result<void> LocalLightComponent::SetAttenuationRadius(float radius) noexcept {
    // 음수 반경은 경계 구가 뒤집힌다(UE UIMin 8 — 거부는 0 미만만).
    if (!NonNegative(radius)) {
        return Invalid();
    }

    // UE SetAttenuationRadius는 AreDynamicDataChangesAllowed(false)다(Stationary도 막는다).
    return ChangeLightData(attenuationRadius_, radius, "attenuationRadius", true);
}

Result<void> LocalLightComponent::SetInverseExposureBlend(float blend) noexcept {
    if (!InRange(blend, 0.0f, 1.0f)) {
        return Invalid();
    }

    return ChangeLightData(inverseExposureBlend_, blend, "inverseExposureBlend");
}

float LocalLightComponent::UnitsConversionFactor(LightUnits from, LightUnits to,
                                                 float cosHalfConeAngle) noexcept {
    // UE GetUnitsConversionFactor(LocalLightComponent.cpp:146–225)와 같은 식·순서다.
    cosHalfConeAngle = math::Clamp(cosHalfConeAngle, -1.0f, 1.0f - ConeCosineMargin);
    if (from == to) {
        return 1.0f;
    }

    float factor = 1.0f;
    switch (from) {
    case LightUnits::Nits:
        break;
    case LightUnits::Candelas:
    case LightUnits::EV:
        factor = SquareCentimetersPerSquareMeter;
        break;
    case LightUnits::Lumens:
        factor = SquareCentimetersPerSquareMeter * InverseConeSolidAngle(cosHalfConeAngle);
        break;
    case LightUnits::Unitless:
    default:
        factor = LegacyUnitlessScale;
        break;
    }

    switch (to) {
    case LightUnits::Nits:
        break;
    case LightUnits::Candelas:
    case LightUnits::EV:
        factor *= 1.0f / SquareCentimetersPerSquareMeter;
        break;
    case LightUnits::Lumens:
        factor *=
            2.0f * math::Pi<float> * (1.0f - cosHalfConeAngle) / SquareCentimetersPerSquareMeter;
        break;
    case LightUnits::Unitless:
    default:
        factor *= 1.0f / LegacyUnitlessScale;
        break;
    }

    return factor;
}

Sphere<double> LocalLightComponent::BoundingSphere() const noexcept {
    return Sphere<double>(WorldLocation(), static_cast<double>(attenuationRadius_));
}

bool LocalLightComponent::AffectsBounds(const ComponentBounds& bounds) const noexcept {
    const double reach = static_cast<double>(attenuationRadius_) + bounds.sphereRadius;
    return (bounds.origin - WorldLocation()).LengthSquared() <= reach * reach;
}

// --- PointLightComponent ---

float PointLightComponent::BrightnessWithUnits(float lumensFactor) const noexcept {
    // UE PointLightComponent.cpp:127–157(Spot은 lumensFactor만 다르다).
    float brightness = intensity_;
    if (!useInverseSquaredFalloff_) {
        return brightness;
    }

    switch (intensityUnits_) {
    case LightUnits::Candelas:
        return brightness * SquareCentimetersPerSquareMeter;
    case LightUnits::Nits:
        // 캡슐 면적(구 + 원통, cm²)이다.
        return brightness *
               (4.0f * math::Pi<float> * sourceRadius_ * (sourceRadius_ + 0.5f * sourceLength_));
    case LightUnits::Lumens:
        return brightness * SquareCentimetersPerSquareMeter * lumensFactor;
    case LightUnits::EV:
        return Ev100ToLuminance(brightness) * SquareCentimetersPerSquareMeter;
    case LightUnits::Unitless:
    default:
        return brightness * LegacyUnitlessScale;
    }
}

float PointLightComponent::ComputeLightBrightness() const noexcept {
    return BrightnessWithUnits(1.0f / (4.0f * math::Pi<float>));
}

Result<void> PointLightComponent::SetUseInverseSquaredFalloff(bool use) noexcept {
    if (auto allowed = CheckDynamicDataChange("useInverseSquaredFalloff", false); !allowed) {
        return allowed;
    }

    if (useInverseSquaredFalloff_ == use) {
        return Result<void>::Success();
    }

    // UE: 1/r²가 꺼지면 단위가 Unitless로 돌아간다(PointLightComponent.cpp:230·270).
    useInverseSquaredFalloff_ = use;
    if (!use) {
        intensityUnits_ = LightUnits::Unitless;
        ClampIntensityForUnits();
    }

    MarkRenderStateDirtyIfPresent();
    return Result<void>::Success();
}

Result<void> PointLightComponent::SetLightFalloffExponent(float exponent) noexcept {
    if (!math::IsFinite(exponent) || exponent < MinLightFalloffExponent) {
        return Invalid();
    }

    return ChangeLightData(lightFalloffExponent_, exponent, "lightFalloffExponent");
}

Result<void> PointLightComponent::SetSourceRadius(float radius) noexcept {
    if (!InRange(radius, 0.0f, MaxSourceSize)) {
        return Invalid();
    }

    return ChangeLightData(sourceRadius_, radius, "sourceRadius");
}

Result<void> PointLightComponent::SetSoftSourceRadius(float radius) noexcept {
    if (!InRange(radius, 0.0f, MaxSourceSize)) {
        return Invalid();
    }

    return ChangeLightData(softSourceRadius_, radius, "softSourceRadius");
}

Result<void> PointLightComponent::SetSourceLength(float length) noexcept {
    if (!InRange(length, 0.0f, MaxSourceSize)) {
        return Invalid();
    }

    return ChangeLightData(sourceLength_, length, "sourceLength");
}

// --- SpotLightComponent ---

float SpotLightComponent::ComputeLightBrightness() const noexcept {
    return BrightnessWithUnits(InverseConeSolidAngle(CosHalfConeAngle()));
}

Result<void> SpotLightComponent::SetInnerConeAngle(float degrees) noexcept {
    if (!math::IsFinite(degrees)) {
        return Invalid();
    }

    return ChangeLightData(innerConeAngle_, degrees, "innerConeAngle", true);
}

Result<void> SpotLightComponent::SetOuterConeAngle(float degrees) noexcept {
    if (!math::IsFinite(degrees)) {
        return Invalid();
    }

    return ChangeLightData(outerConeAngle_, degrees, "outerConeAngle", true);
}

SpotLightComponent::ConeAngles SpotLightComponent::ClampedConeAngles() const noexcept {
    // UE GetClampedConeAngles(SpotLightComponent.cpp:35–41).
    const float maxRadians = math::DegreesToRadians(MaxConeAngle);
    ConeAngles angles;
    angles.innerRadians = math::Clamp(math::DegreesToRadians(innerConeAngle_), 0.0f, maxRadians);
    angles.outerRadians = math::Clamp(math::DegreesToRadians(outerConeAngle_),
                                      angles.innerRadians + MinConeAngleGapRadians,
                                      maxRadians + MinConeAngleGapRadians);
    return angles;
}

float SpotLightComponent::CosHalfConeAngle() const noexcept {
    return math::Cos(HalfConeAngle());
}

// --- RectLightComponent ---

float RectLightComponent::ComputeLightBrightness() const noexcept {
    // UE RectLightComponent.cpp:181–219(옛 EV 오식 호환 bLightRequiresBrokenEVMath는 없다).
    const float brightness = intensity_;
    switch (intensityUnits_) {
    case LightUnits::Candelas:
        return brightness * SquareCentimetersPerSquareMeter;
    case LightUnits::Nits:
        return brightness * (sourceWidth_ * sourceHeight_);
    case LightUnits::Lumens:
        return brightness * (SquareCentimetersPerSquareMeter / math::Pi<float>);
    case LightUnits::EV:
        return Ev100ToLuminance(brightness) * SquareCentimetersPerSquareMeter;
    case LightUnits::Unitless:
    default:
        return brightness * LegacyUnitlessScale;
    }
}

Result<void> RectLightComponent::SetSourceWidth(float width) noexcept {
    if (!InRange(width, MinSourceSize, MaxSourceSize)) {
        return Invalid();
    }

    return ChangeLightData(sourceWidth_, width, "sourceWidth");
}

Result<void> RectLightComponent::SetSourceHeight(float height) noexcept {
    if (!InRange(height, MinSourceSize, MaxSourceSize)) {
        return Invalid();
    }

    return ChangeLightData(sourceHeight_, height, "sourceHeight");
}

Result<void> RectLightComponent::SetBarnDoorAngle(float degrees) noexcept {
    if (!InRange(degrees, 0.0f, MaxBarnDoorAngle)) {
        return Invalid();
    }

    return ChangeLightData(barnDoorAngle_, degrees, "barnDoorAngle");
}

Result<void> RectLightComponent::SetBarnDoorLength(float length) noexcept {
    if (!math::IsFinite(length) || length < MinBarnDoorLength) {
        return Invalid();
    }

    return ChangeLightData(barnDoorLength_, length, "barnDoorLength");
}

// --- DirectionalLightComponent ---

Result<void> DirectionalLightComponent::SetLightSourceAngle(float degrees) noexcept {
    if (!NonNegative(degrees)) {
        return Invalid();
    }

    return ChangeLightData(lightSourceAngle_, degrees, "lightSourceAngle");
}

Result<void> DirectionalLightComponent::SetLightSourceSoftAngle(float degrees) noexcept {
    if (!NonNegative(degrees)) {
        return Invalid();
    }

    return ChangeLightData(lightSourceSoftAngle_, degrees, "lightSourceSoftAngle");
}

Result<void> DirectionalLightComponent::SetShadowAmount(float amount) noexcept {
    if (!InRange(amount, 0.0f, 1.0f)) {
        return Invalid();
    }

    return ChangeLightData(shadowAmount_, amount, "shadowAmount");
}

Result<void>
DirectionalLightComponent::SetDynamicShadowDistanceMovableLight(float distance) noexcept {
    if (!NonNegative(distance)) {
        return Invalid();
    }

    return ChangeLightData(dynamicShadowDistanceMovableLight_, distance,
                           "dynamicShadowDistanceMovableLight");
}

Result<void>
DirectionalLightComponent::SetDynamicShadowDistanceStationaryLight(float distance) noexcept {
    if (!NonNegative(distance)) {
        return Invalid();
    }

    return ChangeLightData(dynamicShadowDistanceStationaryLight_, distance,
                           "dynamicShadowDistanceStationaryLight");
}

Result<void> DirectionalLightComponent::SetDynamicShadowCascades(int32 cascades) noexcept {
    if (cascades < 0 || cascades > MaxShadowCascades) {
        return Invalid();
    }

    return ChangeLightData(dynamicShadowCascades_, cascades, "dynamicShadowCascades");
}

Result<void> DirectionalLightComponent::SetCascadeDistributionExponent(float exponent) noexcept {
    if (!InRange(exponent, MinCascadeDistributionExponent, MaxCascadeDistributionExponent)) {
        return Invalid();
    }

    return ChangeLightData(cascadeDistributionExponent_, exponent, "cascadeDistributionExponent");
}

Result<void> DirectionalLightComponent::SetCascadeTransitionFraction(float fraction) noexcept {
    if (!InRange(fraction, MinCascadeTransitionFraction, MaxCascadeTransitionFraction)) {
        return Invalid();
    }

    return ChangeLightData(cascadeTransitionFraction_, fraction, "cascadeTransitionFraction");
}

Result<void> DirectionalLightComponent::SetShadowDistanceFadeoutFraction(float fraction) noexcept {
    if (!InRange(fraction, 0.0f, 1.0f)) {
        return Invalid();
    }

    return ChangeLightData(shadowDistanceFadeoutFraction_, fraction,
                           "shadowDistanceFadeoutFraction");
}

Result<void> DirectionalLightComponent::SetFarShadowCascadeCount(int32 count) noexcept {
    if (count < 0 || count > MaxShadowCascades) {
        return Invalid();
    }

    return ChangeLightData(farShadowCascadeCount_, count, "farShadowCascadeCount");
}

Result<void> DirectionalLightComponent::SetFarShadowDistance(float distance) noexcept {
    if (!NonNegative(distance)) {
        return Invalid();
    }

    return ChangeLightData(farShadowDistance_, distance, "farShadowDistance");
}

Result<void> DirectionalLightComponent::SetForwardShadingPriority(int32 priority) noexcept {
    if (priority < 0) {
        return Invalid();
    }

    return ChangeLightData(forwardShadingPriority_, priority, "forwardShadingPriority");
}

Result<void> DirectionalLightComponent::SetAtmosphereSunLight(bool sun) noexcept {
    return ChangeLightData(atmosphereSunLight_, sun, "atmosphereSunLight");
}

Result<void> DirectionalLightComponent::SetAtmosphereSunLightIndex(int32 index) noexcept {
    if (index < 0 || index > MaxAtmosphereSunLightIndex) {
        return Invalid();
    }

    return ChangeLightData(atmosphereSunLightIndex_, index, "atmosphereSunLightIndex");
}

Result<void>
DirectionalLightComponent::SetAtmosphereSunDiskColorScale(const LinearColor& scale) noexcept {
    if (!scale.IsFinite()) {
        return Invalid();
    }

    return ChangeLightData(atmosphereSunDiskColorScale_, scale, "atmosphereSunDiskColorScale");
}

// --- SkyLightComponent ---

Result<void> SkyLightComponent::SetRealTimeCapture(bool realTime) noexcept {
    return ChangeLightData(realTimeCapture_, realTime, "realTimeCapture");
}

Result<void> SkyLightComponent::SetSourceType(SkyLightSourceType type) noexcept {
    if (static_cast<uint8>(type) > static_cast<uint8>(SkyLightSourceType::SpecifiedCubemap)) {
        return Invalid();
    }

    return ChangeLightData(sourceType_, type, "sourceType");
}

Result<void> SkyLightComponent::SetSourceCubemapAngle(float degrees) noexcept {
    // UE는 범위를 자르지 않는다(UIMin 0·UIMax 360은 편집기 슬라이더 범위).
    if (!math::IsFinite(degrees)) {
        return Invalid();
    }

    return ChangeLightData(sourceCubemapAngle_, degrees, "sourceCubemapAngle");
}

Result<void> SkyLightComponent::SetCubemapResolution(int32 resolution) noexcept {
    if (resolution < 1) {
        return Invalid();
    }

    // UE SanitizeCubemapSize: 2의 거듭제곱으로 올린 뒤 [8, 최대]로 자른다. 최대를 넘는 입력은 올림
    // 전에 잘라 넘침을 막는다(결과는 같다 — 최대가 2의 거듭제곱이다).
    const int32 wanted = resolution < MaxCubemapResolution ? resolution : MaxCubemapResolution;
    int32 sanitized = MinCubemapResolution;
    while (sanitized < wanted) {
        sanitized <<= 1;
    }

    return ChangeLightData(cubemapResolution_, sanitized, "cubemapResolution");
}

Result<void> SkyLightComponent::SetSkyDistanceThreshold(float distance) noexcept {
    if (!NonNegative(distance)) {
        return Invalid();
    }

    return ChangeLightData(skyDistanceThreshold_, distance, "skyDistanceThreshold");
}

Result<void> SkyLightComponent::SetCaptureEmissiveOnly(bool emissiveOnly) noexcept {
    return ChangeLightData(captureEmissiveOnly_, emissiveOnly, "captureEmissiveOnly");
}

Result<void> SkyLightComponent::SetLowerHemisphereIsBlack(bool black) noexcept {
    return ChangeLightData(lowerHemisphereIsBlack_, black, "lowerHemisphereIsBlack");
}

Result<void> SkyLightComponent::SetLowerHemisphereColor(const LinearColor& color) noexcept {
    if (!color.IsFinite()) {
        return Invalid();
    }

    return ChangeLightData(lowerHemisphereColor_, color, "lowerHemisphereColor");
}

} // namespace rocks
