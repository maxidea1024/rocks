#pragma once

#include <Collision/BodySettings.h>
#include <Collision/CollisionTypes.h>
#include <Core/Containers/Span.h>
#include <Core/Math/Transform.h>
#include <Object/ObjectHandle.h>
#include <World/Physics/BodyShapeElement.h>

namespace rocks {

// 한 body의 부분(루트 + weld 자식) 수 상한이다(잎 형상 user data의 부분 번호 16 bit — 7.1.22절).
inline constexpr usize BodyWeldMaxParts = 65535;

// 루트 body에 합칠(weld) 자식 하나다(UE FBodyInstance::Weld — Docs/Design/PhysicsModule 7.1.22절).
// geometry는 자식 컴포넌트 공간의 scale 전 원소이고 WeldBody 호출 동안만 읽는다.
//
// 사용 예:
//   BodyWeldDesc weld;
//   weld.component = child.Handle();
//   weld.actor = child.GetOwner();
//   weld.geometry = childGeometry.View();
//   weld.relative = childWorld.GetRelativeTransform(rootWorld);   // scale 포함
//   weld.collisionEnabled = CollisionEnabled::QueryAndPhysics;
//   scene.WeldBody(rootBody, weld);
struct BodyWeldDesc {
    ObjectHandle component;
    ObjectHandle actor;
    Span<const BodyShapeElement> geometry;
    // 자식 월드 transform의 루트 월드 기준 상대 transform이다(scale 포함 — UE
    // TheirTM.GetRelativeTransform(MyTM)).
    Transform relative;
    // 자식의 실효 충돌 켬이다(원소 값과 교집합 — UE가 원래 body 값으로 형상 필터를 만든다).
    CollisionEnabled collisionEnabled = CollisionEnabled::QueryAndPhysics;
    // 자식 body 값이다(질량 칸 — UE는 weld body의 질량을 원래 body별로 계산해 합친다). nullptr이면
    // 기본값이다. WeldBody 호출 동안만 읽는다.
    const BodySettings* settings = nullptr;
};

} // namespace rocks
