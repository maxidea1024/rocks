#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Text/NamePool.h>
#include <Mesh/SkeletalMesh/SkeletalMeshRenderData.h>
#include <MeshTools/MeshCookWarning.h>

// 스킨 메시 너겟 값 머리다(Docs/Design/MeshAnimationAssets 6.7.2절 7 — 쿠킹 처리기가 part 0에
// 쓴다).
namespace rocks {

// 너겟 값 머리(part 0)의 형식 버전이다(배치가 바뀌면 올린다 — 너겟 정체성 버전도 바꾼다).
inline constexpr uint32 SkeletalMeshNuggetHeaderVersion = 1;

// 너겟 머리(part 0)를 out에 쓴다(out을 비우고 채운다 — little-endian 고정 배치). 정적 머리
// (StaticMeshNuggetHeader.h)의 항목에 LOD마다 영향 칸 수·영향 stream 둘(색 다음), bone map·필요
// 본(수와 uint16), 프로필(수와 이름 원문 UTF-8(byte 수 uint32 + byte)·stream 둘), morph target(수와
// 이름 원문·항목 수·stream)을 더한다. renderData의 data·inverseBindMatrices·screenSize·
// lodHysteresis는 쓰지 않는다(너겟 밖 값과 다른 part). names는 프로필·morph 이름의 원문을 얻는
// pool이다.
//
// 사용 예(쿠킹 처리기 — Put 값의 part 0):
//   Array<uint8> head(allocator);
//   if (auto written = WriteSkeletalMeshNuggetHeader(shape, warnings, names, head); !written) {...}
//
// 오류: LOD 수가 SkeletalMeshMaxLods를 넘거나 목록 수가 머리 상한을 넘음 InvalidArgument, 이름
// pool의 오류, 할당 실패. 실패하면 out은 비어 있다.
ROCKS_MESHTOOLS_API Result<void>
WriteSkeletalMeshNuggetHeader(const SkeletalMeshRenderData& renderData,
                              Span<const MeshCookWarning> warnings, NamePool& names,
                              Array<uint8>& out) noexcept;

// 너겟 머리를 out(LOD의 data 크기만 rawBytes에 채운다 — id·SHA·inverse-bind는 비움)과 warnings로
// 읽는다. 이름은 names에 intern한다. 머리 값만 읽으므로 stream·bone map 값의 뜻은 호출자가
// ValidateSkeletalMeshRenderData로 본다.
//
// 사용 예(쿠킹 처리기 — 적중 값의 part 0):
//   SkeletalMeshRenderData shape(allocator);
//   Array<MeshCookWarning> warnings(allocator);
//   if (!ReadSkeletalMeshNuggetHeader(got.values[0].bytes.View(), names, shape, warnings)) {
//       /* 미적중으로 다룬다 */
//   }
//
// 오류: 크기·버전·수 상한·열거 값·이름이 맞지 않음 CorruptData(적중을 미적중으로 다룬다), 할당
// 실패 OutOfMemory. 실패하면 out·warnings는 앞부분만 채워져 있을 수 있다(버린다). 실패해도 그 전에
// intern한 이름은 pool에 남는다.
ROCKS_MESHTOOLS_API Result<void>
ReadSkeletalMeshNuggetHeader(Span<const uint8> bytes, NamePool& names, SkeletalMeshRenderData& out,
                             Array<MeshCookWarning>& warnings) noexcept;

} // namespace rocks
