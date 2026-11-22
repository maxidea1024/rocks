#pragma once

#include <Core/Math/Color.h>
#include <Core/Math/IntVector.h>
#include <Core/Math/InterpCurve.h>
#include <Core/Math/Quaternion.h>
#include <Core/Math/Rotator.h>
#include <Core/Math/Transform.h>
#include <Core/Math/Vector2.h>
#include <Core/Math/Vector3.h>
#include <Core/Math/Vector4.h>
#include <Core/Time/FrameRate.h>

// Binds 미러(CoreTypeBindings)가 리플렉션하는 Core 값 타입의
// 목록이다(Docs/Design/BuildTargets.ko.md 3.8절 BS8d4, UE TBaseStructure<FVector> 목록 —
// CoreUObject Class.h). 이 타입들은 Core에 있어 리플렉션 표지를 둘 수 없으므로, NativeTypeKey<T>가
// 이 목록으로 "CoreTypeBindings 생성 헤더의 키를 써야 하는 타입"을 알아본다 — 생성 헤더 없이 키를
// 쓰면 static_assert다(Modular에서 공유 라이브러리마다 다른 키가 생기는 것을 컴파일 때 막는다).
// CoreTypeBindings 생성 소스가 미러마다 이 목록에 있는지 static_assert로 확인하므로 새 Binds 미러를
// 더하면 여기에도 더한다.
//
// 사용 예(새 미러 rocks::Vec2i를 더할 때):
//   ROCKS_BOUND_CORE_VALUE(rocks::Vec2i)
namespace rocks {

// T가 Binds 미러의 실제 타입이면 true다.
template <class T>
inline constexpr bool BoundCoreValueV = false;

} // namespace rocks

// 목록에 타입 하나를 더한다(전역 namespace, 이 헤더 안에서만 — 목록 뒤에 #undef한다).
#define ROCKS_BOUND_CORE_VALUE(...)                                                                \
    template <>                                                                                    \
    inline constexpr bool rocks::BoundCoreValueV<__VA_ARGS__> = true;

ROCKS_BOUND_CORE_VALUE(rocks::Vec2)
ROCKS_BOUND_CORE_VALUE(rocks::Vec2d)
ROCKS_BOUND_CORE_VALUE(rocks::Vec3)
ROCKS_BOUND_CORE_VALUE(rocks::Vec3d)
ROCKS_BOUND_CORE_VALUE(rocks::Vec4)
ROCKS_BOUND_CORE_VALUE(rocks::Vec4d)
ROCKS_BOUND_CORE_VALUE(rocks::Quat)
ROCKS_BOUND_CORE_VALUE(rocks::Quatd)
ROCKS_BOUND_CORE_VALUE(rocks::Rotator<float>)
ROCKS_BOUND_CORE_VALUE(rocks::Rotator<double>)
ROCKS_BOUND_CORE_VALUE(rocks::Color)
ROCKS_BOUND_CORE_VALUE(rocks::LinearColor)
ROCKS_BOUND_CORE_VALUE(rocks::IntVec2)
ROCKS_BOUND_CORE_VALUE(rocks::IntVec3)
ROCKS_BOUND_CORE_VALUE(rocks::Transform)
ROCKS_BOUND_CORE_VALUE(rocks::Transform3f)
ROCKS_BOUND_CORE_VALUE(rocks::Transform3d)
ROCKS_BOUND_CORE_VALUE(rocks::FrameRate)
ROCKS_BOUND_CORE_VALUE(rocks::InterpCurvePoint<float>)
ROCKS_BOUND_CORE_VALUE(rocks::InterpCurve<float>)
ROCKS_BOUND_CORE_VALUE(rocks::InterpCurvePoint<rocks::Vec2d>)
ROCKS_BOUND_CORE_VALUE(rocks::InterpCurve<rocks::Vec2d>)
ROCKS_BOUND_CORE_VALUE(rocks::InterpCurvePoint<rocks::Vec3d>)
ROCKS_BOUND_CORE_VALUE(rocks::InterpCurve<rocks::Vec3d>)
ROCKS_BOUND_CORE_VALUE(rocks::InterpCurvePoint<rocks::Quatd>)
ROCKS_BOUND_CORE_VALUE(rocks::InterpCurve<rocks::Quatd>)
ROCKS_BOUND_CORE_VALUE(rocks::InterpCurvePoint<rocks::LinearColor>)
ROCKS_BOUND_CORE_VALUE(rocks::InterpCurve<rocks::LinearColor>)

#undef ROCKS_BOUND_CORE_VALUE
