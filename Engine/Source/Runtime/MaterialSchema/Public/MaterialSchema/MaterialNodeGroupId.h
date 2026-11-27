#pragma once

#include <Base/Types.h>

// 머티리얼 노드 정의 등록 묶음 하나의 식별이다(MaterialNodeCatalog). 0은 없음이다.
namespace rocks::material {

struct MaterialNodeGroupId {
    uint64 value = 0;
};

} // namespace rocks::material
