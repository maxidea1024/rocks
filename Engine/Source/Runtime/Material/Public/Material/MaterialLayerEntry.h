#pragma once

#include <Base/Guid.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/String.h>
#include <Material/MaterialLayerLinkState.h>
#include <Meta/Annotations.h>
#include <Object/ObjectRefs.h>

// 레이어 스택의 레이어 하나다(Docs/Design/MaterialSystem 4.6.1·6.3절 5항·9.2절, UE
// FMaterialLayersFunctions의 한 줄 — Layers·Blends·LayerStates·LayerNames·RestrictTo*Relatives·
// LayerGuids·LayerLinkStates). 레이어 Guid가 override scope 키라 순서를 바꿔도 override가 남는다.
// 이 struct는 EditorOnly 필드(레이어 스택) 안에만 놓인다.
namespace rocks {

class MaterialFunctionBase;

ROCKS_STRUCT()
struct MaterialLayerEntry {
    ROCKS_GENERATED_BODY()

    explicit MaterialLayerEntry(Allocator& allocator) noexcept : displayName(allocator) {
    }

    ROCKS_PROPERTY() Guid layerId;
    // 레이어 함수(쓰임 Layer — 함수 또는 함수 인스턴스)다.
    ROCKS_PROPERTY() ObjectRef<MaterialFunctionBase> layer;
    // 아래 레이어와 섞는 혼합 함수(쓰임 LayerBlend)다. 첫 레이어는 비어 있다.
    ROCKS_PROPERTY() ObjectRef<MaterialFunctionBase> blend;
    ROCKS_PROPERTY() bool visible = true;
    // 표시 이름이다(의미 없음, UE LayerNames).
    ROCKS_PROPERTY() String displayName;
    // 고를 수 있는 레이어·혼합을 부모와 친척으로 제한한다(UE RestrictToLayerRelatives·
    // RestrictToBlendRelatives).
    ROCKS_PROPERTY() bool restrictToLayerRelatives = false;
    ROCKS_PROPERTY() bool restrictToBlendRelatives = false;
    ROCKS_PROPERTY() MaterialLayerLinkState linkState = MaterialLayerLinkState::NotFromParent;
};

} // namespace rocks
