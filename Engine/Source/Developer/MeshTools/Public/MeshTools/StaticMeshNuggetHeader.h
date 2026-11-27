#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Mesh/StaticMesh/StaticMeshRenderData.h>
#include <MeshSource/MeshSourceTypes.h>
#include <MeshTools/MeshCookWarning.h>

// 정적 메시 너겟 값 머리다(Docs/Design/MeshAnimationAssets 5.5.2절 10·12 — 쿠킹 처리기가 part 0에
// 쓴다).
namespace rocks {

// 너겟 값 머리(part 0)의 형식 버전이다(배치가 바뀌면 올린다 — 너겟 정체성 버전도 바꾼다).
inline constexpr uint32 StaticMeshNuggetHeaderVersion = 1;

// 사용 예(쿠킹 처리기 — Put 값의 part 0):
//   Array<uint8> head(allocator);
//   if (auto written = WriteStaticMeshNuggetHeader(shape, warnings, head); !written) { ... }
//
// 너겟 머리(part 0)를 out에 쓴다(out을 비우고 채운다 — little-endian 고정 배치: 버전, LOD 수,
// bounds 상자 6 double·반지름, 경고 수와 (code, lod), LOD마다 정점 수·data byte 수·stream
// 구간(위치·tangent 기저·UV 수와 각 UV·색·index 형식·index)·섹션 수와 섹션 다섯 정수·다섯 bool).
// renderData의 data· screenSize는 쓰지 않는다. 오류: LOD 수가 StaticMeshMaxLods를 넘음
// InvalidArgument, 할당 실패.
ROCKS_MESHTOOLS_API Result<void> WriteStaticMeshNuggetHeader(const StaticMeshRenderData& renderData,
                                                             Span<const MeshCookWarning> warnings,
                                                             Array<uint8>& out) noexcept;

// 너겟 머리를 out(LOD의 data 크기만 rawBytes에 채운다 — id·SHA는 비움)과 warnings로 읽는다. 오류:
// 크기·버전·수 상한·열거 값이 맞지 않음 CorruptData(적중을 미적중으로 다룬다), 할당 실패.
ROCKS_MESHTOOLS_API Result<void>
ReadStaticMeshNuggetHeader(Span<const uint8> bytes, StaticMeshRenderData& out,
                           Array<MeshCookWarning>& warnings) noexcept;

} // namespace rocks
