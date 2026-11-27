#pragma once

#include <Core/Math/Vector3.h>
#include <Object/ObjectHandle.h>
#include <World/Physics/HitResult.h>

namespace rocks {

// 시뮬레이션 접촉 hit 하나다(Docs/Design/PhysicsModule 5.4절·7.1.12절 — UE FCollisionNotifyInfo의
// 한쪽). 물리 장면이 step 뒤 쌓고 PhysicsResults가 PrimitiveComponent::DispatchPhysicsHit로
// 발행한다. myComponent는 notifyRigidBodyCollision을 켠 컴포넌트이고 hit.component·hit.actor는
// 상대다(otherComponent·otherActor와 같다).
struct PhysicsContactHit {
    ObjectHandle myComponent;
    ObjectHandle myActor;
    ObjectHandle otherComponent;
    ObjectHandle otherActor;
    // 이 스텝에 my 쪽 body가 받은 solver 충격량(kg·cm/s, 월드)이다. probe 접촉은 0이다.
    Vec3d normalImpulse;
    // 첫 접촉점(location·impactPoint), 상대에서 my 쪽을 향하는 법선(normal·impactNormal),
    // 침투 깊이(양수 cm), blockingHit 참이다.
    HitResult hit;
};

} // namespace rocks
