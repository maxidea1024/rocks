#pragma once

#include <Base/Types.h>
#include <MaterialSchema/MaterialAssetReference.h>
#include <MaterialSchema/ParameterKey.h>
#include <Meta/Annotations.h>

// 글꼴 파라미터 override다(Docs/Design/MaterialSystem 4.6.1·19.11절, UE FFontParameterValue —
// FontValue· FontPage). TODO(MPQ-Font): 미리 구운 Font 에셋이 생기면 대상 클래스와 page 범위를
// 검사한다.
namespace rocks::material {

ROCKS_STRUCT()
struct FontParameterOverride {
    ROCKS_GENERATED_BODY()

    explicit FontParameterOverride(Allocator& allocator) noexcept : key(allocator) {
    }

    ROCKS_PROPERTY() ParameterKey key;
    ROCKS_PROPERTY() MaterialAssetReference font;
    // 글꼴 텍스처 페이지 번호다.
    ROCKS_PROPERTY() int32 page = 0;
};

} // namespace rocks::material
