#include <UiFramework/Slider.h>

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

float Slider::ThumbExtent(Vec2 size) noexcept {
    return math::Min(12.0f, math::Min(size.x, size.y));
}

float Slider::TrackPosition(float value, float minimum, float maximum, Vec2 size) noexcept {
    const float thumb = ThumbExtent(size);
    return thumb * 0.5f + (size.x - thumb) * ((value - minimum) / (maximum - minimum));
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
bool Slider::OnGamepadButtonDown(uint32 user, input::GamepadButton button) noexcept {
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
    if (requiresControllerLock_ && !controllerCaptured_) {
        return false;
    }
    if (button == (vertical_ ? B::DPadDown : B::DPadLeft)) {
        SetInteractiveValue(value_ - step_);
        return true;
    }
    if (button == (vertical_ ? B::DPadUp : B::DPadRight)) {
        SetInteractiveValue(value_ + step_);
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

bool Slider::OnKeyEvent(uint32, const input::InputEvent& event) noexcept {
    if (locked_ || event.type != input::InputEventType::KeyDown) {
        return false;
    }
    if (event.key == (vertical_ ? input::Key::Down : input::Key::Left)) {
        SetInteractiveValue(value_ - step_);
        return true;
    }
    if (event.key == (vertical_ ? input::Key::Up : input::Key::Right)) {
        SetInteractiveValue(value_ + step_);
        return true;
    }
    return false;
}

PointerReply Slider::OnPointerEvent(const PointerEvent& event) noexcept {
    if (event.source == PointerSource::Mouse && event.button != input::MouseButton::Left &&
        (event.type == PointerEventType::Down || event.type == PointerEventType::Up)) {
        return {};
    }
    if (event.type == PointerEventType::Down && !dragging_ && !locked_) {
        dragging_ = true;
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
        dragging_ = false;
        captureEnd_.ExecuteIfBound();
        return {.handled = true, .releaseCapture = true};
    }
    return {.handled = true};
}

void Slider::OnPointerCaptureLost(PointerSource source, uint32 userId, uint32 pointerId) noexcept {
    if (dragging_ && dragSource_ == source && dragUser_ == userId && dragPointer_ == pointerId) {
        dragging_ = false;
        captureEnd_.ExecuteIfBound();
    }
}

Result<void> Slider::OnPaint(PaintList& list, int32 layer) const noexcept {
    const auto& geometry = TrackGeometry();
    const float height = math::Min(4.0f, geometry.size.y);
    const float thumb = ThumbExtent(geometry.size);
    const float y = (geometry.size.y - height) * 0.5f;
    const float center = indentHandle_ ? TrackPosition(value_, minimum_, maximum_, geometry.size)
                                       : geometry.size.x * GetNormalizedValue();
    const float x = center - thumb * 0.5f;
    const auto track = geometry.MakeChild({geometry.size.x, height}, {1, 0, 0, 1, 0, y}, {}, {});
    auto drawn = list.TryAddBrush(track, GetBrush(), GetColor(), layer);
    if (!drawn) {
        return drawn;
    }

    const auto filled = geometry.MakeChild({x + thumb * 0.5f, height}, {1, 0, 0, 1, 0, y}, {}, {});
    drawn = list.TryAddBox({filled, fillColor_, layer});
    if (!drawn) {
        return drawn;
    }

    const auto handle = geometry.MakeChild(
        {thumb, thumb}, {1, 0, 0, 1, x, (geometry.size.y - thumb) * 0.5f}, {}, {});
    return list.TryAddBox({handle, handleTint_ ? *handleTint_ : fillColor_, layer});
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
    const float thumb = ThumbExtent(geometry.size);
    const float padding = indentHandle_ ? thumb : 0.0f;
    const float travel = geometry.size.x - padding;
    const float ratio =
        travel > 0 ? math::Clamp((local.x - padding * 0.5f) / travel, 0.0f, 1.0f) : 0.0f;
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
