#include <UiFramework/CheckBox.h>

namespace rocks::ui {

CheckBox::CheckBox(Allocator& allocator) noexcept : Button(allocator) {
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
    const float side = math::Min(geometry.size.x, geometry.size.y);
    const float size = ChildCount() ? math::Min(20.0f, side) : side;
    const float top = (geometry.size.y - size) * 0.5f;
    const auto box = geometry.MakeChild({size, size}, {1, 0, 0, 1, 0, top}, {}, {});
    auto painted = list.TryAddBrush(box, GetBrush(), CurrentColor(), layer);
    if (!painted || state_ == CheckState::Unchecked || size <= 0) {
        return painted;
    }

    // 표시만 정사각형 안에 그리고 레이블까지 같은 입력 영역으로 사용한다.
    const LinearColor ink = LinearColor::Black();
    if (state_ == CheckState::Indeterminate) {
        const auto dash = box.MakeChild({size * 0.6f, size * 0.12f},
                                        {1, 0, 0, 1, size * 0.2f, size * 0.44f}, {}, {});
        return list.TryAddBox({dash, ink, layer});
    }
    const auto stroke = [&](Vec2 from, Vec2 to) noexcept {
        const auto delta = to - from;
        const float length = delta.Length();
        const Vec2 unit = delta / length;
        const auto segment =
            box.MakeChild({length * size, size * 0.12f},
                          {unit.x, -unit.y, unit.y, unit.x, from.x * size, from.y * size}, {}, {});
        return list.TryAddBox({segment, ink, layer});
    };
    painted = stroke({0.23f, 0.46f}, {0.46f, 0.69f});
    if (!painted) {
        return painted;
    }
    return stroke({0.39f, 0.69f}, {0.80f, 0.26f});
}

void CheckBox::OnActivated() noexcept {
    (void)SetState(state_ == CheckState::Checked ? CheckState::Unchecked : CheckState::Checked);
    onChanged_.ExecuteIfBound(state_);
}

AccessibleRole CheckBox::GetAccessibleRole() const noexcept {
    return AccessibleRole::CheckBox;
}

} // namespace rocks::ui
