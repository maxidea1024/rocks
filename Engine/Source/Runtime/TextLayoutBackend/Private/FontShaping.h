#pragma once

#include <Core/Containers/Array.h>
#include <Core/Result.h>
#include <TextLayout/FontTypes.h>
#include <TextLayout/TextShaping.h>
#include <freetype/freetype.h>
#include <hb.h>

// FreeType face의 shaping과 load 플래그다(Docs/Design/Font 6.2·6.4절). FreeTypeBackend.cpp가 쓴다.
namespace rocks::text::detail {

// hinting·안티앨리어싱·scalable 여부의 FreeType load 플래그다(6.2절 표 — UE SlateFontRenderer.cpp
// 57–82행과 같다).
FT_Int32 LoadFlags(FontHinting hinting, bool antialiasing, bool scalable) noexcept;

// 크기를 적용한 face로 run을 shaping해 out 뒤에 붙인다. hbFont는 처음 부를 때 만들어 face와 함께
// 산다.
Result<void> ShapeRun(FT_Face face, hb_font_t*& hbFont, BackendFace* owner, FontHinting hinting,
                      const ShapeRunRequest& request, Array<ShapedGlyph>& out) noexcept;

} // namespace rocks::text::detail
