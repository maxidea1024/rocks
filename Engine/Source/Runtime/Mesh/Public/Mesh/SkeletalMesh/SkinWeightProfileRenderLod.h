#pragma once

#include <Core/Text/Name.h>
#include <Mesh/Common/MeshRenderStream.h>
#include <Meta/Annotations.h>

// 쿠킹 LOD 하나의 가중치 프로필 영향이다(Docs/Design/MeshAnimationAssets 6.7.1절 6, UE
// FSkinWeightProfilesData). 기본 영향과 같은 형식·배치·bone map을 쓴다.
namespace rocks {

// 프로필 이름과 영향 번호·가중치 stream이다(LOD 덩어리 안).
//
// 스레드: 값 타입이다(동기화 없음).
ROCKS_STRUCT()
struct SkinWeightProfileRenderLod {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Name name;
    ROCKS_PROPERTY() MeshRenderStream boneIndices;
    ROCKS_PROPERTY() MeshRenderStream boneWeights;
};

} // namespace rocks
