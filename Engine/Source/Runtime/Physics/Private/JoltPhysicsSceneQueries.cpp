// 장면 질의(Docs/Design/PhysicsModule 4장·6.3절, 7.1.7–7.1.9절): line trace·shape trace·overlap·
// 컴포넌트 형상 질의.
#include "JoltBodyBuild.h"
#include "JoltConversions.h"
#include "JoltPhysicsScene.h"

#include <Base/NumericLimits.h>
#include <Core/Algorithms.h>
#include <Core/Math/MathUtility.h>

// clang-format off
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollector.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/DecoratedShape.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>
#include <Jolt/Physics/Collision/TransformedShape.h>
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

// 질의 종류별 판정 규칙이다(UE FCollisionQueryFilterCallback — 7.1.7·7.1.9절).
struct QueryRule {
    // 겹침(Touch)을 남긴다. 거짓이면 UE bIgnoreTouches처럼 버린다.
    bool keepTouches = false;
    // ObjectTypes 일치를 겹침으로 모은다(UE multitrace 질의 — CollisionQueryFilterCallback.cpp:
    // 36-38). 결과는 막음으로 보고한다(4.4절 3).
    bool objectTypesAsTouch = false;
};

// trace의 규칙이다: Test·First는 겹침을 버리고 ObjectTypes는 막음, All은 겹침을 모으고
// ObjectTypes도 겹침으로 모은다.
QueryRule RuleOf(TraceMode mode) noexcept {
    const bool all = mode == TraceMode::All;
    return {all, all};
}

// overlap의 규칙이다(UE GeomOverlapMultiImp — SceneQuery.cpp:1120-1123): multitrace는 Any만 끄고,
// bIgnoreTouches는 Blocking만 켠다. 그래서 Blocking의 ObjectTypes 일치는 겹침이 되어 버려진다(UE와
// 같다 — 늘 거짓), Any의 ObjectTypes는 막음으로 남는다.
QueryRule RuleOf(OverlapMode mode) noexcept {
    return {mode != OverlapMode::Blocking, mode != OverlapMode::Any};
}

// 응답에 규칙·인자를 적용한다(4.4절 4): ignoreTouches·ignoreBlocks는 그 종류를 버리고(겹침으로 모은
// ObjectTypes 일치는 겹침이다), 겹침을 남기지 않는 규칙은 겹침을 버린다.
QueryHitKind ClassifyQueryHit(const ResponseSetTable& responseSets, const QueryFilter& filter,
                              const QueryParams& params, QueryRule rule,
                              JPH::ObjectLayer layer) noexcept {
    const CollisionResponse response = QueryResponse(responseSets, filter, layer);
    if (response == CollisionResponse::Ignore) {
        return {};
    }

    const bool blocks = response == CollisionResponse::Block;
    const bool asTouch =
        !blocks || (filter.kind == QueryFilter::Kind::ObjectTypes && rule.objectTypesAsTouch);
    if (asTouch ? params.ignoreTouches : params.ignoreBlocks) {
        return {};
    }

    if (asTouch && !rule.keepTouches) {
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
                     const QueryParams& params, QueryRule rule) noexcept
        : responseSets_(responseSets), filter_(filter), params_(params), rule_(rule) {
    }

    bool ShouldCollide(JPH::ObjectLayer layer) const override {
        return ClassifyQueryHit(responseSets_, filter_, params_, rule_, layer).keep;
    }

private:
    const ResponseSetTable& responseSets_;
    const QueryFilter& filter_;
    const QueryParams& params_;
    QueryRule rule_;
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

// a가 b보다 앞인지다(4.5절 — key, 같으면 막음이 아닌 쪽이 앞, 그다음 칸 번호, PMD10). key는
// fraction(정렬) 또는 Jolt early-out fraction(First 고르기)이다.
bool HitBefore(float aKey, bool aBlocking, uint32 aSlot, float bKey, bool bBlocking,
               uint32 bSlot) noexcept {
    if (aKey != bKey) {
        return aKey < bKey;
    }

    if (aBlocking != bBlocking) {
        return !aBlocking;
    }

    return aSlot < bSlot;
}

// Jolt 벡터 축 수다(x·y·z).
constexpr int JoltAxisCount = 3;

// shape 질의 결과의 fraction이다(겹침 결과는 시작 겹침 — 0).
float CastFraction(const JPH::ShapeCastResult& result) noexcept {
    return result.mFraction;
}

float CastFraction(const JPH::CollideShapeResult&) noexcept {
    return 0.0f;
}

} // namespace

// body 단계 필터다(6.3절 3단계): 무시 목록·mobility. QueryOnly body는 층에 운동 묶음이 없으므로
// 칸의 운동 종류로 본다.
class JoltPhysicsScene::QueryBodyFilter final : public JPH::BodyFilter {
public:
    QueryBodyFilter(const JoltPhysicsScene& scene, const QueryParams& params,
                    const QueryExclusion& exclusion = {}) noexcept
        : scene_(scene), params_(params), exclusion_(exclusion) {
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

        // 컴포넌트 질의는 자기 Actor(sweep)·자기 컴포넌트(overlap)를 더 뺀다(7.1.9절).
        if ((!exclusion_.actor.IsNull() && slot->actor == exclusion_.actor) ||
            (!exclusion_.component.IsNull() && slot->component == exclusion_.component)) {
            return false;
        }

        return !Contains(params_.ignoreComponents, slot->component) &&
               !Contains(params_.ignoreActors, slot->actor);
    }

private:
    const JoltPhysicsScene& scene_;
    const QueryParams& params_;
    QueryExclusion exclusion_;
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
        const QueryHitKind kind =
            ClassifyQueryHit(scene_.responseSets_, filter_, params_, RuleOf(mode_),
                             bodies.GetObjectLayer(result.mBodyID));
        if (!kind.keep) {
            return;
        }

        QueryHitRecord record;
        record.fraction = result.mFraction;
        record.order = result.mFraction;
        record.initial = result.mFraction <= 0.0f;
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

// shape hit 수집기다(7.1.8절): CastShapeCollector(sweep)와 CollideShapeCollector(길이 0 — 모두 시작
// 겹침)를 같은 규칙으로 모은다. 시작 겹침은 findInitialOverlaps가 거짓이면 버리고, All에서는 자르기
// 기준이 아니다. First의 순서 key는 Jolt early-out fraction(시작 겹침은 −깊이 — 가장 깊은 것이
// 앞)이다.
template <class CollectorBase>
class JoltPhysicsScene::ShapeHitCollector final : public CollectorBase {
public:
    using ResultType = typename CollectorBase::ResultType;

    ShapeHitCollector(const JoltPhysicsScene& scene, const QueryFilter& filter,
                      const QueryParams& params, TraceMode mode, JPH::RVec3Arg baseOffset) noexcept
        : scene_(scene), filter_(filter), params_(params), mode_(mode), baseOffset_(baseOffset) {
    }

    void AddHit(const ResultType& result) override {
        const JPH::BodyInterface& bodies = scene_.system_.GetBodyInterfaceNoLock();
        const QueryHitKind kind =
            ClassifyQueryHit(scene_.responseSets_, filter_, params_, RuleOf(mode_),
                             bodies.GetObjectLayer(result.mBodyID2));
        if (!kind.keep) {
            return;
        }

        const float fraction = CastFraction(result);
        // fraction 0이어도 Jolt 허용 오차 안의 분리일 수 있다(깊이는 부호가 없다). 부호 있는 깊이
        // (shape 1 점 − shape 2 점)·축이 음수면 시작 겹침이 아니라 time 0의 일반 hit다(UE도 거리가
        // 양수면 일반 hit다 — HadInitialOverlap).
        const JPH::Vec3 separation = result.mContactPointOn1 - result.mContactPointOn2;
        const float axisLength = result.mPenetrationAxis.Length();
        const bool separated =
            axisLength > 0.0f && separation.Dot(result.mPenetrationAxis) / axisLength < 0.0f;
        const bool initial = fraction <= 0.0f && !separated;
        // 시작 겹침을 버린다(UE bDiscardInitialOverlaps — CollisionQueryFilterCallback.cpp:208).
        if (initial && !params_.findInitialOverlaps) {
            return;
        }

        QueryHitRecord record;
        record.fraction = fraction > 0.0f ? fraction : 0.0f;
        record.order = result.GetEarlyOutFraction();
        record.initial = initial;
        record.body = result.mBodyID2;
        record.subShape = result.mSubShapeID2;
        record.slot = UserDataIndex(bodies.GetUserData(result.mBodyID2));
        // 막는 시작 겹침은 겹침으로 모아 sweep을 계속한다(UE — 결과는 막음으로 보고한다).
        record.stops = kind.stops && !initial;
        record.reportBlocking = kind.reportBlocking;
        record.contact = baseOffset_ + result.mContactPointOn2;
        record.axis = result.mPenetrationAxis;
        record.depth = result.mPenetrationDepth;
        switch (mode_) {
        case TraceMode::Test:
            found = true;
            this->ForceEarlyOut();
            return;

        case TraceMode::First:
            if (!found || HitBefore(record.order, true, record.slot, best.order, true, best.slot)) {
                best = record;
                found = true;
                Narrow(record.order);
            }

            return;

        case TraceMode::All:
            if (!scene_.queryHits_.TryPushBack(record)) {
                failed = true;
                this->ForceEarlyOut();
                return;
            }

            if (record.stops) {
                Narrow(record.fraction);
            }

            return;
        }
    }

    bool found = false;
    bool failed = false;
    QueryHitRecord best;

private:
    // 같은 key의 다른 body도 비교하도록 다음 float까지 줄인다.
    void Narrow(float key) noexcept {
        const float limit = math::NextUp(key);
        if (limit < this->GetEarlyOutFraction()) {
            this->UpdateEarlyOutFraction(limit);
        }
    }

    const JoltPhysicsScene& scene_;
    const QueryFilter& filter_;
    const QueryParams& params_;
    TraceMode mode_;
    JPH::RVec3 baseOffset_;
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
    QueryLayerFilter layerFilter(responseSets_, filter, params, RuleOf(mode));
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

    return FlushAllHits(start, end, CollisionShapeKind::Line, *all);
}

HitResult JoltPhysicsScene::MakeShapeHit(const Vec3d& start, const Vec3d& end,
                                         CollisionShapeKind queryKind,
                                         const QueryHitRecord& record) const noexcept {
    const Vec3d delta = end - start;
    HitResult hit;
    hit.traceStart = start;
    hit.traceEnd = end;
    hit.blockingHit = record.reportBlocking;
    const BodySlot& slot = slots_[record.slot];
    hit.component = slot.component;
    hit.actor = slot.actor;
    hit.impactPoint = FromJoltPosition(record.contact, origin_);
    // 형상을 밀어낼 방향(쓸고 간 형상 쪽 법선)이다 — Jolt penetration axis는 맞은 body를 밀어낼
    // 방향이다.
    const float axisLength = record.axis.Length();
    const bool hasAxis = axisLength > 0.0f && math::IsFinite(axisLength);
    if (record.initial) {
        // UE ConvertOverlappedShapeToImpactHit(CollisionConversions.cpp:582-628): time 0, 위치 =
        // 시작, 법선 = MTD 방향, 깊이 = MTD 깊이. MTD가 없으면 +Z·0.
        hit.startPenetrating = true;
        hit.time = 0.0;
        hit.location = start;
        if (hasAxis) {
            hit.impactNormal = FromJoltNormal(-record.axis / axisLength);
            hit.penetrationDepth = FromJoltLength(record.depth);
        } else {
            // MTD가 없을 때(UE :622-628·646-660): 겹침은 −trace 방향(길이 0이면 +Z), 막음은 +Z다
            // (UE ComputeZeroDistanceImpactNormalAndPenetration의 삼각형 검사는 complex 형상 몫).
            const double length = delta.Length();
            hit.impactNormal = !record.reportBlocking && length > math::SmallNumber<double>
                                   ? delta * (-1.0 / length)
                                   : Vec3d(0.0, 0.0, 1.0);
        }

        hit.normal = hit.impactNormal;
        return hit;
    }

    const double length = delta.Length();
    hit.time = static_cast<double>(record.fraction);
    hit.distance = hit.time * length;
    hit.location = start + delta * hit.time;
    hit.normal = hasAxis ? FromJoltNormal(-record.axis / axisLength) : delta * (-1.0 / length);
    hit.impactNormal = hit.normal;
    // UE FindGeomOpposingNormal(CollisionConversions.cpp:115-140): 구·캡슐 질의가 상자를 맞히면
    // 상자 로컬 축 가운데 normal 성분이 있는 면들 중 sweep 방향에 가장 맞서는 면의
    // 법선이다(ChaosCore AABB::FindGeometryOpposingNormal). 그 밖은 normal 그대로다.
    if (!hasAxis ||
        (queryKind != CollisionShapeKind::Sphere && queryKind != CollisionShapeKind::Capsule)) {
        return hit;
    }

    JPH::BodyLockRead lock(system_.GetBodyLockInterface(), record.body);
    if (!lock.Succeeded()) {
        return hit;
    }

    // 질량 중심 이동 body는 형상이 OffsetCenterOfMassShape로 감싸여 있다(7.1.15절) — 안쪽을
    // 본다(상자는 body 원점 중심이고 회전만 쓴다).
    const JPH::Shape* bodyShape = lock.GetBody().GetShape();
    if (bodyShape->GetSubType() == JPH::EShapeSubType::OffsetCenterOfMass) {
        bodyShape = static_cast<const JPH::DecoratedShape*>(bodyShape)->GetInnerShape();
    }

    if (bodyShape->GetSubType() != JPH::EShapeSubType::Box) {
        return hit;
    }

    const JPH::Quat toLocal = lock.GetBody().GetRotation().Conjugated();
    const JPH::Vec3 localNormal = toLocal * (-record.axis / axisLength);
    const JPH::Vec3 localDirection = toLocal * ToJoltDisplacement(delta);
    const float threshold = math::KindaSmallNumber<float>;
    float bestOpposing = NumericLimits<float>::Max();
    JPH::Vec3 best = localNormal;
    for (int axis = 0; axis < JoltAxisCount; ++axis) {
        const float component = localNormal[axis];
        if (component > threshold || component < -threshold) {
            const float sign = component > 0.0f ? 1.0f : -1.0f;
            const float opposing = sign * localDirection[axis];
            if (opposing < bestOpposing) {
                bestOpposing = opposing;
                best = JPH::Vec3::sZero();
                best.SetComponent(axis, sign);
            }
        }
    }

    hit.impactNormal = FromJoltNormal(lock.GetBody().GetRotation() * best);
    return hit;
}

Result<bool> JoltPhysicsScene::FlushAllHits(const Vec3d& start, const Vec3d& end,
                                            CollisionShapeKind queryKind,
                                            Array<HitResult>& all) const noexcept {
    // 가장 가까운 자르기 기준(시작 겹침이 아닌 막음) 하나만 두고, 그보다 먼 hit와 다른 막음은 버린
    // 뒤 정렬한다(4.5절). 압축이 원소를 덮어쓰므로 가장 가까운 막음은 위치·fraction 값으로
    // 기억한다.
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
    bool blocking = false;
    for (usize index = 0; index < queryHits_.Size(); ++index) {
        const QueryHitRecord record = queryHits_[index];
        const bool keep =
            !hasNearest || (record.stops ? index == nearest : record.fraction <= nearestFraction);
        if (keep) {
            blocking = blocking || record.reportBlocking;
            queryHits_[kept++] = record;
        }
    }

    while (queryHits_.Size() > kept) {
        (void)queryHits_.TryPopBack();
    }

    // UE FCompareFHitResultTime: time, 같으면 보고된 막음이 뒤(CollisionConversions.cpp:930-939).
    algo::Sort(queryHits_.View(), [](const QueryHitRecord& a, const QueryHitRecord& b) noexcept {
        return HitBefore(a.fraction, a.reportBlocking, a.slot, b.fraction, b.reportBlocking,
                         b.slot);
    });
    if (auto reserved = all.TryEnsureCapacity(all.Size() + queryHits_.Size()); !reserved) {
        queryHits_.Clear();
        return Result<bool>::Failure(reserved.Error());
    }

    for (const QueryHitRecord& record : queryHits_) {
        (void)all.TryPushBack(queryKind == CollisionShapeKind::Line
                                  ? MakeLineHit(start, end, record)
                                  : MakeShapeHit(start, end, queryKind, record));
    }

    queryHits_.Clear();
    return Result<bool>::Success(blocking);
}

Result<bool> JoltPhysicsScene::TraceShape(const Vec3d& start, const Vec3d& end,
                                          const Quatd& rotation, const CollisionShape& shape,
                                          const QueryFilter& filter, const QueryParams& params,
                                          TraceMode mode, HitResult* first,
                                          Array<HitResult>* all) const noexcept {
    AssertNotStepping();
    // 인자 검사가 형상·회전 검사보다 앞이다(오류 순서 — PhysicsScene 주석).
    if (shape.kind == CollisionShapeKind::Line || filter.kind == QueryFilter::Kind::Profile ||
        (mode == TraceMode::First && first == nullptr) ||
        (mode == TraceMode::All && all == nullptr)) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    // Jolt 회전은 단위 quaternion이어야 한다(아니면 assert — Fatal).
    auto unit = rotation.TryNormalize();
    if (!unit) {
        return Result<bool>::Failure(unit.Error());
    }

    // 질의 형상은 stack에 만든다(질의마다 힙 할당하지 않는다 — Jolt SetEmbedded).
    return WithJoltQueryShape(shape, [&](const JPH::Shape& joltShape) noexcept {
        return TraceJoltShape(joltShape, shape.kind, start, end, unit.Value(), filter, params, mode,
                              first, all, {});
    });
}

Result<bool> JoltPhysicsScene::TraceJoltShape(
    const JPH::Shape& joltShape, CollisionShapeKind queryKind, const Vec3d& start, const Vec3d& end,
    const Quatd& rotation, const QueryFilter& filter, const QueryParams& params, TraceMode mode,
    HitResult* first, Array<HitResult>* all, const QueryExclusion& exclusion) const noexcept {
    if (filter.kind == QueryFilter::Kind::Profile ||
        (mode == TraceMode::First && first == nullptr) ||
        (mode == TraceMode::All && all == nullptr)) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    // 좌표는 시작점을 기준 offset으로 둔다(접촉점은 그 offset 기준 — Jolt CastShape·CollideShape).
    const JPH::RVec3 base = ToJoltPosition(start, origin_);
    const JPH::RMat44 transform = JPH::RMat44::sRotationTranslation(ToJoltRotation(rotation), base);
    const JPH::Vec3 scale = JPH::Vec3::sReplicate(1.0f);
    QueryLayerFilter layerFilter(responseSets_, filter, params, RuleOf(mode));
    QueryBodyFilter bodyFilter(*this, params, exclusion);
    if (mode == TraceMode::All) {
        queryHits_.Clear();
    }

    auto finish = [&](auto& collector) noexcept -> Result<bool> {
        if (collector.failed) {
            queryHits_.Clear();
            return Result<bool>::Failure(ErrorCode::OutOfMemory);
        }

        if (mode == TraceMode::Test) {
            return Result<bool>::Success(collector.found);
        }

        if (mode == TraceMode::First) {
            if (collector.found) {
                *first = MakeShapeHit(start, end, queryKind, collector.best);
            }

            return Result<bool>::Success(collector.found);
        }

        return FlushAllHits(start, end, queryKind, *all);
    };

    // 길이 0은 겹침 판정으로 시작 겹침만 모은다(Jolt cast의 길이 0 처리에 기대지 않는다 — 7.1.8절).
    if (math::IsNearlyZero((end - start).Length(), math::SmallNumber<double>)) {
        JPH::CollideShapeSettings settings;
        settings.mMaxSeparationDistance = 0.0f;
        ShapeHitCollector<JPH::CollideShapeCollector> collector(*this, filter, params, mode, base);
        system_.GetNarrowPhaseQuery().CollideShape(
            &joltShape, scale, transform.PreTranslated(joltShape.GetCenterOfMass()), settings, base,
            collector, {}, layerFilter, bodyFilter);
        return finish(collector);
    }

    // 시작 겹침의 MTD(가장 깊은 점·깊이)를 받는다(UE bComputeMTD). 멀어지는 방향의 시작 겹침도
    // 찾는다(UE는 방향과 상관없이 시작 겹침을 낸다 — Jolt 기본 IgnoreBackFaces는 버린다. 볼록
    // 형상에서 시작 겹침이 아닌 hit에는 영향이 없다. 삼각형(메시)은 PH6에서 정한다).
    JPH::ShapeCastSettings settings;
    settings.mReturnDeepestPoint = true;
    settings.mBackFaceModeConvex = JPH::EBackFaceMode::CollideWithBackFaces;
    const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(
        &joltShape, scale, transform, ToJoltDisplacement(end - start));
    ShapeHitCollector<JPH::CastShapeCollector> collector(*this, filter, params, mode, base);
    system_.GetNarrowPhaseQuery().CastShape(cast, settings, base, collector, {}, layerFilter,
                                            bodyFilter);
    return finish(collector);
}

namespace {

// body 형상 종류의 질의 형상 종류다(impactNormal 규칙 — 7.1.8절).
CollisionShapeKind QueryKindOf(BodyGeometryKind kind) noexcept {
    switch (kind) {
    case BodyGeometryKind::Box:
        return CollisionShapeKind::Box;
    case BodyGeometryKind::Sphere:
        return CollisionShapeKind::Sphere;
    case BodyGeometryKind::Capsule:
        return CollisionShapeKind::Capsule;
    case BodyGeometryKind::None:
        break;
    }

    return CollisionShapeKind::Box;
}

} // namespace

Result<bool> JoltPhysicsScene::TraceBody(PhysicsBodyHandle body, const Vec3d& start,
                                         const Vec3d& end, const Quatd& rotation,
                                         const QueryFilter& filter, const QueryParams& params,
                                         Array<HitResult>& all) const noexcept {
    AssertNotStepping();
    const BodySlot* slot = FindSlot(body);
    if (slot == nullptr) {
        return Result<bool>::Failure(ErrorCode::StaleHandle);
    }

    auto unit = rotation.TryNormalize();
    if (!unit) {
        return Result<bool>::Failure(unit.Error());
    }

    // 형상 참조를 쥐고 잠금을 푼다(질의가 같은 body를 다시 잠근다).
    JPH::RefConst<JPH::Shape> shape;
    {
        JPH::BodyLockRead lock(system_.GetBodyLockInterface(), slot->body);
        if (!lock.Succeeded()) {
            return Result<bool>::Failure(ErrorCode::StaleHandle);
        }

        shape = lock.GetBody().GetShape();
    }

    // UE ComponentSweepMulti: 형상이 가리키는 Actor(자기 Actor)를 모두 뺀다.
    QueryExclusion exclusion;
    exclusion.actor = slot->actor;
    exclusion.component = slot->component;
    return TraceJoltShape(*shape, QueryKindOf(slot->geometry.kind), start, end, unit.Value(),
                          filter, params, TraceMode::All, nullptr, &all, exclusion);
}

// overlap 수집기다(7.1.9절): hit마다 body 층으로 막음 여부를 다시 계산한다. Blocking·Any는 첫
// hit에서 끝내고, All은 칸을 모은다(합치기·정렬은 끝난 뒤).
class JoltPhysicsScene::OverlapCollector final : public JPH::CollideShapeCollector {
public:
    OverlapCollector(const JoltPhysicsScene& scene, const QueryFilter& filter,
                     const QueryParams& params, OverlapMode mode) noexcept
        : scene_(scene), filter_(filter), params_(params), mode_(mode) {
    }

    void AddHit(const JPH::CollideShapeResult& result) override {
        const JPH::BodyInterface& bodies = scene_.system_.GetBodyInterfaceNoLock();
        const QueryHitKind kind =
            ClassifyQueryHit(scene_.responseSets_, filter_, params_, RuleOf(mode_),
                             bodies.GetObjectLayer(result.mBodyID2));
        if (!kind.keep) {
            return;
        }

        if (mode_ != OverlapMode::All) {
            found = true;
            ForceEarlyOut();
            return;
        }

        QueryHitRecord record;
        record.body = result.mBodyID2;
        record.slot = UserDataIndex(bodies.GetUserData(result.mBodyID2));
        record.reportBlocking = kind.reportBlocking;
        if (!scene_.queryHits_.TryPushBack(record)) {
            failed = true;
            ForceEarlyOut();
        }
    }

    bool found = false;
    bool failed = false;

private:
    const JoltPhysicsScene& scene_;
    const QueryFilter& filter_;
    const QueryParams& params_;
    OverlapMode mode_;
};

Result<bool> JoltPhysicsScene::OverlapJoltShape(const JPH::Shape& joltShape, const Vec3d& position,
                                                const Quatd& rotation, const QueryFilter& filter,
                                                const QueryParams& params, OverlapMode mode,
                                                Array<OverlapResult>* all,
                                                const QueryExclusion& exclusion) const noexcept {
    if (filter.kind == QueryFilter::Kind::Profile || (mode == OverlapMode::All && all == nullptr)) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    const JPH::RVec3 base = ToJoltPosition(position, origin_);
    const JPH::RMat44 transform = JPH::RMat44::sRotationTranslation(ToJoltRotation(rotation), base);
    QueryLayerFilter layerFilter(responseSets_, filter, params, RuleOf(mode));
    QueryBodyFilter bodyFilter(*this, params, exclusion);
    // 닿기만 한 형상(깊이 0)도 겹침이다(분리 거리 0).
    JPH::CollideShapeSettings settings;
    settings.mMaxSeparationDistance = 0.0f;
    OverlapCollector collector(*this, filter, params, mode);
    if (mode == OverlapMode::All) {
        queryHits_.Clear();
    }

    system_.GetNarrowPhaseQuery().CollideShape(&joltShape, JPH::Vec3::sReplicate(1.0f),
                                               transform.PreTranslated(joltShape.GetCenterOfMass()),
                                               settings, base, collector, {}, layerFilter,
                                               bodyFilter);
    if (collector.failed) {
        queryHits_.Clear();
        return Result<bool>::Failure(ErrorCode::OutOfMemory);
    }

    if (mode != OverlapMode::All) {
        return Result<bool>::Success(collector.found);
    }

    // 같은 컴포넌트(같은 칸)의 여러 hit는 하나로 합치고(막음은 OR — UE ConvertOverlapResults), body
    // handle 순으로 둔다(PMD10).
    algo::Sort(queryHits_.View(), [](const QueryHitRecord& a, const QueryHitRecord& b) noexcept {
        return a.slot < b.slot;
    });
    usize kept = 0;
    bool blocking = false;
    for (usize index = 0; index < queryHits_.Size(); ++index) {
        const QueryHitRecord record = queryHits_[index];
        blocking = blocking || record.reportBlocking;
        if (kept != 0 && queryHits_[kept - 1].slot == record.slot) {
            queryHits_[kept - 1].reportBlocking =
                queryHits_[kept - 1].reportBlocking || record.reportBlocking;
            continue;
        }

        queryHits_[kept++] = record;
    }

    if (auto reserved = all->TryEnsureCapacity(all->Size() + kept); !reserved) {
        queryHits_.Clear();
        return Result<bool>::Failure(reserved.Error());
    }

    for (usize index = 0; index < kept; ++index) {
        const BodySlot& slot = slots_[queryHits_[index].slot];
        OverlapResult overlap;
        overlap.component = slot.component;
        overlap.actor = slot.actor;
        overlap.blockingHit = queryHits_[index].reportBlocking;
        (void)all->TryPushBack(overlap);
    }

    queryHits_.Clear();
    return Result<bool>::Success(blocking);
}

Result<bool> JoltPhysicsScene::OverlapShape(const Vec3d& position, const Quatd& rotation,
                                            const CollisionShape& shape, const QueryFilter& filter,
                                            const QueryParams& params, OverlapMode mode,
                                            Array<OverlapResult>* all) const noexcept {
    AssertNotStepping();
    if (shape.kind == CollisionShapeKind::Line || filter.kind == QueryFilter::Kind::Profile ||
        (mode == OverlapMode::All && all == nullptr)) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    auto unit = rotation.TryNormalize();
    if (!unit) {
        return Result<bool>::Failure(unit.Error());
    }

    return WithJoltQueryShape(shape, [&](const JPH::Shape& joltShape) noexcept {
        return OverlapJoltShape(joltShape, position, unit.Value(), filter, params, mode, all, {});
    });
}

namespace {

// 가장 깊은 겹침 하나를 고른다(ComputePenetration — 7.1.13절). 같은 깊이면 먼저 온 것이다.
class DeepestCollideCollector final : public JPH::CollideShapeCollector {
public:
    void AddHit(const JPH::CollideShapeResult& result) override {
        if (!found || result.mPenetrationDepth > depth) {
            found = true;
            depth = result.mPenetrationDepth;
            axis = result.mPenetrationAxis;
        }
    }

    bool found = false;
    float depth = 0.0f;
    JPH::Vec3 axis = JPH::Vec3::sZero();
};

} // namespace

bool JoltPhysicsScene::ComputePenetration(PhysicsBodyHandle body, const CollisionShape& shape,
                                          const Vec3d& position, const Quatd& rotation,
                                          MtdResult& out) const noexcept {
    AssertNotStepping();
    const BodySlot* slot = FindSlot(body);
    if (slot == nullptr || shape.kind == CollisionShapeKind::Line || !math::IsFinite(position.x) ||
        !math::IsFinite(position.y) || !math::IsFinite(position.z)) {
        return false;
    }

    auto unit = rotation.TryNormalize();
    if (!unit) {
        return false;
    }

    // body의 형상·자세다(UE FBodyInstance::OverlapTest — 이 body 하나만 본다).
    JPH::TransformedShape target;
    {
        JPH::BodyLockRead lock(system_.GetBodyLockInterface(), slot->body);
        if (!lock.Succeeded()) {
            return false;
        }

        target = lock.GetBody().GetTransformedShape();
    }

    auto found = WithJoltQueryShape(shape, [&](const JPH::Shape& joltShape) noexcept {
        const JPH::RVec3 base = ToJoltPosition(position, origin_);
        const JPH::RMat44 transform =
            JPH::RMat44::sRotationTranslation(ToJoltRotation(unit.Value()), base);
        // 닿기만 한 형상(깊이 0)도 겹침이다(분리 거리 0 — overlap 질의와 같다).
        JPH::CollideShapeSettings settings;
        settings.mMaxSeparationDistance = 0.0f;
        DeepestCollideCollector collector;
        target.CollideShape(&joltShape, JPH::Vec3::sReplicate(1.0f),
                            transform.PreTranslated(joltShape.GetCenterOfMass()), settings, base,
                            collector);
        if (!collector.found) {
            return Result<bool>::Success(false);
        }

        // Jolt 축은 body(shape 2)를 밀어낼 방향이라 질의 형상은 반대로 간다. 축이 0·비유한이면
        // (중심이 겹침) sweep 시작 겹침 hit와 같이 +Z다(MakeShapeHit — UE MTD 없음의 막음 법선).
        const float axisLength = collector.axis.Length();
        const bool hasAxis = axisLength > 0.0f && math::IsFinite(axisLength);
        out.direction = hasAxis ? FromJoltNormal(-collector.axis / axisLength) : Vec3d::Up();
        out.distance = math::Max(FromJoltLength(collector.depth), 0.0);
        return Result<bool>::Success(true);
    });
    return found && found.Value();
}

Result<bool> JoltPhysicsScene::OverlapBody(PhysicsBodyHandle body, const Vec3d& position,
                                           const Quatd& rotation, const QueryFilter& filter,
                                           const QueryParams& params,
                                           Array<OverlapResult>& all) const noexcept {
    AssertNotStepping();
    const BodySlot* slot = FindSlot(body);
    if (slot == nullptr) {
        return Result<bool>::Failure(ErrorCode::StaleHandle);
    }

    auto unit = rotation.TryNormalize();
    if (!unit) {
        return Result<bool>::Failure(unit.Error());
    }

    JPH::RefConst<JPH::Shape> shape;
    {
        JPH::BodyLockRead lock(system_.GetBodyLockInterface(), slot->body);
        if (!lock.Succeeded()) {
            return Result<bool>::Failure(ErrorCode::StaleHandle);
        }

        shape = lock.GetBody().GetShape();
    }

    // UE ComponentOverlapMulti: 자기 컴포넌트만 뺀다.
    QueryExclusion exclusion;
    exclusion.component = slot->component;
    return OverlapJoltShape(*shape, position, unit.Value(), filter, params, OverlapMode::All, &all,
                            exclusion);
}

} // namespace rocks::physics
