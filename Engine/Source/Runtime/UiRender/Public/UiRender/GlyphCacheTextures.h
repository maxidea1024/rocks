#pragma once

#include <Core/Containers/Array.h>
#include <TextLayout/GlyphCache.h>
#include <UiCore/Rendering/UiRenderer.h>

// 글리프 cache 텍스처의 렌더러 쪽이다(Docs/Design/UiToolkit 9.1절 UW2·18.4절, 글꼴 설계 7.1절).
// text::GlyphCache의 텍스처마다 UI 텍스처를 하나 둔다. 형식·sampling은 내용 종류를 따른다:
// Alpha → R8(Mask), Color → BGRA8 sRGB(Image), 거리장 → RGBA8(Image — 거리장 셰이더는 엔진 셰이더
// SLU).
//
// 갱신: cache가 바뀐 텍스처를 알려 주면(TakeDirtyRegion) 그 텍스처 전체를 올린다(영역 갱신은
// Graphics 업로드 ring GX8 뒤 — 9.1절 UW2). 게임 lane에서 그 프레임의 모든 기록 뒤, DrawWindows
// 전에 부른다(18.4절 — lane은 자원 작업을 게시 순서대로 처리하므로 그 프레임보다 먼저 올라간다).
//
// 사용 예(게임 lane):
//   GlyphCacheTextures textures(allocator, renderer);
//   ... cache.FindOrAdd로 글리프를 넣은 뒤 ...
//   textures.Upload(cache);
//   UiTextureId brush = textures.Resource(glyph.texture);
//   if (cache.AdvanceFrame()) { textures.Reset(); }   // flush하면 텍스처를 모두 놓는다
//
// 수명: renderer가 이 객체보다 오래 살아야 한다.
namespace rocks::ui {

class ROCKS_UIRENDER_API GlyphCacheTextures {
public:
    GlyphCacheTextures(Allocator& allocator, UiRenderer& renderer) noexcept;
    ~GlyphCacheTextures() noexcept;

    GlyphCacheTextures(const GlyphCacheTextures&) = delete;
    GlyphCacheTextures& operator=(const GlyphCacheTextures&) = delete;

    // cache의 텍스처를 렌더러와 맞춘다: 새 텍스처는 만들고, 바뀐 텍스처는 다시 올린다. 생성·업로드
    // 실패는 그 오류다(이미 맞춘 텍스처는 남는다).
    Result<void> Upload(text::GlyphCache& cache) noexcept;

    // 모든 텍스처를 놓는다(cache가 flush된 뒤 — GlyphCache::AdvanceFrame이 true).
    void Reset() noexcept;

    // cache 텍스처 번호의 텍스처 id다. 아직 맞추지 않았으면 빈 id다.
    UiTextureId Resource(uint32 textureIndex) const noexcept;

private:
    UiRenderer& renderer_;
    Array<UiTextureId> textures_;
};

} // namespace rocks::ui
