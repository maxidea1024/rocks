#pragma once

#include <Base/Types.h>
#include <Core/Math/Vector3.h>
#include <InputActions/InputValue.h>

namespace rocks::input {

// Smooth의 첫 표본 시간(초 — UE SMOOTH_TOTAL_SAMPLE_TIME_DEFAULT)과 초기화하는 긴 프레임(초)이다.
inline constexpr double SmoothInitialSampleTime = 0.0083;
inline constexpr double SmoothResetDeltaSeconds = 0.25;

// 모디파이어 하나의 실행 중 상태다(Smooth·SmoothDelta만 쓴다). 평가기가 바인딩마다 둔다(3절).
struct InputModifierState {
    // Smooth(UE UInputModifierSmooth).
    double zeroTime = 0.0;
    InputValue averageValue;
    uint32 samples = 0;
    double totalSampleTime = SmoothInitialSampleTime;
    // SmoothDelta(UE UInputModifierSmoothDelta).
    Vec3d oldValue;
    Vec3d delta;
};

} // namespace rocks::input
