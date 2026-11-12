#include <Object/ObjectAccessContext.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>

namespace rocks {
namespace {
thread_local ObjectAccessScope* activeScope = nullptr;
}

ObjectAccessScope::ObjectAccessScope(ObjectAccessContext access) noexcept
    : access_(access), previous_(activeScope), ownerThread_(platform::CurrentThreadId()) {
    activeScope = this;
}

ObjectAccessScope::~ObjectAccessScope() noexcept {
    if (ownerThread_ != platform::CurrentThreadId() || activeScope != this) {
        platform::Fatal("ObjectAccessScope must close on its owner thread in LIFO order");
    }
    activeScope = previous_;
}

Result<ObjectAccessContext> ObjectAccessScope::Current() noexcept {
    if (!activeScope) {
        return Result<ObjectAccessContext>::Failure(ErrorCode::WrongContext);
    }
    return Result<ObjectAccessContext>::Success(activeScope->access_);
}

} // namespace rocks
