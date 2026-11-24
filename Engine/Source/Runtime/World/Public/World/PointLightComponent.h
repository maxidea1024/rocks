#pragma once

#include <Meta/Annotations.h>
#include <World/LightComponent.h>

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

// 원뿔 광원이다(UE USpotLightComponent — 24.2.2절). 각은 반각(도)이고 setter는 값만 저장한다.
// 렌더에 쓰는 각은 ClampedConeAngles가 자른다(UE 실행 중 setter와 같다).
// 데이터만 있다: 렌더 장면 등록(RS1·RS3) 전에는 아무것도 비추지 않는다(24.1절).
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_WORLD_API SpotLightComponent : public PointLightComponent {
    ROCKS_GENERATED_BODY()

public:
    // 기본 바깥 반각(도, UE OuterConeAngle 44)이다.
    static constexpr float DefaultOuterConeAngle = 44.0f;
    // clamp 상한(도, UE GetClampedConeAngles 88.9)과 inner·outer 최소 간격(라디안 0.001)이다.
    static constexpr float MaxConeAngle = 88.9f;
    static constexpr float MinConeAngleGapRadians = 0.001f;

    // ClampedConeAngles 결과다(라디안).
    struct ConeAngles {
        float innerRadians = 0.0f;
        float outerRadians = 0.0f;
    };

    explicit SpotLightComponent(Allocator& allocator) noexcept : PointLightComponent(allocator) {
    }

    // Lumens는 원뿔 입체각으로 나눈다(UE SpotLightComponent.cpp:79).
    float ComputeLightBrightness() const noexcept override;

    float InnerConeAngle() const noexcept {
        return innerConeAngle_;
    }

    float OuterConeAngle() const noexcept {
        return outerConeAngle_;
    }

    // 오류: 비유한, Mobility(Stationary도 막는다 — UE SpotLightComponent.cpp:59). 아래도 같다.
    Result<void> SetInnerConeAngle(float degrees) noexcept;
    Result<void> SetOuterConeAngle(float degrees) noexcept;

    // inner = clamp(inner, 0, 88.9°), outer = clamp(outer, inner + 0.001, 88.9° + 0.001)(라디안 —
    // UE GetClampedConeAngles).
    ConeAngles ClampedConeAngles() const noexcept;

    // clamp한 바깥 반각(라디안)과 그 cos다(UE GetHalfConeAngle·GetCosHalfConeAngle).
    float HalfConeAngle() const noexcept {
        return ClampedConeAngles().outerRadians;
    }

    float CosHalfConeAngle() const noexcept;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "innerConeAngle") float innerConeAngle_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "outerConeAngle") float outerConeAngle_ = DefaultOuterConeAngle;
};

} // namespace rocks
