#pragma once

#include <Core/Math/Quaternion.h>
#include <Core/Math/Rotator.h>
#include <Core/Math/Vector3.h>
#include <Meta/Annotations.h>
#include <World/SceneComponent.h>

namespace rocks {

// 자식(보통 카메라)을 부모에서 일정 거리 뒤에 두는 팔이다(UE USpringArmComponent — Docs/Design/
// EngineRuntime 26.7절). 자식은 socket 이름과 무관하게 팔 끝에 붙는다(UE와 같다 — 26.2절). 위치·
// 회전 지연은 끝까지 동작한다. 충돌 시험(doCollisionTest·probeSize)은 데이터만이고 sweep이 없어
// 팔을 줄이지 않는다(물리 CR16 때 붙는다).
//
// 사용 예(Pawn OnAwake):
//   auto arm = GetWorld().AddComponent<SpringArmComponent>(Handle());
//   auto camera = GetWorld().AddComponent<CameraComponent>(Handle());
//   GetWorld().With<CameraComponent>(camera.Value(), [&](CameraComponent& c) noexcept {
//       return c.AttachTo(arm.Value());           // 팔 끝
//   });
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_WORLD_API SpringArmComponent : public SceneComponent {
    ROCKS_GENERATED_BODY()

public:
    // 팔 끝 socket 이름(UE USpringArmComponent::SocketName)이다.
    static constexpr const char* SocketName = "SpringEndpoint";
    // UE 생성자 기본값·범위다(SpringArmComponent.cpp:18–47, 헤더 ClampMin·Max, OnRegister 하한).
    static constexpr double DefaultTargetArmLength = 300.0;
    static constexpr double DefaultProbeSize = 12.0;
    static constexpr double DefaultLagSpeed = 10.0;
    static constexpr double MaxLagSpeed = 1000.0;
    static constexpr double DefaultLagMaxTimeStep = 1.0 / 60.0;
    static constexpr double MinLagMaxTimeStep = 0.005;
    static constexpr double MaxLagMaxTimeStep = 0.5;
    // substep 반복을 끝내는 남은 시간(UE KINDA_SMALL_NUMBER)이다.
    static constexpr double LagSubstepEpsilon = 1.0e-4;

    explicit SpringArmComponent(Allocator& allocator) noexcept;

    // --- 데이터 ---

    double TargetArmLength() const noexcept {
        return targetArmLength_;
    }

    // 오류: 비유한(UE는 음수도 받는다 — 팔이 앞으로 간다).
    Result<void> SetTargetArmLength(double length) noexcept;

    // 팔 끝 오프셋(cm, 팔 회전 공간)이다.
    const Vec3d& SocketOffset() const noexcept {
        return socketOffset_;
    }

    Result<void> SetSocketOffset(const Vec3d& offset) noexcept;

    // 팔 시작 오프셋(cm, 월드 공간)이다.
    const Vec3d& TargetOffset() const noexcept {
        return targetOffset_;
    }

    Result<void> SetTargetOffset(const Vec3d& offset) noexcept;

    bool UsesPawnControlRotation() const noexcept {
        return usePawnControlRotation_;
    }

    void SetUsePawnControlRotation(bool use) noexcept {
        usePawnControlRotation_ = use;
    }

    bool InheritsPitch() const noexcept {
        return inheritPitch_;
    }

    bool InheritsYaw() const noexcept {
        return inheritYaw_;
    }

    bool InheritsRoll() const noexcept {
        return inheritRoll_;
    }

    void SetInherit(bool pitch, bool yaw, bool roll) noexcept {
        inheritPitch_ = pitch;
        inheritYaw_ = yaw;
        inheritRoll_ = roll;
    }

    bool HasCameraLag() const noexcept {
        return enableCameraLag_;
    }

    void SetEnableCameraLag(bool enable) noexcept {
        enableCameraLag_ = enable;
    }

    bool HasCameraRotationLag() const noexcept {
        return enableCameraRotationLag_;
    }

    void SetEnableCameraRotationLag(bool enable) noexcept {
        enableCameraRotationLag_ = enable;
    }

    bool UsesCameraLagSubstepping() const noexcept {
        return useCameraLagSubstepping_;
    }

    void SetUseCameraLagSubstepping(bool use) noexcept {
        useCameraLagSubstepping_ = use;
    }

    double CameraLagSpeed() const noexcept {
        return cameraLagSpeed_;
    }

    // 오류: [0, MaxLagSpeed] 밖·비유한. 회전 지연 속도도 같다.
    Result<void> SetCameraLagSpeed(double speed) noexcept;

    double CameraRotationLagSpeed() const noexcept {
        return cameraRotationLagSpeed_;
    }

    Result<void> SetCameraRotationLagSpeed(double speed) noexcept;

    double CameraLagMaxTimeStep() const noexcept {
        return cameraLagMaxTimeStep_;
    }

    // 오류: [MinLagMaxTimeStep, MaxLagMaxTimeStep] 밖.
    Result<void> SetCameraLagMaxTimeStep(double seconds) noexcept;

    double CameraLagMaxDistance() const noexcept {
        return cameraLagMaxDistance_;
    }

    // 0은 제한 없음이다. 오류: 음수·비유한.
    Result<void> SetCameraLagMaxDistance(double distance) noexcept;

    bool DoesCollisionTest() const noexcept {
        return doCollisionTest_;
    }

    double ProbeSize() const noexcept {
        return probeSize_;
    }

    // --- 계산 ---

    // 팔 목표 회전이다(UE GetTargetRotation — 26.7절).
    Rotator<double> TargetRotation() noexcept;

    // 팔 끝을 다시 계산한다(UE UpdateDesiredArmLocation — 충돌 시험은 하지 않는다). Tick이 부른다.
    void UpdateDesiredArmLocation(bool locationLag, bool rotationLag, double deltaSeconds) noexcept;

    // 팔 끝의 컴포넌트 기준 상대 위치·회전이다.
    const Vec3d& RelativeSocketLocation() const noexcept {
        return relativeSocketLocation_;
    }

    const Quatd& RelativeSocketRotation() const noexcept {
        return relativeSocketRotation_;
    }

    bool HasSocket(StringView socket) const noexcept override;

    // 이름과 무관하게 팔 끝이다(UE GetSocketTransform — SpringArmComponent.cpp:262).
    Result<Transform> GetSocketTransform(StringView socket,
                                         TransformSpace space) const noexcept override;

protected:
    Result<void> OnRegister() noexcept override;
    void OnStart() noexcept override;
    void OnTick(const TickContext& context) noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "targetArmLength")
    double targetArmLength_ = DefaultTargetArmLength;
    ROCKS_PROPERTY(ReadOnly, Name = "socketOffset") Vec3d socketOffset_;
    ROCKS_PROPERTY(ReadOnly, Name = "targetOffset") Vec3d targetOffset_;
    ROCKS_PROPERTY(ReadOnly, Name = "probeSize") double probeSize_ = DefaultProbeSize;
    ROCKS_PROPERTY(ReadOnly, Name = "doCollisionTest") bool doCollisionTest_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "usePawnControlRotation") bool usePawnControlRotation_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "inheritPitch") bool inheritPitch_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "inheritYaw") bool inheritYaw_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "inheritRoll") bool inheritRoll_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "enableCameraLag") bool enableCameraLag_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "enableCameraRotationLag")
    bool enableCameraRotationLag_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "useCameraLagSubstepping") bool useCameraLagSubstepping_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "cameraLagSpeed") double cameraLagSpeed_ = DefaultLagSpeed;
    ROCKS_PROPERTY(ReadOnly, Name = "cameraRotationLagSpeed")
    double cameraRotationLagSpeed_ = DefaultLagSpeed;
    ROCKS_PROPERTY(ReadOnly, Name = "cameraLagMaxTimeStep")
    double cameraLagMaxTimeStep_ = DefaultLagMaxTimeStep;
    ROCKS_PROPERTY(ReadOnly, Name = "cameraLagMaxDistance") double cameraLagMaxDistance_ = 0.0;
    ROCKS_NATIVE() Vec3d previousDesiredLocation_;
    ROCKS_NATIVE() Vec3d previousArmOrigin_;
    ROCKS_NATIVE() Rotator<double> previousDesiredRotation_;
    ROCKS_NATIVE() Vec3d relativeSocketLocation_;
    ROCKS_NATIVE() Quatd relativeSocketRotation_ = Quatd::Identity();
};

} // namespace rocks
