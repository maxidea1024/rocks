#pragma once

#include <Meta/Annotations.h>
#include <World/Actor.h>
#include <World/Lights/Light.h>

namespace rocks {

// 사각 면 광원 Actor다(UE ARectLight). root는 RectLightComponent다.
ROCKS_CLASS()
class ROCKS_WORLD_API RectLight : public Light {
    ROCKS_GENERATED_BODY()

public:
    explicit RectLight(Allocator& allocator) noexcept : Light(allocator) {
    }

protected:
    Result<void> OnFrameworkPreAwake() noexcept final;
};

} // namespace rocks
