#include <Platform/Fatal.h>
#include <World/SceneComponent.h>
#include <World/World.h>

namespace rocks {

World& Actor::GetWorld() const noexcept {
    if (world_ == nullptr) {
        platform::Fatal("Actor::GetWorld called before the actor was spawned into a world");
    }
    return *world_;
}

ActorState Actor::GetState() const noexcept {
    if (world_ == nullptr) {
        return ActorState::Constructed;
    }

    auto state = world_->GetActorState(Handle());
    // 레코드가 이미 정리되었으면 파괴 대기 이후 단계로 본다.
    return state ? state.Value() : ActorState::PendingDestroy;
}

Result<void> Actor::SetEnabled(bool enabled) noexcept {
    enabled_ = enabled;
    if (world_ == nullptr) {
        return Result<void>::Success();
    }

    return world_->SetActorEnabled(Handle(), enabled);
}

Result<void> Actor::Destroy() noexcept {
    if (world_ == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return world_->DestroyActor(Handle());
}

Result<void> Actor::SetRootComponent(ObjectHandle component) noexcept {
    if (component.IsNull()) {
        rootComponent_ = {};
        return Result<void>::Success();
    }

    // 이 Actor가 소유한 SceneComponent만 대표가 될 수 있다.
    if (!components_.Contains(component)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto scene = GetWorld().Resolve<SceneComponent>(component);
    if (!scene) {
        return Result<void>::Failure(
            scene.Error() == ErrorCode::TypeMismatch ? ErrorCode::InvalidArgument : scene.Error());
    }

    rootComponent_ = component;
    return Result<void>::Success();
}

Result<void> Actor::SetTickEnabled(bool enabled) noexcept {
    tickEnabled_ = enabled;
    if (world_ == nullptr) {
        return Result<void>::Success();
    }

    return world_->SetActorTickEnabled(Handle(), enabled);
}

} // namespace rocks
