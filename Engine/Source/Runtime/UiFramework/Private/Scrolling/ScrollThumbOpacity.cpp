#include <Core/Math/MathUtility.h>
#include <UiFramework/Scrolling/ScrollThumbOpacity.h>

namespace rocks::ui {

namespace {

// 흐려짐 정책이고 지금 시간에 따라 흐려지는 자리인지다(올림·끌기·OnHover 영역 안이 아님).
bool UsesFade(ScrollBarVisibility visibility, const ScrollThumbState& state) noexcept {
    if (state.active) {
        return false;
    }

    return visibility == ScrollBarVisibility::HideWhenNotInUse ||
           (visibility == ScrollBarVisibility::OnHover && !state.areaHovered);
}

} // namespace

float ScrollThumbOpacity(ScrollBarVisibility visibility, const ScrollThumbState& state) noexcept {
    if (state.active) {
        return 1.0f;
    }

    if (visibility == ScrollBarVisibility::OnHover && state.areaHovered) {
        return ScrollThumbNormalOpacity;
    }

    if (!UsesFade(visibility, state)) {
        return ScrollThumbNormalOpacity;
    }

    // UE Lerp(1, 0, Clamp((Δ - 0.2) / 0.2, 0, 1)) — 끄는 중이면 Δ = 0이다.
    const double since = state.scrolling ? 0.0 : state.sinceInteraction;
    const float fade = static_cast<float>(
        math::Clamp((since - ScrollBarFadeDelay) / ScrollBarFadeDuration, 0.0, 1.0));
    const float opacity = 1.0f - fade;
    // OnHover는 보통 불투명도를 넘지 않는다(영역 안의 값과 같은 높이에서 흐려진다).
    return visibility == ScrollBarVisibility::OnHover ? math::Min(opacity, ScrollThumbNormalOpacity)
                                                      : opacity;
}

bool IsScrollThumbFading(ScrollBarVisibility visibility, const ScrollThumbState& state) noexcept {
    return UsesFade(visibility, state) &&
           (state.scrolling || state.sinceInteraction < ScrollBarFadeDelay + ScrollBarFadeDuration);
}

} // namespace rocks::ui
