#include "ComboRow.h"

#include <Base/NumericLimits.h>
#include <UiCore/UiHostServices.h>
#include <UiFramework/Controls/ComboBox.h>
#include <UiFramework/Scrolling/ScrollView.h>
#include <UiFramework/Layout/VerticalStack.h>
#include <Core/ScopeExit.h>

namespace rocks::ui {

namespace {

// 스타일 키다(StyleSet).
constexpr StringView ComboBoxStyleKey = "ComboBox";
constexpr StringView ComboBoxRowStyleKey = "ComboBox.Row";

bool IsValidPadding(Vec4 padding) noexcept {
    return padding.IsFinite() && padding.x >= 0 && padding.y >= 0 && padding.z >= 0 &&
           padding.w >= 0;
}

} // namespace

ComboBox::ComboBox(Allocator& allocator) noexcept
    : MenuAnchor(allocator), allocator_(allocator), rows_(allocator) {
    // 내용은 세로 가운데다(UE SComboButton 기본 VAlign_Center — SComboButton.h 45행).
    SetContentAlignment(StackAlignment::Fill, StackAlignment::Center);
    SetPlacement(MenuPlacement::ComboBox);
    SetMenuFactory(MenuFactory::CreateRaw(this, &ComboBox::MakeMenu));
    (void)ApplyComboStyle(comboStyle_);
}

int32 ComboBox::HighlightedIndex() const noexcept {
    return IsOpen() ? highlighted_ : -1;
}

Result<void> ComboBox::SetComboStyle(const ComboBoxStyle& style) noexcept {
    auto applied = ApplyComboStyle(style);
    if (applied) {
        comboStyleExplicit_ = true;
    }

    return applied;
}

const ComboBoxStyle& ComboBox::GetComboStyle() const noexcept {
    return comboStyle_;
}

void ComboBox::SetRowStyle(const TableRowStyle& style) noexcept {
    rowStyle_ = style;
    rowStyleExplicit_ = true;
}

const TableRowStyle& ComboBox::GetRowStyle() const noexcept {
    return rowStyle_;
}

void ComboBox::ApplyStyles(const UiHostServices& host) noexcept {
    // 활성 집합에 키가 없으면 내장 기본값이다(20.11절).
    if (!comboStyleExplicit_) {
        const ComboBoxStyle* style = host.FindStyle<ComboBoxStyle>(ComboBoxStyleKey);
        (void)ApplyComboStyle(style ? *style : DefaultComboBoxStyle());
    }

    if (!rowStyleExplicit_) {
        const TableRowStyle* row = host.FindStyle<TableRowStyle>(ComboBoxRowStyleKey);
        rowStyle_ = row ? *row : DefaultComboBoxRowStyle();
    }
}

Result<void> ComboBox::ApplyComboStyle(const ComboBoxStyle& style) noexcept {
    const ComboButtonStyle& combo = style.comboButton;
    if (!Button::IsValidStyle(combo.button) || !IsValidPadding(style.contentPadding) ||
        !IsValidPadding(combo.downArrowPadding) || !IsValidPadding(combo.menuBorderPadding) ||
        !IsValidPadding(style.menuRowPadding) || !combo.downArrow.imageSize.IsFinite() ||
        combo.downArrow.imageSize.x < 0 || combo.downArrow.imageSize.y < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 버튼 모양은 콤보 버튼 스타일이다(이 버튼의 명시 스타일이 된다 — "Button" 키를 받지 않는다).
    auto applied = SetStyle(combo.button);
    if (!applied) {
        return applied;
    }

    comboStyle_ = style;
    // 화살표 칸을 내용 오른쪽 여백으로 비워 둔다(UE SComboButton 화살표 슬롯 AutoWidth).
    return SetContentPadding({style.contentPadding.x, style.contentPadding.y,
                              style.contentPadding.z + ArrowZone(), style.contentPadding.w});
}

Vec2 ComboBox::ComputeDesiredSize(float scale) const noexcept {
    const Vec2 base = MenuAnchor::ComputeDesiredSize(scale);
    const ComboButtonStyle& combo = comboStyle_.comboButton;
    const Vec4 padding = GetStyle().normalPadding;
    const float arrow = combo.downArrowPadding.y + combo.downArrow.imageSize.y +
                        combo.downArrowPadding.w + padding.y + padding.w;
    return {base.x, math::Max(base.y, arrow)};
}

float ComboBox::ArrowZone() const noexcept {
    const ComboButtonStyle& combo = comboStyle_.comboButton;
    return combo.downArrowPadding.x + combo.downArrow.imageSize.x + combo.downArrowPadding.z;
}

ButtonStyle ComboBox::RowButtonStyle() const noexcept {
    // 행 바탕은 행 스타일(보통 없음·올림 Primary), 글자는 행 글자색, 여백은 행 여백이다.
    ButtonStyle row;
    row.normal = rowStyle_.evenRow;
    row.hovered = rowStyle_.evenRowHovered;
    row.pressed = rowStyle_.evenRowHovered;
    row.disabled = rowStyle_.evenRow;
    row.normalPadding = comboStyle_.menuRowPadding;
    row.pressedPadding = comboStyle_.menuRowPadding;
    row.normalForeground = rowStyle_.text;
    row.hoveredForeground = rowStyle_.text;
    row.pressedForeground = rowStyle_.text;
    row.disabledForeground = rowStyle_.text;
    return row;
}

Result<void> ComboBox::OnPaint(PaintList& list, int32 layer) const noexcept {
    auto painted = MenuAnchor::OnPaint(list, layer);
    if (!painted) {
        return painted;
    }

    // 화살표는 오른쪽 칸에 세로 가운데다(UE SComboButton 화살표 슬롯 — 버튼 여백 안, 화살표 여백
    // 안). 흰 이미지에 버튼 전경색을 곱한다(UE UseForeground).
    const ComboButtonStyle& combo = comboStyle_.comboButton;
    const Vec2 arrowSize = combo.downArrow.imageSize;
    const auto& geometry = GetGeometry();
    const float right = GetStyle().normalPadding.z;
    const float x = geometry.size.x - right - combo.downArrowPadding.z - arrowSize.x;
    const float y = (geometry.size.y - arrowSize.y) * 0.5f;
    if (x < 0 || geometry.size.y <= 0) {
        return Result<void>::Success();
    }

    const bool disabled = !IsEnabled() || list.IsDisabled();
    const StyleColorContext context = list.ColorContext();
    const LinearColor ink = PaintForegroundColor(disabled).Resolve(list.Theme(), context);
    const auto arrow = geometry.MakeChild(arrowSize, {1, 0, 0, 1, x, y}, {}, {});
    return list.TryAddBrush(arrow, combo.downArrow.Resolve(list.Theme(), context), ink, layer);
}

Result<void> ComboBox::Construct(Args&& args) noexcept {
    auto limit = SetMaxListHeight(args.maxListHeight);
    if (!limit) {
        return limit;
    }

    SetOptionFactory(Move(args.makeOption));
    SetOnSelectionChanged(Move(args.onSelectionChanged));
    SetOnOpening(Move(args.onOpening));
    return Refresh(args.count, args.selected);
}

void ComboBox::DisconnectModel() noexcept {
    DisconnectMenu();
    makeOption_.Unbind();
    selectionChanged_.Unbind();
    UpdateOptions(0, -1);
}

Result<void> ComboBox::SetMaxListHeight(float height) noexcept {
    if (!math::IsFinite(height) || height <= 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    maxListHeight_ = height;
    if (auto bounds = menuBounds_.Lock()) {
        return bounds->SetSizeRange({}, {NumericLimits<float>::Max(), height});
    }

    return Result<void>::Success();
}

void ComboBox::SetOptionFactory(MakeOption&& factory) noexcept {
    makeOption_ = Move(factory);
}

void ComboBox::SetOnSelectionChanged(Delegate<void(int32, SelectionCause)>&& callback) noexcept {
    selectionChanged_ = Move(callback);
}

int32 ComboBox::SelectedIndex() const noexcept {
    return selected_;
}

Result<void> ComboBox::SetDisplay(int32 index) noexcept {
    auto old = ChildRefAt(0);
    SharedPtr<Element> display;
    if (index >= 0) {
        if (!makeOption_) {
            return Result<void>::Failure(ErrorCode::NotReady);
        }

        auto made = makeOption_.Execute(static_cast<usize>(index), true);
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        display = Move(made.Value());
        if (!display || display->Parent() || IsWithin(*display)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        display->SetVisibility(Visibility::HitTestInvisible);
    }

    if (old) {
        (void)RemoveChild(*old);
    }

    if (display) {
        auto added = TryAddChild(display);
        if (!added) {
            if (old) {
                (void)TryAddChild(old);
            }

            return added;
        }
    }

    return Result<void>::Success();
}

Result<void> ComboBox::Refresh(usize count, int32 selected) noexcept {
    if (changing_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (count > static_cast<usize>(NumericLimits<int32>::Max()) || selected < -1 ||
        (selected >= 0 && static_cast<usize>(selected) >= count)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    changing_ = true;
    ROCKS_ON_SCOPE_EXIT {
        changing_ = false;
    };
    auto updated = SetDisplay(selected);
    if (updated) {
        Close();
        rows_.Clear();
        count_ = count;
        selected_ = selected;
    }

    return updated;
}

void ComboBox::UpdateOptions(usize count, int32 selected) noexcept {
    Close();
    rows_.Clear();
    count_ = count;
    const bool cleared = selected_ >= 0 && selected < 0;
    selected_ = selected;
    if (selected < 0) {
        if (auto display = ChildRefAt(0)) {
            (void)RemoveChild(*display);
        }
    }

    if (cleared) {
        selectionChanged_.ExecuteIfBound(-1, SelectionCause::Direct);
    }
}

Result<void> ComboBox::Select(int32 index, SelectionCause cause) noexcept {
    if (index == selected_) {
        Close();
        return Result<void>::Success();
    }

    auto changed = Refresh(count_, index);
    if (changed) {
        selectionChanged_.ExecuteIfBound(index, cause);
    }

    return changed;
}

void ComboBox::Highlight(int32 index) noexcept {
    highlighted_ = index;
    if (auto scroll = scroll_.Lock();
        scroll && index >= 0 && static_cast<usize>(index) < rows_.Size()) {
        auto row = rows_[static_cast<usize>(index)].Lock();
        const auto& viewport = scroll->GetGeometry();
        if (row && viewport.size.y > 0) {
            AffineTransform2D inverse;
            if (viewport.render.TryInverse(inverse)) {
                const auto position = inverse.Compose(row->GetGeometry().render).Apply({});
                const float overflow = position.y + row->GetGeometry().size.y - viewport.size.y;
                if (position.y < 0 || overflow > 0) {
                    (void)scroll->SetScrollOffset(scroll->GetScrollOffset() +
                                                  (position.y < 0 ? position.y : overflow));
                }
            }
        }
    }

    // 강조 행은 올림 모양이다(ComboRow::IsHighlighted) — 다시 그린다.
    for (usize i = 0; i < rows_.Size(); ++i) {
        if (auto row = rows_[i].Lock()) {
            row->InvalidatePaint();
        }
    }
}

Result<SharedPtr<Element>> ComboBox::MakeMenu() noexcept {
    if (!makeOption_ || !Parent() || !count_ || changing_) {
        return Result<SharedPtr<Element>>::Failure(changing_ ? ErrorCode::Busy
                                                             : ErrorCode::NotReady);
    }

    changing_ = true;
    ROCKS_ON_SCOPE_EXIT {
        changing_ = false;
    };
    auto stack = SharedPtr<VerticalStack>::TryMake(allocator_, allocator_);
    if (!stack) {
        return Result<SharedPtr<Element>>::Failure(stack.Error());
    }

    Array<WeakPtr<Button>> rows(allocator_);
    const auto self = Parent()->FindChild(this);
    const ButtonStyle rowButton = RowButtonStyle();
    SharedPtr<Element> selectedRow;
    for (usize i = 0; i < count_; ++i) {
        auto content = makeOption_.Execute(i, false);
        if (!content || !content.Value() || content.Value()->Parent()) {
            return Result<SharedPtr<Element>>::Failure(content ? ErrorCode::InvalidArgument
                                                               : content.Error());
        }

        auto row =
            SharedPtr<ComboRow>::TryMake(allocator_, allocator_, *this, static_cast<int32>(i));
        if (!row) {
            return Result<SharedPtr<Element>>::Failure(row.Error());
        }

        content.Value()->SetVisibility(Visibility::HitTestInvisible);
        auto added = row.Value()->SetStyle(rowButton);
        if (added) {
            added = row.Value()->TryAddChild(content.Value());
        }

        if (added) {
            added = stack.Value()->TryAddChild(row.Value());
        }

        if (added) {
            added = rows.TryPushBack(WeakPtr<Button>(row.Value()));
        }

        if (static_cast<int32>(i) == selected_) {
            selectedRow = row.Value();
        }

        if (!added) {
            return Result<SharedPtr<Element>>::Failure(added.Error());
        }

        row.Value()->SetOnClicked(Delegate<void()>::CreateWeakLambda(self, [this, i]() noexcept {
            if (IsOpen()) {
                (void)Select(static_cast<int32>(i), SelectionCause::Pointer);
            }
        }));
    }

    auto scroll = SharedPtr<ScrollView>::TryMake(allocator_, allocator_);
    if (!scroll) {
        return Result<SharedPtr<Element>>::Failure(scroll.Error());
    }

    auto attached = scroll.Value()->TryAddChild(stack.Value());
    if (!attached) {
        return Result<SharedPtr<Element>>::Failure(attached.Error());
    }

    auto bounds = SharedPtr<SizeBox>::TryMake(allocator_, allocator_);
    auto border = SharedPtr<Frame>::TryMake(allocator_, allocator_);
    if (!bounds || !border) {
        return Result<SharedPtr<Element>>::Failure(!bounds ? bounds.Error() : border.Error());
    }

    // 목록 바탕과 안쪽 여백이다(UE MenuBorderBrush·MenuBorderPadding — 테마를 따른다).
    const ComboButtonStyle& combo = comboStyle_.comboButton;
    attached = border.Value()->SetStyleBrush(combo.menuBorder);
    if (attached) {
        attached = border.Value()->SetPadding(combo.menuBorderPadding);
    }

    if (attached) {
        attached = border.Value()->TryAddChild(scroll.Value());
    }

    if (attached) {
        attached = bounds.Value()->SetSizeRange({}, {NumericLimits<float>::Max(), maxListHeight_});
    }

    if (attached) {
        attached = bounds.Value()->TryAddChild(border.Value());
    }

    if (!attached) {
        return Result<SharedPtr<Element>>::Failure(attached.Error());
    }

    // 처음 펼칠 때 선택 행이 보이게 한다 — 행 높이를 어림하지 않고 다음 배치에서 맞춘다(23.2절).
    if (selectedRow) {
        scroll.Value()->RequestScrollIntoView(selectedRow, ScrollIntoViewAlignment::Nearest);
    }

    menuBounds_ = WeakPtr<SizeBox>(bounds.Value());
    scroll_ = WeakPtr<ScrollView>(scroll.Value());
    rows_ = Move(rows);
    Highlight(selected_ >= 0 ? selected_ : 0);
    return Result<SharedPtr<Element>>::Success(Move(bounds.Value()));
}

KeyReply ComboBox::OnKeyEvent(const KeyEvent& key) noexcept {
    const input::InputEvent& event = key.input;
    if (event.type == input::InputEventType::KeyDown && count_) {
        const bool up = event.key == input::Key::Up;
        if (up || event.key == input::Key::Down) {
            const int32 current = IsOpen() ? highlighted_ : selected_;
            const int32 last = static_cast<int32>(count_) - 1;
            const int32 next =
                up ? (current > 0 ? current - 1 : 0) : (current < last ? current + 1 : last);
            if (IsOpen()) {
                Highlight(next);
            } else {
                (void)Select(next, SelectionCause::Key);
            }

            return KeyReply::Handled();
        }

        // 확인 동작(Enter·Space·패드 South)이면 강조 항목을 고른다(20.8절).
        if (IsOpen() && key.action == NavigationAction::Accept) {
            (void)Select(highlighted_, SelectionCause::Direct);
            return KeyReply::Handled();
        }
    }

    return MenuAnchor::OnKeyEvent(key);
}

} // namespace rocks::ui
