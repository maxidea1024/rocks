// Rocks glue: approximateSDF from msdfgen upstream branch performance-optimization, commit
// 3af96cda22c2eee039a41ccef67adfdb2188d469, core/approximate-sdf.cpp (Viktor Chlumsky, MIT license,
// same as msdfgen-1.13.1/LICENSE.txt). msdfgen 1.13.1 has no approximateSDF and no
// verticalScanlineIntersections; this adaptation computes the vertical pass with
// scanlineIntersections on a transposed copy of the shape (x and y swapped, so the returned x is
// the original y and dy is the original dx). Other changes: loop variables renamed so nothing
// shadows, the pixel macro became a lambda. The algorithm and its results are unchanged.

#include <core/BitmapRef.hpp>
#include <core/SDFTransformation.h>
#include <core/Shape.h>

#include <cmath>
#include <queue>
#include <core/arithmetics.hpp>

namespace msdfgen {

// Fast SDF approximation (out of range values are not computed).
void approximateSDF(const BitmapRef<float, 1>& output, const Shape& shape,
                    const SDFTransformation& transformation);

namespace {

// Cannot be FLT_MAX because it might be divided by range, which could be < 1.
constexpr float EstimatedMaxDistance = 1e24f;

// Sign of a distance that sits exactly on an edge.
constexpr float TinyDistance = .000001f;

struct Entry {
    float absDist;
    int bitmapX, bitmapY;
    Point2 nearPoint;

    bool operator<(const Entry& other) const {
        return absDist > other.absDist;
    }
};

Point2 transposed(const Point2& point) {
    return Point2(point.y, point.x);
}

Shape transposedShape(const Shape& shape) {
    Shape result;
    for (const Contour& contour : shape.contours) {
        Contour& target = result.addContour();
        for (const EdgeHolder& edge : contour.edges) {
            const Point2* points = edge->controlPoints();
            switch (edge->type()) {
            case 1:
                target.addEdge(EdgeHolder(transposed(points[0]), transposed(points[1])));
                break;
            case 2:
                target.addEdge(EdgeHolder(transposed(points[0]), transposed(points[1]),
                                          transposed(points[2])));
                break;
            default:
                target.addEdge(EdgeHolder(transposed(points[0]), transposed(points[1]),
                                          transposed(points[2]), transposed(points[3])));
                break;
            }
        }
    }
    return result;
}

} // namespace

void approximateSDF(const BitmapRef<float, 1>& output, const Shape& shape,
                    const SDFTransformation& transformation) {
    Entry entry = {};

    float* firstRow = output.pixels;
    ptrdiff_t stride = output.width;
    if (shape.inverseYAxis) {
        firstRow += (output.height - 1) * stride;
        stride = -stride;
    }
    const auto pixelAt = [&](int px, int py) -> float& { return (firstRow + py * stride)[px]; };

    for (float *p = output.pixels, *end = output.pixels + output.width * output.height; p < end;
         ++p)
        *p = -EstimatedMaxDistance;

    Vector2 invScale = transformation.unprojectVector(Vector2(1));
    DistanceMapping invDistanceMapping = transformation.distanceMapping.inverse();
    float dLimit = float(max(fabs(invDistanceMapping(0)), fabs(invDistanceMapping(1))));
    std::priority_queue<Entry> queue;
    double hits[3];
    int directions[3];

    // Horizontal scanlines
    for (int bitmapY = 0; bitmapY < output.height; ++bitmapY) {
        float* row = firstRow + bitmapY * stride;
        double shapeY = transformation.unprojectY(bitmapY + .5);
        entry.bitmapY = bitmapY;
        for (const Contour& contour : shape.contours) {
            for (const EdgeHolder& edge : contour.edges) {
                int n = edge->scanlineIntersections(hits, directions, shapeY);
                for (int i = 0; i < n; ++i) {
                    double bitmapX = transformation.projectX(hits[i]);
                    double bitmapX0 = floor(bitmapX - .5) + .5;
                    double bitmapX1 = bitmapX0 + 1;
                    if (bitmapX1 > 0 && bitmapX0 < output.width) {
                        float sd0 = float(directions[i] * invScale.x * (bitmapX0 - bitmapX));
                        float sd1 = float(directions[i] * invScale.x * (bitmapX1 - bitmapX));
                        if (sd0 == 0.f) {
                            if (sd1 == 0.f)
                                continue;
                            sd0 = -TinyDistance * float(sign(sd1));
                        }
                        if (sd1 == 0.f)
                            sd1 = -TinyDistance * float(sign(sd0));
                        if (bitmapX0 > 0) {
                            entry.absDist = fabsf(sd0);
                            entry.bitmapX = int(bitmapX0);
                            float& sd = row[entry.bitmapX];
                            if (entry.absDist < fabsf(sd)) {
                                sd = sd0;
                                entry.nearPoint = Point2(hits[i], shapeY);
                                queue.push(entry);
                            } else if (sd == -sd0)
                                sd = -EstimatedMaxDistance;
                        }
                        if (bitmapX1 < output.width) {
                            entry.absDist = fabsf(sd1);
                            entry.bitmapX = int(bitmapX1);
                            float& sd = row[entry.bitmapX];
                            if (entry.absDist < fabsf(sd)) {
                                sd = sd1;
                                entry.nearPoint = Point2(hits[i], shapeY);
                                queue.push(entry);
                            } else if (sd == -sd1)
                                sd = -EstimatedMaxDistance;
                        }
                    }
                }
            }
        }
    }

    // Bake in distance signs
    for (int rowIndex = 0; rowIndex < output.height; ++rowIndex) {
        float* row = firstRow + rowIndex * stride;
        int column = 0;
        for (; column < output.width && row[column] == -EstimatedMaxDistance; ++column)
            ;
        if (column < output.width) {
            bool flip = row[column] > 0;
            if (flip) {
                for (int i = 0; i < column; ++i)
                    row[i] = EstimatedMaxDistance;
            }
            for (; column < output.width; ++column) {
                if (row[column] != -EstimatedMaxDistance)
                    flip = row[column] > 0;
                else if (flip)
                    row[column] = EstimatedMaxDistance;
            }
        }
    }

    // Vertical scanlines (on the transposed shape, see the header comment)
    const Shape transposedCopy = transposedShape(shape);
    for (int bitmapX = 0; bitmapX < output.width; ++bitmapX) {
        double shapeX = transformation.unprojectX(bitmapX + .5);
        entry.bitmapX = bitmapX;
        for (const Contour& contour : transposedCopy.contours) {
            for (const EdgeHolder& edge : contour.edges) {
                int n = edge->scanlineIntersections(hits, directions, shapeX);
                for (int i = 0; i < n; ++i) {
                    double bitmapY = transformation.projectY(hits[i]);
                    double bitmapY0 = floor(bitmapY - .5) + .5;
                    double bitmapY1 = bitmapY0 + 1;
                    if (bitmapY0 > 0 && bitmapY1 < output.height) {
                        float sd0 = float(directions[i] * invScale.y * (bitmapY - bitmapY0));
                        float sd1 = float(directions[i] * invScale.y * (bitmapY - bitmapY1));
                        if (sd0 == 0.f) {
                            if (sd1 == 0.f)
                                continue;
                            sd0 = -TinyDistance * float(sign(sd1));
                        }
                        if (sd1 == 0.f)
                            sd1 = -TinyDistance * float(sign(sd0));
                        if (bitmapY0 > 0) {
                            entry.absDist = fabsf(sd0);
                            entry.bitmapY = int(bitmapY0);
                            float& sd = pixelAt(bitmapX, entry.bitmapY);
                            if (entry.absDist < fabsf(sd)) {
                                sd = sd0;
                                entry.nearPoint = Point2(shapeX, hits[i]);
                                queue.push(entry);
                            }
                        }
                        if (bitmapY1 < output.height) {
                            entry.absDist = fabsf(sd1);
                            entry.bitmapY = int(bitmapY1);
                            float& sd = pixelAt(bitmapX, entry.bitmapY);
                            if (entry.absDist < fabsf(sd)) {
                                sd = sd1;
                                entry.nearPoint = Point2(shapeX, hits[i]);
                                queue.push(entry);
                            }
                        }
                    }
                }
            }
        }
    }

    const auto spread = [&](const Entry& from, Entry next) {
        float& sd = pixelAt(next.bitmapX, next.bitmapY);
        if (fabsf(sd) == EstimatedMaxDistance) {
            Point2 shapeCoord =
                transformation.unproject(Point2(next.bitmapX + .5, next.bitmapY + .5));
            next.absDist = float((shapeCoord - from.nearPoint).length());
            sd = float(sign(sd)) * next.absDist;
            if (next.absDist < dLimit)
                queue.push(next);
        }
    };

    if (queue.empty())
        return;

    while (!queue.empty()) {
        Entry current = queue.top();
        queue.pop();
        Entry next = current;
        next.bitmapX = current.bitmapX - 1;
        if (next.bitmapX >= 0)
            spread(current, next);
        next.bitmapX = current.bitmapX + 1;
        if (next.bitmapX < output.width)
            spread(current, next);
        next.bitmapX = current.bitmapX;
        next.bitmapY = current.bitmapY - 1;
        if (next.bitmapY >= 0)
            spread(current, next);
        next.bitmapY = current.bitmapY + 1;
        if (next.bitmapY < output.height)
            spread(current, next);
    }

    for (float *p = output.pixels, *end = output.pixels + output.width * output.height; p < end;
         ++p)
        *p = transformation.distanceMapping(*p);
}

} // namespace msdfgen
