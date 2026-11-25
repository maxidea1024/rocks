#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Material/MaterialLayerEntry.h>
#include <Meta/Annotations.h>
#include <Object/StructRefArray.h>

// 레이어 스택이다(Docs/Design/MaterialSystem 6.3절 5항, 단계 MS7). 인스턴스가 덮으면 static
// 변형이다.
namespace rocks {

ROCKS_STRUCT()
struct MaterialLayerStack {
    ROCKS_GENERATED_BODY()

    explicit MaterialLayerStack(Allocator& allocator) noexcept : layers(allocator) {
    }

    // 아래(0)부터 위 순서다.
    ROCKS_PROPERTY() StructRefArray<MaterialLayerEntry> layers;
};

} // namespace rocks
