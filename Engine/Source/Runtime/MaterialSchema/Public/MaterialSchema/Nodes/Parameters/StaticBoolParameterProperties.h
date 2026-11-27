#pragma once

#include <Base/Guid.h>
#include <Core/Text/Name.h>
#include <Meta/Annotations.h>

// Static Bool Parameter 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionStaticBoolParameter —
// RE/Public/Materials/MaterialExpressionStaticBoolParameter.h). 이름·기본값은 UE와 같다(b 접두를
// 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct StaticBoolParameterProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Name parameterName;
    ROCKS_PROPERTY() Guid parameterId {};
    // 함수 안에서 호출 위치마다가 아니라 전역 scope 하나다(Docs/Design/MaterialSystem 9.2절 MD5
    // "공유" 표시 — 6.12절 MS2e2).
    ROCKS_PROPERTY() bool shared = false;
    ROCKS_PROPERTY() Name group;
    ROCKS_PROPERTY() int32 sortPriority = 32;
    ROCKS_PROPERTY() bool defaultValue {};
};

} // namespace rocks::material
