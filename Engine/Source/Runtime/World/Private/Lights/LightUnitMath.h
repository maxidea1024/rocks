#pragma once

#include <Core/Math/MathUtility.h>
#include <World/Lights/LightUnits.h>

// 광원 단위 변환 상수·식이다(UE LocalLightComponent.cpp:146·RenderUtils.h:744 — Docs/Design/
// EngineRuntime 24.2.4절). Local·Point·Spot·Rect 광원이 같이 쓴다.
namespace rocks::light_math {

// cm²↔m²(1e4)와 옛 Unitless 배율(16)이다.
inline constexpr float SquareCentimetersPerSquareMeter = 100.0f * 100.0f;
inline constexpr float LegacyUnitlessScale = 16.0f;
// 원뿔 cos 상한 여유(UE KINDA_SMALL_NUMBER)다.
inline constexpr float ConeCosineMargin = 1.0e-4f;

inline bool IsKnownUnits(LightUnits units) noexcept {
    return static_cast<uint8>(units) <= static_cast<uint8>(LightUnits::Nits);
}

// UE EV100ToLuminance(EV100) = 2^EV100(LuminanceMax 1).
inline float Ev100ToLuminance(float ev100) noexcept {
    return platform::PlatformMath::Pow(2.0f, ev100);
}

// 1/(2π(1 − cos))다(원뿔 입체각의 역수). cos는 이미 잘린 값이다.
inline float InverseConeSolidAngle(float cosHalfConeAngle) noexcept {
    return 1.0f / (2.0f * math::Pi<float> * (1.0f - cosHalfConeAngle));
}

} // namespace rocks::light_math
