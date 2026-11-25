#include <Core/Math/ConvexHull.h>
#include <Core/Math/Rotator.h>
#include <Core/Math/VolumeShape.h>
#include <TestSupport/Test.h>
#include <math.h>
#include <stdio.h>

// 영역 형상 거리 시험이다(VolumeShape.h·ConvexHull.h — Docs/Design/EngineRuntime 28.2절):
// 상자·구·캡슐의 안·경계·밖(회전 포함), 캡슐 반 높이 보정, 볼록 껍질(감기 방향·퇴화 삼각형·쓰이지
// 않는 꼭짓점·평평함), 오류.
using namespace rocks;
namespace volume_shape_tests_file {

namespace {

constexpr double VolumeTolerance = 1.0e-9;

bool NearVolume(const Result<double>& value, double expected) noexcept {
    if (!value) {
        printf("  failed, expected %.12g\n", expected);
        return false;
    }

    const bool same = fabs(value.Value() - expected) <= VolumeTolerance;
    if (!same) {
        printf("  distance %.12g, expected %.12g\n", value.Value(), expected);
    }

    return same;
}

bool RejectsVolume(const Result<double>& value) noexcept {
    return !value && value.Error() == ErrorCode::InvalidArgument;
}

// 원점 중심 한 변 2 정육면체(꼭짓점 8, 삼각형 12 — 일부는 일부러 감기 방향을 뒤집었다).
constexpr Vec3d CubeVertices[] = {
    Vec3d(-1, -1, -1), Vec3d(1, -1, -1), Vec3d(1, 1, -1), Vec3d(-1, 1, -1),
    Vec3d(-1, -1, 1),  Vec3d(1, -1, 1),  Vec3d(1, 1, 1),  Vec3d(-1, 1, 1),
};
constexpr uint32 CubeIndices[] = {
    0, 1, 2, 0, 2, 3, // 아래(−Z)
    4, 6, 5, 4, 7, 6, // 위(+Z) — 반대 감기
    0, 4, 5, 0, 5, 1, // −Y
    1, 5, 6, 1, 6, 2, // +X
    2, 6, 7, 2, 7, 3, // +Y
    3, 7, 4, 3, 4, 0, // −X
};

} // namespace

// 28.2절: 상자(회전)·구·캡슐 — 안·경계는 0, 밖은 표면까지 거리.
TEST(VolumeShape_PrimitiveDistances) {
    auto box = VolumeShape::TryBox(Vec3d(100, 0, 0), Quatd::Identity(), Vec3d(10, 20, 30));
    REQUIRE(box);
    CHECK(NearVolume(TryDistanceToVolume(box.Value(), Vec3d(100, 0, 0)), 0.0));
    CHECK(NearVolume(TryDistanceToVolume(box.Value(), Vec3d(110, 20, 30)), 0.0));
    CHECK(NearVolume(TryDistanceToVolume(box.Value(), Vec3d(115, 0, 0)), 5.0));
    // 모서리 밖: (3, 4) 대각.
    CHECK(NearVolume(TryDistanceToVolume(box.Value(), Vec3d(113, 24, 0)), 5.0));

    // yaw 90°: 지역 X(10)가 월드 Y다.
    const Quatd yaw = Rotator<double>(0.0, 90.0, 0.0).ToQuaternion();
    auto turned = VolumeShape::TryBox(Vec3d(0, 0, 0), yaw, Vec3d(10, 20, 30));
    REQUIRE(turned);
    CHECK(NearVolume(TryDistanceToVolume(turned.Value(), Vec3d(0, 12, 0)), 2.0));
    CHECK(NearVolume(TryDistanceToVolume(turned.Value(), Vec3d(19, 0, 0)), 0.0));
    CHECK(NearVolume(TryDistanceToVolume(turned.Value(), Vec3d(25, 0, 0)), 5.0));

    auto sphere = VolumeShape::TrySphere(Vec3d(0, 0, 50), 10.0);
    REQUIRE(sphere);
    CHECK(NearVolume(TryDistanceToVolume(sphere.Value(), Vec3d(0, 0, 55)), 0.0));
    CHECK(NearVolume(TryDistanceToVolume(sphere.Value(), Vec3d(0, 10, 50)), 0.0));
    CHECK(NearVolume(TryDistanceToVolume(sphere.Value(), Vec3d(30, 0, 50)), 20.0));

    // 캡슐: 반지름 10, 반 높이 50 — 축 선분 z ∈ [−40, 40].
    auto capsule = VolumeShape::TryCapsule(Vec3d(0, 0, 0), Quatd::Identity(), 10.0, 50.0);
    REQUIRE(capsule);
    CHECK(capsule.Value().halfHeight == 50.0);
    CHECK(NearVolume(TryDistanceToVolume(capsule.Value(), Vec3d(0, 0, 50)), 0.0));
    CHECK(NearVolume(TryDistanceToVolume(capsule.Value(), Vec3d(0, 0, 60)), 10.0));
    CHECK(NearVolume(TryDistanceToVolume(capsule.Value(), Vec3d(25, 0, 0)), 15.0));
    // pitch 90°: 축이 월드 X다.
    const Quatd pitch = Rotator<double>(90.0, 0.0, 0.0).ToQuaternion();
    auto lying = VolumeShape::TryCapsule(Vec3d(0, 0, 0), pitch, 10.0, 50.0);
    REQUIRE(lying);
    CHECK(NearVolume(TryDistanceToVolume(lying.Value(), Vec3d(55, 0, 0)), 5.0));
    CHECK(NearVolume(TryDistanceToVolume(lying.Value(), Vec3d(0, 0, 25)), 15.0));
    // 반 높이 < 반지름이면 반지름으로 올린다(구와 같다).
    auto round = VolumeShape::TryCapsule(Vec3d(0, 0, 0), Quatd::Identity(), 10.0, 4.0);
    REQUIRE(round);
    CHECK(round.Value().halfHeight == 10.0);
    CHECK(NearVolume(TryDistanceToVolume(round.Value(), Vec3d(0, 0, 13)), 3.0));

    // 0 크기도 형상이다(점).
    auto point = VolumeShape::TrySphere(Vec3d(1, 2, 3), 0.0);
    REQUIRE(point);
    CHECK(NearVolume(TryDistanceToVolume(point.Value(), Vec3d(1, 2, 3)), 0.0));
}

// 28.2절: 잘못된 입력은 InvalidArgument다.
TEST(VolumeShape_RejectsInvalidInput) {
    const double notANumber = nan("");
    CHECK(!VolumeShape::TryBox(Vec3d(0, 0, 0), Quatd::Identity(), Vec3d(-1, 1, 1)));
    CHECK(!VolumeShape::TryBox(Vec3d(0, 0, 0), Quatd(0, 0, 0, 0), Vec3d(1, 1, 1)));
    CHECK(!VolumeShape::TryBox(Vec3d(notANumber, 0, 0), Quatd::Identity(), Vec3d(1, 1, 1)));
    CHECK(!VolumeShape::TrySphere(Vec3d(0, 0, 0), -1.0));
    CHECK(!VolumeShape::TrySphere(Vec3d(0, 0, 0), HUGE_VAL));
    CHECK(!VolumeShape::TryCapsule(Vec3d(0, 0, 0), Quatd::Identity(), 1.0, notANumber));
    // 정규화하지 않은 회전은 정규화한다.
    auto scaled = VolumeShape::TryBox(Vec3d(0, 0, 0), Quatd(0, 0, 0, 2), Vec3d(1, 1, 1));
    REQUIRE(scaled);
    CHECK(scaled.Value().rotation == Quatd::Identity());

    auto sphere = VolumeShape::TrySphere(Vec3d(0, 0, 0), 1.0);
    REQUIRE(sphere);
    CHECK(RejectsVolume(TryDistanceToVolume(sphere.Value(), Vec3d(0, notANumber, 0))));
    // Try* 밖에서 망가뜨린 값도 거부한다.
    VolumeShape broken = sphere.Value();
    broken.radius = -1.0;
    CHECK(RejectsVolume(TryDistanceToVolume(broken, Vec3d(0, 0, 0))));
    VolumeShape capsule = sphere.Value();
    capsule.kind = VolumeShapeKind::Capsule;
    capsule.halfHeight = 0.5;
    CHECK(RejectsVolume(TryDistanceToVolume(capsule, Vec3d(0, 0, 0))));
    VolumeShape rotated = sphere.Value();
    rotated.rotation = Quatd(0, 0, 0, 3);
    CHECK(RejectsVolume(TryDistanceToVolume(rotated, Vec3d(0, 0, 0))));
}

// 28.2절: 볼록 껍질 — 안은 0, 밖은 삼각형 최솟값, 감기 방향과 퇴화 삼각형에 흔들리지 않는다.
TEST(VolumeShape_ConvexHullDistance) {
    const ConvexHullView cube{Span<const Vec3d>(CubeVertices, 8),
                              Span<const uint32>(CubeIndices, 36)};
    CHECK(NearVolume(TryDistanceToConvexHull(cube, Vec3d(0, 0, 0)), 0.0));
    CHECK(NearVolume(TryDistanceToConvexHull(cube, Vec3d(0.5, -0.5, 0.99)), 0.0));
    CHECK(NearVolume(TryDistanceToConvexHull(cube, Vec3d(1, 1, 1)), 0.0));
    CHECK(NearVolume(TryDistanceToConvexHull(cube, Vec3d(0, 0, 3)), 2.0));
    CHECK(NearVolume(TryDistanceToConvexHull(cube, Vec3d(-4, 0, 0)), 3.0));
    // 모서리 쪽: (1 + 3, 1 + 4, 0) → 5.
    CHECK(NearVolume(TryDistanceToConvexHull(cube, Vec3d(4, 5, 0)), 5.0));
    // 꼭짓점 쪽: (2, 2, 2) → √3.
    CHECK(NearVolume(TryDistanceToConvexHull(cube, Vec3d(2, 2, 2)), sqrt(3.0)));

    // 사면체(0,0,0)·(1,0,0)·(0,1,0)·(0,0,1): 빗면 바깥 점.
    const Vec3d tetraVertices[] = {Vec3d(0, 0, 0), Vec3d(1, 0, 0), Vec3d(0, 1, 0), Vec3d(0, 0, 1)};
    const uint32 tetraIndices[] = {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3, 0, 0, 1};
    const ConvexHullView tetra{Span<const Vec3d>(tetraVertices, 4),
                               Span<const uint32>(tetraIndices, 12)};
    CHECK(NearVolume(TryDistanceToConvexHull(tetra, Vec3d(0.1, 0.1, 0.1)), 0.0));
    CHECK(NearVolume(TryDistanceToConvexHull(tetra, Vec3d(1, 1, 1)), 2.0 / sqrt(3.0)));
    CHECK(NearVolume(TryDistanceToConvexHull(tetra, Vec3d(-1, 0.2, 0.2)), 1.0));
    // 퇴화 삼각형(0, 0, 1)을 섞어도 결과가 같다.
    const ConvexHullView tetraWithDegenerate{Span<const Vec3d>(tetraVertices, 4),
                                             Span<const uint32>(tetraIndices, 15)};
    CHECK(
        NearVolume(TryDistanceToConvexHull(tetraWithDegenerate, Vec3d(1, 1, 1)), 2.0 / sqrt(3.0)));
    CHECK(NearVolume(TryDistanceToConvexHull(tetraWithDegenerate, Vec3d(0.1, 0.1, 0.1)), 0.0));

    // 오류: 꼭짓점 부족, 색인 수, 범위 밖, 비유한, 평면이 없다.
    CHECK(RejectsVolume(TryDistanceToConvexHull(
        ConvexHullView{Span<const Vec3d>(tetraVertices, 3), Span<const uint32>(tetraIndices, 3)},
        Vec3d(0, 0, 0))));
    CHECK(RejectsVolume(TryDistanceToConvexHull(
        ConvexHullView{Span<const Vec3d>(tetraVertices, 4), Span<const uint32>(tetraIndices, 4)},
        Vec3d(0, 0, 0))));
    const uint32 outOfRange[] = {0, 1, 4};
    CHECK(RejectsVolume(TryDistanceToConvexHull(
        ConvexHullView{Span<const Vec3d>(tetraVertices, 4), Span<const uint32>(outOfRange, 3)},
        Vec3d(0, 0, 0))));
    const uint32 onlyDegenerate[] = {0, 0, 1};
    CHECK(RejectsVolume(TryDistanceToConvexHull(
        ConvexHullView{Span<const Vec3d>(tetraVertices, 4), Span<const uint32>(onlyDegenerate, 3)},
        Vec3d(0, 0, 0))));
    const Vec3d badVertices[] = {Vec3d(0, 0, 0), Vec3d(1, 0, 0), Vec3d(0, HUGE_VAL, 0),
                                 Vec3d(0, 0, 1)};
    CHECK(RejectsVolume(TryDistanceToConvexHull(
        ConvexHullView{Span<const Vec3d>(badVertices, 4), Span<const uint32>(tetraIndices, 12)},
        Vec3d(0, 0, 0))));
    CHECK(RejectsVolume(TryDistanceToConvexHull(tetra, Vec3d(nan(""), 0, 0))));
}

// 28.2절: 감기 방향을 모두 뒤집어도, 쓰이지 않는 먼 꼭짓점이 있어도 같고, 평평한 껍질은 거부한다.
TEST(VolumeShape_ConvexHullOrientationAndFlatness) {
    uint32 flipped[36];
    for (usize index = 0; index < 36; index += 3) {
        flipped[index] = CubeIndices[index];
        flipped[index + 1] = CubeIndices[index + 2];
        flipped[index + 2] = CubeIndices[index + 1];
    }

    const ConvexHullView reversed{Span<const Vec3d>(CubeVertices, 8),
                                  Span<const uint32>(flipped, 36)};
    CHECK(NearVolume(TryDistanceToConvexHull(reversed, Vec3d(0.5, 0.5, 0.5)), 0.0));
    CHECK(NearVolume(TryDistanceToConvexHull(reversed, Vec3d(0, 0, 3)), 2.0));

    // 9번째 꼭짓점(먼 곳)은 삼각형이 쓰지 않는다 — 중심에 넣으면 방향이 틀어진다.
    Vec3d withUnused[9];
    for (usize index = 0; index < 8; ++index) {
        withUnused[index] = CubeVertices[index];
    }

    withUnused[8] = Vec3d(1.0e4, 1.0e4, 1.0e4);
    const ConvexHullView unused{Span<const Vec3d>(withUnused, 9),
                                Span<const uint32>(CubeIndices, 36)};
    CHECK(NearVolume(TryDistanceToConvexHull(unused, Vec3d(0, 0, 0)), 0.0));
    CHECK(NearVolume(TryDistanceToConvexHull(unused, Vec3d(0, 0, 3)), 2.0));
    CHECK(NearVolume(TryDistanceToConvexHull(unused, Vec3d(-4, 0, 0)), 3.0));

    // 평평한 껍질(네 점이 z = 0 평면)은 부피가 없어 방향을 정할 수 없다.
    const Vec3d flatVertices[] = {Vec3d(0, 0, 0), Vec3d(1, 0, 0), Vec3d(1, 1, 0), Vec3d(0, 1, 0)};
    const uint32 flatIndices[] = {0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};
    const ConvexHullView flat{Span<const Vec3d>(flatVertices, 4),
                              Span<const uint32>(flatIndices, 12)};
    CHECK(RejectsVolume(TryDistanceToConvexHull(flat, Vec3d(0.5, 0.5, 5.0))));
    CHECK(RejectsVolume(TryDistanceToConvexHull(flat, Vec3d(0.5, 0.5, -5.0))));
}

} // namespace volume_shape_tests_file
