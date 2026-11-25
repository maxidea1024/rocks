#pragma once

#include <Meta/Annotations.h>
#include <World/Actor.h>
#include <World/Lights/Light.h>

namespace rocks {

// 원뿔 광원 Actor다(UE ASpotLight). root는 SpotLightComponent이고 상대 회전은 pitch −90°(아래)다.
ROCKS_CLASS()
class ROCKS_WORLD_API SpotLight : public Light {
    ROCKS_GENERATED_BODY()

public:
    // 새 root의 기본 pitch(도, UE SpotLight.cpp:28)다.
    static constexpr double DefaultPitchDegrees = -90.0;

    explicit SpotLight(Allocator& allocator) noexcept : Light(allocator) {
    }

protected:
    Result<void> OnFrameworkPreAwake() noexcept final;
};

} // namespace rocks
