#pragma once

#include <Core/Memory/UniquePtr.h>
#include <Object/ClassRef.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Layout/Switcher.h>
#include <UiScreens/Screen.h>
#include <UiWidget/WidgetPool.h>

namespace rocks::ui {

// 저장소와 Application이 컨테이너보다 오래 살아야 한다. 화면과 포커스는 플레이어별로 관리한다.
class ROCKS_UISCREENS_API ScreenContainer : public Switcher {
    ROCKS_UI_ELEMENT(ScreenContainer, Switcher)

public:
    ~ScreenContainer() noexcept override;
    Result<ObjectHandle> AddScreen(ClassRef<Screen> type) noexcept;
    Result<void> AddScreen(ObjectHandle screen) noexcept;
    Result<void> RemoveScreen(ObjectHandle screen) noexcept;
    Result<bool> Back() noexcept;
    usize ScreenCount() const noexcept;
    ObjectHandle ScreenAt(usize index) const noexcept;
    Result<void> ClearScreens() noexcept;
    ObjectHandle ActiveScreen() const noexcept;
    uint32 UserId() const noexcept;
    ScreenInputConfig InputConfig() const noexcept;
    bool IsModal() const noexcept;
    SharedPtr<Element> ActiveRoot() const noexcept;
    // 활성 화면의 원하는 초점 대상에 초점을 준다(Docs/Design/UiToolkit 20.8절 — UE
    // DesiredFocusWidget): 남겨 둔 초점(restoreFocus) → 화면 FocusTarget → 화면 안 첫 대상. 활성화
    // 때와 초점이 사라진 채 navigation·확인 입력이 올 때 쓴다. 초점을 주었으면 true다.
    bool FocusDesiredTarget() noexcept;
    Result<bool> DispatchAction(Guid domain, Guid action) noexcept;

    Result<void> AttachPopupLayer(const PopupRequest& popup) noexcept override;
    Result<void> TryAddChild(const SharedPtr<Element>& child) noexcept override;
    bool RemoveChild(Element& child) noexcept override;

protected:
    enum class Order : uint8 {
        Stack,
        Queue,
        Switcher
    };
    Result<void> ActivateAt(usize index) noexcept;
    ScreenContainer(ObjectRegistry& objects, Application& application, uint32 userId,
                    Order order) noexcept;

private:
    using Switcher::SetActiveIndex;

    struct Pool {
        TypeId type;
        UniquePtr<WidgetPool> widgets;
    };
    struct Page {
        ObjectHandle screen;
        SharedPtr<Element> root;
        WeakPtr<Element> focus;
        WidgetPool* pool;
        // 떠날 때의 초점 원인이다(돌아올 때 외곽선 여부를 잇는다).
        FocusCause focusCause = FocusCause::Direct;
    };

    Result<void> Attach(ObjectHandle handle, WidgetPool* pool) noexcept;
    usize Top() const noexcept;
    void Deactivate(Page& page) noexcept;
    void Activate(Page& page) noexcept;
    bool ApplyDesiredFocus(Page& page, const Screen& screen) noexcept;

    ObjectRegistry& objects_;
    Application& app_;
    uint32 userId_;
    Order order_;
    usize selected_ = 0;
    Array<Pool> pools_;
    Array<Page> pages_;
    WeakPtr<Element> previousFocus_;
    FocusCause previousCause_ = FocusCause::Direct;
    // 마지막으로 떠난 화면의 초점 원인이다(새 화면의 첫 초점에 잇는다).
    FocusCause lastCause_ = FocusCause::Direct;
    bool changing_ = false;
    bool backing_ = false;
};

class ROCKS_UISCREENS_API ScreenStack final : public ScreenContainer {
    ROCKS_UI_ELEMENT(ScreenStack, ScreenContainer)

public:
    ScreenStack(ObjectRegistry& objects, Application& app, uint32 userId) noexcept;
};

class ROCKS_UISCREENS_API ScreenQueue final : public ScreenContainer {
    ROCKS_UI_ELEMENT(ScreenQueue, ScreenContainer)

public:
    ScreenQueue(ObjectRegistry& objects, Application& app, uint32 userId) noexcept;
};

} // namespace rocks::ui
