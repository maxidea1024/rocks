#pragma once

#include <Core/Math/Color.h>
#include <Core/Text/Name.h>
#include <Meta/Annotations.h>

// Named Reroute Declaration 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionNamedRerouteDeclaration —
// RE/Public/Materials/MaterialExpressionNamedReroute.h). 이름·기본값은 UE와 같다(b 접두를 떼고
// camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct NamedRerouteDeclarationProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Name name;
    // UE는 경로로 정한 결정적 무작위 색을 준다. 새 노드의 색은 에디터가 정한다.
    ROCKS_PROPERTY() LinearColor nodeColor = LinearColor(1.0f, 1.0f, 1.0f, 1.0f);
};

} // namespace rocks::material
