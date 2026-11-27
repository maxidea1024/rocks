#pragma once

#include <UiWidget/PanelWidget.h>
#include <UiCore/StackLayout.h>
#include <UiFramework/Scrolling/ScrollView.h>

namespace rocks::ui {

// 여러 자식을 세로로 쌓아 한 번에 스크롤한다. 스크롤 위치는 배치 뒤 유효 범위로 제한된다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API ScrollBoxWidget final : public PanelWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit ScrollBoxWidget(Allocator& allocator) noexcept;
    Result<void> SetScrollOffset(float offset) noexcept;
    float ScrollOffset() const noexcept;
    // 논리 단위로 스크롤한다. 끝으로 이동하는 요청은 다음 배치에서 콘텐츠 길이가 확정된 뒤
    // 적용한다.
    void ScrollToEnd() noexcept;
    Result<void> ScrollToStart() noexcept;
    Result<void>
    ScrollWidgetIntoView(const Widget& child,
                         ScrollIntoViewAlignment alignment = ScrollIntoViewAlignment::Nearest,
                         float padding = 0) noexcept;
    float GetViewFraction() const noexcept;
    float GetViewOffsetFraction() const noexcept;
    float GetScrollOffsetOfEnd() const noexcept;
    bool IsScrolling() const noexcept;
    Result<void> SetOrientation(StackDirection direction) noexcept;
    Result<void> SetConsumeMouseWheel(ConsumeMouseWheel policy) noexcept;
    Result<void> SetWheelScrollMultiplier(float multiplier) noexcept;
    Result<void> SetScrollbarThickness(float logicalPixels) noexcept;
    void SetShowScrollbar(bool visible, bool alwaysShow = false) noexcept;
    // 여백의 성분은 왼쪽·위·오른쪽·아래 순이다. 화면 축을 바꿔도 의미를 유지한다.
    Result<void> SetScrollbarPadding(Vec4 padding) noexcept;
    Vec4 GetScrollbarPadding() const noexcept;
    void SetAlwaysShowScrollbarTrack(bool show) noexcept;
    bool IsAlwaysShowScrollbarTrack() const noexcept;
    // 양 끝에 뷰포트 한 페이지의 공간을 두어 첫 항목이나 끝 항목을 완전히 밀어낼 수 있다.
    void SetScrollPadding(bool before, bool after) noexcept;
    void SetTouchScrolling(bool enabled) noexcept;
    void SetRightDragScrolling(bool enabled) noexcept;
    // 사건은 리플렉션 이벤트다(Docs/Design/UiToolkit 21.5a절): C++는 TryAddLambda·TryAdd(native
    // 구독), 에셋·Sparks는 TryBind(리플렉션 함수)로 받는다. 구독은 여럿일 수 있다.
    Event<float>& OnUserScrolled() noexcept;

protected:
    Result<void> AttachChild(ObjectRegistry& objects, ObjectHandle slot, Element& panel,
                             const SharedPtr<Element>& child) noexcept override;
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept override;
    void ApplyProperties(Element& element) noexcept override;
    TypeId SlotType() const noexcept override;
    Element& ChildHost(Element& panel) noexcept override;
    Result<SharedPtr<Element>> CreatePanel(Allocator& allocator) noexcept override;

private:
    friend class ScrollBoxWidgetSlot;
    Result<void> ApplySlotSize(Element& panel, Element& child, StackSizeRule rule,
                               float weight) noexcept;
    ROCKS_PROPERTY(ReadOnly, Name = "horizontal") bool horizontal_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "wheelPolicy") uint8 wheelPolicy_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "wheelMultiplier") float wheelMultiplier_ = 1;
    ROCKS_PROPERTY(ReadOnly, Name = "scrollbarThickness") float thickness_ = 6;
    ROCKS_PROPERTY(ReadOnly, Name = "showScrollbar") bool showBar_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "alwaysShowScrollbar") bool alwaysShow_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "touchScrolling") bool touchScrolling_ = true;
    ROCKS_PROPERTY(ReadOnly, Name = "rightDragScrolling") bool rightDrag_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "scrollbarPadding") Vec4 barPadding_ {};
    ROCKS_PROPERTY(ReadOnly, Name = "alwaysShowTrack") bool alwaysShowTrack_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "backPadScrolling") bool padBefore_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "frontPadScrolling") bool padAfter_ = false;
    ROCKS_PROPERTY(Transient, Name = "userScrolled", Params = "offset") Event<float> scrolled_;
    ROCKS_NATIVE() bool endRequested_ = false;
    ROCKS_PROPERTY(Name = "initialOffset") float initialOffset_ = 0;
};

} // namespace rocks::ui
