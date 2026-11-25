#pragma once

#include <UiFramework/Frame.h>

namespace rocks::ui {

// 전체 내용 길이를 1로 두고 보이는 구간 [offset, offset + thumbSize]을 표시한다.
// 예를 들어 100행 중 25행을 보고 있다면 thumbSize는 0.25이고 마지막 페이지의 offset은 0.75다.
// SetState는 외부 스크롤 상태를 반영하며, 사용자 조작만 onScrolled를 발생시킨다.
class ROCKS_UIFRAMEWORK_API ScrollBar final : public Frame {
public:
    AccessibleRole GetAccessibleRole() const noexcept override;
    using Super = Frame;
    struct Args {
        float offset = 0;
        float thumbSize = 1;
        bool horizontal = false;
        Bindable<LinearColor> color{LinearColor{0.1f, 0.1f, 0.1f, 1}};
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
};

} // namespace rocks::ui
