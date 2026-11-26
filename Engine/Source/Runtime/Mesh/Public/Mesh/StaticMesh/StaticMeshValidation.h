#pragma once

#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Mesh/Common/MeshBounds.h>
#include <Mesh/StaticMesh/StaticMesh.h>
#include <MeshSource/MeshSourceTypes.h>

// 정적 메시 로드 검증과 확장 bounds다(Docs/Design/MeshAnimationAssets 5.5.1절 9). 쿠킹 데이터는
// 쿠커가 만든 것이라 위반은 손상(CorruptPackage)이다. 클래스 PostLoad는 쓰지 않고 소비자(렌더
// 연결·물리)가 쓰기 전에 부른다(텍스처와 같다 — AssetSystem 12.15절 2).
namespace rocks {

// 16비트 index LOD의 최대 정점 수다(UE는 정점 65536개까지 16비트지만 0xFFFF는 OpenGL ES 3.0 이상의
// 고정 primitive restart index라 index로 쓰지 않는다 — 5.5.2절 1).
inline constexpr uint32 MeshIndex16MaxVertices = 65535;

// 쿠킹 LOD 하나가 가질 수 있는 UV stream 수다(UV 채널 둘을 원소 하나에 묶는다 — 5.3절).
inline constexpr uint32 MeshMaxTexCoordStreams = (MeshMaxTexCoords + 1) / 2;

// renderData의 형식 번호·구간·크기·섹션·bounds를 본다(bulk byte는 읽지 않는다). LOD 0개(에디터
// 패키지)는 정상이다. data가 빈 LOD(장치 없는 프로파일 쿠킹 — 5.5.2절 3)는 모든 stream이 형식
// 0·구간 0이어야 하고 UV stream 목록이 비어야 한다. 16비트 index LOD는 정점
// MeshIndex16MaxVertices개까지, 빈 섹션은 정점 범위 0, 0이다. 섹션의 materialIndex는 보지
// 않는다(슬롯 범위 밖은 엔진 기본 머티리얼 — UE와 같다).
//
// 사용 예(업로드 전):
//   MeshDiagnostic failure;
//   if (auto checked = ValidateStaticMeshRenderData(mesh.renderData, &failure); !checked) {
//       Report(MeshIssueName(failure.issue), failure.index);   // index = LOD, count = 섹션·UV
//       stream
//   }
//
// 오류: 위반은 failure->code CorruptPackage, ErrorCode InvalidArgument와 issue(Render*), index =
// LOD 번호(bounds·LOD 수는 MeshNoIndex), count = 섹션 또는 UV stream 번호(그 밖 0)다. 스레드:
// 읽기만 한다.
ROCKS_MESH_API Result<void>
ValidateStaticMeshRenderData(const StaticMeshRenderData& renderData,
                             MeshDiagnostic* failure = nullptr) noexcept;

// lod의 index 값이 모두 vertexCount 미만인지 본다. data는 lod.data의 byte 전체다(크기가
// lod.data.rawBytes와 같아야 한다). 먼저 ValidateStaticMeshRenderData를 통과한 lod여야 한다.
//
// 사용 예(bulk를 읽은 뒤 업로드 전):
//   auto bytes = package.BulkBytes(lod.data);
//   if (bytes && !ValidateStaticMeshIndices(lod, lodIndex, bytes.Value(), &failure)) { ... }
//
// 오류: 크기 불일치 RenderStreamRange, 범위 밖 index RenderIndexValue(count = index 위치) — 모두
// CorruptPackage·InvalidArgument, index = lodIndex.
ROCKS_MESH_API Result<void> ValidateStaticMeshIndices(const StaticMeshRenderLod& lod,
                                                      uint32 lodIndex, Span<const uint8> data,
                                                      MeshDiagnostic* failure = nullptr) noexcept;

// mesh의 단순 충돌(값과 볼록 형상 bulk — MA3a 함수)과 renderData를 차례로 본다. 충돌 위반은
// collisionFailure, 렌더 데이터 위반은 meshFailure에 채운다(먼저 걸린 쪽 하나). scratch는 볼록 형상
// 임시 배열의 할당기다.
//
// 오류: ValidateSimpleCollision·ValidateSimpleCollisionBulk·ValidateStaticMeshRenderData의 오류다.
// 스레드: package를 읽으므로 그 Registry 소유 스레드에서 부른다.
ROCKS_MESH_API Result<void>
ValidateStaticMesh(const StaticMesh& mesh, const Package& package, Allocator& scratch,
                   MeshDiagnostic* meshFailure = nullptr,
                   CollisionDiagnostic* collisionFailure = nullptr) noexcept;

// 확장 bounds다(UE ExtendedBounds — StaticMesh.cpp CalculateExtendedBounds). 두 확장이 모두 0이면
// renderData의 상자·반지름 그대로이고, 아니면 상자 최소에서 negativeBoundsExtension을 빼고 최대에
// positiveBoundsExtension을 더한 상자와 그 반 대각선 길이 반지름이다(UE FBoxSphereBounds(FBox)).
// 컬링·스트리밍 화면 크기처럼 메시가 차지하는 공간이 필요한 소비자가 쓴다.
//
// 사용 예:
//   const MeshBounds bounds = StaticMeshExtendedBounds(mesh);
//   if (bounds.sphereRadius > 0.0) { ... }
//
// 스레드: mesh를 읽기만 한다.
ROCKS_MESH_API MeshBounds StaticMeshExtendedBounds(const StaticMesh& mesh) noexcept;

} // namespace rocks
