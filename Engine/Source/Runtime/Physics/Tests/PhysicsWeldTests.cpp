// Physics 모듈 PH6b2 컴포넌트 시험이다(Docs/Design/PhysicsModule 7.1.23절): autoWeld 상태
// 생성(시뮬레이션 루트에만, QueryOnly 자식 제외), weldSimulatedBodies 부착, 떼기·루트 상태
// 파괴·자식 파괴의 unweld, 루트 시뮬레이션 켬의 ApplyWeldOnChildren, 직접 이동의 다시 weld와 부모
// 이동, API를 루트로, weld 자식 overlap, 깨움 이벤트 전달. World·컴포넌트 공개 API만 쓰므로 Modular
// 시험에서도 돈다.
#include "PhysicsBodyFixture.h"

#include <World/Physics/QueryFilter.h>
#include <World/Physics/WorldCollision.h>

using namespace rocks;
namespace physics_weld_tests_file {

using namespace physics_test_support;

namespace {

constexpr double ChildHalf = 10.0;
constexpr double ChildOffset = 100.0;
constexpr double MovedChildOffset = 200.0;
constexpr double RootMoveX = 300.0;
constexpr double TraceTop = 1000.0;
constexpr double DistanceTolerance = 1.0e-2; // cm
constexpr double KickImpulse = 1000.0;       // kg·cm/s
constexpr int SleepSteps = 45;
// overlap 표적 중심의 x다(자식 상자와 반만 겹친다).
constexpr double OverlapTargetX = ChildHalf * 1.5;
constexpr double BiggerChildHalf = 30.0;
constexpr double SweepLength = 500.0;
constexpr double OtherRootX = 1000.0;

struct WakeCount {
    static inline ObjectHandle watched;
    static inline int sleeps = 0;
};

bool StartWorld(BodyWorldFixture& fixture) noexcept {
    return fixture.registered && fixture.world && fixture.world->Initialize() &&
           fixture.world->BeginPlay();
}

QueryFilter StaticObjects() noexcept {
    return QueryFilter::ByObjectTypes(CollisionObjectTypeBit(CollisionChannel::WorldStatic));
}

// 위에서 x에 쏜 line trace의 첫 hit다.
bool TraceDown(BodyWorldFixture& fixture, double x, HitResult& hit) noexcept {
    return LineTraceFirst(*fixture.world, Vec3d(x, 0.0, TraceTop), Vec3d(x, 0.0, -TraceTop),
                          StaticObjects(), {}, hit);
}

// 루트 구(simulate면 시뮬레이션, 중력 없음)와 그 위 ChildOffset의 상자 자식(autoWeld)을 만든다.
bool SpawnPair(BodyWorldFixture& fixture, ObjectHandle& root, ObjectHandle& child, bool simulate,
               bool autoWeld, CollisionEnabled childCollision = CollisionEnabled::QueryAndPhysics,
               bool childWakeEvents = false) noexcept {
    return fixture.SpawnAttached<SphereComponent, BoxComponent>(
        root, child,
        [&](SphereComponent& sphere) noexcept {
            (void)sphere.SetSphereRadius(static_cast<float>(SphereRadius));
            sphere.SetSimulatePhysics(simulate);
            sphere.SetEnableGravity(false);
        },
        [&](BoxComponent& box) noexcept {
            (void)box.SetBoxExtent(Vec3d(ChildHalf, ChildHalf, ChildHalf));
            (void)box.SetRelativeLocation(Vec3d(0.0, 0.0, ChildOffset));
            (void)box.SetCollisionEnabled(childCollision);
            box.SetAutoWeld(autoWeld);
            box.SetGenerateWakeEvents(childWakeEvents);
            if (childWakeEvents) {
                (void)box.componentSleep.TryAddLambda(
                    [](WeakObjectRef<PrimitiveComponent> component) noexcept {
                        if (component.Handle() == WakeCount::watched) {
                            ++WakeCount::sleeps;
                        }
                    });
            }
        });
}

// 루트 구(시뮬레이션) ← 가운데(middle 설정) ← autoWeld 상자 사슬을 한 Actor에 만든다.
template <class ConfigureMiddle>
bool SpawnChain(BodyWorldFixture& fixture, ObjectHandle& root, ObjectHandle& middle,
                ObjectHandle& leaf, ConfigureMiddle&& configureMiddle) noexcept {
    World& world = *fixture.world;
    auto actor = world.SpawnActorDeferred<PhysicsTestActor>();
    if (!actor) {
        return false;
    }

    auto rootComponent = world.AddComponent<SphereComponent>(actor.Value());
    auto middleComponent = world.AddComponent<BoxComponent>(actor.Value());
    auto leafComponent = world.AddComponent<BoxComponent>(actor.Value());
    if (!rootComponent || !middleComponent || !leafComponent) {
        return false;
    }

    root = rootComponent.Value();
    middle = middleComponent.Value();
    leaf = leafComponent.Value();
    (void)world.With<SphereComponent>(root, [](SphereComponent& sphere) noexcept {
        sphere.SetSimulatePhysics(true);
        sphere.SetEnableGravity(false);
    });
    (void)world.With<BoxComponent>(middle, [&](BoxComponent& box) noexcept {
        (void)box.SetBoxExtent(Vec3d(ChildHalf, ChildHalf, ChildHalf));
        (void)box.SetRelativeLocation(Vec3d(0.0, 0.0, ChildOffset));
        configureMiddle(box);
        (void)box.AttachTo(root);
    });
    (void)world.With<BoxComponent>(leaf, [&](BoxComponent& box) noexcept {
        (void)box.SetBoxExtent(Vec3d(ChildHalf, ChildHalf, ChildHalf));
        (void)box.SetRelativeLocation(Vec3d(0.0, 0.0, ChildOffset));
        box.SetAutoWeld(true);
        (void)box.AttachTo(middle);
    });
    return world.FinishSpawning(actor.Value()).IsOk();
}

ObjectHandle WeldRootOf(BodyWorldFixture& fixture, ObjectHandle component) noexcept {
    ObjectHandle root;
    (void)Apply<PrimitiveComponent>(
        *fixture.world, component,
        [&](PrimitiveComponent& primitive) noexcept { root = primitive.WeldRoot(); });
    return root;
}

bool IsWelded(BodyWorldFixture& fixture, ObjectHandle component) noexcept {
    bool welded = false;
    (void)Apply<PrimitiveComponent>(
        *fixture.world, component,
        [&](PrimitiveComponent& primitive) noexcept { welded = primitive.IsWelded(); });
    return welded;
}

} // namespace

// autoWeld 자식은 시뮬레이션하는 루트 body에 합쳐진다: 자기 body가 없고 위에서 쏜 ray가 자식 윗면을
// 자식 컴포넌트로 맞힌다.
TEST(PhysicsWeld_AutoWeldToSimulatingRoot) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    ObjectHandle root;
    ObjectHandle child;
    REQUIRE(SpawnPair(fixture, root, child, true, true));
    CHECK(IsWelded(fixture, child));
    CHECK(fixture.Body(child).IsNull());
    CHECK(fixture.BodyCount() == 1u);
    HitResult hit;
    REQUIRE(TraceDown(fixture, 0.0, hit));
    CHECK(hit.component == child);
    CHECK(math::Abs(hit.impactPoint.z - (ChildOffset + ChildHalf)) <= DistanceTolerance);
}

// 루트가 시뮬레이션하지 않으면(Kinematic) weld하지 않는다(UE — 자기 body로 남는다). QueryOnly
// 자식도 weld하지 않는다.
TEST(PhysicsWeld_KinematicRootAndQueryOnlyChildKeepOwnBodies) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    ObjectHandle root;
    ObjectHandle child;
    REQUIRE(SpawnPair(fixture, root, child, false, true));
    CHECK(!IsWelded(fixture, child));
    CHECK(!fixture.Body(child).IsNull());

    BodyWorldFixture other;
    REQUIRE(StartWorld(other));
    ObjectHandle queryRoot;
    ObjectHandle queryChild;
    REQUIRE(SpawnPair(other, queryRoot, queryChild, true, true, CollisionEnabled::QueryOnly));
    CHECK(!IsWelded(other, queryChild));
    CHECK(!other.Body(queryChild).IsNull());
}

// 떼면 weld가 풀리고 자식이 자기 body를 다시 만든다(UE DetachFromComponent → UnWeldFromParent).
// weldSimulatedBodies 규칙으로 다시 붙이면 weld한다.
TEST(PhysicsWeld_DetachAndWeldAttach) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    ObjectHandle root;
    ObjectHandle child;
    REQUIRE(SpawnPair(fixture, root, child, true, true));
    REQUIRE(Apply<PrimitiveComponent>(*fixture.world, child, [](PrimitiveComponent& box) noexcept {
        (void)box.Detach(AttachmentRule::KeepWorld);
    }));
    CHECK(!IsWelded(fixture, child));
    CHECK(!fixture.Body(child).IsNull());
    CHECK(fixture.BodyCount() == 2u);

    AttachmentRules rules = AttachmentRules::KeepWorldTransform();
    rules.weldSimulatedBodies = true;
    REQUIRE(Apply<PrimitiveComponent>(*fixture.world, child, [&](PrimitiveComponent& box) noexcept {
        (void)box.AttachTo(root, rules);
    }));
    CHECK(IsWelded(fixture, child));
    CHECK(fixture.BodyCount() == 1u);
}

// 루트가 시뮬레이션을 켜면 autoWeld 자식을 합친다(UE ApplyWeldOnChildren).
TEST(PhysicsWeld_RootSimulateAppliesWeld) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    ObjectHandle root;
    ObjectHandle child;
    REQUIRE(SpawnPair(fixture, root, child, false, true));
    REQUIRE(!IsWelded(fixture, child));
    REQUIRE(
        Apply<PrimitiveComponent>(*fixture.world, root, [](PrimitiveComponent& sphere) noexcept {
            sphere.SetSimulatePhysics(true);
        }));
    CHECK(IsWelded(fixture, child));
    CHECK(fixture.Body(child).IsNull());
}

// 자식을 직접 옮기면 다시 weld한다(새 자리 hit — UE SetWeldedBodyTransform). 루트를 옮기면 자식은
// 부착으로 따라가고 weld를 유지한다.
TEST(PhysicsWeld_DirectMoveRewelds) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    ObjectHandle root;
    ObjectHandle child;
    REQUIRE(SpawnPair(fixture, root, child, true, true));
    REQUIRE(Apply<PrimitiveComponent>(*fixture.world, child, [](PrimitiveComponent& box) noexcept {
        (void)box.SetRelativeLocation(Vec3d(0.0, 0.0, MovedChildOffset));
    }));
    HitResult hit;
    REQUIRE(TraceDown(fixture, 0.0, hit));
    CHECK(hit.component == child);
    CHECK(math::Abs(hit.impactPoint.z - (MovedChildOffset + ChildHalf)) <= DistanceTolerance);

    REQUIRE(
        Apply<PrimitiveComponent>(*fixture.world, root, [](PrimitiveComponent& sphere) noexcept {
            (void)sphere.SetRelativeLocation(Vec3d(RootMoveX, 0.0, 0.0));
        }));
    CHECK(IsWelded(fixture, child));
    REQUIRE(TraceDown(fixture, RootMoveX, hit));
    CHECK(hit.component == child);
    CHECK(math::Abs(hit.impactPoint.z - (MovedChildOffset + ChildHalf)) <= DistanceTolerance);
}

// weld 자식의 물리 API는 루트 body로 간다(UE GetBodyInstance() 기본 weld): 충격량이 루트 속도를
// 바꾸고, body 값 setter가 루트 값을 바꾸고, 질량은 루트(합친) 질량이다.
TEST(PhysicsWeld_ApiGoesToRoot) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    ObjectHandle root;
    ObjectHandle child;
    REQUIRE(SpawnPair(fixture, root, child, true, true));
    float childMass = 0.0f;
    REQUIRE(Apply<PrimitiveComponent>(*fixture.world, child, [&](PrimitiveComponent& box) noexcept {
        box.AddImpulse(Vec3d(KickImpulse, 0.0, 0.0));
        box.SetLinearDamping(0.0f);
        childMass = box.GetMass();
    }));
    fixture.Step(1);
    float rootMass = 0.0f;
    float rootDamping = -1.0f;
    Vec3d rootVelocity = Vec3d::Zero();
    REQUIRE(
        Apply<PrimitiveComponent>(*fixture.world, root, [&](PrimitiveComponent& sphere) noexcept {
            rootMass = sphere.GetMass();
            rootDamping = sphere.GetLinearDamping();
            rootVelocity = sphere.GetPhysicsLinearVelocity();
        }));
    CHECK(childMass == rootMass);
    CHECK(rootDamping == 0.0f);
    CHECK(rootVelocity.x > 0.0);
}

// 루트의 물리 상태가 없어지면(충돌 끔) 자식이 풀려 자기 body를 만든다(UE OnDestroyPhysicsState →
// UnWeldChildren). 자식을 지우면 루트 body에서 빠진다.
TEST(PhysicsWeld_RootStateDestroyAndChildDestroy) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    ObjectHandle root;
    ObjectHandle child;
    REQUIRE(SpawnPair(fixture, root, child, true, true));
    REQUIRE(
        Apply<PrimitiveComponent>(*fixture.world, root, [](PrimitiveComponent& sphere) noexcept {
            (void)sphere.SetCollisionEnabled(CollisionEnabled::NoCollision);
        }));
    CHECK(!IsWelded(fixture, child));
    CHECK(!fixture.Body(child).IsNull());
    CHECK(fixture.BodyCount() == 1u);

    BodyWorldFixture other;
    REQUIRE(StartWorld(other));
    ObjectHandle weldedRoot;
    ObjectHandle weldedChild;
    REQUIRE(SpawnPair(other, weldedRoot, weldedChild, true, true));
    REQUIRE(other.world->DestroyComponent(weldedChild));
    other.Step(1);
    HitResult hit;
    REQUIRE(TraceDown(other, 0.0, hit));
    CHECK(hit.component == weldedRoot);
}

// weld 자식의 컴포넌트 overlap은 루트 body의 자기 부분 원소를 쓴다(UE OverlapMulti weld 경로).
TEST(PhysicsWeld_ChildOverlapUsesItsPart) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    ObjectHandle root;
    ObjectHandle child;
    REQUIRE(SpawnPair(fixture, root, child, true, true));
    const ObjectHandle target = fixture.SpawnShape<BoxComponent>([](BoxComponent& box) noexcept {
        box.SetMobility(ComponentMobility::Static);
        (void)box.SetBoxExtent(Vec3d(ChildHalf, ChildHalf, ChildHalf));
        (void)box.SetRelativeLocation(Vec3d(OverlapTargetX, 0.0, ChildOffset));
    });
    REQUIRE(!target.IsNull());
    Array<OverlapResult> overlaps(fixture.allocator);
    REQUIRE(ComponentOverlapAll(*fixture.world, child, Vec3d(0.0, 0.0, ChildOffset),
                                Quatd::Identity(), {}, overlaps));
    bool found = false;
    for (const OverlapResult& overlap : overlaps) {
        found = found || overlap.component == target;
    }

    CHECK(found);
    // 루트 컴포넌트 overlap은 자식 원소를 쓰지 않는다.
    Array<OverlapResult> rootOverlaps(fixture.allocator);
    REQUIRE(ComponentOverlapAll(*fixture.world, root, Vec3d::Zero(), Quatd::Identity(), {},
                                rootOverlaps));
    for (const OverlapResult& overlap : rootOverlaps) {
        CHECK(overlap.component != target);
    }
}

// 루트가 잠들면 weld 자식에도 잠듦 이벤트가 간다(UE DispatchWakeEvents의 weld 자식 재귀 — 자식의
// generateWakeEvents로 거른다).
TEST(PhysicsWeld_WakeEventsReachWeldedChild) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    ObjectHandle root;
    ObjectHandle child;
    WakeCount::watched = {};
    WakeCount::sleeps = 0;
    REQUIRE(SpawnPair(fixture, root, child, true, true, CollisionEnabled::QueryAndPhysics, true));
    REQUIRE(IsWelded(fixture, child));
    WakeCount::watched = child;
    fixture.Step(SleepSteps);
    CHECK(WakeCount::sleeps == 1);
}

// WeldTo는 붙어 있으면 그대로 weld한다(UE WeldTo). 같은 부모에 다시 붙여도 weld는 풀리지 않는다(UE
// "already attached").
TEST(PhysicsWeld_WeldToAndSameParentReattach) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    ObjectHandle root;
    ObjectHandle child;
    REQUIRE(SpawnPair(fixture, root, child, true, false));
    REQUIRE(!IsWelded(fixture, child));
    REQUIRE(Apply<PrimitiveComponent>(*fixture.world, child,
                                      [&](PrimitiveComponent& box) noexcept { box.WeldTo(root); }));
    CHECK(IsWelded(fixture, child));
    REQUIRE(Apply<PrimitiveComponent>(*fixture.world, child, [&](PrimitiveComponent& box) noexcept {
        (void)box.AttachTo(root, AttachmentRules::KeepRelativeTransform());
    }));
    CHECK(IsWelded(fixture, child));
}

// weld 자식을 가진 컴포넌트를 다른 루트에 weld하면 그 자식도 새 루트로 옮겨진다(UE Weld — 상대의
// weld body를 다시 weld).
TEST(PhysicsWeld_WeldedSubtreeMovesToNewRoot) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    ObjectHandle middle;
    ObjectHandle child;
    REQUIRE(SpawnPair(fixture, middle, child, true, true));
    REQUIRE(IsWelded(fixture, child));
    const ObjectHandle other =
        fixture.SpawnShape<SphereComponent>([](SphereComponent& sphere) noexcept {
            (void)sphere.SetRelativeLocation(Vec3d(OtherRootX, 0.0, 0.0));
            sphere.SetSimulatePhysics(true);
            sphere.SetEnableGravity(false);
        });
    REQUIRE(!other.IsNull());
    REQUIRE(
        Apply<PrimitiveComponent>(*fixture.world, middle, [&](PrimitiveComponent& sphere) noexcept {
            sphere.WeldTo(other);
        }));
    CHECK(WeldRootOf(fixture, middle) == other);
    CHECK(WeldRootOf(fixture, child) == other);
    HitResult hit;
    REQUIRE(TraceDown(fixture, 0.0, hit));
    CHECK(hit.component == child);
}

// body 없는 가운데 컴포넌트(충돌 없음)를 거친 자손도 루트에 weld되고, 루트 상태가 다시 만들어지거나
// 가운데를 떼면 그 자손이 stale 없이 자기 body를 가진다.
TEST(PhysicsWeld_BodylessIntermediate) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    ObjectHandle root;
    ObjectHandle middle;
    ObjectHandle leaf;
    REQUIRE(SpawnChain(fixture, root, middle, leaf, [](BoxComponent& box) noexcept {
        (void)box.SetCollisionEnabled(CollisionEnabled::NoCollision);
    }));
    REQUIRE(WeldRootOf(fixture, leaf) == root);
    REQUIRE(
        Apply<PrimitiveComponent>(*fixture.world, root, [](PrimitiveComponent& sphere) noexcept {
            (void)sphere.SetCollisionEnabled(CollisionEnabled::NoCollision);
            (void)sphere.SetCollisionEnabled(CollisionEnabled::QueryAndPhysics);
        }));
    // 가운데가 weld도 autoWeld도 아니라 다시 합치지 않는다(UE GetWeldedBodies) — 자기 body다.
    CHECK(!IsWelded(fixture, leaf));
    CHECK(!fixture.Body(leaf).IsNull());

    BodyWorldFixture other;
    REQUIRE(StartWorld(other));
    ObjectHandle otherRoot;
    ObjectHandle otherMiddle;
    ObjectHandle otherLeaf;
    REQUIRE(SpawnChain(other, otherRoot, otherMiddle, otherLeaf, [](BoxComponent& box) noexcept {
        (void)box.SetCollisionEnabled(CollisionEnabled::NoCollision);
    }));
    REQUIRE(WeldRootOf(other, otherLeaf) == otherRoot);
    REQUIRE(
        Apply<PrimitiveComponent>(*other.world, otherMiddle, [](PrimitiveComponent& box) noexcept {
            (void)box.Detach(AttachmentRule::KeepWorld);
        }));
    CHECK(!IsWelded(other, otherLeaf));
    CHECK(!other.Body(otherLeaf).IsNull());
}

// 가운데 weld 컴포넌트를 풀면 그 아래의 같은 루트 weld도 풀려 둘 다 자기 body를 가진다.
TEST(PhysicsWeld_UnweldCascades) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    ObjectHandle root;
    ObjectHandle middle;
    ObjectHandle leaf;
    REQUIRE(SpawnChain(fixture, root, middle, leaf,
                       [](BoxComponent& box) noexcept { box.SetAutoWeld(true); }));
    REQUIRE(WeldRootOf(fixture, middle) == root);
    REQUIRE(WeldRootOf(fixture, leaf) == root);
    REQUIRE(Apply<PrimitiveComponent>(
        *fixture.world, middle, [](PrimitiveComponent& box) noexcept { box.UnWeldFromParent(); }));
    CHECK(!IsWelded(fixture, middle));
    CHECK(!IsWelded(fixture, leaf));
    CHECK(!fixture.Body(middle).IsNull());
    CHECK(!fixture.Body(leaf).IsNull());
}

// 루트 충돌을 껐다 켜면 풀렸던 autoWeld 자식이 다시 합쳐진다(UE SetCollisionEnabled →
// ApplyWeldOnChildren). weld 자식의 충돌 켬 변경은 weld를 유지한다.
TEST(PhysicsWeld_CollisionRoundTripRewelds) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    ObjectHandle root;
    ObjectHandle child;
    REQUIRE(SpawnPair(fixture, root, child, true, true));
    REQUIRE(
        Apply<PrimitiveComponent>(*fixture.world, root, [](PrimitiveComponent& sphere) noexcept {
            (void)sphere.SetCollisionEnabled(CollisionEnabled::NoCollision);
            (void)sphere.SetCollisionEnabled(CollisionEnabled::QueryAndPhysics);
        }));
    CHECK(IsWelded(fixture, child));
    REQUIRE(Apply<PrimitiveComponent>(*fixture.world, child, [](PrimitiveComponent& box) noexcept {
        (void)box.SetCollisionEnabled(CollisionEnabled::PhysicsOnly);
    }));
    CHECK(IsWelded(fixture, child));
    HitResult hit;
    REQUIRE(TraceDown(fixture, 0.0, hit));
    // 물리 전용 원소는 질의에 걸리지 않아 루트 구가 맞는다.
    CHECK(hit.component == root);
}

// weld 자식이 시뮬레이션을 켜면 떼어져 자기 Dynamic body를 가진다(UE SetInstanceSimulatePhysics).
// weld 자식의 형상 변경은 다시 weld한다. weld 자식의 컴포넌트 sweep은 hit 없음이다.
TEST(PhysicsWeld_ChildSimulateShapeAndSweep) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    ObjectHandle root;
    ObjectHandle child;
    REQUIRE(SpawnPair(fixture, root, child, true, true));
    REQUIRE(Apply<BoxComponent>(*fixture.world, child, [](BoxComponent& box) noexcept {
        (void)box.SetBoxExtent(Vec3d(BiggerChildHalf, BiggerChildHalf, BiggerChildHalf));
    }));
    HitResult hit;
    REQUIRE(TraceDown(fixture, 0.0, hit));
    CHECK(hit.component == child);
    CHECK(math::Abs(hit.impactPoint.z - (ChildOffset + BiggerChildHalf)) <= DistanceTolerance);
    Array<HitResult> hits(fixture.allocator);
    REQUIRE(ComponentTraceAll(*fixture.world, child, Vec3d(0.0, 0.0, ChildOffset),
                              Vec3d(SweepLength, 0.0, ChildOffset), Quatd::Identity(), {}, hits));
    CHECK(hits.IsEmpty());

    REQUIRE(Apply<PrimitiveComponent>(*fixture.world, child, [](PrimitiveComponent& box) noexcept {
        box.SetSimulatePhysics(true);
    }));
    CHECK(!IsWelded(fixture, child));
    CHECK(fixture.Parent(child).IsNull());
    CHECK(!fixture.Body(child).IsNull());
}

// weldSimulatedBodies로 시뮬레이션하지 않는 루트에 붙이면 자식 시뮬레이션이 꺼진다(UE
// WeldToImplementation의 Kinematic 루트 — SetSimulatePhysics(false)).
TEST(PhysicsWeld_KinematicRootTurnsOffChildSimulation) {
    BodyWorldFixture fixture;
    REQUIRE(StartWorld(fixture));
    ObjectHandle root;
    ObjectHandle child;
    REQUIRE(SpawnPair(fixture, root, child, false, false));
    REQUIRE(Apply<PrimitiveComponent>(*fixture.world, child, [](PrimitiveComponent& box) noexcept {
        (void)box.Detach(AttachmentRule::KeepWorld);
        box.SetSimulatePhysics(true);
    }));
    AttachmentRules rules = AttachmentRules::KeepWorldTransform();
    rules.weldSimulatedBodies = true;
    bool simulating = true;
    REQUIRE(Apply<PrimitiveComponent>(*fixture.world, child, [&](PrimitiveComponent& box) noexcept {
        (void)box.AttachTo(root, rules);
        simulating = box.GetBodySettings().simulatePhysics;
    }));
    CHECK(!simulating);
    CHECK(!IsWelded(fixture, child));
}

// 게임 World가 아니면 autoWeld하지 않는다(UE IsGameWorld).
TEST(PhysicsWeld_NoAutoWeldOutsideGameWorld) {
    BodyWorldFixture fixture(true, WorldKind::Editor);
    REQUIRE(fixture.registered && fixture.world && fixture.world->Initialize());
    ObjectHandle root;
    ObjectHandle child;
    REQUIRE(SpawnPair(fixture, root, child, true, true));
    CHECK(!IsWelded(fixture, child));
}

} // namespace physics_weld_tests_file
