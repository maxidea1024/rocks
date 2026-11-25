#pragma once

#include <Core/Text/StringView.h>

namespace rocks::ui {

// 글꼴 typeface다(UE 핵심 스타일이 쓰는 FSlateFontInfo::TypefaceFontName 값 — "Regular",
// "Bold", "Italic", "BoldItalic", "Light"). 문자열 대신 열거라 스타일이 이름 수명을 갖지 않는다.
enum class Typeface : uint8 {
    Regular,
    Bold,
    Italic,
    BoldItalic,
    Light
};

// typeface의 조합 글꼴 이름이다(UE TypefaceFontName 값 그대로 — "Regular"·"Bold"·"Italic"·
// "BoldItalic"·"Light").
ROCKS_UICORE_API StringView TypefaceName(Typeface typeface) noexcept;

} // namespace rocks::ui
