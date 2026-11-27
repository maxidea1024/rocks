#pragma once

#include <UiCore/ThemeColors.h>

namespace rocks::ui {

enum class ColorSource : uint8 {
    Specified,
    Theme,
    Foreground,
    SubduedForeground,
    Style
};

struct StyleColorContext {
    LinearColor foreground{1, 1, 1, 1};
    LinearColor style{1, 1, 1, 1};
};

// 테마나 부모가 바뀌어도 색 선택 방식을 보존한다. 직접 지정한 색은 선형 공간의 값이다.
struct ROCKS_UICORE_API ColorRef {
    ColorSource source = ColorSource::Specified;
    ThemeSlot slot = ThemeSlot::White;
    LinearColor value{1, 1, 1, 1};
    static ColorRef FromTheme(ThemeSlot slot) noexcept;
    static ColorRef Specified(LinearColor color) noexcept;
    bool IsValid() const noexcept;
    LinearColor Resolve(const ThemeColors& theme, const StyleColorContext& context) const noexcept;
    bool operator==(const ColorRef&) const noexcept = default;
};

} // namespace rocks::ui
