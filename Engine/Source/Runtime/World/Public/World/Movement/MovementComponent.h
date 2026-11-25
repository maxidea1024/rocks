#pragma once

#include <Core/Math/Quaternion.h>
#include <Core/Math/Rotator.h>
#include <Core/Math/Vector3.h>
#include <Meta/Annotations.h>
#include <World/Component.h>

namespace rocks {

// 기본 중력 가속도(cm/s², −Z — UE UPhysicsSettings::DefaultGravityZ, PhysicsSettingsCore.cpp:29)다.
// World 설정·물리 볼륨의 중력은 물리 단계(CR16)다.
inline constexpr double DefaultGravityZ = -980.0;

// 평면 제약 축이다(UE EPlaneConstraintAxisSetting — MovementComponent.h:43).
ROCKS_ENUM()
enum class PlaneConstraintAxis : uint8 {
    // 저장한 planeConstraintNormal을 쓴다.
    Custom,
    X,
    Y,
    Z,
    // UE 프로젝트 전역 자유도 설정이다. Rocks에는 전역 설정이 없어 제약이 없다(법선 0).
    UseGlobalPhysicsSetting
};

// 다른 SceneComponent(갱신 대상)를 움직이는 컴포넌트의 기반이다(UE UMovementComponent —
// Docs/Design/EngineRuntime 26.3절). 충돌 sweep 없이 움직인다: 모든 이동은 순간 이동이고 충돌은
// 일어나지 않는다(물리 CR16 때 sweep이 붙는다).
//
// 규약:
// - 갱신 대상은 영속하지 않는다(UE Transient). autoRegisterUpdatedComponent면 OnRegister·OnAwake·
//   OnStart에서 소유 Actor root를 쓴다.
// - Tick은 PrePhysics다. 갱신 대상의 OnTick은 이 컴포넌트 뒤, tickBeforeOwner면 소유 Actor OnTick도
//   뒤다(그 hook을 켠 대상에만 간선을 건다).
// - 평면 제약은 constrainToPlane일 때만 적용한다.
//
// 사용 예(파생 OnTick):
//   if (ShouldSkipUpdate()) { return; }
//   (void)MoveUpdatedComponent(Velocity() * context.deltaSeconds, CurrentRotation());
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_WORLD_API MovementComponent : public Component {
    ROCKS_GENERATED_BODY()

public:
    // UE MoveComponent가 "같은 회전"으로 보는 성분 허용 오차(SCENECOMPONENT_QUAT_TOLERANCE)와
    // IsExceedingMaxSpeed의 여유 배율(1.01)이다.
    static constexpr double QuaternionTolerance = 1.0e-8;
    static constexpr double MaxSpeedSlack = 1.01;

    explicit MovementComponent(Allocator& allocator) noexcept;

    // --- 갱신 대상 ---

    ObjectHandle UpdatedComponent() const noexcept {
        return updatedComponent_;
    }

    // 갱신 대상을 바꾼다(null은 해제 — Tick이 꺼진다, autoUpdateTickRegistration일 때). Tick 간선을
    // 옮긴다(출처 Framework). 오류: SceneComponent가 아님(TypeMismatch), 다른 World·파괴된 대상
    // (StaleHandle), 파괴 대기 대상·World 장부 밖 컴포넌트(InvalidArgument).
    Result<void> SetUpdatedComponent(ObjectHandle component) noexcept;

    bool AutoRegistersUpdatedComponent() const noexcept {
        return autoRegisterUpdatedComponent_;
    }

    void SetAutoRegisterUpdatedComponent(bool autoRegister) noexcept {
        autoRegisterUpdatedComponent_ = autoRegister;
    }

    bool AutoUpdatesTickRegistration() const noexcept {
        return autoUpdateTickRegistration_;
    }

    void SetAutoUpdateTickRegistration(bool autoUpdate) noexcept {
        autoUpdateTickRegistration_ = autoUpdate;
    }

    bool TicksBeforeOwner() const noexcept {
        return tickBeforeOwner_;
    }

    void SetTickBeforeOwner(bool before) noexcept {
        tickBeforeOwner_ = before;
    }

    // --- 속도 ---

    // 속도(cm/s, 월드 공간)다.
    const Vec3d& Velocity() const noexcept {
        return velocity_;
    }

    // 오류: 비유한 성분.
    Result<void> SetVelocity(const Vec3d& velocity) noexcept;

    // 갱신 대상의 ComponentVelocity를 velocity로 둔다(UE UpdateComponentVelocity).
    void UpdateComponentVelocity() noexcept;

    // velocity 0 + UpdateComponentVelocity(UE StopMovementImmediately).
    virtual void StopMovementImmediately() noexcept;

    // 최대 속도(cm/s)다(기본 0 — 파생이 재정의한다).
    virtual double MaxSpeed() const noexcept {
        return 0.0;
    }

    // |velocity|² > max(0, maxSpeed)² × MaxSpeedSlack다(UE IsExceedingMaxSpeed).
    bool IsExceedingMaxSpeed(double maxSpeed) const noexcept;

    // 중력 Z(cm/s²)다(기본 DefaultGravityZ — 발사체가 scale을 곱한다).
    virtual double GravityZ() const noexcept {
        return DefaultGravityZ;
    }

    // --- 평면 제약(UE MovementComponent.cpp:417–516) ---

    bool IsConstrainedToPlane() const noexcept {
        return constrainToPlane_;
    }

    void SetConstrainToPlane(bool constrain) noexcept {
        constrainToPlane_ = constrain;
    }

    PlaneConstraintAxis PlaneConstraintAxisSetting() const noexcept {
        return planeConstraintAxis_;
    }

    // 오류: 범위 밖 enum.
    Result<void> SetPlaneConstraintAxisSetting(PlaneConstraintAxis axis) noexcept;

    // 실효 법선이다(축 설정 X·Y·Z는 그 축, Custom은 저장 값, UseGlobalPhysicsSetting은 0).
    Vec3d PlaneConstraintNormal() const noexcept;

    // 정규화해 저장하고 Custom으로 둔다(0 벡터는 0 — UE GetSafeNormal). 오류: 비유한 성분.
    Result<void> SetPlaneConstraintNormal(const Vec3d& normal) noexcept;

    // 법선 = 정규화(up × forward)(UE SetPlaneConstraintFromVectors). 오류: 비유한 성분.
    Result<void> SetPlaneConstraintFromVectors(const Vec3d& forward, const Vec3d& up) noexcept;

    const Vec3d& PlaneConstraintOrigin() const noexcept {
        return planeConstraintOrigin_;
    }

    // 오류: 비유한 성분.
    Result<void> SetPlaneConstraintOrigin(const Vec3d& origin) noexcept;

    bool SnapsToPlaneAtStart() const noexcept {
        return snapToPlaneAtStart_;
    }

    void SetSnapToPlaneAtStart(bool snap) noexcept {
        snapToPlaneAtStart_ = snap;
    }

    Vec3d ConstrainDirectionToPlane(const Vec3d& direction) const noexcept;
    Vec3d ConstrainLocationToPlane(const Vec3d& location) const noexcept;
    Vec3d ConstrainNormalToPlane(const Vec3d& normal) const noexcept;

    // 갱신 대상을 평면 위로 옮긴다(제약이 꺼져 있거나 대상이 없으면 성공 — UE
    // SnapUpdatedComponentToPlane). 오류: 대상 이동 실패.
    Result<void> SnapUpdatedComponentToPlane() noexcept;

    // --- 이동 ---

    // 갱신 대상을 delta(평면 제약 적용)만큼 옮기고 월드 회전을 rotation으로 둔다(UE
    // MoveUpdatedComponent, sweep 없음). delta가 0이고 회전이 같으면 아무것도 하지 않는다. 오류:
    // 대상 없음(InvalidArgument), Mobility 위반, 비유한 값.
    Result<void> MoveUpdatedComponent(const Vec3d& delta, const Quatd& rotation) noexcept;

    // 대상이 없거나 Movable이 아니면 참이다(UE ShouldSkipUpdate — 렌더 판정은 렌더 단계).
    bool ShouldSkipUpdate() const noexcept;

protected:
    Result<void> OnRegister() noexcept override;
    Result<void> OnAwake() noexcept override;
    void OnStart() noexcept override;
    void OnTick(const TickContext& context) noexcept override;

    // 갱신 대상의 월드 위치·회전이다(대상이 없으면 0·단위).
    Vec3d UpdatedLocation() const noexcept;
    Quatd UpdatedRotation() const noexcept;

    // 비어 있고 autoRegisterUpdatedComponent면 소유 Actor root를 쓴다.
    void AutoRegisterUpdatedComponent() noexcept;

    ROCKS_PROPERTY(ReadOnly, Name = "velocity") Vec3d velocity_;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "constrainToPlane") bool constrainToPlane_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "planeConstraintNormal") Vec3d planeConstraintNormal_;
    ROCKS_PROPERTY(ReadOnly, Name = "planeConstraintOrigin") Vec3d planeConstraintOrigin_;
    ROCKS_PROPERTY(ReadOnly, Name = "planeConstraintAxis")
    PlaneConstraintAxis planeConstraintAxis_ = PlaneConstraintAxis::Custom;
    ROCKS_PROPERTY(ReadOnly, Name = "snapToPlaneAtStart") bool snapToPlaneAtStart_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "autoRegisterUpdatedComponent")
    bool autoRegisterUpdatedComponent_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "autoUpdateTickRegistration")
    bool autoUpdateTickRegistration_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "tickBeforeOwner") bool tickBeforeOwner_ = true;
    ROCKS_NATIVE() ObjectHandle updatedComponent_;
};

} // namespace rocks
