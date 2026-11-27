#pragma once

#include <Core/Math/Color.h>
#include <Core/Math/Vector3.h>
#include <Meta/Annotations.h>
#include <World/Primitives/PrimitiveTypes.h>
#include <World/Primitives/PrimitiveComponent.h>
#include <World/Primitives/ShapeComponent.h>

namespace rocks {

// 구 모양이다(UE USphereComponent).
ROCKS_CLASS()
class ROCKS_WORLD_API SphereComponent : public ShapeComponent {
    ROCKS_GENERATED_BODY()

public:
    // 기본 반지름(cm, UE SphereComponent.cpp:19)과 색(:20)이다.
    static constexpr float DefaultSphereRadius = 32.0f;
    static constexpr Color DefaultSphereColor = Color(255, 0, 0, 255);

    explicit SphereComponent(Allocator& allocator) noexcept : ShapeComponent(allocator) {
        shapeColor_ = DefaultSphereColor;
    }

    float UnscaledSphereRadius() const noexcept {
        return sphereRadius_;
    }

    // 반지름 × 최소 축 scale(절댓값 — UE GetShapeScale·GetMinimumAxisScale)이다.
    float ScaledSphereRadius() const noexcept;

    // 오류: 음수·비유한.
    Result<void> SetSphereRadius(float radius) noexcept;

    // 반지름 sphereRadius의 구다(Docs/Design/PhysicsModule 2.4절 — scale은 Physics가 굽는다).
    bool BuildBodyGeometry(BodyGeometry& geometry) const noexcept override;

    // 반지름 ScaledSphereRadius의 구다(28.3절).
    Result<VolumeShape> TryWorldVolumeShape() const noexcept override;

    // 반경 ScaledSphereRadius 구를 부풀린 형상이다(UE USphereComponent::GetCollisionShape).
    CollisionShape GetCollisionShape(double inflation) const noexcept override;

protected:
    // 지역 (0, (R, R, R), R)다(UE SphereComponent.cpp:25).
    bool TryLocalBounds(ComponentBounds& local) const noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "sphereRadius") float sphereRadius_ = DefaultSphereRadius;
};

} // namespace rocks
