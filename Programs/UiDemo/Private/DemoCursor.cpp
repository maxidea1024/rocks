#include "DemoCursor.h"

namespace rocks::ui {

DemoCursor::DemoCursor(platform::Window& window) noexcept : window_(&window) {
}

DemoCursor::~DemoCursor() noexcept {
    Detach();
}

void DemoCursor::Detach() noexcept {
    if (window_) {
        window_->UnlockPointer();
    }

    window_ = nullptr;
}

Result<void> DemoCursor::LockPointer(Vec2 at) noexcept {
    if (!window_) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    return window_->LockPointer(at.x, at.y);
}

void DemoCursor::UnlockPointer(Vec2 releaseAt) noexcept {
    if (!window_) {
        return;
    }

    window_->UnlockPointer();
    (void)window_->SetCursorPosition(releaseAt.x, releaseAt.y);
}

} // namespace rocks::ui
