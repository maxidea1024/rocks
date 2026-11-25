#pragma once

#include <Meta/Annotations.h>
#include <World/Actor.h>
#include <World/Lights/Light.h>

namespace rocks {

// 방향광 Actor다(UE ADirectionalLight). root는 DirectionalLightComponent이고 상대 회전은 pitch
// −46°다.
ROCKS_CLASS()
class ROCKS_WORLD_API DirectionalLight : public Light {
    ROCKS_GENERATED_BODY()

public:
    // 새 root의 기본 pitch(도, UE Light.cpp:245)다.
    static constexpr double DefaultPitchDegrees = -46.0;

    explicit DirectionalLight(Allocator& allocator) noexcept : Light(allocator) {
    }

protected:
    Result<void> OnFrameworkPreAwake() noexcept final;
};

} // namespace rocks
