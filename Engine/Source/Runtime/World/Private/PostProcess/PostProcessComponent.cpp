#include <Core/Math/MathUtility.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/PostProcess/PostProcessComponent.generated.h>
#include <World/PostProcess/PostProcessComponent.h>
#include <World/PostProcess/PostProcessSettings.generated.h>
#include <World/Primitives/PrimitiveComponent.generated.h>
#include <World/Primitives/PrimitiveComponent.h>
#include <World/Primitives/ShapeComponent.generated.h>
#include <World/Primitives/ShapeComponent.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>
#include <World/ComponentValueChecks.h>

// 후처리 볼륨 컴포넌트다(Docs/Design/EngineRuntime 27.5·28.4절).
namespace rocks {

Result<void> PostProcessComponent::SetSettings(const PostProcessSettings& settings) noexcept {
    if (!IsValidPostProcessSettings(settings)) {
        return component_checks::Invalid();
    }

    if (!(settings_ == settings)) {
        settings_ = settings;
        MarkRenderStateDirtyIfPresent();
    }

    return Result<void>::Success();
}

Result<void> PostProcessComponent::SetPriority(float priority) noexcept {
    if (!math::IsFinite(priority)) {
        return component_checks::Invalid();
    }

    priority_ = priority;
    return Result<void>::Success();
}

Result<void> PostProcessComponent::SetBlendRadius(float radius) noexcept {
    if (!component_checks::NonNegative(radius)) {
        return component_checks::Invalid();
    }

    blendRadius_ = radius;
    return Result<void>::Success();
}

Result<void> PostProcessComponent::SetBlendWeight(float weight) noexcept {
    if (!math::IsFinite(weight)) {
        return component_checks::Invalid();
    }

    blendWeight_ = weight;
    return Result<void>::Success();
}

bool PostProcessComponent::TryPostProcessBounds(ComponentBounds& bounds) const noexcept {
    // UE GetPostProcessBounds(PostProcessComponent.cpp:50).
    const ObjectHandle parent = Parent();
    if (unbound_ || parent.IsNull() || !HasWorld()) {
        return false;
    }

    auto parentBounds = GetWorld().With<PrimitiveComponent>(
        parent, [](PrimitiveComponent& primitive) noexcept { return primitive.Bounds(); });
    if (!parentBounds) {
        return false;
    }

    bounds = parentBounds.Value();
    return true;
}

PostProcessEncompass PostProcessComponent::EncompassesPoint(const Vec3d& point,
                                                            float sphereRadius) const noexcept {
    // UE EncompassesPoint(PostProcessComponent.cpp:170).
    ComponentBounds bounds;
    if (!TryPostProcessBounds(bounds)) {
        return PostProcessEncompass{true, 0.0};
    }

    const double reach = static_cast<double>(blendRadius_) + static_cast<double>(sphereRadius);
    const Vec3d offset = point - bounds.origin;
    if (!(math::Abs(offset.x) <= bounds.boxExtent.x + reach &&
          math::Abs(offset.y) <= bounds.boxExtent.y + reach &&
          math::Abs(offset.z) <= bounds.boxExtent.z + reach)) {
        return PostProcessEncompass{false, PostProcessOutsideDistance};
    }

    // 부모가 Shape면 정확한 형상 거리다(UE GetSquaredDistanceToCollision — 물리 없이). 형상 거리를
    // 못 재면(비유한 변환 — 위 상자 검사가 이미 거의 걸러낸다) 밖으로 본다.
    struct ShapeQuery {
        bool measured = false;
        double distance = 0.0;
    };

    auto shape = GetWorld().With<ShapeComponent>(Parent(), [&](ShapeComponent& parent) noexcept {
        auto distance = parent.TryDistanceToShape(point);
        return distance ? ShapeQuery{true, distance.Value()} : ShapeQuery{};
    });
    if (!shape) {
        // Shape가 아닌 primitive(물리 객체가 없다)는 UE 마지막 분기처럼 어디서나 안이다(:253).
        return PostProcessEncompass{true, 0.0};
    }

    if (!shape.Value().measured) {
        return PostProcessEncompass{false, PostProcessOutsideDistance};
    }

    const double distance = shape.Value().distance;
    return PostProcessEncompass{distance <= static_cast<double>(sphereRadius), distance};
}

PostProcessVolumeProperties PostProcessComponent::VolumeProperties() const noexcept {
    // UE GetProperties(PostProcessComponent.h:65).
    ComponentBounds bounds;
    const bool bounded = TryPostProcessBounds(bounds);
    PostProcessVolumeProperties properties;
    properties.enabled = postProcessEnabled_ && IsVisible();
    properties.unbound = unbound_ || !bounded;
    properties.blendRadius = blendRadius_;
    properties.blendWeight = blendWeight_;
    properties.priority = priority_;
    // 상자 부피 = 각 축 전체 길이(반 크기 × 2)의 곱이다(UE FBox::GetVolume).
    const Vec3d full = bounds.boxExtent + bounds.boxExtent;
    properties.size = bounded ? full.x * full.y * full.z : PostProcessUnboundSize;
    return properties;
}

} // namespace rocks
