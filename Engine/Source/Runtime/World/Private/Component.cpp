#include <Platform/Fatal.h>
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

} // namespace rocks
