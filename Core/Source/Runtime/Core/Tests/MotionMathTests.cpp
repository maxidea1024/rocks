#include <Core/Math/InterpTo.h>
#include <Core/Math/Rotator.h>
#include <Core/Math/VectorUtility.h>
#include <TestSupport/Test.h>
#include <math.h>
#include <stdio.h>

// 이동·카메라 수학 시험이다(InterpTo.h·VectorUtility.h — Docs/Design/EngineRuntime 26.2절): UE
// VInterpTo·QInterpTo·GetClampedToMaxSize·평면 투영·ToOrientationRotator/Quat 식 대조.
using namespace rocks;
namespace motion_math_tests_file {

namespace {

constexpr double Tolerance = 1.0e-9;

bool NearVector(const Vec3d& value, double x, double y, double z) noexcept {
    const bool same = fabs(value.x - x) <= Tolerance && fabs(value.y - y) <= Tolerance &&
                      fabs(value.z - z) <= Tolerance;
    if (!same) {
        printf("  vector (%.12g, %.12g, %.12g), expected (%g, %g, %g)\n", value.x, value.y, value.z,
               x, y, z);
    }

    return same;
}

double YawOf(const Quatd& rotation) noexcept {
    auto rotator = Rotator<double>::TryFromQuaternion(rotation);
    return rotator ? rotator.Value().yaw : -1.0e9;
}

} // namespace

// UE VInterpTo(UnrealMath.cpp:2584): 비율 clamp(dt·speed, 0, 1), speed ≤ 0·도착 거리는 target.
TEST(MotionMath_VInterpTo) {
    const Vec3d origin;
    const Vec3d target(100.0, 0.0, 0.0);
    CHECK(NearVector(math::VInterpTo(origin, target, 0.01, 10.0), 10.0, 0.0, 0.0));
    CHECK(NearVector(math::VInterpTo(origin, target, 1.0, 10.0), 100.0, 0.0, 0.0));
    CHECK(NearVector(math::VInterpTo(origin, target, 0.01, 0.0), 100.0, 0.0, 0.0));
    CHECK(NearVector(math::VInterpTo(origin, target, 0.01, -1.0), 100.0, 0.0, 0.0));
    // 거리² < 1e-4면 바로 target이다.
    const Vec3d close(0.005, 0.0, 0.0);
    CHECK(NearVector(math::VInterpTo(origin, close, 0.01, 10.0), 0.005, 0.0, 0.0));
}

// UE QInterpTo(:2752): Slerp(clamp(speed·dt, 0, 1)), 같은 회전(q·−q 포함)은 target.
TEST(MotionMath_QInterpTo) {
    const Quatd identity = Quatd::Identity();
    const Quatd yaw90 = Rotator<double>(0.0, 90.0, 0.0).ToQuaternion();
    CHECK(fabs(YawOf(math::QInterpTo(identity, yaw90, 0.01, 10.0)) - 9.0) <= 1.0e-9);
    CHECK(math::QInterpTo(identity, yaw90, 0.01, 0.0) == yaw90);
    CHECK(math::QInterpTo(identity, yaw90, 1.0, 10.0) == yaw90);
    const Quatd negated(-identity.x, -identity.y, -identity.z, -identity.w);
    CHECK(math::QInterpTo(identity, negated, 0.01, 10.0) == negated);
}

// UE GetClampedToMaxSize(Vector.h:1904): 길면 줄이고 max < 1e-4면 0.
TEST(MotionMath_ClampedToMaxSize) {
    CHECK(NearVector(math::ClampedToMaxSize(Vec3d(3.0, 4.0, 0.0), 2.5), 1.5, 2.0, 0.0));
    CHECK(NearVector(math::ClampedToMaxSize(Vec3d(3.0, 4.0, 0.0), 10.0), 3.0, 4.0, 0.0));
    CHECK(NearVector(math::ClampedToMaxSize(Vec3d(3.0, 4.0, 0.0), 5.0e-5), 0.0, 0.0, 0.0));
}

// UE VectorPlaneProject·PointPlaneProject(Vector.h:1408·1416).
TEST(MotionMath_PlaneProject) {
    const Vec3d up(0.0, 0.0, 1.0);
    CHECK(NearVector(math::VectorPlaneProject(Vec3d(1.0, 2.0, 3.0), up), 1.0, 2.0, 0.0));
    CHECK(NearVector(math::PointPlaneProject(Vec3d(1.0, 2.0, 3.0), Vec3d(0.0, 0.0, 5.0), up), 1.0,
                     2.0, 5.0));
    // 0 법선은 그대로다.
    CHECK(NearVector(math::VectorPlaneProject(Vec3d(1.0, 2.0, 3.0), Vec3d()), 1.0, 2.0, 3.0));
}

// UE ToOrientationRotator(:200)·ToOrientationQuat(:285): 같은 회전이다.
TEST(MotionMath_ToOrientation) {
    const Rotator<double> flat = math::ToOrientationRotator(Vec3d(1.0, 1.0, 0.0));
    CHECK(fabs(flat.yaw - 45.0) <= Tolerance && fabs(flat.pitch) <= Tolerance && flat.roll == 0.0);
    const Rotator<double> up = math::ToOrientationRotator(Vec3d(1.0, 0.0, 1.0));
    CHECK(fabs(up.pitch - 45.0) <= Tolerance && fabs(up.yaw) <= Tolerance);
    const Rotator<double> zero = math::ToOrientationRotator(Vec3d());
    CHECK(zero.pitch == 0.0 && zero.yaw == 0.0 && zero.roll == 0.0);
    // quaternion은 Rotator 변환과 같은 회전이고, 앞쪽 축이 방향이다.
    const Vec3d direction(1.0, 2.0, -3.0);
    const Quatd fromDirection = math::ToOrientationQuat(direction);
    const Quatd fromRotator = math::ToOrientationRotator(direction).ToQuaternion();
    CHECK(AngularDistance(fromDirection, fromRotator) <= 1.0e-9);
    auto unit = direction.TryNormalize();
    REQUIRE(unit);
    const Vec3d forward = fromDirection.GetForwardVector();
    CHECK(NearVector(forward, unit.Value().x, unit.Value().y, unit.Value().z));
}

} // namespace motion_math_tests_file
