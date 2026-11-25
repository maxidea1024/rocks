#pragma once

#include <UiCore/PaintList.h>
#include <UiCore/WidgetSurfaceScene.h>
#include <UiCore/RetainerEffect.h>

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

    // 하위 트리 표면은 주기에 따라 갱신한다. 장면 표면은 렌더 호스트의 장면 콜백에서 채운다.
    virtual SharedPtr<const RetainerEffect> EffectMaterial() const noexcept;
    // 자체 장면이 있으면 호스트가 할당한 대상에 카메라 시점을 그린다.
    virtual SharedPtr<WidgetSurfaceScene> SceneSource() const noexcept;
    // 같은 패스의 같은 채널은 한 표면을 공유한다. 비트 i는 i번 후처리 버퍼의 요청이다.
    virtual uint32 RequestedPostBuffers() const noexcept;
    virtual SlotHandle GetPostBuffer(uint32 channel) const noexcept;
    virtual void SetPostBuffer(uint32 channel, SlotHandle texture) noexcept;
    virtual bool NeedsCapture(uint64 frame) const noexcept;
    virtual Result<void> RecordContent(PaintList& output) const noexcept;
};

} // namespace rocks::ui
