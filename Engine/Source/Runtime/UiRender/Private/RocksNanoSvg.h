#pragma once

// UiRender가 쓰는 nanosvg 함수 선언이다(Docs/Design/UiToolkit 17.2절). nanosvg 형식은 불투명 handle
// 뒤에 두고, 구현은 ThirdParty NanoSvg 모듈의 glue(NanoSvgGlue.c)가 nanosvg.h·nanosvgrast.h 위에
// 한다(외부 원본 폴더에는 glue 소스만 둘 수 있어 선언은 여기, glue에는 같은 원형이 있다 — 둘을 함께
// 고친다). 모든 할당은 Rocks 라이브러리 heap을 거친다(UiRender NanoSvgHooks.cpp).

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RocksNanoSvgImage RocksNanoSvgImage;
typedef struct RocksNanoSvgRasterizer RocksNanoSvgRasterizer;

/* Parses a zero-terminated SVG document. nanosvg changes the text while it parses, so the caller
   passes a writable copy. Units are pixels at 96 DPI (the same as UE). Returns NULL when parsing
   fails or memory runs out. */
RocksNanoSvgImage* RocksNanoSvgParse(char* text);

/* Frees a parsed image. NULL does nothing. */
void RocksNanoSvgDelete(RocksNanoSvgImage* image);

/* Width and height of the image in pixels. */
void RocksNanoSvgSize(const RocksNanoSvgImage* image, float* width, float* height);

/* 1 when any shape fills or strokes with a linear or radial gradient, otherwise 0. */
int RocksNanoSvgHasGradient(const RocksNanoSvgImage* image);

/* Multiplies every point, bound and the image size by (sx, sy) and stroke widths and dashes by
   sqrt(sx * sy). Upstream nanosvg has no non-uniform scale entry point (UE patches in
   nsvgRasterizeFull), so a stretched raster scales a parsed copy first. Gradients are not
   transformed. */
void RocksNanoSvgScale(RocksNanoSvgImage* image, float sx, float sy);

/* Rasterizer state that can be reused across images. Returns NULL when memory runs out. */
RocksNanoSvgRasterizer* RocksNanoSvgCreateRasterizer(void);
void RocksNanoSvgDeleteRasterizer(RocksNanoSvgRasterizer* rasterizer);

/* Draws the image scaled by scale and moved by (tx, ty) into width x height straight-alpha RGBA
   pixels (stride bytes per row, first row at the top). */
void RocksNanoSvgRasterize(RocksNanoSvgRasterizer* rasterizer, RocksNanoSvgImage* image, float tx,
                           float ty, float scale, unsigned char* pixels, int width, int height,
                           int stride);

#ifdef __cplusplus
}
#endif
