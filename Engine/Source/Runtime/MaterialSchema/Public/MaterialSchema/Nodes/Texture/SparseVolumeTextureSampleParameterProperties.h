#pragma once

#include <Base/Guid.h>
#include <Core/Text/Name.h>
#include <MaterialSchema/MaterialAssetReference.h>
#include <MaterialSchema/Nodes/Texture/TextureNodeEnums.h>
#include <Meta/Annotations.h>

// Sparse Volume Texture Sample Parameter 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절,
// UE UMaterialExpressionSparseVolumeTextureSampleParameter —
// RE/Public/Materials/MaterialExpressionSparseVolumeTextureSample.h). 이름·기본값은 UE와 같다(b
// 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct SparseVolumeTextureSampleParameterProperties {
    ROCKS_GENERATED_BODY()

    // TODO(MPQ-SparseVolumeTexture): 대상 에셋 클래스가 생기면 그 클래스로 검사한다(19.4.3절).
    // 기대 클래스는 SparseVolumeTexture다(19.4.3절).
    ROCKS_PROPERTY() MaterialAssetReference sparseVolumeTexture;
    ROCKS_PROPERTY() TextureMipValueMode mipValueMode = TextureMipValueMode::None;
    ROCKS_PROPERTY() SamplerSourceMode samplerSource = SamplerSourceMode::FromTextureAsset;
    ROCKS_PROPERTY() int32 constMipValue {};
    ROCKS_PROPERTY() Name parameterName;
    ROCKS_PROPERTY() Guid parameterId {};
    // 함수 안에서 호출 위치마다가 아니라 전역 scope 하나다(Docs/Design/MaterialSystem 9.2절 MD5
    // "공유" 표시 — 6.12절 MS2e2).
    ROCKS_PROPERTY() bool shared = false;
    ROCKS_PROPERTY() Name group;
    ROCKS_PROPERTY() int32 sortPriority = 32;
};

} // namespace rocks::material
