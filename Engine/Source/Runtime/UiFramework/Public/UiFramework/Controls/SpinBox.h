#pragma once

#include <UiCore/Element.h>
#include <UiCore/FocusCause.h>
#include <UiCore/Styles/CoreStyles.h>
#include <UiFramework/Controls/TextCommitReason.h>
#include <UiFramework/Application/Platform.h>
#include <UiFramework/Numeric/NumericUnitFormat.h>

namespace rocks::ui {

class EditableText;
class TextShaper;

// 숫자 입력 확정 사유다(Docs/Design/UiToolkit 24.5절 — UE ECommitMethod·ETextCommit). Enter·
// FocusLost는 글자 입력 확정, Spin은 끌기 놓음, ArrowKey는 위아래 키다.
enum class SpinCommitReason : uint8 {
    Enter,
    FocusLost,
    Spin,
    ArrowKey,
    // 끝 표지다(값 개수 — Widget 층 미러가 개수를 대조한다).
    Count
};

// 배수 키 기본값이다(UE SSpinBox — Shift ×10·Ctrl ×0.1·Shift+Alt ×100·Ctrl+Alt ×0.01).
inline constexpr float SpinShiftMultiplier = 10.0f;
inline constexpr float SpinCtrlMultiplier = 0.1f;
inline constexpr float SpinShiftAltMultiplier = 100.0f;
inline constexpr float SpinCtrlAltMultiplier = 0.01f;
// 내용 여백 기본값이다(UE SSpinBox ContentPadding (2, 1)).
inline constexpr Vec4 SpinBoxContentPadding{2.0f, 1.0f, 2.0f, 1.0f};

// 숫자 입력(스핀 상자)이다(UE SSpinBox — Docs/Design/UiToolkit 24.5절). 칸 안을 좌우로 끌면 값이
// 바뀌고(끄는 동안 커서를 숨기고 놓으면 되돌린다 — 23.5절), 끌지 않고 놓으면 글자 모드로 들어가
// 수식·단위를 칠 수 있다. 슬라이더 범위가 있으면 값 비율만큼 채움 막대를 그린다. 위아래 키는 한
// 단계, 글자 모드 초점에서 휠도 바꾼다. 값은 double이고 형식(NumericKind)이 반올림·범위를 정한다.
//
// 자식은 편집 텍스트 하나다: 표시 모드는 읽기 전용·HitTestInvisible·표시 글자, 글자 모드는 편집
// 가능· 초점·전체 선택. SetOnChanged는 사용자 조작과 끌기 중 값마다, SetOnCommitted는 확정마다
// 온다. SetValue는 모델이 보낸 값이라 알리지 않는다.
//
// 사용 예:
//   auto spin = SharedPtr<SpinBox>::TryMake(allocator, allocator, shaper, platform);
//   (void)spin.Value()->Initialize(NormalFont.size);
//   (void)spin.Value()->SetSliderRange(0.0, 100.0);
//   spin.Value()->SetOnCommitted(Delegate<void(double, SpinCommitReason)>::CreateLambda(
//       [](double value, SpinCommitReason) noexcept { Apply(value); }));
class ROCKS_UIFRAMEWORK_API SpinBox final : public Element {
    ROCKS_UI_ELEMENT(SpinBox, Element)

public:
    // 표시·편집 글자를 바꾸는 delegate다. 글자를 쓰고 true면 그 글자, false면 기본 글자다(UE
    // OnGetDisplayValue·OnEditEditableText).
    using TextOverride = Delegate<bool(double value, String& out)>;

    SpinBox(Allocator& allocator, SharedPtr<TextShaper> shaper, Platform platform) noexcept;
    ~SpinBox() noexcept override;

    // 편집기 자식을 만들고 지금 값을 보인다. 한 번만 부른다. 실패면(할당·셰이퍼 없음) 그 오류다.
    Result<void> Initialize(float fontSize) noexcept;

    // 모델 값을 반영한다(형식으로 반올림·값 범위로 자른다, 알리지 않는다). 유한하지 않으면
    // InvalidArgument다.
    Result<void> SetValue(double value) noexcept;
    double Value() const noexcept;
    void SetNumericKind(NumericKind kind) noexcept;
    NumericKind GetNumericKind() const noexcept;
    // 값 범위다(비어 있으면 형식 범위 — UE MinValue·MaxValue). 최소 > 최대·비유한이면
    // InvalidArgument.
    Result<void> SetValueRange(Optional<double> minimum, Optional<double> maximum) noexcept;
    // 슬라이더 범위다(비어 있으면 값 범위를 따른다 — UE MinSliderValue·MaxSliderValue). 둘 다
    // 있거나 값 범위가 둘 다 있으면 채움 막대를 그린다(UE bUnlimitedSpinRange).
    Result<void> SetSliderRange(Optional<double> minimum, Optional<double> maximum) noexcept;
    // 한 단계(0이면 맞추지 않음 — UE Delta)다. 음수·비유한이면 InvalidArgument다.
    Result<void> SetDelta(double delta) noexcept;
    // 글자 입력 확정도 Delta에 맞춘다(UE AlwaysUsesDeltaSnap).
    void SetAlwaysUsesDeltaSnap(bool enabled) noexcept;
    // 소수 자리(UE MinFractionalDigits·MaxFractionalDigits — 기본 1·6)다. 범위 밖이면
    // InvalidArgument.
    Result<void> SetFractionalDigits(int32 minimum, int32 maximum) noexcept;
    void SetSliderEnabled(bool enabled) noexcept;
    // 배수 키다(Shift·Ctrl·Shift+Alt·Ctrl+Alt). 0 이하·비유한이면 InvalidArgument다.
    Result<void> SetMultipliers(float shift, float ctrl, float shiftAlt, float ctrlAlt) noexcept;
    // 범위가 없을 때 Delta로 끄는 감도(px당 Delta^지수 — UE LinearDeltaSensitivity, 0이면 끔)다.
    void SetLinearDeltaSensitivity(int32 pixels) noexcept;
    // 슬라이더 지수와 중립값(UE SliderExponent·SliderExponentNeutralValue)이다. 0 이하·비유한이면
    // InvalidArgument다.
    Result<void> SetSliderExponent(float exponent, Optional<double> neutral = {}) noexcept;
    void SetWheelEnabled(bool enabled) noexcept;
    // 휠 한 칸이다(비어 있으면 기본 단계 — UE WheelStep).
    void SetWheelStep(Optional<double> step) noexcept;
    // 글자를 칠 때마다 숫자면 값을 알린다(UE BroadcastValueChangesPerKey).
    void SetBroadcastValueChangesPerKey(bool enabled) noexcept;
    Result<void> SetMinDesiredWidth(float width) noexcept;
    // 칸의 단위다(24.4절 — 표시 "100 cm", 입력 "1m").
    void SetUnits(const NumericUnitOptions& units) noexcept;
    void SetClearKeyboardFocusOnCommit(bool enabled) noexcept;
    void SetSelectAllTextOnCommit(bool enabled) noexcept;
    void SetRevertTextOnEscape(bool enabled) noexcept;
    void SetDisplayTextOverride(TextOverride&& callback) noexcept;
    void SetEditTextOverride(TextOverride&& callback) noexcept;
    // 스타일이다. 부르지 않으면 활성 앱 스타일 "SpinBox"(없으면 DefaultSpinBoxStyle)를 따른다.
    void SetSpinBoxStyle(const SpinBoxStyle& style) noexcept;
    const SpinBoxStyle& GetSpinBoxStyle() const noexcept;

    void SetOnChanged(Delegate<void(double)>&& callback) noexcept;
    void SetOnCommitted(Delegate<void(double, SpinCommitReason)>&& callback) noexcept;
    void SetOnDragBegin(Delegate<void()>&& callback) noexcept;
    void SetOnDragEnd(Delegate<void(double)>&& callback) noexcept;

    // 글자 모드인지·끄는 중인지다.
    bool IsEditing() const noexcept;
    bool IsDragging() const noexcept;
    // 채움 막대 비율(0–1, 지수 반영)이다. 범위가 없으면 비어 있다(시험·진단).
    Optional<float> FillFraction() const noexcept;
    // 편집기 자식이다(Initialize 전에는 nullptr).
    EditableText* Editor() const noexcept;

    AccessibleRole GetAccessibleRole() const noexcept override;
    bool SupportsKeyboardFocus() const noexcept override;
    void OnFocusReceived(uint32 user) noexcept override;
    KeyReply OnKeyEvent(const KeyEvent& key) noexcept override;
    PointerReply OnPointerEvent(const PointerEvent& event) noexcept override;
    void OnPointerCaptureLost(PointerSource source, uint32 user, uint32 pointer) noexcept override;
    void OnMouseEnter(uint32 user) noexcept override;
    void OnMouseLeave(uint32 user) noexcept override;
    // 포인터가 위에 있는지다(올림 바탕·채움).
    bool IsHovered() const noexcept;
    input::MouseCursor QueryMouseCursor(Vec2 position) const noexcept override;
    Vec2 ComputeDesiredSize(float scale) const noexcept override;
    Result<void> Arrange(const Geometry& geometry) noexcept override;
    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;

private:
    void ApplyStyles(const UiHostServices& host) noexcept override;
    void ApplyStyle(const SpinBoxStyle& style) noexcept;

    double MinValue() const noexcept;
    double MaxValue() const noexcept;
    double MinSliderValue() const noexcept;
    double MaxSliderValue() const noexcept;
    bool IsSpinRangeUnlimited() const noexcept;
    // 기본 단계(범위 ≤ 10이면 0.1, 아니면 1) × 배수다(UE GetDefaultStepSize).
    double DefaultStep(float multiplier) const noexcept;
    float Multiplier(bool shift, bool control, bool alt) const noexcept;
    NumericFormatOptions FormatOptions() const noexcept;

    enum class CommitMethod : uint8 {
        Spin,
        SpinMultiplier,
        ArrowKey,
        TypeIn,
        Code
    };

    // UE CommitValue: 범위로 자르고, Spin·ArrowKey(·AlwaysUsesDeltaSnap)는 Delta에 맞춘다.
    void CommitValue(double value, double spinValue, CommitMethod method,
                     Optional<SpinCommitReason> committed) noexcept;
    Result<void> RefreshText() noexcept;
    void EnterTextMode(uint32 user, FocusCause cause) noexcept;
    void ExitTextMode() noexcept;
    void HandleTextChanged(StringView text) noexcept;
    void HandleTextCommitted(StringView text, TextCommitReason reason) noexcept;
    void ResetDrag() noexcept;
    bool ToLocalDelta(const PointerEvent& event, float& dx) noexcept;

    Allocator& allocator_;
    SharedPtr<TextShaper> shaper_;
    Platform platform_;
    SharedPtr<EditableText> editor_;
    SpinBoxStyle style_ = DefaultSpinBoxStyle();
    bool styleExplicit_ = false;

    NumericKind kind_ = NumericKind::Double;
    double value_ = 0;
    // 끄는 동안의 값이다(Delta에 맞추기 전 — UE InternalValue).
    double internal_ = 0;
    Optional<double> minValue_{};
    Optional<double> maxValue_{};
    Optional<double> minSlider_{};
    Optional<double> maxSlider_{};
    double delta_ = 0;
    bool alwaysSnap_ = false;
    int32 minDigits_ = DefaultMinNumericFractionalDigits;
    int32 maxDigits_ = DefaultMaxNumericFractionalDigits;
    bool sliderEnabled_ = true;
    float shiftMultiplier_ = SpinShiftMultiplier;
    float ctrlMultiplier_ = SpinCtrlMultiplier;
    float shiftAltMultiplier_ = SpinShiftAltMultiplier;
    float ctrlAltMultiplier_ = SpinCtrlAltMultiplier;
    int32 linearSensitivity_ = 0;
    float exponent_ = 1;
    Optional<double> neutral_{};
    bool wheelEnabled_ = true;
    Optional<double> wheelStep_{};
    bool perKey_ = false;
    float minDesiredWidth_ = 0;
    NumericUnitOptions units_{};
    bool clearFocusOnCommit_ = false;
    TextOverride displayText_;
    TextOverride editText_;

    Delegate<void(double)> changed_;
    Delegate<void(double, SpinCommitReason)> committed_;
    Delegate<void()> beginDrag_;
    Delegate<void(double)> endDrag_;

    // 끌기 상태다(UE PointerDraggingSliderIndex·DistanceDragged·PreDragValue).
    bool tracking_ = false;
    bool dragging_ = false;
    bool editing_ = false;
    bool filtering_ = false;
    bool hovered_ = false;
    PointerSource source_ = PointerSource::Mouse;
    uint32 user_ = 0;
    uint32 pointer_ = 0;
    float distance_ = 0;
    float lastX_ = 0;
    double preDrag_ = 0;
    // 초점을 받은 사용자다(확정 때 초점 지우기).
    uint32 focusUserForCommit_ = 0;
};

} // namespace rocks::ui
