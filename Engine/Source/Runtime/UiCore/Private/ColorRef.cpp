#include <UiCore/ColorRef.h>

namespace rocks::ui {

ColorRef ColorRef::FromTheme(ThemeSlot slot) noexcept {
    return {ColorSource::Theme, slot};
}

ColorRef ColorRef::Specified(LinearColor value) noexcept {
    return {ColorSource::Specified, ThemeSlot::White, value};
}

bool ColorRef::IsValid() const noexcept {
    return source <= ColorSource::Style && slot < ThemeSlot::Count && value.IsFinite();
}

LinearColor ColorRef::Resolve(const ThemeColors& theme,
                              const StyleColorContext& context) const noexcept {
    switch (source) {
    case ColorSource::Theme:
        return theme.Get(slot);
    case ColorSource::Foreground:
        return context.foreground;
    case ColorSource::SubduedForeground: {
        auto subdued = context.foreground;
        subdued.a *= 0.6f;
        return subdued;
    }
    case ColorSource::Style:
        return context.style;
    case ColorSource::Specified:
        return value;
    }

    return {};
}
} // namespace rocks::ui
