#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <MaterialSchema/MaterialAssetReference.h>
#include <MaterialSchema/Nodes/Outputs/MaterialCacheAttribute.h>
#include <Meta/Annotations.h>

// Material Cache Output 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionMaterialCache — RE/Public/Materials/MaterialExpressionMaterialCache.h).
// 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct MaterialCacheOutputProperties {
    ROCKS_GENERATED_BODY()

    explicit MaterialCacheOutputProperties(Allocator& allocator) noexcept : attributes(allocator) {
    }

    ROCKS_PROPERTY() bool isSample = false;
    // TODO(MPQ-VirtualTexture): 대상 에셋 클래스가 생기면 그 클래스로 검사한다(19.4.3절).
    // 기대 클래스는 MaterialCacheTag다(19.4.3절).
    ROCKS_PROPERTY() MaterialAssetReference tag;
    // 캐시에 쓰는 속성 목록이다(UE Attributes — 동적 입출력).
    ROCKS_PROPERTY() Array<MaterialCacheAttribute> attributes;
};

} // namespace rocks::material
