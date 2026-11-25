#pragma once

#include <Core/Math/Color.h>
#include <Meta/Annotations.h>

namespace rocks {

// 행성 위치를 정하는 방식이다(UE ESkyAtmosphereTransformMode — SkyAtmosphereComponent.h:36).
ROCKS_ENUM()
enum class SkyAtmosphereTransformMode : uint8 {
    // 행성 꼭대기가 월드 원점이다.
    PlanetTopAtAbsoluteWorldOrigin,
    // 행성 꼭대기가 컴포넌트 위치다.
    PlanetTopAtComponentTransform,
    // 행성 중심이 컴포넌트 위치다.
    PlanetCenterAtComponentTransform
};

// 고도별 흡수 밀도의 천막 모양 분포다(UE FTentDistribution — km). 기본은 지구 오존(25·1·15)이다.
ROCKS_STRUCT()
struct SkyAtmosphereTentDistribution {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() float tipAltitude = 25.0f;
    ROCKS_PROPERTY() float tipValue = 1.0f;
    ROCKS_PROPERTY() float width = 15.0f;

    friend constexpr bool operator==(const SkyAtmosphereTentDistribution&,
                                     const SkyAtmosphereTentDistribution&) noexcept = default;
};

// 대기 설정이다(UE USkyAtmosphereComponent 속성 — Docs/Design/EngineRuntime 27.2절). 거리는 km,
// 계수는 1/km다. 기본은 UE 생성자의 지구 대기다(SkyAtmosphereComponent.cpp:91–139). 계수 색은
// UE처럼 원시 값 × (1/scale 성분)이라 alpha도 곱해진다(UE FLinearColor 곱과 같다 — 렌더러는 RGB만
// 쓴다).
ROCKS_STRUCT()
struct SkyAtmosphereSettings {
    ROCKS_GENERATED_BODY()

    // UE 기본 원시 계수(1/km)다.
    static constexpr float RayleighRawR = 0.005802f;
    static constexpr float RayleighRawG = 0.013558f;
    static constexpr float RayleighRawB = 0.033100f;
    static constexpr float OzoneRawR = 0.000650f;
    static constexpr float OzoneRawG = 0.001881f;
    static constexpr float OzoneRawB = 0.000085f;

    ROCKS_PROPERTY()
    SkyAtmosphereTransformMode transformMode =
        SkyAtmosphereTransformMode::PlanetTopAtAbsoluteWorldOrigin;
    ROCKS_PROPERTY() float bottomRadius = 6360.0f;
    ROCKS_PROPERTY() float atmosphereHeight = 6420.0f - 6360.0f;
    ROCKS_PROPERTY() Color groundAlbedo = Color(170, 170, 170);
    ROCKS_PROPERTY() float multiScatteringFactor = 1.0f;
    ROCKS_PROPERTY() float traceSampleCountScale = 1.0f;
    ROCKS_PROPERTY() float rayleighScatteringScale = RayleighRawB;
    ROCKS_PROPERTY()
    LinearColor rayleighScattering =
        LinearColor(RayleighRawR * (1.0f / RayleighRawB), RayleighRawG * (1.0f / RayleighRawB),
                    RayleighRawB * (1.0f / RayleighRawB), 1.0f * (1.0f / RayleighRawB));
    ROCKS_PROPERTY() float rayleighExponentialDistribution = 8.0f;
    ROCKS_PROPERTY() float mieScatteringScale = 0.003996f;
    ROCKS_PROPERTY() LinearColor mieScattering = LinearColor::White();
    ROCKS_PROPERTY() float mieAbsorptionScale = 0.000444f;
    ROCKS_PROPERTY() LinearColor mieAbsorption = LinearColor::White();
    ROCKS_PROPERTY() float mieAnisotropy = 0.8f;
    ROCKS_PROPERTY() float mieExponentialDistribution = 1.2f;
    ROCKS_PROPERTY() float otherAbsorptionScale = OzoneRawG;
    ROCKS_PROPERTY()
    LinearColor otherAbsorption =
        LinearColor(OzoneRawR * (1.0f / OzoneRawG), OzoneRawG * (1.0f / OzoneRawG),
                    OzoneRawB * (1.0f / OzoneRawG), 1.0f * (1.0f / OzoneRawG));
    ROCKS_PROPERTY() SkyAtmosphereTentDistribution otherTentDistribution;
    ROCKS_PROPERTY() LinearColor skyLuminanceFactor = LinearColor::White();
    ROCKS_PROPERTY() LinearColor skyAndAerialPerspectiveLuminanceFactor = LinearColor::White();
    ROCKS_PROPERTY() float aerialPerspectiveViewDistanceScale = 1.0f;
    ROCKS_PROPERTY() float heightFogContribution = 1.0f;
    ROCKS_PROPERTY() float transmittanceMinLightElevationAngle = -90.0f;
    ROCKS_PROPERTY() float aerialPerspectiveStartDepth = 0.1f;
    ROCKS_PROPERTY() bool holdout = false;
    ROCKS_PROPERTY() bool renderInMainPass = true;

    friend bool operator==(const SkyAtmosphereSettings&,
                           const SkyAtmosphereSettings&) noexcept = default;
};

// UE ClampMin·ClampMax 범위 안인지다(27.2절 — 비유한 값·범위 밖 enum은 거짓).
ROCKS_WORLD_API bool IsValidSkyAtmosphereSettings(const SkyAtmosphereSettings& settings) noexcept;

} // namespace rocks
