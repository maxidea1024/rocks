#pragma once

#include <Core/Memory/Allocator.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>

// 노드 속성 안의 보조 값이다(Docs/Design/MaterialSystem 19.13.2절, UE FParameterChannelNames). UE의
// 입력 칸 (FExpressionInput)은 그래프 간선이 대신하므로 두지 않는다.
namespace rocks::material {

ROCKS_STRUCT()
struct ParameterChannelNames {
    ROCKS_GENERATED_BODY()

    explicit ParameterChannelNames(Allocator& allocator) noexcept
        : r(allocator), g(allocator), b(allocator), a(allocator) {
    }

    ROCKS_PROPERTY() String r;
    ROCKS_PROPERTY() String g;
    ROCKS_PROPERTY() String b;
    ROCKS_PROPERTY() String a;
};

} // namespace rocks::material
