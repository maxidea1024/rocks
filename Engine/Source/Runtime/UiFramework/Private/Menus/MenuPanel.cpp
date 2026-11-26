#include <UiFramework/Application/Application.h>
#include <UiFramework/Menus/MenuPanel.h>

namespace rocks::ui {

MenuPanel::MenuPanel(Allocator& allocator, const SharedPtr<MenuContext>& context,
                     MenuAnchor* owner) noexcept
    : Element(allocator), context_(context), owner_(owner) {
    // 긴 메뉴는 패널 안에서 스크롤하므로 경계 밖 항목은 그리지도 누르지도 않는다.
    SetClipToBounds(true);
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
        // 채우기 실패면 하위 메뉴 없이 항목에 머문다(항목은 그대로 강조된다).
        (void)next->Open();
    }
}

float MenuPanel::ScrollOffset() const noexcept {
    return scroll_;
}

float MenuPanel::MaxScrollOffset() const noexcept {
    return maxScroll_;
}

float MenuPanel::ContentTop() const noexcept {
    return context_->Style().outline.outlineWidth + context_->Style().panelPadding.y;
}

float MenuPanel::ViewHeight() const noexcept {
    const float outline = context_->Style().outline.outlineWidth;
    const Vec4 pad = context_->Style().panelPadding;
    return math::Max(0.0f, GetGeometry().size.y - 2 * outline - pad.y - pad.w);
}

void MenuPanel::ScrollIntoView(usize index) noexcept {
    if (maxScroll_ <= 0 || index >= ChildCount()) {
        return;
    }

    float top = 0;
    for (usize i = 0; i < index; ++i) {
        if (ChildAt(i)->GetVisibility() != Visibility::Collapsed) {
            top += ChildAt(i)->GetDesiredSize().y;
        }
    }

    const float bottom = top + ChildAt(index)->GetDesiredSize().y;
    const float view = ViewHeight();
    float next = scroll_;
    if (top < next) {
        next = top;
    } else if (bottom > next + view) {
        next = bottom - view;
    }

    next = math::Clamp(next, 0.0f, maxScroll_);
    if (next != scroll_) {
        scroll_ = next;
        InvalidateArrangement();
    }
}

PointerReply MenuPanel::OnPointerEvent(const PointerEvent& event) noexcept {
    if (event.type == PointerEventType::Wheel && maxScroll_ > 0 && math::IsFinite(event.wheel)) {
        const float next =
            math::Clamp(scroll_ - event.wheel * MenuWheelScrollStep, 0.0f, maxScroll_);
        if (next != scroll_) {
            scroll_ = next;
            InvalidateArrangement();
        }

        return {.handled = true};
    }

    return Element::OnPointerEvent(event);
}

MenuRow* MenuPanel::HoveredRow() const noexcept {
    return hoveredRow_;
}

bool MenuPanel::SupportsKeyboardFocus() const noexcept {
    return true;
}

bool MenuPanel::FocusFirstItem(uint32 user) noexcept {
    Application& app = context_->GetApplication();
    for (usize i = 0; i < ChildCount(); ++i) {
        Element* item = ChildAt(i);
        if (item->SupportsKeyboardFocus() && item->IsEnabled()) {
            ScrollIntoView(i);
            return static_cast<bool>(app.SetFocus(user, ChildRefAt(i), FocusCause::Navigation));
        }
    }

    return false;
}

void MenuPanel::OnRowUnhovered(MenuRow& row) noexcept {
    if (hoveredRow_ != &row) {
        return;
    }

    hoveredRow_ = nullptr;
    clobber_.Reset();
    if (openSub_) {
        openSub_->InvalidatePaint();
    }
}

void MenuPanel::OnRowHovered(MenuRow& row) noexcept {
    hoveredRow_ = &row;
    MenuRow* open = OpenSubMenu();
    if (open) {
        // 열린 항목의 강조는 다른 항목을 hover하는 동안 꺼진다.
        open->InvalidatePaint();
    }

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
            // 키보드로 다른 항목으로 가면 열린 하위 메뉴를 닫는다(Windows 메뉴).
            if (openSub_ && openSub_ != candidate) {
                SwitchSubMenu(nullptr);
            }

            ScrollIntoView(index);
            return static_cast<bool>(app.SetFocus(user, ChildRefAt(index), FocusCause::Navigation));
        }
    }

    return false;
}

bool MenuPanel::JumpToLetter(uint32 user, char letter) noexcept {
    const usize count = ChildCount();
    const auto focus = context_->GetApplication().GetFocus(user);
    usize current = count;
    for (usize i = 0; i < count; ++i) {
        if (ChildAt(i) == focus.Get()) {
            current = i;
            break;
        }
    }

    // 항목 첫 글자(ASCII, 대소문자 무시)가 맞는 사용 가능 항목이다.
    const auto matches = [letter](const Element& item) noexcept {
        if (!item.SupportsKeyboardFocus() || !item.IsEnabled()) {
            return false;
        }

        const StringView label = static_cast<const MenuRow&>(item).LabelText();
        if (label.IsEmpty()) {
            return false;
        }

        char first = label.Data()[0];
        if (first >= 'a' && first <= 'z') {
            first = static_cast<char>(first - 'a' + 'A');
        }

        return first == letter;
    };
    usize found = count;
    usize total = 0;
    for (usize step = 1; step <= count; ++step) {
        const usize index = current == count ? step - 1 : (current + step) % count;
        if (!matches(*ChildAt(index))) {
            continue;
        }

        if (found == count) {
            found = index;
        }

        ++total;
    }

    if (found == count) {
        return false;
    }

    ScrollIntoView(found);
    auto focused =
        context_->GetApplication().SetFocus(user, ChildRefAt(found), FocusCause::Navigation);
    if (focused && total == 1) {
        static_cast<MenuRow*>(ChildAt(found))->Invoke();
    }

    return true;
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
    case input::Key::Tab:
        // 메뉴 안 Tab은 아래·위와 같다(초점이 메뉴 밖으로 빠지지 않는다).
        return MoveFocus(user, context_->GetApplication().IsShiftDown(user) ? -1 : 1, false);
    case input::Key::Home:
        return MoveFocus(user, -1, true);
    case input::Key::End:
        return MoveFocus(user, 1, true);
    case input::Key::Left:
        // 하위 메뉴면 닫고 그 항목으로(항목이 초점을 되찾는다), 루트면 막대의 이전 메뉴로 간다.
        if (!IsRoot() && owner_) {
            owner_->Close();
            return true;
        }

        return context_->NavigateRoot(-1);
    case input::Key::Right:
        // 하위 메뉴 항목은 자기가 처리한다. 여기 오면(하위 메뉴 안의 일반 항목 포함) 막대의 다음
        // 메뉴로 간다(Windows).
        return context_->NavigateRoot(1);
    default:
        break;
    }

    // 보조 키 없이 누른 글자는 글자 점프다(Ctrl·Alt 조합은 단축키·막대 가속키로 남긴다).
    Application& app = context_->GetApplication();
    if (event.key >= input::Key::A && event.key <= input::Key::Z && !app.IsControlDown(user) &&
        !app.IsAltDown(user)) {
        const char letter = static_cast<char>(
            'A' + (static_cast<uint32>(event.key) - static_cast<uint32>(input::Key::A)));
        return JumpToLetter(user, letter);
    }

    return false;
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
    float content = 0;
    for (usize i = 0; i < ChildCount(); ++i) {
        if (ChildAt(i)->GetVisibility() != Visibility::Collapsed) {
            content += ChildAt(i)->GetDesiredSize().y;
        }
    }

    // 넘치면 위치 막대 자리만큼 항목 폭을 줄이고 스크롤 범위를 정한다.
    maxScroll_ = math::Max(0.0f, content - ViewHeight());
    scroll_ = math::Clamp(scroll_, 0.0f, maxScroll_);
    const float thumb = maxScroll_ > 0 ? context_->Style().scrollThumbWidth : 0.0f;
    const float width = math::Max(0.0f, geometry.size.x - 2 * outline - pad.x - pad.z - thumb);
    float y = ContentTop() - scroll_;
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
    // 넘치는 메뉴는 오른쪽 안쪽에 스크롤 위치 막대를 그린다(보이는 비율만큼의 길이).
    const MenuStyle& style = context_->Style();
    if (maxScroll_ > 0 && style.scrollThumb.mode != BrushMode::None) {
        const float view = ViewHeight();
        const float total = view + maxScroll_;
        const float length = math::Max(style.scrollThumbWidth * 2, view * view / total);
        const float top = ContentTop() + (view - length) * (scroll_ / maxScroll_);
        const float x = GetGeometry().size.x - style.outline.outlineWidth -
                        style.panelPadding.z * 0.5f - style.scrollThumbWidth;
        auto drawn = list.TryAddBrush(
            GetGeometry().MakeChild({style.scrollThumbWidth, length}, {1, 0, 0, 1, x, top}, {}, {}),
            style.scrollThumb.Resolve(list.Theme(), list.ColorContext()), LinearColor{1, 1, 1, 1},
            layer);
        if (!drawn) {
            return drawn;
        }
    }

    // 외곽선은 항목 올림 바탕 위에 그린다(UE "Menu.Outline").
    const StyleBrush& outline = context_->Style().outline;
    if (outline.mode == BrushMode::None) {
        return Result<void>::Success();
    }

    return list.TryAddBrush(GetGeometry(), outline.Resolve(list.Theme(), list.ColorContext()),
                            LinearColor{1, 1, 1, 1}, layer);
}

} // namespace rocks::ui
