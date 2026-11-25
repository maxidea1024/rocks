#include <Base/InitializerList.h>
#include <Core/Math/Rotator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Meta/TypeSchema.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <TestSupport/Test.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Level.generated.h>
#include <World/Lights/LightingChannels.generated.h>
#include <World/Primitives/PrimitiveTypes.generated.h>
#include <World/Primitives/PrimitiveComponent.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/Spline/SplineTypes.generated.h>
#include <World/Spline/SplineComponent.generated.h>
#include <World/Spline/SplineTypes.h>
#include <World/Spline/SplineComponent.h>
#include <World/World.h>
#include <math.h>
#include <stdio.h>

// D3 SplineComponent 시험이다(Docs/Design/EngineRuntime 25.9절): 기본값, 점 편집 키, 구간 길이(독립
// 수치 적분 대조)·월드 scale·루프, 거리·시간, 방향·회전(MakeFromXZ)·World 공간, 최근접, 회전·up
// 설정, bounds, 리플렉션·저장 왕복.
using namespace rocks;
namespace world_spline_component_tests_file {

namespace {

constexpr double Tolerance = 1.0e-3;
constexpr SplineCoordinateSpace Local = SplineCoordinateSpace::Local;
constexpr SplineCoordinateSpace WorldSpace = SplineCoordinateSpace::World;

bool Near(double value, double expected, double tolerance = Tolerance) noexcept {
    const bool same = fabs(value - expected) <= tolerance;
    if (!same) {
        printf("  value %.9g, expected %.9g\n", value, expected);
    }

    return same;
}

bool NearVector(const Vec3d& value, double x, double y, double z,
                double tolerance = Tolerance) noexcept {
    const bool same = fabs(value.x - x) <= tolerance && fabs(value.y - y) <= tolerance &&
                      fabs(value.z - z) <= tolerance;
    if (!same) {
        printf("  vector (%.9g, %.9g, %.9g), expected (%g, %g, %g)\n", value.x, value.y, value.z, x,
               y, z);
    }

    return same;
}

ErrorCode ErrorOf(const Result<void>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

template <class T>
T* Get(Borrow<T>& borrow) noexcept {
    auto pointer = borrow.Get();
    return pointer ? pointer.Value() : nullptr;
}

float KeyAt(const SplineComponent& spline, int32 index) noexcept {
    auto key = spline.InputKeyValueAtSplinePoint(index);
    return key ? key.Value() : -1.0f;
}

Vec3d PointAt(const SplineComponent& spline, int32 index) noexcept {
    auto location = spline.LocationAtSplinePoint(index, Local);
    return location ? location.Value() : Vec3d(-1.0, -1.0, -1.0);
}

// 구간 0의 Hermite 곡선 길이를 Simpson 법(짝수 n)으로 독립 계산한다(키 간격 1).
double SimpsonHermiteLength(const Vec3d& p0, const Vec3d& t0, const Vec3d& p1, const Vec3d& t1,
                            int steps) noexcept {
    const auto speed = [&](double t) noexcept {
        const double h00 = 6.0 * t * t - 6.0 * t;
        const double h10 = 3.0 * t * t - 4.0 * t + 1.0;
        const double h01 = -6.0 * t * t + 6.0 * t;
        const double h11 = 3.0 * t * t - 2.0 * t;
        return (p0 * h00 + t0 * h10 + p1 * h01 + t1 * h11).Length();
    };

    const double h = 1.0 / steps;
    double sum = speed(0.0) + speed(1.0);
    for (int index = 1; index < steps; ++index) {
        sum += speed(index * h) * (index % 2 == 1 ? 4.0 : 2.0);
    }

    return sum * h / 3.0;
}

// UE GetSegmentLength의 Legendre–Gauss 5점 식(SplineComponent.cpp:854)을 따로 쓴다(scale 1).
double LegendreGaussHermiteLength(const Vec3d& p0, const Vec3d& t0, const Vec3d& p1,
                                  const Vec3d& t1, float param) noexcept {
    const float abscissa[] = {0.0f, -0.5384693f, 0.5384693f, -0.90617985f, 0.90617985f};
    const float weight[] = {0.5688889f, 0.47862867f, 0.47862867f, 0.23692688f, 0.23692688f};
    const Vec3d c1 = ((p0 - p1) * 2.0 + t0 + t1) * 3.0;
    const Vec3d c2 = (p1 - p0) * 6.0 - t0 * 4.0 - t1 * 2.0;
    const float half = param * 0.5f;
    float length = 0.0f;
    for (int index = 0; index < 5; ++index) {
        const double alpha = static_cast<double>(half * (1.0f + abscissa[index]));
        length = static_cast<float>(length + ((c1 * alpha + c2) * alpha + t0).Length() *
                                                 static_cast<double>(weight[index]));
    }

    return static_cast<double>(length * half);
}

struct Fixture {
    explicit Fixture(WorldKind kind = WorldKind::Game) noexcept
        : types(allocator), modules(types, allocator) {
        registered = RegisterWorldTypes(modules).IsOk();
        WorldDesc desc;
        desc.name = "Splines";
        desc.context = {0x53504c4eull, 1};
        desc.timeStep = {1.0 / 60.0, 0.25, 8};
        desc.kind = kind;
        auto created = UniquePtr<World>::TryMake(allocator, allocator, types, desc);
        if (created) {
            world = Move(created.Value());
        }
    }

    ~Fixture() {
        world.Reset();
        CHECK(modules.Unregister().IsOk());
    }

    // 리플렉션 Actor 하나에 root 스플라인을 붙인다.
    ObjectHandle AddSpline(ObjectHandle level = {}) noexcept {
        SpawnParams params;
        params.level = level;
        auto actor = world->SpawnActor<Actor>(params);
        if (!actor) {
            return {};
        }

        auto spline = world->AddComponent<SplineComponent>(actor.Value());
        if (!spline) {
            return {};
        }

        auto rooted = world->With<Actor>(actor.Value(), [&](Actor& value) noexcept {
            return value.SetRootComponent(spline.Value());
        });
        return rooted ? spline.Value() : ObjectHandle{};
    }

    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration modules;
    bool registered = false;
    UniquePtr<World> world;
};

// 사용 예: fixture.Run(spline, [](SplineComponent& s) noexcept { ... });
template <class Function>
bool Run(Fixture& fixture, ObjectHandle spline, Function&& function) noexcept {
    return fixture.world->With<SplineComponent>(spline, function).IsOk();
}

} // namespace

// 25.2·25.3절: 기본값·기본 두 점(길이 100)·profile.
TEST(Spline_DefaultsMatchUe) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    REQUIRE(fixture.world->Initialize());
    const ObjectHandle spline = fixture.AddSpline();
    REQUIRE(!spline.IsNull());
    CHECK(Run(fixture, spline, [](SplineComponent& path) noexcept {
        CHECK(path.SplinePointCount() == 2 && path.SplineSegmentCount() == 1);
        CHECK(KeyAt(path, 0) == 0.0f && KeyAt(path, 1) == 1.0f);
        CHECK(NearVector(PointAt(path, 0), 0.0, 0.0, 0.0));
        CHECK(NearVector(PointAt(path, 1), 100.0, 0.0, 0.0));
        CHECK(path.SplinePointTypeAt(0).Value() == SplinePointType::Curve);
        // 자동 접선 (100, 0, 0)·길이 100(직선 Hermite).
        CHECK(NearVector(path.LeaveTangentAtSplinePoint(0, Local).Value(), 100.0, 0.0, 0.0));
        CHECK(NearVector(path.ArriveTangentAtSplinePoint(1, Local).Value(), 100.0, 0.0, 0.0));
        CHECK(Near(path.SplineLength(), 100.0));
        CHECK(path.ReparamStepsPerSegment() == 10 && path.Duration() == 1.0f);
        CHECK(!path.HasStationaryEndpoints() && !path.IsClosedLoop());
        CHECK(!path.HasLoopPositionOverride() && path.LoopPosition() == 0.0f);
        CHECK(NearVector(path.DefaultUpVector(Local), 0.0, 0.0, 1.0));
        CHECK(path.CollisionProfileName() == StringView("NoCollision"));
        CHECK(!path.GeneratesOverlapEvents());
        CHECK(path.Mobility() == ComponentMobility::Movable);
        // 설정 검증.
        CHECK(ErrorOf(path.SetReparamStepsPerSegment(3)) == ErrorCode::InvalidArgument);
        CHECK(ErrorOf(path.SetReparamStepsPerSegment(101)) == ErrorCode::InvalidArgument);
        CHECK(ErrorOf(path.SetDuration(-1.0f)) == ErrorCode::InvalidArgument);
        CHECK(ErrorOf(path.SetDefaultUpVector(Vec3d(), Local)) == ErrorCode::InvalidArgument);
        CHECK(path.ReparamStepsPerSegment() == 10);
    }));
    REQUIRE(fixture.world->Shutdown());
}

// 25.6절: 추가·삽입·삭제의 키, loopPosition 조정, 범위 밖 거부, updateSpline 거짓, SetSplinePoints.
TEST(Spline_PointEditingKeys) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    REQUIRE(fixture.world->Initialize());
    const ObjectHandle spline = fixture.AddSpline();
    REQUIRE(!spline.IsNull());
    CHECK(Run(fixture, spline, [](SplineComponent& path) noexcept {
        CHECK(path.AddSplinePoint(Vec3d(200.0, 0.0, 0.0), Local));
        CHECK(KeyAt(path, 2) == 2.0f);
        CHECK(path.AddSplinePointAtIndex(Vec3d(50.0, 50.0, 0.0), 1, Local));
        CHECK(path.SplinePointCount() == 4);
        for (int32 index = 0; index < 4; ++index) {
            CHECK(KeyAt(path, index) == static_cast<float>(index));
        }

        CHECK(NearVector(PointAt(path, 1), 50.0, 50.0, 0.0));
        CHECK(NearVector(PointAt(path, 2), 100.0, 0.0, 0.0));
        CHECK(ErrorOf(path.AddSplinePointAtIndex(Vec3d(), 9, Local)) == ErrorCode::InvalidArgument);
        CHECK(ErrorOf(path.AddSplinePointAtIndex(Vec3d(), -1, Local)) ==
              ErrorCode::InvalidArgument);
        // 삭제: 뒤 키를 앞 간격만큼 당긴다.
        CHECK(path.RemoveSplinePoint(1));
        CHECK(path.SplinePointCount() == 3);
        CHECK(KeyAt(path, 1) == 1.0f && KeyAt(path, 2) == 2.0f);
        CHECK(NearVector(PointAt(path, 1), 100.0, 0.0, 0.0));
        CHECK(ErrorOf(path.RemoveSplinePoint(3)) == ErrorCode::InvalidArgument);
        // loopPositionOverride면 추가·삭제가 loopPosition을 1씩 옮긴다.
        CHECK(path.SetClosedLoopAtPosition(true, 5.0f));
        CHECK(path.HasLoopPositionOverride() && path.IsClosedLoop());
        CHECK(path.AddSplinePoint(Vec3d(300.0, 0.0, 0.0), Local));
        CHECK(path.LoopPosition() == 6.0f);
        CHECK(path.RemoveSplinePoint(3));
        CHECK(path.LoopPosition() == 5.0f);
        path.SetClosedLoop(false);
        CHECK(!path.HasLoopPositionOverride());
        // AddPoint: 같은 키가 있으면 그 자리에 넣고 뒤를 민다.
        SplinePoint point;
        point.inputKey = 1.0f;
        point.position = Vec3d(0.0, 77.0, 0.0);
        point.type = SplinePointType::Linear;
        CHECK(path.AddPoint(point));
        CHECK(path.SplinePointCount() == 4);
        CHECK(NearVector(PointAt(path, 1), 0.0, 77.0, 0.0));
        CHECK(KeyAt(path, 1) == 1.0f && KeyAt(path, 2) == 2.0f && KeyAt(path, 3) == 3.0f);
        CHECK(path.SplinePointTypeAt(1).Value() == SplinePointType::Linear);
        // updateSpline 거짓: 길이는 UpdateSpline 전까지 그대로다.
        const double before = path.SplineLength();
        CHECK(path.SetLocationAtSplinePoint(3, Vec3d(1000.0, 0.0, 0.0), Local, false));
        CHECK(Near(path.SplineLength(), before));
        path.UpdateSpline();
        CHECK(path.SplineLength() > before + 100.0);
        // SetSplinePoints: 키 0, 1, 2.
        const Vec3d points[] = {Vec3d(0.0, 0.0, 0.0), Vec3d(10.0, 0.0, 0.0),
                                Vec3d(10.0, 10.0, 0.0)};
        CHECK(path.SetSplinePoints(Span<const Vec3d>(points, 3), Local));
        CHECK(path.SplinePointCount() == 3 && KeyAt(path, 2) == 2.0f);
        // 첫 점 지우기·첫 자리 넣기.
        CHECK(path.RemoveSplinePoint(0));
        CHECK(KeyAt(path, 0) == 0.0f && KeyAt(path, 1) == 1.0f);
        CHECK(NearVector(PointAt(path, 0), 10.0, 0.0, 0.0));
        CHECK(path.AddSplinePointAtIndex(Vec3d(-5.0, 0.0, 0.0), 0, Local));
        CHECK(KeyAt(path, 0) == 0.0f && KeyAt(path, 1) == 1.0f && KeyAt(path, 2) == 2.0f);
        CHECK(NearVector(PointAt(path, 0), -5.0, 0.0, 0.0));
        // AddPoint가 loopPosition 이상의 키를 넣으면 override를 끈다.
        CHECK(path.SetClosedLoopAtPosition(true, 3.5f));
        SplinePoint far;
        far.inputKey = 4.0f;
        far.position = Vec3d(0.0, 0.0, 30.0);
        CHECK(path.AddPoint(far));
        CHECK(!path.HasLoopPositionOverride());
        far.rotation.yaw = NAN;
        CHECK(ErrorOf(path.AddPoint(far)) == ErrorCode::InvalidArgument);
        path.SetClosedLoop(false);
        // 끝점 고정: 끝점 접선이 0이 된다.
        const Vec3d pair[] = {Vec3d(0.0, 0.0, 0.0), Vec3d(100.0, 0.0, 0.0)};
        CHECK(path.SetSplinePoints(Span<const Vec3d>(pair, 2), Local));
        path.SetStationaryEndpoints(true);
        CHECK(NearVector(path.LeaveTangentAtSplinePoint(0, Local).Value(), 0.0, 0.0, 0.0));
        CHECK(NearVector(path.ArriveTangentAtSplinePoint(1, Local).Value(), 0.0, 0.0, 0.0));
        path.SetStationaryEndpoints(false);
        // 점 하나: 길이 0, 어느 거리든 그 점.
        const Vec3d single[] = {Vec3d(7.0, 8.0, 9.0)};
        CHECK(path.SetSplinePoints(Span<const Vec3d>(single, 1), Local));
        CHECK(path.SplineSegmentCount() == 0 && path.SplineLength() == 0.0);
        CHECK(path.InputKeyValueAtDistanceAlongSpline(5.0) == 0.0f);
        CHECK(NearVector(path.LocationAtDistanceAlongSpline(5.0, Local), 7.0, 8.0, 9.0));
        CHECK(path.TimeAtDistanceAlongSpline(5.0) == 0.0f);
        path.ClearSplinePoints();
        CHECK(path.SplinePointCount() == 0 && path.SplineLength() == 0.0);
        CHECK(path.InputKeyValueAtDistanceAlongSpline(10.0) == 0.0f);
        CHECK(NearVector(path.LocationAtSplineInputKey(0.5f, Local), 0.0, 0.0, 0.0));
    }));
    REQUIRE(fixture.world->Shutdown());
}

// 25.4절: 구간 길이(Linear·Constant·Curve — 독립 Simpson 적분 대조), 월드 scale, 루프.
TEST(Spline_SegmentLengthsScaleAndLoops) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    REQUIRE(fixture.world->Initialize());
    const ObjectHandle spline = fixture.AddSpline();
    REQUIRE(!spline.IsNull());
    CHECK(Run(fixture, spline, [](SplineComponent& path) noexcept {
        // 사각형: Linear 열린 길이 300, 닫으면 400.
        const Vec3d square[] = {Vec3d(0.0, 0.0, 0.0), Vec3d(100.0, 0.0, 0.0),
                                Vec3d(100.0, 100.0, 0.0), Vec3d(0.0, 100.0, 0.0)};
        CHECK(path.SetSplinePoints(Span<const Vec3d>(square, 4), Local, false));
        for (int32 index = 0; index < 4; ++index) {
            CHECK(path.SetSplinePointType(index, SplinePointType::Linear, false));
        }

        path.UpdateSpline();
        CHECK(Near(path.SplineLength(), 300.0));
        CHECK(Near(path.SegmentLength(0, 0.25f), 25.0));
        path.SetClosedLoop(true);
        CHECK(path.SplineSegmentCount() == 4);
        CHECK(Near(path.SplineLength(), 400.0));
        CHECK(Near(path.DistanceAlongSplineAtSplinePoint(4).Value(), 400.0));
        CHECK(Near(path.InputKeyValueAtSplinePoint(4).Value(), 4.0));
        // 유효한 override(마지막 키 3 + 2): 닫는 구간 길이는 같고 키 범위만 [0, 5]다.
        CHECK(path.SetClosedLoopAtPosition(true, 5.0f));
        CHECK(path.SplineSegmentCount() == 4);
        CHECK(Near(path.SplineLength(), 400.0));
        CHECK(path.InputKeyRange().min == 0.0f && path.InputKeyRange().max == 5.0f);
        CHECK(Near(path.InputKeyValueAtSplinePoint(4).Value(), 5.0));
        // 잘못된 루프 키(마지막 키 이하)는 열린 스플라인이다(UE SetLoopKey).
        CHECK(path.SetClosedLoopAtPosition(true, 2.0f));
        CHECK(path.SplineSegmentCount() == 3);
        CHECK(Near(path.SplineLength(), 300.0));
        // Constant: 구간 끝에서만 길이가 생긴다.
        path.SetClosedLoop(false, false);
        CHECK(path.SetSplinePointType(0, SplinePointType::Constant));
        CHECK(path.SegmentLength(0, 0.5f) == 0.0);
        CHECK(Near(path.SegmentLength(0, 1.0f), 100.0));
        CHECK(Near(path.SplineLength(), 300.0));
        CHECK(path.SegmentLength(9, 0.5f) == 0.0 && path.SegmentLength(0, 1.5f) == 0.0);
    }));

    // 곡선 구간: UE 식(Legendre–Gauss 5점)을 시험 안에서 따로 계산한 값과 같고, 독립 Simpson
    // 적분(2000구간)과 1% 안이다.
    CHECK(Run(fixture, spline, [](SplineComponent& path) noexcept {
        const Vec3d arc[] = {Vec3d(0.0, 0.0, 0.0), Vec3d(100.0, 100.0, 0.0),
                             Vec3d(200.0, 0.0, 50.0)};
        CHECK(path.SetSplinePoints(Span<const Vec3d>(arc, 3), Local));
        const Vec3d t0 = path.LeaveTangentAtSplinePoint(0, Local).Value();
        const Vec3d t1 = path.ArriveTangentAtSplinePoint(1, Local).Value();
        const double ueLength = LegendreGaussHermiteLength(arc[0], t0, arc[1], t1, 1.0f);
        CHECK(Near(path.SegmentLength(0, 1.0f), ueLength, 1.0e-3));
        CHECK(Near(path.DistanceAlongSplineAtSplinePoint(1).Value(), ueLength, 1.0e-3));
        CHECK(Near(path.SegmentLength(0, 0.5f),
                   LegendreGaussHermiteLength(arc[0], t0, arc[1], t1, 0.5f), 1.0e-3));
        const double simpson = SimpsonHermiteLength(arc[0], t0, arc[1], t1, 2000);
        CHECK(fabs(ueLength - simpson) <= simpson * 0.01);
    }));

    // 월드 scale: x 방향 2배면 직선 길이 2배, scale을 바꾸면 표를 다시 만든다.
    CHECK(Run(fixture, spline, [](SplineComponent& path) noexcept {
        const Vec3d line[] = {Vec3d(0.0, 0.0, 0.0), Vec3d(100.0, 0.0, 0.0)};
        CHECK(path.SetSplinePoints(Span<const Vec3d>(line, 2), Local));
        CHECK(Near(path.SplineLength(), 100.0));
        CHECK(path.SetRelativeScale(Vec3d(2.0, 1.0, 1.0)));
        CHECK(Near(path.SplineLength(), 200.0));
        CHECK(path.SetRelativeScale(Vec3d(1.0, 3.0, 1.0)));
        CHECK(Near(path.SplineLength(), 100.0));
        CHECK(path.SetRelativeScale(Vec3d::One()));
    }));
    REQUIRE(fixture.world->Shutdown());
}

// 25.5절: 거리↔키, 시간(일정 속도 포함), duration 0.
TEST(Spline_DistanceAndTime) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    REQUIRE(fixture.world->Initialize());
    const ObjectHandle spline = fixture.AddSpline();
    REQUIRE(!spline.IsNull());
    CHECK(Run(fixture, spline, [](SplineComponent& path) noexcept {
        // 기본 직선(길이 100, 일정 속도).
        CHECK(Near(path.InputKeyValueAtDistanceAlongSpline(50.0), 0.5));
        CHECK(Near(path.DistanceAlongSplineAtSplineInputKey(0.5f), 50.0));
        CHECK(Near(path.DistanceAlongSplineAtSplineInputKey(1.0f), 100.0));
        CHECK(path.DistanceAlongSplineAtSplineInputKey(-1.0f) == 0.0);
        CHECK(NearVector(path.LocationAtDistanceAlongSpline(25.0, Local), 25.0, 0.0, 0.0));
        // 범위 밖 거리는 끝으로 자른다.
        CHECK(Near(path.InputKeyValueAtDistanceAlongSpline(-10.0), 0.0));
        CHECK(Near(path.InputKeyValueAtDistanceAlongSpline(500.0), 1.0));
        // 시간: duration 2 → t = 1은 키 0.5.
        CHECK(path.SetDuration(2.0f));
        CHECK(Near(path.InputKeyValueAtTime(1.0f), 0.5));
        CHECK(Near(path.InputKeyValueAtTime(5.0f), 1.0));
        CHECK(NearVector(path.LocationAtTime(1.0f, Local), 50.0, 0.0, 0.0));
        CHECK(NearVector(path.LocationAtTime(0.5f, Local, true), 25.0, 0.0, 0.0));
        CHECK(Near(path.TimeAtDistanceAlongSpline(50.0), 1.0));
        CHECK(path.SetDuration(0.0f));
        CHECK(NearVector(path.LocationAtTime(1.0f, Local), 0.0, 0.0, 0.0));
        CHECK(NearVector(path.ScaleAtTime(1.0f), 1.0, 1.0, 1.0));
        CHECK(path.QuaternionAtTime(1.0f, Local) == Quatd::Identity());
        CHECK(Near(path.InputKeyValueAtTime(1.0f), 0.0));
        // Constant 첫 구간: 표 앞 거리 0이 10개 이어진다 — UE Eval은 마지막(키 0.9)을 고른다.
        const Vec3d steps[] = {Vec3d(0.0, 0.0, 0.0), Vec3d(100.0, 0.0, 0.0),
                               Vec3d(200.0, 0.0, 0.0)};
        CHECK(path.SetSplinePoints(Span<const Vec3d>(steps, 3), Local, false));
        CHECK(path.SetSplinePointType(0, SplinePointType::Constant));
        CHECK(Near(path.InputKeyValueAtDistanceAlongSpline(0.0), 0.9));
        CHECK(Near(path.InputKeyValueAtDistanceAlongSpline(50.0), 0.95));
        CHECK(Near(path.InputKeyValueAtDistanceAlongSpline(-1.0), 0.0));
        // 곡선: 거리 → 키 → 거리 왕복이 0.5 cm 안이다(표 선형 근사).
        const Vec3d arc[] = {Vec3d(0.0, 0.0, 0.0), Vec3d(100.0, 100.0, 0.0),
                             Vec3d(300.0, 0.0, 0.0)};
        CHECK(path.SetSplinePoints(Span<const Vec3d>(arc, 3), Local));
        const double length = path.SplineLength();
        for (int step = 1; step < 10; ++step) {
            const double distance = length * step / 10.0;
            const float key = path.InputKeyValueAtDistanceAlongSpline(distance);
            CHECK(Near(path.DistanceAlongSplineAtSplineInputKey(key), distance, 0.5));
        }
    }));
    REQUIRE(fixture.world->Shutdown());
}

// 25.5절: 방향·회전(MakeFromXZ — up 평행 포함), up·right·roll·scale·transform, World 공간.
TEST(Spline_OrientationAndWorldSpace) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    REQUIRE(fixture.world->Initialize());
    const ObjectHandle spline = fixture.AddSpline();
    REQUIRE(!spline.IsNull());
    CHECK(Run(fixture, spline, [](SplineComponent& path) noexcept {
        // +X 직선: 방향 X, 회전 단위, up Z, right Y.
        CHECK(NearVector(path.DirectionAtSplineInputKey(0.5f, Local), 1.0, 0.0, 0.0));
        CHECK(NearVector(path.UpVectorAtSplineInputKey(0.5f, Local), 0.0, 0.0, 1.0));
        CHECK(NearVector(path.RightVectorAtSplineInputKey(0.5f, Local), 0.0, 1.0, 0.0));
        CHECK(Near(path.RollAtSplineInputKey(0.5f, Local), 0.0));
        // 수직 직선(방향 ∥ up): N이 (1, 0, 0)으로 바뀐다 → up (1, 0, 0), right (0, −1, 0).
        const Vec3d vertical[] = {Vec3d(0.0, 0.0, 0.0), Vec3d(0.0, 0.0, 100.0)};
        CHECK(path.SetSplinePoints(Span<const Vec3d>(vertical, 2), Local));
        CHECK(NearVector(path.DirectionAtSplineInputKey(0.5f, Local), 0.0, 0.0, 1.0));
        CHECK(NearVector(path.UpVectorAtSplineInputKey(0.5f, Local), 1.0, 0.0, 0.0));
        CHECK(NearVector(path.RightVectorAtSplineInputKey(0.5f, Local), 0.0, -1.0, 0.0));
        // scale 곡선·transform.
        const Vec3d line[] = {Vec3d(0.0, 0.0, 0.0), Vec3d(100.0, 0.0, 0.0)};
        CHECK(path.SetSplinePoints(Span<const Vec3d>(line, 2), Local));
        CHECK(path.SetScaleAtSplinePoint(1, Vec3d(2.0, 2.0, 2.0)));
        CHECK(NearVector(path.ScaleAtSplineInputKey(1.0f), 2.0, 2.0, 2.0));
        const Transform scaled = path.TransformAtSplineInputKey(1.0f, Local, true);
        CHECK(NearVector(scaled.scale, 2.0, 2.0, 2.0));
        CHECK(NearVector(path.TransformAtSplineInputKey(1.0f, Local).scale, 1.0, 1.0, 1.0));
        // World: 컴포넌트 yaw 90°·위치 (0, 0, 50)·scale (2, 1, 1).
        CHECK(path.SetRelativeLocation(Vec3d(0.0, 0.0, 50.0)));
        CHECK(path.SetRelativeRotation(Rotator<double>(0.0, 90.0, 0.0).ToQuaternion()));
        CHECK(path.SetRelativeScale(Vec3d(2.0, 1.0, 1.0)));
        CHECK(NearVector(path.LocationAtSplineInputKey(1.0f, WorldSpace), 0.0, 200.0, 50.0));
        CHECK(NearVector(path.TangentAtSplineInputKey(0.5f, WorldSpace), 0.0, 200.0, 0.0));
        CHECK(NearVector(path.DirectionAtSplineInputKey(0.5f, WorldSpace), 0.0, 1.0, 0.0));
        CHECK(NearVector(path.UpVectorAtSplineInputKey(0.5f, WorldSpace), 0.0, 0.0, 1.0));
        CHECK(NearVector(path.RightVectorAtSplineInputKey(0.5f, WorldSpace), -1.0, 0.0, 0.0));
        CHECK(Near(path.RotationAtSplineInputKey(0.5f, WorldSpace).yaw, 90.0));
        CHECK(NearVector(path.TransformAtSplineInputKey(0.5f, WorldSpace).translation, 0.0, 100.0,
                         50.0));
        // World 입력: 점 위치를 월드로 주면 Local로 바꿔 넣는다.
        // (아래에서 점 1을 다시 옮긴다.)
        CHECK(path.SetLocationAtSplinePoint(1, Vec3d(0.0, 100.0, 50.0), WorldSpace));
        CHECK(NearVector(PointAt(path, 1), 50.0, 0.0, 0.0));
        CHECK(Near(path.SplineLength(), 100.0));
        // 역변환할 수 없는 변환(0 scale)은 World 입력을 거부한다.
        CHECK(path.SetRelativeScale(Vec3d(0.0, 1.0, 1.0)));
        CHECK(ErrorOf(path.AddSplinePoint(Vec3d(1.0, 1.0, 1.0), WorldSpace)) ==
              ErrorCode::InvalidArgument);
        CHECK(path.SplinePointCount() == 2);
        // 비균일 scale × 회전한 접선(행렬로는 shear): transform은 UE처럼 성분 합성이다.
        CHECK(path.SetRelativeRotation(Quatd::Identity()));
        CHECK(path.SetRelativeLocation(Vec3d()));
        CHECK(path.SetRelativeScale(Vec3d(2.0, 1.0, 1.0)));
        const Vec3d diagonal[] = {Vec3d(0.0, 0.0, 0.0), Vec3d(100.0, 100.0, 0.0)};
        CHECK(path.SetSplinePoints(Span<const Vec3d>(diagonal, 2), Local));
        const Transform skew = path.TransformAtSplineInputKey(0.5f, WorldSpace, true);
        CHECK(NearVector(skew.scale, 2.0, 1.0, 1.0));
        CHECK(NearVector(skew.translation, 100.0, 50.0, 0.0));
        auto skewRotator = Rotator<double>::TryFromQuaternion(skew.rotation);
        CHECK(skewRotator && Near(skewRotator.Value().yaw, 45.0));
        // 반사(scale y −1): UE 회전은 TRS 사슬이라 단위다(행렬 분해는 yaw 180°가 된다).
        CHECK(path.SetSplinePoints(Span<const Vec3d>(line, 2), Local));
        CHECK(path.SetRelativeScale(Vec3d(1.0, -1.0, 1.0)));
        CHECK(Near(path.RotationAtSplineInputKey(0.5f, WorldSpace).yaw, 0.0));
        CHECK(NearVector(path.RightVectorAtSplineInputKey(0.5f, WorldSpace), 0.0, 1.0, 0.0));
        CHECK(path.SetRelativeScale(Vec3d::One()));
    }));
    REQUIRE(fixture.world->Shutdown());
}

// 25.5절: 최근접 키·위치·거리.
TEST(Spline_ClosestPoint) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    REQUIRE(fixture.world->Initialize());
    const ObjectHandle spline = fixture.AddSpline();
    REQUIRE(!spline.IsNull());
    CHECK(Run(fixture, spline, [](SplineComponent& path) noexcept {
        CHECK(path.SetRelativeLocation(Vec3d(0.0, 0.0, 10.0)));
        const Vec3d target(30.0, 25.0, 10.0);
        CHECK(Near(path.FindInputKeyClosestToWorldLocation(target), 0.3));
        CHECK(NearVector(path.FindLocationClosestToWorldLocation(target, WorldSpace), 30.0, 0.0,
                         10.0));
        CHECK(NearVector(path.FindLocationClosestToWorldLocation(target, Local), 30.0, 0.0, 0.0));
        CHECK(Near(path.DistanceAlongSplineAtLocation(target, WorldSpace), 30.0));
        CHECK(Near(path.DistanceAlongSplineAtLocation(Vec3d(30.0, 25.0, 0.0), Local), 30.0));
        // 끝 밖은 끝점이다.
        CHECK(Near(path.FindInputKeyClosestToWorldLocation(Vec3d(500.0, 0.0, 10.0)), 1.0));
        path.ClearSplinePoints();
        CHECK(path.FindInputKeyClosestToWorldLocation(target) == 0.0f);
    }));
    REQUIRE(fixture.world->Shutdown());
}

// 25.6절: up·회전 설정, 접선 설정과 CurveCustomTangent, 점 type.
TEST(Spline_RotationAndTangentSetters) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    REQUIRE(fixture.world->Initialize());
    const ObjectHandle spline = fixture.AddSpline();
    REQUIRE(!spline.IsNull());
    CHECK(Run(fixture, spline, [](SplineComponent& path) noexcept {
        // up을 +Y로: 방향 X, up Y → MakeFromXZ의 up은 (0, 1, 0).
        CHECK(path.SetUpVectorAtSplinePoint(0, Vec3d(0.0, 1.0, 0.0), Local));
        CHECK(NearVector(path.UpVectorAtSplineInputKey(0.0f, Local), 0.0, 1.0, 0.0));
        CHECK(ErrorOf(path.SetUpVectorAtSplinePoint(0, Vec3d(), Local)) ==
              ErrorCode::InvalidArgument);
        CHECK(path.SetUpVectorAtSplinePoint(0, Vec3d(0.0, 0.0, 1.0), Local));
        // 점 1 회전을 yaw 90°로: 접선이 +Y로 돌고 CurveCustomTangent가 된다.
        CHECK(path.SetQuaternionAtSplinePoint(1, Rotator<double>(0.0, 90.0, 0.0).ToQuaternion(),
                                              Local));
        CHECK(NearVector(path.LeaveTangentAtSplinePoint(1, Local).Value(), 0.0, 100.0, 0.0));
        CHECK(path.SplinePointTypeAt(1).Value() == SplinePointType::CurveCustomTangent);
        CHECK(NearVector(path.DirectionAtSplineInputKey(1.0f, Local), 0.0, 1.0, 0.0));
        CHECK(NearVector(path.UpVectorAtSplineInputKey(1.0f, Local), 0.0, 0.0, 1.0));
        // 접선 설정.
        CHECK(path.SetTangentAtSplinePoint(0, Vec3d(0.0, 50.0, 0.0), Local));
        CHECK(path.SplinePointTypeAt(0).Value() == SplinePointType::CurveCustomTangent);
        CHECK(NearVector(path.ArriveTangentAtSplinePoint(0, Local).Value(), 0.0, 50.0, 0.0));
        CHECK(path.SetTangentsAtSplinePoint(0, Vec3d(1.0, 0.0, 0.0), Vec3d(2.0, 0.0, 0.0), Local));
        CHECK(NearVector(path.LeaveTangentAtSplinePoint(0, Local).Value(), 2.0, 0.0, 0.0));
        // 사용자 접선은 UpdateSpline이 바꾸지 않는다.
        path.UpdateSpline();
        CHECK(NearVector(path.LeaveTangentAtSplinePoint(0, Local).Value(), 2.0, 0.0, 0.0));
        CHECK(ErrorOf(path.SetSplinePointType(0, static_cast<SplinePointType>(9))) ==
              ErrorCode::InvalidArgument);
        // Curve로 되돌리면 자동 접선 (P1 − P0)/1 = (100, 0, 0)이다(끝점은 자기 자신이 이웃).
        CHECK(path.SetSplinePointType(0, SplinePointType::Curve));
        CHECK(NearVector(path.LeaveTangentAtSplinePoint(0, Local).Value(), 100.0, 0.0, 0.0));
        CHECK(ErrorOf(path.SetScaleAtSplinePoint(5, Vec3d::One())) == ErrorCode::InvalidArgument);
        // World 회전: 컴포넌트 yaw 90°에서 월드 yaw 180°는 Local yaw 90° → 접선이 Local +Y로.
        CHECK(path.SetSplinePointType(1, SplinePointType::Curve));
        CHECK(path.SetRelativeRotation(Rotator<double>(0.0, 90.0, 0.0).ToQuaternion()));
        CHECK(path.SetRotationAtSplinePoint(1, Rotator<double>(0.0, 180.0, 0.0), WorldSpace));
        CHECK(NearVector(path.LeaveTangentAtSplinePoint(1, Local).Value(), 0.0, 100.0, 0.0));
        CHECK(path.SetRelativeRotation(Quatd::Identity()));
        // 반대 방향 up(UE 5.8 FindBetween_Helper): defaultUp (1, 0, 0)·up (−1, 0, 0)은 |x|가 가장
        // 커서 기저 +Y → 축 X × Y = Z(Z축 180°). 옛 식(UE 4)은 Y축이다.
        CHECK(path.SetDefaultUpVector(Vec3d(1.0, 0.0, 0.0), Local));
        CHECK(path.SetUpVectorAtSplinePoint(0, Vec3d(-1.0, 0.0, 0.0), Local));
        const Quatd flipped = path.RotationCurve().GetPoint(0).outValue;
        CHECK(Near(fabs(flipped.z), 1.0) && Near(flipped.x, 0.0) && Near(flipped.y, 0.0));
        // up −Z(defaultUp Z): |z|가 가장 크므로 기저 −X → 축 Z × (−X) = (0, −1, 0).
        CHECK(path.SetDefaultUpVector(Vec3d(0.0, 0.0, 1.0), Local));
        CHECK(path.SetUpVectorAtSplinePoint(0, Vec3d(0.0, 0.0, -1.0), Local));
        const Quatd down = path.RotationCurve().GetPoint(0).outValue;
        CHECK(Near(fabs(down.y), 1.0) && Near(down.x, 0.0) && Near(down.z, 0.0));
    }));
    REQUIRE(fixture.world->Shutdown());
}

// 25.7절: bounds — 곡선 극값, 월드 회전에서 반지름 = |extent|.
TEST(Spline_Bounds) {
    Fixture fixture;
    REQUIRE(fixture.registered);
    REQUIRE(fixture.world->Initialize());
    const ObjectHandle spline = fixture.AddSpline();
    REQUIRE(!spline.IsNull());
    CHECK(Run(fixture, spline, [](SplineComponent& path) noexcept {
        ComponentBounds bounds = path.Bounds();
        CHECK(NearVector(bounds.origin, 50.0, 0.0, 0.0));
        CHECK(NearVector(bounds.boxExtent, 50.0, 0.0, 0.0));
        CHECK(Near(bounds.sphereRadius, 50.0));
        // 곡선: (0, 0)·(100, 100)·(200, 0) — y 최대는 키 1의 100, x는 [0, 200].
        const Vec3d arc[] = {Vec3d(0.0, 0.0, 0.0), Vec3d(100.0, 100.0, 0.0),
                             Vec3d(200.0, 0.0, 0.0)};
        CHECK(path.SetSplinePoints(Span<const Vec3d>(arc, 3), Local));
        bounds = path.Bounds();
        CHECK(NearVector(bounds.origin, 100.0, 50.0, 0.0));
        CHECK(NearVector(bounds.boxExtent, 100.0, 50.0, 0.0));
        // yaw 45°: extent = (150√½, 150√½, 0), 반지름 = |extent| = 150(Shape 규칙이면 min(√(100² +
        // 50²), 150) ≈ 111.8).
        CHECK(path.SetRelativeRotation(Rotator<double>(0.0, 45.0, 0.0).ToQuaternion()));
        bounds = path.Bounds();
        const double half = sqrt(0.5);
        CHECK(
            NearVector(bounds.origin, 100.0 * half - 50.0 * half, 100.0 * half + 50.0 * half, 0.0));
        CHECK(NearVector(bounds.boxExtent, 150.0 * half, 150.0 * half, 0.0));
        CHECK(Near(bounds.sphereRadius, 150.0));
        // 사용자 접선이 넘치는 극값: y(t) = 300(t³ − 2t² + t)의 최대 t = 1/3에서 400/9.
        CHECK(path.SetRelativeRotation(Quatd::Identity()));
        const Vec3d pair[] = {Vec3d(0.0, 0.0, 0.0), Vec3d(100.0, 0.0, 0.0)};
        CHECK(path.SetSplinePoints(Span<const Vec3d>(pair, 2), Local));
        CHECK(path.SetTangentsAtSplinePoint(0, Vec3d(100.0, 300.0, 0.0), Vec3d(100.0, 300.0, 0.0),
                                            Local));
        CHECK(path.SetTangentsAtSplinePoint(1, Vec3d(100.0, 0.0, 0.0), Vec3d(100.0, 0.0, 0.0),
                                            Local));
        bounds = path.Bounds();
        CHECK(NearVector(bounds.origin, 50.0, 200.0 / 9.0, 0.0));
        CHECK(NearVector(bounds.boxExtent, 50.0, 200.0 / 9.0, 0.0));
        // 점이 없으면 원점의 점이다.
        path.ClearSplinePoints();
        CHECK(Near(path.Bounds().sphereRadius, 0.0));
    }));
    REQUIRE(fixture.world->Shutdown());
}

// 25.9절 9: 리플렉션 형태와 저장 왕복(곡선·설정), 로드 뒤 첫 질의가 거리 표를 만든다.
TEST(Spline_ReflectionAndSaveRoundTrip) {
    Fixture fixture(WorldKind::Editor);
    REQUIRE(fixture.registered);
    {
        auto lease = fixture.types.Acquire(ObjectTypeIdOf<SplineComponent>());
        REQUIRE(lease);
        const TypeSchema& schema = lease.Value().Schema();
        CHECK(HasFlag(schema.flags, TypeFlags::WorldOnly));
        CHECK(!HasFlag(schema.flags, TypeFlags::Abstract));
        CHECK(schema.baseType == ObjectTypeIdOf<PrimitiveComponent>());
        // PrimitiveComponent 52(bodySettings 포함 — CD2) + 스플라인 10.
        CHECK(schema.fields.Size() == 62);
    }

    World& world = *fixture.world;
    REQUIRE(world.Initialize());
    auto level = world.CreateLevel("Paths");
    REQUIRE(level);
    const ObjectHandle spline = fixture.AddSpline(level.Value());
    REQUIRE(!spline.IsNull());
    double length = 0.0;
    CHECK(Run(fixture, spline, [&](SplineComponent& path) noexcept {
        CHECK(path.AddSplinePoint(Vec3d(100.0, 100.0, 0.0), Local));
        CHECK(path.SetReparamStepsPerSegment(20));
        CHECK(path.SetDuration(3.0f));
        CHECK(path.SetDefaultUpVector(Vec3d(0.0, 1.0, 0.0), Local));
        path.SetClosedLoop(true);
        length = path.SplineLength();
    }));
    CHECK(length > 300.0);

    Array<uint8> saved(fixture.allocator);
    {
        auto bytes =
            SaveObjectGraph(fixture.allocator, world.Objects(), world.Access(), level.Value());
        REQUIRE(bytes);
        saved = Move(bytes.Value());
    }

    {
        ObjectRegistry loaded(fixture.allocator, fixture.types, {0x5041544848ull, 2});
        const auto access = loaded.OwnerAccess();
        auto root = LoadObjectGraph(fixture.allocator, loaded, fixture.types, access, saved.View());
        REQUIRE(root);
        ObjectHandle loadedActor;
        {
            auto borrowed = loaded.Resolve<Level>(access, root.Value().Handle());
            REQUIRE(borrowed);
            Level* pointer = Get(borrowed.Value());
            REQUIRE(pointer != nullptr && pointer->Actors().Size() == 1);
            loadedActor = pointer->Actors()[0];
        }

        ObjectHandle loadedSpline;
        {
            auto borrowed = loaded.Resolve<Actor>(access, loadedActor);
            REQUIRE(borrowed);
            Actor* pointer = Get(borrowed.Value());
            REQUIRE(pointer != nullptr);
            loadedSpline = pointer->RootComponent();
        }

        auto borrowed = loaded.Resolve<SplineComponent>(access, loadedSpline);
        REQUIRE(borrowed);
        SplineComponent* path = Get(borrowed.Value());
        REQUIRE(path != nullptr);
        CHECK(path->SplinePointCount() == 3);
        CHECK(NearVector(PointAt(*path, 2), 100.0, 100.0, 0.0));
        CHECK(path->ReparamStepsPerSegment() == 20 && path->Duration() == 3.0f);
        CHECK(path->IsClosedLoop() && path->PositionCurve().IsLooped());
        CHECK(NearVector(path->DefaultUpVector(Local), 0.0, 1.0, 0.0));
        // 로드 뒤 첫 질의가 표를 만든다(생성자 기본 두 점의 표가 아니다).
        CHECK(Near(path->SplineLength(), length));
    }

    REQUIRE(world.Shutdown());
}

} // namespace world_spline_component_tests_file
