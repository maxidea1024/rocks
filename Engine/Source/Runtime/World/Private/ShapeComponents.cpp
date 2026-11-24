#include <Core/Math/MathUtility.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/LightingChannels.generated.h>
#include <World/PrimitiveComponent.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/ShapeComponent.generated.h>
#include <World/ShapeComponent.h>

// Shape 컴포넌트다(Docs/Design/EngineRuntime 24.3.6절, D2). 기하·bounds는 끝까지 동작하고 충돌은
// CR16이다.
namespace rocks {

namespace {

Result<void> InvalidShapeValue() noexcept {
    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

bool IsNonNegativeSize(float value) noexcept {
    return math::IsFinite(value) && value >= 0.0f;
}

bool IsNonNegativeSize(double value) noexcept {
    return math::IsFinite(value) && value >= 0.0;
}

} // namespace

// --- ShapeComponent ---

Result<void> ShapeComponent::SetLineThickness(float thickness) noexcept {
    if (!IsNonNegativeSize(thickness)) {
        return InvalidShapeValue();
    }

    Change(lineThickness_, thickness);
    return Result<void>::Success();
}

ComponentBounds ShapeComponent::CalcBounds(const Transform& world) const noexcept {
    ComponentBounds local;
    (void)TryLocalBounds(local);
    return TransformComponentBounds(local, world.ToMatrix());
}

// --- BoxComponent ---

Vec3d BoxComponent::ScaledBoxExtent() const noexcept {
    return boxExtent_ * WorldScale3D();
}

Result<void> BoxComponent::SetBoxExtent(const Vec3d& extent) noexcept {
    if (!IsNonNegativeSize(extent.x) || !IsNonNegativeSize(extent.y) ||
        !IsNonNegativeSize(extent.z)) {
        return InvalidShapeValue();
    }

    if (!(boxExtent_ == extent)) {
        boxExtent_ = extent;
        InvalidateBounds();
        MarkRenderStateDirtyIfPresent();
    }

    return Result<void>::Success();
}

bool BoxComponent::TryLocalBounds(ComponentBounds& local) const noexcept {
    // UE FBoxSphereBounds(FBox(-E, E)): 중심 0, extent E, 반지름 |E|.
    local = ComponentBounds{};
    local.boxExtent = boxExtent_;
    local.sphereRadius = boxExtent_.Length();
    return true;
}

// --- SphereComponent ---

float SphereComponent::ScaledSphereRadius() const noexcept {
    const Vec3d scale = WorldScale3D();
    const double minimum =
        math::Min(math::Min(math::Abs(scale.x), math::Abs(scale.y)), math::Abs(scale.z));
    return sphereRadius_ * static_cast<float>(minimum);
}

Result<void> SphereComponent::SetSphereRadius(float radius) noexcept {
    if (!IsNonNegativeSize(radius)) {
        return InvalidShapeValue();
    }

    if (sphereRadius_ != radius) {
        sphereRadius_ = radius;
        InvalidateBounds();
        MarkRenderStateDirtyIfPresent();
    }

    return Result<void>::Success();
}

bool SphereComponent::TryLocalBounds(ComponentBounds& local) const noexcept {
    const double radius = static_cast<double>(sphereRadius_);
    local = ComponentBounds{};
    local.boxExtent = Vec3d(radius, radius, radius);
    local.sphereRadius = radius;
    return true;
}

// --- CapsuleComponent ---

float CapsuleComponent::ScaledCapsuleRadius() const noexcept {
    // UE GetScaledCapsuleRadius: min(scale.x, scale.y)(부호 그대로).
    const Vec3d scale = WorldScale3D();
    return capsuleRadius_ * static_cast<float>(math::Min(scale.x, scale.y));
}

float CapsuleComponent::ScaledCapsuleHalfHeight() const noexcept {
    return capsuleHalfHeight_ * static_cast<float>(WorldScale3D().z);
}

Result<void> CapsuleComponent::SetCapsuleSize(float radius, float halfHeight) noexcept {
    if (!IsNonNegativeSize(radius) || !IsNonNegativeSize(halfHeight)) {
        return InvalidShapeValue();
    }

    // UE: 반 높이는 반지름 이상(반구 둘이 들어간다).
    const float raisedHalfHeight = math::Max(halfHeight, radius);
    if (capsuleRadius_ != radius || capsuleHalfHeight_ != raisedHalfHeight) {
        capsuleRadius_ = radius;
        capsuleHalfHeight_ = raisedHalfHeight;
        InvalidateBounds();
        MarkRenderStateDirtyIfPresent();
    }

    return Result<void>::Success();
}

bool CapsuleComponent::TryLocalBounds(ComponentBounds& local) const noexcept {
    const double radius = static_cast<double>(capsuleRadius_);
    const double halfHeight = static_cast<double>(capsuleHalfHeight_);
    local = ComponentBounds{};
    local.boxExtent = Vec3d(radius, radius, halfHeight);
    local.sphereRadius = halfHeight;
    return true;
}

} // namespace rocks
