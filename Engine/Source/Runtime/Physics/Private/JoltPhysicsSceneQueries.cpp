// 장면 질의(Docs/Design/PhysicsModule 4장·6.3절, 7.1.7절): line trace.
#include "JoltConversions.h"
#include "JoltPhysicsScene.h"

#include <Core/Algorithms.h>
#include <Core/Math/MathUtility.h>

// clang-format off
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollector.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>
// clang-format on

namespace rocks::physics {

namespace {

// 질의 hit 한 건의 분류다(4.4절).
struct QueryHitKind {
    // 결과에 남는다.
    bool keep = false;
    // 자르기 기준(가장 가까운 막음)이다. ObjectTypes All은 자르지 않으므로 거짓이다.
    bool stops = false;
    // 결과의 blockingHit이다(ObjectTypes는 언제나 참 — 4.4절 3).
    bool reportBlocking = false;
};

// body 층의 질의 응답이다(4.4절 1–3): query 표지가 없으면 Ignore, Channel은 두 쪽 응답의 최소,
// ObjectTypes는 object type bit가 집합에 있으면 Block.
CollisionResponse QueryResponse(const ResponseSetTable& responseSets, const QueryFilter& filter,
                                JPH::ObjectLayer layer) noexcept {
    if ((ObjectLayerParticipation(layer) & LayerParticipationQuery) == 0) {
        return CollisionResponse::Ignore;
    }

    const auto objectType = static_cast<CollisionChannel>(ObjectLayerChannel(layer));
    if (filter.kind == QueryFilter::Kind::ObjectTypes) {
        return (filter.objectTypes & CollisionObjectTypeBit(objectType)) != 0
                   ? CollisionResponse::Block
                   : CollisionResponse::Ignore;
    }

    const CollisionResponse bodySide =
        responseSets.Get(ObjectLayerResponseSet(layer)).Get(filter.channel);
    const CollisionResponse querySide = filter.responses.Get(objectType);
    return bodySide < querySide ? bodySide : querySide;
}

// 응답에 모드·인자를 적용한다(4.4절 4): Test·First는 겹침을 버리고(UE bIgnoreTouches),
// ignoreTouches·ignoreBlocks는 그 종류를 버린다.
QueryHitKind ClassifyQueryHit(const ResponseSetTable& responseSets, const QueryFilter& filter,
                              const QueryParams& params, TraceMode mode,
                              JPH::ObjectLayer layer) noexcept {
    const CollisionResponse response = QueryResponse(responseSets, filter, layer);
    if (response == CollisionResponse::Ignore) {
        return {};
    }

    const bool blocks = response == CollisionResponse::Block;
    // UE는 ObjectTypes 다중 질의의 막음을 겹침으로 모은 뒤(CollisionQueryFilterCallback.cpp:36-38)
    // bIgnoreTouches를 적용한다(:160-168). 결과는 막음으로 보고한다(4.4절 3).
    const bool asTouch =
        !blocks || (filter.kind == QueryFilter::Kind::ObjectTypes && mode == TraceMode::All);
    if (asTouch ? params.ignoreTouches : params.ignoreBlocks) {
        return {};
    }

    if (!blocks && mode != TraceMode::All) {
        return {};
    }

    QueryHitKind kind;
    kind.keep = true;
    kind.reportBlocking = blocks;
    kind.stops = !asTouch;
    return kind;
}

// object 층 단계 필터다(6.3절 2단계).
class QueryLayerFilter final : public JPH::ObjectLayerFilter {
public:
    QueryLayerFilter(const ResponseSetTable& responseSets, const QueryFilter& filter,
                     const QueryParams& params, TraceMode mode) noexcept
        : responseSets_(responseSets), filter_(filter), params_(params), mode_(mode) {
    }

    bool ShouldCollide(JPH::ObjectLayer layer) const override {
        return ClassifyQueryHit(responseSets_, filter_, params_, mode_, layer).keep;
    }

private:
    const ResponseSetTable& responseSets_;
    const QueryFilter& filter_;
    const QueryParams& params_;
    TraceMode mode_;
};

// 무시 목록에 handle이 있는지다. 목록의 null은 건너뛴다(UE AddIgnoredActor도 null을 넣지 않는다 —
// 소유 Actor가 없는 body가 모두 빠지지 않게).
bool Contains(Span<const ObjectHandle> handles, ObjectHandle handle) noexcept {
    for (const ObjectHandle candidate : handles) {
        if (!candidate.IsNull() && candidate == handle) {
            return true;
        }
    }

    return false;
}

// a가 b보다 앞인지다(4.5절 — fraction, 같으면 겹침이 앞, 그다음 칸 번호, PMD10).
bool HitBefore(float aFraction, bool aStops, uint32 aSlot, float bFraction, bool bStops,
               uint32 bSlot) noexcept {
    if (aFraction != bFraction) {
        return aFraction < bFraction;
    }

    if (aStops != bStops) {
        return !aStops;
    }

    return aSlot < bSlot;
}

} // namespace

// body 단계 필터다(6.3절 3단계): 무시 목록·mobility. QueryOnly body는 층에 운동 묶음이 없으므로
// 칸의 운동 종류로 본다.
class JoltPhysicsScene::QueryBodyFilter final : public JPH::BodyFilter {
public:
    QueryBodyFilter(const JoltPhysicsScene& scene, const QueryParams& params) noexcept
        : scene_(scene), params_(params) {
    }

    bool ShouldCollideLocked(const JPH::Body& body) const override {
        const JPH::uint64 userData = body.GetUserData();
        const BodySlot* slot =
            scene_.FindSlot({UserDataIndex(userData), UserDataGeneration(userData)});
        if (slot == nullptr) {
            return false;
        }

        const bool isStatic = slot->motion == BodyMotion::Static;
        if ((params_.mobility == QueryMobility::Static && !isStatic) ||
            (params_.mobility == QueryMobility::Dynamic && isStatic)) {
            return false;
        }

        return !Contains(params_.ignoreComponents, slot->component) &&
               !Contains(params_.ignoreActors, slot->actor);
    }

private:
    const JoltPhysicsScene& scene_;
    const QueryParams& params_;
};

// ray hit 수집기다(6.3절): hit마다 body 층으로 막음 여부를 다시 계산한다. Test는 첫 막음에서
// 끝내고, First는 가장 가까운 막음(같은 fraction이면 작은 칸)을 고르며, All은 모두 모으되 막음이
// 생기면 그 뒤를 잘라낸다. early-out은 막음 fraction의 다음 float라 같은 fraction의 hit도 본다.
class JoltPhysicsScene::RayHitCollector final : public JPH::CastRayCollector {
public:
    RayHitCollector(const JoltPhysicsScene& scene, const QueryFilter& filter,
                    const QueryParams& params, TraceMode mode) noexcept
        : scene_(scene), filter_(filter), params_(params), mode_(mode) {
    }

    void AddHit(const JPH::RayCastResult& result) override {
        const JPH::BodyInterface& bodies = scene_.system_.GetBodyInterfaceNoLock();
        const QueryHitKind kind = ClassifyQueryHit(scene_.responseSets_, filter_, params_, mode_,
                                                   bodies.GetObjectLayer(result.mBodyID));
        if (!kind.keep) {
            return;
        }

        QueryHitRecord record;
        record.fraction = result.mFraction;
        record.body = result.mBodyID;
        record.subShape = result.mSubShapeID2;
        record.slot = UserDataIndex(bodies.GetUserData(result.mBodyID));
        record.stops = kind.stops;
        record.reportBlocking = kind.reportBlocking;
        switch (mode_) {
        case TraceMode::Test:
            found = true;
            ForceEarlyOut();
            return;

        case TraceMode::First:
            if (!found ||
                HitBefore(record.fraction, true, record.slot, best.fraction, true, best.slot)) {
                best = record;
                found = true;
                Narrow(record.fraction);
            }

            return;

        case TraceMode::All:
            if (!scene_.queryHits_.TryPushBack(record)) {
                failed = true;
                ForceEarlyOut();
                return;
            }

            if (record.stops) {
                found = true;
                Narrow(record.fraction);
            }

            return;
        }
    }

    bool found = false;
    bool failed = false;
    QueryHitRecord best;

private:
    // 같은 fraction의 다른 body도 비교하도록 다음 float까지 줄인다.
    void Narrow(float fraction) noexcept {
        const float limit = math::NextUp(fraction);
        if (limit < GetEarlyOutFraction()) {
            UpdateEarlyOutFraction(limit);
        }
    }

    const JoltPhysicsScene& scene_;
    const QueryFilter& filter_;
    const QueryParams& params_;
    TraceMode mode_;
};

HitResult JoltPhysicsScene::MakeLineHit(const Vec3d& start, const Vec3d& end,
                                        const QueryHitRecord& record) const noexcept {
    const Vec3d delta = end - start;
    const double length = delta.Length();
    HitResult hit;
    hit.time = static_cast<double>(record.fraction);
    hit.distance = hit.time * length;
    hit.location = start + delta * hit.time;
    hit.traceStart = start;
    hit.traceEnd = end;
    hit.blockingHit = record.reportBlocking;
    const BodySlot& slot = slots_[record.slot];
    hit.component = slot.component;
    hit.actor = slot.actor;
    if (record.fraction <= 0.0f) {
        // 시작점이 볼록 형상 안이다(mTreatConvexAsSolid — UE line trace의 시작 겹침: 점 = 시작,
        // 법선 = −trace 방향, 깊이 0 — CollisionConversions.cpp:378-421).
        hit.startPenetrating = true;
        hit.impactPoint = start;
        hit.normal = delta * (-1.0 / length);
        hit.impactNormal = hit.normal;
        return hit;
    }

    const JPH::RRayCast ray(ToJoltPosition(start, origin_), ToJoltDisplacement(delta));
    const JPH::RVec3 point = ray.GetPointOnRay(record.fraction);
    hit.impactPoint = FromJoltPosition(point, origin_);
    JPH::BodyLockRead lock(system_.GetBodyLockInterface(), record.body);
    if (lock.Succeeded()) {
        hit.impactNormal =
            FromJoltNormal(lock.GetBody().GetWorldSpaceSurfaceNormal(record.subShape, point));
    }

    hit.normal = hit.impactNormal;
    return hit;
}

Result<bool> JoltPhysicsScene::TraceLine(const Vec3d& start, const Vec3d& end,
                                         const QueryFilter& filter, const QueryParams& params,
                                         TraceMode mode, HitResult* first,
                                         Array<HitResult>* all) const noexcept {
    AssertNotStepping();
    if (filter.kind == QueryFilter::Kind::Profile ||
        (mode == TraceMode::First && first == nullptr) ||
        (mode == TraceMode::All && all == nullptr)) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    const JPH::RRayCast ray(ToJoltPosition(start, origin_), ToJoltDisplacement(end - start));
    // 볼록 형상 안에서 시작하면 fraction 0 hit다(시작 겹침 — 4.6절).
    JPH::RayCastSettings settings;
    settings.mTreatConvexAsSolid = true;
    QueryLayerFilter layerFilter(responseSets_, filter, params, mode);
    QueryBodyFilter bodyFilter(*this, params);
    RayHitCollector collector(*this, filter, params, mode);
    // 임시 목록은 All만 쓴다(질의는 게임 thread에서 하나씩 — 장면 콜백이 없어 재진입하지 않는다).
    if (mode == TraceMode::All) {
        queryHits_.Clear();
    }

    system_.GetNarrowPhaseQuery().CastRay(ray, settings, collector, {}, layerFilter, bodyFilter);
    if (collector.failed) {
        queryHits_.Clear();
        return Result<bool>::Failure(ErrorCode::OutOfMemory);
    }

    if (mode == TraceMode::Test) {
        return Result<bool>::Success(collector.found);
    }

    if (mode == TraceMode::First) {
        if (collector.found) {
            *first = MakeLineHit(start, end, collector.best);
        }

        return Result<bool>::Success(collector.found);
    }

    // All(4.5절): 가장 가까운 막음 하나만 두고, 그보다 먼 hit와 다른 막음은 버린 뒤 정렬한다.
    // 압축이 원소를 덮어쓰므로 가장 가까운 막음은 위치·fraction 값으로 기억한다.
    usize nearest = IndexNone;
    for (usize index = 0; index < queryHits_.Size(); ++index) {
        const QueryHitRecord& record = queryHits_[index];
        if (record.stops && (nearest == IndexNone || HitBefore(record.fraction, true, record.slot,
                                                               queryHits_[nearest].fraction, true,
                                                               queryHits_[nearest].slot))) {
            nearest = index;
        }
    }

    const bool hasNearest = nearest != IndexNone;
    const float nearestFraction = hasNearest ? queryHits_[nearest].fraction : 0.0f;
    usize kept = 0;
    for (usize index = 0; index < queryHits_.Size(); ++index) {
        const QueryHitRecord record = queryHits_[index];
        const bool keep =
            !hasNearest || (record.stops ? index == nearest : record.fraction <= nearestFraction);
        if (keep) {
            queryHits_[kept++] = record;
        }
    }

    while (queryHits_.Size() > kept) {
        (void)queryHits_.TryPopBack();
    }

    algo::Sort(queryHits_.View(), [](const QueryHitRecord& a, const QueryHitRecord& b) noexcept {
        return HitBefore(a.fraction, a.stops, a.slot, b.fraction, b.stops, b.slot);
    });
    if (auto reserved = all->TryEnsureCapacity(all->Size() + queryHits_.Size()); !reserved) {
        queryHits_.Clear();
        return Result<bool>::Failure(reserved.Error());
    }

    for (const QueryHitRecord& record : queryHits_) {
        (void)all->TryPushBack(MakeLineHit(start, end, record));
    }

    const bool blocking =
        filter.kind == QueryFilter::Kind::ObjectTypes ? !queryHits_.IsEmpty() : hasNearest;
    queryHits_.Clear();
    return Result<bool>::Success(blocking);
}

} // namespace rocks::physics
