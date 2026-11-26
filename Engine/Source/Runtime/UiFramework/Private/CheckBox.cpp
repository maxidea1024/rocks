#include <UiFramework/CheckBox.h>

namespace rocks::ui {

CheckBox::CheckBox(Allocator& allocator) noexcept : Button(allocator) {
    // 상자는 CheckBoxStyle로 그리므로 버튼 brush·여백은 쓰지 않는다.
    (void)SetStyle(ButtonStyle{});
    UpdateContentPadding();
    // 상자 오른쪽 내용은 세로 가운데다(UE SCheckBox 내용 슬롯 VAlign_Center, 558행).
    SetContentAlignment(StackAlignment::Fill, StackAlignment::Center);
}

Result<void> CheckBox::SetCheckBoxStyle(const CheckBoxStyle& style) noexcept {
    const auto validSize = [](Vec2 size) noexcept {
        return size.IsFinite() && size.x > 0 && size.y > 0;
    };
    const auto validPadding = [](Vec4 padding) noexcept {
        return padding.IsFinite() && padding.x >= 0 && padding.y >= 0 && padding.z >= 0 &&
               padding.w >= 0;
    };
    const ColorRef colors[] = {style.foreground,
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

    const StyleBrush* brushes[] = {
        &style.background,        &style.backgroundHovered,        &style.backgroundPressed,
        &style.checkedImage,      &style.checkedHoveredImage,      &style.checkedPressedImage,
        &style.undeterminedImage, &style.undeterminedHoveredImage, &style.undeterminedPressedImage};
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

    // 표시는 상태별 이미지(UE CheckedImage 등)를 상자 전체에 그리고, 색은 이미지 tint x 전경색이다
    // (UE 표시 SImage ColorAndOpacity(UseForeground()) — SCheckBox.cpp 556행).
    const bool checked = state_ == CheckState::Checked;
    const StyleBrush& mark =
        IsPressed()
            ? (checked ? checkStyle_.checkedPressedImage : checkStyle_.undeterminedPressedImage)
        : IsHovered()
            ? (checked ? checkStyle_.checkedHoveredImage : checkStyle_.undeterminedHoveredImage)
            : (checked ? checkStyle_.checkedImage : checkStyle_.undeterminedImage);
    if (mark.mode == BrushMode::None) {
        return Result<void>::Success();
    }

    return list.TryAddBrush(box, mark.Resolve(list.Theme(), context),
                            context.foreground * CurrentColor(), layer);
}

void CheckBox::OnActivated() noexcept {
    (void)SetState(state_ == CheckState::Checked ? CheckState::Unchecked : CheckState::Checked);
    onChanged_.ExecuteIfBound(state_);
}

AccessibleRole CheckBox::GetAccessibleRole() const noexcept {
    return AccessibleRole::CheckBox;
}

} // namespace rocks::ui
