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
    // OnNavigation이 방향을 스스로 처리했다(탐색이 초점을 옮기지 않는다 —
    // Docs/Design/UiToolkit 20.7절). 규칙 값과 따로 둔다(SetNavigationRule의 대상 없는 Explicit과
    // 겹치지 않게).
    bool handled = false;

    static NavigationBoundary Handled() noexcept {
        return {NavigationRule::Explicit, Guid{}, true};
    }
};
using NavigationHandler = Delegate<Guid(uint32, FocusDirection)>;

} // namespace rocks::ui
