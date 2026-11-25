#pragma once

#include <Core/Result.h>
#include <TextLayout/GlyphGeometry.h>
#include <TextLayout/GlyphRaster.h>
#include <freetype/freetype.h>

// 전처리 geometry와 거리장 생성이다(Docs/Design/Font 6.5·6.8절 FT2e — UE SlateSdfGenerator.cpp
// 53–520행). FreeTypeBackend.cpp가 쓴다.
namespace rocks::text::detail {

// face의 글리프 윤곽을 글꼴 단위로 읽는다(UE FFreeTypeShapeBuilder::Build의 FreeType 경로).
Result<void> ExtractGlyphGeometry(FT_Face face, uint32 glyphIndex, GlyphGeometry& out) noexcept;

// geometry로 거리장을 만든다(UE FGlyphSdfMapping·FSdfGeneratorTask). 스레드 안전하다.
Result<void> GenerateGlyphDistanceField(const GlyphGeometry& geometry,
                                        const DistanceFieldRequest& request,
                                        GlyphBitmap& out) noexcept;

} // namespace rocks::text::detail
