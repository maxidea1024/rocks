#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Text/Name.h>
#include <Mesh/Common/MeshSectionInfo.h>
#include <Mesh/SkeletalMesh/SkinnedRenderLod.h>
#include <MeshTools/SkinnedMeshBuilder.h>
#include <MeshTools/StaticMeshPacking.h>

// 스킨 메시 쿠킹 LOD 패킹이다(Docs/Design/MeshAnimationAssets 6.7.2절 1·7).
namespace rocks {

// built(BuildSkinnedMeshLod 결과)를 6.7.1절 6 배치로 bytes에 쓰고 lod의 stream·섹션·bone map·필요
// 본·프로필·morph를 채운다. 순서는 위치 → tangent 기저 → UV 쌍 → 색(정적과 같다) → 영향 번호 →
// 영향 가중치 → 프로필(번호·가중치 쌍, 빌드 순서) → morph target(빌드 순서) → index이고 각 시작은
// 16 byte 정렬이다.
//
// 인자: settings는 그 LOD의 유효 빌드 설정(tangent·UV 정밀도), sections는 섹션마다의 유효 섹션
// 정보(정적과 같다), lodMaterialMap은 섹션 번호 → 슬롯 덮어쓰기(값이 [0, slotCount)일 때만 섹션
// materialIndex에 굽는다 — SkeletalMeshNoMaterialRemap·범위 밖 값은 그대로, UE
// IsValidMaterialIndex), slotCount는 메시 머티리얼 슬롯 수, profileNames·morphNames는
// built.profiles·built.morphTargets와 같은 길이의 이름(원본 속성 이름에서 접두를 뗀 것 — 처리기가
// 만든다)이다.
//
// 사용 예(쿠킹 처리기):
//   SkinnedRenderLod lod(allocator);
//   Array<uint8> bytes(allocator);
//   if (auto packed = PackSkinnedMeshLod(built, settings, sections, map, slots, color, profiles,
//                                        morphs, bytes, lod); !packed) { ... }
//
// 오류: profileNames·morphNames 길이가 빌드 결과와 다름, influencesPerVertex가 4·8이 아님,
// weightTotal이 255·65535가 아님, 영향 배열(기본·프로필) 크기가 정점 수 × 칸 수가 아님, 영향 번호가
// bone map 크기 이상이거나 가중치가 weightTotal을 넘음 — 모두 InvalidArgument, 크기 넘침
// CapacityExceeded, 할당 실패. 실패하면 bytes·lod는 부분적으로 바뀔 수
// 있다.
ROCKS_MESHTOOLS_API Result<void>
PackSkinnedMeshLod(const SkinnedMeshBuildResult& built, const MeshBuildSettings& settings,
                   Span<const MeshSectionInfo> sections, Span<const int32> lodMaterialMap,
                   uint32 slotCount, MeshColorFormat colorFormat, Span<const Name> profileNames,
                   Span<const Name> morphNames, Array<uint8>& bytes,
                   SkinnedRenderLod& lod) noexcept;

} // namespace rocks
