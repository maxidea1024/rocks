#pragma once

#include <Core/Math/Color.h>
#include <Core/Math/MathUtility.h>
#include <Core/Math/Shapes.h>
#include <Core/Math/Vector3.h>
#include <Meta/Annotations.h>
#include <World/Lights/LightingChannels.h>
#include <World/SceneComponent.h>
#include <World/Lights/LightComponentBase.h>

namespace rocks {

// 방향을 가진 광원의 기반이다(UE ULightComponent — 24.2.2절). 방향은 월드 +X 축이다.
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS(Abstract)
class ROCKS_WORLD_API LightComponent : public LightComponentBase {
    ROCKS_GENERATED_BODY()

public:
    // 기본 색온도(K, D65 — UE Temperature 6500)다.
    static constexpr float DefaultTemperature = 6500.0f;
    // 기본 그림자 편향(UE ShadowBias·ShadowSlopeBias 0.5)과 방향광의 상한(UE
    // DirectionalLightComponent PostEditChange clamp 10)이다.
    static constexpr float DefaultShadowBias = 0.5f;
    static constexpr float MaxShadowBias = 10.0f;

    explicit LightComponent(Allocator& allocator) noexcept : LightComponentBase(allocator) {
    }

    // 렌더러에 줄 밝기다(UE ComputeLightBrightness). 기본은 intensity 그대로이고 국소 광원이 단위
    // 변환을 더한다(24.2.4절). IES 프로파일은 아직 없다.
    virtual float ComputeLightBrightness() const noexcept {
        return intensity_;
    }

    // 빛이 나가는 방향(월드 +X 축 단위 벡터 — UE GetDirection)이다. 월드 행렬 축이 0이면 +X다.
    Vec3d Direction() const noexcept;

    float Temperature() const noexcept {
        return temperature_;
    }

    // 오류: 비유한, Mobility.
    Result<void> SetTemperature(float kelvin) noexcept;

    bool UsesTemperature() const noexcept {
        return useTemperature_;
    }

    Result<void> SetUseTemperature(bool use) noexcept;

    // 그리기를 멈추는 거리(cm, 0 이하 = 제한 없음)다.
    float MaxDrawDistance() const noexcept {
        return maxDrawDistance_;
    }

    // 오류: 비유한, Mobility.
    Result<void> SetMaxDrawDistance(float distance) noexcept;

    float MaxDistanceFadeRange() const noexcept {
        return maxDistanceFadeRange_;
    }

    // 오류: 비유한, Mobility.
    Result<void> SetMaxDistanceFadeRange(float range) noexcept;

    float SpecularScale() const noexcept {
        return specularScale_;
    }

    // 오류: [0, 1] 밖·비유한, Mobility.
    Result<void> SetSpecularScale(float scale) noexcept;

    float DiffuseScale() const noexcept {
        return diffuseScale_;
    }

    // 오류: [0, 1] 밖·비유한, Mobility.
    Result<void> SetDiffuseScale(float scale) noexcept;

    float ShadowResolutionScale() const noexcept {
        return shadowResolutionScale_;
    }

    // 0이면 그림자 맵을 끈다. 오류: 음수·비유한, Mobility.
    Result<void> SetShadowResolutionScale(float scale) noexcept;

    float ShadowBias() const noexcept {
        return shadowBias_;
    }

    // 오류: 비유한, Mobility.
    Result<void> SetShadowBias(float bias) noexcept;

    float ShadowSlopeBias() const noexcept {
        return shadowSlopeBias_;
    }

    // 오류: 비유한, Mobility.
    Result<void> SetShadowSlopeBias(float bias) noexcept;

    float ShadowSharpen() const noexcept {
        return shadowSharpen_;
    }

    // 오류: 비유한, Mobility.
    Result<void> SetShadowSharpen(float sharpen) noexcept;

    // 접촉 그림자 길이다(0 = 끔). 기본은 화면 공간 비율이고 ContactShadowLengthInWorldSpace면 cm다.
    float ContactShadowLength() const noexcept {
        return contactShadowLength_;
    }

    // 오류: 음수·비유한, Mobility.
    Result<void> SetContactShadowLength(float length) noexcept;

    bool ContactShadowLengthInWorldSpace() const noexcept {
        return contactShadowLengthInWorldSpace_;
    }

    Result<void> SetContactShadowLengthInWorldSpace(bool worldSpace) noexcept;

    bool CastsTranslucentShadows() const noexcept {
        return castTranslucentShadows_;
    }

    Result<void> SetCastTranslucentShadows(bool cast) noexcept;

    const rocks::LightingChannels& LightingChannels() const noexcept {
        return lightingChannels_;
    }

    // UE처럼 Mobility를 보지 않는다(LightComponent.cpp:1366).
    Result<void> SetLightingChannels(const rocks::LightingChannels& channels) noexcept;

protected:
    // 그림자 편향을 [0, MaxShadowBias]로 제한하는지다(방향광만 — UE
    // DirectionalLightComponent.cpp:1107).
    virtual bool LimitsShadowBias() const noexcept {
        return false;
    }

    ROCKS_PROPERTY(ReadOnly, Name = "temperature") float temperature_ = DefaultTemperature;
    ROCKS_PROPERTY(ReadOnly, Name = "useTemperature") bool useTemperature_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "maxDrawDistance") float maxDrawDistance_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "maxDistanceFadeRange") float maxDistanceFadeRange_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "specularScale") float specularScale_ = 1.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "diffuseScale") float diffuseScale_ = 1.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "shadowResolutionScale") float shadowResolutionScale_ = 1.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "shadowBias") float shadowBias_ = DefaultShadowBias;
    ROCKS_PROPERTY(ReadOnly, Name = "shadowSlopeBias") float shadowSlopeBias_ = DefaultShadowBias;
    ROCKS_PROPERTY(ReadOnly, Name = "shadowSharpen") float shadowSharpen_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "contactShadowLength") float contactShadowLength_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "contactShadowLengthInWorldSpace")
    bool contactShadowLengthInWorldSpace_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "castTranslucentShadows") bool castTranslucentShadows_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "lightingChannels") rocks::LightingChannels lightingChannels_;
};

} // namespace rocks
