#pragma once

#include <AssetRuntime/StreamableRenderAsset.h>
#include <Meta/Annotations.h>

// 스킨 에셋 기반이다(Docs/Design/MeshAnimationAssets 6.3절·6.7.1절 2, UE USkinnedAsset — 고유 저장
// 필드 없음).
namespace rocks {

// 본으로 변형하는 그리기 에셋의 공통 기반이다(추상). 지금 파생은 SkeletalMesh 하나다.
//
// 스레드: 에셋 객체 Registry의 소유 스레드 전용이다.
ROCKS_CLASS(Abstract)
class ROCKS_MESH_API SkinnedAsset : public StreamableRenderAsset {
    ROCKS_GENERATED_BODY()
};

} // namespace rocks
