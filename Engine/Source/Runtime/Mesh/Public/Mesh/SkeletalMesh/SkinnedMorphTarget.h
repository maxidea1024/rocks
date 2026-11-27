#pragma once

#include <Core/Text/Name.h>
#include <Mesh/Common/MeshRenderStream.h>
#include <Meta/Annotations.h>

// 쿠킹 morph target 하나다(Docs/Design/MeshAnimationAssets 6.3절·6.7.1절 6, UE
// FMorphTargetLODModel의 희소 변화량). 변화량 항목은 LOD 덩어리 안 stream이다.
namespace rocks {

// morph 변화량 항목 하나의 byte 수다: 렌더 정점 번호 uint32 + 위치 변화량 Half4 + 법선 변화량
// Half4(w = 0).
inline constexpr uint64 SkinnedMorphDeltaBytes = 20;

// 이름(원본 Morph-<이름>)·항목 수·항목 stream이다. stream의 vertexFormat은 MeshRenderStreamNone이고
// byteSize = deltaCount × SkinnedMorphDeltaBytes다. 항목은 렌더 정점 번호 오름차순이다.
//
// 스레드: 값 타입이다(동기화 없음).
ROCKS_STRUCT()
struct SkinnedMorphTarget {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Name name;
    ROCKS_PROPERTY() uint32 deltaCount = 0;
    ROCKS_PROPERTY() MeshRenderStream deltas;
};

} // namespace rocks
