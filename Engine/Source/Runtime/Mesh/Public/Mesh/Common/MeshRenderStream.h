#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// 쿠킹 렌더 LOD 덩어리 안의 stream 구간이다(Docs/Design/MeshAnimationAssets 5.3절·5.5.1절 9).
// 정적·스킨 메시의 쿠킹 LOD가 함께 쓴다.
namespace rocks {

// stream 시작 정렬이다(LOD 덩어리 안 — 사이는 0으로 채운다, RenderBase 6.5절 계약·텍스처 상주
// 꼬리와 같은 값).
inline constexpr uint64 MeshRenderStreamAlignment = 16;

// "stream 없음" 형식 번호다(색 stream이 없는 LOD 등).
inline constexpr uint32 MeshRenderStreamNone = 0;

// LOD 덩어리(BulkDataRef 하나) 안의 stream 하나다. vertexFormat은 GraphicsTypes VertexFormat 고정
// 번호(MeshRenderStreamNone = 없음)이고 index 구간은 0이다(형식은 LOD의 indexFormat). 로드 때
// ValidateStaticMeshRenderData가 형식·구간을 검사한다.
//
// 스레드: 값 타입이다(동기화 없음).
ROCKS_STRUCT()
struct MeshRenderStream {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() uint32 vertexFormat = MeshRenderStreamNone;
    // 덩어리 안 시작 byte(MeshRenderStreamAlignment 배수)와 byte 수다.
    ROCKS_PROPERTY() uint64 offset = 0;
    ROCKS_PROPERTY() uint64 byteSize = 0;
};

} // namespace rocks
