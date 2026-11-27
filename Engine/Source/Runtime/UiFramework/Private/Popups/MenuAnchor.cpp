#include "MenuPopupLayer.h"
#include <Core/ScopeExit.h>
#include <UiFramework/Application/Application.h>

namespace rocks::ui {

MenuAnchor::MenuAnchor(Allocator& allocator) noexcept : Button(allocator), allocator_(allocator) {
}

MenuAnchor::~MenuAnchor() noexcept {
    DisconnectMenu();
}

Result<void> MenuAnchor::Construct(Args&& args) noexcept {
    SetPlacement(args.placement);
    SetFitInWindow(args.fitInWindow);
    auto added = args.content.AttachTo(*this);
    if (added) {
        SetMenuFactory(Move(args.makeMenu));
        SetOnOpening(Move(args.onOpening));
    }

    return added;
}

void MenuAnchor::SetOnOpening(Delegate<void()>&& callback) noexcept {
    openingEvent_ = Move(callback);
}

void MenuAnchor::SetMenuFactory(MenuFactory&& factory) noexcept {
    factory_ = Move(factory);
}

void MenuAnchor::DisconnectMenu() noexcept {
    factory_.Unbind();
    openingEvent_.Unbind();
    openChanged_.Unbind();
    Close();
}

Result<void> MenuAnchor::Open() noexcept {
    if (IsOpen()) {
        return Result<void>::Success();
    }

    if (opening_ || !factory_) {
        return Result<void>::Failure(opening_ ? ErrorCode::Busy : ErrorCode::NotReady);
    }

    auto self = Parent() ? Parent()->FindChild(this) : SharedPtr<Element>{};
    if (!self) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    Close();
    opening_ = true;
    ROCKS_ON_SCOPE_EXIT {
        opening_ = false;
    };
    openingEvent_.ExecuteIfBound();
    if (!factory_) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    auto menu = factory_.Execute();
    if (!menu) {
        return Result<void>::Failure(menu.Error());
    }

    if (!factory_) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    if (!menu.Value() || IsWithin(*menu.Value())) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto layer = SharedPtr<MenuPopupLayer>::TryMake(allocator_, allocator_, *this);
    if (!layer) {
        return Result<void>::Failure(layer.Error());
    }

    if (!catchOutside_) {
        // 층 자신은 누름을 받지 않고 메뉴만 받는다.
        layer.Value()->SetVisibility(Visibility::SelfHitTestInvisible);
    }

    auto added = layer.Value()->TryAddChild(menu.Value());
    if (added) {
        added = AttachPopupLayer({layer.Value(), self, [](Element& owner) noexcept {
                                      static_cast<MenuAnchor&>(owner).Close();
                                  }});
    }

    if (added) {
        layer_ = Move(layer.Value());
        // 열림 상태로 모양이 바뀌는 anchor(메뉴 막대 버튼·하위 메뉴 항목)를 다시 그린다.
        InvalidatePaint();
        openChanged_.ExecuteIfBound(true);
    }

    return added;
}

Result<WeakPtr<Application*>> Application::WeakContext() noexcept {
    if (!context_) {
        auto made = SharedPtr<Application*>::TryMake(allocator_, this);
        if (!made) {
            return Result<WeakPtr<Application*>>::Failure(made.Error());
        }

        context_ = Move(made.Value());
    }

    return Result<WeakPtr<Application*>>::Success(WeakPtr<Application*>(context_));
}

Result<void> MenuAnchor::OpenFocused(Application& app, uint32 user,
                                     Optional<FocusCause> openCause) noexcept {
    auto context = app.WeakContext();
    if (!context) {
        return Result<void>::Failure(context.Error());
    }

    auto previous = app.GetFocus(user);
    const FocusCause cause =
        openCause ? *openCause : (previous ? app.GetFocusCause(user) : FocusCause::Direct);
    if (!previous && Parent()) {
        previous = Parent()->FindChild(this);
    }

    auto opened = Open();
    if (!opened || !layer_) {
        return opened;
    }

    auto menu = layer_->ChildRefAt(0);
    auto moved = app.MoveFocusWithin(user, menu, true, false, cause);
    if (!moved) {
        Close();
        return Result<void>::Failure(moved.Error());
    }

    if (moved.Value() && layer_ && (!previous || !previous->IsWithin(*layer_))) {
        app_ = Move(context.Value());
        restoreFocus_ = WeakPtr<Element>(previous);
        focusUser_ = user;
    }

    return Result<void>::Success();
}

void MenuAnchor::SetPlacement(MenuPlacement placement) noexcept {
    placement_ = placement;
    if (layer_) {
        layer_->InvalidateArrangement();
    }
}

void MenuAnchor::SetCatchOutsidePresses(bool catchOutside) noexcept {
    catchOutside_ = catchOutside;
}

void MenuAnchor::SetFitInWindow(bool fit) noexcept {
    fit_ = fit;
    if (layer_) {
        layer_->InvalidateArrangement();
    }
}

Optional<Vec2> MenuAnchor::MenuPosition() const noexcept {
    if (!IsOpen()) {
        return {};
    }

    return layer_->ChildAt(0)->GetGeometry().render.Apply({});
}

Element* MenuAnchor::MenuContent() const noexcept {
    return IsOpen() ? layer_->ChildAt(0) : nullptr;
}

SharedPtr<Element> MenuAnchor::MenuContentRef() const noexcept {
    return IsOpen() ? layer_->ChildRefAt(0) : SharedPtr<Element>{};
}

void MenuAnchor::SetMenuOffset(Vec2 offset) noexcept {
    menuOffset_ = offset;
    if (layer_) {
        layer_->InvalidateArrangement();
    }
}

void MenuAnchor::SetOnOpenChanged(Delegate<void(bool)>&& callback) noexcept {
    openChanged_ = Move(callback);
}

void MenuAnchor::Close() noexcept {
    auto layer = Move(layer_);
    if (layer) {
        auto app = app_.Lock();
        auto restore = restoreFocus_.Lock();
        app_.Reset();
        restoreFocus_.Reset();
        if (app && *app && restore) {
            auto current = (*app)->GetFocus(focusUser_);
            if (current && current->IsWithin(*layer)) {
                // 메뉴 안에서 마지막으로 쓴 입력(키보드·마우스)을 복원 원인으로 잇는다.
                (void)(*app)->SetFocus(focusUser_, restore, (*app)->GetFocusCause(focusUser_));
            }
        }

        layer->Disconnect();
        if (auto* parent = layer->Parent()) {
            (void)parent->RemoveChild(*layer);
        }

        InvalidatePaint();

        openChanged_.ExecuteIfBound(false);
    }
}

bool MenuAnchor::IsOpen() const noexcept {
    return layer_ && layer_->Parent();
}

void MenuAnchor::OnActivated() noexcept {
    if (IsOpen()) {
        Close();
    } else {
        (void)Open();
    }
}

Result<void> MenuAnchor::Arrange(const Geometry& geometry) noexcept {
    if (layer_ && geometry != GetGeometry()) {
        layer_->InvalidateArrangement();
    }

    return Button::Arrange(geometry);
}

KeyReply MenuAnchor::OnKeyEvent(const KeyEvent& key) noexcept {
    const uint32 user = key.user;
    const input::InputEvent& event = key.input;
    if (IsOpen() && event.type == input::InputEventType::KeyDown &&
        event.key == input::Key::Escape) {
        Close();
        return KeyReply::Handled();
    }

    return Button::OnKeyEvent(key);
}

} // namespace rocks::ui
