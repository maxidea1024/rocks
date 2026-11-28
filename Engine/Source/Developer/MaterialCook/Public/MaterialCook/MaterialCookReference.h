#pragma once

#include <Object/AssetId.h>
#include <Object/ObjectHandle.h>
#include <MaterialSchema/MaterialAssetReferenceRole.h>

// 번역 입력 모으기가 연 참조 하나다(Docs/Design/MaterialSystem 12.8절 MS3c — 참조 동기화와 의존
// 대조가 읽는다).
namespace rocks {

struct MaterialCookReference {
    material::MaterialAssetReferenceRole role = material::MaterialAssetReferenceRole::Texture;
    AssetId asset{};
    // 대표 에셋 handle이다(열지 못했거나 역할 클래스가 아니면 null — 번역이 그 노드에서 진단한다).
    ObjectHandle object{};
};

} // namespace rocks
