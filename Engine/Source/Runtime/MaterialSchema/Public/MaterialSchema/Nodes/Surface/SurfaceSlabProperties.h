#pragma once

#include <MaterialSchema/MaterialAssetReference.h>
#include <MaterialSchema/Nodes/Surface/SurfaceNodeEnums.h>
#include <Meta/Annotations.h>

// Surface Slab 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionSubstrateSlabBSDF — RE/Public/Materials/MaterialExpressionSubstrate.h).
// 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct SurfaceSlabProperties {
    ROCKS_GENERATED_BODY()

    // TODO(MPQ-SubsurfaceProfile): 대상 에셋 클래스가 생기면 그 클래스로 검사한다(19.4.3절).
    // 기대 클래스는 SubsurfaceProfile다(19.4.3절).
    ROCKS_PROPERTY() MaterialAssetReference subsurfaceProfile;
    // TODO(MPQ-SpecularProfile): 대상 에셋 클래스가 생기면 그 클래스로 검사한다(19.4.3절).
    // 기대 클래스는 SpecularProfile다(19.4.3절).
    ROCKS_PROPERTY() MaterialAssetReference specularProfile;
    ROCKS_PROPERTY() SubsurfaceType subSurfaceType = SubsurfaceType::None;
};

} // namespace rocks::material
