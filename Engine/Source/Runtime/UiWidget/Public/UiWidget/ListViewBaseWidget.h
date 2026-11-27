#pragma once

#include <UiFramework/Lists/ListView.h>
#include <UiWidget/Widget.h>

namespace rocks::ui {

// 목록·타일·트리의 공통 모델과 가상화된 행을 제어한다. 인덱스는 현재 펼쳐진 항목의
// 논리 순서이며, 선택은 행이 회수되거나 화면 밖으로 나간 뒤에도 모델의 키로 유지한다.
ROCKS_CLASS(Abstract)
class ROCKS_UIWIDGET_API ListViewBaseWidget : public Widget {
    ROCKS_GENERATED_BODY()
public:
    // 이벤트 필드가 allocator를 받는다(EventReflection ED14).
    explicit ListViewBaseWidget(Allocator& allocator) noexcept;
    // 화면 트리에서 분리하고 ReleaseElements를 호출한 뒤 소스를 교체할 수 있다.
    Result<void> SetSource(Application& app, const SharedRef<TableRowSource>& source) noexcept;
    void Refresh() noexcept;
    Result<void> SetScrollOffset(double offset) noexcept;
    double ScrollOffset() const noexcept;
    Result<void> ScrollToStart() noexcept;
    Result<void> ScrollToEnd() noexcept;
    // 해당 행이 아직 만들어지지 않았으면 다음 배치에서 필요한 만큼만 스크롤한다.
    Result<void> ScrollIndexIntoView(uint32 index) noexcept;
    Result<void> NavigateToIndex(uint32 index, uint32 user = 0) noexcept;
    Result<void> SelectIndex(uint32 index) noexcept;
    // Multi는 Ctrl로 항목을 전환하고 Shift로 마지막 선택 지점부터 범위를 선택한다.
    void SetSelectionMode(ListSelectionMode mode) noexcept;
    Result<void> SetItemSelection(uint64 key, bool selected) noexcept;
    Span<const uint64> SelectedKeys() const noexcept;
    bool IsItemVisible(uint64 key) const noexcept;
    bool IsRefreshPending() const noexcept;
    Result<void> ScrollItemIntoView(uint64 key) noexcept;
    Result<void> NavigateToItem(uint64 key, uint32 user = 0) noexcept;
    void CancelScrollIntoView() noexcept;
    void SetScrollIntoViewAlignment(ScrollIntoViewAlignment alignment) noexcept;
    // 사건은 리플렉션 이벤트다(Docs/Design/UiToolkit 21.5a절): C++는 TryAddLambda·TryAdd(native
    // 구독), 에셋·Sparks는 TryBind(리플렉션 함수)로 받는다. 구독은 여럿일 수 있다.
    Event<uint64>& OnItemClicked() noexcept;
    Event<>& OnSelectionChanged() noexcept;
    Event<uint64>& OnItemScrolledIntoView() noexcept;
    void ReleaseElements() noexcept override;
    void ClearSelection() noexcept;
    Optional<uint64> SelectedKey() const noexcept;
    void SetPointerScrolling(bool enabled) noexcept;
    void SetSelectOnNavigation(bool enabled) noexcept;
    Result<void> SetWheelMultiplier(float multiplier) noexcept;

protected:
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept override;
    void ApplyProperties(Element& element) noexcept override;
    virtual Result<SharedPtr<Element>>
    CreateView(Allocator& allocator, Application& app,
               const SharedRef<TableRowSource>& source) noexcept = 0;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "pointerScrolling") bool pointerScrolling_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "selectOnNavigation") bool selectOnNavigation_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "wheelMultiplier") float wheelMultiplier_ = 1;
    ROCKS_PROPERTY(ReadOnly, Name = "selectionMode")
    uint8 selectionMode_ = static_cast<uint8>(ListSelectionMode::Single);
    ROCKS_PROPERTY(ReadOnly, Name = "scrollAlignment")
    uint8 scrollAlignment_ = static_cast<uint8>(ScrollIntoViewAlignment::Nearest);
    ROCKS_PROPERTY(Transient, Name = "itemClicked", Params = "key") Event<uint64> itemClicked_;
    ROCKS_PROPERTY(Transient, Name = "selectionChanged") Event<> selectionChanged_;
    ROCKS_PROPERTY(Transient, Name = "itemScrolledIntoView", Params = "key")
    Event<uint64> itemScrolled_;
    ROCKS_NATIVE() Application* app_ = nullptr;
    ROCKS_NATIVE() SharedPtr<TableRowSource> source_;
};

} // namespace rocks::ui
