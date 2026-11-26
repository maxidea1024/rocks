#pragma once

#include <UiWidget/Widget.h>
#include <UiFramework/Controls/ComboBox.h>

namespace rocks::ui {

// 객체·문자열·이름 콤보 상자의 공통 선택 수명이다. -1은 선택 해제이며 나머지는 0부터 센다.
// SetOptionFactory로 선택 표시와 메뉴 행 각각의 모양을 지정할 수 있다. 기본 표시는 위젯
// 트리의 텍스트 셰이퍼를 사용한다. 사용자 입력과 SetSelectedIndex는 같은 선택 콜백을 거친다.
ROCKS_CLASS(Abstract)
class ROCKS_UIWIDGET_API ComboBoxWidgetBase : public Widget {
    ROCKS_GENERATED_BODY()
public:
    ~ComboBoxWidgetBase() noexcept;
    int32 SelectedIndex() const noexcept;
    virtual usize OptionCount() const noexcept = 0;
    Result<void> SetSelectedIndex(int32 index) noexcept;
    void SetOnSelectionChanged(Delegate<void(int32)>&& callback) noexcept;
    void SetOptionFactory(ComboBox::MakeOption&& factory) noexcept;
    Result<void> ClearSelection() noexcept;
    Result<void> RefreshOptions() noexcept;
    Result<void> Open() noexcept;
    void Close() noexcept;
    Result<void> SetMaxListHeight(float height) noexcept;
    void SetOnOpening(Delegate<void()>&& callback) noexcept;
    bool IsOpen() const noexcept;
    void ReleaseElements() noexcept override;

protected:
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept override;
    virtual Result<void> WriteLabel(ObjectRegistry& objects, usize index,
                                    String& out) const noexcept = 0;
    void OptionsChanged(int32 selection) noexcept;
    int32 SelectionAfterRemoving(usize index) const noexcept;
    bool CanEditOptions() const noexcept;

private:
    Result<SharedPtr<Element>> MakeOption(ObjectRegistry& objects, usize index,
                                          bool display) noexcept;
    ROCKS_PROPERTY(ReadOnly, Name = "selectedIndex") int32 selectedIndex_ = -1;
    ROCKS_PROPERTY(ReadOnly, Name = "maxListHeight") float maxListHeight_ = 450;
    ROCKS_NATIVE() Delegate<void()> opening_;
    ROCKS_NATIVE() ComboBox::MakeOption factory_;
    ROCKS_NATIVE() Delegate<void(int32)> selectionChanged_;
    ROCKS_NATIVE() bool generating_ = false;
};

} // namespace rocks::ui
