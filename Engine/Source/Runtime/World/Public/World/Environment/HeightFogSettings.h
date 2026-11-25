#pragma once

#include <Core/Math/Color.h>
#include <Meta/Annotations.h>

namespace rocks {

// 안개 한 겹이다(UE FExponentialHeightFogData — ExponentialHeightFogData.h:12). 높이 offset은
// 컴포넌트 Z 기준 cm다.
ROCKS_STRUCT()
struct HeightFogData {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() float fogDensity = 0.02f;
    ROCKS_PROPERTY() float fogHeightFalloff = 0.2f;
    ROCKS_PROPERTY() float fogHeightOffset = 0.0f;

    friend constexpr bool operator==(const HeightFogData&, const HeightFogData&) noexcept = default;
};

// 높이 안개 설정이다(UE UExponentialHeightFogComponent 속성 — Docs/Design/EngineRuntime 27.3절).
// 거리는 cm다. 기본은 UE 생성자(ExponentialHeightFogComponent.cpp:72–119)다.
ROCKS_STRUCT()
struct HeightFogSettings {
    ROCKS_GENERATED_BODY()

    // 둘째 겹 기본(밀도 0 — UE 생성자 :95)이다.
    static constexpr HeightFogData DefaultSecondFog() noexcept {
        HeightFogData data;
        data.fogDensity = 0.0f;
        return data;
    }

    ROCKS_PROPERTY() float fogDensity = 0.02f;
    ROCKS_PROPERTY() float fogHeightFalloff = 0.2f;
    ROCKS_PROPERTY() HeightFogData secondFogData = DefaultSecondFog();
    ROCKS_PROPERTY() LinearColor fogInscatteringLuminance = LinearColor::Black();
    ROCKS_PROPERTY() LinearColor skyAtmosphereAmbientContributionColorScale = LinearColor::White();
    ROCKS_PROPERTY() float skyLightCaptureAffectsHeightFogStrength = 0.0f;
    ROCKS_PROPERTY() float skyLightCaptureAffectsHeightFogRoughness = 0.15f;
    ROCKS_PROPERTY() float inscatteringColorCubemapAngle = 0.0f;
    ROCKS_PROPERTY() LinearColor inscatteringTextureTint = LinearColor::White();
    ROCKS_PROPERTY() float fullyDirectionalInscatteringColorDistance = 100000.0f;
    ROCKS_PROPERTY() float nonDirectionalInscatteringColorDistance = 1000.0f;
    ROCKS_PROPERTY() float directionalInscatteringExponent = 4.0f;
    ROCKS_PROPERTY() float directionalInscatteringStartDistance = 10000.0f;
    ROCKS_PROPERTY() LinearColor directionalInscatteringLuminance = LinearColor::Black();
    ROCKS_PROPERTY() float fogMaxOpacity = 1.0f;
    ROCKS_PROPERTY() float startDistance = 0.0f;
    ROCKS_PROPERTY() float endDistance = 0.0f;
    ROCKS_PROPERTY() float fogCutoffDistance = 0.0f;
    ROCKS_PROPERTY() bool enableVolumetricFog = false;
    ROCKS_PROPERTY() float volumetricFogScatteringDistribution = 0.2f;
    ROCKS_PROPERTY() Color volumetricFogAlbedo = Color::White();
    ROCKS_PROPERTY() LinearColor volumetricFogEmissive = LinearColor::Black();
    ROCKS_PROPERTY() float volumetricFogExtinctionScale = 1.0f;
    ROCKS_PROPERTY() float volumetricFogDistance = 6000.0f;
    ROCKS_PROPERTY() float volumetricFogStartDistance = 0.0f;
    ROCKS_PROPERTY() float volumetricFogNearFadeInDistance = 0.0f;
    ROCKS_PROPERTY() float volumetricFogStaticLightingScatteringIntensity = 1.0f;
    ROCKS_PROPERTY() bool overrideLightColorsWithFogInscatteringColors = false;
    ROCKS_PROPERTY() bool holdout = false;
    ROCKS_PROPERTY() bool renderInMainPass = true;
    ROCKS_PROPERTY() bool visibleInReflectionCaptures = true;
    ROCKS_PROPERTY() bool visibleInRealTimeSkyCaptures = true;

    friend bool operator==(const HeightFogSettings&, const HeightFogSettings&) noexcept = default;
};

// UE clamp 범위 안인지다(27.3절 — 비유한 값은 거짓).
ROCKS_WORLD_API bool IsValidHeightFogSettings(const HeightFogSettings& settings) noexcept;

} // namespace rocks
