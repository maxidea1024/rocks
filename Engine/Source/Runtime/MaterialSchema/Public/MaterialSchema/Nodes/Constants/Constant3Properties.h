#pragma once

#include <Core/Math/Color.h>
#include <Meta/Annotations.h>

// Constant3 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionConstant3Vector — RE/Public/Materials/MaterialExpressionConstant3Vector.h).
// 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct Constant3Properties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() LinearColor constant {};
};

} // namespace rocks::material
