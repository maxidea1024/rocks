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
    // World 장부 밖 Actor(그래프 로드 등)는 대표 간선을 바꿀 Registry 문맥이 없다.
    if (world_ == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 이 Actor가 소유한 SceneComponent만 대표가 될 수 있다(null은 대표를 비운다).
    if (!component.IsNull() && !components_.Contains(component)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 강한 참조 필드는 Registry로만 쓴다: 대상 생존·타입(SceneComponent) 검사와 Marking 중 삽입
    // 장벽을 거친다. root는 늘 components_ 안의 객체라 새 생존 경로를 만들지 않는다.
    World& world = *world_;
    auto set = world.Objects().SetReference(world.Access(), Handle(), rootComponent_, component);
    if (!set) {
        return Result<void>::Failure(
            set.Error() == ErrorCode::TypeMismatch ? ErrorCode::InvalidArgument : set.Error());
    }

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
