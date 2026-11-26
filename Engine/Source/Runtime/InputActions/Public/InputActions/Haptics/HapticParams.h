#pragma once

#include <Core/Text/StringView.h>

namespace rocks::input {

// 진동 효과 재생 옵션이다(UE FForceFeedbackParameters — Docs/Design/InputActions 6.2절).
// - tag: 비어 있지 않으면 같은 tag로 재생 중인 효과를 바꾼다(재생은 사본을 둔다 — 호출 뒤 버려도
//   된다).
// - looping: 길이를 넘으면 처음부터 다시다(멈출 때까지).
// - ignoreTimeDilation: 게임 시간 배율 대신 실제 경과 시간으로 재생한다(pause는 playWhilePaused가
//   정한다).
// - playWhilePaused: pause 중에도 재생하고 기여한다.
struct HapticParams {
    StringView tag;
    bool looping = false;
    bool ignoreTimeDilation = false;
    bool playWhilePaused = false;
};

} // namespace rocks::input
