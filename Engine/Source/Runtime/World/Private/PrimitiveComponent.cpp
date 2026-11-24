#include <Core/Math/MathUtility.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/LightingChannels.generated.h>
#include <World/PrimitiveComponent.generated.h>
#include <World/PrimitiveComponent.h>
#include <World/SceneComponent.generated.h>

// PrimitiveComponent다(Docs/Design/EngineRuntime 24.3.3–24.3.5절, D2). 데이터만 있고 렌더·충돌
// 연결은 RS1·CR16이다.
namespace rocks {

namespace {

Result<void> InvalidPrimitiveValue() noexcept {
    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

bool IsNonNegativeDistance(float value) noexcept {
    return math::IsFinite(value) && value >= 0.0f;
}

} // namespace

Result<void> PrimitiveComponent::SetMinDrawDistance(float distance) noexcept {
    if (!IsNonNegativeDistance(distance)) {
        return InvalidPrimitiveValue();
    }

    Change(minDrawDistance_, distance);
    return Result<void>::Success();
}

Result<void> PrimitiveComponent::SetDesiredMaxDrawDistance(float distance) noexcept {
    // UE SetCullDistance(PrimitiveComponent.cpp:2461)는 음수를 무시한다 — Rocks는 거부한다.
    if (!IsNonNegativeDistance(distance)) {
        return InvalidPrimitiveValue();
    }

    Change(desiredMaxDrawDistance_, distance);
    return Result<void>::Success();
}

Result<void> PrimitiveComponent::SetCustomDepthStencilValue(int32 value) noexcept {
    if (value < 0 || value > MaxCustomDepthStencilValue) {
        return InvalidPrimitiveValue();
    }

    Change(customDepthStencilValue_, value);
    return Result<void>::Success();
}

Result<void> PrimitiveComponent::SetCustomDepthStencilWriteMask(RendererStencilMask mask) noexcept {
    if (static_cast<uint8>(mask) > static_cast<uint8>(RendererStencilMask::Mask128)) {
        return InvalidPrimitiveValue();
    }

    Change(customDepthStencilWriteMask_, mask);
    return Result<void>::Success();
}

Result<void> PrimitiveComponent::SetTranslucencySortDistanceOffset(float offset) noexcept {
    if (!math::IsFinite(offset)) {
        return InvalidPrimitiveValue();
    }

    Change(translucencySortDistanceOffset_, offset);
    return Result<void>::Success();
}

Result<void> PrimitiveComponent::SetBoundsScale(float scale) noexcept {
    if (!math::IsFinite(scale) || scale <= 0.0f) {
        return InvalidPrimitiveValue();
    }

    if (boundsScale_ != scale) {
        boundsScale_ = scale;
        InvalidateBounds();
        MarkRenderStateDirtyIfPresent();
    }

    return Result<void>::Success();
}

Result<void> PrimitiveComponent::SetCanCharacterStepUpOn(rocks::CanBeCharacterBase value) noexcept {
    if (static_cast<uint8>(value) > static_cast<uint8>(rocks::CanBeCharacterBase::Owner)) {
        return InvalidPrimitiveValue();
    }

    Change(canCharacterStepUpOn_, value);
    return Result<void>::Success();
}

StringView PrimitiveComponent::CollisionProfileName() const noexcept {
    return collisionProfileName_.IsEmpty() ? DefaultCollisionProfileName()
                                           : collisionProfileName_.View();
}

Result<void> PrimitiveComponent::SetCollisionProfileName(StringView name) noexcept {
    // 클래스 기본과 같으면 빈 값(기본 따름)으로 둔다(24.3.4절 — UE FName처럼 대소문자 무시).
    if (EqualsIgnoringAsciiCase(name, DefaultCollisionProfileName())) {
        collisionProfileName_.Clear();
        return Result<void>::Success();
    }

    return collisionProfileName_.TryAssign(name);
}

} // namespace rocks
