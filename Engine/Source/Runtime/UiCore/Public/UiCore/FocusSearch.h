#pragma once

#include <Core/Delegate.h>
#include <Core/Memory/SharedPtr.h>

namespace rocks::ui {

class Element;

// hit grid 방향 탐색의 결과다(Docs/Design/UiToolkit 20.9절 — UE FHittestGrid::FindFocusableWidget).
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

struct FocusSearchResult {
    FocusSearchOutcome outcome = FocusSearchOutcome::None;
    SharedPtr<Element> target;
};

// 후보 거르기다(scope·사용자·입력 경로). true면 후보다.
using FocusCandidateFilter = Delegate<bool(const Element& candidate)>;

} // namespace rocks::ui
