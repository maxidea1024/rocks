#pragma once

#include <UiFramework/Button.h>

namespace rocks::ui {

enum class CheckState : uint8 {
    Unchecked,
    Checked,
    Indeterminate
};

// 활성화할 때 체크 상태를 바꾸고 변경된 값을 통지한다. 모양은 CheckBoxStyle(UE "Checkbox")이다:
// 왼쪽에 상자, 그 오른쪽에 내용(레이블)을 둔다. 버튼 스타일은 쓰지 않는다.
//
// 사용 예:
//   auto check = SharedPtr<CheckBox>::TryMake(allocator, allocator);
//   check.Value()->TryAddChild(label);                 // 상자 오른쪽 레이블
//   check.Value()->SetOnChanged(Delegate<void(CheckState)>::CreateLambda(...));
class ROCKS_UIFRAMEWORK_API CheckBox : public Button {
public:
    AccessibleRole GetAccessibleRole() const noexcept override;
    explicit CheckBox(Allocator& allocator) noexcept;

    // 상자 크기·여백이 음수·비유한이거나 색이 잘못되면 InvalidArgument다.
    Result<void> SetCheckBoxStyle(const CheckBoxStyle& style) noexcept;
    const CheckBoxStyle& GetCheckBoxStyle() const noexcept;
    ColorRef GetForegroundColor() const noexcept override;

    CheckState GetState() const noexcept;
    Result<void> SetState(CheckState state) noexcept;
    void SetOnChanged(Delegate<void(CheckState)>&& callback) noexcept;
    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;
    Result<void> Arrange(const Geometry& geometry) noexcept override;

protected:
    void OnActivated() noexcept override;

private:
    // 상자 오른쪽에 내용을 두도록 Button 내용 여백을 맞춘다.
    void UpdateContentPadding() noexcept;

    CheckBoxStyle checkStyle_ = DefaultCheckBoxStyle();
    CheckState state_ = CheckState::Unchecked;
    Delegate<void(CheckState)> onChanged_;
};

} // namespace rocks::ui
