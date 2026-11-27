#pragma once
#include <UiScreens/ScreenContainerWidget.h>

namespace rocks::ui {

// 먼저 추가한 화면부터 표시한다. 현재 화면이 닫히면 대기 중인 다음 화면을 활성화한다.
ROCKS_CLASS()
class ROCKS_UISCREENS_API ScreenQueueWidget final : public ScreenContainerWidget {
    ROCKS_GENERATED_BODY()
protected:
    Result<SharedPtr<Element>> CreateContainer(ObjectRegistry& objects, Application& app,
                                               uint32 user) noexcept override;
};
} // namespace rocks::ui
