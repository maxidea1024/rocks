#pragma once

#include <Core/Math/Quaternion.h>
#include <Core/Math/Vector3.h>

namespace rocks {

// body의 현재 운동 상태다(Docs/Design/PhysicsModule 2.2절 ReadKinematics). 값은 호출 시점의
// 스냅숏이고 단위는 Rocks(cm·cm/s·rad/s)다.
struct BodyKinematics {
    // body 원점(컴포넌트 위치와 같은 점 — 질량 중심이 아니다)의 월드 위치(cm)와 회전이다.
    Vec3d location = Vec3d::Zero();
    Quatd rotation;
    // 선속도(cm/s)와 각속도(rad/s, 월드 축)다.
    Vec3d linearVelocity = Vec3d::Zero();
    Vec3d angularVelocityRadians = Vec3d::Zero();
    // 깨어 있는지다(Static body는 언제나 거짓).
    bool awake = false;
};

} // namespace rocks
