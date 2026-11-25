#pragma once

#include <MaterialSchema/MaterialAssetReference.h>
#include <MaterialSchema/ParameterKey.h>
#include <Meta/Annotations.h>

// 에셋 참조 파라미터 override다(Docs/Design/MaterialSystem 4.3·19.11절). 키는 9.2절 ParameterKey다.
namespace rocks::material {

ROCKS_STRUCT()
struct AssetParameterOverride {
    ROCKS_GENERATED_BODY()

    explicit AssetParameterOverride(Allocator& allocator) noexcept : key(allocator) {
    }

    ROCKS_PROPERTY() ParameterKey key;
    // 값 종류는 파라미터 노드가 정한다(컬렉션·텍스처 컬렉션·가상 텍스처·sparse volume·글꼴 —
    // 19.11절). 대상 클래스가 없는 종류는 TODO(MPQ)다.
    ROCKS_PROPERTY() MaterialAssetReference value;
};

} // namespace rocks::material
