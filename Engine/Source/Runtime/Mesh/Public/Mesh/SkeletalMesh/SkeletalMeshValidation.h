#pragma once

#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Mesh/SkeletalMesh/SkeletalMesh.h>
#include <Mesh/Common/MeshBounds.h>
#include <MeshSource/MeshSourceTypes.h>

// 스킨 메시 로드 검증과 확장 bounds다(Docs/Design/MeshAnimationAssets 6.7.1절 7). 정적 메시와 같이
// 쿠킹 데이터 위반은 손상(CorruptPackage)이고, 클래스 PostLoad는 쓰지 않고 소비자(렌더
// 연결·애니메이션)가 쓰기 전에 부른다.
namespace rocks {

// renderData의 형식 번호·구간·크기·섹션·bounds와 스킨 항목을 본다(bulk byte는 읽지 않는다). 정적
// 메시 규칙(ValidateStaticMeshRenderData)에 더해: LOD 수 ≤ SkeletalMeshMaxLods, influencesPerVertex
// ∈ {4, 8}, 영향 번호 형식(bone map MeshBoneIndex8Limit개 이하 UByte4, 넘으면 UShort4)·가중치 형식
// (UByte4Unorm·UShort4Unorm)과 두 stream 크기(정점 수 × 영향 / 4 × 원소 byte), bone map·필요 본이
// 엄격 증가하고 < boneCount, morph target 항목 stream(형식 없음, byte = 항목 수 ×
// SkinnedMorphDeltaBytes), 프로필 stream(기본과 같은 형식·크기)이다. 배치 순서는 위치 → tangent
// 기저 → UV → 색 → 영향 번호 → 가중치 → 프로필(번호·가중치) → morph target → index다. data가 빈
// LOD(장치 없는 프로파일)는 정적 규칙(data.rawBytes도 0)에 더해 스킨·프로필·morph stream이 모두
// 비고 influencesPerVertex·morph deltaCount가 0이다(bone map·필요 본·morph·프로필 이름은 남는다).
//
// 사용 예(업로드 전):
//   MeshDiagnostic failure;
//   if (!ValidateSkeletalMeshRenderData(mesh.renderData, mesh.bones.bones.Size(), &failure)) {
//       Report(MeshIssueName(failure.issue), failure.index);
//   }
//
// 오류: 위반은 failure->code CorruptPackage, ErrorCode CorruptData와 issue(Render*), index =
// LOD (bounds·LOD 수는 MeshNoIndex), count = 섹션·UV stream·위치·target·프로필 번호다. 스레드:
// 읽기만 한다.
ROCKS_MESH_API Result<void>
ValidateSkeletalMeshRenderData(const SkeletalMeshRenderData& renderData, usize boneCount,
                               MeshDiagnostic* failure = nullptr) noexcept;

// lod의 bulk 값을 본다: index 값 < 정점 수, 영향 번호 < bone map 크기(기본·프로필), 정점마다 가중치
// 합 = 255(UByte4Unorm) 또는 65535(UShort4Unorm), morph 항목 정점 번호 < 정점 수이고 엄격 증가.
// data는 lod.data의 byte 전체다. 먼저 ValidateSkeletalMeshRenderData를 통과한 lod여야 한다.
//
// data가 빈 LOD는 빈 byte만 받는다(아니면 RenderStreamRange).
//
// 오류: 크기 불일치 RenderStreamRange, 위 규칙 위반 RenderIndexValue·RenderBoneIndexValue·
// RenderSkinWeightSum·RenderMorphVertex(count = 위치, attribute = 프로필·morph target 번호 — 기본
// 영향은 MeshNoAttribute) — 모두 CorruptPackage·CorruptData, index = lodIndex.
ROCKS_MESH_API Result<void>
ValidateSkinnedRenderLodBulk(const SkinnedRenderLod& lod, uint32 lodIndex, Span<const uint8> data,
                             MeshDiagnostic* failure = nullptr) noexcept;

// mesh의 본 계층(ValidateBoneHierarchy — 위반은 issue InvalidBoneHierarchy, index = 본, code
// CorruptPackage), inverse-bind bulk(쿠킹 LOD가 있으면 bulk가 있고 본 수 ×
// SkinnedInverseBindMatrixBytes byte, 없으면(에디터 패키지) 빈 ref — 아니면 RenderInverseBind),
// renderData를 차례로 본다. bulk byte는 읽지 않는다. scratch는 본 이름 정렬 표의 할당기다.
//
// 오류: 위 함수들의 오류, 할당 실패(진단 코드 없음). 스레드: 읽기만 한다.
ROCKS_MESH_API Result<void> ValidateSkeletalMesh(const SkeletalMesh& mesh, Allocator& scratch,
                                                 MeshDiagnostic* failure = nullptr) noexcept;

// 확장 bounds다(UE USkeletalMesh::CalculateExtendedBounds — 상자 최소에서 negativeBoundsExtension을
// 빼고 최대에 positiveBoundsExtension을 더한 상자, 반지름은 늘 그 반 대각선 길이. 확장이 0이어도
// 다시 만든다 — 정적과 다르다).
ROCKS_MESH_API MeshBounds SkeletalMeshExtendedBounds(const SkeletalMesh& mesh) noexcept;

} // namespace rocks
