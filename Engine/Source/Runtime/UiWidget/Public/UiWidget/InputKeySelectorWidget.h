#pragma once

#include <UiFramework/Application/Commands.h>
#include <UiWidget/Widget.h>

namespace rocks::ui {

// 게임 설정 화면에서 키보드 단축키를 변경한다. 키 이름과 보조 키는 애셋과 화면 재구성을
// 거쳐도 유지된다. 표시에는 WidgetTree의 텍스트 셰이퍼를 사용한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API InputKeySelectorWidget final : public Widget {
    ROCKS_GENERATED_BODY()
public:
    explicit InputKeySelectorWidget(Allocator& allocator) noexcept;
    Result<void> SetSelectedKey(KeyChord chord) noexcept;
    KeyChord SelectedKey() const noexcept;
    void SetAllowModifiers(bool enabled) noexcept;
    void SetAllowGamepad(bool enabled) noexcept;
    void SetOnSelected(Delegate<void(KeyChord)>&& callback) noexcept;

protected:
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept override;

private:
    Result<void> StoreChord(KeyChord chord) noexcept;
    ROCKS_PROPERTY(ReadOnly, Name = "keyName") String keyName_;
    ROCKS_PROPERTY(ReadOnly, Name = "shift") bool shift_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "control") bool control_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "alt") bool alt_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "super") bool super_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "allowModifiers") bool allowModifiers_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "allowGamepad") bool allowGamepad_ = false;
    ROCKS_NATIVE() Delegate<void(KeyChord)> selected_;
};

} // namespace rocks::ui
