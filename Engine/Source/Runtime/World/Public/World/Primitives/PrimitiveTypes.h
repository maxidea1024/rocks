#pragma once

#include <Core/Text/String.h>
#include <Meta/Annotations.h>
#include <World/Lights/LightingChannels.h>
#include <World/SceneComponent.h>

namespace rocks {

// 캐릭터가 밟고 오를 수 있는지다(UE ECanBeCharacterBase — PrimitiveComponent.h:87).
ROCKS_ENUM()
enum class CanBeCharacterBase : uint8 {
    No,
    Yes,
    // 소유 Actor가 정한다(UE AActor::CanBeBaseForCharacter).
    Owner
};

// custom depth stencil 쓰기 mask다(UE ERendererStencilMask — PrimitiveComponent.h:122, 순번 값).
ROCKS_ENUM()
enum class RendererStencilMask : uint8 {
    Default,
    Mask255,
    Mask1,
    Mask2,
    Mask4,
    Mask8,
    Mask16,
    Mask32,
    Mask64,
    Mask128
};

} // namespace rocks
