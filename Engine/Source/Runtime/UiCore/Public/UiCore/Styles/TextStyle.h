#pragma once

#include <UiCore/ColorRef.h>
#include <UiCore/Styles/FontStyle.h>

namespace rocks::ui {

// 텍스트 스타일이다(UE FTextBlockStyle의 글꼴·색).
struct TextStyle {
    FontStyle font{};
    ColorRef color{ColorSource::Foreground};
    bool operator==(const TextStyle&) const noexcept = default;
};

} // namespace rocks::ui
