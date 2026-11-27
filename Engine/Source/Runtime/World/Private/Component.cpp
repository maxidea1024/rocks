#include "WorldTags.h"

#include <Platform/Fatal.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Level.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>

namespace rocks {

World& Component::GetWorld() const noexcept {
    if (world_ == nullptr) {
        platform::Fatal("Component::GetWorld called before the component was added to an actor");
    }

    return *world_;
}

ComponentState Component::GetState() const noexcept {
    return state_;
}

Result<void> Component::SetEnabled(bool enabled) noexcept {
    enabled_ = enabled;
    if (world_ == nullptr) {
        return Result<void>::Success();
    }

    return world_->SetComponentEnabled(Handle(), enabled);
}

Result<void> Component::Destroy() noexcept {
    if (world_ == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return world_->DestroyComponent(Handle());
}

Result<void> Component::SetTickEnabled(bool enabled) noexcept {
    tickEnabled_ = enabled;
    if (world_ == nullptr) {
        return Result<void>::Success();
    }

    return world_->SetComponentTickEnabled(Handle(), enabled);
}

// 태그(Docs/Design/EngineRuntime 31.4절).

bool Component::HasTag(Name tag) const noexcept {
    return world_tags::Contains(componentTags_.View(), tag);
}

Result<void> Component::AddTag(Name tag) noexcept {
    if (world_ == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (GetState() == ComponentState::PendingDestroy) {
        return Result<void>::Failure(ErrorCode::Destroyed);
    }

    return world_tags::Add(componentTags_, world_->Objects(), tag);
}

Result<void> Component::RemoveTag(Name tag) noexcept {
    if (world_ == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (GetState() == ComponentState::PendingDestroy) {
        return Result<void>::Failure(ErrorCode::Destroyed);
    }

    return world_tags::Remove(componentTags_, tag);
}

Result<void> Component::SetTags(Span<const Name> tags) noexcept {
    if (world_ == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (GetState() == ComponentState::PendingDestroy) {
        return Result<void>::Failure(ErrorCode::Destroyed);
    }

    return world_tags::Set(componentTags_, world_->Objects(), tags);
}

} // namespace rocks
