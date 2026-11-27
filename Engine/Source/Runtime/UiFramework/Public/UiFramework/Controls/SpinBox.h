#pragma once

#include <UiFramework/Layout/Frame.h>

namespace rocks::ui {

// 숫자 입력의 소수 자릿수 상한이다(형식 문자열 "{:.Nf}"의 N은 한 자리).
inline constexpr int32 MaxSpinBoxFractionalDigits = 9;

// 좌우 드래그와 위·아래 방향키로 숫자를 조정한다. 클릭 후 숫자를 입력하고 Enter로
// 확정하거나 Escape로 취소할 수 있다. 값의 범위와 한 단계의 변화량은 SetLimits로 설정한다.
// SetOnChanged는 사용자 조작만 전달한다. SetValue는 모델이 보낸 값을 반영한다.
enum class SpinCommitReason : uint8 {
    Enter,
    FocusLost
};
class ROCKS_UIFRAMEWORK_API SpinBox final : public Frame {
public:
    AccessibleRole GetAccessibleRole() const noexcept override;
    using Super = Frame;
    struct Args {
        float value = 0;
        float minimum = 0;
        float maximum = 100;
        float step = 1;
        ElementChild content{};
        Bindable<LinearColor> color{LinearColor{0.12f, 0.12f, 0.12f, 1}};
        Delegate<Result<void>(StringView)> caption{};
        Delegate<void(float)> onChanged{};
        ElementArgs common{};
    };

    Result<void> Construct(Args&& args) noexcept;
    explicit SpinBox(Allocator& allocator) noexcept;
    Result<void> SetLimits(float minimum, float maximum, float step) noexcept;
    Result<void> SetValue(float value) noexcept;
    Result<void> SetFractionalDigits(int32 minimum, int32 maximum) noexcept;
    void SetSliderEnabled(bool enabled) noexcept;
    void SetSnapToStep(bool enabled) noexcept;
    void SetOnCommitted(Delegate<void(float, SpinCommitReason)>&& callback) noexcept;
    void SetOnDragBegin(Delegate<void()>&& callback) noexcept;
    void SetOnDragEnd(Delegate<void(float)>&& callback) noexcept;
    float Value() const noexcept;
    bool IsEditing() const noexcept;
    Result<void> SetCaption(Delegate<Result<void>(StringView)>&& callback) noexcept;
    void SetOnChanged(Delegate<void(float)>&& callback) noexcept;
    bool SupportsKeyboardFocus() const noexcept override;
    KeyReply OnKeyEvent(const KeyEvent& key) noexcept override;
    void OnFocusLost(uint32 user) noexcept override;
    PointerReply OnPointerEvent(const PointerEvent& event) noexcept override;
    void OnPointerCaptureLost(PointerSource source, uint32 user, uint32 pointer) noexcept override;

private:
    Result<void> ShowNumber(float value) noexcept;
    void Commit(SpinCommitReason reason) noexcept;
    bool Change(float value) noexcept;
    void BeginEdit() noexcept;
    void CancelEdit() noexcept;
    Allocator& allocator_;
    Delegate<Result<void>(StringView)> caption_;
    Delegate<void(float)> changed_, endDrag_;
    Delegate<void()> beginDrag_;
    Delegate<void(float, SpinCommitReason)> committed_;
    bool sliderEnabled_ = true, snapToStep_ = false;
    int32 minDigits_ = 0, maxDigits_ = -1;
    float minimum_ = 0, maximum_ = 100, step_ = 1, value_ = 0;
    float dragX_ = 0, dragValue_ = 0;
    PointerSource source_ = PointerSource::Mouse;
    uint32 pointer_ = 0, user_ = 0;
    bool tracking_ = false, dragged_ = false, editing_ = false;
    char buffer_[64]{};
    usize length_ = 0;
};

} // namespace rocks::ui
