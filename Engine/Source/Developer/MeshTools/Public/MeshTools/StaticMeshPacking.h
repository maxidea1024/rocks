#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Mesh/Common/MeshSectionInfo.h>
#include <Mesh/StaticMesh/StaticMeshRenderLod.h>
#include <Mesh/StaticMesh/StaticMeshValidation.h>
#include <MeshTools/StaticMeshBuilder.h>

// 정적 메시 쿠킹 LOD 패킹이다(Docs/Design/MeshAnimationAssets 5.3절·5.5.2절 1, UE
// FStaticMeshVertexBuffer·FPositionVertexBuffer·FColorVertexBuffer·FRawStaticIndexBuffer 배치).
namespace rocks {

// 쿠킹 정점 색 형식이다(대상 프로파일이 정한다 — 5.5.2절 2). 값은 너겟 키에 들어간다.
enum class MeshColorFormat : uint8 {
    // D3D·Vulkan·Metal(B·G·R·A).
    Bgra8Unorm,
    // OpenGL ES(R·G·B·A — GL에 BGRA 정점 배열이 없다).
    UByte4Unorm
};

// built를 쿠킹 LOD 배치로 바꾼다: 위치 Float3 → tangent 기저(Byte4Snorm,
// useHighPrecisionTangentBasis면 Short4Snorm — 정점마다 (tangent xyz, 1), (법선 xyz, 쌍법선 부호))
// → UV 채널 쌍(Half4·Float4, useFullPrecisionUVs면 Float, 홀수 마지막은 Half2·Float2) → 색(8비트
// sRGB, 원본에 색이 없거나 모두 흰색이면 없음) → index(정점 수 ≤ MeshIndex16MaxVertices면 16비트).
// 각 시작은 16 byte 정렬이고 사이는 0이다. sections는 섹션 번호마다의 유효 섹션 정보(짧으면
// 나머지는 MeshSectionInfo 기본값 — materialIndex는 늘 built의 값을 쓴다)이고 쿠킹 섹션의 플래그
// 다섯을 채운다.
//
// 사용 예:
//   Array<uint8> bytes(allocator);
//   StaticMeshRenderLod lod(allocator);
//   if (auto packed = PackStaticMeshLod(built, settings, sections, MeshColorFormat::Bgra8Unorm,
//   bytes, lod);
//       !packed) { return packed; }
//
// 결과: bytes는 LOD 덩어리 전체로 바뀌고(앞 내용은 버린다), lod의 vertexCount·stream·indexFormat·
// sections가 채워진다(screenSize·data는 호출자가 정한다). 같은 입력이면 같은 byte다.
//
// 오류: 할당 실패(실패하면 bytes·lod는 지정되지 않은 상태 — 호출자가 버린다), 정점 수·index 수가
// uint32 구간 크기를 넘으면 CapacityExceeded.
ROCKS_MESHTOOLS_API Result<void> PackStaticMeshLod(const StaticMeshBuildResult& built,
                                                   const MeshBuildSettings& settings,
                                                   Span<const MeshSectionInfo> sections,
                                                   MeshColorFormat colorFormat, Array<uint8>& bytes,
                                                   StaticMeshRenderLod& lod) noexcept;

} // namespace rocks
