#pragma once

#include <Object/AssetId.h>
#include <MaterialSchema/MaterialAssetReferenceRole.h>

// 머티리얼 그래프 노드가 참조하는 에셋 하나다(Docs/Design/MaterialSystem 19.4.3절 "참조 선언",
// 12.8절 MS3c — CollectMaterialGraphReferences가 낸다).
namespace rocks {

struct MaterialGraphReference {
    material::MaterialAssetReferenceRole role = material::MaterialAssetReferenceRole::Texture;
    AssetId asset{};
};

} // namespace rocks
