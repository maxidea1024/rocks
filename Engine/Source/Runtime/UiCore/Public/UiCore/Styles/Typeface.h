#pragma once

#include <Core/Text/StringView.h>

namespace rocks::ui {

// 글꼴 typeface다(UE 기본 합성 글꼴의 TypefaceFontName — LegacySlateFontInfoCache.cpp 111–122행).
// 문자열 대신 열거라 스타일이 이름 수명을 갖지 않는다.
enum class Typeface : uint8 {
    Regular,
    Medium,
    Light,
    VeryLight,
    Bold,
    Black,
    Italic,
    BoldItalic,
    BlackItalic,
    BoldCondensed,
    BoldCondensedItalic,
    Mono
};

// typeface의 조합 글꼴 이름이다(UE TypefaceFontName 값 그대로 — "Regular"·"BoldCondensed"·"Mono"
// 등).
ROCKS_UICORE_API StringView TypefaceName(Typeface typeface) noexcept;

} // namespace rocks::ui
