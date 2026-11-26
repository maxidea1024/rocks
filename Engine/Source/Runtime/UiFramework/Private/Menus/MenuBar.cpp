#include <UiFramework/Application/Application.h>
#include <UiFramework/Menus/MenuBar.h>

namespace rocks::ui {

namespace {

// 막대 가속키 명령 ID의 영역이다(번호 0은 F10, 1부터 메뉴 순서). 정의는 막대가 소유하므로 다른
// 막대와 겹쳐도 충돌하지 않는다.
constexpr uint64 MenuAcceleratorDomain = 0x4D454E5542415221ull;
constexpr uint64 FocusMenuBarIndex = 0;
// 넘침 버튼 글자다(UE 툴바 넘침 표시처럼 오른쪽 겹화살괄호).
constexpr StringView OverflowLabel = "\xC2\xBB";

} // namespace

MenuBar::MenuBar(Allocator& allocator, const SharedPtr<MenuContext>& context,
                 const MenuBarStyle& style) noexcept
    : Element(allocator), allocator_(allocator), context_(context), style_(style),
      buttons_(allocator), labels_(allocator), fills_(allocator), acceleratorSet_(allocator),
      acceleratorBindings_(allocator) {
    if (context_) {
        context_->SetOnNavigateRoot(Delegate<bool(int32)>::CreateLambda(
            [this](int32 step) noexcept { return Navigate(step); }));
        // 막대가 Application보다 오래 살 수 있어 약한 문맥으로 잡고 소멸 때 뗀다. 등록에 실패하면
        // 바깥 누름 닫기만 빠진다(Esc·막대 버튼 다시 누름·항목 실행으로는 닫힌다).
        Application& app = context_->GetApplication();
        auto weak = app.WeakContext();
        auto observer = app.AddPressObserver(Application::PressObserver::CreateLambda(
            [this](const PointerEvent&, Element* hit) noexcept { return CloseIfOutside(hit); }));
        if (weak && observer) {
            app_ = Move(weak.Value());
            pressObserver_ = observer.Value();
        } else if (observer) {
            app.RemovePressObserver(observer.Value());
        }
    }
}

MenuBar::~MenuBar() noexcept {
    if (auto app = app_.Lock(); app && *app) {
        (*app)->RemovePressObserver(pressObserver_);
        if (accelerators_) {
            (*app)->RemoveWindowCommands(*accelerators_);
        }
    }

    // 문맥이 막대보다 오래 살 수 있으므로 막대를 가리키는 루트·탐색을 지운다.
    if (context_) {
        context_->SetOnNavigateRoot({});
        for (const auto& button : buttons_) {
            if (context_->Root() == button.Get()) {
                context_->SetRoot(nullptr);
            }
        }
    }
}

bool MenuBar::CloseIfOutside(const Element* hit) noexcept {
    if (OpenIndex() == IndexNone ||
        (hit && (hit->IsWithin(*this) || context_->IsInOpenMenus(*hit)))) {
        // 막대 위 누름은 막대 버튼이, 메뉴 안 누름은 그 메뉴가 처리한다.
        return false;
    }

    context_->CloseAll();
    return true;
}

Result<void> MenuBar::EnsureOverflow() noexcept {
    if (!buttons_.IsEmpty()) {
        return Result<void>::Success();
    }

    auto text = context_->MakeLabel(OverflowLabel, style_.label.font);
    auto button = SharedPtr<MenuBarButton>::TryMake(allocator_, allocator_, *this, style_);
    if (!text || !button) {
        return Result<void>::Failure(!text ? text.Error() : button.Error());
    }

    String copy(allocator_);
    auto stored = copy.TryAssign(OverflowLabel);
    if (stored) {
        stored = button.Value()->TryAddChild(text.Value());
    }

    if (!stored) {
        return stored;
    }

    MenuBarButton* owner = button.Value().Get();
    button.Value()->SetMenuFactory(MenuAnchor::MenuFactory::CreateLambda(
        [this, owner]() noexcept -> Result<SharedPtr<Element>> {
            context_->SetRoot(owner);
            MenuBuilder menu(context_, owner);
            auto filled = FillOverflow(menu);
            if (!filled) {
                return Result<SharedPtr<Element>>::Failure(filled.Error());
            }

            auto panel = menu.MakePanel();
            if (!panel) {
                return Result<SharedPtr<Element>>::Failure(panel.Error());
            }

            return Result<SharedPtr<Element>>::Success(panel.Value());
        }));
    button.Value()->SetOnOpenChanged(Delegate<void(bool)>::CreateLambda(
        [this](bool open) noexcept { OnMenuOpenChanged(open); }));
    button.Value()->SetVisibility(Visibility::Collapsed);
    auto added = TryAddChild(button.Value());
    if (added) {
        added = buttons_.TryPushBack(Move(button.Value()));
    }

    if (added) {
        added = labels_.TryPushBack(Move(copy));
    }

    return added;
}

Result<void> MenuBar::FillOverflow(MenuBuilder& menu) const noexcept {
    for (usize i = 0; i < MenuCount(); ++i) {
        if (!IsMenuHidden(i)) {
            continue;
        }

        menu.AddSubMenu(labels_[i].View(),
                        MenuFill::CreateLambda([fill = fills_[i]](MenuBuilder& sub) noexcept {
                            return fill && fill->IsBound() ? fill->Execute(sub)
                                                           : Result<void>::Success();
                        }));
    }

    return Result<void>::Success();
}

MenuBarButton* MenuBar::OverflowButton() const noexcept {
    return buttons_.IsEmpty() ? nullptr : buttons_[buttons_.Size() - 1].Get();
}

bool MenuBar::IsMenuHidden(usize index) const noexcept {
    return index < MenuCount() && buttons_[index]->GetVisibility() == Visibility::Collapsed;
}

Result<void> MenuBar::AddMenu(StringView label, MenuFill fill) noexcept {
    if (!context_) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    auto overflow = EnsureOverflow();
    if (!overflow) {
        return overflow;
    }

    auto sharedFill = SharedPtr<MenuFill>::TryMake(allocator_, Move(fill));
    if (!sharedFill) {
        return Result<void>::Failure(sharedFill.Error());
    }

    auto text = context_->MakeLabel(label, style_.label.font);
    auto button = SharedPtr<MenuBarButton>::TryMake(allocator_, allocator_, *this, style_);
    if (!text || !button) {
        return Result<void>::Failure(!text ? text.Error() : button.Error());
    }

    String copy(allocator_);
    auto stored = copy.TryAssign(label);
    if (stored) {
        stored = button.Value()->TryAddChild(text.Value());
    }

    if (!stored) {
        return stored;
    }

    MenuBarButton* owner = button.Value().Get();
    // 채우기 delegate는 넘침 메뉴와 나눠 쓴다(공유). factory는 인라인 저장보다 커서 allocator에
    // 둔다.
    auto factory = MenuAnchor::MenuFactory::TryCreateLambda(
        allocator_,
        [context = context_, owner,
         fill = sharedFill.Value()]() noexcept -> Result<SharedPtr<Element>> {
            context->SetRoot(owner);
            MenuBuilder menu(context, owner);
            if (fill->IsBound()) {
                auto filled = fill->Execute(menu);
                if (!filled) {
                    return Result<SharedPtr<Element>>::Failure(filled.Error());
                }
            }

            auto panel = menu.MakePanel();
            if (!panel) {
                return Result<SharedPtr<Element>>::Failure(panel.Error());
            }

            return Result<SharedPtr<Element>>::Success(panel.Value());
        });
    if (!factory) {
        return Result<void>::Failure(factory.Error());
    }

    button.Value()->SetMenuFactory(Move(factory.Value()));
    button.Value()->SetOnOpenChanged(Delegate<void(bool)>::CreateLambda(
        [this](bool open) noexcept { OnMenuOpenChanged(open); }));
    // 넘침 버튼 앞에 넣는다(넘침 버튼은 늘 마지막이다).
    const usize at = MenuCount();
    auto added = TryAddChild(button.Value());
    if (added) {
        added = buttons_.TryInsert(Move(button.Value()), at);
    }

    if (added) {
        added = labels_.TryInsert(Move(copy), at);
    }

    if (added) {
        added = fills_.TryPushBack(Move(sharedFill.Value()));
    }

    return added;
}

usize MenuBar::MenuCount() const noexcept {
    return buttons_.IsEmpty() ? 0 : buttons_.Size() - 1;
}

MenuBarButton* MenuBar::Button(usize index) const noexcept {
    return index < MenuCount() ? buttons_[index].Get() : nullptr;
}

usize MenuBar::IndexOf(const MenuBarButton& button) const noexcept {
    for (usize i = 0; i < buttons_.Size(); ++i) {
        if (buttons_[i].Get() == &button) {
            return i;
        }
    }

    return IndexNone;
}

usize MenuBar::OpenIndex() const noexcept {
    for (usize i = 0; i < buttons_.Size(); ++i) {
        if (buttons_[i]->IsOpen()) {
            return i;
        }
    }

    return IndexNone;
}

Result<void> MenuBar::OpenMenu(usize index, bool keyboard) noexcept {
    if (index >= buttons_.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 넘쳐 숨은 메뉴는 넘침 메뉴로 연다(그 안의 하위 메뉴로 보인다).
    if (IsMenuHidden(index)) {
        index = buttons_.Size() - 1;
    }

    // 지금 초점(막대·열린 메뉴 밖이면)을 기억하고, 다른 메뉴를 닫는 동안 세션을 끝내지 않는다.
    Application& app = context_->GetApplication();
    context_->BeginSession(0, *this);
    MenuBarButton& button = *buttons_[index];
    Result<void> opened = Result<void>::Success();
    {
        switching_ = true;
        const usize open = OpenIndex();
        if (open != IndexNone && open != index) {
            buttons_[open]->Close();
        }

        context_->SetRoot(&button);
        if (!button.IsOpen()) {
            opened = button.Open();
        }

        switching_ = false;
    }

    if (!opened) {
        if (OpenIndex() == IndexNone) {
            context_->EndSession(*this);
        }

        return opened;
    }

    auto* panel = static_cast<MenuPanel*>(button.MenuContent());
    if (!panel) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    if (keyboard && panel->FocusFirstItem(0)) {
        return Result<void>::Success();
    }

    // 마우스로 열면 패널에 초점을 둔다(외곽선 없음 — 첫 방향키가 첫 항목으로 간다).
    return app.SetFocus(0, button.MenuContentRef(), FocusCause::Pointer);
}

void MenuBar::OnMenuOpenChanged(bool open) noexcept {
    if (!open && !switching_ && OpenIndex() == IndexNone && context_) {
        context_->EndSession(*this);
    }
}

void MenuBar::OnButtonHovered(MenuBarButton& button) noexcept {
    // 다른 막대 메뉴가 열린 동안에만 hover로 바꾼다. 키보드로 열었던 메뉴면 새 메뉴에도 초점을
    // 둔다.
    const usize open = OpenIndex();
    const usize index = IndexOf(button);
    if (open == IndexNone || open == index || index == IndexNone) {
        return;
    }

    const bool keyboard = context_->GetApplication().GetFocusCause(0) == FocusCause::Navigation;
    // 열기 실패(채우기 오류)면 메뉴가 닫힌 채 세션이 끝난다 — 메뉴 동작은 실패를 보이지 않는다.
    (void)OpenMenu(index, keyboard);
}

void MenuBar::OnButtonActivated(MenuBarButton& button) noexcept {
    const usize index = IndexOf(button);
    if (index == IndexNone) {
        return;
    }

    // 열린 메뉴의 버튼을 다시 누르면 닫는다(세션이 끝나 초점이 돌아간다).
    if (button.IsOpen()) {
        button.Close();
        return;
    }

    // 키보드(Enter·Space)로 누르면 첫 항목에 초점을 둔다.
    Application& app = context_->GetApplication();
    const bool keyboard =
        app.GetFocus(0).Get() == &button && app.GetFocusCause(0) == FocusCause::Navigation;
    // 열기 실패면 메뉴 없이 막대에 머문다.
    (void)OpenMenu(index, keyboard);
}

bool MenuBar::ReleaseOverMenu(Vec2 windowPoint) noexcept {
    if (OpenIndex() == IndexNone) {
        return false;
    }

    Element* top = this;
    while (top->Parent()) {
        top = top->Parent();
    }

    // 놓은 지점의 항목을 찾는다: 열린 메뉴 사슬의 패널 직속 자식 중 초점을 받는 것(항목)이다.
    Element* hit = top->HitTest(windowPoint);
    if (!hit || !context_->IsInOpenMenus(*hit)) {
        return false;
    }

    for (Element* current = hit; current && current->Parent(); current = current->Parent()) {
        Element* parent = current->Parent();
        if (!parent->SupportsKeyboardFocus() || !current->SupportsKeyboardFocus()) {
            continue;
        }

        // 메뉴 패널(초점 받는 요소)의 직속 항목(MenuRow — 패널 안에서 초점 받는 것은 항목뿐)이다.
        if (context_->IsInOpenMenus(*parent) && current->IsEnabled()) {
            static_cast<MenuRow*>(current)->Invoke();
            return true;
        }
    }

    return false;
}

namespace {

// 보이는(접히지 않은) 다음 버튼 번호다. 모두 접혔으면 from이다.
usize NextVisible(const Array<SharedPtr<MenuBarButton>>& buttons, usize from, int32 step) noexcept {
    const usize count = buttons.Size();
    usize index = from;
    for (usize tried = 0; tried < count; ++tried) {
        index = step > 0 ? (index + 1) % count : (index + count - 1) % count;
        if (buttons[index]->GetVisibility() != Visibility::Collapsed) {
            return index;
        }
    }

    return from;
}

} // namespace

bool MenuBar::Navigate(int32 step) noexcept {
    const usize open = OpenIndex();
    if (open == IndexNone || buttons_.IsEmpty()) {
        return false;
    }

    return static_cast<bool>(OpenMenu(NextVisible(buttons_, open, step), true));
}

void MenuBar::FocusNeighbor(const MenuBarButton& from, int32 step) noexcept {
    const usize index = IndexOf(from);
    if (index == IndexNone) {
        return;
    }

    // 초점 콜백 중(Busy)이면 그대로 둔다.
    (void)context_->GetApplication().SetFocus(0, buttons_[NextVisible(buttons_, index, step)],
                                              FocusCause::Navigation);
}

bool MenuBar::HasKeyboardFocus(const MenuBarButton& button) const noexcept {
    if (!context_) {
        return false;
    }

    Application& app = context_->GetApplication();
    return app.GetFocusCause(0) == FocusCause::Navigation && app.GetFocus(0).Get() == &button;
}

void MenuBar::LeaveMenuMode() noexcept {
    if (OpenIndex() != IndexNone) {
        context_->CloseAll();
        return;
    }

    context_->EndSession(*this);
}

void MenuBar::ToggleMenuMode() noexcept {
    if (buttons_.IsEmpty()) {
        return;
    }

    Application& app = context_->GetApplication();
    const auto focus = app.GetFocus(0);
    if (OpenIndex() == IndexNone && focus && focus->IsWithin(*this)) {
        LeaveMenuMode();
        return;
    }

    if (OpenIndex() != IndexNone) {
        // 메뉴가 열려 있으면 F10은 메뉴를 닫고 나간다(Windows).
        LeaveMenuMode();
        return;
    }

    context_->BeginSession(0, *this);
    // 첫 메뉴가 넘쳐 숨었으면 넘침 버튼이 첫 칸이다. 초점 콜백 중(Busy)이면 그대로 둔다.
    const usize first = IsMenuHidden(0) ? buttons_.Size() - 1 : 0;
    (void)app.SetFocus(0, buttons_[first], FocusCause::Navigation);
}

Result<void> MenuBar::RegisterAccelerators() noexcept {
    if (accelerators_) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    auto app = app_.Lock();
    if (!app || !*app) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    auto list = SharedPtr<CommandList>::TryMake(allocator_, allocator_, acceleratorSet_,
                                                acceleratorBindings_);
    if (!list) {
        return Result<void>::Failure(list.Error());
    }

    // F10은 막대 초점을 켜고 끈다(Windows 메뉴 막대 규칙).
    const Guid focusId{MenuAcceleratorDomain, FocusMenuBarIndex};
    auto added =
        acceleratorSet_.Add(focusId, "Focus Menu Bar", "Moves keyboard focus to the menu bar",
                            {.primary = {.key = input::Key::F10}, .secondary = {}});
    if (added) {
        added = list.Value()->Map(focusId, {.execute = Delegate<void()>::CreateLambda(
                                                [this]() noexcept { ToggleMenuMode(); })});
    }

    // Alt+글자는 그 글자로 시작하는 메뉴를 연다(첫 항목에 초점). 넘침 버튼은 가속키가 없다.
    for (usize i = 0; added && i < MenuCount(); ++i) {
        const StringView label = labels_[i].View();
        if (label.IsEmpty()) {
            continue;
        }

        auto key = FindChordKey(label.SliceBytes(0, 1));
        if (!key) {
            continue;
        }

        const Guid id{MenuAcceleratorDomain, i + 1};
        added =
            acceleratorSet_.Add(id, label, "Opens this menu",
                                {.primary = {.key = key.Value(), .alt = true}, .secondary = {}});
        if (added) {
            added = list.Value()->Map(id, {.execute = Delegate<void()>::CreateLambda(
                                               [this, i]() noexcept { (void)OpenMenu(i, true); })});
        }
    }

    if (added) {
        added = (*app)->AddWindowCommands(list.Value());
    }

    if (added) {
        accelerators_ = Move(list.Value());
    }

    return added;
}

Vec2 MenuBar::ComputeDesiredSize(float) const noexcept {
    // 원하는 크기는 모든 메뉴가 보일 때다(넘침 버튼은 넣지 않는다). 접힌 버튼도 원하는 크기는 잰다.
    Vec2 size{};
    for (usize i = 0; i < MenuCount(); ++i) {
        const Vec2 desired = buttons_[i]->GetDesiredSize();
        size.x += desired.x;
        size.y = math::Max(size.y, desired.y);
    }

    return size;
}

Result<void> MenuBar::Arrange(const Geometry& geometry) noexcept {
    SetGeometry(geometry);
    const usize count = MenuCount();
    if (count == 0) {
        return Result<void>::Success();
    }

    // 다 들어가면 넘침 버튼을 접고, 아니면 넘침 버튼 자리를 남기고 들어가는 앞 메뉴만 보인다.
    MenuBarButton& overflow = *buttons_[count];
    const float overflowWidth = overflow.ComputeDesiredSize(1).x;
    float total = 0;
    for (usize i = 0; i < count; ++i) {
        total += buttons_[i]->ComputeDesiredSize(1).x;
    }

    const bool overflowing = total > geometry.size.x;
    const float limit = overflowing ? geometry.size.x - overflowWidth : geometry.size.x;
    float x = 0;
    bool fits = true;
    for (usize i = 0; i < count; ++i) {
        MenuBarButton& button = *buttons_[i];
        const float width = button.ComputeDesiredSize(1).x;
        fits = fits && x + width <= limit;
        // 숨는 메뉴가 열려 있었으면 닫는다(넘침 메뉴에서 다시 연다).
        if (!fits && button.IsOpen()) {
            button.Close();
        }

        button.SetVisibility(fits ? Visibility::Visible : Visibility::Collapsed);
        if (!fits) {
            continue;
        }

        auto placed = button.ArrangeIfNeeded(
            GetGeometry().MakeChild({width, geometry.size.y}, {1, 0, 0, 1, x, 0}, {}, {}));
        if (!placed) {
            return placed;
        }

        x += width;
    }

    if (!overflowing && overflow.IsOpen()) {
        overflow.Close();
    }

    overflow.SetVisibility(overflowing ? Visibility::Visible : Visibility::Collapsed);
    if (!overflowing) {
        return Result<void>::Success();
    }

    return overflow.ArrangeIfNeeded(
        GetGeometry().MakeChild({overflowWidth, geometry.size.y}, {1, 0, 0, 1, x, 0}, {}, {}));
}

} // namespace rocks::ui
