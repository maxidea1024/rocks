#pragma once

#include <Core/Math/Color.h>
#include <Core/Memory/SharedPtr.h>
#include <Core/Result.h>

namespace rocks::graphics {

class Device;
class CommandContext;

} // namespace rocks::graphics

namespace rocks::ui {

// 단일 소유 스레드에서 준비하는 장면 표면이다. 호스트가 프레임과 타깃 수명을 관리하고,
// 장면은 월드 갱신과 열린 그래픽 패스에 대한 그리기 명령을 담당한다.
class ROCKS_UICORE_API WidgetSurfaceScene {
public:
    virtual ~WidgetSurfaceScene() noexcept = default;
    virtual Result<void> Advance(uint64 frame) noexcept = 0;
    // 깊이가 필요한 3D 패스는 D32Float, 역 깊이 0으로 초기화한 첨부를 요청한다.
    virtual bool UsesDepth() const noexcept = 0;
    virtual LinearColor Background() const noexcept = 0;
    virtual Result<void> Draw(graphics::Device& device, graphics::CommandContext& commands,
                              uint32 width, uint32 height) noexcept = 0;
};

} // namespace rocks::ui
