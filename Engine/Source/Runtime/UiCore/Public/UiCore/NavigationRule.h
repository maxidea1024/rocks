#pragma once
#include <Base/Guid.h>
#include <Core/Delegate.h>

namespace rocks::ui {
enum class FocusDirection : uint8 {
    Left,
    Right,
    Up,
    Down,
    Next,
    Previous
};
enum class NavigationRule : uint8 {
    Escape,
    Stop,
    Wrap,
    Explicit,
    Custom,
    CustomBoundary
};
struct NavigationBoundary {
    NavigationRule rule = NavigationRule::Escape;
    Guid target{};
};
using NavigationHandler = Delegate<Guid(uint32, FocusDirection)>;
} // namespace rocks::ui
