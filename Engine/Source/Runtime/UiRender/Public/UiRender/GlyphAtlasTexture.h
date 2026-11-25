#pragma once

#include <TextLayout/GlyphAtlas.h>
#include <UiRender/DrawBatchRenderer.h>

namespace rocks::ui {

// CPU 아틀라스 한 페이지에 대응한다. 장치·등록부·파이프라인이 이 객체보다 오래 살아야 한다.
// 같은 페이지의 스냅숏만 넘기고 생성·갱신·소멸은 렌더 소유 스레드에서 수행한다.
class ROCKS_UIRENDER_API GlyphAtlasTexture {
public:
    GlyphAtlasTexture(graphics::Device& device, SlotArray<BrushTexture>& textures) noexcept;
    ~GlyphAtlasTexture() noexcept;

    GlyphAtlasTexture(const GlyphAtlasTexture&) = delete;
    GlyphAtlasTexture& operator=(const GlyphAtlasTexture&) = delete;

    Result<void> Upload(const text::GlyphAtlasSnapshot& page,
                        graphics::PipelineHandle pipeline) noexcept;
    SlotHandle Resource() const noexcept;

private:
    graphics::Device& device_;
    SlotArray<BrushTexture>& textures_;
    graphics::TextureHandle texture_{};
    SlotHandle resource_{};
    uint64 revision_ = 0;
    uint32 size_ = 0;
    text::GlyphAtlasFormat format_ = text::GlyphAtlasFormat::Alpha;
};

} // namespace rocks::ui
