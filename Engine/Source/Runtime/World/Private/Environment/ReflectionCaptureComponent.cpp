#include <Core/Math/MathUtility.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Environment/ReflectionCaptureComponent.generated.h>
#include <World/Environment/ReflectionCaptureComponent.h>
#include <World/Environment/ReflectionSourceType.generated.h>
#include <World/SceneComponent.generated.h>
#include "../ComponentValueChecks.h"

// 반사 캡처 기반 컴포넌트다(Docs/Design/EngineRuntime 27.6절, D5).
namespace rocks {

Result<void> ReflectionCaptureComponent::SetSourceType(ReflectionSourceType type) noexcept {
    if (static_cast<uint8>(type) > static_cast<uint8>(ReflectionSourceType::SpecifiedCubemap)) {
        return component_checks::Invalid();
    }

    sourceType_ = type;
    MarkRenderStateDirtyIfPresent();
    return Result<void>::Success();
}

Result<void> ReflectionCaptureComponent::SetSourceCubemapAngle(float degrees) noexcept {
    if (!math::IsFinite(degrees)) {
        return component_checks::Invalid();
    }

    sourceCubemapAngle_ = degrees;
    MarkRenderStateDirtyIfPresent();
    return Result<void>::Success();
}

Result<void> ReflectionCaptureComponent::SetBrightness(float brightness) noexcept {
    if (!math::IsFinite(brightness)) {
        return component_checks::Invalid();
    }

    brightness_ = brightness;
    MarkRenderStateDirtyIfPresent();
    return Result<void>::Success();
}

Result<void> ReflectionCaptureComponent::SetCaptureOffset(const Vec3d& offset) noexcept {
    if (!component_checks::FiniteVector(offset)) {
        return component_checks::Invalid();
    }

    captureOffset_ = offset;
    MarkRenderStateDirtyIfPresent();
    return Result<void>::Success();
}

Result<void>
ReflectionCaptureComponent::SetRuntimeSkylightScale(const LinearColor& scale) noexcept {
    if (!scale.IsFinite()) {
        return component_checks::Invalid();
    }

    runtimeSkylightScale_ = scale;
    MarkRenderStateDirtyIfPresent();
    return Result<void>::Success();
}

void ReflectionCaptureComponent::SetFiniteFarPlane(bool finite) noexcept {
    finiteFarPlane_ = finite;
    MarkRenderStateDirtyIfPresent();
}

void ReflectionCaptureComponent::SetRuntimeCapture(bool runtime) noexcept {
    runtimeCapture_ = runtime;
    MarkRenderStateDirtyIfPresent();
}

Result<void> ReflectionCaptureComponent::SetMaxViewDistance(float distance) noexcept {
    if (!math::IsFinite(distance)) {
        return component_checks::Invalid();
    }

    maxViewDistance_ = distance;
    MarkRenderStateDirtyIfPresent();
    return Result<void>::Success();
}

} // namespace rocks
