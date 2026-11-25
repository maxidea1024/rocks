#pragma once

#include <Collision/CollisionProfileTable.h>
#include <Core/Math/Vector3.h>
#include <Core/Result.h>
#include <World/Physics/BodyGeometry.h>
#include <World/Physics/BodyMotion.h>

// Jolt.h가 Jolt의 모든 헤더보다 먼저 와야 한다(Core.h의 설정).
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/Reference.h>
#include <Jolt/Physics/Body/MotionType.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
// clang-format on

namespace rocks::physics {

// UE 캡슐 반높이·반경 하한과 원통 길이 하한(cm — BodySetup.cpp:2167-2182의 0.1)이다.
inline constexpr double MinCapsuleDimension = 0.1;
inline constexpr double MinCapsuleCylinderLength = 0.1;

// 형상 값(scale 전 cm)과 월드 scale로 Jolt 형상을 만든다(어댑터 경계 4절 UE scale 규칙,
// PhysicsModule 7.1.3절): box 반폭 × |scale| 축별, 구 반경 × min |scale|(둘 다 최소
// KindaSmallNumber), 캡슐 반높이(반구 포함) × |Z|·반경 × max(|X|, |Y|)(반경은 [0.1, 반높이],
// 원통 길이 최소 0.1). 0인 scale 축은 KindaSmallNumber로 바꾼다 — 크기 0은 거부하지 않는다(UE처럼
// body를 만든다). 오류: 형상 없음·비유한 값(InvalidArgument).
Result<JPH::RefConst<JPH::Shape>> MakeJoltShape(const BodyGeometry& geometry,
                                                const Vec3d& scale) noexcept;

// 실효 충돌과 운동 종류로 object 층을 만든다(6.1절 bit 배치, 7.1.3절): 채널 = object type, 운동
// 묶음 = Static·Moving(실효 충돌 켬이 시뮬레이션을 안 하면 QueryOnly), 참여 표지는 실효 충돌
// 켬에서, 응답 묶음 색인 0(표는 PH3).
JPH::ObjectLayer ObjectLayerFor(const ResolvedCollision& collision, BodyMotion motion) noexcept;

// 운동 종류를 Jolt로 바꾼다.
JPH::EMotionType ToJoltMotion(BodyMotion motion) noexcept;

} // namespace rocks::physics
