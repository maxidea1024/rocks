#pragma once

#include <Core/Containers/SlotArray.h>
#include <Core/Math/Color.h>
#include <Core/Math/Rect.h>
#include <Core/Math/Vector4.h>
#include <UiCore/VectorImageKey.h>

namespace rocks::ui {

enum class BrushMode : uint8 {
    None,
    Image,
    Box,
    Border,
    // 둥근 상자(채움 + 안쪽 외곽선 — UE FSlateRoundedBoxBrush, UiToolkit 9.1절 UW3a)다.
    RoundedBox
};

// 둥근 상자 반경 방식이다(UE ESlateBrushRoundingType).
enum class BrushRounding : uint8 {
    // cornerRadii를 그대로 쓴다.
    Fixed,
    // 네 반경 모두 높이의 절반이다(알약·원 — 반경 없이 만든 UE 둥근 상자와 같다).
    HalfHeight
};

// 둥근 상자의 외곽선 설정이다(UE FSlateBrushOutlineSettings). 반경 순서는 왼쪽 위·오른쪽 위·오른쪽
// 아래·왼쪽 아래(UE와 같다)이고 단위는 로컬 UI 단위다. 외곽선은 상자 안쪽에 그린다.
struct BrushOutline {
    Vec4 cornerRadii{};
    LinearColor color{0, 0, 0, 0};
    float width = 0.0f;
    BrushRounding rounding = BrushRounding::HalfHeight;
    bool operator==(const BrushOutline&) const noexcept = default;
};

// resource는 렌더러에 등록한 이미지다. 빈 핸들은 단색을 뜻한다.
// imageSize는 원본의 논리 크기, margin은 원본 크기에 대한 각 변의 비율이다.
// vectorImage가 있는 Image brush는 벡터 이미지(SVG)다(UE ESlateBrushImageType::Vector —
// Docs/Design/UiToolkit 17.2절). 그리기 목록에 기록할 때 그릴 화소 크기로 래스터화한 자리로 바뀐다.
// resource는 쓰지 않고, uv는 그 자리 안의 부분 범위다. 벡터 9분할(Box·Border)은 아직 없어 Image
// 모드가 아니면 유효하지 않다.
struct ROCKS_UICORE_API Brush {
    BrushMode mode = BrushMode::Image;
    SlotHandle resource{};
    Vec2 imageSize{};
    Box2<float> uv{{0, 0}, {1, 1}};
    Vec4 margin{};
    LinearColor tint{1, 1, 1, 1};
    bool mirrorX = false;
    bool mirrorY = false;
    // RoundedBox 모드의 외곽선·반경이다.
    BrushOutline outline{};
    VectorImageKey vectorImage{};
    bool IsValid() const noexcept;

    // 고정 반경 둥근 상자다(UE FSlateRoundedBoxBrush(fill, radius, outline, width)).
    static Brush RoundedBox(LinearColor fill, float radius, LinearColor outlineColor = {0, 0, 0, 0},
                            float outlineWidth = 0.0f) noexcept;
    // 네 반경을 따로 준 둥근 상자다.
    static Brush RoundedBox(LinearColor fill, Vec4 radii, LinearColor outlineColor,
                            float outlineWidth) noexcept;
    // 높이 절반 반경(알약·원) 둥근 상자다(UE FSlateRoundedBoxBrush(fill, outline, width)).
    static Brush PillBox(LinearColor fill, LinearColor outlineColor = {0, 0, 0, 0},
                         float outlineWidth = 0.0f) noexcept;
    // 벡터 이미지 brush다(UE FSlateVectorImageBrush(path, size, tint)).
    static Brush Vector(VectorImageKey image, Vec2 size, LinearColor tint = {1, 1, 1, 1}) noexcept;
    bool operator==(const Brush&) const noexcept = default;
};

} // namespace rocks::ui
