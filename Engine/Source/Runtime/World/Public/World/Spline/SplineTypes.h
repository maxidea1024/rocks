#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Math/InterpCurve.h>
#include <Core/Math/Interval.h>
#include <Core/Math/Quaternion.h>
#include <Core/Math/Rotator.h>
#include <Core/Math/Transform.h>
#include <Core/Math/Vector3.h>
#include <Meta/Annotations.h>
#include <World/Primitives/PrimitiveTypes.h>
#include <World/Primitives/PrimitiveComponent.h>

namespace rocks {

// 스플라인 점의 보간 종류다(UE ESplinePointType — SplineComponent.h:24). 위치 곡선의 키 모드로
// 저장한다(Docs/Design/EngineRuntime 25.2절).
ROCKS_ENUM()
enum class SplinePointType : uint8 {
    Linear,
    Curve,
    Constant,
    CurveClamped,
    CurveCustomTangent
};

// 스플라인 질의·편집의 좌표계다(UE ESplineCoordinateSpace).
ROCKS_ENUM()
enum class SplineCoordinateSpace : uint8 {
    Local,
    World
};

// 점 하나를 통째로 넣을 때의 값이다(UE FSplinePoint — SplineComponent.h:156). 위치·접선은 Local
// 공간(cm), 회전은 도 단위 Rotator다.
struct SplinePoint {
    float inputKey = 0.0f;
    Vec3d position;
    Vec3d arriveTangent;
    Vec3d leaveTangent;
    Rotator<double> rotation;
    Vec3d scale = Vec3d::One();
    SplinePointType type = SplinePointType::Curve;
};

} // namespace rocks
