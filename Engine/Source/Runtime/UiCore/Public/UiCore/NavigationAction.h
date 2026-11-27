#pragma once

#include <Base/Types.h>

namespace rocks::ui {

// 키가 뜻하는 UI 동작이다(UE EUINavigationAction — NavigationConfig.cpp 31–38행).
enum class NavigationAction : uint8 {
    None,
    Accept,
    Back
};

} // namespace rocks::ui
