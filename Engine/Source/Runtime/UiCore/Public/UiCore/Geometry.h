#pragma once

#include <Core/Math/Rect.h>

namespace rocks::ui {

// 2차원 아핀 변환이다. x' = m00*x + m01*y + tx, y' = m10*x + m11*y + ty.
struct ROCKS_UICORE_API AffineTransform2D {
    float m00 = 1, m01 = 0, m10 = 0, m11 = 1, tx = 0, ty = 0;
    bool operator==(const AffineTransform2D&) const noexcept = default;

    Vec2 Apply(Vec2 p) const noexcept;

    // inner를 먼저 적용하고 이 변환을 적용하는 합성 변환을 돌려준다.
    AffineTransform2D Compose(const AffineTransform2D& inner) const noexcept;

    // 역변환을 구한다. 행렬이 특이하거나 유한하지 않으면 out을 변경하지 않는다.
    bool TryInverse(AffineTransform2D& out) const noexcept;
};

// 로컬 크기와 창 좌표까지 누적된 변환이다. render에는 layout과 부모 render가 이미 포함된다.
struct ROCKS_UICORE_API Geometry {
    Vec2 size;
    AffineTransform2D layout;
    AffineTransform2D render;
    bool operator==(const Geometry&) const noexcept = default;

    // 루트에서는 layout과 render의 누적 변환이 같다.
    static Geometry Root(Vec2 localSize, const AffineTransform2D& rootTransform) noexcept;

    // 자식의 render 변환은 로컬 피벗 기준으로 적용한 뒤 배치와 부모 변환을 적용한다.
    Geometry MakeChild(Vec2 localSize, const AffineTransform2D& localLayout,
                       const AffineTransform2D& localRender, Vec2 pivot) const noexcept;

    // 창 좌표의 점을 로컬 좌표로 바꾸어 판정한다. 오른쪽과 아래 경계는 포함하지 않는다.
    bool Contains(Vec2 windowPoint) const noexcept;
    Result<Box2<float>> WindowBounds() const noexcept;
};

} // namespace rocks::ui
