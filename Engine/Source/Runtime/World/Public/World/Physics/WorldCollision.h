#pragma once

#include <Core/Containers/Array.h>
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

} // namespace rocks
