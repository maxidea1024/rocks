#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// Physics Field 범주 노드 속성의 열거다(Docs/Design/MaterialSystem 19.13.2절). 값 번호와 뜻은
// UE 5.8 열거와 같고 이름은 UE 접두를 뗀 나머지다. UE의 숨은 값과 _MAX 표지는 두지 않는다. 저장
// 형식이라 바꾸지 않는다.
namespace rocks::material {

// PhysicsFieldIntegerType 값이다(UE EFieldIntegerType —
// Source/Runtime/Experimental/Chaos/Public/Field/FieldSystemTypes.h).
ROCKS_ENUM()
enum class PhysicsFieldIntegerType : uint8 {
    DynamicState = 0,
    ActivateDisabled = 1,
    CollisionGroup = 2
};

// PhysicsFieldScalarType 값이다(UE EFieldScalarType —
// Source/Runtime/Experimental/Chaos/Public/Field/FieldSystemTypes.h).
ROCKS_ENUM()
enum class PhysicsFieldScalarType : uint8 {
    ExternalClusterStrain = 0,
    Kill = 1,
    DisableThreshold = 2,
    SleepingThreshold = 3,
    InternalClusterStrain = 4,
    DynamicConstraint = 5
};

// PhysicsFieldVectorType 값이다(UE EFieldVectorType —
// Source/Runtime/Experimental/Chaos/Public/Field/FieldSystemTypes.h).
ROCKS_ENUM()
enum class PhysicsFieldVectorType : uint8 {
    LinearForce = 0,
    LinearVelocity = 1,
    AngularVelocity = 2,
    AngularTorque = 3
};

} // namespace rocks::material
