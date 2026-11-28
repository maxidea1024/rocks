// World 장면 질의(Docs/Design/PhysicsModule 4장, 7.1.7절).
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogContext.h>
#include <Core/Math/MathUtility.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Physics/PhysicsScene.h>
#include <World/Primitives/PrimitiveComponent.generated.h>
#include <World/Primitives/PrimitiveComponent.h>
#include <World/SceneComponent.generated.h>
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

// 질의할 형상 trace다: 끝점·형상 값이 유한하고 회전을 정규화할 수 있다(0·비유한 quaternion은 hit
// 없음 — 장면이 정규화한다). 길이 0은 질의한다(UE SceneQuery.cpp:515). Cylinder는 CY2 전이라 hit
// 없음이다.
bool IsTraceableShape(const Vec3d& start, const Vec3d& end, const Quatd& rotation,
                      const CollisionShape& shape) noexcept {
    return shape.kind != CollisionShapeKind::Cylinder && IsFinite(start) && IsFinite(end) &&
           rotation.TryNormalize().IsOk() && IsFinite(shape.halfExtent) &&
           math::IsFinite(shape.radius) && math::IsFinite(shape.halfHeight);
}

// 컴포넌트 질의에 쓰는 body 값이다.
struct ComponentQuery {
    PhysicsBodyHandle body;
    // weld된 컴포넌트면 그 컴포넌트(루트 body의 부분 — 7.1.23절)다.
    ObjectHandle part;
    QueryFilter filter;
    // 형상 값이 0이다(UE IsZeroExtent — PrimitiveComponent::IsZeroExtent).
    bool zeroExtent = false;
};

// 컴포넌트의 body·채널·응답을 읽는다. body가 없으면 거짓이다.
bool ReadComponentQuery(World& world, ObjectHandle component, ComponentQuery& query) noexcept {
    bool found = false;
    (void)world.With<PrimitiveComponent>(component, [&](PrimitiveComponent& primitive) noexcept {
        // weld면 루트 body의 내 부분이다(UE OverlapMulti의 weld 경로 — 7.1.23절).
        query.body = primitive.PhysicsBody();
        query.part = primitive.IsWelded() ? component : ObjectHandle{};
        auto resolved = primitive.TryResolveCollision();
        if (query.body.IsNull() || !resolved) {
            return Result<void>::Success();
        }

        // UE ComponentSweepMulti: 채널 = object type, 응답 = 컴포넌트 응답 묶음.
        query.filter =
            QueryFilter::ByChannel(resolved.Value().objectType, resolved.Value().responses);
        query.zeroExtent = primitive.IsZeroExtent();
        found = true;
        return Result<void>::Success();
    });
    return found;
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

bool ShapeTraceTest(const World& world, const Vec3d& start, const Vec3d& end, const Quatd& rotation,
                    const CollisionShape& shape, const QueryFilter& filter,
                    const QueryParams& params) noexcept {
    if (shape.IsNearlyZero()) {
        return LineTraceTest(world, start, end, filter, params);
    }

    const PhysicsScene* scene = world.GetPhysicsScene();
    if (scene == nullptr || !IsTraceableShape(start, end, rotation, shape)) {
        return false;
    }

    auto traced = scene->TraceShape(start, end, rotation, shape, ResolveFilter(world, filter),
                                    params, TraceMode::Test, nullptr, nullptr);
    return traced && traced.Value();
}

bool ShapeTraceFirst(const World& world, const Vec3d& start, const Vec3d& end,
                     const Quatd& rotation, const CollisionShape& shape, const QueryFilter& filter,
                     const QueryParams& params, HitResult& hit) noexcept {
    if (shape.IsNearlyZero()) {
        return LineTraceFirst(world, start, end, filter, params, hit);
    }

    hit = EmptyHit(start, end);
    const PhysicsScene* scene = world.GetPhysicsScene();
    if (scene == nullptr || !IsTraceableShape(start, end, rotation, shape)) {
        return false;
    }

    auto traced = scene->TraceShape(start, end, rotation, shape, ResolveFilter(world, filter),
                                    params, TraceMode::First, &hit, nullptr);
    if (!traced || !traced.Value()) {
        hit = EmptyHit(start, end);
        return false;
    }

    return true;
}

Result<bool> ShapeTraceAll(const World& world, const Vec3d& start, const Vec3d& end,
                           const Quatd& rotation, const CollisionShape& shape,
                           const QueryFilter& filter, const QueryParams& params,
                           Array<HitResult>& hits) noexcept {
    if (shape.IsNearlyZero()) {
        return LineTraceAll(world, start, end, filter, params, hits);
    }

    hits.Clear();
    const PhysicsScene* scene = world.GetPhysicsScene();
    if (scene == nullptr || !IsTraceableShape(start, end, rotation, shape)) {
        return Result<bool>::Success(false);
    }

    auto traced = scene->TraceShape(start, end, rotation, shape, ResolveFilter(world, filter),
                                    params, TraceMode::All, nullptr, &hits);
    if (!traced) {
        hits.Clear();
    }

    return traced;
}

namespace {

// 질의할 overlap이다: Line이 아니고 shape trace와 같은 값 조건이다.
bool IsOverlappable(const Vec3d& position, const Quatd& rotation,
                    const CollisionShape& shape) noexcept {
    return shape.kind != CollisionShapeKind::Line &&
           IsTraceableShape(position, position, rotation, shape);
}

bool OverlapTest(const World& world, const Vec3d& position, const Quatd& rotation,
                 const CollisionShape& shape, const QueryFilter& filter, const QueryParams& params,
                 OverlapMode mode) noexcept {
    const PhysicsScene* scene = world.GetPhysicsScene();
    if (scene == nullptr || !IsOverlappable(position, rotation, shape)) {
        return false;
    }

    auto overlapped = scene->OverlapShape(position, rotation, shape, ResolveFilter(world, filter),
                                          params, mode, nullptr);
    return overlapped && overlapped.Value();
}

} // namespace

bool OverlapBlocking(const World& world, const Vec3d& position, const Quatd& rotation,
                     const CollisionShape& shape, const QueryFilter& filter,
                     const QueryParams& params) noexcept {
    return OverlapTest(world, position, rotation, shape, filter, params, OverlapMode::Blocking);
}

bool OverlapAny(const World& world, const Vec3d& position, const Quatd& rotation,
                const CollisionShape& shape, const QueryFilter& filter,
                const QueryParams& params) noexcept {
    return OverlapTest(world, position, rotation, shape, filter, params, OverlapMode::Any);
}

Result<bool> OverlapAll(const World& world, const Vec3d& position, const Quatd& rotation,
                        const CollisionShape& shape, const QueryFilter& filter,
                        const QueryParams& params, Array<OverlapResult>& overlaps) noexcept {
    overlaps.Clear();
    const PhysicsScene* scene = world.GetPhysicsScene();
    if (scene == nullptr || !IsOverlappable(position, rotation, shape)) {
        return Result<bool>::Success(false);
    }

    auto overlapped = scene->OverlapShape(position, rotation, shape, ResolveFilter(world, filter),
                                          params, OverlapMode::All, &overlaps);
    if (!overlapped) {
        overlaps.Clear();
    }

    return overlapped;
}

Result<bool> ComponentTraceAll(World& world, ObjectHandle component, const Vec3d& start,
                               const Vec3d& end, const Quatd& rotation, const QueryParams& params,
                               Array<HitResult>& hits) noexcept {
    hits.Clear();
    const PhysicsScene* scene = world.GetPhysicsScene();
    ComponentQuery query;
    if (scene == nullptr || !ReadComponentQuery(world, component, query)) {
        return Result<bool>::Success(false);
    }

    // 크기 0 컴포넌트는 line trace다(UE — 이때 자기 Actor를 빼지 않는다,
    // WorldCollision.cpp:501-504). UE의 크기 0 형상은 맞을 수 없지만 Rocks body는 아주 작은
    // 형상으로 남으므로 자기 컴포넌트만 뺀다(같은 결과 — 7.1.9절).
    if (query.zeroExtent) {
        QueryParams withSelf = params;
        if (params.ignoreComponents.IsEmpty()) {
            withSelf.ignoreComponents = Span<const ObjectHandle>(&component, 1);
            return LineTraceAll(world, start, end, query.filter, withSelf, hits);
        }

        Array<ObjectHandle> ignored(hits.GetAllocator());
        if (auto reserved = ignored.TryReserve(params.ignoreComponents.Size() + 1); !reserved) {
            return Result<bool>::Failure(reserved.Error());
        }

        for (const ObjectHandle handle : params.ignoreComponents) {
            (void)ignored.TryPushBack(handle);
        }

        (void)ignored.TryPushBack(component);
        withSelf.ignoreComponents = ignored.View();
        return LineTraceAll(world, start, end, query.filter, withSelf, hits);
    }

    if (!IsTraceableShape(start, end, rotation, CollisionShape::Sphere(0.0))) {
        return Result<bool>::Success(false);
    }

    // weld된 컴포넌트는 자기 물리 객체가 없어 쓸 형상이 없다(UE GetPhysicsObjectById가 null —
    // PrimitiveComponentPhysics.cpp:1598-1611, 7.1.23절). 크기 0 line trace는 그 앞이다(UE 순서).
    if (!query.part.IsNull()) {
        return Result<bool>::Success(false);
    }

    auto traced = scene->TraceBody(query.body, start, end, rotation, query.filter, params, hits);
    if (!traced) {
        hits.Clear();
    }

    return traced;
}

Result<bool> ComponentOverlapAll(World& world, ObjectHandle component, const Vec3d& position,
                                 const Quatd& rotation, const QueryParams& params,
                                 Array<OverlapResult>& overlaps) noexcept {
    overlaps.Clear();
    const PhysicsScene* scene = world.GetPhysicsScene();
    ComponentQuery query;
    if (scene == nullptr || !ReadComponentQuery(world, component, query) ||
        !IsTraceableShape(position, position, rotation, CollisionShape::Sphere(0.0))) {
        return Result<bool>::Success(false);
    }

    auto overlapped = scene->OverlapBody(query.body, query.part, position, rotation, query.filter,
                                         params, overlaps);
    if (!overlapped) {
        overlaps.Clear();
        return overlapped;
    }

    // UE ComponentOverlapMulti: 겹침이 하나라도 있으면 참이다(막음만이 아니다).
    return Result<bool>::Success(!overlaps.IsEmpty());
}

} // namespace rocks
