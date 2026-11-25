#pragma once
#include <UiScreens/ScreenContainerWidget.h>
namespace rocks::ui {
// 마지막으로 추가한 화면을 먼저 활성화한다. 뒤로가면 이전 화면의 포커스를 복구한다.
ROCKS_CLASS()
class ROCKS_UISCREENS_API ScreenStackWidget final : public ScreenContainerWidget {
    ROCKS_GENERATED_BODY()
protected:
    Result<SharedPtr<Element>> CreateContainer(ObjectRegistry& objects, Application& app,
                                               uint32 user) noexcept override;
};
} // namespace rocks::ui
