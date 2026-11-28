#pragma once

#include <Object/AssetId.h>
#include <Base/Guid.h>
#include <Core/Text/StringView.h>
#include <MaterialSchema/MaterialAssetReferenceRole.h>

// 머티리얼 그래프 노드가 참조하는 에셋 하나다(Docs/Design/MaterialSystem 19.4.3절 "참조 선언",
// 12.8절 MS3c — CollectMaterialGraphReferences가 낸다).
namespace rocks {

struct MaterialGraphReference {
    material::MaterialAssetReferenceRole role = material::MaterialAssetReferenceRole::Texture;
    AssetId asset{};
    // 참조한 노드와 속성 필드다(번역 키의 텍스처 참조 위치 — 12.2절, 레이어는 "layer"·"blend").
    // field는 정적 문자열이다(노드 정의 선언 또는 리터럴).
    Guid node{};
    StringView field;
    // 필드가 파라미터 기본값인가: 파라미터 노드(Parameter 표지)이고 이름(parameterName)이 있으며
    // 필드가 비의미 필드(semantic hash 밖 — 5.5절)다. 이름이 빈 파라미터 샘플 노드는 UE처럼 보통
    // 샘플이라 기본값이 아니다. 번역 키는 이런 텍스처 위치만 AssetId 없이 사실로 쓴다(12.8절
    // MS3d1).
    bool parameterDefault = false;
};

} // namespace rocks
