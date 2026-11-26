#include <Core/Math/MathUtility.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Environment/DecalComponent.generated.h>
#include <World/Environment/DecalComponent.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>
#include <World/ComponentValueChecks.h>

// decal 컴포넌트다(Docs/Design/EngineRuntime 27.4절, D5).
namespace rocks {

DecalComponent::DecalComponent(Allocator& allocator) noexcept : SceneComponent(allocator) {
    // 수명이 있을 때만 Tick한다(27.4절).
    PrimaryTick().tick = true;
    (void)SetTickEnabled(false);
}

void DecalComponent::SetSortOrder(int32 order) noexcept {
    if (sortOrder_ != order) {
        sortOrder_ = order;
        MarkRenderStateDirtyIfPresent();
    }
}

Result<void> DecalComponent::SetFadeScreenSize(float size) noexcept {
    if (!math::IsFinite(size)) {
        return component_checks::Invalid();
    }

    fadeScreenSize_ = size;
    MarkRenderStateDirtyIfPresent();
    return Result<void>::Success();
}

Result<void> DecalComponent::SetFadeOut(float startDelay, float duration,
                                        bool destroyOwnerAfterFade) noexcept {
    // UE SetFadeOut(DecalComponent.cpp:318) — r.Decal.FadeDurationScale은 Rocks에 없어 1이다.
    // 합(수명)이 넘쳐도 거부하도록 바꾸기 전에 검사한다.
    if (!math::IsFinite(startDelay) || !math::IsFinite(duration) ||
        !math::IsFinite(startDelay + duration)) {
        return component_checks::Invalid();
    }

    fadeStartDelay_ = startDelay;
    fadeDuration_ = duration;
    destroyOwnerAfterFade_ = destroyOwnerAfterFade;
    MarkRenderStateDirtyIfPresent();
    return SetLifeSpan(startDelay + duration);
}

Result<void> DecalComponent::SetFadeIn(float startDelay, float duration) noexcept {
    if (!math::IsFinite(startDelay) || !math::IsFinite(duration)) {
        return component_checks::Invalid();
    }

    fadeInStartDelay_ = startDelay;
    fadeInDuration_ = duration;
    MarkRenderStateDirtyIfPresent();
    return Result<void>::Success();
}

Result<void> DecalComponent::SetDecalSize(const Vec3d& size) noexcept {
    if (!component_checks::FiniteVector(size)) {
        return component_checks::Invalid();
    }

    if (!(decalSize_ == size)) {
        decalSize_ = size;
        InvalidateBounds();
        MarkRenderStateDirtyIfPresent();
    }

    return Result<void>::Success();
}

Result<void> DecalComponent::SetDecalColor(const LinearColor& color) noexcept {
    if (!color.IsFinite()) {
        return component_checks::Invalid();
    }

    decalColor_ = color;
    MarkRenderStateDirtyIfPresent();
    return Result<void>::Success();
}

Result<void> DecalComponent::SetLifeSpan(float seconds) noexcept {
    // UE SetLifeSpan(:270): 0보다 크면 타이머, 아니면 취소.
    if (!math::IsFinite(seconds)) {
        return component_checks::Invalid();
    }

    remainingLife_ = seconds > 0.0f ? static_cast<double>(seconds) : 0.0;
    return SetTickEnabled(remainingLife_ > 0.0);
}

void DecalComponent::OnStart() noexcept {
    // UE BeginPlay(:507).
    (void)SetLifeSpan(fadeStartDelay_ + fadeDuration_);
}

void DecalComponent::OnTick(const TickContext& context) noexcept {
    if (!(remainingLife_ > 0.0)) {
        return;
    }

    remainingLife_ -= context.deltaSeconds;
    if (remainingLife_ > 0.0) {
        return;
    }

    // UE LifeSpanCallback(:285): 컴포넌트를 파괴하고, fade가 있고 플래그가 켜져 있으면 Actor도.
    remainingLife_ = 0.0;
    (void)SetTickEnabled(false);
    World& world = GetWorld();
    const ObjectHandle owner = GetOwner();
    const bool destroyOwner =
        destroyOwnerAfterFade_ && (fadeDuration_ > 0.0f || fadeStartDelay_ > 0.0f);
    (void)world.DestroyComponent(Handle());
    if (destroyOwner && !owner.IsNull()) {
        (void)world.DestroyActor(owner);
    }
}

bool DecalComponent::TryLocalBounds(ComponentBounds& local) const noexcept {
    // UE CalcBounds(:472): 지역 (0, decalSize, |decalSize|).
    local = ComponentBounds{};
    local.boxExtent = decalSize_;
    local.sphereRadius = decalSize_.Length();
    return true;
}

} // namespace rocks
