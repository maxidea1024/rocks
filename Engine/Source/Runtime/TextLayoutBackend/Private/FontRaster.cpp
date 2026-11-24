// FreeType 글리프 래스터다(Docs/Design/Font 6.5·6.8절 FT2d). UE SlateFontRenderer.cpp 420–774행
// (GetRenderData·RenderOutlineRows·GetRenderDataInternal·ResizeFontBitmap)과 같은 규칙이다.
#include "FontRaster.h"

#include "FontShaping.h"

#include <Core/Diagnostics/Log.h>
#include <Core/Math/MathUtility.h>
#include <freetype/ftbitmap.h>
#include <freetype/ftglyph.h>
#include <freetype/ftimage.h>
#include <freetype/ftoutln.h>
#include <freetype/ftstroke.h>
#include <string.h>

namespace rocks::text::detail {

namespace {

inline constexpr LogCategory LogTextLayoutRaster{"TextLayoutBackend"};

// 16.16·26.6 고정소수의 1이다.
constexpr float Fixed16Dot16One = 65536.0f;
constexpr float Fixed26Dot6One = 64.0f;
constexpr FT_Fixed Fixed16Dot16Unit = 0x10000L;

// 26.6 → 정수 픽셀 자리 이동과 올림 보정이다(UE RenderOutlineRows의 clip_box).
constexpr int Fixed26Dot6Shift = 6;
constexpr FT_Pos Fixed26Dot6CeilBias = 63;

// mitered 외곽선의 miter 한계(픽셀)다(UE DEFAULT_MITER_LIMIT).
constexpr float DefaultMiterLimit = 2.0f;

// 기울임을 0으로 보는 허용 오차다(UE FMath::IsNearlyEqual 기본 SMALL_NUMBER).
constexpr float SkewTolerance = 1.e-8f;

// 8 bit 회색 단계 수와 FT_Bitmap_Convert 정렬이다.
constexpr int FullGrayLevels = 256;
constexpr uint32 MaxCoverage = 255;
constexpr FT_Int ConvertAlignment = 4;

const char* ErrorText(FT_Error error) noexcept {
    const char* text = FT_Error_String(error);
    return text != nullptr ? text : "unknown FreeType error";
}

// UE FMath::RoundToFloat·RoundToInt(0.5 올림)이다.
float RoundHalfUp(float value) noexcept {
    return math::Floor(value + 0.5f);
}

// 같은 coverage의 가로 화소 묶음이다(UE FRasterizerSpan).
struct RasterSpan {
    int32 x = 0;
    int32 y = 0;
    int32 width = 0;
    uint8 coverage = 0;
};

// span과 그 경계 상자다(UE FRasterizerSpanList — 상자는 FBox2D처럼 비면 무효이고 0이다).
struct RasterSpanList {
    explicit RasterSpanList(Allocator& allocator) noexcept : spans(allocator) {
    }

    void Include(int32 x, int32 y) noexcept {
        if (!valid) {
            minX = maxX = x;
            minY = maxY = y;
            valid = true;
            return;
        }

        minX = x < minX ? x : minX;
        maxX = x > maxX ? x : maxX;
        minY = y < minY ? y : minY;
        maxY = y > maxY ? y : maxY;
    }

    Array<RasterSpan> spans;
    int32 minX = 0;
    int32 minY = 0;
    int32 maxX = 0;
    int32 maxY = 0;
    bool valid = false;
    bool failed = false;
};

void CollectSpans(int y, int count, const FT_Span* spans, void* user) {
    auto& list = *static_cast<RasterSpanList*>(user);
    for (int index = 0; index < count && !list.failed; ++index) {
        const FT_Span& span = spans[index];
        list.Include(span.x, y);
        list.Include(span.x + span.len - 1, y);
        if (!list.spans.TryPushBack({span.x, y, span.len, span.coverage})) {
            list.failed = true;
        }
    }
}

// 윤곽을 직접 모드로 래스터해 span을 모은다(UE RenderOutlineRows).
Result<void> RenderOutlineRows(FT_Library library, FT_Outline* outline, RasterSpanList& out) {
    FT_Raster_Params params;
    memset(&params, 0, sizeof(params));
    params.flags = FT_RASTER_FLAG_AA | FT_RASTER_FLAG_DIRECT | FT_RASTER_FLAG_CLIP;
    params.gray_spans = CollectSpans;
    params.user = &out;
    FT_Outline_Get_CBox(outline, &params.clip_box);
    params.clip_box.xMin = params.clip_box.xMin >> Fixed26Dot6Shift;
    params.clip_box.yMin = params.clip_box.yMin >> Fixed26Dot6Shift;
    params.clip_box.xMax = (params.clip_box.xMax + Fixed26Dot6CeilBias) >> Fixed26Dot6Shift;
    params.clip_box.yMax = (params.clip_box.yMax + Fixed26Dot6CeilBias) >> Fixed26Dot6Shift;
    FT_Outline_Render(library, outline, &params);
    return out.failed ? Result<void>::Failure(ErrorCode::OutOfMemory) : Result<void>::Success();
}

// 채움과 외곽선 span으로 화소를 만든다(UE GetRenderDataInternal 외곽선 경로). bearing 보정
// offset을 낸다.
Result<void> ComposeSpans(FT_Library library, FT_GlyphSlot slot, float outlineSize,
                          const GlyphOutline& settings, GlyphBitmap& out, int32& offsetX,
                          int32& offsetY) noexcept {
    Allocator& allocator = out.pixels.GetAllocator();
    RasterSpanList fill(allocator);
    if (auto rendered = RenderOutlineRows(library, &slot->outline, fill); !rendered) {
        return rendered;
    }

    RasterSpanList stroke(allocator);
    if (outlineSize > 0.0f) {
        const FT_Stroker_LineCap cap =
            settings.mitered ? FT_STROKER_LINECAP_SQUARE : FT_STROKER_LINECAP_ROUND;
        const FT_Stroker_LineJoin join =
            settings.mitered ? FT_STROKER_LINEJOIN_MITER : FT_STROKER_LINEJOIN_ROUND;
        const FT_Fixed miterLimit =
            settings.mitered ? static_cast<FT_Fixed>(DefaultMiterLimit * Fixed16Dot16One) : 0;

        FT_Stroker stroker = nullptr;
        if (FT_Stroker_New(library, &stroker) != FT_Err_Ok) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        FT_Stroker_Set(stroker, static_cast<FT_Fixed>(outlineSize * Fixed26Dot6One), cap, join,
                       miterLimit);
        FT_Glyph glyph = nullptr;
        FT_Error error = FT_Get_Glyph(slot, &glyph);
        if (error == FT_Err_Ok) {
            // StrokeBorder가 glyph를 바꿀 수 있으므로 원본을 지우게 한다(UE와 같다).
            error = FT_Glyph_StrokeBorder(&glyph, stroker, false, true);
        }

        Result<void> rendered = Result<void>::Success();
        if (error == FT_Err_Ok) {
            rendered = RenderOutlineRows(
                library, &reinterpret_cast<FT_OutlineGlyph>(glyph)->outline, stroke);
        }

        FT_Stroker_Done(stroker);
        FT_Done_Glyph(glyph);
        if (error != FT_Err_Ok) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        if (!rendered) {
            return rendered;
        }
    }

    // 두 상자의 합이다(FBox2D + FBox2D — 빈 쪽은 무시, 둘 다 비면 0 상자).
    RasterSpanList& first = fill.valid ? fill : stroke;
    int32 minX = first.minX;
    int32 minY = first.minY;
    int32 maxX = first.maxX;
    int32 maxY = first.maxY;
    if (fill.valid && stroke.valid) {
        minX = stroke.minX < minX ? stroke.minX : minX;
        minY = stroke.minY < minY ? stroke.minY : minY;
        maxX = stroke.maxX > maxX ? stroke.maxX : maxX;
        maxY = stroke.maxY > maxY ? stroke.maxY : maxY;
    }

    // 사각형이 양 끝을 포함하므로 1을 더한다(UE와 같다).
    const int32 width = maxX - minX + 1;
    const int32 height = maxY - minY + 1;
    if (auto resized = out.pixels.TryResize(static_cast<usize>(width) * static_cast<usize>(height));
        !resized) {
        return resized;
    }

    memset(out.pixels.Data(), 0, out.pixels.Size());
    out.width = width;
    out.height = height;
    offsetX = minX - fill.minX;
    offsetY = maxY - fill.maxY;

    const auto pixelAt = [&](const RasterSpan& span, int32 column) -> uint8& {
        const int32 row = height - 1 - (span.y - minY);
        return out.pixels[static_cast<usize>(row * width + span.x - minX + column)];
    };

    for (const RasterSpan& span : stroke.spans) {
        for (int32 column = 0; column < span.width; ++column) {
            pixelAt(span, column) = span.coverage;
        }
    }

    for (const RasterSpan& span : fill.spans) {
        for (int32 column = 0; column < span.width; ++column) {
            uint8& pixel = pixelAt(span, column);
            if (outlineSize <= 0.0f) {
                pixel = span.coverage;
            } else if (settings.separateFillAlpha) {
                // 반투명 채움용: 외곽선에서 채움을 뺀다.
                const int32 difference = static_cast<int32>(pixel) - span.coverage;
                pixel = span.coverage != 0
                            ? static_cast<uint8>(difference < 0 ? -difference : difference)
                            : 0;
            } else if (span.coverage == MaxCoverage) {
                pixel = span.coverage;
            }
        }
    }

    return Result<void>::Success();
}

// UE ResizeFontBitmap 평균 축소다(FImageUtils 방식, 알파 채널 포함).
Result<void> ResizeBitmap(GlyphBitmap& bitmap, int32 targetWidth, int32 targetHeight) noexcept {
    const uint32 bytesPerPixel = FontAtlasBytesPerPixel(bitmap.content);
    Array<uint8> resized(bitmap.pixels.GetAllocator());
    if (auto sized =
            resized.TryResize(static_cast<usize>(targetWidth) * targetHeight * bytesPerPixel);
        !sized) {
        return sized;
    }

    memset(resized.Data(), 0, resized.Size());
    const int32 sourceWidth = bitmap.width;
    const int32 sourceHeight = bitmap.height;
    const float stepX = static_cast<float>(sourceWidth) / static_cast<float>(targetWidth);
    const float stepY = static_cast<float>(sourceHeight) / static_cast<float>(targetHeight);
    const auto clampTo = [](int32 value, int32 last) noexcept {
        return value < 0 ? 0 : (value > last ? last : value);
    };

    float sourceY = 0.0f;
    for (int32 y = 0; y < targetHeight; ++y) {
        float sourceX = 0.0f;
        for (int32 x = 0; x < targetWidth; ++x) {
            const float endX = sourceX + stepX;
            const float endY = sourceY + stepY;
            const int32 startRow = clampTo(static_cast<int32>(sourceY + 0.5f), sourceHeight - 1);
            const int32 startColumn = clampTo(static_cast<int32>(sourceX + 0.5f), sourceWidth - 1);
            const int32 endRow = clampTo(static_cast<int32>(endY + 0.5f), sourceHeight - 1);
            const int32 endColumn = clampTo(static_cast<int32>(endX + 0.5f), sourceWidth - 1);

            float sums[ColorBytesPerPixel] = {};
            int32 count = 0;
            for (int32 column = startColumn; column <= endColumn; ++column) {
                for (int32 row = startRow; row <= endRow; ++row) {
                    const usize source =
                        (static_cast<usize>(row) * sourceWidth + column) * bytesPerPixel;
                    for (uint32 channel = 0; channel < bytesPerPixel; ++channel) {
                        sums[channel] += static_cast<float>(bitmap.pixels[source + channel]);
                    }

                    ++count;
                }
            }

            const usize target = (static_cast<usize>(y) * targetWidth + x) * bytesPerPixel;
            for (uint32 channel = 0; channel < bytesPerPixel; ++channel) {
                const float average = sums[channel] / static_cast<float>(count);
                resized[target + channel] = static_cast<uint8>(
                    average > static_cast<float>(MaxCoverage) ? MaxCoverage : average);
            }

            sourceX = endX;
        }

        sourceY += stepY;
    }

    bitmap.pixels = Move(resized);
    bitmap.width = targetWidth;
    bitmap.height = targetHeight;
    return Result<void>::Success();
}

// FreeType 비트맵을 out에 옮긴다(UE 표준 경로). Color·Alpha를 정하고 회색 범위를 0–255로 늘린다.
Result<void> CopyRenderedBitmap(FT_Library library, FT_GlyphSlot slot, GlyphBitmap& out) noexcept {
    FT_Bitmap converted;
    FT_Bitmap_Init(&converted);
    const FT_Bitmap* bitmap = &slot->bitmap;
    out.content = FontAtlasContent::Alpha;
    if (slot->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
        out.content = FontAtlasContent::Color;
    } else if (slot->bitmap.pixel_mode != FT_PIXEL_MODE_GRAY) {
        if (FT_Bitmap_Convert(library, &slot->bitmap, &converted, ConvertAlignment) != FT_Err_Ok) {
            FT_Bitmap_Done(library, &converted);
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        bitmap = &converted;
    }

    const uint32 bytesPerPixel = FontAtlasBytesPerPixel(out.content);
    const usize rowBytes = static_cast<usize>(bitmap->width) * bytesPerPixel;
    Result<void> result = out.pixels.TryResize(rowBytes * bitmap->rows);
    if (result) {
        for (uint32 row = 0; row < bitmap->rows; ++row) {
            memcpy(out.pixels.Data() + row * rowBytes,
                   bitmap->buffer + static_cast<ptrdiff_t>(row) * bitmap->pitch, rowBytes);
        }

        if (out.content == FontAtlasContent::Alpha && bitmap->num_grays != FullGrayLevels &&
            bitmap->num_grays > 1) {
            const uint32 boost = MaxCoverage / static_cast<uint32>(bitmap->num_grays - 1);
            for (uint8& pixel : out.pixels) {
                pixel = static_cast<uint8>(pixel * boost);
            }
        }

        out.width = static_cast<int32>(bitmap->width);
        out.height = static_cast<int32>(bitmap->rows);
    }

    FT_Bitmap_Done(library, &converted);
    return result;
}

Result<void> RasterizeLoaded(FT_Library library, FT_Face face, const GlyphRasterRequest& request,
                             GlyphBitmap& out) noexcept {
    const FT_GlyphSlot slot = face->glyph;
    const bool scalable = FT_IS_SCALABLE(face) != 0;
    const bool fixedSizes = FT_HAS_FIXED_SIZES(face) != 0;
    const FT_Fixed xScale = face->size->metrics.x_scale;
    // 비트맵 strike는 크면 atlas에 넣기 전에 줄이고, 작으면 GPU가 키운다(UE GetBitmapAtlasScale·
    // GetBitmapRenderScale).
    const float atlasScale = !scalable && fixedSizes && xScale < Fixed16Dot16Unit
                                 ? static_cast<float>(xScale) / Fixed16Dot16One
                                 : 1.0f;
    out.bitmapRenderScale = !scalable && fixedSizes && xScale > Fixed16Dot16Unit
                                ? static_cast<float>(xScale) / Fixed16Dot16One
                                : 1.0f;
    out.supportsOutline = scalable;
    out.content = FontAtlasContent::Alpha;

    const bool monochrome = !request.antialiasing;
    const FT_Render_Mode renderMode = monochrome ? FT_RENDER_MODE_MONO : FT_RENDER_MODE_NORMAL;
    const float outlineSize = RoundHalfUp(request.outline.size * request.scale);
    int32 offsetX = 0;
    int32 offsetY = 0;
    if ((outlineSize > 0.0f || request.spanRasterizer) && slot->format == FT_GLYPH_FORMAT_OUTLINE) {
        if (auto composed =
                ComposeSpans(library, slot, outlineSize, request.outline, out, offsetX, offsetY);
            !composed) {
            return composed;
        }

        // 외곽선을 그린 뒤 bearing을 얻으려고 채움을 렌더한다(UE와 같다).
        if (const FT_Error error = FT_Render_Glyph(slot, renderMode); error != FT_Err_Ok) {
            Log::Warning(LogTextLayoutRaster, "Cannot render glyph {Glyph}: {Error}",
                         request.glyphIndex, ErrorText(error));
            return Result<void>::Failure(ErrorCode::Unsupported);
        }
    } else {
        if (const FT_Error error = FT_Render_Glyph(slot, renderMode); error != FT_Err_Ok) {
            Log::Warning(LogTextLayoutRaster, "Cannot render glyph {Glyph}: {Error}",
                         request.glyphIndex, ErrorText(error));
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        if (auto copied = CopyRenderedBitmap(library, slot, out); !copied) {
            return copied;
        }

        if (atlasScale < 1.0f) {
            const int32 width = static_cast<int32>(static_cast<float>(out.width) * atlasScale);
            const int32 height = static_cast<int32>(static_cast<float>(out.height) * atlasScale);
            if (width > 0 && height > 0) {
                if (auto resized = ResizeBitmap(out, width, height); !resized) {
                    return resized;
                }
            } else {
                out.pixels.Clear();
                out.width = width;
                out.height = height;
            }
        }
    }

    out.bearingX = static_cast<int32>(
        RoundHalfUp(static_cast<float>(slot->bitmap_left + offsetX) * atlasScale));
    out.bearingY = static_cast<int32>(static_cast<float>(slot->bitmap_top + offsetY) * atlasScale);
    return Result<void>::Success();
}

} // namespace

Result<void> RasterizeGlyph(FT_Library library, FT_Face face, FontHinting hinting,
                            const GlyphRasterRequest& request, GlyphBitmap& out) noexcept {
    out.pixels.Clear();
    out.width = 0;
    out.height = 0;
    out.bearingX = 0;
    out.bearingY = 0;
    if (request.glyphIndex >= static_cast<uint32>(face->num_glyphs)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const bool skewed = request.skew > SkewTolerance || request.skew < -SkewTolerance;
    if (skewed) {
        FT_Matrix matrix;
        matrix.xx = Fixed16Dot16Unit;
        matrix.xy = static_cast<FT_Fixed>(request.skew * Fixed16Dot16One);
        matrix.yx = 0;
        matrix.yy = Fixed16Dot16Unit;
        FT_Set_Transform(face, &matrix, nullptr);
    } else {
        FT_Set_Transform(face, nullptr, nullptr);
    }

    const FT_Int32 flags = LoadFlags(hinting, request.antialiasing, FT_IS_SCALABLE(face) != 0);
    Result<void> result = Result<void>::Success();
    if (const FT_Error error = FT_Load_Glyph(face, request.glyphIndex, flags); error != FT_Err_Ok) {
        Log::Warning(LogTextLayoutRaster, "Cannot load glyph {Glyph}: {Error}", request.glyphIndex,
                     ErrorText(error));
        result = Result<void>::Failure(ErrorCode::Unsupported);
    } else {
        result = RasterizeLoaded(library, face, request, out);
    }

    // 변환은 face 상태라 shaping(hb-ft advance)에 새지 않게 지운다.
    if (skewed) {
        FT_Set_Transform(face, nullptr, nullptr);
    }

    return result;
}

} // namespace rocks::text::detail
