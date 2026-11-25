#pragma once

#include <Core/Containers/SlotArray.h>
#include <Core/Math/Color.h>
#include <Core/Math/Rect.h>
#include <Core/Math/Vector4.h>

namespace rocks::ui {

enum class BrushMode : uint8 {
    None,
    Image,
    Box,
    Border
};

// resource는 렌더러에 등록한 이미지다. 빈 핸들은 단색을 뜻한다.
// imageSize는 원본의 논리 크기, margin은 원본 크기에 대한 각 변의 비율이다.
struct ROCKS_UICORE_API Brush {
    BrushMode mode = BrushMode::Image;
    SlotHandle resource{};
    Vec2 imageSize{};
    Box2<float> uv{{0, 0}, {1, 1}};
    Vec4 margin{};
    LinearColor tint{1, 1, 1, 1};
    bool mirrorX = false;
    bool mirrorY = false;

    bool IsValid() const noexcept;
    bool operator==(const Brush&) const noexcept = default;
};

} // namespace rocks::ui
