#pragma once

#include <Base/Guid.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/String.h>
#include <MaterialSchema/MaterialAssetReference.h>
#include <Meta/Annotations.h>

// 노드 속성 안의 보조 값이다(Docs/Design/MaterialSystem 19.13.2절, UE
// FMaterialLayersFunctionsLayer). UE의 입력 칸 (FExpressionInput)은 그래프 간선이 대신하므로 두지
// 않는다.
namespace rocks::material {

ROCKS_STRUCT()
struct LayerStackLayer {
    ROCKS_GENERATED_BODY()

    explicit LayerStackLayer(Allocator& allocator) noexcept : displayName(allocator) {
    }

    // 레이어 id다(override scope 키 — 9.2절).
    ROCKS_PROPERTY() Guid layerId {};
    // 레이어 함수다(기대 클래스 MaterialFunctionBase — 19.4.3절).
    ROCKS_PROPERTY() MaterialAssetReference layer;
    // 아래 레이어와 섞는 혼합 함수다. 첫 레이어는 비어 있다.
    ROCKS_PROPERTY() MaterialAssetReference blend;
    ROCKS_PROPERTY() bool visible = true;
    // 표시 이름이다(의미 없음).
    ROCKS_PROPERTY() String displayName;
    ROCKS_PROPERTY() bool restrictToLayerRelatives = false;
    ROCKS_PROPERTY() bool restrictToBlendRelatives = false;
};

} // namespace rocks::material
