#include "PointerLock.h"

namespace rocks::platform {

void PointerLock::NoteMove(float x, float y) noexcept {
    lastX_ = x;
    lastY_ = y;
}

void PointerLock::Lock(float anchorX, float anchorY) noexcept {
    anchorX_ = anchorX;
    anchorY_ = anchorY;
    if (!locked_) {
        locked_ = true;
        referenceX_ = lastX_;
        referenceY_ = lastY_;
    }
}

void PointerLock::Unlock() noexcept {
    if (!locked_) {
        return;
    }

    locked_ = false;
    lastX_ = anchorX_;
    lastY_ = anchorY_;
}

void PointerLock::NoteWarp(float x, float y) noexcept {
    lastX_ = x;
    lastY_ = y;
    if (locked_) {
        referenceX_ = x;
        referenceY_ = y;
    }
}

bool PointerLock::IsLocked() const noexcept {
    return locked_;
}

float PointerLock::AnchorX() const noexcept {
    return anchorX_;
}

float PointerLock::AnchorY() const noexcept {
    return anchorY_;
}

PointerLockMove PointerLock::Move(float x, float y) noexcept {
    if (!locked_) {
        NoteMove(x, y);
        return {};
    }

    PointerLockMove move;
    move.x = anchorX_;
    move.y = anchorY_;
    move.deltaX = x - referenceX_;
    move.deltaY = y - referenceY_;
    if (move.deltaX == 0 && move.deltaY == 0) {
        return move;
    }

    move.push = true;
    // 이미 잠금 자리면 되돌릴 필요가 없다(기준만 맞춘다).
    move.warp = x != anchorX_ || y != anchorY_;
    referenceX_ = anchorX_;
    referenceY_ = anchorY_;
    lastX_ = anchorX_;
    lastY_ = anchorY_;
    return move;
}

} // namespace rocks::platform
