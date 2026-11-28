#pragma once

#include <Base/Types.h>

namespace rocks::ui {

// 스크롤 막대를 언제 보이는지다(Docs/Design/UiToolkit 23.6절).
enum class ScrollBarVisibility : uint8 {
    // 넘칠 때만 보인다(UE 기본 — SScrollBar::ShouldBeVisible).
    Auto,
    // 늘 보이고 트랙도 보인다(UE AlwaysShowScrollbar·AlwaysShowScrollbarTrack).
    Always,
    // 넘칠 때만, 마지막 상호작용 뒤 0.2초부터 0.2초 동안 흐려진다(UE bHideWhenNotInUse).
    HideWhenNotInUse,
    // 넘칠 때, 포인터가 영역 안에 있거나 막 스크롤했으면 보인다(Rocks 추가 — UE에 없음).
    OnHover
};

} // namespace rocks::ui
