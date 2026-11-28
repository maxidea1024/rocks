#pragma once

#include <UiFramework/Scrolling/ScrollBarVisibility.h>

namespace rocks::ui {

// 손잡이 보통 불투명도다(UE SScrollBar::GetThumbOpacity 0.75).
inline constexpr float ScrollThumbNormalOpacity = 0.75f;
// 흐려짐 정책에서 마지막 상호작용 뒤 흐려지기 시작할 때까지와 흐려지는 동안의 시간(초)이다(UE
// 0.2·0.2).
inline constexpr double ScrollBarFadeDelay = 0.2;
inline constexpr double ScrollBarFadeDuration = 0.2;

// 손잡이 불투명도를 정할 상태다.
struct ScrollThumbState {
    // 막대 위 올림이거나 손잡이를 끄는 중이다(불투명도 1 — UE IsHovered·bDraggingThumb).
    bool active = false;
    // 내용 끌기·터치 팬 중이다(경과 시간 0 — UE bIsScrolling).
    bool scrolling = false;
    // 포인터가 영역(내용 포함) 안에 있다(OnHover).
    bool areaHovered = false;
    // 마지막 상호작용(위치 변화·막대 들어옴·나감·영역 나감) 뒤 지난 시간(초)이다.
    double sinceInteraction = 0;
};

// 손잡이 불투명도(0–1)다(Docs/Design/UiToolkit 23.6절 — UE GetThumbOpacity 284–305행). 넘치는지는
// 호출자가 본다(안 넘치면 그리지 않는다). 목록이 막대를 얻을 때도 이 식을 쓴다.
//
// 사용 예:
//   const float alpha = ScrollThumbOpacity(ScrollBarVisibility::HideWhenNotInUse,
//                                          {.sinceInteraction = now - lastInteraction});
ROCKS_UIFRAMEWORK_API float ScrollThumbOpacity(ScrollBarVisibility visibility,
                                               const ScrollThumbState& state) noexcept;

// 시간이 지나면 불투명도가 바뀌는 중인지다(흐려짐 정책에서 아직 다 흐려지지 않음). 참인 동안
// 호스트가 매 프레임 다시 그린다.
ROCKS_UIFRAMEWORK_API bool IsScrollThumbFading(ScrollBarVisibility visibility,
                                               const ScrollThumbState& state) noexcept;

} // namespace rocks::ui
