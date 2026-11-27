#pragma once

#include <MaterialSchema/ParameterKey.h>
#include <Meta/Annotations.h>
#include <Object/ObjectRefs.h>
#include <Texture/Texture.h>

// 텍스처 파라미터 override다(Docs/Design/MaterialSystem 4.3·19.11절). 값은 ObjectRef<Texture>
// 멤버라 패키지 사이 Hard 간선 관찰 대상이다(AssetSystem 6.1절). 그래서 MaterialSchema가 아니라
// 텍스처 에셋 타입을 아는 이 모듈에 둔다.
namespace rocks {

ROCKS_STRUCT()
struct TextureParameterOverride {
    ROCKS_GENERATED_BODY()

    explicit TextureParameterOverride(Allocator& allocator) noexcept : key(allocator) {
    }

    ROCKS_PROPERTY() material::ParameterKey key;
    ROCKS_PROPERTY() ObjectRef<Texture> value;
};

} // namespace rocks
