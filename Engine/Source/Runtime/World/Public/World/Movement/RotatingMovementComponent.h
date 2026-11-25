#pragma once

#include <Core/Math/Quaternion.h>
#include <Core/Math/Rotator.h>
#include <Core/Math/Vector3.h>
#include <Meta/Annotations.h>
#include <World/Component.h>
#include <World/Movement/MovementComponent.h>

namespace rocks {

// 갱신 대상을 일정 속도로 돌린다(UE URotatingMovementComponent — 26.4절). velocity는 바꾸지 않는다.
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_WORLD_API RotatingMovementComponent : public MovementComponent {
    ROCKS_GENERATED_BODY()

public:
    // 기본 회전 속도(도/초, UE RotationRate yaw 180 — RotatingMovementComponent.cpp:13)다.
    static constexpr double DefaultYawRate = 180.0;

    explicit RotatingMovementComponent(Allocator& allocator) noexcept
        : MovementComponent(allocator) {
    }

    const Rotator<double>& RotationRate() const noexcept {
        return rotationRate_;
    }

    // 오류: 비유한 성분.
    Result<void> SetRotationRate(const Rotator<double>& rate) noexcept;

    // 회전 중심의 상대 위치(cm, 대상 기준)다.
    const Vec3d& PivotTranslation() const noexcept {
        return pivotTranslation_;
    }

    // 오류: 비유한 성분.
    Result<void> SetPivotTranslation(const Vec3d& pivot) noexcept;

    bool IsRotationInLocalSpace() const noexcept {
        return rotationInLocalSpace_;
    }

    void SetRotationInLocalSpace(bool local) noexcept {
        rotationInLocalSpace_ = local;
    }

protected:
    void OnTick(const TickContext& context) noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "rotationRate")
    Rotator<double> rotationRate_ = Rotator<double>(0.0, DefaultYawRate, 0.0);
    ROCKS_PROPERTY(ReadOnly, Name = "pivotTranslation") Vec3d pivotTranslation_;
    ROCKS_PROPERTY(ReadOnly, Name = "rotationInLocalSpace") bool rotationInLocalSpace_ = true;
};

} // namespace rocks
