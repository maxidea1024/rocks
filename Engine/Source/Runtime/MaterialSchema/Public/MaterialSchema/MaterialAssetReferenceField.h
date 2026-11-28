#pragma once

#include <Core/Text/StringView.h>
#include <MaterialSchema/MaterialAssetReferenceRole.h>

// 노드 정의가 선언하는 에셋 참조 필드 하나다(Docs/Design/MaterialSystem 19.4.3절 "참조 선언").
// field는 속성 struct의 최상위 MaterialAssetReference 필드 이름이다(등록 검사).
//
// 사용 예:
//   constexpr MaterialAssetReferenceField SampleReferences[] = {
//       {"texture", MaterialAssetReferenceRole::Texture}};
//   MaterialNode("Material.TextureSample").AssetReferences(SampleReferences)
namespace rocks::material {

struct MaterialAssetReferenceField {
    StringView field;
    MaterialAssetReferenceRole role = MaterialAssetReferenceRole::Texture;
};

} // namespace rocks::material
