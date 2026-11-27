#pragma once

#include <Core/Math/Color.h>
#include <Core/Math/Vector3.h>
#include <Meta/Annotations.h>
#include <World/Primitives/PrimitiveTypes.h>
#include <World/Primitives/PrimitiveComponent.h>
#include <World/Primitives/ShapeComponent.h>

namespace rocks {

// 상자 모양이다(UE UBoxComponent). extent는 반 크기(cm)다.
ROCKS_CLASS()
class ROCKS_WORLD_API BoxComponent : public ShapeComponent {
    ROCKS_GENERATED_BODY()

public:
    // 기본 반 크기(cm, UE BoxComponent.cpp:22)다.
    static constexpr double DefaultBoxHalfExtent = 32.0;

    explicit BoxComponent(Allocator& allocator) noexcept : ShapeComponent(allocator) {
    }

    const Vec3d& UnscaledBoxExtent() const noexcept {
        return boxExtent_;
    }

    // extent × 월드 scale(성분별, 부호 유지 — UE GetScaledBoxExtent)이다.
    Vec3d ScaledBoxExtent() const noexcept;

    // 오류: 비유한·음수 성분(UE는 검증하지 않는다 — 음수는 상자가 뒤집힌다).
    Result<void> SetBoxExtent(const Vec3d& extent) noexcept;

    // 반 크기 |extent × scale|의 회전 상자다(28.3절).
    Result<VolumeShape> TryWorldVolumeShape() const noexcept override;

    // 반폭 ScaledBoxExtent 상자를 부풀린 형상이다(UE UBoxComponent::GetCollisionShape).
    CollisionShape GetCollisionShape(double inflation) const noexcept override;

    // 반폭 boxExtent의 상자다(Docs/Design/PhysicsModule 2.4절 — scale은 Physics가 굽는다).
    bool BuildBodyGeometry(BodyGeometry& geometry) const noexcept override;

protected:
    // 지역 (0, E, |E|)다(UE BoxComponent.cpp:122).
    bool TryLocalBounds(ComponentBounds& local) const noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "boxExtent")
    Vec3d boxExtent_ = Vec3d(DefaultBoxHalfExtent, DefaultBoxHalfExtent, DefaultBoxHalfExtent);
};

} // namespace rocks
