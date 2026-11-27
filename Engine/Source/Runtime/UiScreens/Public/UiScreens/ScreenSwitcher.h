#pragma once
#include <UiScreens/ScreenContainer.h>

namespace rocks::ui {

// 추가 순서를 유지하고 지정한 화면 하나만 활성화한다. 화면 전환은 사용자별 포커스 복귀 규칙을
// 따른다.
class ROCKS_UISCREENS_API ScreenSwitcher final : public ScreenContainer {
    ROCKS_UI_ELEMENT(ScreenSwitcher, ScreenContainer)

public:
    ScreenSwitcher(ObjectRegistry& objects, Application& app, uint32 user) noexcept;
    Result<void> SetActivePage(usize index) noexcept;
};
} // namespace rocks::ui
