#pragma once

#include <UiCore/WidgetSurface.h>
#include <UiRender/DrawBatchRenderer.h>

namespace rocks::ui {

// 한 패스의 이름 있는 후처리 채널이다. 창과 각 Retainer 캡처는 서로 다른 세트를 사용한다.
// 각 채널의 최초 요청에만 생성하고 크기가 바뀌어도 브러시의 슬롯 핸들은 유지한다.
class ROCKS_UIRENDER_API PostBufferSet {
public:
    PostBufferSet(graphics::Device& device, SlotArray<BrushTexture>& textures) noexcept;
    ~PostBufferSet() noexcept;
    PostBufferSet(const PostBufferSet&) = delete;
    PostBufferSet& operator=(const PostBufferSet&) = delete;
    Result<SlotHandle> Prepare(uint32 channel, uint32 width, uint32 height,
                               graphics::PixelFormat format,
                               graphics::PipelineHandle pipeline) noexcept;
    SlotHandle Get(uint32 channel) const noexcept;

private:
    struct Target {
        SlotHandle brush{};
        graphics::TextureHandle texture{};
        uint32 width = 0;
        uint32 height = 0;
        graphics::PixelFormat format{};
    };
    Target targets_[PostBufferChannelCount]{};
    graphics::Device& device_;
    SlotArray<BrushTexture>& textures_;
};

} // namespace rocks::ui
