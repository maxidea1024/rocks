#pragma once
#include <UiScreens/ScreenContainer.h>

namespace rocks::ui {

// 게임 위젯 트리에서 화면 스택 또는 대기열을 배치한다. BindPlayer는 TakeElement 전에 호출하며,
// Application과 ObjectRegistry가 화면 컨테이너보다 오래 유지되어야 한다.
ROCKS_CLASS(Abstract)
class ROCKS_UISCREENS_API ScreenContainerWidget : public Widget {
    ROCKS_GENERATED_BODY()
public:
    Result<void> BindPlayer(Application& app, uint32 user) noexcept;
    Result<ObjectHandle> PushScreen(ClassRef<Screen> type) noexcept;
    Result<void> PushScreen(ObjectHandle screen) noexcept;
    Result<void> RemoveScreen(ObjectHandle screen) noexcept;
    Result<void> ClearScreens() noexcept;
    ObjectHandle ActiveScreen() const noexcept;
    usize ScreenCount() const noexcept;
    SharedPtr<ScreenContainer> NativeContainer() const noexcept;
    void ReleaseElements() noexcept override;

protected:
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept override;
    virtual Result<SharedPtr<Element>> CreateContainer(ObjectRegistry& objects, Application& app,
                                                       uint32 user) noexcept = 0;

private:
    ROCKS_NATIVE() Application* app_ = nullptr;
    ROCKS_NATIVE() uint32 user_ = 0;
};
} // namespace rocks::ui
