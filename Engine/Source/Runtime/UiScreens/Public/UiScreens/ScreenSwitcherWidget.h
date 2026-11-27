#pragma once
#include <UiScreens/ScreenContainerWidget.h>

namespace rocks::ui {

// 게임 화면 여러 개를 등록한 뒤 인덱스로 활성 페이지를 전환한다. 비활성 페이지는 입력을 받지
// 않는다.
ROCKS_CLASS()
class ROCKS_UISCREENS_API ScreenSwitcherWidget final : public ScreenContainerWidget {
    ROCKS_GENERATED_BODY()
public:
    Result<void> SetActivePage(usize index) noexcept;

protected:
    Result<SharedPtr<Element>> CreateContainer(ObjectRegistry& objects, Application& app,
                                               uint32 user) noexcept override;
};
} // namespace rocks::ui
