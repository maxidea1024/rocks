#pragma once

#include <Core/Math/Color.h>
#include <Core/Math/Vector3.h>
#include <Meta/Annotations.h>
#include <World/Primitives/PrimitiveTypes.h>
#include <World/Primitives/PrimitiveComponent.h>
#include <World/Primitives/ShapeComponent.h>

namespace rocks {

// 캡슐 모양이다(UE UCapsuleComponent). 축은 지역 Z이고 반 높이는 반구를 포함한다(늘 반지름 이상).
ROCKS_CLASS()
class ROCKS_WORLD_API CapsuleComponent : public ShapeComponent {
    ROCKS_GENERATED_BODY()

public:
    // 기본 반지름·반 높이(cm, UE CapsuleComponent.cpp:23–24)다.
    static constexpr float DefaultCapsuleRadius = 22.0f;
    static constexpr float DefaultCapsuleHalfHeight = 44.0f;
    // UE 물리 캡슐 크기 하한(cm — FKSphylElem 0.1)이다.
    static constexpr double CapsuleMinimumPhysicsSize = 0.1;

    explicit CapsuleComponent(Allocator& allocator) noexcept : ShapeComponent(allocator) {
    }

    float UnscaledCapsuleRadius() const noexcept {
        return capsuleRadius_;
    }

    float UnscaledCapsuleHalfHeight() const noexcept {
        return capsuleHalfHeight_;
    }

    // 반구를 뺀 반 높이다(UE GetUnscaledCapsuleHalfHeight_WithoutHemisphere).
    float UnscaledCapsuleHalfHeightWithoutHemisphere() const noexcept {
        return capsuleHalfHeight_ - capsuleRadius_;
    }

    // 반지름 × min(scale.x, scale.y)·반 높이 × scale.z다(UE GetScaledCapsuleRadius·HalfHeight).
    float ScaledCapsuleRadius() const noexcept;
    float ScaledCapsuleHalfHeight() const noexcept;

    float ScaledCapsuleHalfHeightWithoutHemisphere() const noexcept {
        return ScaledCapsuleHalfHeight() - ScaledCapsuleRadius();
    }

    // 반 높이는 max(halfHeight, radius)로 올린다(UE SetCapsuleSize — CapsuleComponent.cpp:171).
    // 오류: 음수·비유한(값 유지).
    Result<void> SetCapsuleSize(float radius, float halfHeight) noexcept;

    Result<void> SetCapsuleRadius(float radius) noexcept {
        return SetCapsuleSize(radius, capsuleHalfHeight_);
    }

    Result<void> SetCapsuleHalfHeight(float halfHeight) noexcept {
        return SetCapsuleSize(capsuleRadius_, halfHeight);
    }

    // UE 물리 형상 크기의 캡슐이다(28.3절 — 반지름은 max(|sx|, |sy|) 배, 0.1 cm 하한).
    Result<VolumeShape> TryWorldVolumeShape() const noexcept override;

    // ScaledCapsuleRadius·ScaledCapsuleHalfHeight 캡슐을 부풀린 형상이다(UE
    // UCapsuleComponent::GetCollisionShape).
    CollisionShape GetCollisionShape(double inflation) const noexcept override;

    // 반경·반높이(반구 포함, 로컬 +Z)의 캡슐이다(Docs/Design/PhysicsModule 2.4절 — scale은
    // Physics가 굽는다).
    Result<void> BuildBodyGeometry(BodyGeometry& geometry) const noexcept override;

    // UE UCapsuleComponent::IsZeroExtent(반경·반높이 모두 0).
    bool IsZeroExtent() const noexcept override;

protected:
    // 지역 (0, (r, r, h), h)다(UE CapsuleComponent.cpp:104).
    bool TryLocalBounds(ComponentBounds& local) const noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "capsuleRadius") float capsuleRadius_ = DefaultCapsuleRadius;
    ROCKS_PROPERTY(ReadOnly, Name = "capsuleHalfHeight")
    float capsuleHalfHeight_ = DefaultCapsuleHalfHeight;
};

} // namespace rocks
