// Physics 모듈 PH3b 시험이다(Docs/Design/PhysicsModule 4장·6.3절, 7.1.7절): line trace의 결과 값·
// PA08 법선, 채널·ObjectTypes·Profile 필터, 무시 목록·mobility, 참여 표지, 시작 겹침, All의 정렬·
// 자르기. World·컴포넌트 공개 API만 쓰므로 Modular 시험에서도 돈다.
#include "PhysicsBodyFixture.h"

#include <Core/Diagnostics/LogSystem.h>
#include <Core/Diagnostics/RuntimeLogSink.h>
#include <Platform/PlatformMath.h>
#include <World/Physics/WorldCollision.h>

using namespace rocks;
namespace physics_query_tests_file {

using namespace physics_test_support;

namespace {

// 질의 결과 위치·법선 허용 오차다(Jolt float m 좌표 — 수 m 거리에서 1e-6 m 정도).
constexpr double QueryTolerance = 1.0e-3; // cm
constexpr double NormalTolerance = 1.0e-5;
// 시험 상자 반폭(cm)과 line 반길이(cm)다.
constexpr double BoxHalf = 10.0;
constexpr double TraceReach = 1000.0;

// x축을 따라가는 line이다(z = 0, y = 0).
const Vec3d TraceStart(-TraceReach, 0.0, 0.0);
const Vec3d TraceEnd(TraceReach, 0.0, 0.0);

// 시험용 profile 층이다: PawnQuery는 Pawn object type(질의 채널 Pawn), StaticBlind는 WorldStatic
// object type을 Ignore하는 응답(질의 쪽 응답이 WorldStatic body를 뺀다).
constexpr const char* QueryProfileLayer = R"({ "Collision": { "profiles": [
    { "name": "PawnQuery", "collisionEnabled": "QueryAndPhysics", "objectTypeName": "Pawn" },
    { "name": "StaticBlind", "collisionEnabled": "QueryAndPhysics", "objectTypeName": "Pawn",
      "customResponses": [ { "channel": "WorldStatic", "response": "Ignore" } ] } ] } })";

// QueryProfileMissing 경고 수를 센다.
class QueryProfileWarnings final : public RuntimeLogSink {
public:
    Result<void> Write(const LogEvent& event) noexcept override {
        if (StringView(event.message)
                .Contains("(QueryProfileMissing)", StringComparison::CaseSensitive)) {
            ++count;
        }

        return Result<void>::Success();
    }

    uint32 count = 0;
};

bool StartWorld(BodyWorldFixture& fixture) noexcept {
    return fixture.registered && fixture.world && fixture.world->Initialize() &&
           fixture.world->BeginPlay();
}

bool NearVector(const Vec3d& value, const Vec3d& expected, double tolerance) noexcept {
    return Near(value.x, expected.x, tolerance) && Near(value.y, expected.y, tolerance) &&
           Near(value.z, expected.z, tolerance);
}

// x에 상자 하나(반폭 BoxHalf)를 둔다. configure는 등록 전에 부른다.
template <class Configure>
ObjectHandle SpawnBoxAt(BodyWorldFixture& fixture, double x, Configure&& configure) noexcept {
    return fixture.SpawnShape<BoxComponent>([&](BoxComponent& box) noexcept {
        (void)box.SetBoxExtent(Vec3d(BoxHalf, BoxHalf, BoxHalf));
        (void)box.SetRelativeLocation(Vec3d(x, 0.0, 0.0));
        configure(box);
    });
}

ObjectHandle SpawnBoxAt(BodyWorldFixture& fixture, double x) noexcept {
    return SpawnBoxAt(fixture, x, [](BoxComponent&) noexcept {});
}

// Visibility 응답을 정한 상자다.
ObjectHandle SpawnVisibilityBox(BodyWorldFixture& fixture, double x,
                                CollisionResponse response) noexcept {
    return SpawnBoxAt(fixture, x, [response](BoxComponent& box) noexcept {
        (void)box.SetCollisionResponseToChannel(CollisionChannel::Visibility, response);
    });
}

// 상자의 충돌을 끈다(body를 지워 장면 칸을 비운다 — 칸 재사용 시험).
void DisableCollision(World& world, ObjectHandle component) noexcept {
    REQUIRE(world.With<PrimitiveComponent>(component, [](PrimitiveComponent& primitive) noexcept {
        return primitive.SetCollisionEnabled(CollisionEnabled::NoCollision);
    }));
}

ObjectHandle Owner(World& world, ObjectHandle component) noexcept {
    ObjectHandle owner;
    (void)world.With<Component>(component, [&](Component& value) noexcept {
        owner = value.GetOwner();
        return Result<void>::Success();
    });
    return owner;
}

// line이 x에 있는 상자의 앞면(x − BoxHalf)에 닿는 time이다.
double EntryTime(double x) noexcept {
    return (x - BoxHalf - TraceStart.x) / (TraceEnd.x - TraceStart.x);
}

const QueryFilter Visibility = QueryFilter::ByChannel(CollisionChannel::Visibility);

} // namespace

// PA08(line 몫)과 결과 값: 위에서 바닥(윗면 z = 0)을 맞힌 hit의 법선은 +Z이고 time·distance·위치는
// line 기하 그대로다(UE ConvertQueryImpactHit).
TEST(PhysicsQuery_LineHitValuesAndNormal) {
    constexpr double Height = 100.0;
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    const ObjectHandle floor = fixture.SpawnFloor();
    REQUIRE(!floor.IsNull());
    const Vec3d start(0.0, 0.0, Height);
    const Vec3d end(0.0, 0.0, -Height);
    CHECK(LineTraceTest(world, start, end, Visibility, {}));

    HitResult hit;
    REQUIRE(LineTraceFirst(world, start, end, Visibility, {}, hit));
    CHECK(hit.blockingHit);
    CHECK(!hit.startPenetrating);
    CHECK(hit.IsValidBlockingHit());
    CHECK(Near(hit.time, 0.5, NormalTolerance));
    CHECK(Near(hit.distance, Height, QueryTolerance));
    CHECK(NearVector(hit.location, Vec3d::Zero(), QueryTolerance));
    CHECK(NearVector(hit.impactPoint, Vec3d::Zero(), QueryTolerance));
    CHECK(NearVector(hit.impactNormal, Vec3d(0.0, 0.0, 1.0), NormalTolerance));
    CHECK(NearVector(hit.normal, Vec3d(0.0, 0.0, 1.0), NormalTolerance));
    CHECK(hit.traceStart == start);
    CHECK(hit.traceEnd == end);
    CHECK(hit.penetrationDepth == 0.0);
    CHECK(hit.faceIndex == NoHitFaceIndex);
    CHECK(hit.component == floor);
    CHECK(hit.actor == Owner(world, floor));

    Array<HitResult> hits(fixture.allocator);
    auto all = LineTraceAll(world, start, end, Visibility, {}, hits);
    REQUIRE(all);
    CHECK(all.Value());
    REQUIRE(hits.Size() == 1u);
    CHECK(hits[0].component == floor);
}

// 물리 장면이 없는 World, 길이 0, 비유한 끝점은 hit 없음이다. First는 hit가 없어도 traceStart·
// traceEnd를 채운다.
TEST(PhysicsQuery_DegenerateQueriesMiss) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    REQUIRE(!SpawnBoxAt(fixture, 0.0).IsNull());
    HitResult hit;
    CHECK(!LineTraceFirst(world, Vec3d::Zero(), Vec3d::Zero(), Visibility, {}, hit));
    const double nan = platform::PlatformMath::QuietNaN<double>();
    CHECK(!LineTraceTest(world, Vec3d(nan, 0.0, 0.0), TraceEnd, Visibility, {}));
    const double infinity = platform::PlatformMath::Infinity<double>();
    CHECK(!LineTraceFirst(world, TraceStart, Vec3d(infinity, 0.0, 0.0), Visibility, {}, hit));
    CHECK(LineTraceFirst(world, TraceStart, TraceEnd, Visibility, {}, hit));

    BodyWorldFixture plain(false);
    REQUIRE(StartWorld(plain));
    HitResult missed;
    CHECK(!LineTraceFirst(*plain.world, TraceStart, TraceEnd, Visibility, {}, missed));
    CHECK(missed.traceStart == TraceStart);
    CHECK(missed.traceEnd == TraceEnd);
    CHECK(!missed.blockingHit);
    CHECK(missed.component.IsNull());
    Array<HitResult> hits(plain.allocator);
    auto all = LineTraceAll(*plain.world, TraceStart, TraceEnd, Visibility, {}, hits);
    REQUIRE(all);
    CHECK(!all.Value());
    CHECK(hits.IsEmpty());
}

// 채널 응답(4.4절 2): First·Test는 막음만, All은 가장 가까운 막음까지의 겹침들과 그 막음이다(그 뒤
// 겹침은 버린다). 겹침만 있으면 Test·First는 거짓이다. Ignore는 맞지 않고, 질의 쪽 응답이
// Ignore여도 맞지 않는다.
TEST(PhysicsQuery_ChannelResponsesClassifyHits) {
    constexpr double IgnoredX = -500.0;
    constexpr double FirstOverlapX = -300.0;
    constexpr double SecondOverlapX = -200.0;
    constexpr double BlockX = 0.0;
    constexpr double FarOverlapX = 200.0;
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    const ObjectHandle ignored = SpawnVisibilityBox(fixture, IgnoredX, CollisionResponse::Ignore);
    const ObjectHandle firstOverlap =
        SpawnVisibilityBox(fixture, FirstOverlapX, CollisionResponse::Overlap);
    const ObjectHandle secondOverlap =
        SpawnVisibilityBox(fixture, SecondOverlapX, CollisionResponse::Overlap);
    const ObjectHandle block = SpawnVisibilityBox(fixture, BlockX, CollisionResponse::Block);
    const ObjectHandle farOverlap =
        SpawnVisibilityBox(fixture, FarOverlapX, CollisionResponse::Overlap);
    REQUIRE(!ignored.IsNull() && !firstOverlap.IsNull() && !secondOverlap.IsNull() &&
            !block.IsNull() && !farOverlap.IsNull());

    HitResult hit;
    REQUIRE(LineTraceFirst(world, TraceStart, TraceEnd, Visibility, {}, hit));
    CHECK(hit.component == block);
    CHECK(Near(hit.time, EntryTime(BlockX), NormalTolerance));
    CHECK(NearVector(hit.impactNormal, Vec3d(-1.0, 0.0, 0.0), NormalTolerance));

    Array<HitResult> hits(fixture.allocator);
    auto all = LineTraceAll(world, TraceStart, TraceEnd, Visibility, {}, hits);
    REQUIRE(all);
    CHECK(all.Value());
    REQUIRE(hits.Size() == 3u);
    CHECK(hits[0].component == firstOverlap);
    CHECK(Near(hits[0].time, EntryTime(FirstOverlapX), NormalTolerance));
    CHECK(hits[1].component == secondOverlap);
    CHECK(!hits[0].blockingHit && !hits[1].blockingHit);
    CHECK(hits[2].component == block);
    CHECK(hits[2].blockingHit);

    QueryParams touchesOff;
    touchesOff.ignoreTouches = true;
    REQUIRE(LineTraceAll(world, TraceStart, TraceEnd, Visibility, touchesOff, hits));
    REQUIRE(hits.Size() == 1u);
    CHECK(hits[0].component == block);

    // 막음을 버리면 자르지 않으므로 겹침이 모두 남고 막는 hit는 없다.
    QueryParams blocksOff;
    blocksOff.ignoreBlocks = true;
    all = LineTraceAll(world, TraceStart, TraceEnd, Visibility, blocksOff, hits);
    REQUIRE(all);
    CHECK(!all.Value());
    REQUIRE(hits.Size() == 3u);
    CHECK(hits[0].component == firstOverlap);
    CHECK(hits[1].component == secondOverlap);
    CHECK(hits[2].component == farOverlap);
    CHECK(!LineTraceFirst(world, TraceStart, TraceEnd, Visibility, blocksOff, hit));

    // 겹침만 있는 line은 Test·First가 거짓이다(UE Single·Test는 겹침을 보지 않는다).
    const ObjectHandle skipBlock[] = {block};
    QueryParams overlapsOnly;
    overlapsOnly.ignoreComponents = skipBlock;
    CHECK(!LineTraceTest(world, TraceStart, TraceEnd, Visibility, overlapsOnly));
    CHECK(!LineTraceFirst(world, TraceStart, TraceEnd, Visibility, overlapsOnly, hit));

    // 질의 쪽 응답: body object type(WorldStatic)을 Ignore하면 아무것도 맞지 않는다.
    CollisionResponses ignoreStatic;
    (void)ignoreStatic.Set(CollisionChannel::WorldStatic, CollisionResponse::Ignore);
    const QueryFilter blind = QueryFilter::ByChannel(CollisionChannel::Visibility, ignoreStatic);
    CHECK(!LineTraceTest(world, TraceStart, TraceEnd, blind, {}));
    REQUIRE(LineTraceAll(world, TraceStart, TraceEnd, blind, {}, hits));
    CHECK(hits.IsEmpty());
}

// ObjectTypes(4.4절 3): 집합의 type만 맞는다. All은 자르지 않고 모두 막음으로 돌려준다.
// AllObjectTypes는 trace 채널(Visibility)을 뺀다.
TEST(PhysicsQuery_ObjectTypesFilter) {
    constexpr double FirstPawnX = -200.0;
    constexpr double DynamicX = 0.0;
    constexpr double SecondPawnX = 200.0;
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    auto typed = [&](double x, CollisionChannel channel) noexcept {
        return SpawnBoxAt(fixture, x, [channel](BoxComponent& box) noexcept {
            (void)box.SetCollisionObjectType(channel);
        });
    };

    const ObjectHandle firstPawn = typed(FirstPawnX, CollisionChannel::Pawn);
    const ObjectHandle dynamic = typed(DynamicX, CollisionChannel::WorldDynamic);
    const ObjectHandle secondPawn = typed(SecondPawnX, CollisionChannel::Pawn);
    REQUIRE(!firstPawn.IsNull() && !dynamic.IsNull() && !secondPawn.IsNull());
    const QueryFilter pawns =
        QueryFilter::ByObjectTypes(CollisionObjectTypeBit(CollisionChannel::Pawn));

    HitResult hit;
    REQUIRE(LineTraceFirst(world, TraceStart, TraceEnd, pawns, {}, hit));
    CHECK(hit.component == firstPawn);
    CHECK(hit.blockingHit);

    Array<HitResult> hits(fixture.allocator);
    auto all = LineTraceAll(world, TraceStart, TraceEnd, pawns, {}, hits);
    REQUIRE(all);
    CHECK(all.Value());
    REQUIRE(hits.Size() == 2u);
    CHECK(hits[0].component == firstPawn);
    CHECK(hits[1].component == secondPawn);
    CHECK(hits[0].blockingHit && hits[1].blockingHit);

    const CollisionProfileTable& table = world.CollisionProfiles();
    const uint64 everything = AllObjectTypes(table);
    CHECK((everything & CollisionObjectTypeBit(CollisionChannel::Visibility)) == 0);
    CHECK((everything & CollisionObjectTypeBit(CollisionChannel::Pawn)) != 0);
    CHECK((AllStaticObjectTypes(table) & CollisionObjectTypeBit(CollisionChannel::WorldStatic)) !=
          0);
    CHECK((AllDynamicObjectTypes(table) & CollisionObjectTypeBit(CollisionChannel::WorldStatic)) ==
          0);
    REQUIRE(LineTraceAll(world, TraceStart, TraceEnd, QueryFilter::ByObjectTypes(everything), {},
                         hits));
    CHECK(hits.Size() == 3u);

    // ObjectTypes All은 겹침으로 모으므로 ignoreTouches가 버리고 ignoreBlocks는 버리지 않는다(UE
    // CollisionQueryFilterCallback.cpp:36-38·160-168).
    QueryParams touchesOff;
    touchesOff.ignoreTouches = true;
    all = LineTraceAll(world, TraceStart, TraceEnd, pawns, touchesOff, hits);
    REQUIRE(all);
    CHECK(!all.Value());
    CHECK(hits.IsEmpty());
    QueryParams blocksOff;
    blocksOff.ignoreBlocks = true;
    REQUIRE(LineTraceAll(world, TraceStart, TraceEnd, pawns, blocksOff, hits));
    CHECK(hits.Size() == 2u);
    // First는 막음으로 본다(ignoreBlocks면 없다).
    CHECK(!LineTraceFirst(world, TraceStart, TraceEnd, pawns, blocksOff, hit));

    // 빈 집합은 UE처럼 WorldStatic 채널·모두 Block 질의다(상자들은 WorldStatic을 막는다).
    REQUIRE(LineTraceFirst(world, TraceStart, TraceEnd, QueryFilter::ByObjectTypes(0), {}, hit));
    CHECK(hit.component == firstPawn);
}

// Profile(4.2절): profile의 object type을 채널로, 응답 묶음을 응답으로 쓴다. 없는 profile은
// 질의마다 Warning(QueryProfileMissing) 뒤 WorldStatic 채널·모두 Block이다.
TEST(PhysicsQuery_ProfileFilter) {
    constexpr double PawnBlindX = 0.0;
    constexpr double StaticBlindX = 200.0;
    QueryProfileWarnings warnings;
    SystemAllocator logAllocator;
    LogSystemConfig logConfig;
    logConfig.mode = LogMode::Manual;
    logConfig.globalMinimum = LogLevel::Debug;
    LogSystem logs(logAllocator, logConfig);
    REQUIRE(logs.Start() && logs.AddSink(warnings));
    LogInstallScope installed(logs);
    BodyWorldFixture fixture(true, WorldKind::Game, nullptr, QueryProfileLayer);
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    REQUIRE(world.CollisionProfiles().FindProfile("PawnQuery") != nullptr);
    // 앞 상자는 Pawn 채널만, 뒤 상자는 WorldStatic 채널만 Ignore한다(둘 다 object type
    // WorldStatic).
    const ObjectHandle pawnBlind = SpawnBoxAt(fixture, PawnBlindX, [](BoxComponent& box) noexcept {
        (void)box.SetCollisionResponseToChannel(CollisionChannel::Pawn, CollisionResponse::Ignore);
    });
    const ObjectHandle staticBlind =
        SpawnBoxAt(fixture, StaticBlindX, [](BoxComponent& box) noexcept {
            (void)box.SetCollisionResponseToChannel(CollisionChannel::WorldStatic,
                                                    CollisionResponse::Ignore);
        });
    REQUIRE(!pawnBlind.IsNull() && !staticBlind.IsNull());

    // 채널: PawnQuery는 Pawn 채널이라 앞 상자를 지나 뒤 상자를 맞힌다.
    HitResult hit;
    REQUIRE(
        LineTraceFirst(world, TraceStart, TraceEnd, QueryFilter::ByProfile("PawnQuery"), {}, hit));
    CHECK(hit.component == staticBlind);
    // 응답: StaticBlind는 WorldStatic object type body를 질의 쪽에서 뺀다.
    CHECK(!LineTraceTest(world, TraceStart, TraceEnd, QueryFilter::ByProfile("StaticBlind"), {}));
    (void)logs.Pump();
    CHECK(warnings.count == 0u);

    // 없는 profile은 WorldStatic 채널이다: 앞 상자(Pawn만 Ignore)를 맞히고, 그것을 빼면 뒤
    // 상자(WorldStatic Ignore)는 맞지 않는다.
    const QueryFilter missing = QueryFilter::ByProfile("NoSuchProfile");
    REQUIRE(LineTraceFirst(world, TraceStart, TraceEnd, missing, {}, hit));
    CHECK(hit.component == pawnBlind);
    const ObjectHandle skipFront[] = {pawnBlind};
    QueryParams params;
    params.ignoreComponents = skipFront;
    CHECK(!LineTraceTest(world, TraceStart, TraceEnd, missing, params));
    (void)logs.Pump();
    CHECK(warnings.count == 2u);
}

// 무시 목록과 mobility(4.4절 1): 컴포넌트·Actor로 빼고, Static·Dynamic body 하나만 질의한다.
TEST(PhysicsQuery_IgnoreListsAndMobility) {
    constexpr double StaticX = -200.0;
    constexpr double MovableX = 200.0;
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    const ObjectHandle pinned = SpawnBoxAt(fixture, StaticX, [](BoxComponent& box) noexcept {
        box.SetMobility(ComponentMobility::Static);
    });
    const ObjectHandle movable = SpawnBoxAt(fixture, MovableX);
    REQUIRE(!pinned.IsNull() && !movable.IsNull());

    HitResult hit;
    REQUIRE(LineTraceFirst(world, TraceStart, TraceEnd, Visibility, {}, hit));
    CHECK(hit.component == pinned);

    const ObjectHandle skipPinned[] = {pinned};
    QueryParams params;
    params.ignoreComponents = skipPinned;
    REQUIRE(LineTraceFirst(world, TraceStart, TraceEnd, Visibility, params, hit));
    CHECK(hit.component == movable);
    const ObjectHandle skipMovableActor[] = {Owner(world, movable)};
    params.ignoreActors = skipMovableActor;
    CHECK(!LineTraceTest(world, TraceStart, TraceEnd, Visibility, params));

    QueryParams dynamicOnly;
    dynamicOnly.mobility = QueryMobility::Dynamic;
    REQUIRE(LineTraceFirst(world, TraceStart, TraceEnd, Visibility, dynamicOnly, hit));
    CHECK(hit.component == movable);
    QueryParams staticOnly;
    staticOnly.mobility = QueryMobility::Static;
    REQUIRE(LineTraceFirst(world, TraceStart, TraceEnd, Visibility, staticOnly, hit));
    CHECK(hit.component == pinned);
    staticOnly.ignoreComponents = skipPinned;
    CHECK(!LineTraceTest(world, TraceStart, TraceEnd, Visibility, staticOnly));
}

// 참여(CollisionData 2.2절): QueryOnly body는 질의에 맞고 PhysicsOnly·NoCollision body는 맞지
// 않는다.
TEST(PhysicsQuery_CollisionEnabledParticipation) {
    const CollisionEnabled modes[] = {CollisionEnabled::QueryOnly, CollisionEnabled::PhysicsOnly,
                                      CollisionEnabled::NoCollision,
                                      CollisionEnabled::QueryAndPhysics};
    for (const CollisionEnabled mode : modes) {
        BodyWorldFixture fixture;
        REQUIRE(StartWorld(fixture));
        const ObjectHandle box = SpawnBoxAt(fixture, 0.0, [mode](BoxComponent& value) noexcept {
            (void)value.SetCollisionEnabled(mode);
        });
        REQUIRE(!box.IsNull());
        const bool queryable =
            mode == CollisionEnabled::QueryOnly || mode == CollisionEnabled::QueryAndPhysics;
        CHECK(LineTraceTest(*fixture.world, TraceStart, TraceEnd, Visibility, {}) == queryable);
    }
}

// 시작점이 상자 안이면 time 0의 시작 겹침이다: 점 = 시작, 법선 = −trace 방향, 깊이 0(UE line
// trace).
TEST(PhysicsQuery_StartInsideIsPenetrating) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    const ObjectHandle box = SpawnBoxAt(fixture, 0.0);
    REQUIRE(!box.IsNull());
    const Vec3d inside(BoxHalf * 0.5, 0.0, 0.0);
    HitResult hit;
    REQUIRE(LineTraceFirst(world, inside, TraceEnd, Visibility, {}, hit));
    CHECK(hit.component == box);
    CHECK(hit.time == 0.0);
    CHECK(hit.distance == 0.0);
    CHECK(hit.startPenetrating);
    CHECK(hit.blockingHit);
    CHECK(!hit.IsValidBlockingHit());
    CHECK(hit.impactPoint == inside);
    CHECK(NearVector(hit.normal, Vec3d(-1.0, 0.0, 0.0), NormalTolerance));
    CHECK(hit.impactNormal == hit.normal);
    CHECK(hit.penetrationDepth == 0.0);
}

// 같은 time의 순서(4.5절, PMD10): 겹침이 막음보다 앞이고, 겹침끼리·막음끼리는 body handle(칸 번호)
// 순이다. 칸을 비웠다가 다시 쓰게 해 만든 순서와 칸 순서를 다르게 한다(빈 칸은 나중에 만든 body가
// 쓴다).
TEST(PhysicsQuery_AllOrdersTiesDeterministically) {
    constexpr double SharedX = 0.0;
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    const ObjectHandle freed = SpawnVisibilityBox(fixture, SharedX, CollisionResponse::Overlap);
    const ObjectHandle block = SpawnVisibilityBox(fixture, SharedX, CollisionResponse::Block);
    const ObjectHandle overlap = SpawnVisibilityBox(fixture, SharedX, CollisionResponse::Overlap);
    REQUIRE(!freed.IsNull() && !block.IsNull() && !overlap.IsNull());
    DisableCollision(world, freed);
    // 마지막에 만들었지만 첫 칸을 쓴다.
    const ObjectHandle reused = SpawnVisibilityBox(fixture, SharedX, CollisionResponse::Overlap);
    REQUIRE(!reused.IsNull());
    REQUIRE(fixture.Body(reused).index < fixture.Body(overlap).index);

    Array<HitResult> hits(fixture.allocator);
    REQUIRE(LineTraceAll(world, TraceStart, TraceEnd, Visibility, {}, hits));
    REQUIRE(hits.Size() == 3u);
    CHECK(hits[0].component == reused);
    CHECK(hits[1].component == overlap);
    CHECK(hits[2].component == block);
    CHECK(hits[0].time == hits[2].time);

    // trace 방향을 뒤집어도 같은 규칙이다.
    REQUIRE(LineTraceAll(world, TraceEnd, TraceStart, Visibility, {}, hits));
    REQUIRE(hits.Size() == 3u);
    CHECK(hits[0].component == reused);
    CHECK(hits[1].component == overlap);
    CHECK(hits[2].component == block);
}

// First의 같은 fraction 막음은 칸 번호가 작은 쪽이다(early-out을 다음 float로 두어 같은 fraction의
// body도 비교한다 — 7.1.7절).
TEST(PhysicsQuery_FirstTieUsesBodyHandleOrder) {
    constexpr double SharedX = 0.0;
    constexpr int Blocks = 4;
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    World& world = *fixture.world;
    ObjectHandle boxes[Blocks];
    for (ObjectHandle& box : boxes) {
        box = SpawnVisibilityBox(fixture, SharedX, CollisionResponse::Block);
        REQUIRE(!box.IsNull());
    }

    HitResult hit;
    REQUIRE(LineTraceFirst(world, TraceStart, TraceEnd, Visibility, {}, hit));
    CHECK(hit.component == boxes[0]);
    // 첫 칸을 비우고 새 상자가 쓰게 하면 그 상자가 고른 것이 된다(만든 순서는 마지막).
    DisableCollision(world, boxes[0]);
    const ObjectHandle reused = SpawnVisibilityBox(fixture, SharedX, CollisionResponse::Block);
    REQUIRE(!reused.IsNull());
    REQUIRE(LineTraceFirst(world, TraceStart, TraceEnd, Visibility, {}, hit));
    CHECK(hit.component == reused);
    REQUIRE(LineTraceFirst(world, TraceEnd, TraceStart, Visibility, {}, hit));
    CHECK(hit.component == reused);
}

// step하지 않는 Editor World의 장면도 질의한다(편집기 picking — 2.1절).
TEST(PhysicsQuery_EditorWorldQueries) {
    BodyWorldFixture editor(true, WorldKind::Editor);
    REQUIRE(editor.registered);
    REQUIRE(editor.world);
    REQUIRE(editor.world->Initialize());
    const ObjectHandle box = SpawnBoxAt(editor, 0.0);
    REQUIRE(!box.IsNull());
    HitResult hit;
    REQUIRE(LineTraceFirst(*editor.world, TraceStart, TraceEnd, Visibility, {}, hit));
    CHECK(hit.component == box);
}

} // namespace physics_query_tests_file
