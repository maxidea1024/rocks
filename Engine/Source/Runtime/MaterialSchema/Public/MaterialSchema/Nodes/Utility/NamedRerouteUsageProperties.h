#pragma once

#include <Base/Guid.h>
#include <Meta/Annotations.h>

// Named Reroute Usage 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionNamedRerouteUsage — RE/Public/Materials/MaterialExpressionNamedReroute.h).
// 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct NamedRerouteUsageProperties {
    ROCKS_GENERATED_BODY()

    // 선언 노드의 id다(UE Declaration·DeclarationGuid).
    ROCKS_PROPERTY() Guid declaration {};
};

} // namespace rocks::material
