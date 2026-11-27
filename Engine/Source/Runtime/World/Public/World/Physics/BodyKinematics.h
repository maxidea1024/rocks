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
    // 월드 질량 중심(cm)이다(UE GetCOMPosition — 질량 중심 이동이 있으면 location과 다르다).
    Vec3d centerOfMass = Vec3d::Zero();
    // 질량(kg)과 관성(kg·cm² — body 축 텐서 대각, 축 정렬 형상에서 UE GetLocalInertiaTensor 주값과
    // 같다)이다. UE 규칙으로 계산한 값이고 자유도 잠금과 무관하다. Static body는 0이다(UE는 rigid
    // body만 질량을 낸다).
    double mass = 0.0;
    Vec3d inertia = Vec3d::Zero();
    // 선속도(cm/s)와 각속도(rad/s, 월드 축)다.
    Vec3d linearVelocity = Vec3d::Zero();
    Vec3d angularVelocityRadians = Vec3d::Zero();
    // 깨어 있는지다(Static body는 언제나 거짓).
    bool awake = false;
};

} // namespace rocks
