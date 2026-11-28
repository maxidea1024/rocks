#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>

// 번역 값의 표 번호 → Material 필드 번호다(Docs/Design/MaterialSystem 12.8절 MS3d2·MS3d3 —
// MakeMaterialArtifact가 artifact를 순수 값으로 만들 때 쓴다). 처리기가 파라미터 표·
// referencedTextures·parameterCollections에서 찾아 채운다.
namespace rocks {

struct MaterialArtifactLinks {
    // 값 파라미터 표(IR 순)마다 Material::parameters 번호다.
    Span<const uint32> parameters{};
    // 값 텍스처 slot마다 Material::referencedTextures 번호다.
    Span<const uint32> textures{};
    // 값 텍스처 slot마다 Material::parameters 번호다(참조 텍스처는 NoMaterialReference).
    Span<const uint32> textureParameters{};
    // 값 컬렉션(모듈 순)마다 Material::parameterCollections 번호다.
    Span<const uint32> collections{};
};

} // namespace rocks
