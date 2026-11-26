#pragma once

#include <Base/Guid.h>
#include <Base/Types.h>
#include <Core/Containers/Span.h>

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

// 항목이 vector(float4 칸 전체)임을 나타내는 성분 번호다.
inline constexpr uint8 MaterialCollectionWholeVector = 0xFF;

// 컬렉션 파라미터 하나의 자리다(UE GetParameterIndex 결과).
struct MaterialCollectionEntry {
    // 컬렉션 파라미터의 ParameterId다(노드가 이 id로 찾는다 — UE와 같다).
    Guid parameterId;
    // float4 칸 번호다.
    uint32 vector = 0;
    // scalar면 칸 안의 성분(0–3), vector면 MaterialCollectionWholeVector다.
    uint8 component = MaterialCollectionWholeVector;
};

struct MaterialCollectionLayout {
    // 컬렉션 에셋의 AssetId Guid다.
    Guid asset;
    Span<const MaterialCollectionEntry> entries;
    // layout의 float4 수다(UE GetTotalVectorStorage).
    uint32 vectors = 0;
};

} // namespace rocks::material
