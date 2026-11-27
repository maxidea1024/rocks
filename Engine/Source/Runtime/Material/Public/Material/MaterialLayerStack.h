#pragma once

#include <Base/Guid.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Material/MaterialLayerEntry.h>
#include <Meta/Annotations.h>
#include <Object/StructRefArray.h>

// 레이어 스택이다(Docs/Design/MaterialSystem 4.6.1·6.3절 5항, 단계 MS7). 인스턴스가 덮으면 static
// 변형이다. EditorOnly 필드로만 쓴다(번역 입력 — 실행 때 필요 없다).
namespace rocks {

ROCKS_STRUCT()
struct MaterialLayerStack {
    ROCKS_GENERATED_BODY()

    explicit MaterialLayerStack(Allocator& allocator) noexcept
        : layers(allocator), deletedParentLayerIds(allocator) {
    }

    // 아래(0)부터 위 순서다.
    ROCKS_PROPERTY() StructRefArray<MaterialLayerEntry> layers;
    // 부모에서 왔다가 이 인스턴스에서 지운 레이어 id다(UE DeletedParentLayerGuids).
    ROCKS_PROPERTY() Array<Guid> deletedParentLayerIds;
};

} // namespace rocks
