#pragma once

#include <UiCore/Styles/Typeface.h>

namespace rocks::ui {

// 글꼴 지정이다(UE FSlateFontInfo의 typeface·크기). size는 96 DPI 포인트다.
struct FontStyle {
    Typeface typeface = Typeface::Regular;
    float size = 10.0f;
    bool operator==(const FontStyle&) const noexcept = default;
};

} // namespace rocks::ui
