#pragma once

#include <Core/Math/Vector3.h>
#include <World/Physics/BodyGeometryKind.h>

namespace rocks {

// 컴포넌트가 주는 scale 전 형상 값(cm)이다. scale은 BodyCreateDesc의 transform이 가지며 Physics가
// UE 규칙으로 형상 크기에 굽는다(어댑터 경계 4절).
// - Box: halfExtent(축별 반폭).
// - Sphere: radius.
// - Capsule: radius와 halfHeight(**반구 포함** — UE CapsuleHalfHeight와 같은 뜻, 로컬 +Z 축).
struct BodyGeometry {
    BodyGeometryKind kind = BodyGeometryKind::None;
    Vec3d halfExtent = Vec3d::Zero();
    double radius = 0.0;
    double halfHeight = 0.0;
};

} // namespace rocks
