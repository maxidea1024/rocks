// World 장면 질의(Docs/Design/PhysicsModule 4장, 7.1.7절).
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogContext.h>
#include <Core/Math/MathUtility.h>
#include <World/Physics/PhysicsScene.h>
#include <World/Physics/WorldCollision.h>
#include <World/World.h>
#include <World/WorldLog.h>

namespace rocks {

namespace {

bool IsFinite(const Vec3d& value) noexcept {
    return math::IsFinite(value.x) && math::IsFinite(value.y) && math::IsFinite(value.z);
}

// 질의할 line이다: 끝점이 유한하고 길이가 0이 아니다(UE SceneQuery.cpp:513 — FMath::IsNearlyZero).
bool IsTraceableLine(const Vec3d& start, const Vec3d& end) noexcept {
    if (!IsFinite(start) || !IsFinite(end)) {
        return false;
    }

    return !math::IsNearlyZero((end - start).Length(), math::SmallNumber<double>);
}

// Profile 필터를 Channel 필터로 푼다(UE UWorld::GetCollisionProfileChannelAndResponseParams —
// World.cpp:2052-2063). 빈 ObjectTypes는 UE처럼 채널 0(WorldStatic)·모두 Block 질의다(UE
// FCollisionObjectQueryParams::IsValid 거짓 → DefaultCollisionChannel —
// PhysicsInterfaceUtils.cpp:17, WorldCollision.cpp:55). 다른 종류는 그대로다.
QueryFilter ResolveFilter(const World& world, const QueryFilter& filter) noexcept {
    if (filter.kind == QueryFilter::Kind::ObjectTypes && filter.objectTypes == 0) {
        return QueryFilter::ByChannel(CollisionChannel::WorldStatic);
    }

    if (filter.kind != QueryFilter::Kind::Profile) {
        return filter;
    }

    if (const CollisionProfile* profile = world.CollisionProfiles().FindProfile(filter.profile)) {
        return QueryFilter::ByChannel(profile->objectType, profile->responses);
    }

    LogContextScope context("World", world.Name());
    Log::Warning(LogWorld,
                 "Collision profile {Profile} for a scene query is not found; querying WorldStatic "
                 "with default responses ({Code})",
                 filter.profile, DiagQueryProfileMissing);
    return QueryFilter::ByChannel(CollisionChannel::WorldStatic);
}

// 질의 결과가 없을 때의 First hit다(UE ResetOutHits — TraceStart·TraceEnd만).
HitResult EmptyHit(const Vec3d& start, const Vec3d& end) noexcept {
    HitResult hit;
    hit.traceStart = start;
    hit.traceEnd = end;
    return hit;
}

} // namespace

uint64 AllObjectTypes(const CollisionProfileTable& table) noexcept {
    uint64 bits = 0;
    for (uint32 index = 0; index < CollisionChannelCount; ++index) {
        const auto channel = static_cast<CollisionChannel>(index);
        if (table.ChannelKind(channel) != CollisionChannelKind::Trace) {
            bits |= CollisionObjectTypeBit(channel);
        }
    }

    return bits;
}

uint64 AllStaticObjectTypes(const CollisionProfileTable& table) noexcept {
    uint64 bits = 0;
    for (uint32 index = 0; index < CollisionChannelCount; ++index) {
        const auto channel = static_cast<CollisionChannel>(index);
        if (table.IsStaticObjectChannel(channel)) {
            bits |= CollisionObjectTypeBit(channel);
        }
    }

    return bits;
}

uint64 AllDynamicObjectTypes(const CollisionProfileTable& table) noexcept {
    return AllObjectTypes(table) & ~AllStaticObjectTypes(table);
}

bool LineTraceTest(const World& world, const Vec3d& start, const Vec3d& end,
                   const QueryFilter& filter, const QueryParams& params) noexcept {
    const PhysicsScene* scene = world.GetPhysicsScene();
    if (scene == nullptr || !IsTraceableLine(start, end)) {
        return false;
    }

    auto traced = scene->TraceLine(start, end, ResolveFilter(world, filter), params,
                                   TraceMode::Test, nullptr, nullptr);
    return traced && traced.Value();
}

bool LineTraceFirst(const World& world, const Vec3d& start, const Vec3d& end,
                    const QueryFilter& filter, const QueryParams& params, HitResult& hit) noexcept {
    hit = EmptyHit(start, end);
    const PhysicsScene* scene = world.GetPhysicsScene();
    if (scene == nullptr || !IsTraceableLine(start, end)) {
        return false;
    }

    auto traced = scene->TraceLine(start, end, ResolveFilter(world, filter), params,
                                   TraceMode::First, &hit, nullptr);
    if (!traced || !traced.Value()) {
        hit = EmptyHit(start, end);
        return false;
    }

    return true;
}

Result<bool> LineTraceAll(const World& world, const Vec3d& start, const Vec3d& end,
                          const QueryFilter& filter, const QueryParams& params,
                          Array<HitResult>& hits) noexcept {
    hits.Clear();
    const PhysicsScene* scene = world.GetPhysicsScene();
    if (scene == nullptr || !IsTraceableLine(start, end)) {
        return Result<bool>::Success(false);
    }

    auto traced = scene->TraceLine(start, end, ResolveFilter(world, filter), params, TraceMode::All,
                                   nullptr, &hits);
    if (!traced) {
        hits.Clear();
    }

    return traced;
}

} // namespace rocks
