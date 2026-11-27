#pragma once

#include <Base/Guid.h>
#include <Base/Types.h>

// 파라미터 컬렉션 layout의 항목 하나다(Docs/Design/MaterialSystem 6.10절 MS2c2, 9.7절 — 컬렉션
// 에셋이 계산한 자리, MaterialCollectionLayout이 담는다).
//
// 사용 예:
//   const MaterialCollectionEntry fog{fogId, 0, 1};
//   const MaterialCollectionEntry tint{tintId, 1, MaterialCollectionWholeVector};
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

} // namespace rocks::material
