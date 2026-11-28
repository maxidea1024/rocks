#pragma once

#include <UiFramework/Controls/SpinBox.h>

namespace rocks::ui {

class CheckBox;
class EditableTextBox;

// 숫자 입력 칸 레이블 자리다(UE SNumericEntryBox::ELabelLocation).
enum class NumericLabelLocation : uint8 {
    // 칸 왼쪽 별도 칸이다(글자 레이블에 알맞다 — UE 기본).
    Outside,
    // 칸 위 왼쪽에 겹친다(색 막대 같은 글자 없는 레이블 — 스핀이면 스핀 위).
    Inside
};

// 레이블 여백 기본값(UE LabelPadding (3, 0))과 토글 여백(UE TogglePadding (1, 0, 1, 0))이다.
inline constexpr Vec4 NumericLabelPadding{3.0f, 0.0f, 3.0f, 0.0f};
inline constexpr Vec4 NumericTogglePadding{1.0f, 0.0f, 1.0f, 0.0f};
// 미정 값 글자다(UE DefaultUndeterminedString "---").
inline constexpr StringView NumericUndeterminedText = "---";

// 숫자 입력 칸이다(UE SNumericEntryBox — Docs/Design/UiToolkit 24.6절). 값은 Optional이고, 비어
// 있으면 편집 텍스트 상자에 미정 글자("---")를 보인다. AllowSpin이면 값이 있을 때 스핀
// 상자(끌기·글자 모드)가 보인다. 레이블(바깥·안쪽)과 활성 토글을 둘 수 있다. 툴팁은 앞말 + 값(+
// 범위)이다.
//
// 사용 예:
//   auto box = SharedPtr<NumericEntryBox>::TryMake(allocator, allocator, shaper, platform);
//   (void)box.Value()->Initialize(NormalFontSize, /*allowSpin*/ true);
//   auto label = NumericEntryBox::MakeNarrowColorLabel(allocator, AxisColorX);
//   (void)box.Value()->SetLabel(label.Value(), NumericLabelLocation::Inside);
//   box.Value()->SetOnValueCommitted(Delegate<void(double, SpinCommitReason)>::CreateLambda(...));
class ROCKS_UIFRAMEWORK_API NumericEntryBox final : public Element {
    ROCKS_UI_ELEMENT(NumericEntryBox, Element)

public:
    NumericEntryBox(Allocator& allocator, SharedPtr<TextShaper> shaper, Platform platform) noexcept;
    ~NumericEntryBox() noexcept override;

    // 칸을 만든다(스핀은 allowSpin일 때만). 한 번만 부른다.
    Result<void> Initialize(float fontSize, bool allowSpin) noexcept;

    // 값(비어 있으면 미정)이다. 형식으로 반올림하고 값 범위로 자른다. 비유한이면 InvalidArgument다.
    Result<void> SetValue(Optional<double> value) noexcept;
    Optional<double> Value() const noexcept;
    Result<void> SetUndeterminedText(StringView text) noexcept;
    // 레이블(빈 포인터면 뗀다)이다. 여백이 음수·비유한이면 InvalidArgument다.
    Result<void> SetLabel(const SharedPtr<Element>& label, NumericLabelLocation location,
                          Vec4 padding = NumericLabelPadding) noexcept;
    // 활성 토글을 보이거나 숨긴다(UE DisplayToggle·ToggleChecked). 끄면 칸이 비활성이다.
    Result<void> SetToggle(bool display, bool checked) noexcept;
    bool IsToggleChecked() const noexcept;
    void SetOnToggleChanged(Delegate<void(bool)>&& callback) noexcept;
    // 툴팁 앞말이다("X: " — UE ToolTipTextFormat). 빈 글자면 값만 보인다.
    Result<void> SetToolTipPrefix(StringView prefix) noexcept;

    // 스핀·상자에 함께 넘기는 설정이다(24.6절).
    void SetNumericKind(NumericKind kind) noexcept;
    Result<void> SetValueRange(Optional<double> minimum, Optional<double> maximum) noexcept;
    Result<void> SetSliderRange(Optional<double> minimum, Optional<double> maximum) noexcept;
    Result<void> SetDelta(double delta) noexcept;
    Result<void> SetFractionalDigits(int32 minimum, int32 maximum) noexcept;
    void SetUnits(const NumericUnitOptions& units) noexcept;

    // 스핀 상자(AllowSpin이 아니면 nullptr)와 편집 텍스트 상자다(Initialize 전에는 nullptr).
    SpinBox* Spin() const noexcept;
    EditableTextBox* TextBox() const noexcept;
    CheckBox* Toggle() const noexcept;

    void SetOnValueChanged(Delegate<void(double)>&& callback) noexcept;
    void SetOnValueCommitted(Delegate<void(double, SpinCommitReason)>&& callback) noexcept;
    // 미정 값에 친 글자를 그대로 받는다(UE OnUndeterminedValueCommitted). 없으면 읽어 값으로
    // 확정한다.
    void
    SetOnUndeterminedCommitted(Delegate<void(StringView, SpinCommitReason)>&& callback) noexcept;
    void SetOnDragBegin(Delegate<void()>&& callback) noexcept;
    void SetOnDragEnd(Delegate<void(double)>&& callback) noexcept;

    // 글자 레이블이다: 둥근 바탕 위 글자, 여백 (1, 0, 6, 0), hit 투명(UE BuildLabel).
    static Result<SharedPtr<Element>> MakeLabel(Allocator& allocator, TextShaper& shaper,
                                                StringView text, float fontSize,
                                                ColorRef foreground, ColorRef background) noexcept;
    // 폭 2 세로 색 막대다(UE BuildNarrowColorLabel).
    static Result<SharedPtr<Element>> MakeNarrowColorLabel(Allocator& allocator,
                                                           LinearColor color) noexcept;

    Vec2 ComputeDesiredSize(float scale) const noexcept override;
    Result<void> Arrange(const Geometry& geometry) noexcept override;

private:
    void SyncFieldVisibility() noexcept;
    Result<void> RefreshText() noexcept;
    void HandleTextChanged(StringView text) noexcept;
    void HandleTextCommitted(StringView text, TextCommitReason reason) noexcept;
    Result<SharedPtr<Element>> BuildToolTip() noexcept;
    NumericFormatOptions FormatOptions() const noexcept;

    Allocator& allocator_;
    SharedPtr<TextShaper> shaper_;
    Platform platform_;
    float fontSize_ = 0;
    SharedPtr<SpinBox> spin_;
    SharedPtr<EditableTextBox> text_;
    SharedPtr<CheckBox> toggle_;
    SharedPtr<Element> label_;
    NumericLabelLocation labelLocation_ = NumericLabelLocation::Outside;
    Vec4 labelPadding_ = NumericLabelPadding;
    Optional<double> value_{};
    String undetermined_;
    String toolTipPrefix_;
    NumericKind kind_ = NumericKind::Double;
    Optional<double> minValue_{};
    Optional<double> maxValue_{};
    int32 minDigits_ = DefaultMinNumericFractionalDigits;
    int32 maxDigits_ = DefaultMaxNumericFractionalDigits;
    NumericUnitOptions units_{};
    bool filling_ = false;

    Delegate<void(double)> changed_;
    Delegate<void(double, SpinCommitReason)> committed_;
    Delegate<void(StringView, SpinCommitReason)> undeterminedCommitted_;
    Delegate<void()> beginDrag_;
    Delegate<void(double)> endDrag_;
    Delegate<void(bool)> toggled_;
};

} // namespace rocks::ui
