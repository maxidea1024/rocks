#pragma once

#include <Collision/BodySettings.h>
#include <Collision/CollisionProfileTable.h>
#include <Core/Math/Transform.h>
#include <Object/ObjectHandle.h>
#include <World/Physics/BodyGeometry.h>
#include <World/Physics/BodyMotion.h>

namespace rocks {

// body 하나를 만드는 값이다(Docs/Design/PhysicsModule 2.3절, 7.1.3절). PrimitiveComponent가
// CreateSystemState(Physics)에서 채운다. 포인터(settings)는 CreateBody 호출 동안만 읽는다.
struct BodyCreateDesc {
    // body를 가진 컴포넌트다(결과 반영이 이 컴포넌트에 쓴다 — 3.4절).
    ObjectHandle component;
    // 그 컴포넌트의 소유 Actor다(질의 hit의 actor·무시 목록 — 7.1.7절).
    ObjectHandle actor;
    // scale 전 형상 원소(cm — 7.1.21절)다. CreateBody 호출 동안만 읽는다. 비어 있으면 body를 만들지
    // 않는다(InvalidArgument).
    Span<const BodyShapeElement> geometry;
    // 월드 transform(cm·scale 포함)이다. scale은 형상 크기에 굽는다.
    Transform worldTransform;
    BodyMotion motion = BodyMotion::Static;
    // 실효 충돌 값(실효 충돌 켬·object type·응답)이다.
    ResolvedCollision collision;
    // body 값(중력·감쇠 등 — PH2b는 일부만 쓴다)이다. nullptr이면 기본값이다.
    const BodySettings* settings = nullptr;
    // 깨어서 시작할지다(Dynamic만 의미가 있다 — 2.3절 시작 잠듦).
    bool startAwake = true;
};

} // namespace rocks
