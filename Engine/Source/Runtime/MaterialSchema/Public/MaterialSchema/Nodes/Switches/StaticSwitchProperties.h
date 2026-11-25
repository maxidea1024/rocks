#pragma once

#include <Meta/Annotations.h>

// Static Switch 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionStaticSwitch — RE/Public/Materials/MaterialExpressionStaticSwitch.h).
// 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct StaticSwitchProperties {
    ROCKS_GENERATED_BODY()

    // Value 입력이 연결되지 않았을 때의 static 값이다.
    ROCKS_PROPERTY() bool defaultValue {};
};

} // namespace rocks::material
