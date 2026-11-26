#pragma once

#include <Core/Math/Shapes.h>

// 메시 bounds 값이다(Docs/Design/MeshAnimationAssets 5.5.1절 9·6.7.1절 7 — 정적·스킨 메시 확장
// bounds의 결과).
namespace rocks {

// 상자 + 구 bounds다(구 중심 = 상자 중심, cm — UE FBoxSphereBounds).
struct MeshBounds {
    Box<double> box{};
    double sphereRadius = 0.0;
};

} // namespace rocks
