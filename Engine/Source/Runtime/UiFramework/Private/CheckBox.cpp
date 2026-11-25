#include <UiFramework/CheckBox.h>

namespace rocks::ui {

namespace {

// 체크 표시 꺾은선의 세 점이다(상자 크기에 대한 비율). UE check.svg는 UE 클론에 없어 대조하지
// 못했고, 16 단위 아이콘의 (3.5, 8.5) → (6.5, 11.5) → (12.5, 4.5)를 근사로 쓴다.
constexpr Vec2 CheckPoints[] = {
    {3.5f / 16.0f, 8.5f / 16.0f}, {6.5f / 16.0f, 11.5f / 16.0f}, {12.5f / 16.0f, 4.5f / 16.0f}};
// 선 두께(16 단위 아이콘에서 2)와 미정 표시 막대 너비의 상자 비율이다.
constexpr float MarkThickness = 2.0f / 16.0f;
constexpr float DashWidth = 0.5f;

} // namespace

CheckBox::CheckBox(Allocator& allocator) noexcept : Button(allocator) {
    // 상자는 CheckBoxStyle로 그리므로 버튼 brush·여백은 쓰지 않는다.
    (void)SetStyle(ButtonStyle{});
    UpdateContentPadding();
}

Result<void> CheckBox::SetCheckBoxStyle(const CheckBoxStyle& style) noexcept {
    const auto validSize = [](Vec2 size) noexcept {
        return size.IsFinite() && size.x > 0 && size.y > 0;
    };
    const auto validPadding = [](Vec4 padding) noexcept {
        return padding.IsFinite() && padding.x >= 0 && padding.y >= 0 && padding.z >= 0 &&
               padding.w >= 0;
    };
    const ColorRef colors[] = {style.checkedColor,
                               style.checkedHoveredColor,
                               style.checkedPressedColor,
                               style.undeterminedColor,
                               style.undeterminedHoveredColor,
                               style.undeterminedPressedColor,
                               style.foreground,
                               style.hoveredForeground,
                               style.pressedForeground,
                               style.checkedForeground,
                               style.checkedHoveredForeground,
                               style.checkedPressedForeground,
                               style.undeterminedForeground};
    for (const ColorRef& color : colors) {
        if (!color.IsValid()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    const StyleBrush* brushes[] = {&style.background, &style.backgroundHovered,
                                   &style.backgroundPressed};
    for (const StyleBrush* brush : brushes) {
        if (!brush->Resolve(DefaultThemeColors(), {}).IsValid()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    if (!validSize(style.boxSize) || !validPadding(style.padding)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (checkStyle_ != style) {
        checkStyle_ = style;
        InvalidatePaint();
        UpdateContentPadding();
    }

    return Result<void>::Success();
}

const CheckBoxStyle& CheckBox::GetCheckBoxStyle() const noexcept {
    return checkStyle_;
}

Result<void> CheckBox::Arrange(const Geometry& geometry) noexcept {
    // 상자 오른쪽 내용은 세로 가운데다(UE SCheckBox 내용 슬롯 VAlign_Center, 558행).
    SetGeometry(geometry);
    Element* child = ChildAt(0);
    if (!child || child->GetVisibility() == Visibility::Collapsed) {
        return Result<void>::Success();
    }

    const Vec4 padding = GetContentPadding();
    const Vec2 inner{math::Max(0.0f, geometry.size.x - padding.x - padding.z),
                     math::Max(0.0f, geometry.size.y - padding.y - padding.w)};
    const float height = math::Min(inner.y, child->GetDesiredSize().y);
    const float top = padding.y + (inner.y - height) * 0.5f;
    return child->ArrangeIfNeeded(
        GetGeometry().MakeChild({inner.x, height}, {1, 0, 0, 1, padding.x, top}, {}, {}));
}

void CheckBox::UpdateContentPadding() noexcept {
    const Vec4 padding = checkStyle_.padding;
    (void)SetContentPadding({checkStyle_.boxSize.x + padding.x, padding.y, padding.z, padding.w});
    SetDesiredSize(checkStyle_.boxSize);
}

ColorRef CheckBox::GetForegroundColor() const noexcept {
    // 사용자가 정한 전경색이 있으면 그것이다(UE ForegroundColorOverride가 UseStyle이 아닐 때,
    // SCheckBox.cpp 310–326행). Button 생성자가 기본값을 ColorSource::Style(UseStyle)로 둔다.
    const ColorRef assigned = Element::GetForegroundColor();
    if (assigned.source != ColorSource::Style) {
        return assigned;
    }

    // UE SCheckBox::GetForegroundColor: 상태(체크·미정·보통)와 눌림·올림에 따른 스타일 색이다.
    if (state_ == CheckState::Indeterminate) {
        return checkStyle_.undeterminedForeground;
    }

    const bool checked = state_ == CheckState::Checked;
    if (IsPressed()) {
        return checked ? checkStyle_.checkedPressedForeground : checkStyle_.pressedForeground;
    }

    if (IsHovered()) {
        return checked ? checkStyle_.checkedHoveredForeground : checkStyle_.hoveredForeground;
    }

    return checked ? checkStyle_.checkedForeground : checkStyle_.foreground;
}

CheckState CheckBox::GetState() const noexcept {
    return state_;
}

Result<void> CheckBox::SetState(CheckState state) noexcept {
    if (state != CheckState::Unchecked && state != CheckState::Checked &&
        state != CheckState::Indeterminate) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (state_ != state) {
        state_ = state;
        InvalidatePaint();
    }

    return Result<void>::Success();
}

void CheckBox::SetOnChanged(Delegate<void(CheckState)>&& callback) noexcept {
    onChanged_ = Move(callback);
}

Result<void> CheckBox::OnPaint(PaintList& list, int32 layer) const noexcept {
    const auto& geometry = GetGeometry();
    const Vec2 boxSize = checkStyle_.boxSize;
    const float top = (geometry.size.y - boxSize.y) * 0.5f;
    const auto box = geometry.MakeChild(boxSize, {1, 0, 0, 1, 0, top}, {}, {});
    const StyleBrush& background = IsPressed()   ? checkStyle_.backgroundPressed
                                   : IsHovered() ? checkStyle_.backgroundHovered
                                                 : checkStyle_.background;
    const StyleColorContext context = list.ColorContext();
    if (background.mode != BrushMode::None) {
        auto painted =
            list.TryAddBrush(box, background.Resolve(list.Theme(), context), CurrentColor(), layer);
        if (!painted) {
            return painted;
        }
    }

    if (state_ == CheckState::Unchecked) {
        return Result<void>::Success();
    }

    // 표시는 상자 전체에 그리고, 색은 스타일 tint × 전경색이다(UE 표시 SImage
    // ColorAndOpacity(UseForeground()) — SCheckBox.cpp 556행).
    const bool checked = state_ == CheckState::Checked;
    const ColorRef& markColor =
        IsPressed()
            ? (checked ? checkStyle_.checkedPressedColor : checkStyle_.undeterminedPressedColor)
        : IsHovered()
            ? (checked ? checkStyle_.checkedHoveredColor : checkStyle_.undeterminedHoveredColor)
            : (checked ? checkStyle_.checkedColor : checkStyle_.undeterminedColor);
    const LinearColor ink =
        markColor.Resolve(list.Theme(), context) * context.foreground * CurrentColor();
    const float thickness = boxSize.y * MarkThickness;
    if (!checked) {
        const auto dash = box.MakeChild(
            {boxSize.x * DashWidth, thickness},
            {1, 0, 0, 1, boxSize.x * (1.0f - DashWidth) * 0.5f, (boxSize.y - thickness) * 0.5f}, {},
            {});
        return list.TryAddBox({dash, ink, layer});
    }

    const auto stroke = [&](Vec2 from, Vec2 to) noexcept -> Result<void> {
        const Vec2 start{from.x * boxSize.x, from.y * boxSize.y};
        const Vec2 delta = Vec2{to.x * boxSize.x, to.y * boxSize.y} - start;
        const float length = delta.Length();
        if (!(length > 0)) {
            return Result<void>::Success();
        }

        const Vec2 unit = delta / length;
        // 선 가운데가 from에서 to를 지나도록 두께의 절반만큼 법선 쪽으로 옮긴다.
        const Vec2 origin = start + Vec2{unit.y, -unit.x} * (thickness * 0.5f);
        const auto segment = box.MakeChild(
            {length, thickness}, {unit.x, -unit.y, unit.y, unit.x, origin.x, origin.y}, {}, {});
        return list.TryAddBox({segment, ink, layer});
    };
    auto painted = stroke(CheckPoints[0], CheckPoints[1]);
    if (!painted) {
        return painted;
    }

    return stroke(CheckPoints[1], CheckPoints[2]);
}

void CheckBox::OnActivated() noexcept {
    (void)SetState(state_ == CheckState::Checked ? CheckState::Unchecked : CheckState::Checked);
    onChanged_.ExecuteIfBound(state_);
}

AccessibleRole CheckBox::GetAccessibleRole() const noexcept {
    return AccessibleRole::CheckBox;
}

} // namespace rocks::ui
