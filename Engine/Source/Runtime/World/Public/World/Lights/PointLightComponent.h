#pragma once

#include <Meta/Annotations.h>
#include <World/Lights/LightUnits.h>
#include <World/Lights/LightComponentBase.h>
#include <World/Lights/LightComponent.h>
#include <World/Lights/LocalLightComponent.h>

namespace rocks {

// 점 광원이다(UE UPointLightComponent — Docs/Design/EngineRuntime 24.2.2절). 기본은 물리 1/r²
// 감쇠다. 1/r²를 끄면 lightFalloffExponent로 반경 감쇠하고 단위는 Unitless로 되돌아간다.
// 데이터만 있다: 렌더 장면 등록(RS1·RS3) 전에는 아무것도 비추지 않는다(24.1절).
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_WORLD_API PointLightComponent : public LocalLightComponent {
    ROCKS_GENERATED_BODY()

public:
    // 1/r²가 꺼졌을 때의 기본 감쇠 지수(UE LightFalloffExponent 8)와 광원 크기 상한(cm, UE
    // SourceRadius 등 ClampMax 10000)이다. 감쇠 지수 하한은 UE PostEditChange의
    // KINDA_SMALL_NUMBER다.
    static constexpr float DefaultLightFalloffExponent = 8.0f;
    static constexpr float MinLightFalloffExponent = 1.0e-4f;
    static constexpr float MaxSourceSize = 10000.0f;

    explicit PointLightComponent(Allocator& allocator) noexcept : LocalLightComponent(allocator) {
    }

    // 단위별 배율을 더한 밝기다(1/r²일 때만 — UE PointLightComponent.cpp:127, 24.2.4절).
    float ComputeLightBrightness() const noexcept override;

    // 1/r²가 꺼져 있으면 저장 값과 무관하게 Unitless다(UE는 로드 때 되돌린다 — 저장 파일의 어긋난
    // 조합도 같은 뜻으로 읽힌다).
    LightUnits IntensityUnits() const noexcept override {
        return useInverseSquaredFalloff_ ? intensityUnits_ : LightUnits::Unitless;
    }

    bool UsesInverseSquaredFalloff() const noexcept {
        return useInverseSquaredFalloff_;
    }

    // 끄면 단위를 Unitless로 되돌린다(UE 로드·편집 규칙). 오류: Mobility.
    Result<void> SetUseInverseSquaredFalloff(bool use) noexcept;

    float LightFalloffExponent() const noexcept {
        return lightFalloffExponent_;
    }

    // 오류: MinLightFalloffExponent 미만·비유한, Mobility.
    Result<void> SetLightFalloffExponent(float exponent) noexcept;

    // 구 광원 반지름(cm)이다.
    float SourceRadius() const noexcept {
        return sourceRadius_;
    }

    // 오류: [0, MaxSourceSize] 밖·비유한, Mobility. 아래 둘도 같다.
    Result<void> SetSourceRadius(float radius) noexcept;

    float SoftSourceRadius() const noexcept {
        return softSourceRadius_;
    }

    Result<void> SetSoftSourceRadius(float radius) noexcept;

    // 캡슐 광원 길이(cm)다.
    float SourceLength() const noexcept {
        return sourceLength_;
    }

    Result<void> SetSourceLength(float length) noexcept;

protected:
    bool AcceptsIntensityUnits(LightUnits units) const noexcept override {
        return useInverseSquaredFalloff_ || units == LightUnits::Unitless;
    }

    // 1/r² 단위 배율이다. lumensFactor는 Lumens의 입체각 계수다(Point 1/(4π), Spot 원뿔).
    float BrightnessWithUnits(float lumensFactor) const noexcept;

    ROCKS_PROPERTY(ReadOnly, Name = "useInverseSquaredFalloff")
    bool useInverseSquaredFalloff_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "lightFalloffExponent")
    float lightFalloffExponent_ = DefaultLightFalloffExponent;
    ROCKS_PROPERTY(ReadOnly, Name = "sourceRadius") float sourceRadius_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "softSourceRadius") float softSourceRadius_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "sourceLength") float sourceLength_ = 0.0f;
};

} // namespace rocks
