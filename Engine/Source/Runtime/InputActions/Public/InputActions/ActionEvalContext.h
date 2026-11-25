#pragma once

#include <Base/Types.h>

namespace rocks::input {

// 평가 한 번의 문맥이다(Docs/Design/InputActions 4.5절).
struct ActionEvalContext {
    // dilation을 뺀 dt(초)다(게임 TickContext::unscaledDeltaSeconds — UE NonDilatedDeltaTime).
    double deltaSeconds = 0.0;
    // 시간 트리거가 affectedByTimeDilation일 때 곱한다.
    double timeDilation = 1.0;
    bool paused = false;
    // 카메라 FOV(도 — 0 이하면 카메라 없음)와 플레이어 여부다(FovScaling — 3.1절).
    float fov = 0.0f;
    bool hasPlayer = false;
    // 이 플레이어의 패드 번호다.
    uint32 gamepad = 0;
};

} // namespace rocks::input
