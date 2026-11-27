#pragma once

#include <UiCore/Rendering/UiRenderer.h>
#include <UiWidget/UserWidget.h>
#include <UiWidget/Widget.h>

namespace rocks::ui {

// 화면의 공유 참조가 UserWidget을 유지한다. 입력 콜백은 Borrow를 쥐지 않고 핸들로 접근한다.
struct GameHud {
    SharedPtr<Element> root;
    ObjectHandle progress;
};

int RunGameHudSelfTest(Allocator& allocator, UiRenderer& renderer) noexcept;

Result<GameHud> CreateGameHud(ObjectRegistry& objects) noexcept;

} // namespace rocks::ui
