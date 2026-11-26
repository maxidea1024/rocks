#pragma once

#include <Collision/CollisionShape.h>
#include <Core/Containers/Array.h>
#include <Core/Math/Quaternion.h>
#include <Core/Math/Vector3.h>
#include <Core/Result.h>
#include <World/Physics/HitResult.h>
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

} // namespace rocks
