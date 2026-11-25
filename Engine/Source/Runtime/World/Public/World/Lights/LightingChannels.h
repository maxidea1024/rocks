#pragma once

#include <Meta/Annotations.h>

namespace rocks {

// 광원 채널이다(UE FLightingChannels — EngineTypes.h:568). 같은 채널의 primitive만 빛을 받는다.
// 광원(D1)과 PrimitiveComponent(D2)가 같이 쓴다(Docs/Design/EngineRuntime 24.2.2·24.3.3절).
ROCKS_STRUCT()
struct LightingChannels {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() bool channel0 = true;
    ROCKS_PROPERTY() bool channel1 = false;
    ROCKS_PROPERTY() bool channel2 = false;

    friend constexpr bool operator==(const LightingChannels&,
                                     const LightingChannels&) noexcept = default;
};

} // namespace rocks
