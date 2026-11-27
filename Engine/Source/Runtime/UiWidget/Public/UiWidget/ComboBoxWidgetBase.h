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
    // 이벤트 필드가 allocator를 받는다(EventReflection ED14).
    explicit ComboBoxWidgetBase(Allocator& allocator) noexcept;
    ~ComboBoxWidgetBase() noexcept;
    int32 SelectedIndex() const noexcept;
    virtual usize OptionCount() const noexcept = 0;
    Result<void> SetSelectedIndex(int32 index) noexcept;
    // 사건은 리플렉션 이벤트다(Docs/Design/UiToolkit 21.5a절): C++는 TryAddLambda·TryAdd(native
    // 구독), 에셋·Sparks는 TryBind(리플렉션 함수)로 받는다. 구독은 여럿일 수 있다.
    Event<int32>& OnSelectionChanged() noexcept;
    void SetOptionFactory(ComboBox::MakeOption&& factory) noexcept;
    Result<void> ClearSelection() noexcept;
    Result<void> RefreshOptions() noexcept;
    Result<void> Open() noexcept;
    void Close() noexcept;
    Result<void> SetMaxListHeight(float height) noexcept;
    Event<>& OnOpening() noexcept;
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
    ROCKS_PROPERTY(Transient, Name = "opening") Event<> opening_;
    ROCKS_NATIVE() ComboBox::MakeOption factory_;
    ROCKS_PROPERTY(Transient, Name = "selectionChanged", Params = "index")
    Event<int32> selectionChanged_;
    ROCKS_NATIVE() bool generating_ = false;
};

} // namespace rocks::ui
