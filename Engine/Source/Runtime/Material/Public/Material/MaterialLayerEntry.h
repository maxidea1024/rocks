#pragma once

#include <Base/Guid.h>
#include <Core/Text/Name.h>
#include <Meta/Annotations.h>
#include <Object/ObjectRefs.h>

// 레이어 스택의 레이어 하나다(Docs/Design/MaterialSystem 6.3절 5항·9.2절, UE
// FMaterialLayersFunctions의 레이어·혼합·상태·Guid 한 줄). 레이어 Guid가 override scope 키라 순서를
// 바꿔도 override가 남는다.
namespace rocks {

class MaterialFunction;

ROCKS_STRUCT()
struct MaterialLayerEntry {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Guid layerId;
    // 레이어 함수(쓰임 Layer)다.
    ROCKS_PROPERTY() ObjectRef<MaterialFunction> layer;
    // 아래 레이어와 섞는 혼합 함수(쓰임 LayerBlend)다. 첫 레이어는 비어 있다.
    ROCKS_PROPERTY() ObjectRef<MaterialFunction> blend;
    ROCKS_PROPERTY() bool visible = true;
    // 표시 이름이다(의미 없음).
    ROCKS_PROPERTY() Name displayName;
};

} // namespace rocks
