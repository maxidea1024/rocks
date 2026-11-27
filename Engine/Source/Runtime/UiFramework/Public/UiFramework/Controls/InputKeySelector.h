#pragma once

#include <UiFramework/Controls/Button.h>
#include <UiFramework/Application/Commands.h>

namespace rocks::ui {

// 클릭 또는 Enter로 키 캡처를 시작한다. 다음 일반 키를 누르면 현재 보조 키까지
// KeyChord로 전달한다. Escape 또는 포커스 상실은 기존 선택을 보존한다.
// SetCaption은 화면 언어에 맞게 문자열을 셰이핑하는 콜백이다.
class ROCKS_UIFRAMEWORK_API InputKeySelector final : public Button {
public:
    using Super = Button;
    struct Args {
        KeyChord selectedKey{};
        bool allowModifiers = true;
        bool allowGamepad = false;
        ElementChild content{};
        // brush 색에 곱한다(UE ButtonColorAndOpacity).
        Bindable<LinearColor> color{LinearColor{1, 1, 1, 1}};
        Delegate<Result<void>(StringView)> caption{};
        Delegate<void(KeyChord)> onSelected{};
        ElementArgs common{};
    };

    Result<void> Construct(Args&& args) noexcept;
    explicit InputKeySelector(Allocator& allocator) noexcept;
    Result<void> SetSelectedKey(KeyChord chord) noexcept;
    KeyChord SelectedKey() const noexcept;
    void SetAllowModifiers(bool enabled) noexcept;
    // 게임패드 버튼을 선택할 수 있게 한다. 기본값에서는 키보드와 마우스 버튼을 받는다.
    void SetAllowGamepad(bool enabled) noexcept;
    bool IsSelecting() const noexcept;
    Result<void> SetCaption(Delegate<Result<void>(StringView)>&& callback) noexcept;
    void SetOnSelected(Delegate<void(KeyChord)>&& callback) noexcept;
    bool OnPreviewKeyChord(uint32 user, const input::InputEvent& event,
                           KeyChord chord) noexcept override;
    PointerReply OnPreviewPointerDown(const PointerEvent& event) noexcept override;
    PointerReply OnPointerEvent(const PointerEvent& event) noexcept override;

    KeyReply OnKeyEvent(const KeyEvent& key) noexcept override;
    void OnFocusLost(uint32 user) noexcept override;

protected:
    void OnActivated() noexcept override;

private:
    // 패드 버튼 처리다(선택 중이면 고르기, West는 지우기). 처리했으면 true다.
    bool HandleGamepadButton(uint32 user, input::GamepadButton button) noexcept;
    Result<void> RefreshCaption(KeyChord chord, bool selecting) noexcept;
    void Select(KeyChord chord) noexcept;
    void Cancel() noexcept;
    Allocator& allocator_;
    KeyChord selected_{};
    bool selecting_ = false;
    bool allowModifiers_ = true;
    bool allowGamepad_ = false;
    uint32 activationUser_ = 0;
    uint32 selectingUser_ = 0;

    Delegate<Result<void>(StringView)> caption_;
    Delegate<void(KeyChord)> selectedEvent_;
};

} // namespace rocks::ui
