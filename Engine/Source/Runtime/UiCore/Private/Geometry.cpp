#include <UiCore/Geometry.h>

namespace rocks::ui {

Vec2 AffineTransform2D::Apply(Vec2 p) const noexcept {
    return {m00 * p.x + m01 * p.y + tx, m10 * p.x + m11 * p.y + ty};
}

AffineTransform2D AffineTransform2D::Compose(const AffineTransform2D& inner) const noexcept {
    return {m00 * inner.m00 + m01 * inner.m10,    m00 * inner.m01 + m01 * inner.m11,
            m10 * inner.m00 + m11 * inner.m10,    m10 * inner.m01 + m11 * inner.m11,
            m00 * inner.tx + m01 * inner.ty + tx, m10 * inner.tx + m11 * inner.ty + ty};
}

bool AffineTransform2D::TryInverse(AffineTransform2D& out) const noexcept {
    const double determinant = static_cast<double>(m00) * m11 - static_cast<double>(m01) * m10;
    if (!math::IsFinite(determinant) || determinant == 0) {
        return false;
    }

    const double inverse = 1.0 / determinant;
    AffineTransform2D value{static_cast<float>(m11 * inverse),
                            static_cast<float>(-m01 * inverse),
                            static_cast<float>(-m10 * inverse),
                            static_cast<float>(m00 * inverse),
                            0,
                            0};
    value.tx = -(value.m00 * tx + value.m01 * ty);
    value.ty = -(value.m10 * tx + value.m11 * ty);
    if (!math::IsFinite(value.m00) || !math::IsFinite(value.m01) || !math::IsFinite(value.m10) ||
        !math::IsFinite(value.m11) || !math::IsFinite(value.tx) || !math::IsFinite(value.ty)) {
        return false;
    }

    out = value;
    return true;
}

Geometry Geometry::Root(Vec2 localSize, const AffineTransform2D& rootTransform) noexcept {
    return {localSize, rootTransform, rootTransform};
}

Geometry Geometry::MakeChild(Vec2 localSize, const AffineTransform2D& localLayout,
                             const AffineTransform2D& localRender, Vec2 pivot) const noexcept {
    const Vec2 offset{localSize.x * pivot.x, localSize.y * pivot.y};
    const AffineTransform2D toPivot{1, 0, 0, 1, offset.x, offset.y};
    const AffineTransform2D fromPivot{1, 0, 0, 1, -offset.x, -offset.y};
    return {localSize, layout.Compose(localLayout),
            render.Compose(localLayout).Compose(toPivot).Compose(localRender).Compose(fromPivot)};
}

Result<Box2<float>> Geometry::WindowBounds() const noexcept {
    const Vec2 corners[] = {render.Apply({}), render.Apply({size.x, 0}), render.Apply(size),
                            render.Apply({0, size.y})};
    return Box2<float>::TryFromPoints(corners);
}

bool Geometry::Contains(Vec2 windowPoint) const noexcept {
    if (!(size.x > 0 && size.y > 0) || !math::IsFinite(windowPoint.x) ||
        !math::IsFinite(windowPoint.y)) {
        return false;
    }

    AffineTransform2D inverse;
    if (!render.TryInverse(inverse)) {
        return false;
    }

    const Vec2 local = inverse.Apply(windowPoint);
    return local.x >= 0 && local.y >= 0 && local.x < size.x && local.y < size.y;
}

} // namespace rocks::ui
