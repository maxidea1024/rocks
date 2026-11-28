#include <Core/Math/MathUtility.h>
#include <Core/Text/NumberText.h>
#include <UiCore/InputMetrics.h>
#include <UiCore/PaintList.h>
#include <UiCore/UiHostServices.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Controls/EditableText.h>
#include <UiFramework/Controls/SpinBox.h>

namespace rocks::ui {

namespace {

// 기본 단계와 작은 단계, 작은 단계를 쓰는 범위 상한이다(UE SSpinBox StepSize 1·SmallStepSize 0.1·
// SmallStepSizeMax 10).
constexpr double SpinStepSize = 1.0;
constexpr double SpinSmallStepSize = 0.1;
constexpr double SpinSmallStepSizeMax = 10.0;
// 끌기 계산의 최소 칸 폭이다(UE OnMouseMove MinSliderWidth 100).
constexpr float SpinMinSliderWidth = 100.0f;

// value가 [minimum, maximum]에서 차지하는 비율이다(UE Fraction — 범위가 0이면 0).
float Fraction(double value, double minimum, double maximum) noexcept {
    const double span = maximum - minimum;
    if (!(span > 0) || !math::IsFinite(span)) {
        return 0.0f;
    }

    return static_cast<float>(math::Clamp((value - minimum) / span, 0.0, 1.0));
}

// 중립값 양쪽으로 따로 지수 곡선을 쓴다(UE SpinBoxComputeExponentSliderFraction).
float ExponentFraction(float fraction, float start, float exponent) noexcept {
    if (fraction > start) {
        const float range = 1.0f - start;
        if (range <= 0) {
            return fraction;
        }

        const float local = (fraction - start) / range;
        return start + platform::PlatformMath::Pow(local, exponent) * range;
    }

    if (start <= 0) {
        return fraction;
    }

    const float local = (start - fraction) / start;
    return start - platform::PlatformMath::Pow(local, exponent) * start;
}

double GridSnap(double value, double grid) noexcept {
    return grid > 0 ? math::Floor((value + grid * 0.5) / grid) * grid : value;
}

} // namespace

SpinBox::SpinBox(Allocator& allocator, SharedPtr<TextShaper> shaper, Platform platform) noexcept
    : Element(allocator), allocator_(allocator), shaper_(Move(shaper)), platform_(platform) {
    (void)SetForegroundColor(style_.foreground);
}

SpinBox::~SpinBox() noexcept = default;

Result<void> SpinBox::Initialize(float fontSize) noexcept {
    if (editor_) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    auto made = SharedPtr<EditableText>::TryMake(allocator_, allocator_, shaper_, platform_, false);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    SharedPtr<EditableText> editor = Move(made.Value());
    auto result = editor->Initialize({}, fontSize);
    if (result) {
        result = TryAddChild(editor);
    }

    if (!result) {
        return result;
    }

    editor_ = Move(editor);
    // 표시 모드: 읽기 전용이고 누름·Tab은 스핀 상자가 받는다(24.5절).
    editor_->SetReadOnly(true);
    editor_->SetVisibility(Visibility::HitTestInvisible);
    editor_->SetSelectAllTextWhenFocused(true);
    editor_->SetSelectAllTextOnCommit(true);
    editor_->SetRevertTextOnEscape(true);
    editor_->SetOnTextChanged(Delegate<void(StringView)>::CreateLambda(
        [this](StringView text) noexcept { HandleTextChanged(text); }));
    editor_->SetOnTextCommitted(Delegate<void(StringView, TextCommitReason)>::CreateLambda(
        [this](StringView text, TextCommitReason reason) noexcept {
            HandleTextCommitted(text, reason);
        }));
    return RefreshText();
}

Result<void> SpinBox::SetValue(double value) noexcept {
    if (!math::IsFinite(value)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const double next = math::Clamp(RoundToNumericKind(value, kind_), MinValue(), MaxValue());
    value_ = next;
    internal_ = next;
    InvalidatePaint();
    return editing_ ? Result<void>::Success() : RefreshText();
}

double SpinBox::Value() const noexcept {
    return value_;
}

void SpinBox::SetNumericKind(NumericKind kind) noexcept {
    kind_ = kind;
    (void)SetValue(value_);
}

NumericKind SpinBox::GetNumericKind() const noexcept {
    return kind_;
}

Result<void> SpinBox::SetValueRange(Optional<double> minimum, Optional<double> maximum) noexcept {
    if ((minimum && !math::IsFinite(minimum.Value())) ||
        (maximum && !math::IsFinite(maximum.Value())) ||
        (minimum && maximum && minimum.Value() > maximum.Value())) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    minValue_ = minimum;
    maxValue_ = maximum;
    return SetValue(value_);
}

Result<void> SpinBox::SetSliderRange(Optional<double> minimum, Optional<double> maximum) noexcept {
    if ((minimum && !math::IsFinite(minimum.Value())) ||
        (maximum && !math::IsFinite(maximum.Value())) ||
        (minimum && maximum && minimum.Value() > maximum.Value())) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    minSlider_ = minimum;
    maxSlider_ = maximum;
    InvalidatePaint();
    return Result<void>::Success();
}

Result<void> SpinBox::SetDelta(double delta) noexcept {
    if (!math::IsFinite(delta) || delta < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    delta_ = delta;
    InvalidatePaint();
    return Result<void>::Success();
}

void SpinBox::SetAlwaysUsesDeltaSnap(bool enabled) noexcept {
    alwaysSnap_ = enabled;
}

Result<void> SpinBox::SetFractionalDigits(int32 minimum, int32 maximum) noexcept {
    if (minimum < 0 || maximum < 0 || minimum > MaxNumericFractionalDigits ||
        maximum > MaxNumericFractionalDigits) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    minDigits_ = minimum;
    maxDigits_ = maximum;
    return editing_ ? Result<void>::Success() : RefreshText();
}

void SpinBox::SetSliderEnabled(bool enabled) noexcept {
    sliderEnabled_ = enabled;
}

Result<void> SpinBox::SetMultipliers(float shift, float ctrl, float shiftAlt,
                                     float ctrlAlt) noexcept {
    const float values[] = {shift, ctrl, shiftAlt, ctrlAlt};
    for (const float value : values) {
        if (!math::IsFinite(value) || value <= 0) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    shiftMultiplier_ = shift;
    ctrlMultiplier_ = ctrl;
    shiftAltMultiplier_ = shiftAlt;
    ctrlAltMultiplier_ = ctrlAlt;
    return Result<void>::Success();
}

void SpinBox::SetLinearDeltaSensitivity(int32 pixels) noexcept {
    linearSensitivity_ = pixels > 0 ? pixels : 0;
}

Result<void> SpinBox::SetSliderExponent(float exponent, Optional<double> neutral) noexcept {
    if (!math::IsFinite(exponent) || exponent <= 0 ||
        (neutral && !math::IsFinite(neutral.Value()))) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    exponent_ = exponent;
    neutral_ = neutral;
    InvalidatePaint();
    return Result<void>::Success();
}

void SpinBox::SetWheelEnabled(bool enabled) noexcept {
    wheelEnabled_ = enabled;
}

void SpinBox::SetWheelStep(Optional<double> step) noexcept {
    wheelStep_ = step && math::IsFinite(step.Value()) ? step : Optional<double>{};
}

void SpinBox::SetBroadcastValueChangesPerKey(bool enabled) noexcept {
    perKey_ = enabled;
}

Result<void> SpinBox::SetMinDesiredWidth(float width) noexcept {
    if (!math::IsFinite(width) || width < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    minDesiredWidth_ = width;
    InvalidateLayout();
    return Result<void>::Success();
}

void SpinBox::SetUnits(const NumericUnitOptions& units) noexcept {
    units_ = units;
    if (!editing_) {
        (void)RefreshText();
    }
}

void SpinBox::SetClearKeyboardFocusOnCommit(bool enabled) noexcept {
    clearFocusOnCommit_ = enabled;
}

void SpinBox::SetSelectAllTextOnCommit(bool enabled) noexcept {
    if (editor_) {
        editor_->SetSelectAllTextOnCommit(enabled);
    }
}

void SpinBox::SetRevertTextOnEscape(bool enabled) noexcept {
    if (editor_) {
        editor_->SetRevertTextOnEscape(enabled);
    }
}

void SpinBox::SetDisplayTextOverride(TextOverride&& callback) noexcept {
    displayText_ = Move(callback);
    if (!editing_) {
        (void)RefreshText();
    }
}

void SpinBox::SetEditTextOverride(TextOverride&& callback) noexcept {
    editText_ = Move(callback);
}

void SpinBox::SetSpinBoxStyle(const SpinBoxStyle& style) noexcept {
    styleExplicit_ = true;
    ApplyStyle(style);
}

const SpinBoxStyle& SpinBox::GetSpinBoxStyle() const noexcept {
    return style_;
}

void SpinBox::ApplyStyle(const SpinBoxStyle& style) noexcept {
    style_ = style;
    (void)SetForegroundColor(style_.foreground);
    InvalidateLayout();
}

void SpinBox::ApplyStyles(const UiHostServices& host) noexcept {
    if (styleExplicit_) {
        return;
    }

    const SpinBoxStyle* style = host.FindStyle<SpinBoxStyle>("SpinBox");
    const SpinBoxStyle& next = style ? *style : DefaultSpinBoxStyle();
    if (!(next == style_)) {
        ApplyStyle(next);
    }
}

void SpinBox::SetOnChanged(Delegate<void(double)>&& callback) noexcept {
    changed_ = Move(callback);
}

void SpinBox::SetOnCommitted(Delegate<void(double, SpinCommitReason)>&& callback) noexcept {
    committed_ = Move(callback);
}

void SpinBox::SetOnDragBegin(Delegate<void()>&& callback) noexcept {
    beginDrag_ = Move(callback);
}

void SpinBox::SetOnDragEnd(Delegate<void(double)>&& callback) noexcept {
    endDrag_ = Move(callback);
}

bool SpinBox::IsEditing() const noexcept {
    return editing_;
}

bool SpinBox::IsDragging() const noexcept {
    return dragging_;
}

EditableText* SpinBox::Editor() const noexcept {
    return editor_.Get();
}

double SpinBox::MinValue() const noexcept {
    return minValue_ ? minValue_.Value() : NumericKindLowest(kind_);
}

double SpinBox::MaxValue() const noexcept {
    return maxValue_ ? maxValue_.Value() : NumericKindMax(kind_);
}

// 슬라이더 범위가 없으면 값 범위를 따른다(UE SetMinSliderValue — MinValue로 둔다).
double SpinBox::MinSliderValue() const noexcept {
    return minSlider_ ? minSlider_.Value() : MinValue();
}

double SpinBox::MaxSliderValue() const noexcept {
    return maxSlider_ ? maxSlider_.Value() : MaxValue();
}

bool SpinBox::IsSpinRangeUnlimited() const noexcept {
    const bool values = minValue_ && maxValue_;
    const bool sliders = (minSlider_ || minValue_) && (maxSlider_ || maxValue_);
    return !(values || sliders);
}

float SpinBox::Multiplier(bool shift, bool control, bool alt) const noexcept {
    if (shift) {
        return alt ? shiftAltMultiplier_ : shiftMultiplier_;
    }

    if (control) {
        return alt ? ctrlAltMultiplier_ : ctrlMultiplier_;
    }

    return 1.0f;
}

double SpinBox::DefaultStep(float multiplier) const noexcept {
    const bool small = MaxSliderValue() - MinSliderValue() <= SpinSmallStepSizeMax;
    return (small ? SpinSmallStepSize : SpinStepSize) * multiplier;
}

NumericFormatOptions SpinBox::FormatOptions() const noexcept {
    return {.kind = kind_, .minFractionalDigits = minDigits_, .maxFractionalDigits = maxDigits_};
}

Optional<float> SpinBox::FillFraction() const noexcept {
    if (IsSpinRangeUnlimited()) {
        return {};
    }

    const double minimum = MinSliderValue();
    const double maximum = MaxSliderValue();
    const double shown = delta_ > 0 ? GridSnap(value_, delta_) : value_;
    float fraction = Fraction(shown, minimum, maximum);
    if (exponent_ != 1.0f) {
        if (neutral_ && neutral_.Value() > minimum && neutral_.Value() < maximum) {
            fraction =
                ExponentFraction(fraction, Fraction(neutral_.Value(), minimum, maximum), exponent_);
        } else {
            fraction = 1.0f - platform::PlatformMath::Pow(1.0f - fraction, exponent_);
        }
    }

    return fraction;
}

void SpinBox::CommitValue(double value, double spinValue, CommitMethod method,
                          Optional<SpinCommitReason> committed) noexcept {
    // Spin·ArrowKey는 슬라이더 범위로, 모두 값 범위로 자른다(UE CommitValue 993–1010행).
    if (method == CommitMethod::Spin || method == CommitMethod::ArrowKey) {
        value = math::Clamp(value, MinSliderValue(), MaxSliderValue());
        spinValue = math::Clamp(spinValue, MinSliderValue(), MaxSliderValue());
    }

    value = math::Clamp(value, MinValue(), MaxValue());
    spinValue = math::Clamp(spinValue, MinValue(), MaxValue());
    internal_ = spinValue;
    // Spin·ArrowKey(·AlwaysUsesDeltaSnap)는 Delta에 맞춘다 — 끄는 값 자체는 맞추지 않는다.
    if ((method == CommitMethod::Spin || method == CommitMethod::ArrowKey || alwaysSnap_) &&
        delta_ > 0) {
        // 맞춘 값이 값 범위를 넘지 않게 다시 자른다(UE는 자른 뒤 맞춰 최대를 넘을 수 있다 —
        // 슬라이더 범위는 느슨한 범위라 넘어도 된다).
        value = math::Clamp(GridSnap(value, delta_), MinValue(), MaxValue());
    }

    value_ = RoundToNumericKind(value, kind_);
    InvalidatePaint();
    if (committed) {
        committed_.ExecuteIfBound(value_, committed.Value());
    }

    changed_.ExecuteIfBound(value_);
    if (!editing_) {
        (void)RefreshText();
    }
}

Result<void> SpinBox::RefreshText() noexcept {
    if (!editor_) {
        return Result<void>::Success();
    }

    String text(allocator_);
    const TextOverride& override = editing_ ? editText_ : displayText_;
    if (!override.IsBound() || !override.Execute(value_, text)) {
        auto formatted = FormatNumberWithUnit(value_, FormatOptions(), units_, text);
        if (!formatted) {
            return formatted;
        }
    }

    filtering_ = true;
    auto set = editor_->SetText(text.View());
    filtering_ = false;
    return set;
}

void SpinBox::EnterTextMode(uint32 user, FocusCause cause) noexcept {
    if (!editor_) {
        return;
    }

    editing_ = true;
    editor_->SetVisibility(Visibility::Visible);
    editor_->SetReadOnly(false);
    (void)RefreshText();
    InvalidatePaint();
    if (Application* app = Application::Find(*this)) {
        (void)app->SetFocus(user, editor_, cause);
    }
}

void SpinBox::ExitTextMode() noexcept {
    if (!editing_) {
        return;
    }

    editing_ = false;
    if (editor_) {
        editor_->SetReadOnly(true);
        editor_->SetVisibility(Visibility::HitTestInvisible);
    }

    (void)RefreshText();
    InvalidatePaint();
}

void SpinBox::HandleTextChanged(StringView text) noexcept {
    if (filtering_ || !editor_) {
        return;
    }

    // 칠 수 없는 글자가 들어오면 그 앞까지만 남긴다(UE TextField_OnTextChanged). 앞이 비면 지금
    // 값이다.
    usize valid = 0;
    while (valid < text.ByteCount()) {
        const unsigned char c = static_cast<unsigned char>(text.Data()[valid]);
        if (c < 0x80 && !IsNumericUnitCharacter(static_cast<char32_t>(c), units_)) {
            break;
        }

        if (c >= 0x80 && units_.underlying == NumericUnit::Unspecified) {
            break;
        }

        ++valid;
    }

    if (valid < text.ByteCount()) {
        if (valid == 0) {
            (void)RefreshText();
        } else {
            String prefix(allocator_);
            if (prefix.TryAssign({text.Data(), valid})) {
                filtering_ = true;
                (void)editor_->SetText(prefix.View());
                filtering_ = false;
            }
        }

        return;
    }

    // 키마다 숫자면 값을 알린다(UE BroadcastValueChangesPerKey — 수식 중간은 건너뛴다).
    if (perKey_) {
        auto parsed = TryParseDouble(text);
        if (parsed && math::IsFinite(parsed.Value())) {
            CommitValue(parsed.Value(), parsed.Value(), CommitMethod::Code, {});
        }
    }
}

void SpinBox::HandleTextCommitted(StringView text, TextCommitReason reason) noexcept {
    // 초점 잃음 확정은 표시 모드로 돌아간다. Enter는 글자 모드에 남는다(UE
    // TextField_OnTextCommitted).
    const SpinCommitReason committed =
        reason == TextCommitReason::Enter ? SpinCommitReason::Enter : SpinCommitReason::FocusLost;
    auto parsed = ParseNumberWithUnit(text, value_, kind_, units_);
    if (reason != TextCommitReason::Enter) {
        ExitTextMode();
    }

    if (parsed) {
        CommitValue(parsed.Value(), parsed.Value(), CommitMethod::TypeIn, committed);
    }

    if (editing_) {
        (void)RefreshText();
        editor_->SelectAllText();
    }

    if (reason == TextCommitReason::Enter && clearFocusOnCommit_) {
        if (Application* app = Application::Find(*this)) {
            (void)app->SetFocus(focusUserForCommit_, SharedPtr<Element>{});
        }
    }
}

AccessibleRole SpinBox::GetAccessibleRole() const noexcept {
    return AccessibleRole::SpinBox;
}

bool SpinBox::SupportsKeyboardFocus() const noexcept {
    return true;
}

void SpinBox::OnFocusReceived(uint32 user) noexcept {
    Element::OnFocusReceived(user);
    focusUserForCommit_ = user;
    // Tab·코드가 준 초점은 글자 모드로 들어간다(UE OnFocusReceived — Navigation·SetDirectly). 초점
    // 알림 안에서는 초점을 옮길 수 없어 다음 프레임 시작으로 미룬다. 누름이 준 초점은 끌기를
    // 기다린다.
    Application* app = Application::Find(*this);
    if (!app || tracking_ || editing_ || app->GetFocusCause(user) == FocusCause::Pointer) {
        return;
    }

    const SharedPtr<UiHostServices> host = FindUiHostServicesRef();
    const SharedPtr<Element> self = Parent() ? Parent()->FindChild(this) : SharedPtr<Element>{};
    if (!host || !self) {
        return;
    }

    const FocusCause cause = app->GetFocusCause(user);
    (void)host->DeferFrameAction(
        Delegate<void()>::CreateLambda([weak = WeakPtr<Element>(self), user, cause]() noexcept {
            if (const SharedPtr<Element> spin = weak.Lock()) {
                auto& box = StaticElementCast<SpinBox>(*spin);
                if (!box.editing_ && !box.tracking_) {
                    box.EnterTextMode(user, cause);
                }
            }
        }));
}

KeyReply SpinBox::OnKeyEvent(const KeyEvent& key) noexcept {
    const input::InputEvent& event = key.input;
    if (event.type != input::InputEventType::KeyDown) {
        return Element::OnKeyEvent(key);
    }

    using input::Key;
    // 끄는 중 Esc는 끌기 전 값으로 되돌린다(UE OnKeyDown Escape → CancelMouseCapture).
    if (event.key == Key::Escape && tracking_) {
        const bool wasDragging = dragging_;
        ResetDrag();
        if (wasDragging) {
            internal_ = preDrag_;
            value_ = preDrag_;
            committed_.ExecuteIfBound(value_, SpinCommitReason::Spin);
            endDrag_.ExecuteIfBound(value_);
            (void)RefreshText();
        }

        return KeyReply::Handled();
    }

    const bool up = event.key == Key::Up || event.key == Key::Right;
    const bool down = event.key == Key::Down || event.key == Key::Left;
    if (up || down) {
        // 한 단계(Delta, 없으면 기본 단계 × 배수 — UE OnKeyDown 676–693행).
        const float multiplier = Multiplier(key.chord.shift, key.chord.control, key.chord.alt);
        const double step = delta_ > 0 ? delta_ : DefaultStep(multiplier);
        const double next = value_ + (up ? step : -step);
        internal_ = value_;
        ExitTextMode();
        CommitValue(next, next, CommitMethod::ArrowKey, SpinCommitReason::ArrowKey);
        return KeyReply::Handled();
    }

    if ((event.key == Key::Enter || event.key == Key::NumpadEnter) && !editing_) {
        internal_ = value_;
        EnterTextMode(key.user, FocusCause::Navigation);
        return KeyReply::Handled();
    }

    return Element::OnKeyEvent(key);
}

bool SpinBox::ToLocalDelta(const PointerEvent& event, float& dx) noexcept {
    AffineTransform2D inverse;
    if (!GetGeometry().render.TryInverse(inverse)) {
        return false;
    }

    // 잠근 capture면 Application이 준 이동량, 아니면 직전 위치와의 차이다(창 → local).
    const Vec2 local = inverse.Apply(event.position);
    if (event.hasDelta) {
        const Vec2 moved = inverse.Apply(event.position + event.delta);
        dx = moved.x - local.x;
    } else {
        dx = local.x - lastX_;
        lastX_ = local.x;
    }

    return math::IsFinite(dx);
}

PointerReply SpinBox::OnPointerEvent(const PointerEvent& event) noexcept {
    if (event.type == PointerEventType::Wheel) {
        // 글자 모드 초점일 때만이다(UE OnMouseWheel — HasKeyboardFocus는 편집기 초점).
        if (!wheelEnabled_ || tracking_ || !editing_ || !editor_ || !editor_->IsFocused() ||
            !math::IsFinite(event.wheel) || event.wheel == 0) {
            return {};
        }

        Application* app = Application::Find(*this);
        const float multiplier =
            app ? Multiplier(app->IsShiftDown(event.userId), app->IsControlDown(event.userId),
                             app->IsAltDown(event.userId))
                : 1.0f;
        const bool small = !IsIntegralNumericKind(kind_) &&
                           MaxSliderValue() - MinSliderValue() <= SpinSmallStepSizeMax;
        const double base =
            wheelStep_ ? wheelStep_.Value() : (small ? SpinSmallStepSize : SpinStepSize);
        const double next = internal_ + (event.wheel > 0 ? 1.0 : -1.0) * base * multiplier;
        CommitValue(next, next, CommitMethod::Spin, {});
        (void)RefreshText();
        editor_->SelectAllText();
        return {.handled = true};
    }

    if (tracking_ &&
        (event.source != source_ || event.userId != user_ || event.pointerId != pointer_)) {
        return {};
    }

    const bool mouse = event.source == PointerSource::Mouse;
    if (event.type == PointerEventType::Down) {
        if (tracking_ || (mouse && event.button != input::MouseButton::Left)) {
            return {};
        }

        AffineTransform2D inverse;
        if (!GetGeometry().render.TryInverse(inverse)) {
            return {};
        }

        // 누름: capture, 끈 거리 0, 끌기 전 값(UE OnMouseButtonDown). 커서는 끌기가 시작될 때
        // 잠근다.
        tracking_ = true;
        dragging_ = false;
        source_ = event.source;
        user_ = event.userId;
        pointer_ = event.pointerId;
        distance_ = 0;
        lastX_ = inverse.Apply(event.position).x;
        preDrag_ = value_;
        internal_ = value_;
        return {.handled = true, .capture = true};
    }

    if (!tracking_) {
        return {};
    }

    if (event.type == PointerEventType::Move) {
        float dx = 0;
        if (!ToLocalDelta(event, dx) || !sliderEnabled_) {
            return {.handled = true};
        }

        if (!dragging_) {
            // 문턱을 넘으면 끌기다(UE DistanceDragged > DragTriggerDistance). 글자 모드는 끝낸다.
            distance_ += math::Abs(dx);
            if (distance_ <= PointerDragThreshold) {
                return {.handled = true};
            }

            ExitTextMode();
            dragging_ = true;
            InvalidatePaint();
            beginDrag_.ExecuteIfBound();
            return {.handled = true, .lockCursor = mouse};
        }

        Application* app = Application::Find(*this);
        const bool shift = app && app->IsShiftDown(event.userId);
        const bool control = app && app->IsControlDown(event.userId);
        const bool alt = app && app->IsAltDown(event.userId);
        const CommitMethod method =
            shift || control ? CommitMethod::SpinMultiplier : CommitMethod::Spin;
        const double step = DefaultStep(Multiplier(shift, control, alt));
        const float width = math::Max(GetGeometry().size.x, SpinMinSliderWidth);
        double next = internal_;
        if (!IsSpinRangeUnlimited()) {
            // 채움 비율에 이동량을 더해 값으로 되돌린다(UE OnMouseMove 505–551행).
            const double minimum = MinSliderValue();
            const double maximum = MaxSliderValue();
            const bool neutral =
                neutral_ && neutral_.Value() > minimum && neutral_.Value() < maximum;
            const float start = neutral ? Fraction(neutral_.Value(), minimum, maximum) : 0.0f;
            float filled = Fraction(internal_, minimum, maximum);
            if (exponent_ != 1.0f) {
                filled = neutral ? ExponentFraction(filled, start, exponent_)
                                 : 1.0f - platform::PlatformMath::Pow(1.0f - filled, exponent_);
            }

            filled = math::Clamp(filled * width + dx * static_cast<float>(step), 0.0f, width);
            float percent = math::Clamp(filled / width, 0.0f, 1.0f);
            if (exponent_ != 1.0f) {
                percent =
                    neutral ? ExponentFraction(percent, start, 1.0f / exponent_)
                            : 1.0f - platform::PlatformMath::Pow(1.0f - percent, 1.0f / exponent_);
            }

            next = minimum + (maximum - minimum) * static_cast<double>(percent);
        } else if (dx != 0) {
            // 범위가 없으면 지금 크기에 비례한다(선형 감도·Delta가 있으면 Delta^지수 — UE
            // 556–570행).
            const double sign = dx > 0 ? 1.0 : -1.0;
            if (linearSensitivity_ > 0 && delta_ > 0) {
                const double moved = math::Abs(dx) / static_cast<double>(linearSensitivity_);
                next = internal_ +
                       sign * moved *
                           platform::PlatformMath::Pow(delta_, static_cast<double>(exponent_)) *
                           step;
            } else {
                const double moved = math::Abs(dx) / static_cast<double>(width);
                const double magnitude = math::Clamp(math::Abs(internal_), 1.0, MaxValue());
                next = internal_ +
                       sign * moved *
                           platform::PlatformMath::Pow(magnitude, static_cast<double>(exponent_)) *
                           step;
            }
        }

        CommitValue(next, next, method, {});
        return {.handled = true};
    }

    if (event.type == PointerEventType::Up || event.type == PointerEventType::Cancel) {
        if (mouse && event.type == PointerEventType::Up &&
            event.button != input::MouseButton::Left) {
            return {.handled = true};
        }

        const bool dragged = dragging_;
        const bool tap = event.type == PointerEventType::Up && distance_ <= PointerDragThreshold;
        ResetDrag();
        if (dragged && event.type == PointerEventType::Up) {
            // 놓으면 Delta에 맞추고(배수 키를 누른 채면 맞추지 않음) 확정한다(UE OnMouseButtonUp).
            Application* app = Application::Find(*this);
            const bool multiplier =
                app && (app->IsShiftDown(event.userId) || app->IsControlDown(event.userId));
            if (delta_ > 0 && !multiplier) {
                internal_ = GridSnap(internal_, delta_);
            }

            value_ = RoundToNumericKind(internal_, kind_);
            InvalidatePaint();
            (void)RefreshText();
            committed_.ExecuteIfBound(value_, SpinCommitReason::Spin);
            endDrag_.ExecuteIfBound(value_);
        } else if (tap) {
            // 끌지 않고 놓으면 글자 모드다.
            internal_ = value_;
            EnterTextMode(event.userId, FocusCause::Direct);
        }

        // 커서는 끌기가 시작된 자리(잠금 자리)로 돌아간다 — Application 기본(23.5절).
        return {.handled = true, .releaseCapture = true};
    }

    return {.handled = true};
}

void SpinBox::ResetDrag() noexcept {
    if (dragging_) {
        InvalidatePaint();
    }

    tracking_ = false;
    dragging_ = false;
    distance_ = 0;
}

void SpinBox::OnPointerCaptureLost(PointerSource source, uint32 user, uint32 pointer) noexcept {
    if (tracking_ && source == source_ && user == user_ && pointer == pointer_) {
        ResetDrag();
    }
}

void SpinBox::OnMouseEnter(uint32 user) noexcept {
    Element::OnMouseEnter(user);
    if (!hovered_) {
        hovered_ = true;
        InvalidatePaint();
    }
}

void SpinBox::OnMouseLeave(uint32 user) noexcept {
    Element::OnMouseLeave(user);
    if (hovered_) {
        hovered_ = false;
        InvalidatePaint();
    }
}

bool SpinBox::IsHovered() const noexcept {
    return hovered_;
}

input::MouseCursor SpinBox::QueryMouseCursor(Vec2) const noexcept {
    // 끌기를 끄면 기본, 평소 좌우 크기 조절이다(UE OnCursorQuery — 끄는 중에는 커서가 잠겨 숨는다).
    return sliderEnabled_ && !editing_ ? input::MouseCursor::ResizeHorizontal
                                       : input::MouseCursor::Arrow;
}

Vec2 SpinBox::ComputeDesiredSize(float) const noexcept {
    const Vec4 content = SpinBoxContentPadding;
    const Vec4 text = style_.textPadding;
    Vec2 inner = editor_ ? editor_->GetDesiredSize() : Vec2{};
    inner.x = math::Max(inner.x, minDesiredWidth_ - style_.arrowsImage.imageSize.x);
    const float arrows =
        style_.arrowsImage.mode != BrushMode::None ? style_.arrowsImage.imageSize.x : 0.0f;
    return {content.x + content.z + text.x + text.z + inner.x + arrows,
            content.y + content.w +
                math::Max(text.y + text.w + inner.y, style_.arrowsImage.imageSize.y)};
}

Result<void> SpinBox::Arrange(const Geometry& geometry) noexcept {
    SetGeometry(geometry);
    if (!editor_) {
        return Result<void>::Success();
    }

    const Vec4 content = SpinBoxContentPadding;
    const Vec4 text = style_.textPadding;
    const float arrows =
        style_.arrowsImage.mode != BrushMode::None ? style_.arrowsImage.imageSize.x : 0.0f;
    const Vec2 size = geometry.size;
    const float left = content.x + text.x;
    const float width = math::Max(0.0f, size.x - left - text.z - content.z - arrows);
    const float height =
        math::Min(editor_->GetDesiredSize().y,
                  math::Max(0.0f, size.y - content.y - content.w - text.y - text.w));
    // 글자 칸은 세로 가운데다(UE SSpinBox 글자 칸 VAlign_Center).
    const float top = (size.y - height) * 0.5f;
    return editor_->ArrangeIfNeeded(
        GetGeometry().MakeChild({width, height}, {1, 0, 0, 1, left, top}, {}, {}));
}

Result<void> SpinBox::OnPaint(PaintList& list, int32 layer) const noexcept {
    const Geometry& geometry = GetGeometry();
    const StyleColorContext context = list.ColorContext();
    const bool active = dragging_ || editing_;
    // 활성 바탕이 없으면 올림 바탕, 올림 채움이 없으면 활성 채움이다(UE Construct 159–165행).
    const StyleBrush& activeBackground = style_.activeBackground.mode != BrushMode::None
                                             ? style_.activeBackground
                                             : style_.hoveredBackground;
    const StyleBrush& hoveredFill =
        style_.hoveredFill.mode != BrushMode::None ? style_.hoveredFill : style_.activeFill;
    const StyleBrush& background = active        ? activeBackground
                                   : IsHovered() ? style_.hoveredBackground
                                                 : style_.background;
    const StyleBrush& fill = active        ? style_.activeFill
                             : IsHovered() ? hoveredFill
                                           : style_.inactiveFill;
    if (background.mode != BrushMode::None) {
        auto painted = list.TryAddBrush(geometry, background.Resolve(list.Theme(), context),
                                        {1, 1, 1, 1}, layer);
        if (!painted) {
            return painted;
        }
    }

    // 범위가 있으면 채움 막대(글자 모드에서는 그리지 않는다 — UE OnPaint 258–296행).
    const Optional<float> fraction = FillFraction();
    if (fraction && !editing_ && fill.mode != BrushMode::None) {
        const Vec4 inset = style_.insetPadding;
        const Vec2 size{geometry.size.x * fraction.Value() - inset.x - inset.z,
                        geometry.size.y - inset.y - inset.w};
        if (size.x > 0 && size.y > 0) {
            auto painted =
                list.TryAddBrush(geometry.MakeChild(size, {1, 0, 0, 1, inset.x, inset.y}, {}, {}),
                                 fill.Resolve(list.Theme(), context), {1, 1, 1, 1}, layer);
            if (!painted) {
                return painted;
            }
        }
    }

    if (style_.arrowsImage.mode != BrushMode::None) {
        const Vec2 image = style_.arrowsImage.imageSize;
        const Vec4 content = SpinBoxContentPadding;
        const auto arrows = geometry.MakeChild(
            image,
            {1, 0, 0, 1, geometry.size.x - content.z - image.x, (geometry.size.y - image.y) * 0.5f},
            {}, {});
        return list.TryAddBrush(arrows, style_.arrowsImage.Resolve(list.Theme(), context),
                                context.foreground, layer);
    }

    return Result<void>::Success();
}

} // namespace rocks::ui
