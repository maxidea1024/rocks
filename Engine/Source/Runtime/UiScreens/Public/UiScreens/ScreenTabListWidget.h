#pragma once
#include <Core/Text/Name.h>
#include <UiFramework/Controls/Button.h>
#include <UiWidget/WidgetSwitcherWidget.h>

namespace rocks::ui {

// 이름으로 탭을 등록하고 연결된 스위처에서 해당 콘텐츠를 선택한다. 탭 버튼과 화면 콘텐츠는 별도
// 트리에 배치한다. 기본 버튼은 위젯 트리의 셰이퍼로 탭 이름을 표시하며 팩토리로 교체할 수 있다.
ROCKS_CLASS()
class ROCKS_UISCREENS_API ScreenTabListWidget final : public Widget {
    ROCKS_GENERATED_BODY()
public:
    using ButtonFactory = Delegate<Result<SharedPtr<Button>>(Name)>;
    explicit ScreenTabListWidget(Allocator& allocator) noexcept;
    Result<void> SetLinkedSwitcher(ObjectRegistry& objects, ObjectHandle switcher) noexcept;
    Result<void> RegisterTab(ObjectRegistry& objects, Name id, ObjectHandle page) noexcept;
    Result<void> RemoveTab(ObjectRegistry& objects, Name id) noexcept;
    Result<void> SelectTab(ObjectRegistry& objects, Name id) noexcept;
    Result<void> StepTab(ObjectRegistry& objects, bool forward) noexcept;
    Result<void> SetTabEnabled(Name id, bool enabled) noexcept;
    Result<void> SetTabVisible(Name id, bool visible) noexcept;
    Result<void> SetButtonFactory(ObjectRegistry& objects, ButtonFactory&& factory) noexcept;
    Name SelectedTab(ObjectRegistry& objects) const noexcept;
    Span<const Name> Tabs() const noexcept;
    void SetOnTabSelected(Delegate<void(Name)>&& callback) noexcept;
    void ReleaseElements() noexcept override;

protected:
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept override;

private:
    Result<void> Subscribe(ObjectRegistry& objects, const SharedPtr<Element>& host) noexcept;
    void Unsubscribe(ObjectRegistry& objects) noexcept;
    Optional<usize> FindTab(Name id) const noexcept;
    Optional<uint32> PageIndex(ObjectRegistry& objects, ObjectHandle page) const noexcept;
    Result<void> Rebuild(ObjectRegistry& objects, Element& host) noexcept;
    Result<SharedPtr<Button>> MakeButton(ObjectRegistry& objects, Name id) noexcept;
    void UpdateButtons(ObjectRegistry& objects, Element& host) noexcept;
    uint8 Flags(usize index) const noexcept;
    Result<void> SetFlag(Name id, uint8 bit, bool enabled) noexcept;
    ROCKS_PROPERTY(ReadOnly, Name = "switcher") ObjectRef<WidgetSwitcherWidget> switcher_;
    ROCKS_PROPERTY(ReadOnly, Name = "tabIds") Array<Name> tabIds_;
    ROCKS_PROPERTY(ReadOnly, Name = "contents") ObjectRefArray<Widget> contents_;
    ROCKS_NATIVE() Array<uint8> flags_;
    ROCKS_NATIVE() DelegateHandle activeChanged_ {};
    ROCKS_NATIVE() bool changing_ = false;
    ROCKS_NATIVE() ButtonFactory factory_;
    ROCKS_NATIVE() Delegate<void(Name)> selected_;
    ROCKS_NATIVE() ObjectRegistry* registry_ = nullptr;
};
} // namespace rocks::ui
