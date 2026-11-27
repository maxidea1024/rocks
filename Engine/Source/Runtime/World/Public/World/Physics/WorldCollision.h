#pragma once

#include <Collision/CollisionShape.h>
#include <Core/Containers/Array.h>
#include <Core/Math/Quaternion.h>
#include <Core/Math/Vector3.h>
#include <Core/Result.h>
#include <World/Physics/HitResult.h>
#include <World/Physics/OverlapResult.h>
#include <World/Physics/QueryFilter.h>
#include <World/Physics/QueryParams.h>

namespace rocks {

class World;

// World 장면 질의다(Docs/Design/PhysicsModule 4장 — UE UWorld LineTrace*, World.h:2128-2499).
// 위치는 월드 좌표 cm다. World에 물리 장면이 없거나(물리 없는 World), 끝점이 비유한이거나, 길이가
// 0이면 hit 없음이다(UE도 길이 0 ray는 질의하지 않는다 — SceneQuery.cpp:513-515). 질의는 World의
// 게임 thread에서 물리 step 밖에서만 한다.
//
// 사용 예:
//   HitResult hit;
//   if (LineTraceFirst(world, eye, eye + forward * 1000.0,
//                      QueryFilter::ByChannel(CollisionChannel::Visibility), {}, hit)) {
//       // hit.component를 맞혔다(hit.impactPoint·hit.impactNormal)
//   }

// start → end에 막는 hit가 있는지다(UE LineTraceTestBy*). 겹침은 보지 않는다.
ROCKS_WORLD_API bool LineTraceTest(const World& world, const Vec3d& start, const Vec3d& end,
                                   const QueryFilter& filter, const QueryParams& params) noexcept;

// 가장 가까운 막는 hit를 hit에 쓰고 참을 돌려준다(UE LineTraceSingleBy*). 같은 거리면 body
// handle(장면 칸 번호)이 작은 쪽이다. hit가 없으면 거짓이고 hit는 traceStart·traceEnd만 채운
// 기본값이다.
ROCKS_WORLD_API bool LineTraceFirst(const World& world, const Vec3d& start, const Vec3d& end,
                                    const QueryFilter& filter, const QueryParams& params,
                                    HitResult& hit) noexcept;

// hits를 비우고 가장 가까운 막는 hit까지의 겹침들과 그 막는 hit를 거리 순으로 채운다(UE
// LineTraceMultiBy* — 같은 거리면 겹침이 앞, 그다음 body handle 순). 반환 값은 막는 hit가 있는지다.
// ObjectTypes 필터는 자르지 않고 맞힌 것을 모두 막음으로 돌려준다. 오류: hits 할당 실패(hits는 비어
// 있다).
ROCKS_WORLD_API Result<bool> LineTraceAll(const World& world, const Vec3d& start, const Vec3d& end,
                                          const QueryFilter& filter, const QueryParams& params,
                                          Array<HitResult>& hits) noexcept;

// shape trace(sweep)다(UE SweepTest·SweepSingle·SweepMulti — Docs/Design/PhysicsModule 4.1절,
// 7.1.8절). rotation으로 놓은 shape를 start에서 end로 쓸고 간다. shape가 Line이거나 거의 0이면
// 같은 인자의 line trace다(UE WorldCollision.cpp:151-155). 형상 trace는 길이 0도 질의한다(시작
// 겹침만 나온다). 끝점·형상 값이 비유한이거나 회전이 0·비유한 quaternion이면 hit 없음이다(회전은
// 정규화해 쓴다). Cylinder는 CY2(Jolt 실린더) 전에는 hit 없음이다.
//
// 시작 겹침: params.findInitialOverlaps가 참이면 시작점에서 겹친 body를 time 0·startPenetrating
// hit로 돌려준다(location = start, normal = 형상을 밀어낼 방향, penetrationDepth = MTD 깊이).
// First는 막는 시작 겹침을 다른 hit보다 앞에 두고 그중 가장 깊은 것을 고른다. All은 막는 시작
// 겹침을 자르지 않고 모으며(보고는 막음), 시작 겹침이 아닌 가장 가까운 막음에서 자른다.
//
// 사용 예:
//   HitResult hit;
//   if (ShapeTraceFirst(world, from, to, Quatd(), CollisionShape::Sphere(30.0),
//                       QueryFilter::ByChannel(CollisionChannel::Pawn), {}, hit)) {
//       // hit.location이 구 중심이 멈출 자리다
//   }
ROCKS_WORLD_API bool ShapeTraceTest(const World& world, const Vec3d& start, const Vec3d& end,
                                    const Quatd& rotation, const CollisionShape& shape,
                                    const QueryFilter& filter, const QueryParams& params) noexcept;

ROCKS_WORLD_API bool ShapeTraceFirst(const World& world, const Vec3d& start, const Vec3d& end,
                                     const Quatd& rotation, const CollisionShape& shape,
                                     const QueryFilter& filter, const QueryParams& params,
                                     HitResult& hit) noexcept;

// 반환 값은 보고된 막는 hit(막는 시작 겹침 포함)가 있는지다. 오류: hits 할당 실패(hits는 비어
// 있다).
ROCKS_WORLD_API Result<bool> ShapeTraceAll(const World& world, const Vec3d& start, const Vec3d& end,
                                           const Quatd& rotation, const CollisionShape& shape,
                                           const QueryFilter& filter, const QueryParams& params,
                                           Array<HitResult>& hits) noexcept;

// overlap이다(UE OverlapBlockingTest·OverlapAnyTest·OverlapMulti — Docs/Design/PhysicsModule 4.1절,
// 7.1.9절). rotation으로 position에 놓은 shape와 겹치는 body를 찾는다. Line 형상은 겹침이 없고(UE는
// 구·캡슐·상자·convex만 받는다), 값이 비유한이거나 회전이 0 quaternion이거나 Cylinder(CY2 전)이면
// hit 없음이다. 닿기만 한 형상도 겹침이다.
//
// 사용 예:
//   Array<OverlapResult> found(allocator);
//   if (OverlapAll(world, center, Quatd(), CollisionShape::Sphere(500.0),
//                  QueryFilter::ByObjectTypes(CollisionObjectTypeBit(CollisionChannel::Pawn)), {},
//                  found)) {
//       // found의 컴포넌트가 반경 5 m 안의 Pawn이다
//   }

// 막는 겹침이 있는지다(겹침은 보지 않는다).
ROCKS_WORLD_API bool OverlapBlocking(const World& world, const Vec3d& position,
                                     const Quatd& rotation, const CollisionShape& shape,
                                     const QueryFilter& filter, const QueryParams& params) noexcept;

// 겹침이든 막음이든 하나라도 있는지다.
ROCKS_WORLD_API bool OverlapAny(const World& world, const Vec3d& position, const Quatd& rotation,
                                const CollisionShape& shape, const QueryFilter& filter,
                                const QueryParams& params) noexcept;

// overlaps를 비우고 겹친 컴포넌트를 body handle 순으로 채운다(같은 컴포넌트는 하나 — 막음은 OR).
// 반환 값은 막는 겹침이 있는지다(ObjectTypes는 맞힌 것이 모두 막음이다). 오류: overlaps 할당
// 실패(overlaps는 비어 있다).
ROCKS_WORLD_API Result<bool> OverlapAll(const World& world, const Vec3d& position,
                                        const Quatd& rotation, const CollisionShape& shape,
                                        const QueryFilter& filter, const QueryParams& params,
                                        Array<OverlapResult>& overlaps) noexcept;

// 컴포넌트 자기 형상의 sweep이다(UE ComponentSweepMulti — 7.1.9절). component의 물리 body
// 형상(scale이 구워진 그대로)을 rotation으로 start → end에 쓸고 간다. 채널은 컴포넌트 object type,
// 응답은 컴포넌트의 실효 응답 묶음이고, 컴포넌트의 소유 Actor 전체를 무시한다. 형상 값이 0인
// 컴포넌트(UE IsZeroExtent)는 같은 채널·응답의 line trace다(UE처럼 이때는 자기 Actor를 빼지 않고
// 자기 컴포넌트만 뺀다 — UE의 크기 0 형상은 맞을 수 없다). body가 없는 컴포넌트는 hit 없음이다.
// hits·반환 값은 ShapeTraceAll과 같다.
ROCKS_WORLD_API Result<bool> ComponentTraceAll(World& world, ObjectHandle component,
                                               const Vec3d& start, const Vec3d& end,
                                               const Quatd& rotation, const QueryParams& params,
                                               Array<HitResult>& hits) noexcept;

// 컴포넌트 자기 형상의 overlap이다(UE ComponentOverlapMulti). 채널·응답은 ComponentTraceAll과 같고
// 자기 컴포넌트만 뺀다(같은 Actor의 다른 컴포넌트는 맞는다). 반환 값은 겹침이 하나라도 있는지다(UE
// — object query처럼). body가 없는 컴포넌트는 겹침 없음이다. 오류: overlaps 할당 실패.
ROCKS_WORLD_API Result<bool> ComponentOverlapAll(World& world, ObjectHandle component,
                                                 const Vec3d& position, const Quatd& rotation,
                                                 const QueryParams& params,
                                                 Array<OverlapResult>& overlaps) noexcept;

} // namespace rocks
