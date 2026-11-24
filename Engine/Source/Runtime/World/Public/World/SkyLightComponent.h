#pragma once

#include <Meta/Annotations.h>
#include <World/LightComponent.h>

namespace rocks {

// 하늘광의 원천이다(UE ESkyLightSourceType).
ROCKS_ENUM()
enum class SkyLightSourceType : uint8 {
    // 먼 장면을 캡처한다.
    CapturedScene,
    // 지정한 cubemap을 쓴다(cubemap 참조는 TextureCube 참조가 생길 때 — 24.2.8절).
    SpecifiedCubemap
};

// 하늘에서 오는 주변광이다(UE USkyLightComponent — Docs/Design/EngineRuntime 24.2.2절). UE처럼
// LightComponent가 아니라 LightComponentBase를 잇는다. 컴포넌트 기본 Mobility가 Stationary이고
// affectTranslucentLighting은 거짓이다(UE는 ULightComponent 생성자만 참으로 둔다).
// 데이터만 있다: 렌더 장면 등록(RS1·RS3) 전에는 아무것도 비추지 않는다(24.1절).
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_WORLD_API SkyLightComponent : public LightComponentBase {
    ROCKS_GENERATED_BODY()

public:
    // UE 생성자 기본값이다(SkyLightComponent.cpp:294–342).
    static constexpr float DefaultSkyIntensity = 1.0f;
    static constexpr int32 DefaultCubemapResolution = 128;
    static constexpr float DefaultSkyDistanceThreshold = 150000.0f;
    // cubemap 해상도 범위다(UE SanitizeCubemapSize 하한 8, 상한은 RHI 최대 cube 크기 —
    // GetMaxCubeTextureDimension의 D3D12 값 16384).
    static constexpr int32 MinCubemapResolution = 8;
    static constexpr int32 MaxCubemapResolution = 16384;

    explicit SkyLightComponent(Allocator& allocator) noexcept : LightComponentBase(allocator) {
        intensity_ = DefaultSkyIntensity;
        castVolumetricShadow_ = true;
        affectTranslucentLighting_ = false;
        SetMobility(ComponentMobility::Stationary);
    }

    bool UsesRealTimeCapture() const noexcept {
        return realTimeCapture_;
    }

    Result<void> SetRealTimeCapture(bool realTime) noexcept;

    SkyLightSourceType SourceType() const noexcept {
        return sourceType_;
    }

    // 오류: 범위 밖 enum, Mobility.
    Result<void> SetSourceType(SkyLightSourceType type) noexcept;

    float SourceCubemapAngle() const noexcept {
        return sourceCubemapAngle_;
    }

    // 지정 cubemap 회전(도)이다. UE처럼 범위를 자르지 않는다. 오류: 비유한, Mobility.
    Result<void> SetSourceCubemapAngle(float degrees) noexcept;

    int32 CubemapResolution() const noexcept {
        return cubemapResolution_;
    }

    // 2의 거듭제곱으로 올리고 [MinCubemapResolution, MaxCubemapResolution]로 자른다(UE
    // SanitizeCubemapSize). 오류: 1 미만(InvalidArgument), Mobility.
    Result<void> SetCubemapResolution(int32 resolution) noexcept;

    // 이보다 먼 기하를 하늘로 본다(cm).
    float SkyDistanceThreshold() const noexcept {
        return skyDistanceThreshold_;
    }

    // 오류: 음수·비유한, Mobility.
    Result<void> SetSkyDistanceThreshold(float distance) noexcept;

    bool CapturesEmissiveOnly() const noexcept {
        return captureEmissiveOnly_;
    }

    Result<void> SetCaptureEmissiveOnly(bool emissiveOnly) noexcept;

    bool IsLowerHemisphereBlack() const noexcept {
        return lowerHemisphereIsBlack_;
    }

    Result<void> SetLowerHemisphereIsBlack(bool black) noexcept;

    const LinearColor& LowerHemisphereColor() const noexcept {
        return lowerHemisphereColor_;
    }

    // 오류: 비유한 성분, Mobility.
    Result<void> SetLowerHemisphereColor(const LinearColor& color) noexcept;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "realTimeCapture") bool realTimeCapture_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "sourceType")
    SkyLightSourceType sourceType_ = SkyLightSourceType::CapturedScene;
    ROCKS_PROPERTY(ReadOnly, Name = "sourceCubemapAngle") float sourceCubemapAngle_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "cubemapResolution")
    int32 cubemapResolution_ = DefaultCubemapResolution;
    ROCKS_PROPERTY(ReadOnly, Name = "skyDistanceThreshold")
    float skyDistanceThreshold_ = DefaultSkyDistanceThreshold;
    ROCKS_PROPERTY(ReadOnly, Name = "captureEmissiveOnly") bool captureEmissiveOnly_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "lowerHemisphereIsBlack") bool lowerHemisphereIsBlack_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "lowerHemisphereColor")
    LinearColor lowerHemisphereColor_ = LinearColor::Black();
};

} // namespace rocks
