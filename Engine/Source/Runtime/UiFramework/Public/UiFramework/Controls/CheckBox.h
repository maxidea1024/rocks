#pragma once

#include <UiFramework/Controls/Button.h>

namespace rocks::ui {

enum class CheckState : uint8 {
    Unchecked,
    Checked,
    Indeterminate,
    // 끝 표지다(값 개수 — Widget 층 미러가 개수를 대조한다).
    Count
};

// 활성화할 때 체크 상태를 바꾸고 변경된 값을 통지한다. 모양은 CheckBoxStyle(UE "Checkbox")이다.
// 체크 상자 형식은 왼쪽에 상자, 그 오른쪽에 내용(레이블)을 둔다. 토글 형식(UE
// "ToggleButtonCheckbox")은 상태 이미지를 바탕으로 그리고 내용을 여백 안에 둔다. 라디오는 체크 상자
// 형식의 "RadioButton" 스타일이고, 묶음(하나만 체크)은 호출자가 상태 delegate로 만든다(UE와 같다 —
// Docs/Design/UiToolkit 23.3절). 버튼 스타일은 쓰지 않는다.
//
// 사용 예:
//   auto check = SharedPtr<CheckBox>::TryMake(allocator, allocator);
//   check.Value()->TryAddChild(label);                 // 상자 오른쪽 레이블
//   check.Value()->SetOnChanged(Delegate<void(CheckState)>::CreateLambda(...));
class ROCKS_UIFRAMEWORK_API CheckBox : public Button {
    ROCKS_UI_ELEMENT(CheckBox, Button)

public:
    AccessibleRole GetAccessibleRole() const noexcept override;
    explicit CheckBox(Allocator& allocator) noexcept;

    // 상자 크기·여백이 음수·비유한이거나 색이 잘못되면 InvalidArgument다. 명시 스타일이다 —
    // 부르지 않으면 활성 앱 스타일의 "Checkbox"(없으면 DefaultCheckBoxStyle)다(20.11절).
    Result<void> SetCheckBoxStyle(const CheckBoxStyle& style) noexcept;
    const CheckBoxStyle& GetCheckBoxStyle() const noexcept;
    ColorRef GetForegroundColor() const noexcept override;

    CheckState GetState() const noexcept;
    Result<void> SetState(CheckState state) noexcept;
    void SetOnChanged(Delegate<void(CheckState)>&& callback) noexcept;
    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;

protected:
    void OnActivated() noexcept override;

private:
    void ApplyStyles(const UiHostServices& host) noexcept override;
    Result<void> ApplyCheckBoxStyle(const CheckBoxStyle& style) noexcept;
    // 지금 상태(해제·체크·미정 × 보통·올림·눌림)의 상태 이미지다.
    const StyleBrush& CurrentStateImage() const noexcept;
    // 형식에 맞게 Button 내용 여백을 맞춘다(체크 상자: 상자 오른쪽, 토글: 사방 여백).
    void UpdateContentPadding() noexcept;

    CheckBoxStyle checkStyle_ = DefaultCheckBoxStyle();
    bool checkStyleExplicit_ = false;
    CheckState state_ = CheckState::Unchecked;
    Delegate<void(CheckState)> onChanged_;
};

} // namespace rocks::ui
