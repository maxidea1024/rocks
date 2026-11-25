#include <UiCore/Declarative.h>

namespace rocks::ui {

namespace {

thread_local Allocator* currentAllocator = nullptr;
}

ElementBuildScope::ElementBuildScope(Allocator& allocator) noexcept : previous_(currentAllocator) {
    currentAllocator = &allocator;
}

ElementBuildScope::~ElementBuildScope() noexcept {
    currentAllocator = previous_;
}

Allocator* ElementBuildScope::Current() noexcept {
    return currentAllocator;
}

Result<void> ElementChild::AttachTo(Element& parent) const noexcept {
    if (error != ErrorCode::None) {
        return Result<void>::Failure(error);
    }

    return element ? parent.TryAddChild(element) : Result<void>::Success();
}
} // namespace rocks::ui
