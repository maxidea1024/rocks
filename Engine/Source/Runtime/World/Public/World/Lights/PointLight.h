#pragma once

#include <Meta/Annotations.h>
#include <World/Actor.h>
#include <World/Lights/Light.h>

namespace rocks {

// 점 광원 Actor다(UE APointLight). root는 PointLightComponent다.
ROCKS_CLASS()
class ROCKS_WORLD_API PointLight : public Light {
    ROCKS_GENERATED_BODY()

public:
    explicit PointLight(Allocator& allocator) noexcept : Light(allocator) {
    }

protected:
    Result<void> OnFrameworkPreAwake() noexcept final;
};

} // namespace rocks
