#pragma once

#include <Base/Types.h>

namespace rocks::ui {

// hit grid 방향 탐색의 결과 종류다(Docs/Design/UiToolkit 20.9절 — UE
// FHittestGrid::FindFocusableWidget).
enum class FocusSearchOutcome : uint8 {
    // 찾지 못했다.
    None,
    // target으로 간다.
    Found,
    // 가장 가까운 후보가 경계 밖이다(CustomBoundary — 호출자가 사용자 정의 처리를 한다).
    BeyondBoundary,
    // 경계 끝까지 훑었는데 후보가 없다(CustomBoundary — 호출자가 사용자 정의 처리를 한다).
    ReachedEdge
};

} // namespace rocks::ui
