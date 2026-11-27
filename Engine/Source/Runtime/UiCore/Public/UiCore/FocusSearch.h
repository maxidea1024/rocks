#pragma once

#include <Core/Delegate.h>
#include <Core/Memory/SharedPtr.h>
#include <UiCore/FocusSearchOutcome.h>

namespace rocks::ui {

class Element;

// hit grid 방향 탐색의 결과다(Docs/Design/UiToolkit 20.9절). target은 Found일 때만 있다.
struct FocusSearchResult {
    FocusSearchOutcome outcome = FocusSearchOutcome::None;
    SharedPtr<Element> target;
};

// 후보 거르기다(scope·사용자·입력 경로). true면 후보다.
using FocusCandidateFilter = Delegate<bool(const Element& candidate)>;

} // namespace rocks::ui
