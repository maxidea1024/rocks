#pragma once

#include <Base/Guid.h>
#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <MaterialCompiler/MaterialCollectionEntry.h>

// 번역 입력의 파라미터 컬렉션 layout이다(Docs/Design/MaterialSystem 6.10절 MS2c2, 9.7절). 컬렉션
// 에셋이 base 상속을 펼치고 배치(scalar 4개씩 묶은 float4 다음 vector float4 — UE
// ParameterCollection.cpp:681–732)를 계산해 넘긴다. 번역기는 배치를 다시 계산하지 않는다.
//
// 사용 예:
//   const MaterialCollectionEntry entries[] = {
//       {fogId, 0, 0},
//       {tintId, 1, MaterialCollectionWholeVector},
//   };
//   const MaterialCollectionLayout layouts[] = {{collectionId, entries, 2}};
//   input.collections = layouts;
namespace rocks::material {

struct MaterialCollectionLayout {
    // 컬렉션 에셋의 AssetId Guid다.
    Guid asset;
    Span<const MaterialCollectionEntry> entries;
    // layout의 float4 수다(UE GetTotalVectorStorage).
    uint32 vectors = 0;
};

} // namespace rocks::material
