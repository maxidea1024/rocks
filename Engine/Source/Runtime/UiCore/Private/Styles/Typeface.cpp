#include <UiCore/Styles/Typeface.h>

namespace rocks::ui {

StringView TypefaceName(Typeface typeface) noexcept {
    switch (typeface) {
    case Typeface::Bold:
        return "Bold";
    case Typeface::Italic:
        return "Italic";
    case Typeface::BoldItalic:
        return "BoldItalic";
    case Typeface::Light:
        return "Light";
    case Typeface::Regular:
    default:
        return "Regular";
    }
}

} // namespace rocks::ui
