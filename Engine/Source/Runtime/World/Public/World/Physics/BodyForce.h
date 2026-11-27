#pragma once

#include <Base/Types.h>
#include <Core/Math/Vector3.h>
#include <World/Physics/RadialFalloff.h>

namespace rocks {

// body에 주는 힘·충격량의 종류다(Docs/Design/PhysicsModule 7.1.17절 — UE FBodyInstance의 Add…).
// - Impulse: 질량 중심 선형 충격량(kg·cm/s, 질량 무관이면 속도 변화 cm/s — UE AddImpulse).
// - AngularImpulse: 각충격량(kg·cm²/s, 질량 무관이면 각속도 변화 rad/s — UE
//   AddAngularImpulseInRadians).
// - ImpulseAtLocation: location(월드 cm)의 선형 충격량(질량 무관이면 속도 변화 — UE
//   AddImpulseAtPosition·AddVelocityChangeImpulseAtLocation).
// - RadialImpulse: location을 중심으로 radius 안 body를 밀어내는 충격량(UE AddRadialImpulseToBody).
// - Force: 질량 중심 힘(kg·cm/s², 질량 무관이면 가속도 cm/s² — UE AddForce).
// - ForceAtLocation: location(월드 cm)의 힘(UE AddForceAtPosition).
// - ForceAtLocationLocal: body 공간 location·힘(UE AddForceAtPosition bIsLocalForce).
// - Torque: 토크(kg·cm²/s², 질량 무관이면 각가속도 rad/s² — UE AddTorqueInRadians).
// - RadialForce: location을 중심으로 radius 안 body를 미는 힘(UE AddRadialForceToBody).
enum class BodyForceKind : uint8 {
    Impulse,
    AngularImpulse,
    ImpulseAtLocation,
    RadialImpulse,
    Force,
    ForceAtLocation,
    ForceAtLocationLocal,
    Torque,
    RadialForce
};

// PhysicsScene::ApplyBodyForce의 인자다. 쓰지 않는 칸은 종류마다 무시한다(방사는 value를 쓰지
// 않고, 방사가 아니면 radius·strength·falloff를 쓰지 않는다).
//
// 사용 예:
//   BodyForce push;
//   push.kind = BodyForceKind::Impulse;
//   push.value = Vec3d(0.0, 0.0, 200.0);   // kg·cm/s
//   scene.ApplyBodyForce(body, push);
struct BodyForce {
    BodyForceKind kind = BodyForceKind::Impulse;
    // 충격량·힘·토크 값이다(단위는 종류 설명).
    Vec3d value = Vec3d::Zero();
    // 작용 위치(…AtLocation — 월드 cm, …Local은 body 공간 cm)이거나 방사 중심(월드 cm)이다.
    Vec3d location = Vec3d::Zero();
    // 방사 범위(cm)·세기(충격량 kg·cm/s·힘 kg·cm/s², 질량 무관이면 cm/s·cm/s²)·감쇠다.
    double radius = 0.0;
    double strength = 0.0;
    RadialFalloff falloff = RadialFalloff::Constant;
    // 질량·관성과 무관한 값인지다(UE bVelChange·bAccelChange — 값에 질량·월드 관성을 곱해 넣는다).
    bool massIndependent = false;
    // 힘·토크 종류에서 참이면 이번 프레임의 결과 반영(PhysicsResults)까지 그 프레임의 고정 스텝마다
    // 준다(UE bAllowSubstepping — 고정 스텝이 둘이면 둘 다, 0이면 버린다). 거짓이면 다음 고정 스텝
    // 한 번이다(Fixed hook 안의 호출). 충격량 종류는 쓰지 않는다(다음 스텝에 한 번 더한다).
    bool everyFixedStep = false;
};

} // namespace rocks
