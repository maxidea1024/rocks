#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>

// 노드 속성 안의 보조 값이다(Docs/Design/MaterialSystem 19.13.2절, UE FCustomDefine). UE의 입력 칸
// (FExpressionInput)은 그래프 간선이 대신하므로 두지 않는다.
namespace rocks::material {

ROCKS_STRUCT()
struct CustomDefine {
    ROCKS_GENERATED_BODY()

    explicit CustomDefine(Allocator& allocator) noexcept
        : defineName(allocator), defineValue(allocator) {
    }

    ROCKS_PROPERTY() String defineName;
    ROCKS_PROPERTY() String defineValue;
};

} // namespace rocks::material
