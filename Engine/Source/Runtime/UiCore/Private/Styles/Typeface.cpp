#include <UiCore/Styles/Typeface.h>

namespace rocks::ui {

StringView TypefaceName(Typeface typeface) noexcept {
    switch (typeface) {
    case Typeface::Medium:
        return "Medium";
    case Typeface::Light:
        return "Light";
    case Typeface::VeryLight:
        return "VeryLight";
    case Typeface::Bold:
        return "Bold";
    case Typeface::Black:
        return "Black";
    case Typeface::Italic:
        return "Italic";
    case Typeface::BoldItalic:
        return "BoldItalic";
    case Typeface::BlackItalic:
        return "BlackItalic";
    case Typeface::BoldCondensed:
        return "BoldCondensed";
    case Typeface::BoldCondensedItalic:
        return "BoldCondensedItalic";
    case Typeface::Mono:
        return "Mono";
    case Typeface::Regular:
    default:
        return "Regular";
    }
}

} // namespace rocks::ui
