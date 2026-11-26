#include <UiFramework/Application/Application.h>
#include <UiFramework/Menus/MenuBar.h>

namespace rocks::ui {

namespace {

// 막대 가속키 명령 ID의 영역이다(번호 0은 F10, 1부터 메뉴 순서).
constexpr uint64 MenuAcceleratorDomain = 0x4D454E5542415221ull;
constexpr uint64 FocusMenuBarIndex = 0;

} // namespace

MenuBar::MenuBar(Allocator& allocator, const SharedPtr<MenuContext>& context,
                 const MenuBarStyle& style) noexcept
    : Element(allocator), allocator_(allocator), context_(context), style_(style),
      buttons_(allocator), labels_(allocator) {
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
    }
}

bool MenuBar::CloseIfOutside(const Element* hit) noexcept {
    const usize open = OpenIndex();
    if (open == IndexNone || (hit && hit->IsWithin(*this))) {
        // 막대 위 누름은 막대 버튼이 처리한다(다른 메뉴 열기·열린 메뉴 닫기).
        return false;
    }

    // 펼친 메뉴와 그 하위 메뉴 사슬 안의 누름은 그 메뉴가 처리한다. 막대 메뉴의 내용은 늘
    // MenuBuilder가 만든 MenuPanel이다.
    MenuAnchor* anchor = buttons_[open].Get();
    while (anchor) {
        Element* content = anchor->MenuContent();
        if (!content) {
            break;
        }

        if (hit && hit->IsWithin(*content)) {
            return false;
        }

        anchor = static_cast<MenuPanel*>(content)->OpenSubMenu();
    }

    context_->CloseAll();
    return true;
}

Result<void> MenuBar::AddMenu(StringView label, MenuFill fill) noexcept {
    if (!context_) {
        return Result<void>::Failure(ErrorCode::NotReady);
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
    // 채우기 delegate를 품은 factory는 인라인 저장보다 커서 allocator에 둔다.
    auto factory = MenuAnchor::MenuFactory::TryCreateLambda(
        allocator_,
        [context = context_, owner, fill = Move(fill)]() noexcept -> Result<SharedPtr<Element>> {
            context->SetRoot(owner);
            MenuBuilder menu(context, owner);
            if (fill.IsBound()) {
                auto filled = fill.Execute(menu);
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
    auto added = TryAddChild(button.Value());
    if (added) {
        added = buttons_.TryPushBack(Move(button.Value()));
    }

    if (added) {
        added = labels_.TryPushBack(Move(copy));
    }

    return added;
}

usize MenuBar::MenuCount() const noexcept {
    return buttons_.Size();
}

MenuBarButton* MenuBar::Button(usize index) const noexcept {
    return index < buttons_.Size() ? buttons_[index].Get() : nullptr;
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

Result<void> MenuBar::OpenMenu(usize index, bool focus) noexcept {
    if (index >= buttons_.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const usize open = OpenIndex();
    if (open != IndexNone && open != index) {
        buttons_[open]->Close();
    }

    MenuBarButton& button = *buttons_[index];
    context_->SetRoot(&button);
    if (focus) {
        return button.OpenFocused(context_->GetApplication(), 0, FocusCause::Navigation);
    }

    return button.IsOpen() ? Result<void>::Success() : button.Open();
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
    (void)OpenMenu(index, keyboard);
}

void MenuBar::OnButtonActivated(MenuBarButton& button) noexcept {
    const usize index = IndexOf(button);
    if (index == IndexNone) {
        return;
    }

    if (button.IsOpen()) {
        button.Close();
        return;
    }

    // 키보드(Enter·Space)로 누르면 첫 항목에 초점을 둔다.
    Application& app = context_->GetApplication();
    const bool keyboard =
        app.GetFocus(0).Get() == &button && app.GetFocusCause(0) == FocusCause::Navigation;
    (void)OpenMenu(index, keyboard);
}

bool MenuBar::Navigate(int32 step) noexcept {
    const usize open = OpenIndex();
    const usize count = buttons_.Size();
    if (open == IndexNone || count == 0) {
        return false;
    }

    const usize next = step > 0 ? (open + 1) % count : (open + count - 1) % count;
    return static_cast<bool>(OpenMenu(next, true));
}

Result<void> MenuBar::RegisterAccelerators(CommandSet& definitions,
                                           CommandList& commands) noexcept {
    // F10은 첫 버튼에 키보드 초점을 둔다(Windows 메뉴 막대 규칙).
    const Guid focusId{MenuAcceleratorDomain, FocusMenuBarIndex};
    auto added = definitions.Add(focusId, "Focus Menu Bar", "Moves keyboard focus to the menu bar",
                                 {.primary = {.key = input::Key::F10}, .secondary = {}});
    if (added) {
        added = commands.Map(focusId, {.execute = Delegate<void()>::CreateLambda([this]() noexcept {
                                           if (!buttons_.IsEmpty()) {
                                               (void)context_->GetApplication().SetFocus(
                                                   0, buttons_[0], FocusCause::Navigation);
                                           }
                                       })});
    }

    // Alt+글자는 그 글자로 시작하는 메뉴를 연다(첫 항목에 초점).
    for (usize i = 0; added && i < labels_.Size(); ++i) {
        const StringView label = labels_[i].View();
        if (label.IsEmpty()) {
            continue;
        }

        auto key = FindChordKey(label.SliceBytes(0, 1));
        if (!key) {
            continue;
        }

        const Guid id{MenuAcceleratorDomain, i + 1};
        added = definitions.Add(id, label, "Opens this menu",
                                {.primary = {.key = key.Value(), .alt = true}, .secondary = {}});
        if (added) {
            added = commands.Map(id, {.execute = Delegate<void()>::CreateLambda(
                                          [this, i]() noexcept { (void)OpenMenu(i, true); })});
        }
    }

    return added;
}

Vec2 MenuBar::ComputeDesiredSize(float) const noexcept {
    Vec2 size{};
    for (const auto& button : buttons_) {
        const Vec2 desired = button->GetDesiredSize();
        size.x += desired.x;
        size.y = math::Max(size.y, desired.y);
    }

    return size;
}

Result<void> MenuBar::Arrange(const Geometry& geometry) noexcept {
    SetGeometry(geometry);
    float x = 0;
    for (const auto& button : buttons_) {
        const float width = button->GetDesiredSize().x;
        auto placed = button->ArrangeIfNeeded(
            GetGeometry().MakeChild({width, geometry.size.y}, {1, 0, 0, 1, x, 0}, {}, {}));
        if (!placed) {
            return placed;
        }

        x += width;
    }

    return Result<void>::Success();
}

} // namespace rocks::ui
