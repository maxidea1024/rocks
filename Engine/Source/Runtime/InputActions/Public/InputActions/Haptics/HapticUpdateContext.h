#pragma once

namespace rocks::input {

// 진동 합성 한 번의 문맥이다(Docs/Design/InputActions 6.3절 — PlayerController가 TickContext와
// input.* 설정으로 채운다).
// - deltaSeconds: 게임 dt(시간 배율 적용), unscaledDeltaSeconds: 실제 경과(ignoreTimeDilation).
// - paused: pause 중이면 playWhilePaused 효과만 갱신·기여하고 동적 진동은 멈춘다.
// - scale: input.haptic_scale(곱한 뒤 [0, 1]로 자른다 — 비유한 값은 0), enabled:
// input.enable_haptics.
struct HapticUpdateContext {
    double deltaSeconds = 0.0;
    double unscaledDeltaSeconds = 0.0;
    bool paused = false;
    float scale = 1.0f;
    bool enabled = true;
};

} // namespace rocks::input
