#pragma once

#include <Core/Optional.h>
#include <Meta/Annotations.h>

// 인스턴스의 조명 굽기 설정 override다(Docs/Design/MaterialSystem 4.6.1·19.12절, UE
// FLightmassMaterialInterfaceSettings의 bOverride* 넷 —
// RE/Public/Materials/MaterialInterface.h:144–182). 값이 있으면 부모 값을 덮는다.
// TODO(MPQ-LightBake): 정적 조명 굽기 도구가 생기면 그 도구가 읽는다.
namespace rocks {

ROCKS_STRUCT()
struct MaterialLightBakeOverrides {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Optional<bool> castShadowAsMasked;
    ROCKS_PROPERTY() Optional<float> emissiveBoost;
    ROCKS_PROPERTY() Optional<float> diffuseBoost;
    ROCKS_PROPERTY() Optional<float> exportResolutionScale;
};

} // namespace rocks
