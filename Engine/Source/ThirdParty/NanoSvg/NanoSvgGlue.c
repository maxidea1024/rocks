/* Rocks glue for nanosvg (Docs/Design/UiToolkit/17-VectorImages.ko.md section 17.2). It compiles
   the header-only parser and rasterizer once, routes every malloc, realloc and free through the
   Rocks hooks below, and implements the API UiRender declares in UiRender/Private/RocksNanoSvg.h.
   The module that links NanoSvg defines the hooks over the Rocks library heap (UiRender). The
   standard headers are included first so that the macros only rename the calls inside the
   implementation, not the library declarations. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Prototypes of the API UiRender declares in UiRender/Private/RocksNanoSvg.h (engine-owned headers
   cannot live in a third-party library folder, so the glue repeats them; change both together). */
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

void* RocksNanoSvgMalloc(size_t size);
void* RocksNanoSvgRealloc(void* memory, size_t size);
void RocksNanoSvgFree(void* memory);

#define malloc(size) RocksNanoSvgMalloc(size)
#define realloc(memory, size) RocksNanoSvgRealloc(memory, size)
#define free(memory) RocksNanoSvgFree(memory)

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

/* Pixel units at 96 DPI, the same as UE SlateSVGRasterizer. */
#define ROCKS_NANOSVG_DPI 96.0f

RocksNanoSvgImage* RocksNanoSvgParse(char* text) {
    return (RocksNanoSvgImage*)nsvgParse(text, "px", ROCKS_NANOSVG_DPI);
}

void RocksNanoSvgDelete(RocksNanoSvgImage* image) {
    if (image != NULL) {
        nsvgDelete((NSVGimage*)image);
    }
}

void RocksNanoSvgSize(const RocksNanoSvgImage* image, float* width, float* height) {
    const NSVGimage* parsed = (const NSVGimage*)image;
    *width = parsed->width;
    *height = parsed->height;
}

static int RocksNanoSvgIsGradient(const NSVGpaint* paint) {
    return paint->type == NSVG_PAINT_LINEAR_GRADIENT || paint->type == NSVG_PAINT_RADIAL_GRADIENT;
}

int RocksNanoSvgHasGradient(const RocksNanoSvgImage* image) {
    const NSVGshape* shape = ((const NSVGimage*)image)->shapes;
    for (; shape != NULL; shape = shape->next) {
        if (RocksNanoSvgIsGradient(&shape->fill) || RocksNanoSvgIsGradient(&shape->stroke)) {
            return 1;
        }
    }

    return 0;
}

void RocksNanoSvgScale(RocksNanoSvgImage* image, float sx, float sy) {
    NSVGimage* parsed = (NSVGimage*)image;
    const float stroke = sqrtf(sx * sy);
    NSVGshape* shape = parsed->shapes;
    parsed->width *= sx;
    parsed->height *= sy;
    for (; shape != NULL; shape = shape->next) {
        NSVGpath* path = shape->paths;
        int dash = 0;
        shape->bounds[0] *= sx;
        shape->bounds[1] *= sy;
        shape->bounds[2] *= sx;
        shape->bounds[3] *= sy;
        shape->strokeWidth *= stroke;
        shape->strokeDashOffset *= stroke;
        for (dash = 0; dash < shape->strokeDashCount; ++dash) {
            shape->strokeDashArray[dash] *= stroke;
        }

        for (; path != NULL; path = path->next) {
            int point = 0;
            path->bounds[0] *= sx;
            path->bounds[1] *= sy;
            path->bounds[2] *= sx;
            path->bounds[3] *= sy;
            for (point = 0; point < path->npts; ++point) {
                path->pts[point * 2] *= sx;
                path->pts[point * 2 + 1] *= sy;
            }
        }
    }
}

RocksNanoSvgRasterizer* RocksNanoSvgCreateRasterizer(void) {
    return (RocksNanoSvgRasterizer*)nsvgCreateRasterizer();
}

void RocksNanoSvgDeleteRasterizer(RocksNanoSvgRasterizer* rasterizer) {
    if (rasterizer != NULL) {
        nsvgDeleteRasterizer((NSVGrasterizer*)rasterizer);
    }
}

void RocksNanoSvgRasterize(RocksNanoSvgRasterizer* rasterizer, RocksNanoSvgImage* image, float tx,
                           float ty, float scale, unsigned char* pixels, int width, int height,
                           int stride) {
    nsvgRasterize((NSVGrasterizer*)rasterizer, (NSVGimage*)image, tx, ty, scale, pixels, width,
                  height, stride);
}
