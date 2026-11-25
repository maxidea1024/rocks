#include <Collision/BodySettings.generated.h>
#include <Collision/CollisionSettings.generated.h>
#include <Collision/CollisionTypes.generated.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogContext.h>
#include <Core/Math/MathUtility.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Primitives/PrimitiveComponent.generated.h>
#include <World/Primitives/PrimitiveComponent.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>
#include <World/WorldLog.h>
#include <World/ComponentValueChecks.h>

// PrimitiveComponent다(Docs/Design/EngineRuntime 24.3.3–24.3.5절, D2). 데이터만 있고 렌더·충돌
// 연결은 RS1·CR16이다.
namespace rocks {

Result<void> PrimitiveComponent::SetMinDrawDistance(float distance) noexcept {
    if (!component_checks::NonNegative(distance)) {
        return component_checks::Invalid();
    }

    Change(minDrawDistance_, distance);
    return Result<void>::Success();
}

Result<void> PrimitiveComponent::SetDesiredMaxDrawDistance(float distance) noexcept {
    // UE SetCullDistance(PrimitiveComponent.cpp:2461)는 음수를 무시한다 — Rocks는 거부한다.
    if (!component_checks::NonNegative(distance)) {
        return component_checks::Invalid();
    }

    Change(desiredMaxDrawDistance_, distance);
    return Result<void>::Success();
}

Result<void> PrimitiveComponent::SetCustomDepthStencilValue(int32 value) noexcept {
    if (value < 0 || value > MaxCustomDepthStencilValue) {
        return component_checks::Invalid();
    }

    Change(customDepthStencilValue_, value);
    return Result<void>::Success();
}

Result<void> PrimitiveComponent::SetCustomDepthStencilWriteMask(RendererStencilMask mask) noexcept {
    if (static_cast<uint8>(mask) > static_cast<uint8>(RendererStencilMask::Mask128)) {
        return component_checks::Invalid();
    }

    Change(customDepthStencilWriteMask_, mask);
    return Result<void>::Success();
}

Result<void> PrimitiveComponent::SetTranslucencySortDistanceOffset(float offset) noexcept {
    if (!math::IsFinite(offset)) {
        return component_checks::Invalid();
    }

    Change(translucencySortDistanceOffset_, offset);
    return Result<void>::Success();
}

Result<void> PrimitiveComponent::SetBoundsScale(float scale) noexcept {
    if (!math::IsFinite(scale) || scale <= 0.0f) {
        return component_checks::Invalid();
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
        return component_checks::Invalid();
    }

    Change(canCharacterStepUpOn_, value);
    return Result<void>::Success();
}

StringView PrimitiveComponent::CollisionProfileName() const noexcept {
    return collisionProfileName_.IsEmpty() ? DefaultCollisionProfileName()
                                           : collisionProfileName_.View();
}

namespace {

// 해석한 값이 표의 profile이면 body 사용자 지정 칸에 옮긴다(UE ReadConfig가 body에 복사 —
// CollisionData 5절).
Result<void> CopyResolvedProfile(const CollisionProfileTable& table,
                                 const ResolvedCollision& resolved, BodySettings& body) noexcept {
    if (!resolved.fromProfile) {
        return Result<void>::Success();
    }

    return MakeCustomCollision(table, resolved, body);
}

bool KnownCollisionEnabled(CollisionEnabled value) noexcept {
    return static_cast<uint32>(value) < CollisionEnabledCount;
}

bool KnownCollisionChannel(CollisionChannel channel) noexcept {
    return static_cast<uint32>(channel) < CollisionChannelCount;
}

bool KnownCollisionResponse(CollisionResponse response) noexcept {
    return static_cast<uint32>(response) < CollisionResponseCount;
}

} // namespace

Result<void> PrimitiveComponent::SetCollisionProfileName(StringView name) noexcept {
    // 클래스 기본과 같으면 빈 값(기본 따름)으로 둔다(24.3.4절 — UE FName처럼 대소문자 무시).
    String previous = Move(collisionProfileName_);
    collisionProfileName_ = String(previous.GetAllocator());
    if (!EqualsIgnoringAsciiCase(name, DefaultCollisionProfileName())) {
        if (auto assigned = collisionProfileName_.TryAssign(name); !assigned) {
            collisionProfileName_ = Move(previous);
            return assigned;
        }
    }

    if (!HasWorld()) {
        return Result<void>::Success();
    }

    // 표에 있으면 profile 값을 body에 옮긴다. 없으면 body는 마지막 해석 값 그대로다.
    const CollisionProfileTable& table = GetWorld().CollisionProfiles();
    const ResolvedCollision resolved =
        ResolveCollision(table, CollisionProfileName(), bodySettings_);
    if (auto copied = CopyResolvedProfile(table, resolved, bodySettings_); !copied) {
        collisionProfileName_ = Move(previous);
        return copied;
    }

    return Result<void>::Success();
}

Result<ResolvedCollision> PrimitiveComponent::TryResolveCollision() const noexcept {
    if (!HasWorld()) {
        return Result<ResolvedCollision>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<ResolvedCollision>::Success(
        ResolveCollision(GetWorld().CollisionProfiles(), CollisionProfileName(), bodySettings_));
}

template <class Edit>
Result<void> PrimitiveComponent::EditCustomCollision(Edit&& edit) noexcept {
    if (!HasWorld()) {
        return component_checks::Invalid();
    }

    const CollisionProfileTable& table = GetWorld().CollisionProfiles();
    ResolvedCollision resolved = ResolveCollision(table, CollisionProfileName(), bodySettings_);
    if (!edit(resolved)) {
        return Result<void>::Success();
    }

    // 이름을 먼저 준비해 할당 실패 때 아무것도 바꾸지 않는다.
    String custom(collisionProfileName_.GetAllocator());
    if (auto assigned = custom.TryAssign(CustomCollisionProfileName); !assigned) {
        return assigned;
    }

    if (auto made = MakeCustomCollision(table, resolved, bodySettings_); !made) {
        return made;
    }

    collisionProfileName_ = Move(custom);
    return Result<void>::Success();
}

Result<void> PrimitiveComponent::SetCollisionEnabled(CollisionEnabled enabled) noexcept {
    if (!KnownCollisionEnabled(enabled)) {
        return component_checks::Invalid();
    }

    return EditCustomCollision([enabled](ResolvedCollision& resolved) noexcept {
        if (resolved.collisionEnabled == enabled) {
            return false;
        }

        resolved.collisionEnabled = enabled;
        return true;
    });
}

Result<void> PrimitiveComponent::SetCollisionObjectType(CollisionChannel channel) noexcept {
    if (!KnownCollisionChannel(channel)) {
        return component_checks::Invalid();
    }

    // UE FBodyInstance::SetObjectType(BodyInstance.cpp:675)은 값을 비교하지 않고 언제나 Custom으로
    // 바꾼다(Enabled·응답과 다르다).
    return EditCustomCollision([channel](ResolvedCollision& resolved) noexcept {
        resolved.objectType = channel;
        return true;
    });
}

Result<void>
PrimitiveComponent::SetCollisionResponseToChannel(CollisionChannel channel,
                                                  CollisionResponse response) noexcept {
    if (!KnownCollisionChannel(channel) || !KnownCollisionResponse(response)) {
        return component_checks::Invalid();
    }

    return EditCustomCollision([channel, response](ResolvedCollision& resolved) noexcept {
        return resolved.responses.Set(channel, response);
    });
}

Result<void>
PrimitiveComponent::SetCollisionResponseToAllChannels(CollisionResponse response) noexcept {
    if (!KnownCollisionResponse(response)) {
        return component_checks::Invalid();
    }

    return EditCustomCollision([response](ResolvedCollision& resolved) noexcept {
        return resolved.responses.SetAll(response);
    });
}

Result<void> PrimitiveComponent::OnRegister() noexcept {
    if (auto base = SceneComponent::OnRegister(); !base) {
        return base;
    }

    const CollisionProfileTable& table = GetWorld().CollisionProfiles();
    const ResolvedCollision resolved =
        ResolveCollision(table, CollisionProfileName(), bodySettings_);
    if (resolved.profileMissing) {
        // UE는 경고 없이 이름을 Custom으로 바꾼다. Rocks는 이름을 두고 한 번 알린다(CollisionData
        // 5절).
        LogContextScope context("World", GetWorld().Name());
        Log::Warning(LogWorld,
                     "Primitive component #{Slot} collision profile '{Profile}' is not in the "
                     "profile table; using its custom collision values ({Code})",
                     Handle().slot, CollisionProfileName(), DiagCollisionProfileMissing);
        return Result<void>::Success();
    }

    return CopyResolvedProfile(table, resolved, bodySettings_);
}

} // namespace rocks
