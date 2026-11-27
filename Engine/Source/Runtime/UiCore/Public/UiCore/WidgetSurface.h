#pragma once

#include <Core/Memory/SharedPtr.h>
#include <UiCore/PaintList.h>

namespace rocks::ui {

class ViewportSource;

} // namespace rocks::ui

namespace rocks::ui {

// 그리기 트리와 렌더 호스트 사이의 표면 계약이다. GPU 장치와 텍스처의 수명은 호스트가 관리한다.
enum class WidgetSurfaceKind : uint8 {
    RetainedContent,
    PostBuffer,
    Scene
};

inline constexpr uint32 PostBufferChannelCount = 5;

class ROCKS_UICORE_API WidgetSurface {
public:
    virtual ~WidgetSurface() noexcept = default;
    virtual WidgetSurfaceKind SurfaceKind() const noexcept = 0;
    virtual bool WantsManagedSurface() const noexcept = 0;
    virtual SlotHandle ManagedSurface() const noexcept = 0;
    virtual void SetManagedSurface(SlotHandle texture) noexcept = 0;

    // 장면 표면(viewport)에 장면을 대는 source다(Docs/Design/UiToolkit 18.3절). 없으면 외부 장면
    // 텍스처를 쓰거나 그리지 않는다.
    virtual SharedPtr<ViewportSource> GetViewportSource() const noexcept;
    // 같은 패스의 같은 채널은 한 표면을 공유한다. 비트 i는 i번 후처리 버퍼의 요청이다.
    virtual uint32 RequestedPostBuffers() const noexcept;
    virtual SlotHandle GetPostBuffer(uint32 channel) const noexcept;
    virtual void SetPostBuffer(uint32 channel, SlotHandle texture) noexcept;
    virtual bool NeedsCapture(uint64 frame) const noexcept;
    virtual Result<void> RecordContent(PaintList& output) const noexcept;
};

} // namespace rocks::ui
