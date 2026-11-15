#include <Object/Object.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Fatal.h>

namespace rocks {

Result<void> Object::Destroy() noexcept {
    if (!registry_) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }
    return registry_->DestroySelf(handle_, this);
}

Result<TypeLease> Object::TryGetType() const noexcept {
    if (!registry_) {
        return Result<TypeLease>::Failure(ErrorCode::StaleHandle);
    }
    return registry_->AcquireTypeSelf(handle_, this);
}

Result<ObjectLifecycle> Object::InspectLifecycle() const noexcept {
    if (!registry_) {
        return Result<ObjectLifecycle>::Failure(ErrorCode::StaleHandle);
    }
    return registry_->InspectSelf(handle_, this);
}

namespace {

// 프로그래머 오류를 알 수 없는 타입으로 바꾸지 않고 할당 없이 원인을 남긴다.
[[noreturn]] void FailTypeQuery(ErrorCode error) noexcept {
    switch (error) {
    case ErrorCode::InvalidArgument:
        platform::Fatal("Object::GetType failed: InvalidArgument");
    case ErrorCode::OutOfMemory:
        platform::Fatal("Object::GetType failed: OutOfMemory");
    case ErrorCode::Overflow:
        platform::Fatal("Object::GetType failed: Overflow");
    case ErrorCode::Busy:
        platform::Fatal("Object::GetType failed: Busy");
    case ErrorCode::NotFound:
        platform::Fatal("Object::GetType failed: NotFound");
    case ErrorCode::AlreadyExists:
        platform::Fatal("Object::GetType failed: AlreadyExists");
    case ErrorCode::StaleHandle:
        platform::Fatal("Object::GetType failed: StaleHandle");
    case ErrorCode::WrongThread:
        platform::Fatal("Object::GetType failed: WrongThread");
    case ErrorCode::WrongContext:
        platform::Fatal("Object::GetType failed: WrongContext");
    case ErrorCode::Destroyed:
        platform::Fatal("Object::GetType failed: Destroyed");
    case ErrorCode::Unsupported:
        platform::Fatal("Object::GetType failed: Unsupported");
    case ErrorCode::CapacityExceeded:
        platform::Fatal("Object::GetType failed: CapacityExceeded");
    case ErrorCode::DependencyFailed:
        platform::Fatal("Object::GetType failed: DependencyFailed");
    case ErrorCode::Cancelled:
        platform::Fatal("Object::GetType failed: Cancelled");
    case ErrorCode::ShuttingDown:
        platform::Fatal("Object::GetType failed: ShuttingDown");
    case ErrorCode::TypeMismatch:
        platform::Fatal("Object::GetType failed: TypeMismatch");
    case ErrorCode::AuthenticationFailed:
        platform::Fatal("Object::GetType failed: AuthenticationFailed");
    case ErrorCode::None:
        platform::Fatal("Object::GetType failed: unexpected success code");
    }
    platform::Fatal("Object::GetType failed: unknown error code");
}

// 오류를 기본 상태로 바꾸지 않고, 할당 없이 원래 원인을 진단한다.
[[noreturn]] void FailDestroyRequestedQuery(ErrorCode error) noexcept {
    switch (error) {
    case ErrorCode::InvalidArgument:
        platform::Fatal("Object::IsDestroyRequested failed: InvalidArgument");
    case ErrorCode::OutOfMemory:
        platform::Fatal("Object::IsDestroyRequested failed: OutOfMemory");
    case ErrorCode::Overflow:
        platform::Fatal("Object::IsDestroyRequested failed: Overflow");
    case ErrorCode::Busy:
        platform::Fatal("Object::IsDestroyRequested failed: Busy");
    case ErrorCode::NotFound:
        platform::Fatal("Object::IsDestroyRequested failed: NotFound");
    case ErrorCode::AlreadyExists:
        platform::Fatal("Object::IsDestroyRequested failed: AlreadyExists");
    case ErrorCode::StaleHandle:
        platform::Fatal("Object::IsDestroyRequested failed: StaleHandle");
    case ErrorCode::WrongThread:
        platform::Fatal("Object::IsDestroyRequested failed: WrongThread");
    case ErrorCode::WrongContext:
        platform::Fatal("Object::IsDestroyRequested failed: WrongContext");
    case ErrorCode::Destroyed:
        platform::Fatal("Object::IsDestroyRequested failed: Destroyed");
    case ErrorCode::Unsupported:
        platform::Fatal("Object::IsDestroyRequested failed: Unsupported");
    case ErrorCode::CapacityExceeded:
        platform::Fatal("Object::IsDestroyRequested failed: CapacityExceeded");
    case ErrorCode::DependencyFailed:
        platform::Fatal("Object::IsDestroyRequested failed: DependencyFailed");
    case ErrorCode::Cancelled:
        platform::Fatal("Object::IsDestroyRequested failed: Cancelled");
    case ErrorCode::ShuttingDown:
        platform::Fatal("Object::IsDestroyRequested failed: ShuttingDown");
    case ErrorCode::TypeMismatch:
        platform::Fatal("Object::IsDestroyRequested failed: TypeMismatch");
    case ErrorCode::AuthenticationFailed:
        platform::Fatal("Object::IsDestroyRequested failed: AuthenticationFailed");
    case ErrorCode::None:
        platform::Fatal("Object::IsDestroyRequested failed: unexpected success code");
    }
    platform::Fatal("Object::IsDestroyRequested failed: unknown error code");
}

} // namespace

TypeLease Object::GetType() const noexcept {
    auto result = TryGetType();
    if (!result) {
        FailTypeQuery(result.Error());
    }
    return Move(result.Value());
}

bool Object::IsDestroyRequested() const noexcept {
    bool requested = false;
    auto result = TryIsDestroyRequested(requested);
    if (!result) {
        FailDestroyRequestedQuery(result.Error());
    }
    return requested;
}

Result<void> Object::TryIsDestroyRequested(bool& requested) const noexcept {
    auto state = InspectLifecycle();
    if (!state) {
        return Result<void>::Failure(state.Error());
    }
    if (state.Value() == ObjectLifecycle::Alive) {
        requested = false;
        return Result<void>::Success();
    }
    if (state.Value() == ObjectLifecycle::DestroyRequested) {
        requested = true;
        return Result<void>::Success();
    }
    return Result<void>::Failure(ErrorCode::Destroyed);
}

} // namespace rocks
