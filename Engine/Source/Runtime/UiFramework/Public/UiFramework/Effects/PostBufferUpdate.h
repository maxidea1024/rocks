#pragma once

#include <UiCore/Declarative.h>

#include <UiCore/WidgetSurface.h>
#include <UiCore/Element.h>

namespace rocks::ui {

// 텍스처 하나에 적용할 후처리다. 0이면 화면을 그대로 복사하며 양수면 위젯 영역을 흐리게 한다.
// 빈 핸들은 호스트가 해당 위젯에 할당한 표면을 선택한다.
struct PostBufferUpdateInfo {
    SlotHandle destination{};
    float blurStrength = 0;
    // -1은 destination, 0~4는 동일 패스의 공유 버퍼를 사용한다.
    int32 channel = -1;
};

// 자신의 그리기 순서에서 현재 패스의 색상 버퍼 전체를 복사한다. 자체 픽셀은 출력하지 않는다.
class ROCKS_UIFRAMEWORK_API PostBufferUpdate final : public Element, public WidgetSurface {
    ROCKS_UI_ELEMENT(PostBufferUpdate, Element)

public:
    struct Args {
        Bindable<SlotHandle> destination{};
        Span<const PostBufferUpdateInfo> updates{};
        ElementArgs common{.visibility = Visibility::HitTestInvisible};
    };

    // Build·Make로 중첩할 때 자식과 그리기 속성을 한 번에 연결한다.
    Result<void> Construct(Args&& args) noexcept;

    WidgetSurface* GetWidgetSurface() noexcept override;
    uint32 RequestedPostBuffers() const noexcept override;
    SlotHandle GetPostBuffer(uint32 channel) const noexcept override;
    void SetPostBuffer(uint32 channel, SlotHandle texture) noexcept override;
    WidgetSurfaceKind SurfaceKind() const noexcept override;
    bool WantsManagedSurface() const noexcept override;
    SlotHandle ManagedSurface() const noexcept override;
    void SetManagedSurface(SlotHandle texture) noexcept override;

    explicit PostBufferUpdate(Allocator& allocator) noexcept;

    // 목적지는 현재 패스와 같은 크기와 형식으로 등록한 렌더 타깃이다.
    void SetDestination(SlotHandle destination) noexcept;
    Result<void> SetUpdates(Span<const PostBufferUpdateInfo> updates) noexcept;
    Result<void> TryAddChild(const SharedPtr<Element>& child) noexcept override;

    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;

private:
    Array<PostBufferUpdateInfo> updates_;
    SlotHandle managed_{};
    SlotHandle channels_[PostBufferChannelCount]{};
    ElementAttribute<SlotHandle, InvalidateReason::Paint> destination_;
};

} // namespace rocks::ui
