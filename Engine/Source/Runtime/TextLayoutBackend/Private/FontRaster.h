#pragma once

#include <Core/Result.h>
#include <TextLayout/FontTypes.h>
#include <TextLayout/GlyphRaster.h>
#include <freetype/freetype.h>

// FreeType 글리프 래스터다(Docs/Design/Font 6.5·6.8절 FT2d). FreeTypeBackend.cpp가 크기를 적용한 뒤
// 부른다.
namespace rocks::text::detail {

// 크기를 적용한 face의 글리프를 래스터해 out에 쓴다(UE FSlateFontRenderer::GetRenderData·
// GetRenderDataInternal과 같다). face의 변환은 끝나면 지운다.
Result<void> RasterizeGlyph(FT_Library library, FT_Face face, FontHinting hinting,
                            const GlyphRasterRequest& request, GlyphBitmap& out) noexcept;

} // namespace rocks::text::detail
