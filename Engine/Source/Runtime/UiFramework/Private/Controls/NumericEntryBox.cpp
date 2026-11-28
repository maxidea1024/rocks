#include "NumericColorBar.h"
#include "NumericEntryLabel.h"

#include <Core/Math/MathUtility.h>
#include <Core/Text/StringBuilder.h>
#include <UiCore/Styles/ToolTipStyle.h>
#include <UiFramework/Controls/CheckBox.h>
#include <UiFramework/Controls/EditableText.h>
#include <UiFramework/Controls/EditableTextBox.h>
#include <UiFramework/Controls/NumericEntryBox.h>
#include <UiFramework/Popups/ToolTip.h>
#include <UiFramework/Text/TextLabel.h>
#include <UiFramework/Text/TextShaper.h>

namespace rocks::ui {

namespace {

// 스핀 칸의 슬라이더 범위 기본값이다(UE SNumericEntryBox MinSliderValue 0·MaxSliderValue 100).
constexpr double EntrySliderMinimum = 0.0;
constexpr double EntrySliderMaximum = 100.0;
// 툴팁의 범위 구분과 정확한 값 앞말이다.
constexpr StringView RangeSeparator = " \xE2\x80\x93 ";
constexpr StringView ExactValuePrefix = "Exact value: ";

// 요소에 여백을 둔 칸 하나를 놓는다.
Result<void> Place(Element& child, const Geometry& parent, float x, float y, float width,
                   float height) noexcept {
    return child.ArrangeIfNeeded(parent.MakeChild({math::Max(0.0f, width), math::Max(0.0f, height)},
                                                  {1, 0, 0, 1, x, y}, {}, {}));
}

} // namespace

NumericEntryBox::NumericEntryBox(Allocator& allocator, SharedPtr<TextShaper> shaper,
                                 Platform platform) noexcept
    : Element(allocator), allocator_(allocator), shaper_(Move(shaper)), platform_(platform),
      undetermined_(allocator), toolTipPrefix_(allocator) {
    (void)undetermined_.TryAssign(NumericUndeterminedText);
}

NumericEntryBox::~NumericEntryBox() noexcept = default;

Result<void> NumericEntryBox::Initialize(float fontSize, bool allowSpin) noexcept {
    if (text_) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    fontSize_ = fontSize;
    // 스핀(있으면) 다음 상자다 — 상자가 위에 그려진다(UE Overlay 슬롯 순서).
    if (allowSpin) {
        auto spin = SharedPtr<SpinBox>::TryMake(allocator_, allocator_, shaper_, platform_);
        if (!spin) {
            return Result<void>::Failure(spin.Error());
        }

        auto ready = spin.Value()->Initialize(fontSize);
        if (ready) {
            ready = spin.Value()->SetSliderRange(EntrySliderMinimum, EntrySliderMaximum);
        }

        if (ready) {
            ready = TryAddChild(spin.Value());
        }

        if (!ready) {
            return ready;
        }

        spin_ = Move(spin.Value());
        spin_->SetSpinBoxStyle(DefaultSpinBoxStyle());
        spin_->SetOnChanged(Delegate<void(double)>::CreateLambda([this](double value) noexcept {
            value_ = value;
            changed_.ExecuteIfBound(value);
        }));
        spin_->SetOnCommitted(Delegate<void(double, SpinCommitReason)>::CreateLambda(
            [this](double value, SpinCommitReason reason) noexcept {
                value_ = value;
                committed_.ExecuteIfBound(value, reason);
            }));
        spin_->SetOnDragBegin(
            Delegate<void()>::CreateLambda([this]() noexcept { beginDrag_.ExecuteIfBound(); }));
        spin_->SetOnDragEnd(Delegate<void(double)>::CreateLambda(
            [this](double value) noexcept { endDrag_.ExecuteIfBound(value); }));
    }

    auto box =
        SharedPtr<EditableTextBox>::TryMake(allocator_, allocator_, shaper_, platform_, false);
    if (!box) {
        return Result<void>::Failure(box.Error());
    }

    auto ready = box.Value()->Initialize({}, fontSize);
    if (ready) {
        ready = TryAddChild(box.Value());
    }

    if (!ready) {
        return ready;
    }

    text_ = Move(box.Value());
    EditableText& editor = text_->Editor();
    editor.SetSelectAllTextWhenFocused(true);
    editor.SetSelectAllTextOnCommit(true);
    editor.SetOnTextChanged(Delegate<void(StringView)>::CreateLambda(
        [this](StringView text) noexcept { HandleTextChanged(text); }));
    editor.SetOnTextCommitted(Delegate<void(StringView, TextCommitReason)>::CreateLambda(
        [this](StringView text, TextCommitReason reason) noexcept {
            HandleTextCommitted(text, reason);
        }));
    // 툴팁은 띄울 때 지금 값으로 만든다.
    auto tip = SetToolTipFactory(
        Element::ToolTipFactory::CreateLambda([this]() noexcept { return BuildToolTip(); }));
    if (!tip) {
        return tip;
    }

    SyncFieldVisibility();
    return RefreshText();
}

Result<void> NumericEntryBox::SetValue(Optional<double> value) noexcept {
    if (value && !math::IsFinite(value.Value())) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (value) {
        const double low = minValue_ ? minValue_.Value() : NumericKindLowest(kind_);
        const double high = maxValue_ ? maxValue_.Value() : NumericKindMax(kind_);
        value = math::Clamp(RoundToNumericKind(value.Value(), kind_), low, high);
    }

    value_ = value;
    if (spin_ && value_) {
        auto set = spin_->SetValue(value_.Value());
        if (!set) {
            return set;
        }
    }

    SyncFieldVisibility();
    return RefreshText();
}

Optional<double> NumericEntryBox::Value() const noexcept {
    return value_;
}

Result<void> NumericEntryBox::SetUndeterminedText(StringView text) noexcept {
    auto assigned = undetermined_.TryAssign(text);
    if (!assigned) {
        return assigned;
    }

    return RefreshText();
}

Result<void> NumericEntryBox::SetLabel(const SharedPtr<Element>& label,
                                       NumericLabelLocation location, Vec4 padding) noexcept {
    if (!padding.IsFinite() || padding.x < 0 || padding.y < 0 || padding.z < 0 || padding.w < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (label_) {
        (void)RemoveChild(*label_);
        label_.Reset();
    }

    if (label) {
        auto added = TryAddChild(label);
        if (!added) {
            return added;
        }

        label_ = label;
    }

    labelLocation_ = location;
    labelPadding_ = padding;
    InvalidateLayout();
    return Result<void>::Success();
}

Result<void> NumericEntryBox::SetToggle(bool display, bool checked) noexcept {
    if (!display) {
        if (toggle_) {
            (void)RemoveChild(*toggle_);
            toggle_.Reset();
        }
    } else if (!toggle_) {
        auto made = SharedPtr<CheckBox>::TryMake(allocator_, allocator_);
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        auto added = TryAddChild(made.Value());
        if (!added) {
            return added;
        }

        toggle_ = Move(made.Value());
        toggle_->SetOnChanged(
            Delegate<void(CheckState)>::CreateLambda([this](CheckState state) noexcept {
                const bool on = state == CheckState::Checked;
                if (spin_) {
                    spin_->SetEnabled(on);
                }

                if (text_) {
                    text_->SetEnabled(on);
                }

                toggled_.ExecuteIfBound(on);
            }));
    }

    if (toggle_) {
        auto set = toggle_->SetState(checked ? CheckState::Checked : CheckState::Unchecked);
        if (!set) {
            return set;
        }
    }

    // 토글이 없으면 칸은 늘 활성이다.
    const bool enabled = !toggle_ || checked;
    if (spin_) {
        spin_->SetEnabled(enabled);
    }

    if (text_) {
        text_->SetEnabled(enabled);
    }

    InvalidateLayout();
    return Result<void>::Success();
}

bool NumericEntryBox::IsToggleChecked() const noexcept {
    return !toggle_ || toggle_->GetState() == CheckState::Checked;
}

void NumericEntryBox::SetOnToggleChanged(Delegate<void(bool)>&& callback) noexcept {
    toggled_ = Move(callback);
}

Result<void> NumericEntryBox::SetToolTipPrefix(StringView prefix) noexcept {
    return toolTipPrefix_.TryAssign(prefix);
}

void NumericEntryBox::SetNumericKind(NumericKind kind) noexcept {
    kind_ = kind;
    if (spin_) {
        spin_->SetNumericKind(kind);
    }

    (void)SetValue(value_);
}

Result<void> NumericEntryBox::SetValueRange(Optional<double> minimum,
                                            Optional<double> maximum) noexcept {
    if ((minimum && !math::IsFinite(minimum.Value())) ||
        (maximum && !math::IsFinite(maximum.Value())) ||
        (minimum && maximum && minimum.Value() > maximum.Value())) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (spin_) {
        auto set = spin_->SetValueRange(minimum, maximum);
        if (!set) {
            return set;
        }
    }

    minValue_ = minimum;
    maxValue_ = maximum;
    return SetValue(value_);
}

Result<void> NumericEntryBox::SetSliderRange(Optional<double> minimum,
                                             Optional<double> maximum) noexcept {
    return spin_ ? spin_->SetSliderRange(minimum, maximum) : Result<void>::Success();
}

Result<void> NumericEntryBox::SetDelta(double delta) noexcept {
    return spin_ ? spin_->SetDelta(delta) : Result<void>::Success();
}

Result<void> NumericEntryBox::SetFractionalDigits(int32 minimum, int32 maximum) noexcept {
    if (minimum < 0 || maximum < 0 || minimum > MaxNumericFractionalDigits ||
        maximum > MaxNumericFractionalDigits) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (spin_) {
        auto set = spin_->SetFractionalDigits(minimum, maximum);
        if (!set) {
            return set;
        }
    }

    minDigits_ = minimum;
    maxDigits_ = maximum;
    return RefreshText();
}

void NumericEntryBox::SetUnits(const NumericUnitOptions& units) noexcept {
    units_ = units;
    if (spin_) {
        spin_->SetUnits(units);
    }

    (void)RefreshText();
}

SpinBox* NumericEntryBox::Spin() const noexcept {
    return spin_.Get();
}

EditableTextBox* NumericEntryBox::TextBox() const noexcept {
    return text_.Get();
}

CheckBox* NumericEntryBox::Toggle() const noexcept {
    return toggle_.Get();
}

void NumericEntryBox::SetOnValueChanged(Delegate<void(double)>&& callback) noexcept {
    changed_ = Move(callback);
}

void NumericEntryBox::SetOnValueCommitted(
    Delegate<void(double, SpinCommitReason)>&& callback) noexcept {
    committed_ = Move(callback);
}

void NumericEntryBox::SetOnUndeterminedCommitted(
    Delegate<void(StringView, SpinCommitReason)>&& callback) noexcept {
    undeterminedCommitted_ = Move(callback);
}

void NumericEntryBox::SetOnDragBegin(Delegate<void()>&& callback) noexcept {
    beginDrag_ = Move(callback);
}

void NumericEntryBox::SetOnDragEnd(Delegate<void(double)>&& callback) noexcept {
    endDrag_ = Move(callback);
}

NumericFormatOptions NumericEntryBox::FormatOptions() const noexcept {
    return {.kind = kind_, .minFractionalDigits = minDigits_, .maxFractionalDigits = maxDigits_};
}

void NumericEntryBox::SyncFieldVisibility() noexcept {
    // 값이 있고 스핀이면 스핀, 아니면 상자다(UE Tick 765–790행).
    const bool spinShown = spin_ && value_;
    if (spin_) {
        spin_->SetVisibility(spinShown ? Visibility::Visible : Visibility::Collapsed);
    }

    if (text_) {
        text_->SetVisibility(spinShown ? Visibility::Collapsed : Visibility::Visible);
    }

    InvalidateLayout();
}

Result<void> NumericEntryBox::RefreshText() noexcept {
    // 치는 중에는 글자를 바꾸지 않는다.
    if (!text_ || text_->Editor().IsFocused()) {
        return Result<void>::Success();
    }

    String text(allocator_);
    if (value_) {
        auto formatted = FormatNumberWithUnit(value_.Value(), FormatOptions(), units_, text);
        if (!formatted) {
            return formatted;
        }
    } else {
        auto assigned = text.TryAssign(undetermined_.View());
        if (!assigned) {
            return assigned;
        }
    }

    filling_ = true;
    auto set = text_->Editor().SetText(text.View());
    filling_ = false;
    return set;
}

void NumericEntryBox::HandleTextChanged(StringView text) noexcept {
    if (filling_) {
        return;
    }

    // 미정이고 받는 쪽이 있으면 바뀐 값을 내지 않는다(UE OnTextChanged).
    if (!value_ && undeterminedCommitted_.IsBound()) {
        return;
    }

    auto parsed = ParseNumberWithUnit(text, value_ ? value_.Value() : 0.0, kind_, units_);
    if (parsed) {
        const double low = minValue_ ? minValue_.Value() : NumericKindLowest(kind_);
        const double high = maxValue_ ? maxValue_.Value() : NumericKindMax(kind_);
        changed_.ExecuteIfBound(math::Clamp(parsed.Value(), low, high));
    }
}

void NumericEntryBox::HandleTextCommitted(StringView text, TextCommitReason reason) noexcept {
    const SpinCommitReason committed =
        reason == TextCommitReason::Enter ? SpinCommitReason::Enter : SpinCommitReason::FocusLost;
    if (!value_ && undeterminedCommitted_.IsBound()) {
        undeterminedCommitted_.Execute(text, committed);
        return;
    }

    auto parsed = ParseNumberWithUnit(text, value_ ? value_.Value() : 0.0, kind_, units_);
    if (parsed) {
        // 값이 생기면 스핀으로 돌아간다(SetValue가 자르고 보인다).
        (void)SetValue(parsed.Value());
        committed_.ExecuteIfBound(value_.Value(), committed);
    }

    // 초점이 남아 있어도 확정한 값으로 글자를 맞춘다.
    filling_ = true;
    String shown(allocator_);
    if (value_ ? FormatNumberWithUnit(value_.Value(), FormatOptions(), units_, shown).IsOk()
               : shown.TryAssign(undetermined_.View()).IsOk()) {
        (void)text_->Editor().SetText(shown.View());
    }

    filling_ = false;
}

Result<SharedPtr<Element>> NumericEntryBox::BuildToolTip() noexcept {
    if (!shaper_) {
        return Result<SharedPtr<Element>>::Failure(ErrorCode::NotReady);
    }

    StringBuilder<> line(allocator_);
    String value(allocator_);
    NumericFormatOptions options = FormatOptions();
    options.indicateNearlyInteger = true;
    Result<void> written = Result<void>::Success();
    if (value_) {
        written = FormatNumberWithUnit(value_.Value(), options, units_, value);
    } else {
        written = value.TryAssign(undetermined_.View());
    }

    // 거의 정수 표시("...")면 정확한 값을 보인다(UE NearZeroWarning_ToolTip).
    const bool nearly = value_ && value.View().Contains("...");
    if (written) {
        written = line.TryAppend(toolTipPrefix_.View());
    }

    if (written && nearly) {
        written = line.TryAppend(ExactValuePrefix);
        if (written) {
            written = line.TryAppendFormat("{:e}", value_.Value());
        }
    } else if (written) {
        written = line.TryAppend(value.View());
    }

    // 범위가 둘 다 있으면 붙인다(Rocks 보강 — 24.6절).
    if (written && minValue_ && maxValue_) {
        String low(allocator_);
        String high(allocator_);
        written = FormatNumber(minValue_.Value(), FormatOptions(), low);
        if (written) {
            written = FormatNumber(maxValue_.Value(), FormatOptions(), high);
        }

        if (written) {
            written = line.TryAppend(" (");
        }

        if (written) {
            written = line.TryAppend(low.View());
        }

        if (written) {
            written = line.TryAppend(RangeSeparator);
        }

        if (written) {
            written = line.TryAppend(high.View());
        }

        if (written) {
            written = line.TryAppend(")");
        }
    }

    if (!written) {
        return Result<SharedPtr<Element>>::Failure(written.Error());
    }

    auto label = SharedPtr<TextLabel>::TryMake(allocator_, allocator_);
    auto tip = SharedPtr<ToolTip>::TryMake(allocator_, allocator_);
    auto content = Text::Invariant(allocator_, line.View());
    if (!label || !tip || !content) {
        return Result<SharedPtr<Element>>::Failure(ErrorCode::OutOfMemory);
    }

    auto shaped =
        shaper_->Shape(*label.Value(), content.Value(), DefaultToolTipStyle().text.font.size);
    if (shaped) {
        shaped = tip.Value()->Construct({.text = label.Value()});
    }

    if (!shaped) {
        return Result<SharedPtr<Element>>::Failure(shaped.Error());
    }

    return Result<SharedPtr<Element>>::Success(tip.Value());
}

Result<SharedPtr<Element>> NumericEntryBox::MakeLabel(Allocator& allocator, TextShaper& shaper,
                                                      StringView text, float fontSize,
                                                      ColorRef foreground,
                                                      ColorRef background) noexcept {
    auto label = SharedPtr<NumericEntryLabel>::TryMake(allocator, allocator, background);
    auto glyphs = SharedPtr<TextLabel>::TryMake(allocator, allocator);
    auto content = Text::Invariant(allocator, text);
    if (!label || !glyphs || !content) {
        return Result<SharedPtr<Element>>::Failure(ErrorCode::OutOfMemory);
    }

    auto ready = shaper.Shape(*glyphs.Value(), content.Value(), fontSize);
    if (ready) {
        ready = label.Value()->TryAddChild(glyphs.Value());
    }

    if (ready) {
        ready = label.Value()->SetForegroundColor(foreground);
    }

    if (!ready) {
        return Result<SharedPtr<Element>>::Failure(ready.Error());
    }

    return Result<SharedPtr<Element>>::Success(label.Value());
}

Result<SharedPtr<Element>> NumericEntryBox::MakeNarrowColorLabel(Allocator& allocator,
                                                                 LinearColor color) noexcept {
    auto bar = SharedPtr<NumericColorBar>::TryMake(allocator, allocator, color);
    if (!bar) {
        return Result<SharedPtr<Element>>::Failure(bar.Error());
    }

    return Result<SharedPtr<Element>>::Success(bar.Value());
}

Vec2 NumericEntryBox::ComputeDesiredSize(float) const noexcept {
    float width = 0;
    float height = 0;
    if (toggle_) {
        const Vec2 size = toggle_->GetDesiredSize();
        width += size.x + NumericTogglePadding.x + NumericTogglePadding.z;
        height = math::Max(height, size.y);
    }

    if (label_ && labelLocation_ == NumericLabelLocation::Outside) {
        const Vec2 size = label_->GetDesiredSize();
        width += size.x + labelPadding_.x + labelPadding_.z;
        height = math::Max(height, size.y + labelPadding_.y + labelPadding_.w);
    }

    Vec2 field{};
    if (spin_ && spin_->GetVisibility() != Visibility::Collapsed) {
        field = spin_->GetDesiredSize();
    }

    if (text_ && text_->GetVisibility() != Visibility::Collapsed) {
        const Vec2 size = text_->GetDesiredSize();
        field = {math::Max(field.x, size.x), math::Max(field.y, size.y)};
    }

    return {width + field.x, math::Max(height, field.y)};
}

Result<void> NumericEntryBox::Arrange(const Geometry& geometry) noexcept {
    SetGeometry(geometry);
    const Geometry& self = GetGeometry();
    const Vec2 size = geometry.size;
    float x = 0;
    if (toggle_) {
        const Vec2 desired = toggle_->GetDesiredSize();
        auto placed = Place(*toggle_, self, x + NumericTogglePadding.x, (size.y - desired.y) * 0.5f,
                            desired.x, desired.y);
        if (!placed) {
            return placed;
        }

        x += desired.x + NumericTogglePadding.x + NumericTogglePadding.z;
    }

    const bool inside = label_ && labelLocation_ == NumericLabelLocation::Inside;
    if (label_ && !inside) {
        const Vec2 desired = label_->GetDesiredSize();
        auto placed = Place(*label_, self, x + labelPadding_.x, labelPadding_.y, desired.x,
                            size.y - labelPadding_.y - labelPadding_.w);
        if (!placed) {
            return placed;
        }

        x += desired.x + labelPadding_.x + labelPadding_.z;
    }

    const float fieldWidth = size.x - x;
    Element* const fields[] = {spin_.Get(), text_.Get()};
    for (Element* field : fields) {
        if (!field || field->GetVisibility() == Visibility::Collapsed) {
            continue;
        }

        auto placed = Place(*field, self, x, 0, fieldWidth, size.y);
        if (!placed) {
            return placed;
        }
    }

    // 안쪽 레이블은 칸 왼쪽 위에 세로 채움으로 겹친다(UE LabelVAlign Fill).
    if (inside) {
        const Vec2 desired = label_->GetDesiredSize();
        auto placed = Place(*label_, self, x + labelPadding_.x, labelPadding_.y, desired.x,
                            size.y - labelPadding_.y - labelPadding_.w);
        if (!placed) {
            return placed;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::ui
