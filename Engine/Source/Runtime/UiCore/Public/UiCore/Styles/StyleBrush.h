#pragma once

#include <Core/Math/Vector4.h>
#include <UiCore/Brush.h>
#include <UiCore/ColorRef.h>

namespace rocks::ui {

// 테마 색으로 푸는 brush다(UE FSlateBrush — tint가 FSlateColor). 둥근 상자면 반경·외곽선을 쓴다.
struct ROCKS_UICORE_API StyleBrush {
    BrushMode mode = BrushMode::None;
    ColorRef fill = ColorRef::Specified({1, 1, 1, 1});
    ColorRef outlineColor = ColorRef::Specified({0, 0, 0, 0});
    float outlineWidth = 0.0f;
    // 반경 넷(왼쪽 위부터 시계 방향)과 방식이다. HalfHeight면 반경을 쓰지 않는다.
    Vec4 cornerRadii{};
    BrushRounding rounding = BrushRounding::Fixed;
    // 이미지 brush의 resource·논리 크기·9분할 여백이다.
    SlotHandle resource{};
    // 벡터 이미지(SVG)다. 있으면 Image 모드 벡터 brush로 푼다.
    VectorImageKey vectorImage{};
    Vec2 imageSize{};
    Vec4 margin{};
    // 채움 색 알파에 곱한다(UE Panel.CopyWithNewOpacity(0.8)처럼 테마 색의 반투명 변형).
    float opacity = 1.0f;

    // 그리지 않는 brush다(UE FSlateNoResource).
    static StyleBrush None() noexcept;
    // 테마 색 둥근 상자다(UE FSlateRoundedBoxBrush(fill, radius, outline, width)).
    static StyleBrush RoundedBox(ThemeSlot fill, float radius, ThemeSlot outline,
                                 float outlineWidth) noexcept;
    // 외곽선 없는 테마 색 둥근 상자다.
    static StyleBrush RoundedBox(ThemeSlot fill, float radius) noexcept;
    // 테마 색 단색 상자다(UE FSlateColorBrush).
    static StyleBrush Color(ThemeSlot fill) noexcept;
    // 고정 선형 색 둥근 상자·단색 상자다(UE FUMGColor처럼 테마를 따르지 않는 색 — 20.11절).
    static StyleBrush RoundedBox(LinearColor fill, float radius, LinearColor outline,
                                 float outlineWidth) noexcept;
    static StyleBrush Color(LinearColor fill) noexcept;
    // 높이 절반 반경(알약·원)의 테마 색 둥근 상자다(UE FSlateRoundedBoxBrush(fill) — 반경 없이 만든
    // 것).
    static StyleBrush Pill(ThemeSlot fill) noexcept;
    // 테마 색으로 tint한 벡터 이미지다(UE IMAGE_BRUSH_SVG(path, size, color) — 17.2절).
    static StyleBrush Svg(VectorImageKey image, Vec2 size, ThemeSlot tint) noexcept;
    // 전경색(UE FSlateColor::UseForeground)으로 tint한 벡터 이미지다.
    static StyleBrush SvgForeground(VectorImageKey image, Vec2 size) noexcept;

    // 테마로 색을 풀어 그릴 Brush를 만든다.
    Brush Resolve(const ThemeColors& theme, const StyleColorContext& context) const noexcept;

    bool operator==(const StyleBrush&) const noexcept = default;
};

} // namespace rocks::ui
