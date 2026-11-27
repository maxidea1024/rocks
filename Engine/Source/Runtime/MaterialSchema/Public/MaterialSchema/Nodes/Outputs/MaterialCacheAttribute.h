#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>

// 노드 속성 안의 보조 값이다(Docs/Design/MaterialSystem 19.13.2절, UE
// FMaterialExpressionMaterialCacheAttribute). UE의 입력 칸 (FExpressionInput)은 그래프 간선이
// 대신하므로 두지 않는다.
namespace rocks::material {

ROCKS_STRUCT()
struct MaterialCacheAttribute {
    ROCKS_GENERATED_BODY()

    explicit MaterialCacheAttribute(Allocator& allocator) noexcept : decoration(allocator) {
    }

    // 핀 표시 이름이다(동적 핀 키).
    ROCKS_PROPERTY() String decoration;
    // MaterialPinType 값이다(UE EMaterialValueType 대신 Rocks 핀 타입 번호).
    ROCKS_PROPERTY() uint32 valueType = 0;
};

} // namespace rocks::material
