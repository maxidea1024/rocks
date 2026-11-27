#pragma once

#include <Core/Math/Vector3.h>

namespace rocks {

// 최소 이동(MTD) 결과다(UE FMTDResult — Docs/Design/PhysicsModule 5.2절·7.1.13절).
// PrimitiveComponent::ComputePenetration이 채운다: 질의 형상을 direction(단위 벡터) 쪽으로
// distance(cm)만큼 옮기면 겹침에서 빠진다. 닿기만 했으면 distance는 0이다.
struct MtdResult {
    Vec3d direction;
    double distance = 0.0;
};

} // namespace rocks
