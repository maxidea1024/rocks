#include <Object/Object.h>
#include <Object/ObjectRegistry.h>

namespace rocks {

Result<void> Object::Destroy() noexcept {
    if (!registry_) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }
    return registry_->DestroySelf(handle_, this);
}

Result<ObjectLifecycle> Object::InspectLifecycle() const noexcept {
    if (!registry_) {
        return Result<ObjectLifecycle>::Failure(ErrorCode::StaleHandle);
    }
    return registry_->InspectSelf(handle_, this);
}

Result<bool> Object::IsDestroyRequested() const noexcept {
    auto state = InspectLifecycle();
    if (!state) {
        return Result<bool>::Failure(state.Error());
    }
    if (state.Value() == ObjectLifecycle::Alive) {
        return Result<bool>::Success(false);
    }
    if (state.Value() == ObjectLifecycle::DestroyRequested) {
        return Result<bool>::Success(true);
    }
    return Result<bool>::Failure(ErrorCode::Destroyed);
}

} // namespace rocks
