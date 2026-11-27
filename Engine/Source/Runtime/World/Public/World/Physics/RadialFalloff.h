#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

namespace rocks {

// 방사 충격량·힘의 거리 감쇠다(UE ERadialImpulseFalloff — EngineTypes.h, Docs/Design/PhysicsModule
// 7.1.17절).
// - Constant: 범위 안이면 거리와 상관없이 strength 그대로다.
// - Linear: 중심에서 strength, 범위 끝에서 0으로 줄어든다(strength × (radius − 거리) / radius).
ROCKS_ENUM()
enum class RadialFalloff : uint8 {
    Constant,
    Linear
};

} // namespace rocks
