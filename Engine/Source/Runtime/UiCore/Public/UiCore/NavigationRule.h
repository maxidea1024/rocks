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
// 탐색 방향 수다(FocusDirection 값 개수 — 방향마다 규칙 하나).
inline constexpr usize NavigationDirectionCount = static_cast<usize>(FocusDirection::Previous) + 1;

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
