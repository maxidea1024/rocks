#pragma once

#include <UiCore/Styles/CoreStyles.h>
#include <UiFramework/Layout/StackPanel.h>
#include <UiFramework/Scrolling/ScrollAlignment.h>

namespace rocks::ui {

// 스크롤 막대 둘레 여백 기본값(UI 단위)이다(UE SScrollBox ScrollBarPadding 2).
inline constexpr float ScrollBarPaddingDefault = 2.0f;

enum class ConsumeMouseWheel : uint8 {
    WhenScrolling,
    Always,
    Never
};

// 내용 하나를 주어진 방향으로 스크롤한다. 현재 clip은 창 축에 평행해야 한다.
class ROCKS_UIFRAMEWORK_API ScrollView : public Element {
public:
    using Super = Element;
    struct Args {
        ElementChild content{};
        StackDirection direction = StackDirection::Vertical;
        ElementArgs common{.clipToBounds = true};
    };

    // 세로 목록은 content만 지정하면 된다. 가로로 넘길 때는 direction을 바꾼다.
    Result<void> Construct(const Args& args) noexcept;
    explicit ScrollView(Allocator& allocator,
                        StackDirection direction = StackDirection::Vertical) noexcept;

    Result<void> TryAddChild(const SharedPtr<Element>& child) noexcept override;
    Vec2 ComputeDesiredSize(float scale) const noexcept override;
    Result<void> Arrange(const Geometry& geometry) noexcept override;
    PointerReply OnTouchPan(const PointerEvent& move, Vec2 origin) noexcept override;
    PointerReply OnPreviewPointerDown(const PointerEvent& event) noexcept override;
    PointerReply OnPointerEvent(const PointerEvent& event) noexcept override;
    void OnPointerCaptureLost(PointerSource source, uint32 userId,
                              uint32 pointerId) noexcept override;
    void OnMouseLeave(uint32 user) noexcept override;
    Result<void> OnPaintOverlay(PaintList& list, int32 layer) const noexcept override;

    Result<void> SetScrollOffset(float offset) noexcept;
    float GetScrollOffset() const noexcept;
    void SetDirection(StackDirection direction) noexcept;
    void SetWheelPolicy(ConsumeMouseWheel policy) noexcept;
    Result<void> SetWheelMultiplier(float value) noexcept;
    Result<void> SetScrollbarThickness(float logicalPixels) noexcept;
    void SetShowScrollbar(bool visible, bool alwaysShow = false) noexcept;
    Result<void> SetScrollbarPadding(Vec4 padding) noexcept;
    void SetAlwaysShowTrack(bool show) noexcept;
    void SetScrollPadding(bool before, bool after) noexcept;
    void SetTouchScrolling(bool enabled) noexcept;
    void SetRightDragScrolling(bool enabled) noexcept;
    void SetOnUserScrolled(Delegate<void(float)>&& callback) noexcept;
    bool IsScrolling() const noexcept;
    float GetViewFraction() const noexcept;
    float GetViewOffsetFraction() const noexcept;
    // 자식 배치 좌표를 스크롤 영역으로 변환한 뒤 요청한 위치가 보이도록 이동한다.
    Result<void> ScrollIntoView(const Element& child, ScrollIntoViewAlignment alignment,
                                float padding = 0) noexcept;
    void ScrollToEnd() noexcept;
    float GetMaxScrollOffset() const noexcept;

private:
    bool GetTrack(Vec2& position, Vec2& size) const noexcept;
    bool GetThumb(Vec2& position, Vec2& size) const noexcept;
    bool ToLocal(Vec2 position, Vec2& local) const noexcept;

    StackDirection direction_;
    float offset_ = 0;
    float maxOffset_ = 0;
    bool arranged_ = false;
    bool endRequested_ = false;
    ConsumeMouseWheel wheelPolicy_ = ConsumeMouseWheel::WhenScrolling;
    float wheelMultiplier_ = 1;
    // 막대 두께다(UE "Scrollbar" 두께 8 — DefaultScrollBarStyle).
    float thickness_ = DefaultScrollBarStyle().thickness;
    bool showBar_ = true;
    // 막대 둘레 여백이다(UE SScrollBox ScrollBarPadding 기본 2, SScrollBox.h 164행).
    Vec4 barPadding_{ScrollBarPaddingDefault, ScrollBarPaddingDefault, ScrollBarPaddingDefault,
                     ScrollBarPaddingDefault};
    bool alwaysShowTrack_ = false;
    bool padBefore_ = false;
    bool padAfter_ = false;
    bool alwaysShowBar_ = false;
    bool touchScrolling_ = true;
    bool rightDragScrolling_ = false;
    bool draggingContent_ = false;
    input::MouseButton dragButton_ = input::MouseButton::Left;
    Delegate<void(float)> userScrolled_;
    void SetUserOffset(float value) noexcept;
    float dragPosition_ = 0;
    float dragOffset_ = 0;
    PointerSource dragSource_ = PointerSource::Mouse;
    uint32 dragUser_ = 0;
    uint32 dragPointer_ = 0;
    bool dragging_ = false;
    // 마우스가 손잡이 위에 있는지다(올림 모양).
    bool thumbHovered_ = false;
    void SetDragging(bool dragging, bool content) noexcept;
    void UpdateThumbHover(Vec2 position) noexcept;
};

} // namespace rocks::ui
