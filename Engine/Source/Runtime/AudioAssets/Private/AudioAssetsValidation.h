#pragma once

#include <Core/Math/MathUtility.h>

// AudioAssets 값 검사의 공용 도움 함수다(스냅숏·분류·믹스 검사가 같이 쓴다).
namespace rocks::audio_assets_detail {

inline bool IsFiniteWithin(float value, float low, float high) noexcept {
    return math::IsFinite(value) && value >= low && value <= high;
}

inline bool IsNonNegativeFinite(float value) noexcept {
    return math::IsFinite(value) && value >= 0.0f;
}

} // namespace rocks::audio_assets_detail
