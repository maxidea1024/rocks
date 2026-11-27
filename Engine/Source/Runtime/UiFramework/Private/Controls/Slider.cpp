#include <UiFramework/Controls/Slider.h>

namespace rocks::ui {

Slider::Slider(Allocator& allocator) noexcept : Frame(allocator) {
}

Geometry Slider::TrackGeometry() const noexcept {
    const auto& geometry = GetGeometry();
    if (!vertical_) {
        return geometry;
    }

    return geometry.MakeChild({geometry.size.y, geometry.size.x}, {0, 1, -1, 0, 0, geometry.size.y},
                              {}, {});
}

void Slider::SetVertical(bool vertical) noexcept {
    if (vertical_ != vertical) {
        vertical_ = vertical;
        InvalidateLayout();
        InvalidatePaint();
    }
}

void Slider::SetSnapToStep(bool enabled) noexcept {
    snapToStep_ = enabled;
}

float Slider::Minimum() const noexcept {
    return minimum_;
}

float Slider::Maximum() const noexcept {
    return maximum_;
}

LinearColor Slider::FillColor() const noexcept {
    return fillColor_;
}

float Slider::TrackStart(float handle, bool indent) noexcept {
    return (indent ? handle : 0.0f) * 0.5f + handle * 0.5f;
}

float Slider::TrackPosition(float value, float minimum, float maximum, float width, float handle,
                            bool indent) noexcept {
    const float travel = math::Max(0.0f, width - (indent ? handle : 0.0f) - handle);
    const float ratio = math::Clamp((value - minimum) / (maximum - minimum), 0.0f, 1.0f);
    return TrackStart(handle, indent) + ratio * travel;
}

float Slider::HandleWidth() const noexcept {
    return style_.thumbSize.x;
}

bool Slider::IsHandleIndented() const noexcept {
    return indentHandle_;
}

void Slider::SetDragging(bool dragging) noexcept {
    if (dragging_ != dragging) {
        dragging_ = dragging;
        InvalidatePaint();
    }
}

Vec2 Slider::ComputeDesiredSize(float scale) const noexcept {
    const Vec2 framed = Frame::ComputeDesiredSize(scale);
    if (framed.x > 0 || framed.y > 0) {
        return framed;
    }

    constexpr float DefaultLength = 16.0f;
    const float thickness = math::Max(style_.barThickness, style_.thumbSize.y);
    return vertical_ ? Vec2{thickness, DefaultLength} : Vec2{DefaultLength, thickness};
}

Result<void> Slider::SetRange(float minimum, float maximum) noexcept {
    if (!math::IsFinite(minimum) || !math::IsFinite(maximum) || minimum >= maximum ||
        !math::IsFinite(maximum - minimum)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (minimum_ == minimum && maximum_ == maximum) {
        return Result<void>::Success();
    }

    InvalidatePaint();
    minimum_ = minimum;
    maximum_ = maximum;
    value_ = math::Clamp(value_, minimum_, maximum_);
    return Result<void>::Success();
}

Result<void> Slider::SetValue(float value) noexcept {
    if (!math::IsFinite(value)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const float next = math::Clamp(value, minimum_, maximum_);
    if (value_ != next) {
        value_ = next;
        InvalidatePaint();
    }

    return Result<void>::Success();
}

Result<void> Slider::SetStep(float step) noexcept {
    if (!math::IsFinite(step) || step <= 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    step_ = step;
    return Result<void>::Success();
}

void Slider::SetLocked(bool locked) noexcept {
    if (locked_ != locked) {
        // 잠금은 비활성 모양으로 그린다(UE GetBarImage·GetThumbImage 485·501행).
        InvalidatePaint();
    }

    locked_ = locked;
    if (locked) {
        EndControllerCapture();
    }
}

void Slider::SetRequiresControllerLock(bool required) noexcept {
    requiresControllerLock_ = required;
    if (!required) {
        EndControllerCapture();
    }
}

void Slider::SetOnControllerCaptureBegin(Delegate<void()>&& callback) noexcept {
    controllerBegin_ = Move(callback);
}

void Slider::SetOnControllerCaptureEnd(Delegate<void()>&& callback) noexcept {
    controllerEnd_ = Move(callback);
}

void Slider::EndControllerCapture() noexcept {
    if (controllerCaptured_) {
        controllerCaptured_ = false;
        controllerEnd_.ExecuteIfBound();
    }
}

void Slider::OnFocusLost(uint32 user) noexcept {
    if (controllerCaptured_ && user == controllerUser_) {
        EndControllerCapture();
    }
}

bool Slider::HandleGamepadButton(uint32 user, input::GamepadButton button) noexcept {
    using B = input::GamepadButton;
    if (locked_ || (controllerCaptured_ && controllerUser_ != user)) {
        return false;
    }

    if (requiresControllerLock_ && button == B::South) {
        if (controllerCaptured_) {
            EndControllerCapture();
        } else {
            controllerUser_ = user;
            controllerCaptured_ = true;
            controllerBegin_.ExecuteIfBound();
        }

        return true;
    }

    if (controllerCaptured_ && button == B::East) {
        EndControllerCapture();
        return true;
    }

    return false;
}

void Slider::SetIndentHandle(bool indent) noexcept {
    if (indentHandle_ != indent) {
        indentHandle_ = indent;
        InvalidatePaint();
    }
}

void Slider::SetHandleColor(LinearColor color) noexcept {
    if (!handleTint_ || *handleTint_ != color) {
        handleTint_ = color;
        InvalidatePaint();
    }
}

void Slider::SetOnCaptureBegin(Delegate<void()>&& callback) noexcept {
    captureBegin_ = Move(callback);
}

void Slider::SetOnCaptureEnd(Delegate<void()>&& callback) noexcept {
    captureEnd_ = Move(callback);
}

float Slider::GetNormalizedValue() const noexcept {
    return (value_ - minimum_) / (maximum_ - minimum_);
}

float Slider::GetValue() const noexcept {
    return value_;
}

void Slider::SetFillColor(LinearColor color) noexcept {
    if (fillColor_ != color) {
        fillColor_ = color;
        InvalidatePaint();
    }
}

void Slider::SetOnChanged(Delegate<void(float)>&& callback) noexcept {
    onChanged_ = Move(callback);
}

bool Slider::SupportsKeyboardFocus() const noexcept {
    return true;
}

KeyReply Slider::OnKeyEvent(const KeyEvent& key) noexcept {
    const input::InputEvent& event = key.input;
    if (event.type != input::InputEventType::KeyDown) {
        return Element::OnKeyEvent(key);
    }

    // 패드 조종 잠금(South 잡기·East 놓기)이다(20.8절).
    if (key.chord.gamepad != input::GamepadButton::Count &&
        HandleGamepadButton(key.user, key.chord.gamepad)) {
        return KeyReply::Handled();
    }

    // 값은 navigation 방향으로 바꾼다(UE SSlider::OnKeyDown — GetNavigationDirectionFromKey).
    // 키보드 방향키·패드 D-pad·스틱이 같다. 잠겼거나 조종 잠금이 필요한데 잡지 않았거나 다른
    // 사용자가 잡았으면 넘긴다(navigation).
    const bool blocked = locked_ ||
                         (requiresControllerLock_ && !controllerCaptured_ &&
                          key.chord.gamepad != input::GamepadButton::Count) ||
                         (controllerCaptured_ && controllerUser_ != key.user);
    if (key.navigation && !blocked) {
        const FocusDirection decrease = vertical_ ? FocusDirection::Down : FocusDirection::Left;
        const FocusDirection increase = vertical_ ? FocusDirection::Up : FocusDirection::Right;
        if (*key.navigation == decrease || *key.navigation == increase) {
            SetInteractiveValue(*key.navigation == decrease ? value_ - step_ : value_ + step_);
            return KeyReply::Handled();
        }
    }

    return Element::OnKeyEvent(key);
}

PointerReply Slider::OnPointerEvent(const PointerEvent& event) noexcept {
    if (event.source == PointerSource::Mouse && event.button != input::MouseButton::Left &&
        (event.type == PointerEventType::Down || event.type == PointerEventType::Up)) {
        return {};
    }

    if (event.type == PointerEventType::Down && !dragging_ && !locked_) {
        SetDragging(true);
        dragSource_ = event.source;
        dragUser_ = event.userId;
        dragPointer_ = event.pointerId;
        captureBegin_.ExecuteIfBound();
        if (!locked_) {
            UpdateFromPointer(event.position, true);
        }

        return {.handled = true, .capture = dragging_};
    }

    if (!dragging_ || dragSource_ != event.source || dragUser_ != event.userId ||
        dragPointer_ != event.pointerId) {
        return {};
    }

    if (!locked_ && (event.type == PointerEventType::Move || event.type == PointerEventType::Up)) {
        UpdateFromPointer(event.position, false);
    }

    if (event.type == PointerEventType::Up || event.type == PointerEventType::Cancel) {
        SetDragging(false);
        captureEnd_.ExecuteIfBound();
        return {.handled = true, .releaseCapture = true};
    }

    return {.handled = true};
}

void Slider::OnPointerCaptureLost(PointerSource source, uint32 userId, uint32 pointerId) noexcept {
    if (dragging_ && dragSource_ == source && dragUser_ == userId && dragPointer_ == pointerId) {
        SetDragging(false);
        captureEnd_.ExecuteIfBound();
    }
}

Result<void> Slider::OnPaint(PaintList& list, int32 layer) const noexcept {
    // UE SSlider::OnPaint(81-135행): 막대는 손잡이 폭 절반씩 들여 그리고, 손잡이는 값 위치 가운데에
    // 스타일 크기로 둔다. 잠금은 비활성 모양이다. 끌기 중은 포인터를 잡고 있어 올림으로 본다(UE도
    // capture 중인 위젯은 hover 경로에 남는다).
    const auto& geometry = TrackGeometry();
    const bool disabled = !IsEnabled() || list.IsDisabled() || locked_;
    const bool hovered = hoverCount_ != 0 || dragging_;
    const StyleBrush& bar = disabled  ? style_.barDisabled
                            : hovered ? style_.barHovered
                                      : style_.bar;
    const StyleBrush& thumbBrush = disabled  ? style_.thumbDisabled
                                   : hovered ? style_.thumbHovered
                                             : style_.thumb;
    const StyleColorContext context = list.ColorContext();
    const float handle = HandleWidth();
    const float thickness = math::Min(style_.barThickness, geometry.size.y);
    const float y = (geometry.size.y - thickness) * 0.5f;
    const float start = handle * 0.5f;
    const float length = math::Max(0.0f, geometry.size.x - handle);
    const float center =
        TrackPosition(value_, minimum_, maximum_, geometry.size.x, handle, indentHandle_);
    if (bar.mode != BrushMode::None && length > 0) {
        const auto track = geometry.MakeChild({length, thickness}, {1, 0, 0, 1, start, y}, {}, {});
        auto drawn = list.TryAddBrush(track, bar.Resolve(list.Theme(), context), GetColor(), layer);
        if (!drawn) {
            return drawn;
        }
    }

    // 채움은 색을 정한 경우에만 그린다(UE 기본 슬라이더에는 없다 - 원형 슬라이더·타임라인용).
    if (fillColor_.a > 0 && center > start) {
        const auto filled =
            geometry.MakeChild({center - start, thickness}, {1, 0, 0, 1, start, y}, {}, {});
        auto drawn = list.TryAddBox({filled, fillColor_, layer});
        if (!drawn) {
            return drawn;
        }
    }

    if (thumbBrush.mode == BrushMode::None) {
        return Result<void>::Success();
    }

    const Vec2 size = style_.thumbSize;
    const auto thumb = geometry.MakeChild(
        size, {1, 0, 0, 1, center - size.x * 0.5f, (geometry.size.y - size.y) * 0.5f}, {}, {});
    return list.TryAddBrush(thumb, thumbBrush.Resolve(list.Theme(), context),
                            handleTint_ ? *handleTint_ : LinearColor{1, 1, 1, 1}, layer);
}

Result<void> Slider::SetStyle(const SliderStyle& style) noexcept {
    const ThemeColors& theme = DefaultThemeColors();
    const StyleBrush* brushes[] = {&style.bar,   &style.barHovered,   &style.barDisabled,
                                   &style.thumb, &style.thumbHovered, &style.thumbDisabled};
    for (const StyleBrush* brush : brushes) {
        if (!brush->Resolve(theme, {}).IsValid()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    if (!math::IsFinite(style.barThickness) || style.barThickness < 0 ||
        !style.thumbSize.IsFinite() || style.thumbSize.x < 0 || style.thumbSize.y < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (style_ != style) {
        style_ = style;
        InvalidatePaint();
    }

    return Result<void>::Success();
}

const SliderStyle& Slider::GetStyle() const noexcept {
    return style_;
}

void Slider::OnMouseEnter(uint32) noexcept {
    if (++hoverCount_ == 1) {
        InvalidatePaint();
    }
}

void Slider::OnMouseLeave(uint32) noexcept {
    if (hoverCount_ && --hoverCount_ == 0) {
        InvalidatePaint();
    }
}

void Slider::UpdateFromPointer(Vec2 position, bool) noexcept {
    const Geometry geometry = TrackGeometry();
    AffineTransform2D inverse;
    if (geometry.size.x <= 0 || !geometry.render.TryInverse(inverse)) {
        return;
    }

    const Vec2 local = inverse.Apply(position);
    if (!local.IsFinite()) {
        return;
    }

    // UE PositionToValue(439-445행): (x - 시작) / (폭 - 들여쓰기 - 손잡이 폭).
    const float handle = HandleWidth();
    const float travel = geometry.size.x - (indentHandle_ ? handle : 0.0f) - handle;
    const float ratio =
        travel > 0 ? math::Clamp((local.x - TrackStart(handle, indentHandle_)) / travel, 0.0f, 1.0f)
                   : 0.0f;
    SetInteractiveRatio(ratio);
}

void Slider::SetInteractiveRatio(float ratio) noexcept {
    if (ratio <= 0 || ratio >= 1) {
        SetInteractiveValue(ratio <= 0 ? minimum_ : maximum_);
        return;
    }

    const double range = static_cast<double>(maximum_) - minimum_;
    const double offset = ratio * range;
    const double amount = snapToStep_ ? math::Floor(offset / step_ + 0.5) * step_ : offset;
    SetInteractiveValue(static_cast<float>(math::Clamp(
        minimum_ + amount, static_cast<double>(minimum_), static_cast<double>(maximum_))));
}

void Slider::SetInteractiveValue(float value) noexcept {
    const float clamped = math::Clamp(value, minimum_, maximum_);
    if (clamped != value_) {
        value_ = clamped;
        InvalidatePaint();
        onChanged_.ExecuteIfBound(value_);
    }
}

AccessibleRole Slider::GetAccessibleRole() const noexcept {
    return AccessibleRole::Slider;
}

} // namespace rocks::ui
