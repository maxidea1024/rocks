#pragma once
#include <UiFramework/Scrolling/ScrollAlignment.h>

#include <Core/Optional.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Lists/TableRowSource.h>

namespace rocks::ui {

enum class ListSelectionMode : uint8 {
    None,
    Single,
    SingleToggle,
    Multi
};

// 고정 높이 행을 사용한다. 소스와 Application은 뷰보다 오래 살아야 한다.
class ROCKS_UIFRAMEWORK_API ListView : public Element {
public:
    AccessibleRole GetAccessibleRole() const noexcept override;
    ListView(Allocator& allocator, Application& app, TableRowSource& source) noexcept;
    // 공유 소스를 넘기면 뷰가 마지막 행을 반납할 때까지 모델을 붙잡는다.
    ListView(Allocator& allocator, Application& app,
             const SharedRef<TableRowSource>& source) noexcept;
    ~ListView() noexcept override;
    Result<void> SetRowHeight(float height) noexcept;
    Result<void> SetScrollOffset(double offset) noexcept;
    double ScrollOffset() const noexcept;
    double MaxScrollOffset() const noexcept;
    void SetPointerScrolling(bool enabled) noexcept;
    void SetSelectOnNavigation(bool enabled) noexcept;
    Result<void> SetWheelMultiplier(float multiplier) noexcept;
    Result<void> SelectIndex(uint32 index) noexcept;
    Result<void> SetItemSelection(uint64 key, bool selected) noexcept;
    void SetSelectionMode(ListSelectionMode mode) noexcept;
    Span<const uint64> SelectedKeys() const noexcept;
    bool IsSelected(uint64 key) const noexcept;
    bool IsItemVisible(uint64 key) const noexcept;
    Result<uint32> FindIndex(uint64 key) noexcept;
    void SetOnItemClicked(Delegate<void(uint64)>&& callback) noexcept;
    void SetOnSelectionChanged(Delegate<void()>&& callback) noexcept;
    void SetOnItemScrolledIntoView(Delegate<void(uint64)>&& callback) noexcept;
    void SetScrollIntoViewAlignment(ScrollIntoViewAlignment alignment) noexcept;
    void CancelScrollIntoView() noexcept;
    bool IsRefreshPending() const noexcept;
    void ClearSelection() noexcept;
    Optional<uint64> SelectedKey() const noexcept;
    // 아직 표시되지 않은 행은 다음 배치에서 생성한다. 키보드 이동은 사용자의 포커스를 함께 옮긴다.
    Result<void> ScrollToEnd() noexcept;
    Result<void> ScrollIndexIntoView(uint32 index) noexcept;
    Result<void> NavigateToIndex(uint32 index, uint32 user) noexcept;
    bool SupportsKeyboardFocus() const noexcept override;
    Result<void> OnPaintOverlay(PaintList& list, int32 layer) const noexcept override;
    PointerReply OnTouchPan(const PointerEvent& move, Vec2 origin) noexcept override;
    void OnPointerCaptureLost(PointerSource source, uint32 user, uint32 pointer) noexcept override;
    void OnPointerReleaseInPath(const PointerEvent& event) noexcept override;
    PointerReply OnPreviewPointerDown(const PointerEvent& event) noexcept override;
    bool OnKeyEvent(uint32 user, const input::InputEvent& event) noexcept override;
    virtual void Refresh() noexcept;
    Result<void> Arrange(const Geometry& geometry) noexcept override;
    PointerReply OnPointerEvent(const PointerEvent& event) noexcept override;
    Result<void> TryAddChild(const SharedPtr<Element>& child) noexcept override;
    bool RemoveChild(Element& child) noexcept override;
    Vec2 ComputeDesiredSize(float scale) const noexcept override;
    // 행을 생성하지 않고 현재 표시 중인 항목만 조회한다.
    TableItem ItemAtRow(usize row) const noexcept;

protected:
    virtual uint32 Columns(float width) const noexcept;
    virtual Result<void> PrepareItems() noexcept;
    virtual uint32 ItemCount() const noexcept;
    virtual TableItem ItemAt(uint32 index) const noexcept;
    TableRowSource& source_;
    Allocator& allocator_;

private:
    Result<void> RequestIndex(uint32 index, Optional<uint32> focusUser,
                              bool updateSelection = true) noexcept;
    Result<void> ReplaceSelection(uint64 key) noexcept;
    void SelectionChanged() noexcept;
    void SelectFromPointer(uint32 index, uint64 key, uint32 user) noexcept;
    Result<void> SelectRange(uint32 first, uint32 last) noexcept;
    Result<void> AttachRow(uint32 index, bool prepend) noexcept;
    void RecycleRow(usize row) noexcept;
    SharedPtr<TableRowSource> retainedSource_;
    Application& app_;
    Array<TableItem> items_;
    Array<SharedPtr<Element>> spare_;
    uint32 first_ = 0;
    float rowHeight_ = 24;
    double offset_ = 0;
    double maxOffset_ = 0;
    bool refresh_ = true;
    bool pointerScrolling_ = true;
    bool selectOnNavigation_ = true;
    float wheelMultiplier_ = 1;
    Array<uint64> selected_;
    ListSelectionMode selectionMode_ = ListSelectionMode::Single;
    ScrollIntoViewAlignment scrollAlignment_ = ScrollIntoViewAlignment::Nearest;
    Optional<uint32> rangeAnchor_;
    struct PressedRow {
        uint32 user;
        uint32 pointer;
        PointerSource source;
        uint64 key;
    };
    Array<PressedRow> presses_;
    uint32 panUser_ = 0;
    uint32 panPointer_ = 0;
    float panStart_ = 0;
    double panOffset_ = 0;
    bool panning_ = false;
    Delegate<void(uint64)> itemClicked_;
    Delegate<void()> selectionChanged_;
    Delegate<void(uint64)> itemScrolled_;
    Optional<uint32> requestedIndex_;
    Optional<uint32> focusUser_;
    Optional<uint32> navigationIndex_;
};

} // namespace rocks::ui
