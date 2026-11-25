#pragma once

#include <UiFramework/Button.h>

namespace rocks::ui {

enum class CheckState : uint8 {
    Unchecked,
    Checked,
    Indeterminate
};

// 활성화할 때 체크 상태를 바꾸고 변경된 값을 통지한다.
class ROCKS_UIFRAMEWORK_API CheckBox : public Button {
public:
    AccessibleRole GetAccessibleRole() const noexcept override;
    explicit CheckBox(Allocator& allocator) noexcept;

    CheckState GetState() const noexcept;
    Result<void> SetState(CheckState state) noexcept;
    void SetOnChanged(Delegate<void(CheckState)>&& callback) noexcept;
    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;

protected:
    void OnActivated() noexcept override;

private:
    CheckState state_ = CheckState::Unchecked;
    Delegate<void(CheckState)> onChanged_;
};

} // namespace rocks::ui
