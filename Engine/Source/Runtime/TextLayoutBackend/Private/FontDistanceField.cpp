// 전처리 geometry와 거리장 생성이다(Docs/Design/Font 6.5·6.8절 FT2e). 규칙은 UE
// SlateSdfGenerator.cpp(FGlyphSdfMapping::Wrap·SetSpread 126–193행, FFreeTypeShapeBuilder::Build
// 221–297행, FSdfGeneratorTask::DoOutlineDecomposition·MakePlaceholder 412–520행)와 같다. msdfgen
// 1.13.1은 출력 bitmap의 y축 방향을 받으므로 UE의 inverseYAxis 뒤집기 대신 Y_DOWNWARD 출력으로 첫
// 행을 위로 둔다(근사 경로는 UE와 같은 inverseYAxis를 쓴다).
#include "FontDistanceField.h"

#include <Core/Math/MathUtility.h>
#include <freetype/ftoutln.h>
#include <core/ShapeDistanceFinder.h>
#include <msdfgen.h>
#include <ext/import-font.h>

// Msdfgen 모듈의 glue RocksApproximateSdf.cpp가 정의한다(서드파티 glue는 헤더가 없다 — 서명이
// 다르면 link가 실패한다, Docs/Design/Font 8.2절).
namespace msdfgen {

void approximateSDF(const BitmapRef<float, 1>& output, const Shape& shape,
                    const SDFTransformation& transformation);

} // namespace msdfgen

namespace rocks::text::detail {

namespace {

// 거리장 한 변의 상한(화소)이다(UE MAX_GLYPH_SDF_SIDE).
constexpr int32 MaxGlyphSdfSide = 4096;

// MTSDF 모서리 판정 각도(radian)다(UE SDF_CORNER_ANGLE_THRESHOLD).
constexpr double SdfCornerAngleThreshold = 3.0;

// 경계 상자 miter 한계다(UE SDF_BOUNDS_MITER_LIMIT).
constexpr double SdfBoundsMiterLimit = 1.0;

// 거리 매핑 초기 범위(UE Range(2) — SetSpread가 바로 덮는다)다.
constexpr double InitialSdfRange = 2.0;

// 화소 중심이 가장자리에 오도록 더하는 반 화소다.
constexpr double HalfPixel = 0.5;

// 단일 채널·MTSDF의 채널 수다.
constexpr int32 SimpleChannels = 1;
constexpr int32 MultichannelChannels = 4;

// edge 하나의 최대 제어점 수(3차)다.
constexpr usize MaxEdgePoints = 4;

// boundMiters의 극성(바깥 윤곽)이다(UE와 같다).
constexpr int MiterPolarity = 1;

// 글꼴 단위 그대로 읽는 윤곽 배율이다.
constexpr double FontUnitScale = 1.0;

// 모양이 뒤집혔는지 보는 바깥 점의 여유다(UE와 같다).
constexpr double OuterPointMargin = 1.0;

bool BuildShape(const GlyphGeometry& geometry, msdfgen::Shape& shape) noexcept {
    usize edge = 0;
    usize point = 0;
    for (const uint32 end : geometry.contourEdgeEnds) {
        msdfgen::Contour& contour = shape.addContour();
        for (; edge < end; ++edge) {
            const GeometryEdgeKind kind = geometry.edgeKinds[edge];
            const usize count = static_cast<usize>(kind) + 1;
            if (point + count > geometry.points.Size()) {
                return false;
            }

            msdfgen::Point2 points[MaxEdgePoints];
            for (usize index = 0; index < count; ++index) {
                const GeometryPoint& source = geometry.points[point + index];
                points[index] =
                    msdfgen::Point2(static_cast<double>(source.x), static_cast<double>(source.y));
            }

            switch (kind) {
            case GeometryEdgeKind::Line:
                contour.addEdge(msdfgen::EdgeHolder(points[0], points[1]));
                break;
            case GeometryEdgeKind::Quadratic:
                contour.addEdge(msdfgen::EdgeHolder(points[0], points[1], points[2]));
                break;
            case GeometryEdgeKind::Cubic:
            default:
                contour.addEdge(msdfgen::EdgeHolder(points[0], points[1], points[2], points[3]));
                break;
            }

            point += count;
        }
    }

    return edge == geometry.edgeKinds.Size() && point == geometry.points.Size();
}

} // namespace

Result<void> ExtractGlyphGeometry(FT_Face face, uint32 glyphIndex, GlyphGeometry& out) noexcept {
    out.Clear();
    if (glyphIndex >= static_cast<uint32>(face->num_glyphs)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // UE FreeTypeUtils::IsFaceEligibleForSdf다.
    if (FT_IS_TRICKY(face) || !FT_IS_SCALABLE(face) ||
        (FT_HAS_FIXED_SIZES(face) && face->num_glyphs <= 0)) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    const FT_Int32 flags = FT_LOAD_NO_SCALE | FT_LOAD_IGNORE_TRANSFORM | FT_LOAD_NO_HINTING |
                           FT_LOAD_NO_AUTOHINT | FT_LOAD_NO_BITMAP;
    if (FT_Load_Glyph(face, glyphIndex, flags) != FT_Err_Ok) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    const FT_GlyphSlot slot = face->glyph;
    if (slot->format != FT_GLYPH_FORMAT_OUTLINE || slot->metrics.width <= 0 ||
        slot->metrics.height <= 0 || slot->outline.n_points <= 0) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    msdfgen::Shape shape;
    if (msdfgen::readFreetypeOutline(shape, &slot->outline, FontUnitScale) != FT_Err_Ok ||
        shape.contours.empty()) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    for (const msdfgen::Contour& contour : shape.contours) {
        for (const msdfgen::EdgeHolder& edge : contour.edges) {
            const auto kind = static_cast<GeometryEdgeKind>(edge->type());
            if (auto added = out.edgeKinds.TryPushBack(kind); !added) {
                return added;
            }

            const msdfgen::Point2* points = edge->controlPoints();
            for (int32 index = 0; index <= edge->type(); ++index) {
                const GeometryPoint point{static_cast<int32>(math::Round(points[index].x)),
                                          static_cast<int32>(math::Round(points[index].y))};
                if (auto added = out.points.TryPushBack(point); !added) {
                    return added;
                }
            }
        }

        if (auto added = out.contourEdgeEnds.TryPushBack(static_cast<uint32>(out.edgeKinds.Size()));
            !added) {
            return added;
        }
    }

    out.unitsPerEm = face->units_per_EM;
    return Result<void>::Success();
}

Result<void> GenerateGlyphDistanceField(const GlyphGeometry& geometry,
                                        const DistanceFieldRequest& request,
                                        GlyphBitmap& out) noexcept {
    out.pixels.Clear();
    out.width = 0;
    out.height = 0;
    if (request.ppem == 0 || !(request.emOuterSpread > 0.0f) || !(request.emInnerSpread > 0.0f)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (geometry.Empty() || geometry.unitsPerEm == 0) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    msdfgen::Shape shape;
    if (!BuildShape(geometry, shape)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 모양의 경계 상자와 뒤집힌 윤곽 보정이다(UE Build 271–281행).
    msdfgen::Shape::Bounds bounds = shape.getBounds();
    const msdfgen::Point2 outerPoint(bounds.l - (bounds.r - bounds.l) - OuterPointMargin,
                                     bounds.b - (bounds.t - bounds.b) - OuterPointMargin);
    if (msdfgen::SimpleTrueShapeDistanceFinder::oneShotDistance(shape, outerPoint) > 0) {
        for (msdfgen::Contour& contour : shape.contours) {
            contour.reverse();
        }
    }

    // 화소 경계에 맞춘 거리장 상자다(UE FGlyphSdfMapping::Wrap, mitered).
    const double unitsPerEm = static_cast<double>(geometry.unitsPerEm);
    const double unitScale = static_cast<double>(request.ppem) / unitsPerEm;
    const double outerSpread = unitsPerEm * static_cast<double>(request.emOuterSpread);
    bounds.l -= outerSpread;
    bounds.b -= outerSpread;
    bounds.r += outerSpread;
    bounds.t += outerSpread;
    shape.boundMiters(bounds.l, bounds.b, bounds.r, bounds.t, outerSpread, SdfBoundsMiterLimit,
                      MiterPolarity);
    bounds.l = bounds.l * unitScale - HalfPixel;
    bounds.b = bounds.b * unitScale - HalfPixel;
    bounds.r = bounds.r * unitScale + HalfPixel;
    bounds.t = bounds.t * unitScale + HalfPixel;
    if (bounds.l > bounds.r) {
        bounds.l = 0;
        bounds.r = 0;
    }

    if (bounds.b > bounds.t) {
        bounds.b = 0;
        bounds.t = 0;
    }

    const int32 minX = static_cast<int32>(math::Floor(bounds.l));
    const int32 minY = static_cast<int32>(math::Floor(bounds.b));
    const int32 maxX = static_cast<int32>(math::Ceil(bounds.r));
    const int32 maxY = static_cast<int32>(math::Ceil(bounds.t));
    const int32 width = maxX - minX;
    const int32 height = maxY - minY;
    if (!(width > 0 && height > 0 && width < MaxGlyphSdfSide && height < MaxGlyphSdfSide)) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    msdfgen::SDFTransformation transformation(
        msdfgen::Projection(msdfgen::Vector2(unitScale),
                            msdfgen::Vector2(-minX, -minY) / unitScale),
        msdfgen::Range(InitialSdfRange));
    // UE FGlyphSdfMapping::SetSpread다.
    transformation.distanceMapping = msdfgen::DistanceMapping(
        msdfgen::Range(-unitsPerEm * static_cast<double>(request.emOuterSpread),
                       unitsPerEm * static_cast<double>(request.emInnerSpread)));

    const int32 channels = request.multichannel ? MultichannelChannels : SimpleChannels;
    const usize area = static_cast<usize>(width) * static_cast<usize>(height);
    Array<float> floats(out.pixels.GetAllocator());
    const int32 floatChannels = request.approximate ? SimpleChannels : channels;
    if (auto sized = floats.TryResize(area * static_cast<usize>(floatChannels)); !sized) {
        return sized;
    }

    if (auto sized = out.pixels.TryResize(area * static_cast<usize>(channels)); !sized) {
        return sized;
    }

    if (request.approximate) {
        // 자리표시(UE MakePlaceholder): 근사 단일 채널을 모든 채널에 쓴다.
        shape.inverseYAxis = true;
        msdfgen::approximateSDF(
            msdfgen::BitmapRef<float, SimpleChannels>(floats.Data(), width, height), shape,
            transformation);
        for (usize index = 0; index < area; ++index) {
            const uint8 value = msdfgen::pixelFloatToByte(floats[index]);
            for (int32 channel = 0; channel < channels; ++channel) {
                out.pixels[index * static_cast<usize>(channels) + static_cast<usize>(channel)] =
                    value;
            }
        }
    } else {
        // 겹친 윤곽 지원을 켠다(UE OverlappedContourSupport).
        constexpr bool overlapSupport = true;
        if (request.multichannel) {
            msdfgen::edgeColoringInkTrap(shape, SdfCornerAngleThreshold);
            msdfgen::generateMTSDF(
                msdfgen::BitmapSection<float, MultichannelChannels>(floats.Data(), width, height,
                                                                    msdfgen::Y_DOWNWARD),
                shape, transformation,
                msdfgen::MSDFGeneratorConfig(
                    overlapSupport, msdfgen::ErrorCorrectionConfig(
                                        msdfgen::ErrorCorrectionConfig::EDGE_PRIORITY,
                                        msdfgen::ErrorCorrectionConfig::CHECK_DISTANCE_AT_EDGE,
                                        msdfgen::ErrorCorrectionConfig::defaultMinDeviationRatio,
                                        msdfgen::ErrorCorrectionConfig::defaultMinImproveRatio)));
        } else {
            msdfgen::generateSDF(msdfgen::BitmapSection<float, SimpleChannels>(
                                     floats.Data(), width, height, msdfgen::Y_DOWNWARD),
                                 shape, transformation, msdfgen::GeneratorConfig(overlapSupport));
        }

        for (usize index = 0; index < floats.Size(); ++index) {
            out.pixels[index] = msdfgen::pixelFloatToByte(floats[index]);
        }
    }

    out.content = request.multichannel ? FontAtlasContent::DistanceField : FontAtlasContent::Alpha;
    out.width = width;
    out.height = height;
    out.bearingX = minX;
    out.bearingY = maxY;
    out.bitmapRenderScale = 1.0f;
    out.supportsOutline = true;
    return Result<void>::Success();
}

} // namespace rocks::text::detail
