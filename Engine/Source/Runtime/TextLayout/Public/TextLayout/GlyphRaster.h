#pragma once

#include <Core/Containers/Array.h>

// 글리프 래스터 요청·결과 형식이다(Docs/Design/Font 6.5·6.8절 FT2d). 라이브러리 형식은 쓰지 않는다.
//
// 사용 예:
//   text::GlyphBitmap bitmap(allocator);
//   face->Rasterize({.glyphIndex = glyph, .size = 24.0f}, bitmap);
namespace rocks::text {

// atlas 화소 내용이다(UE ESlateFontAtlasContentType Alpha·Color·Msdf).
enum class FontAtlasContent : uint8 {
    // 8 bit coverage다.
    Alpha,
    // 8 bit sRGB BGRA, premultiplied다(FreeType 출력 그대로 — UE PF_B8G8R8A8).
    Color,
    // 8 bit linear RGBA 거리장(MTSDF)이다(FT2e).
    DistanceField,
};

inline constexpr uint32 AlphaBytesPerPixel = 1;
inline constexpr uint32 ColorBytesPerPixel = 4;

// 내용 종류의 화소 byte 수다.
constexpr uint32 FontAtlasBytesPerPixel(FontAtlasContent content) noexcept {
    return content == FontAtlasContent::Alpha ? AlphaBytesPerPixel : ColorBytesPerPixel;
}

// 글리프 외곽선 설정이다(UE FFontOutlineSettings의 글리프 부분).
struct GlyphOutline {
    // 외곽선 두께(포인트가 아니라 UI 단위, 배율 전)다. 0이면 외곽선이 없다.
    float size = 0.0f;
    // 참이면 모서리를 각지게(square cap·miter join), 거짓이면 둥글게 한다.
    bool mitered = false;
    // 참이면 채움 영역을 외곽선에서 빼 반투명 채움에 맞춘다(UE bSeparateFillAlpha).
    bool separateFillAlpha = false;

    friend constexpr bool operator==(const GlyphOutline&, const GlyphOutline&) noexcept = default;
};

struct GlyphRasterRequest {
    uint32 glyphIndex = 0;
    // 96 DPI 포인트 크기와 배율이다(6.1절).
    float size = 0.0f;
    float scale = 1.0f;
    // 가짜 기울임(x += skew × y)이다. 0이면 변환이 없다.
    float skew = 0.0f;
    GlyphOutline outline = {};
    // 거짓이면 MONO로 래스터한다(UE Slate.EnableFontAntiAliasing = 0).
    bool antialiasing = true;
    // 외곽선이 없어도 span 래스터 경로를 쓴다(UE Slate.OutlineFontRenderMethod = 1).
    bool spanRasterizer = false;
};

// 래스터 결과다. 화소는 첫 행이 위이고 행 사이 간격이 없다.
struct GlyphBitmap {
    explicit GlyphBitmap(Allocator& allocator) noexcept : pixels(allocator) {
    }

    FontAtlasContent content = FontAtlasContent::Alpha;
    int32 width = 0;
    int32 height = 0;
    // 펜 위치에서 왼쪽 위 화소까지의 거리다(y는 위가 양수 — UE HorizontalOffset·VerticalOffset).
    int32 bearingX = 0;
    int32 bearingY = 0;
    // 비트맵 strike가 목표보다 작을 때 GPU가 키울 배율이다(그 밖은 1).
    float bitmapRenderScale = 1.0f;
    // face가 외곽선을 지원하나다(scalable face만).
    bool supportsOutline = false;
    Array<uint8> pixels;
};

} // namespace rocks::text
