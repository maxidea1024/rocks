#pragma once

#include <Core/Math/MathUtility.h>

// EngineAudio 안에서 같이 쓰는 값 범위 검사다(유니티 묶음에서 파일마다 같은 이름을 두지 않는다).
namespace rocks::engine_audio_detail {

// value가 유한하고 [low, high] 안이다.
inline bool IsFiniteInRange(float value, float low, float high) noexcept {
    return math::IsFinite(value) && value >= low && value <= high;
}

} // namespace rocks::engine_audio_detail
