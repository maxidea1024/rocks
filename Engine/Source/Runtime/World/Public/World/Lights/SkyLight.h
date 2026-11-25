#pragma once

#include <Meta/Annotations.h>
#include <World/Actor.h>
#include <World/Lights/Light.h>

namespace rocks {

// 하늘광 Actor다(UE ASkyLight — UE는 AInfo 파생, Rocks에는 Info가 없어 Light 파생이다). root는
// SkyLightComponent다(Mobility는 컴포넌트 기본 Stationary).
ROCKS_CLASS()
class ROCKS_WORLD_API SkyLight : public Light {
    ROCKS_GENERATED_BODY()

public:
    explicit SkyLight(Allocator& allocator) noexcept : Light(allocator) {
    }

protected:
    Result<void> OnFrameworkPreAwake() noexcept final;
};

} // namespace rocks
