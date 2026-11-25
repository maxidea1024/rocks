#pragma once

#include <Meta/Annotations.h>
#include <World/MovementComponent.h>

namespace rocks {

// 발사체 궤적이다(UE UProjectileMovementComponent — Docs/Design/EngineRuntime 26.5절). 중력·homing·
// 최대 속도·substep은 끝까지 동작한다. 충돌·bounce 데이터(shouldBounce·bounciness 등)는 영속만 하고
// sweep이 없어 쓰이지 않는다(물리 CR16 때 붙는다).
//
// 사용 예(Actor OnAwake):
//   auto shot = GetWorld().AddComponent<ProjectileMovementComponent>(Handle());
//   GetWorld().With<ProjectileMovementComponent>(shot.Value(), [](ProjectileMovementComponent& p) {
//       (void)p.SetInitialSpeed(3000.0);   // 30 m/s, root 앞쪽
//       p.SetRotationFollowsVelocity(true);
//   });
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_WORLD_API ProjectileMovementComponent : public MovementComponent {
    ROCKS_GENERATED_BODY()

public:
    // UE 생성자 기본값·범위다(ProjectileMovementComponent.cpp:18–64, 헤더 ClampMin·Max).
    static constexpr double DefaultBounciness = 0.6;
    static constexpr double DefaultFriction = 0.2;
    static constexpr double DefaultBounceStopThreshold = 5.0;
    static constexpr double DefaultMaxSimulationTimeStep = 0.05;
    static constexpr double MinSimulationTimeStep = 0.0166;
    static constexpr double MaxSimulationTimeStepLimit = 0.50;
    static constexpr int32 DefaultMaxSimulationIterations = 4;
    static constexpr int32 MaxSimulationIterationsLimit = 25;
    static constexpr int32 DefaultBounceAdditionalIterations = 1;
    static constexpr int32 MaxBounceAdditionalIterations = 4;
    // 시뮬레이션 단계의 최소 시간(UE MIN_TICK_TIME 1e-6)과 회전을 속도에 맞추는 최소 속도
    // 성분(cm/s, UE FRepMovement 양자화 1)이다.
    static constexpr double MinTickTime = 1.0e-6;
    static constexpr double RotationVelocityTolerance = 1.0;

    explicit ProjectileMovementComponent(Allocator& allocator) noexcept;

    // --- 데이터 ---

    double InitialSpeed() const noexcept {
        return initialSpeed_;
    }

    // 오류: 음수·비유한. 아래 속도·크기 setter도 같다.
    Result<void> SetInitialSpeed(double speed) noexcept;

    double MaxSpeed() const noexcept override {
        return maxSpeed_;
    }

    // 0은 제한 없음이다.
    Result<void> SetMaxSpeed(double speed) noexcept;

    bool RotationFollowsVelocity() const noexcept {
        return rotationFollowsVelocity_;
    }

    void SetRotationFollowsVelocity(bool follows) noexcept {
        rotationFollowsVelocity_ = follows;
    }

    bool RotationRemainsVertical() const noexcept {
        return rotationRemainsVertical_;
    }

    void SetRotationRemainsVertical(bool vertical) noexcept {
        rotationRemainsVertical_ = vertical;
    }

    bool InitialVelocityInLocalSpace() const noexcept {
        return initialVelocityInLocalSpace_;
    }

    void SetInitialVelocityInLocalSpace(bool local) noexcept {
        initialVelocityInLocalSpace_ = local;
    }

    bool ForcesSubStepping() const noexcept {
        return forceSubStepping_;
    }

    void SetForceSubStepping(bool force) noexcept {
        forceSubStepping_ = force;
    }

    bool IsSimulationEnabled() const noexcept {
        return simulationEnabled_;
    }

    void SetSimulationEnabled(bool enabled) noexcept {
        simulationEnabled_ = enabled;
    }

    double ProjectileGravityScale() const noexcept {
        return projectileGravityScale_;
    }

    // 오류: 비유한.
    Result<void> SetProjectileGravityScale(double scale) noexcept;

    bool IsHomingProjectile() const noexcept {
        return homing_;
    }

    void SetHomingProjectile(bool homing) noexcept {
        homing_ = homing;
    }

    double HomingAccelerationMagnitude() const noexcept {
        return homingAccelerationMagnitude_;
    }

    Result<void> SetHomingAccelerationMagnitude(double magnitude) noexcept;

    // homing 대상(SceneComponent, 영속하지 않음 — UE TWeakObjectPtr)이다.
    ObjectHandle HomingTargetComponent() const noexcept {
        return homingTarget_;
    }

    void SetHomingTargetComponent(ObjectHandle target) noexcept {
        homingTarget_ = target;
    }

    double MaxSimulationTimeStep() const noexcept {
        return maxSimulationTimeStep_;
    }

    // 오류: [MinSimulationTimeStep, MaxSimulationTimeStepLimit] 밖.
    Result<void> SetMaxSimulationTimeStep(double seconds) noexcept;

    int32 MaxSimulationIterations() const noexcept {
        return maxSimulationIterations_;
    }

    // 오류: [1, MaxSimulationIterationsLimit] 밖.
    Result<void> SetMaxSimulationIterations(int32 iterations) noexcept;

    // 충돌 데이터(sweep 전에는 쓰이지 않는다).
    bool ShouldBounce() const noexcept {
        return shouldBounce_;
    }

    void SetShouldBounce(bool bounce) noexcept {
        shouldBounce_ = bounce;
    }

    double Bounciness() const noexcept {
        return bounciness_;
    }

    Result<void> SetBounciness(double bounciness) noexcept;

    double Friction() const noexcept {
        return friction_;
    }

    Result<void> SetFriction(double friction) noexcept;

    double BounceVelocityStopSimulatingThreshold() const noexcept {
        return bounceVelocityStopSimulatingThreshold_;
    }

    double MinFrictionFraction() const noexcept {
        return minFrictionFraction_;
    }

    bool BounceAngleAffectsFriction() const noexcept {
        return bounceAngleAffectsFriction_;
    }

    bool SweepsCollision() const noexcept {
        return sweepCollision_;
    }

    int32 BounceAdditionalIterations() const noexcept {
        return bounceAdditionalIterations_;
    }

    // --- 동작 ---

    // 중력 Z = 기본 × projectileGravityScale(scale 0이면 0 — UE GetGravityZ)다.
    double GravityZ() const noexcept override;

    // 이번 갱신에 더할 힘(가속도, cm/s²)이다(UE AddForce·ClearPendingForce).
    void AddForce(const Vec3d& force) noexcept;
    void ClearPendingForce(bool clearImmediateForce = false) noexcept;

    // v + a·t를 최대 속도·평면 제약으로 자른다(UE ComputeVelocity).
    Vec3d ComputeVelocity(const Vec3d& initialVelocity, double deltaSeconds) const noexcept;

    // velocity Verlet 이동량 v0·t + (v1 − v0)·t/2다(UE ComputeMoveDelta).
    Vec3d ComputeMoveDelta(const Vec3d& initialVelocity, double deltaSeconds) const noexcept;

    // 다음 시뮬레이션 단계 시간이다(UE GetSimulationTimeStep).
    double SimulationTimeStep(double remainingSeconds, int32 iteration) const noexcept;

    // 중력·forceSubStepping·homing 대상이 있으면 참이다(UE ShouldUseSubStepping).
    bool ShouldUseSubStepping() const noexcept;

    // 대상이 없거나 컴포넌트가 꺼져 있다(UE HasStoppedSimulation).
    bool HasStoppedSimulation() const noexcept;

    // velocity·힘을 0으로, 대상 해제(Tick 꺼짐), OnProjectileStop()(UE StopSimulating).
    void StopSimulating() noexcept;

protected:
    // 초기 속도·회전을 정한다(UE InitializeComponent — 26.5절).
    Result<void> OnAwake() noexcept override;
    void OnTick(const TickContext& context) noexcept override;

    // StopSimulating 뒤 불린다(UE OnProjectileStop delegate 자리 — 26.1절).
    virtual void OnProjectileStop() noexcept {
    }

private:
    Vec3d ComputeAcceleration(const Vec3d& velocity) const noexcept;

    ROCKS_PROPERTY(ReadOnly, Name = "initialSpeed") double initialSpeed_ = 0.0;
    ROCKS_PROPERTY(ReadOnly, Name = "maxSpeed") double maxSpeed_ = 0.0;
    ROCKS_PROPERTY(ReadOnly, Name = "rotationFollowsVelocity")
    bool rotationFollowsVelocity_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "rotationRemainsVertical")
    bool rotationRemainsVertical_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "initialVelocityInLocalSpace")
    bool initialVelocityInLocalSpace_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "forceSubStepping") bool forceSubStepping_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "simulationEnabled") bool simulationEnabled_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "projectileGravityScale") double projectileGravityScale_ = 1.0;
    ROCKS_PROPERTY(ReadOnly, Name = "homing") bool homing_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "homingAccelerationMagnitude")
    double homingAccelerationMagnitude_ = 0.0;
    ROCKS_PROPERTY(ReadOnly, Name = "maxSimulationTimeStep")
    double maxSimulationTimeStep_ = DefaultMaxSimulationTimeStep;
    ROCKS_PROPERTY(ReadOnly, Name = "maxSimulationIterations")
    int32 maxSimulationIterations_ = DefaultMaxSimulationIterations;
    ROCKS_PROPERTY(ReadOnly, Name = "shouldBounce") bool shouldBounce_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "bounciness") double bounciness_ = DefaultBounciness;
    ROCKS_PROPERTY(ReadOnly, Name = "friction") double friction_ = DefaultFriction;
    ROCKS_PROPERTY(ReadOnly, Name = "bounceVelocityStopSimulatingThreshold")
    double bounceVelocityStopSimulatingThreshold_ = DefaultBounceStopThreshold;
    ROCKS_PROPERTY(ReadOnly, Name = "minFrictionFraction") double minFrictionFraction_ = 0.0;
    ROCKS_PROPERTY(ReadOnly, Name = "bounceAngleAffectsFriction")
    bool bounceAngleAffectsFriction_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "sweepCollision") bool sweepCollision_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "bounceAdditionalIterations")
    int32 bounceAdditionalIterations_ = DefaultBounceAdditionalIterations;
    ROCKS_NATIVE() ObjectHandle homingTarget_;
    ROCKS_NATIVE() Vec3d pendingForce_;
    ROCKS_NATIVE() Vec3d pendingForceThisUpdate_;
};

} // namespace rocks
