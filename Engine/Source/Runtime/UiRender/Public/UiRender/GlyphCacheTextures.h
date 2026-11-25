#pragma once

#include <Core/Containers/Array.h>
#include <TextLayout/GlyphCache.h>
#include <UiRender/DrawBatchRenderer.h>

// 글리프 cache 텍스처의 GPU 쪽이다(Docs/Design/UiToolkit 9.1절 UW2, 글꼴 설계 7.1절).
// text::GlyphCache의 텍스처마다 GPU 텍스처를 하나 두고 브러시 등록부(SlotArray<BrushTexture>)에
// 등록한다. 형식은 내용 종류를 따른다: Alpha → R8Unorm(mask 파이프라인), Color →
// BGRA8UnormSrgb(premultiplied, image 파이프라인), 거리장 → RGBA8Unorm(image 파이프라인 — 거리장
// 셰이더는 엔진 셰이더 SLU).
//
// 갱신: cache가 바뀐 텍스처를 알려 주면(TakeDirtyRegion) 그 텍스처를 올린다. Graphics
// UpdateTexture가 mip 단위라 바뀐 사각형이 아니라 텍스처 전체를 올린다(영역 갱신은 Graphics 업로드
// ring에 영역 API가 생기면 바꾼다 — 9.1절 UW2).
//
// 사용 예(렌더 소유 스레드):
//   GlyphCacheTextures textures(allocator, device, brushes);
//   ... cache.FindOrAdd로 글리프를 넣은 뒤 ...
//   textures.Sync(cache, maskPipeline, imagePipeline);
//   SlotHandle brush = textures.Resource(glyph.texture);
//   if (cache.AdvanceFrame()) { textures.Reset(); }   // flush하면 텍스처를 모두 놓는다
//
// 수명: 장치·등록부·파이프라인이 이 객체보다 오래 살아야 한다.
namespace rocks::ui {

class ROCKS_UIRENDER_API GlyphCacheTextures {
public:
    GlyphCacheTextures(Allocator& allocator, graphics::Device& device,
                       SlotArray<BrushTexture>& textures) noexcept;
    ~GlyphCacheTextures() noexcept;

    GlyphCacheTextures(const GlyphCacheTextures&) = delete;
    GlyphCacheTextures& operator=(const GlyphCacheTextures&) = delete;

    // cache의 텍스처를 GPU와 맞춘다: 새 텍스처는 만들고 등록하며, 바뀐 텍스처는 다시 올린다.
    // 파이프라인이 비었으면 InvalidArgument, 텍스처 생성·등록·업로드 실패는 그 오류다(이미 맞춘
    // 텍스처는 남는다).
    Result<void> Sync(text::GlyphCache& cache, graphics::PipelineHandle maskPipeline,
                      graphics::PipelineHandle imagePipeline) noexcept;

    // 모든 GPU 텍스처와 등록을 놓는다(cache가 flush된 뒤 — GlyphCache::AdvanceFrame이 true).
    void Reset() noexcept;

    // cache 텍스처 번호의 브러시 handle이다. 아직 맞추지 않았으면 빈 handle이다.
    SlotHandle Resource(uint32 textureIndex) const noexcept;

private:
    struct Entry {
        graphics::TextureHandle texture{};
        SlotHandle slot{};
    };

    graphics::Device& device_;
    SlotArray<BrushTexture>& textures_;
    Array<Entry> entries_;
};

} // namespace rocks::ui
