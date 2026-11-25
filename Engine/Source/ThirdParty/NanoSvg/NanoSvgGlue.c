/* Rocks glue for nanosvg (Docs/Design/UiToolkit/17-VectorImages.ko.md section 17.2). It compiles the
   header-only parser and rasterizer once, routes every malloc, realloc and free through the Rocks
   hooks below, and implements the Rocks adapter API of RocksNanoSvg.h. The module that links
   NanoSvg defines the hooks over the Rocks library heap (UiRender). The standard headers are
   included first so that the macros only rename the calls inside the implementation, not the
   library declarations. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "RocksNanoSvg.h"

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
