#pragma once

#include <AssetRuntime/BulkData.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Mesh/Common/MeshRenderSection.h>
#include <Mesh/Common/MeshRenderStream.h>

// 정적 메시 쿠킹 LOD 하나다(Docs/Design/MeshAnimationAssets 5.3절·5.5.2절 1, UE
// FStaticMeshLODResources). LOD 하나가 스트리밍 단위이고 그 stream·index를 bulk 하나(data)에
// 잇는다.
namespace rocks {

// 배치(data 안 순서): positions → tangentBasis → texCoords(채널 쌍마다) → colors → indices, 시작은
// MeshRenderStreamAlignment 정렬이다.
// - positions: Float3, 정점마다 하나.
// - tangentBasis: Byte4Snorm 또는 Short4Snorm, 정점마다 원소 둘 — (tangent xyz, 1), (법선 xyz,
//   쌍법선 부호)(UE TangentX·TangentZ 순서).
// - texCoords: UV 채널 두 개를 원소 하나에(Half4·Float4), 홀수 채널의 마지막은 Half2·Float2.
// - colors: Bgra8Unorm 또는 UByte4Unorm(8비트 sRGB), 없으면 vertexFormat MeshRenderStreamNone.
// - indices: indexFormat(GraphicsTypes IndexFormat 번호)의 index, 수는 byteSize / 폭(3의 배수).
//
// 스레드: 값 타입이다(동기화 없음).
ROCKS_STRUCT()
struct StaticMeshRenderLod {
    ROCKS_GENERATED_BODY()

    explicit StaticMeshRenderLod(Allocator& allocator) noexcept
        : texCoords(allocator), sections(allocator) {
    }

    // 쿠킹 프로파일의 화면 크기다(5.5.2절 8).
    ROCKS_PROPERTY() float screenSize = 0.0f;
    ROCKS_PROPERTY() uint32 vertexCount = 0;
    // 이 LOD의 덩어리다(쿠킹 패키지 bulk — 4 KiB 정렬).
    ROCKS_PROPERTY() BulkDataRef data;
    ROCKS_PROPERTY() MeshRenderStream positions;
    ROCKS_PROPERTY() MeshRenderStream tangentBasis;
    ROCKS_PROPERTY() Array<MeshRenderStream> texCoords;
    ROCKS_PROPERTY() MeshRenderStream colors;
    ROCKS_PROPERTY() uint32 indexFormat = 0;
    ROCKS_PROPERTY() MeshRenderStream indices;
    ROCKS_PROPERTY() Array<MeshRenderSection> sections;
};

} // namespace rocks
