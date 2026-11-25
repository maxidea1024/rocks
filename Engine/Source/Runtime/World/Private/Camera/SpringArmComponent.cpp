#include <Core/Math/InterpTo.h>
#include <Core/Math/MathUtility.h>
#include <Core/Math/VectorUtility.h>
#include <World/Actor.generated.h>
#include <World/Actor.h>
#include <World/Component.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/Camera/SpringArmComponent.generated.h>
#include <World/Camera/SpringArmComponent.h>
#include <World/World.h>

// SpringArmComponent다(UE USpringArmComponent — Docs/Design/EngineRuntime 26.7절, D4).
namespace rocks {

namespace {

Result<void> InvalidArmValue() noexcept {
    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

bool IsFiniteArmVector(const Vec3d& value) noexcept {
    return math::IsFinite(value.x) && math::IsFinite(value.y) && math::IsFinite(value.z);
}

Rotator<double> ToRotator(const Quatd& rotation) noexcept {
    auto rotator = Rotator<double>::TryFromQuaternion(rotation);
    return rotator ? rotator.Value() : Rotator<double>();
}

} // namespace

SpringArmComponent::SpringArmComponent(Allocator& allocator) noexcept : SceneComponent(allocator) {
    // UE TG_PostPhysics(SpringArmComponent.cpp:18).
    TickSettings& tick = PrimaryTick();
    tick.tick = true;
    tick.phase = TickPhase::PostPhysics;
}

Result<void> SpringArmComponent::SetTargetArmLength(double length) noexcept {
    if (!math::IsFinite(length)) {
        return InvalidArmValue();
    }

    targetArmLength_ = length;
    return Result<void>::Success();
}

Result<void> SpringArmComponent::SetSocketOffset(const Vec3d& offset) noexcept {
    if (!IsFiniteArmVector(offset)) {
        return InvalidArmValue();
    }

    socketOffset_ = offset;
    return Result<void>::Success();
}

Result<void> SpringArmComponent::SetTargetOffset(const Vec3d& offset) noexcept {
    if (!IsFiniteArmVector(offset)) {
        return InvalidArmValue();
    }

    targetOffset_ = offset;
    return Result<void>::Success();
}

Result<void> SpringArmComponent::SetCameraLagSpeed(double speed) noexcept {
    if (!math::IsFinite(speed) || speed < 0.0 || speed > MaxLagSpeed) {
        return InvalidArmValue();
    }

    cameraLagSpeed_ = speed;
    return Result<void>::Success();
}

Result<void> SpringArmComponent::SetCameraRotationLagSpeed(double speed) noexcept {
    if (!math::IsFinite(speed) || speed < 0.0 || speed > MaxLagSpeed) {
        return InvalidArmValue();
    }

    cameraRotationLagSpeed_ = speed;
    return Result<void>::Success();
}

Result<void> SpringArmComponent::SetCameraLagMaxTimeStep(double seconds) noexcept {
    if (!math::IsFinite(seconds) || seconds < MinLagMaxTimeStep || seconds > MaxLagMaxTimeStep) {
        return InvalidArmValue();
    }

    cameraLagMaxTimeStep_ = seconds;
    return Result<void>::Success();
}

Result<void> SpringArmComponent::SetCameraLagMaxDistance(double distance) noexcept {
    if (!math::IsFinite(distance) || distance < 0.0) {
        return InvalidArmValue();
    }

    cameraLagMaxDistance_ = distance;
    return Result<void>::Success();
}

Rotator<double> SpringArmComponent::TargetRotation() noexcept {
    // UE GetTargetRotation(SpringArmComponent.cpp:54–91).
    Rotator<double> rotation = ToRotator(WorldQuaternion());
    if (usePawnControlRotation_ && HasWorld() && !GetOwner().IsNull()) {
        Rotator<double> view;
        auto provided = GetWorld().With<Actor>(
            GetOwner(), [&view](Actor& owner) noexcept { return owner.TryGetViewRotation(view); });
        if (provided && provided.Value()) {
            rotation = view;
        }
    }

    if (!IsAbsoluteRotation()) {
        const Rotator<double> relative = ToRotator(RelativeTransform().rotation);
        if (!inheritPitch_) {
            rotation.pitch = relative.pitch;
        }

        if (!inheritYaw_) {
            rotation.yaw = relative.yaw;
        }

        if (!inheritRoll_) {
            rotation.roll = relative.roll;
        }
    }

    return rotation;
}

void SpringArmComponent::UpdateDesiredArmLocation(bool locationLag, bool rotationLag,
                                                  double deltaSeconds) noexcept {
    // UE UpdateDesiredArmLocation(SpringArmComponent.cpp:93–225), 충돌 시험 없음.
    Rotator<double> desiredRotation = TargetRotation();
    if (rotationLag) {
        if (useCameraLagSubstepping_ && deltaSeconds > cameraLagMaxTimeStep_ &&
            cameraRotationLagSpeed_ > 0.0) {
            const Rotator<double> difference =
                Rotator<double>(desiredRotation.pitch - previousDesiredRotation_.pitch,
                                desiredRotation.yaw - previousDesiredRotation_.yaw,
                                desiredRotation.roll - previousDesiredRotation_.roll)
                    .GetNormalized();
            const double inverse = 1.0 / deltaSeconds;
            const Rotator<double> step(difference.pitch * inverse, difference.yaw * inverse,
                                       difference.roll * inverse);
            Rotator<double> lerpTarget = previousDesiredRotation_;
            double remaining = deltaSeconds;
            while (remaining > LagSubstepEpsilon) {
                const double alpha = math::Min(cameraLagMaxTimeStep_, remaining);
                lerpTarget = Rotator<double>(lerpTarget.pitch + step.pitch * alpha,
                                             lerpTarget.yaw + step.yaw * alpha,
                                             lerpTarget.roll + step.roll * alpha);
                remaining -= alpha;
                desiredRotation = ToRotator(math::QInterpTo(previousDesiredRotation_.ToQuaternion(),
                                                            lerpTarget.ToQuaternion(), alpha,
                                                            cameraRotationLagSpeed_));
                previousDesiredRotation_ = desiredRotation;
            }
        } else {
            desiredRotation = ToRotator(math::QInterpTo(previousDesiredRotation_.ToQuaternion(),
                                                        desiredRotation.ToQuaternion(),
                                                        deltaSeconds, cameraRotationLagSpeed_));
        }
    }

    previousDesiredRotation_ = desiredRotation;

    // 팔 시작(월드 공간 targetOffset)과 위치 지연.
    const Vec3d armOrigin = WorldLocation() + targetOffset_;
    Vec3d desiredLocation = armOrigin;
    if (locationLag) {
        if (useCameraLagSubstepping_ && deltaSeconds > cameraLagMaxTimeStep_ &&
            cameraLagSpeed_ > 0.0) {
            const Vec3d step = (desiredLocation - previousDesiredLocation_) * (1.0 / deltaSeconds);
            Vec3d lerpTarget = previousDesiredLocation_;
            double remaining = deltaSeconds;
            while (remaining > LagSubstepEpsilon) {
                const double alpha = math::Min(cameraLagMaxTimeStep_, remaining);
                lerpTarget = lerpTarget + step * alpha;
                remaining -= alpha;
                desiredLocation =
                    math::VInterpTo(previousDesiredLocation_, lerpTarget, alpha, cameraLagSpeed_);
                previousDesiredLocation_ = desiredLocation;
            }
        } else {
            desiredLocation = math::VInterpTo(previousDesiredLocation_, desiredLocation,
                                              deltaSeconds, cameraLagSpeed_);
        }

        if (cameraLagMaxDistance_ > 0.0 && (desiredLocation - armOrigin).LengthSquared() >
                                               cameraLagMaxDistance_ * cameraLagMaxDistance_) {
            desiredLocation = armOrigin + math::ClampedToMaxSize(desiredLocation - armOrigin,
                                                                 cameraLagMaxDistance_);
        }
    }

    previousArmOrigin_ = armOrigin;
    previousDesiredLocation_ = desiredLocation;

    // 팔 끝 = 시작 − 앞쪽 × 길이 + 팔 회전 공간 socketOffset. 충돌 시험은 하지 않는다(26.7절).
    const Quatd armRotation = desiredRotation.ToQuaternion();
    desiredLocation = desiredLocation - desiredRotation.Vector() * targetArmLength_;
    desiredLocation = desiredLocation + armRotation.RotateVector(socketOffset_);

    // 컴포넌트 월드 기준 상대 Transform(UE GetRelativeTransform)이다.
    auto inverse = WorldMatrix().TryInverse();
    relativeSocketLocation_ = inverse ? inverse.Value().TransformPoint(desiredLocation)
                                      : desiredLocation - WorldLocation();
    auto relativeRotation = (WorldQuaternion().Conjugate() * armRotation).TryNormalize();
    relativeSocketRotation_ = relativeRotation ? relativeRotation.Value() : Quatd::Identity();
    NotifyChildSocketsChanged();
}

bool SpringArmComponent::HasSocket(StringView socket) const noexcept {
    return socket == StringView(SocketName);
}

Result<Transform> SpringArmComponent::GetSocketTransform(StringView,
                                                         TransformSpace space) const noexcept {
    // UE GetSocketTransform(:262): 이름과 무관하게 팔 끝.
    const Transform local(relativeSocketRotation_, relativeSocketLocation_, Vec3d::One());
    if (space == TransformSpace::Component) {
        return Result<Transform>::Success(local);
    }

    auto world = TryWorldTransform();
    if (!world) {
        return world;
    }

    return Transform::TryMultiply(local, world.Value());
}

Result<void> SpringArmComponent::OnRegister() noexcept {
    if (auto base = SceneComponent::OnRegister(); !base) {
        return base;
    }

    // UE OnRegister(:239–249): 하한을 걸고 이전 값을 지연 없이 채운다.
    cameraLagMaxTimeStep_ = math::Max(cameraLagMaxTimeStep_, MinLagMaxTimeStep);
    cameraLagSpeed_ = math::Max(cameraLagSpeed_, 0.0);
    UpdateDesiredArmLocation(false, false, 0.0);
    return Result<void>::Success();
}

void SpringArmComponent::OnStart() noexcept {
    // Rocks는 AddComponent(OnRegister) 뒤에 부착하므로 시작 때 지연 기준값을 다시 채운다(UE는
    // SetupAttachment가 OnRegister 전이다 — 26.7절).
    UpdateDesiredArmLocation(false, false, 0.0);
}

void SpringArmComponent::OnTick(const TickContext& context) noexcept {
    UpdateDesiredArmLocation(enableCameraLag_, enableCameraRotationLag_, context.deltaSeconds);
}

} // namespace rocks
