#include <UiFramework/SpinBox.h>
#include <Core/Text/NumberText.h>
#include <Core/Text/StringBuilder.h>

namespace rocks::ui {

Result<void> SpinBox::Construct(Args&& args) noexcept {
    auto result = Frame::Construct(
        {.content = args.content, .color = Move(args.color), .desiredSize = Vec2{72, 24}});
    if (result) {
        result = SetLimits(args.minimum, args.maximum, args.step);
    }

    if (result) {
        result = SetValue(args.value);
    }

    if (result) {
        result = SetCaption(Move(args.caption));
    }

    if (result) {
        SetOnChanged(Move(args.onChanged));
    }

    return result;
}

SpinBox::SpinBox(Allocator& allocator) noexcept : Frame(allocator), allocator_(allocator) {
    SetDesiredSize({72, 24});
    SetColor({0.12f, 0.12f, 0.12f, 1});
}

Result<void> SpinBox::ShowNumber(float value) noexcept {
    if (!caption_) {
        return Result<void>::Success();
    }

    StringBuilder<> label(allocator_);
    char spec[] = "{:.0f}";
    if (maxDigits_ >= 0) {
        spec[3] = static_cast<char>('0' + maxDigits_);
    }

    auto written = label.TryAppendFormat(maxDigits_ >= 0 ? spec : "{}", value);
    if (!written) {
        return written;
    }

    const auto text = label.View();
    usize length = text.ByteCount();
    if (maxDigits_ > 0) {
        const usize decimal = length - static_cast<usize>(maxDigits_) - 1;
        while (length > decimal + 1 + static_cast<usize>(minDigits_) &&
               text.Data()[length - 1] == '0') {
            --length;
        }

        if (length == decimal + 1) {
            --length;
        }
    }

    return caption_.Execute({text.Data(), length});
}

Result<void> SpinBox::SetCaption(Delegate<Result<void>(StringView)>&& callback) noexcept {
    caption_ = Move(callback);
    return editing_ && caption_ ? caption_.Execute({buffer_, length_}) : ShowNumber(value_);
}

Result<void> SpinBox::SetLimits(float minimum, float maximum, float step) noexcept {
    if (!math::IsFinite(minimum) || !math::IsFinite(maximum) || !math::IsFinite(step) ||
        minimum > maximum || step <= 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const float value = math::Clamp(value_, minimum, maximum);
    auto shown = ShowNumber(value);
    if (shown) {
        minimum_ = minimum;
        maximum_ = maximum;
        step_ = step;
        value_ = value;
        editing_ = false;
    }

    return shown;
}

Result<void> SpinBox::SetValue(float value) noexcept {
    if (!math::IsFinite(value)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    value = math::Clamp(value, minimum_, maximum_);
    if (value == value_ && !editing_) {
        return Result<void>::Success();
    }

    auto shown = ShowNumber(value);
    if (shown) {
        value_ = value;
        editing_ = false;
    }

    return shown;
}

bool SpinBox::Change(float value) noexcept {
    if (snapToStep_) {
        const double snapped =
            math::GridSnap(static_cast<double>(value), static_cast<double>(step_));
        value = static_cast<float>(
            math::Clamp(snapped, static_cast<double>(minimum_), static_cast<double>(maximum_)));
    }

    const float old = value_;
    if (!SetValue(value)) {
        return false;
    }

    if (old != value_) {
        changed_.ExecuteIfBound(value_);
    }

    return true;
}

float SpinBox::Value() const noexcept {
    return value_;
}

Result<void> SpinBox::SetFractionalDigits(int32 minimum, int32 maximum) noexcept {
    if (minimum < 0 || maximum < minimum || maximum > 9) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const int32 oldMin = minDigits_, oldMax = maxDigits_;
    minDigits_ = minimum;
    maxDigits_ = maximum;
    if (!editing_) {
        auto shown = ShowNumber(value_);
        if (!shown) {
            minDigits_ = oldMin;
            maxDigits_ = oldMax;
            return shown;
        }
    }

    return Result<void>::Success();
}

void SpinBox::SetSliderEnabled(bool enabled) noexcept {
    sliderEnabled_ = enabled;
}

void SpinBox::SetSnapToStep(bool enabled) noexcept {
    snapToStep_ = enabled;
}

void SpinBox::SetOnCommitted(Delegate<void(float, SpinCommitReason)>&& callback) noexcept {
    committed_ = Move(callback);
}

void SpinBox::SetOnDragBegin(Delegate<void()>&& callback) noexcept {
    beginDrag_ = Move(callback);
}

void SpinBox::SetOnDragEnd(Delegate<void(float)>&& callback) noexcept {
    endDrag_ = Move(callback);
}

bool SpinBox::IsEditing() const noexcept {
    return editing_;
}

void SpinBox::SetOnChanged(Delegate<void(float)>&& callback) noexcept {
    changed_ = Move(callback);
}

bool SpinBox::SupportsKeyboardFocus() const noexcept {
    return true;
}

void SpinBox::BeginEdit() noexcept {
    if (!caption_ || caption_.Execute({})) {
        editing_ = true;
        length_ = 0;
    }
}

void SpinBox::CancelEdit() noexcept {
    if (editing_) {
        (void)ShowNumber(value_);
        editing_ = false;
    }
}

void SpinBox::Commit(SpinCommitReason reason) noexcept {
    if (!editing_) {
        return;
    }

    auto parsed = TryParseFloat({buffer_, length_});
    if (parsed && math::IsFinite(parsed.Value()) && Change(parsed.Value())) {
        committed_.ExecuteIfBound(value_, reason);
    } else {
        CancelEdit();
    }
}

void SpinBox::OnFocusLost(uint32) noexcept {
    Commit(SpinCommitReason::FocusLost);
}

bool SpinBox::OnKeyEvent(uint32, const input::InputEvent& event) noexcept {
    using input::Key;
    if (event.type == input::InputEventType::Text) {
        const auto c = event.codePoint;
        if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'E' || c == 'e') {
            if (!editing_) {
                BeginEdit();
            }

            if (editing_ && length_ < sizeof(buffer_)) {
                buffer_[length_] = static_cast<char>(c);
                if (!caption_ || caption_.Execute({buffer_, length_ + 1})) {
                    ++length_;
                }
            }

            return true;
        }

        return editing_;
    }

    if (event.type != input::InputEventType::KeyDown) {
        return false;
    }

    switch (event.key) {
    case Key::Enter:
    case Key::NumpadEnter:
        if (event.repeat) {
            return true;
        }

        if (editing_) {
            Commit(SpinCommitReason::Enter);
        } else {
            BeginEdit();
        }

        return true;
    case Key::Escape:
        if (editing_) {
            CancelEdit();
            return true;
        }

        return false;
    case Key::Backspace:
        if (editing_) {
            if (length_ && (!caption_ || caption_.Execute({buffer_, length_ - 1}))) {
                --length_;
            }

            return true;
        }

        return false;
    case Key::Up:
    case Key::Down:
        Change(static_cast<float>(
            math::Clamp(static_cast<double>(value_) + (event.key == Key::Up ? step_ : -step_),
                        static_cast<double>(minimum_), static_cast<double>(maximum_))));
        return true;
    default:
        return false;
    }
}

PointerReply SpinBox::OnPointerEvent(const PointerEvent& event) noexcept {
    if (tracking_ &&
        (source_ != event.source || user_ != event.userId || pointer_ != event.pointerId)) {
        return {};
    }

    if ((event.type == PointerEventType::Down || event.type == PointerEventType::Up) &&
        event.source == PointerSource::Mouse && event.button != input::MouseButton::Left) {
        return {};
    }

    if (tracking_ &&
        (event.type == PointerEventType::Up || event.type == PointerEventType::Cancel)) {
        tracking_ = false;
        if (dragged_) {
            dragged_ = false;
            endDrag_.ExecuteIfBound(value_);
        } else if (event.type == PointerEventType::Up && GetGeometry().Contains(event.position)) {
            BeginEdit();
        }

        return {.handled = true, .releaseCapture = true};
    }

    AffineTransform2D inverse;
    if (!GetGeometry().render.TryInverse(inverse)) {
        return {};
    }

    const float x = inverse.Apply(event.position).x;
    if (!math::IsFinite(x)) {
        return {};
    }

    if (!tracking_ && event.type == PointerEventType::Down) {
        source_ = event.source;
        user_ = event.userId;
        pointer_ = event.pointerId;
        dragX_ = x;
        dragValue_ = value_;
        tracking_ = true;
        dragged_ = false;
        return {.handled = true, .capture = true};
    }

    if (tracking_ && sliderEnabled_ && event.type == PointerEventType::Move) {
        const double steps = math::Trunc((static_cast<double>(x) - dragX_) / 4);
        if (steps != 0 || dragged_) {
            if (!dragged_) {
                dragged_ = true;
                beginDrag_.ExecuteIfBound();
            }

            Change(static_cast<float>(math::Clamp(dragValue_ + steps * step_,
                                                  static_cast<double>(minimum_),
                                                  static_cast<double>(maximum_))));
        }

        return {.handled = true};
    }

    return {};
}

void SpinBox::OnPointerCaptureLost(PointerSource source, uint32 user, uint32 pointer) noexcept {
    if (tracking_ && source == source_ && user == user_ && pointer == pointer_) {
        tracking_ = false;
        if (dragged_) {
            dragged_ = false;
            endDrag_.ExecuteIfBound(value_);
        }
    }
}

AccessibleRole SpinBox::GetAccessibleRole() const noexcept {
    return AccessibleRole::SpinBox;
}

} // namespace rocks::ui
