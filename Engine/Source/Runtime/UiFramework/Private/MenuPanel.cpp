#include <UiFramework/Application.h>
#include <UiFramework/MenuPanel.h>

namespace rocks::ui {

MenuPanel::MenuPanel(Allocator& allocator, const SharedPtr<MenuContext>& context,
                     MenuAnchor* owner) noexcept
    : Element(allocator), context_(context), owner_(owner) {
}

Result<void> MenuPanel::AddItem(const SharedPtr<Element>& item) noexcept {
    if (!item) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto added = TryAddChild(item);
    if (added && item->SupportsKeyboardFocus()) {
        // 항목만 초점을 받는다(구분선·묶음 제목은 아니다). 항목은 MenuRow뿐이다.
        static_cast<MenuRow&>(*item).SetPanel(this);
    }

    return added;
}

bool MenuPanel::IsRoot() const noexcept {
    return owner_ != nullptr && owner_ == context_->Root();
}

MenuAnchor* MenuPanel::Owner() const noexcept {
    return owner_;
}

MenuRow* MenuPanel::OpenSubMenu() const noexcept {
    return openSub_ && openSub_->IsOpen() ? openSub_ : nullptr;
}

void MenuPanel::SwitchSubMenu(MenuRow* next) noexcept {
    clobber_.Reset();
    if (openSub_ && openSub_ != next) {
        openSub_->Close();
    }

    openSub_ = next;
    if (next && !next->IsOpen()) {
        (void)next->Open();
    }
}

void MenuPanel::OnRowHovered(MenuRow& row) noexcept {
    MenuRow* open = OpenSubMenu();
    if (open == &row) {
        // 열린 하위 메뉴 항목으로 돌아오면 바꾸기를 취소한다.
        clobber_.Reset();
        return;
    }

    if (!open) {
        // 열린 하위 메뉴가 없으면 바로 연다(UE SubMenuOpenTime 0).
        if (row.HasSubMenu()) {
            SwitchSubMenu(&row);
        }

        return;
    }

    // 다른 하위 메뉴가 열려 있으면 잠시 뒤 바꾼다(포인터가 하위 메뉴로 가는 중일 수 있다).
    MenuRow* target = row.HasSubMenu() ? &row : nullptr;
    auto self = Parent() ? Parent()->FindChild(this) : SharedPtr<Element>{};
    if (!self) {
        return;
    }

    auto timer = context_->GetApplication().StartTimer(
        self, SubMenuClobberDelay,
        Delegate<void()>::CreateLambda([this, target]() noexcept { SwitchSubMenu(target); }));
    clobber_ = timer ? Move(timer.Value()) : SharedPtr<UiTimer>{};
}

bool MenuPanel::MoveFocus(uint32 user, int32 step, bool toEnd) noexcept {
    // 사용 가능한 항목 사이를 돈다(끝에서 처음으로).
    const usize count = ChildCount();
    if (count == 0) {
        return false;
    }

    Application& app = context_->GetApplication();
    const auto focus = app.GetFocus(user);
    usize current = count;
    for (usize i = 0; i < count; ++i) {
        if (ChildAt(i) == focus.Get()) {
            current = i;
            break;
        }
    }

    for (usize tried = 0; tried < count; ++tried) {
        usize index = 0;
        if (toEnd) {
            index = step > 0 ? count - 1 - tried : tried;
        } else if (current == count) {
            index = step > 0 ? tried : count - 1 - tried;
        } else {
            const usize offset = (tried + 1) % count;
            index = step > 0 ? (current + offset) % count : (current + count - offset) % count;
        }

        Element* candidate = ChildAt(index);
        if (candidate->SupportsKeyboardFocus() && candidate->IsEnabled()) {
            return static_cast<bool>(app.SetFocus(user, ChildRefAt(index), FocusCause::Navigation));
        }
    }

    return false;
}

bool MenuPanel::OnKeyEvent(uint32 user, const input::InputEvent& event) noexcept {
    if (event.type != input::InputEventType::KeyDown) {
        return false;
    }

    switch (event.key) {
    case input::Key::Up:
        return MoveFocus(user, -1, false);
    case input::Key::Down:
        return MoveFocus(user, 1, false);
    case input::Key::Home:
        return MoveFocus(user, -1, true);
    case input::Key::End:
        return MoveFocus(user, 1, true);
    case input::Key::Left:
        // 하위 메뉴면 닫고 그 항목으로, 루트면 막대의 이전 메뉴로 간다.
        if (!IsRoot() && owner_) {
            owner_->Close();
            return true;
        }

        return context_->NavigateRoot(-1);
    case input::Key::Right:
        // 하위 메뉴 항목은 자기가 처리한다. 여기 오면 루트 메뉴에서 막대의 다음 메뉴로.
        return IsRoot() && context_->NavigateRoot(1);
    default:
        return false;
    }
}

Vec2 MenuPanel::ComputeDesiredSize(float) const noexcept {
    Vec2 size{};
    for (usize i = 0; i < ChildCount(); ++i) {
        const Element* item = ChildAt(i);
        if (item->GetVisibility() == Visibility::Collapsed) {
            continue;
        }

        const Vec2 desired = item->GetDesiredSize();
        size.x = math::Max(size.x, desired.x);
        size.y += desired.y;
    }

    // 외곽선 두께와 패널 안쪽 여백만큼 둘레를 둔다.
    const float outline = context_->Style().outline.outlineWidth;
    const Vec4 pad = context_->Style().panelPadding;
    return {size.x + 2 * outline + pad.x + pad.z, size.y + 2 * outline + pad.y + pad.w};
}

Result<void> MenuPanel::Arrange(const Geometry& geometry) noexcept {
    SetGeometry(geometry);
    const float outline = context_->Style().outline.outlineWidth;
    const Vec4 pad = context_->Style().panelPadding;
    const float left = outline + pad.x;
    const float width = math::Max(0.0f, geometry.size.x - 2 * outline - pad.x - pad.z);
    float y = outline + pad.y;
    for (usize i = 0; i < ChildCount(); ++i) {
        Element* item = ChildAt(i);
        if (item->GetVisibility() == Visibility::Collapsed) {
            continue;
        }

        const float height = item->GetDesiredSize().y;
        auto placed = item->ArrangeIfNeeded(
            GetGeometry().MakeChild({width, height}, {1, 0, 0, 1, left, y}, {}, {}));
        if (!placed) {
            return placed;
        }

        y += height;
    }

    return Result<void>::Success();
}

Result<void> MenuPanel::OnPaint(PaintList& list, int32 layer) const noexcept {
    const StyleBrush& background = context_->Style().background;
    if (background.mode == BrushMode::None) {
        return Result<void>::Success();
    }

    return list.TryAddBrush(GetGeometry(), background.Resolve(list.Theme(), list.ColorContext()),
                            LinearColor{1, 1, 1, 1}, layer);
}

Result<void> MenuPanel::OnPaintOverlay(PaintList& list, int32 layer) const noexcept {
    // 외곽선은 항목 올림 바탕 위에 그린다(UE "Menu.Outline").
    const StyleBrush& outline = context_->Style().outline;
    if (outline.mode == BrushMode::None) {
        return Result<void>::Success();
    }

    return list.TryAddBrush(GetGeometry(), outline.Resolve(list.Theme(), list.ColorContext()),
                            LinearColor{1, 1, 1, 1}, layer);
}

} // namespace rocks::ui
