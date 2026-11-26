#include <UiCore/Styles/CoreIcons.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Menus/MenuPanel.h>
#include <UiFramework/Menus/MenuRow.h>

namespace rocks::ui {

namespace {

// 아이콘 칸 폭은 아이콘 + 2다(UE WidthOverride(MenuIconSize + 2) — SMenuEntryBlock.cpp 588행).
constexpr float IconColumnExtra = 2.0f;

} // namespace

MenuRow::MenuRow(Allocator& allocator, const SharedPtr<MenuContext>& context) noexcept
    : MenuAnchor(allocator), context_(context) {
    const MenuStyle& style = context_->Style();
    (void)SetStyle(style.row);
    (void)SetContentPadding(style.rowPadding);
    // 하위 메뉴는 오른쪽에 연다(UE 메뉴 하위 메뉴 배치).
    SetPlacement(MenuPlacement::Right);
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
    hasSubMenu_ = static_cast<bool>(args.subMenu);
    if (hasSubMenu_) {
        SetMenuFactory(Move(args.subMenu));
        // 하위 메뉴 층은 메뉴 영역만 받는다 — 부모 메뉴의 hover·누름이 그대로 가야 한다.
        SetCatchOutsidePresses(false);
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

    if (hasSubMenu_) {
        const Vec2 size{style.subMenuIconSize, style.subMenuIconSize};
        const float x = geometry.size.x - style.rowPadding.z - size.x;
        const auto at = geometry.MakeChild(size, {1, 0, 0, 1, x, middle - size.y * 0.5f}, {}, {});
        painted = list.TryAddBrush(at, Brush::Vector(style.subMenuIcon, size), ink, layer);
    }

    return painted;
}

bool MenuRow::IsHighlighted() const noexcept {
    return MenuAnchor::IsHighlighted() || IsOpen();
}

void MenuRow::OnMouseEnter(uint32 user) noexcept {
    MenuAnchor::OnMouseEnter(user);
    if (panel_ && IsEnabled()) {
        panel_->OnRowHovered(*this);
    }
}

bool MenuRow::OnKeyEvent(uint32 user, const input::InputEvent& event) noexcept {
    // 오른쪽은 하위 메뉴를 열고 첫 항목으로 간다(키보드 — 외곽선을 잇는다).
    if (event.type == input::InputEventType::KeyDown && event.key == input::Key::Right &&
        hasSubMenu_ && IsEnabled()) {
        (void)OpenFocused(context_->GetApplication(), user, FocusCause::Navigation);
        return true;
    }

    return MenuAnchor::OnKeyEvent(user, event);
}

void MenuRow::OnActivated() noexcept {
    if (hasSubMenu_) {
        Application& app = context_->GetApplication();
        // 키보드(Enter·Space)로 누르면 하위 메뉴 첫 항목으로 가고, 마우스는 열기만 한다.
        if (app.GetFocusCause(0) == FocusCause::Navigation && app.GetFocus(0).Get() == this) {
            (void)OpenFocused(app, 0, FocusCause::Navigation);
        } else if (!IsOpen()) {
            (void)Open();
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
