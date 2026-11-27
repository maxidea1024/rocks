#include <UiCore/Styles/CoreIcons.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Menus/MenuPanel.h>
#include <UiFramework/Menus/MenuRow.h>
#include <UiFramework/Popups/ToolTip.h>

namespace rocks::ui {

namespace {

// 아이콘 칸 폭은 아이콘 + 2다(UE WidthOverride(MenuIconSize + 2) — SMenuEntryBlock.cpp 588행).
constexpr float IconColumnExtra = 2.0f;

} // namespace

MenuRow::MenuRow(Allocator& allocator, const SharedPtr<MenuContext>& context) noexcept
    : MenuAnchor(allocator), context_(context), toolTip_(allocator) {
    const MenuStyle& style = context_->Style();
    // 기본 메뉴 스타일 값은 늘 유효하다(IsValidStyle 실패는 호출자 스타일 오류뿐).
    (void)SetStyle(style.row);
    (void)SetContentPadding(style.rowPadding);
    // 하위 메뉴는 오른쪽에 열고, 첫 항목이 이 항목과 같은 높이가 되게 패널 여백·외곽선만큼 올린다.
    SetPlacement(MenuPlacement::Right);
    SetMenuOffset({0, -(style.panelPadding.y + style.outline.outlineWidth)});
}

Result<void> MenuRow::Construct(Args&& args) noexcept {
    if (!args.label) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const MenuStyle& style = context_->Style();
    label_ = Move(args.label);
    label_->SetColor(style.label.color);
    auto added = TryAddChild(label_);
    if (added && args.keybinding) {
        keybinding_ = Move(args.keybinding);
        keybinding_->SetColor(style.keybinding.color);
        added = TryAddChild(keybinding_);
    }

    if (!added) {
        return added;
    }

    check_ = args.check;
    isChecked_ = Move(args.isChecked);
    execute_ = Move(args.execute);
    icon_ = args.icon;
    hasSubMenu_ = static_cast<bool>(args.subMenu);
    if (hasSubMenu_) {
        SetMenuFactory(Move(args.subMenu));
        // 하위 메뉴 층은 메뉴 영역만 받는다 — 부모 메뉴의 hover·누름이 그대로 가야 한다.
        SetCatchOutsidePresses(false);
        SetOnOpenChanged(Delegate<void(bool)>::CreateLambda(
            [this](bool open) noexcept { OnSubMenuOpenChanged(open); }));
    }

    if (!args.toolTip.IsEmpty()) {
        auto copied = toolTip_.TryAssign(args.toolTip);
        if (copied) {
            copied = SetToolTipFactory(
                ToolTipFactory::CreateLambda([this]() noexcept -> Result<SharedPtr<Element>> {
                    auto text =
                        context_->MakeLabel(toolTip_.View(), DefaultToolTipStyle().text.font);
                    if (!text) {
                        return Result<SharedPtr<Element>>::Failure(text.Error());
                    }

                    Allocator& allocator = context_->GetAllocator();
                    auto tip = SharedPtr<ToolTip>::TryMake(allocator, allocator);
                    if (!tip) {
                        return Result<SharedPtr<Element>>::Failure(tip.Error());
                    }

                    auto built = tip.Value()->Construct({.text = Move(text.Value())});
                    if (!built) {
                        return Result<SharedPtr<Element>>::Failure(built.Error());
                    }

                    return Result<SharedPtr<Element>>::Success(Move(tip.Value()));
                }),
                args.toolTip);
        }

        if (!copied) {
            return copied;
        }
    }

    SetEnabled(!args.canExecute.IsBound() || args.canExecute.Execute());
    return Result<void>::Success();
}

bool MenuRow::HasSubMenu() const noexcept {
    return hasSubMenu_;
}

bool MenuRow::IsChecked() const noexcept {
    return isChecked_.IsBound() && isChecked_.Execute();
}

MenuCheckKind MenuRow::CheckKind() const noexcept {
    return check_;
}

StringView MenuRow::LabelText() const noexcept {
    return label_ ? label_->GetText() : StringView{};
}

VectorImageKey MenuRow::Icon() const noexcept {
    return icon_;
}

bool MenuRow::IsHighlighted() const noexcept {
    // hover 강조는 키보드로 옮긴 뒤 포인터가 다시 움직일 때까지 끈다(강조 하나 — R16).
    if (MenuAnchor::IsHighlighted() && !(panel_ && panel_->IsKeyboardNavigating())) {
        return true;
    }

    // 키보드로 고른 항목이다(포인터로 옮긴 초점은 hover가 강조한다).
    Application& app = context_->GetApplication();
    if (app.GetFocusCause(0) == FocusCause::Navigation && app.GetFocus(0).Get() == this) {
        return true;
    }

    // 하위 메뉴가 열린 항목은 패널에서 다른 항목을 hover하지 않는 동안 강조를 유지한다(하위
    // 메뉴로 옮겨 가도 단계마다 강조 — UE ShouldSubMenuAppearHovered).
    if (IsOpen()) {
        const MenuRow* hovered = panel_ ? panel_->HoveredRow() : nullptr;
        return hovered == nullptr || hovered == this;
    }

    return false;
}

void MenuRow::SetPanel(MenuPanel* panel) noexcept {
    panel_ = panel;
}

Result<void> MenuRow::TryAddChild(const SharedPtr<Element>& child) noexcept {
    // 라벨과 단축키 두 자식을 직접 놓는다(Frame의 자식 하나 제한을 쓰지 않는다).
    return Element::TryAddChild(child);
}

float MenuRow::IconColumnWidth() const noexcept {
    const MenuStyle& style = context_->Style();
    return style.iconPadding.x + style.iconSize + IconColumnExtra + style.iconPadding.z;
}

float MenuRow::TrailingWidth() const noexcept {
    const MenuStyle& style = context_->Style();
    return hasSubMenu_ ? style.subMenuGap + style.subMenuIconSize : 0.0f;
}

float MenuRow::ContentOffsetY() const noexcept {
    // 눌린 동안 내용을 1 px 내린다(UE Menu.Button 눌림 여백 (0, 3, 0, 1)).
    const MenuStyle& style = context_->Style();
    return IsPressed() ? style.row.pressedPadding.y : style.row.normalPadding.y;
}

Vec2 MenuRow::ComputeDesiredSize(float) const noexcept {
    const MenuStyle& style = context_->Style();
    const Vec4 row = style.rowPadding;
    const Vec4 button = style.row.normalPadding;
    const Vec2 label = label_ ? label_->GetDesiredSize() : Vec2{};
    const Vec2 keys = keybinding_ ? keybinding_->GetDesiredSize() : Vec2{};
    float width = row.x + IconColumnWidth() + style.labelPadding.x + label.x +
                  style.labelPadding.z + TrailingWidth() + row.z + button.x + button.z;
    if (keybinding_) {
        width +=
            style.keybindingGap + style.keybindingPadding.x + keys.x + style.keybindingPadding.z;
    }

    const float content =
        math::Max(math::Max(label.y, keys.y),
                  math::Max(style.iconSize, hasSubMenu_ ? style.subMenuIconSize : 0.0f));
    return {width, content + row.y + row.w + button.y + button.w};
}

Result<void> MenuRow::Arrange(const Geometry& geometry) noexcept {
    SetGeometry(geometry);
    const MenuStyle& style = context_->Style();
    const Vec4 row = style.rowPadding;
    const float top = row.y + ContentOffsetY();
    const float height = math::Max(0.0f, geometry.size.y - row.y - row.w -
                                             style.row.normalPadding.y - style.row.normalPadding.w);
    const auto place = [&](TextLabel& label, float x, float width) noexcept {
        const float h = math::Min(height, label.GetDesiredSize().y);
        return label.ArrangeIfNeeded(GetGeometry().MakeChild(
            {math::Max(0.0f, width), h}, {1, 0, 0, 1, x, top + (height - h) * 0.5f}, {}, {}));
    };

    // 단축키는 오른쪽(하위 메뉴 표시 앞)에, 라벨은 아이콘 칸 뒤 남는 폭에 둔다.
    float right = geometry.size.x - row.z - TrailingWidth();
    if (keybinding_) {
        const float keys = keybinding_->GetDesiredSize().x;
        right -= style.keybindingPadding.z + keys;
        auto placed = place(*keybinding_, right, keys);
        if (!placed) {
            return placed;
        }

        right -= style.keybindingPadding.x + style.keybindingGap;
    }

    const float labelX = row.x + IconColumnWidth() + style.labelPadding.x;
    return place(*label_, labelX, right - style.labelPadding.z - labelX);
}

Result<void> MenuRow::OnPaintOverlay(PaintList& list, int32 layer) const noexcept {
    auto painted = MenuAnchor::OnPaintOverlay(list, layer);
    if (!painted) {
        return painted;
    }

    const MenuStyle& style = context_->Style();
    const Geometry& geometry = GetGeometry();
    const LinearColor ink = list.ForegroundColor();
    const float middle = geometry.size.y * 0.5f + ContentOffsetY() - style.row.normalPadding.y;
    // 체크·라디오 표시는 아이콘 칸 가운데다.
    if (check_ != MenuCheckKind::None) {
        const float columnX = style.rowPadding.x + style.iconPadding.x;
        const float columnWidth = style.iconSize + IconColumnExtra;
        if (check_ == MenuCheckKind::Check && IsChecked()) {
            const Vec2 size{style.iconSize, style.iconSize};
            const auto at = geometry.MakeChild(
                size, {1, 0, 0, 1, columnX + (columnWidth - size.x) * 0.5f, middle - size.y * 0.5f},
                {}, {});
            painted = list.TryAddBrush(at, Brush::Vector(style.checkIcon, size), ink, layer);
        } else if (check_ == MenuCheckKind::Radio) {
            const Vec2 size{style.radioSize, style.radioSize};
            const auto at = geometry.MakeChild(
                size, {1, 0, 0, 1, columnX + (columnWidth - size.x) * 0.5f, middle - size.y * 0.5f},
                {}, {});
            const StyleBrush& dot = IsChecked() ? style.radioOn : style.radioOff;
            painted = list.TryAddBrush(at, dot.Resolve(list.Theme(), list.ColorContext()),
                                       LinearColor{1, 1, 1, 1}, layer);
        }

        if (!painted) {
            return painted;
        }
    }

    // 아이콘은 체크 표시가 없는 항목의 아이콘 칸 가운데다.
    if (check_ == MenuCheckKind::None && icon_ != VectorImageKey{}) {
        const float columnX = style.rowPadding.x + style.iconPadding.x;
        const float columnWidth = style.iconSize + IconColumnExtra;
        const Vec2 size{style.iconSize, style.iconSize};
        const auto at = geometry.MakeChild(
            size, {1, 0, 0, 1, columnX + (columnWidth - size.x) * 0.5f, middle - size.y * 0.5f}, {},
            {});
        painted = list.TryAddBrush(at, Brush::Vector(icon_, size), ink, layer);
        if (!painted) {
            return painted;
        }
    }

    if (hasSubMenu_) {
        const Vec2 size{style.subMenuIconSize, style.subMenuIconSize};
        const float x = geometry.size.x - style.rowPadding.z - size.x;
        const auto at = geometry.MakeChild(size, {1, 0, 0, 1, x, middle - size.y * 0.5f}, {}, {});
        painted = list.TryAddBrush(at, Brush::Vector(style.subMenuIcon, size), ink, layer);
    }

    return painted;
}

void MenuRow::OnMouseEnter(uint32 user) noexcept {
    MenuAnchor::OnMouseEnter(user);
    if (!panel_ || !IsEnabled()) {
        return;
    }

    // 포인터가 고른 항목으로 키보드 초점도 옮긴다(Enter가 보이는 항목을 실행하고, 강조가 하나다).
    // 초점 콜백 중이라 Busy면 다음 hover에서 맞춘다.
    panel_->OnPointerMoved();
    if (auto self = Parent() ? Parent()->FindChild(this) : SharedPtr<Element>{}) {
        (void)context_->GetApplication().SetFocus(user, self, FocusCause::Pointer);
    }

    panel_->OnRowHovered(*this);
}

void MenuRow::OnMouseLeave(uint32 user) noexcept {
    MenuAnchor::OnMouseLeave(user);
    if (!panel_) {
        return;
    }

    panel_->OnRowUnhovered(*this);
    // 포인터로 옮긴 초점은 항목을 떠나면 패널로 돌린다 — 강조 없는 항목을 Enter가 실행하지 않는다.
    // 하위 메뉴가 열린 항목(하위 메뉴로 가는 중)과 키보드로 고른 항목은 그대로 둔다.
    Application& app = context_->GetApplication();
    if (!IsOpen() && app.GetFocus(user).Get() == this &&
        app.GetFocusCause(user) == FocusCause::Pointer) {
        panel_->FocusPanel(user);
    }
}

PointerReply MenuRow::OnPointerEvent(const PointerEvent& event) noexcept {
    if (event.type == PointerEventType::Move && panel_) {
        panel_->OnPointerMoved();
    }

    return MenuAnchor::OnPointerEvent(event);
}

void MenuRow::OnFocusReceived(uint32 user) noexcept {
    MenuAnchor::OnFocusReceived(user);
    InvalidatePaint();
}

void MenuRow::OnFocusLost(uint32 user) noexcept {
    MenuAnchor::OnFocusLost(user);
    InvalidatePaint();
}

Optional<bool> MenuRow::OnQueryShowFocus(FocusCause) const noexcept {
    // 초점은 외곽선이 아니라 Primary 강조로 보인다.
    return false;
}

void MenuRow::Invoke() noexcept {
    OnActivated();
}

Result<void> MenuRow::OpenSubMenuFocused(uint32 user) noexcept {
    if (!hasSubMenu_ || !IsEnabled()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (panel_) {
        panel_->SwitchSubMenu(this);
    }

    auto* sub = StaticElementCast<MenuPanel>(MenuContent());
    if (!sub) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    return sub->FocusFirstItem(user) ? Result<void>::Success()
                                     : Result<void>::Failure(ErrorCode::NotFound);
}

void MenuRow::OnSubMenuOpenChanged(bool open) noexcept {
    if (open) {
        return;
    }

    // 하위 메뉴가 닫혔는데 초점이 그 안(이제 트리에서 떨어진 항목)에 있으면 이 항목으로 돌아온다.
    Application& app = context_->GetApplication();
    const auto focus = app.GetFocus(0);
    const auto& root = app.GetRoot();
    if (!focus || !root || focus->IsWithin(*root)) {
        return;
    }

    // 초점 콜백 중(Busy)이면 그대로 둔다 — 떨어진 초점은 다음 입력에서 Application이 지운다.
    if (auto self = Parent() ? Parent()->FindChild(this) : SharedPtr<Element>{}) {
        (void)app.SetFocus(0, self, app.GetFocusCause(0));
    }
}

KeyReply MenuRow::OnKeyEvent(const KeyEvent& key) noexcept {
    const uint32 user = key.user;
    const input::InputEvent& event = key.input;
    // 오른쪽은 하위 메뉴를 열고 첫 항목으로 간다(키보드 — 외곽선을 잇는다).
    if (event.type == input::InputEventType::KeyDown && event.key == input::Key::Right &&
        hasSubMenu_ && IsEnabled()) {
        // 채우기 실패(빈 하위 메뉴)면 그대로 이 항목에 머문다.
        (void)OpenSubMenuFocused(user);
        return KeyReply::Handled();
    }

    return MenuAnchor::OnKeyEvent(key);
}

void MenuRow::OnActivated() noexcept {
    if (hasSubMenu_) {
        Application& app = context_->GetApplication();
        // 키보드(Enter·Space)로 누르면 하위 메뉴 첫 항목으로 가고, 마우스는 열기만 한다.
        // 열기 실패(채우기 오류)면 항목에 머문다 — 메뉴 동작은 실패를 보이지 않는다.
        if (app.GetFocusCause(0) == FocusCause::Navigation && app.GetFocus(0).Get() == this) {
            (void)OpenSubMenuFocused(0);
        } else if (!IsOpen() && panel_) {
            panel_->SwitchSubMenu(this);
        }

        return;
    }

    // 메뉴를 닫으면 이 항목이 트리에서 떨어져 해제될 수 있어 실행 동안 자기와 문맥을 잡아 둔다(UE도
    // 메뉴를 닫은 뒤 실행한다).
    const SharedPtr<Element> self = Parent() ? Parent()->FindChild(this) : SharedPtr<Element>{};
    const SharedPtr<MenuContext> context = context_;
    context->CloseAll();
    execute_.ExecuteIfBound();
}

} // namespace rocks::ui
