#pragma once

#include <Core/Math/Color.h>
#include <Core/Math/Vector3.h>
#include <Meta/Annotations.h>
#include <World/Environment/ReflectionSourceType.h>
#include <World/SceneComponent.h>

namespace rocks {

// 반사 캡처의 기반이다(UE UReflectionCaptureComponent — Docs/Design/EngineRuntime 27.6절). 생성자
// Mobility는 Static이다(UE "실행 중 바꾸면 안 된다"). 데이터만 있다: 캡처·렌더(RS 단계) 연결 전에는
// 아무것도 하지 않는다. 파생: Sphere·Box·PlaneReflectionCaptureComponent.
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS(Abstract)
class ROCKS_WORLD_API ReflectionCaptureComponent : public SceneComponent {
    ROCKS_GENERATED_BODY()

public:
    // UE 생성자 기본값이다(ReflectionCaptureComponent.cpp:768–780).
    static constexpr float DefaultRuntimeSkylightScale = 0.1f;

    explicit ReflectionCaptureComponent(Allocator& allocator) noexcept : SceneComponent(allocator) {
        SetMobility(ComponentMobility::Static);
    }

    ReflectionSourceType SourceType() const noexcept {
        return sourceType_;
    }

    // 오류: 범위 밖 enum.
    Result<void> SetSourceType(ReflectionSourceType type) noexcept;

    float SourceCubemapAngle() const noexcept {
        return sourceCubemapAngle_;
    }

    // 오류: 비유한(UE는 UIMin·UIMax [0, 360]뿐이라 범위로 막지 않는다).
    Result<void> SetSourceCubemapAngle(float degrees) noexcept;

    float Brightness() const noexcept {
        return brightness_;
    }

    // 오류: 비유한(아래 거리도 같다 — UE에 UIMin뿐이다).
    Result<void> SetBrightness(float brightness) noexcept;

    // 캡처 위치 offset(cm, 월드 공간)이다.
    const Vec3d& CaptureOffset() const noexcept {
        return captureOffset_;
    }

    // 오류: 비유한 성분.
    Result<void> SetCaptureOffset(const Vec3d& offset) noexcept;

    // 실행 중 캡처에서 하늘광에 곱하는 배율이다(UE RuntimeSkylightScale).
    const LinearColor& RuntimeSkylightScale() const noexcept {
        return runtimeSkylightScale_;
    }

    // 오류: 비유한 성분.
    Result<void> SetRuntimeSkylightScale(const LinearColor& scale) noexcept;

    bool UsesRuntimeCapture() const noexcept {
        return runtimeCapture_;
    }

    void SetRuntimeCapture(bool runtime) noexcept;

    float MaxViewDistance() const noexcept {
        return maxViewDistance_;
    }

    Result<void> SetMaxViewDistance(float distance) noexcept;

    // 실행 중 캡처의 먼 평면을 maxViewDistance로 둘지다(UE bFiniteFarPlane).
    bool HasFiniteFarPlane() const noexcept {
        return finiteFarPlane_;
    }

    void SetFiniteFarPlane(bool finite) noexcept;

    // 영향 경계 구 반지름(cm)이다(UE GetInfluenceBoundingRadius — 파생마다 다르다).
    virtual double InfluenceBoundingRadius() const noexcept = 0;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "sourceType")
    ReflectionSourceType sourceType_ = ReflectionSourceType::CapturedScene;
    ROCKS_PROPERTY(ReadOnly, Name = "sourceCubemapAngle") float sourceCubemapAngle_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "brightness") float brightness_ = 1.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "captureOffset") Vec3d captureOffset_;
    ROCKS_PROPERTY(ReadOnly, Name = "runtimeSkylightScale")
    LinearColor runtimeSkylightScale_ =
        LinearColor(DefaultRuntimeSkylightScale, DefaultRuntimeSkylightScale,
                    DefaultRuntimeSkylightScale, 1.0f);
    ROCKS_PROPERTY(ReadOnly, Name = "runtimeCapture") bool runtimeCapture_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "maxViewDistance") float maxViewDistance_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "finiteFarPlane") bool finiteFarPlane_ = false;
};

} // namespace rocks
