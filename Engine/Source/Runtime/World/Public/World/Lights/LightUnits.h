#pragma once

#include <Core/Math/Color.h>
#include <Core/Math/MathUtility.h>
#include <Core/Math/Shapes.h>
#include <Core/Math/Vector3.h>
#include <Meta/Annotations.h>
#include <World/Lights/LightingChannels.h>
#include <World/SceneComponent.h>

namespace rocks {

// 광원 세기의 단위다(UE ELightUnits — E/Classes/Engine/Scene.h:83). EV는 EV100이다.
ROCKS_ENUM()
enum class LightUnits : uint8 {
    Unitless,
    Candelas,
    Lumens,
    EV,
    Nits
};

} // namespace rocks
