#pragma once

#include <Meta/Annotations.h>

// 머티리얼의 조명 굽기 설정이다(Docs/Design/MaterialSystem 19.12절, UE
// FLightmassMaterialInterfaceSettings).
// TODO 필드다: 정적 조명 굽기 도구가 생기면 그 도구가 읽는다. 기본값은 UE와 같다.
namespace rocks {

ROCKS_STRUCT()
struct MaterialLightBakeSettings {
    ROCKS_GENERATED_BODY()

    // 그림자를 Masked처럼 굽는다(UE bCastShadowAsMasked).
    ROCKS_PROPERTY() bool castShadowAsMasked = false;
    ROCKS_PROPERTY() float emissiveBoost = 1.0f;
    ROCKS_PROPERTY() float diffuseBoost = 1.0f;
    ROCKS_PROPERTY() float exportResolutionScale = 1.0f;
};

} // namespace rocks
