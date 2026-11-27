#pragma once

#include <UiCore/Styles/CoreStyles.h>
#include <UiFramework/Layout/Frame.h>

namespace rocks::ui {

// 손잡이 최소 길이(화소)다. 내용이 아주 길어도 잡을 수 있게 둔다(UE SScrollBarTrack MinThumbSize
// 35, SScrollBarTrack.cpp 13행 — ScrollView 막대도 같은 값을 쓴다).
inline constexpr float MinScrollThumbLength = 35.0f;

// 원하는 크기의 길이 방향 기본값은 두께의 이 배수다.
inline constexpr float ScrollBarLengthFactor = 4.0f;

// 전체 내용 길이를 1로 두고 보이는 구간 [offset, offset + thumbSize]을 표시한다.
// 예를 들어 100행 중 25행을 보고 있다면 thumbSize는 0.25이고 마지막 페이지의 offset은 0.75다.
// SetState는 외부 스크롤 상태를 반영하며, 사용자 조작만 onScrolled를 발생시킨다.
class ROCKS_UIFRAMEWORK_API ScrollBar final : public Frame {
    ROCKS_UI_ELEMENT(ScrollBar, Frame)

public:
    AccessibleRole GetAccessibleRole() const noexcept override;
    struct Args {
        float offset = 0;
        float thumbSize = 1;
        bool horizontal = false;
        // 손잡이 tint다(기본 흰색은 스타일 색 그대로).
        Bindable<LinearColor> color{LinearColor{1, 1, 1, 1}};
        Delegate<void(float)> onScrolled{};
        ElementArgs common{};
    };

    Result<void> Construct(Args&& args) noexcept;
    explicit ScrollBar(Allocator& allocator) noexcept;
    Result<void> SetState(float offset, float thumbSize) noexcept;
    void SetHorizontal(bool horizontal) noexcept;
    float Offset() const noexcept;
    float ThumbSize() const noexcept;
    void SetOnScrolled(Delegate<void(float)>&& callback) noexcept;
    PointerReply OnPointerEvent(const PointerEvent& event) noexcept override;
    void OnPointerCaptureLost(PointerSource source, uint32 user, uint32 pointer) noexcept override;
    // 손잡이·트랙 모양이다(UE FScrollBarStyle — 기본 "Scrollbar"). 올림·끌기 상태가 있다.
    Result<void> SetStyle(const ScrollBarStyle& style) noexcept;
    const ScrollBarStyle& GetStyle() const noexcept;
    void OnMouseEnter(uint32 user) noexcept override;
    void OnMouseLeave(uint32 user) noexcept override;
    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;

private:
    float ThumbLength(float track) const noexcept;
    float ThumbStart(float track, float thumb) const noexcept;
    void ScrollTo(float offset) noexcept;
    float offset_ = 0;
    float thumbSize_ = 1;
    bool horizontal_ = false;
    bool dragging_ = false;
    float grabOffset_ = 0;
    PointerSource source_ = PointerSource::Mouse;
    uint32 user_ = 0, pointer_ = 0;
    Delegate<void(float)> scrolled_;
    void UpdateDesiredSize() noexcept;
    void SetDragging(bool dragging) noexcept;
    ScrollBarStyle style_ = DefaultScrollBarStyle();
    uint32 hoverCount_ = 0;
};

} // namespace rocks::ui
