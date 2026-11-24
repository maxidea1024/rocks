#include <Core/Math/MathUtility.h>
#include <World/Actor.generated.h>
#include <World/CameraComponent.generated.h>
#include <World/CameraComponent.h>
#include <World/Component.generated.h>
#include <World/SceneComponent.generated.h>

// CameraComponent다(Docs/Design/EngineRuntime 23.4절, CM0c).
namespace rocks {

Result<void> CameraComponent::SetFieldOfView(double degrees) noexcept {
    if (!math::IsFinite(degrees) || degrees < MinFieldOfView || degrees > MaxFieldOfView) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    fieldOfView_ = degrees;
    return Result<void>::Success();
}

Result<void> CameraComponent::SetOrthoWidth(double width) noexcept {
    if (!math::IsFinite(width) || width <= 0.0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    orthoWidth_ = width;
    return Result<void>::Success();
}

Result<void> CameraComponent::SetAspectRatio(double ratio) noexcept {
    if (!math::IsFinite(ratio) || ratio <= 0.0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    aspectRatio_ = ratio;
    return Result<void>::Success();
}

void CameraComponent::GetCameraView(double, CameraViewInfo& out) const noexcept {
    // 위치·회전은 Component 월드 Transform이다(분해할 수 없으면 위치만 — 회전은 그대로 둔다).
    auto world = TryWorldTransform();
    if (world) {
        out.location = world.Value().translation;
        out.rotation = world.Value().rotation;
    } else {
        out.location = WorldLocation();
    }

    out.fieldOfView = fieldOfView_;
    out.projection = projection_;
    out.orthoWidth = orthoWidth_;
    out.aspectRatio = aspectRatio_;
    out.constrainAspectRatio = constrainAspectRatio_;
    out.usePawnControlRotation = usePawnControlRotation_;
}

} // namespace rocks
