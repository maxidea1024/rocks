#pragma once

#include <UiWidget/Widget.h>
#include <UiWidget/UserWidget.h>

namespace rocks::ui {

class ScreenContainer;

ROCKS_ENUM()
enum class ScreenInputMode : uint8 {
    Game,
    Menu,
    GameAndMenu
};

ROCKS_STRUCT()
struct ScreenInputConfig {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() ScreenInputMode mode = ScreenInputMode::Menu;
    ROCKS_PROPERTY() bool showCursor = true;
    ROCKS_PROPERTY() bool captureMouse = false;
    ROCKS_PROPERTY() bool ignoreMove = true;
    ROCKS_PROPERTY() bool ignoreLook = true;

    bool operator==(const ScreenInputConfig&) const noexcept = default;
};

ROCKS_CLASS()
class ROCKS_UISCREENS_API Screen : public UserWidget {
    ROCKS_GENERATED_BODY()

public:
    explicit Screen(Allocator& allocator) noexcept;
    // 같은 action은 한 번만 등록한다. 콜백이 false를 반환하면 화면의 HandleAction으로 계속
    // 전달한다.
    Result<void> BindAction(Guid action, Delegate<bool()>&& callback) noexcept;
    bool UnbindAction(Guid action) noexcept;
    void SetActionDomain(Guid domain) noexcept;
    void SetInputConfig(ScreenInputConfig config) noexcept;
    void SetModal(bool modal) noexcept;
    Result<void> SetFocusTarget(ObjectRegistry& objects, ObjectHandle widget) noexcept;
    bool IsActive() const noexcept;
    bool IsModal() const noexcept;
    ScreenInputConfig InputConfig() const noexcept;
    ObjectHandle FocusTarget() const noexcept;
    Result<void> Dismiss() noexcept;

    Guid ActionDomain() const noexcept;
    virtual bool HandleAction(Guid action) noexcept;
    virtual bool OnBack() noexcept;
    virtual void OnActivated() noexcept;
    virtual void OnDeactivated() noexcept;

private:
    friend class ScreenContainer;
    bool DispatchBoundAction(Guid action) noexcept;
    struct Binding {
        Guid action;
        SharedPtr<Delegate<bool()>> callback;
    };
    ROCKS_NATIVE() Array<Binding> bindings_;
    ROCKS_PROPERTY(Name = "inputConfig") ScreenInputConfig config_;
    ROCKS_PROPERTY(Name = "actionDomain") Guid domain_ {};
    ROCKS_PROPERTY(Name = "modal") bool modal_ = false;
    ROCKS_PROPERTY(Name = "restoreFocus") bool restoreFocus_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "focusTarget") ObjectRef<Widget> focusTarget_;
    ROCKS_NATIVE() ScreenContainer* container_ = nullptr;
    ROCKS_NATIVE() bool active_ = false;
};

ROCKS_UISCREENS_API Result<void> RegisterScreenTypes(ModuleRegistration& registration) noexcept;

} // namespace rocks::ui
