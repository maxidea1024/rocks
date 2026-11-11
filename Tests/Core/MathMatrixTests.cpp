#include "Tests/Support/Test.h"
#include "Tests/Support/MathTestSupport.h"
#include <Core/Math/MathBatch.h>
#include <Core/Math/MatrixFactory.h>

namespace rocks::test {
namespace {

// 부분 피벗 Gauss-Jordan 소거로 만든 독립 역행렬 oracle이다(Laplace 전개 구현과 다른 방법).
bool OracleInverse(const Mat4d& matrix, Mat4d& output) {
    double a[4][8];
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            a[row][column] = matrix.At(row, column);
            a[row][column + 4] = row == column ? 1.0 : 0.0;
        }
    }
    for (int pivot = 0; pivot < 4; ++pivot) {
        int best = pivot;
        for (int row = pivot + 1; row < 4; ++row) {
            if (math::Abs(a[row][pivot]) > math::Abs(a[best][pivot])) {
                best = row;
            }
        }
        if (a[best][pivot] == 0.0) {
            return false;
        }
        for (int column = 0; column < 8; ++column) {
            const double swap = a[pivot][column];
            a[pivot][column] = a[best][column];
            a[best][column] = swap;
        }
        const double scale = a[pivot][pivot];
        for (int column = 0; column < 8; ++column) {
            a[pivot][column] /= scale;
        }
        for (int row = 0; row < 4; ++row) {
            if (row != pivot) {
                const double factor = a[row][pivot];
                for (int column = 0; column < 8; ++column) {
                    a[row][column] -= factor * a[pivot][column];
                }
            }
        }
    }
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            output.At(row, column) = a[row][column + 4];
        }
    }
    return true;
}

Mat4d RandomMatrix(MathRandom& random, double range) {
    Mat4d result;
    for (double& value : result.m) {
        value = random.Range(-range, range);
    }
    return result;
}

// 비대칭 원소로 행·열 혼동을 드러내는 행렬이다.
Mat4d Asymmetric() {
    return Mat4d::FromRows(Vec4d(2.0, 3.0, 5.0, 7.0), Vec4d(11.0, 13.0, 17.0, 19.0),
                           Vec4d(23.0, 29.0, 31.0, 37.0), Vec4d(41.0, 43.0, 47.0, 53.0));
}

} // namespace

TEST(Matrix4_StorageIsColumnMajorWithColumnVectors) {
    const Mat4d translation = Mat4d::Translation(Vec3d(1.0, 2.0, 3.0));
    CHECK(translation.m[12] == 1.0 && translation.m[13] == 2.0 && translation.m[14] == 3.0);
    CHECK(translation.At(0, 3) == 1.0);
    CHECK(translation.TransformPoint(Vec3d(10.0, 0.0, 0.0)) == Vec3d(11.0, 2.0, 3.0));
    CHECK(translation.TransformVector(Vec3d(10.0, 0.0, 0.0)) == Vec3d(10.0, 0.0, 0.0));
    const Mat4d rows = Asymmetric();
    CHECK(rows.At(0, 1) == 3.0 && rows.At(1, 0) == 11.0);
    CHECK(rows.Row(2) == Vec4d(23.0, 29.0, 31.0, 37.0));
    CHECK(rows.Column(1) == Vec4d(3.0, 13.0, 29.0, 43.0));
    CHECK(rows.Transposed().Transposed() == rows);
    CHECK(Mat4::Identity() == Mat4());
}

TEST(Matrix4_CompositionAppliesRightOperandFirst) {
    // world = parent * local: scale(2)을 먼저, 이동을 나중에 적용한다.
    const Mat4d scale = Mat4d::Scale(Vec3d(2.0, 2.0, 2.0));
    const Mat4d move = Mat4d::Translation(Vec3d(1.0, 0.0, 0.0));
    CHECK((move * scale).TransformPoint(Vec3d(1.0, 0.0, 0.0)) == Vec3d(3.0, 0.0, 0.0));
    CHECK((scale * move).TransformPoint(Vec3d(1.0, 0.0, 0.0)) == Vec3d(4.0, 0.0, 0.0));
    // 비대칭 행렬 곱의 한 원소를 손으로 계산해 고정한다.
    const Mat4d product = Asymmetric() * Asymmetric().Transposed();
    CHECK(product.At(0, 0) == 4.0 + 9.0 + 25.0 + 49.0);
    CHECK(product.At(0, 1) == 2.0 * 11.0 + 3.0 * 13.0 + 5.0 * 17.0 + 7.0 * 19.0);
}

TEST(Matrix4_FloatSimdMatchesDoubleReference) {
    MathRandom random(0x77aa55cc33ee11ffull ^ CurrentSeed());
    for (int iteration = 0; iteration < 5000; ++iteration) {
        const Mat4d a = RandomMatrix(random, 10.0);
        const Mat4d b = RandomMatrix(random, 10.0);
        Mat4 af;
        Mat4 bf;
        for (int index = 0; index < 16; ++index) {
            af.m[index] = static_cast<float>(a.m[index]);
            bf.m[index] = static_cast<float>(b.m[index]);
        }
        const Mat4 product = af * bf;
        Mat4d reference;
        for (int index = 0; index < 16; ++index) {
            reference.m[index] = static_cast<double>(af.m[index]);
        }
        Mat4d referenceB;
        for (int index = 0; index < 16; ++index) {
            referenceB.m[index] = static_cast<double>(bf.m[index]);
        }
        const Mat4d expected = reference * referenceB;
        bool near = true;
        for (int index = 0; index < 16; ++index) {
            near = near &&
                   Near(static_cast<double>(product.m[index]), expected.m[index], 1.0e-4, 1.0e-5);
        }
        CHECK(near);
        const Vec4 v{static_cast<float>(random.Range(-5.0, 5.0)),
                     static_cast<float>(random.Range(-5.0, 5.0)),
                     static_cast<float>(random.Range(-5.0, 5.0)), 1.0f};
        const Vec4 transformed = af * v;
        const Vec4d expectedVector = reference * Vec4d(v);
        CHECK(Near(static_cast<double>(transformed.x), expectedVector.x, 1.0e-4, 1.0e-5));
        CHECK(Near(static_cast<double>(transformed.w), expectedVector.w, 1.0e-4, 1.0e-5));
    }
}

TEST(Matrix4_DeterminantKnownValues) {
    CHECK(Mat4d::Identity().Determinant() == 1.0);
    CHECK(Mat4d::Scale(Vec3d(2.0, 3.0, 4.0)).Determinant() == 24.0);
    CHECK(Mat4d::Scale(Vec3d(-1.0, 1.0, 1.0)).Determinant() == -1.0);
    // 행을 교환하면 부호가 바뀐다.
    const Mat4d a = Asymmetric();
    const Mat4d swapped = Mat4d::FromRows(a.Row(1), a.Row(0), a.Row(2), a.Row(3));
    CHECK(Near(a.Determinant(), -swapped.Determinant(), 1.0e-9));
    CHECK(Near(a.Determinant(), 880.0, 1.0e-9)); // 소수 행렬의 독립 계산값
}

TEST(Matrix4_TryInverseMatchesOracleAndFailsOnSingular) {
    MathRandom random(0x0123456789abcdefull ^ CurrentSeed());
    int compared = 0;
    for (int iteration = 0; iteration < 3000; ++iteration) {
        const Mat4d matrix = RandomMatrix(random, 4.0);
        Mat4d oracle;
        if (!OracleInverse(matrix, oracle) || math::Abs(matrix.Determinant()) < 1.0e-3) {
            continue;
        }
        auto inverse = matrix.TryInverse();
        REQUIRE(inverse);
        CHECK(
            inverse.Value().Equals(oracle, 1.0e-7 * (1.0 + 1.0 / math::Abs(matrix.Determinant()))));
        CHECK((matrix * inverse.Value()).Equals(Mat4d::Identity(), 1.0e-9));
        ++compared;
    }
    CHECK(compared > 2500);
    // 특이 행렬: 같은 행 두 개, 영 행렬, scale 0.
    const Mat4d a = Asymmetric();
    CHECK(!Mat4d::FromRows(a.Row(0), a.Row(0), a.Row(2), a.Row(3)).TryInverse());
    CHECK(Mat4d::Zero().TryInverse().Error() == ErrorCode::InvalidArgument);
    CHECK(!Mat4::Scale(Vec3(1.0f, 0.0f, 1.0f)).TryInverse());
    Mat4d nanMatrix;
    nanMatrix.m[5] = __builtin_nan("");
    CHECK(!nanMatrix.TryInverse());
    // float 행렬: double 계산 뒤 float로 표현할 수 없는 역행렬은 실패한다.
    CHECK(!Mat4::Scale(Vec3(1.0e-39f, 1.0f, 1.0f)).TryInverse());    // 역수 1e39 > FLT_MAX
    CHECK(Mat4::Scale(Vec3(1.0e-30f, 1.0e-30f, 1.0f)).TryInverse()); // 1e30은 float 범위 안
    auto floatInverse = Mat4::Scale(Vec3(2.0f, 4.0f, 8.0f)).TryInverse();
    REQUIRE(floatInverse);
    CHECK(floatInverse.Value() == Mat4::Scale(Vec3(0.5f, 0.25f, 0.125f)));
}

TEST(MatrixFactory_RotationAxesFollowRightHandRule) {
    const double quarter = math::HalfPi<double>;
    CHECK(MakeRotationZ(quarter).TransformVector(Vec3d::Forward()).Equals(Vec3d::Left(), 1.0e-15));
    CHECK(MakeRotationX(quarter).TransformVector(Vec3d::Left()).Equals(Vec3d::Up(), 1.0e-15));
    CHECK(MakeRotationY(quarter).TransformVector(Vec3d::Up()).Equals(Vec3d::Forward(), 1.0e-15));
    // 회전 행렬과 같은 축의 quaternion은 같은 결과를 낸다.
    const Quatd q = Quatd::FromUnitAxisAngle(Vec3d::UnitY(), 0.7);
    CHECK(MakeRotation(q).Equals(MakeRotationY(0.7), 1.0e-15));
    CHECK((MakeRotation(q) * MakeInverseRotation(q)).Equals(Mat4d::Identity(), 1.0e-15));
}

TEST(MatrixFactory_TrsAndRotationAboutPoint) {
    const Quatd rotation = Quatd::FromUnitAxisAngle(Vec3d::Up(), math::HalfPi<double>);
    const Mat4d trs =
        MakeTranslationRotationScale(Vec3d(10.0, 0.0, 0.0), rotation, Vec3d(2.0, 1.0, 1.0));
    // scale(x2) → +90도 Z 회전 → 이동
    CHECK(trs.TransformPoint(Vec3d(1.0, 0.0, 0.0)).Equals(Vec3d(10.0, 2.0, 0.0), 1.0e-14));
    const Mat4d about = MakeRotationAboutPoint(rotation, Vec3d(1.0, 1.0, 0.0));
    CHECK(about.TransformPoint(Vec3d(1.0, 1.0, 0.0)).Equals(Vec3d(1.0, 1.0, 0.0), 1.0e-15));
    CHECK(about.TransformPoint(Vec3d(2.0, 1.0, 0.0)).Equals(Vec3d(1.0, 2.0, 0.0), 1.0e-15));
    CHECK(MakeRotationTranslation(rotation, Vec3d(0.0, 0.0, 5.0)).GetTranslation() ==
          Vec3d(0.0, 0.0, 5.0));
}

TEST(MatrixFactory_MirrorReflectsAcrossPlane) {
    // z = 2 평면 반사: (1, 2, 5) → (1, 2, -1). 법선 길이는 결과에 영향을 주지 않는다.
    auto mirror = TryMakeMirror(Vec3d(0.0, 0.0, 3.0), 6.0);
    REQUIRE(mirror);
    CHECK(
        mirror.Value().TransformPoint(Vec3d(1.0, 2.0, 5.0)).Equals(Vec3d(1.0, 2.0, -1.0), 1.0e-14));
    CHECK(Near(mirror.Value().Determinant(), -1.0, 1.0e-15));
    CHECK(!TryMakeMirror(Vec3d::Zero(), 1.0));
    CHECK(!TryMakeMirror(Vec3d::Up(), __builtin_nan("")));
}

TEST(MatrixFactory_PerspectiveMapsRocksViewToNdc) {
    const double fov = math::DegreesToRadians(90.0);
    auto reversed = TryMakePerspective(fov, 2.0, 0.5, 100.0);
    auto standard = TryMakePerspective(fov, 2.0, 0.5, 100.0, DepthRange::Standard);
    REQUIRE(reversed);
    REQUIRE(standard);
    auto ndc = [](const Mat4d& projection, const Vec3d& view) {
        const Vec4d clip = projection * Vec4d::Point(view);
        return Vec3d(clip.x / clip.w, clip.y / clip.w, clip.z / clip.w);
    };
    // reversed: near → 1, far → 0. standard: near → 0, far → 1.
    CHECK(Near(ndc(reversed.Value(), Vec3d(0.5, 0.0, 0.0)).z, 1.0, 1.0e-15));
    CHECK(Near(ndc(reversed.Value(), Vec3d(100.0, 0.0, 0.0)).z, 0.0, 1.0e-15));
    CHECK(Near(ndc(standard.Value(), Vec3d(0.5, 0.0, 0.0)).z, 0.0, 1.0e-15));
    CHECK(Near(ndc(standard.Value(), Vec3d(100.0, 0.0, 0.0)).z, 1.0, 1.0e-15));
    // +Y(왼쪽)은 NDC x 음수, +Z(위)는 NDC y 양수. 90도 수직 화각, aspect 2.
    const Vec3d leftEdge = ndc(reversed.Value(), Vec3d(1.0, 2.0, 0.0));
    const Vec3d topEdge = ndc(reversed.Value(), Vec3d(1.0, 0.0, 1.0));
    CHECK(Near(leftEdge.x, -1.0, 1.0e-15) && Near(topEdge.y, 1.0, 1.0e-15));
    // 무한 원거리: 깊이가 커질수록 0(reversed)·1(standard)에 수렴한다.
    auto infinite = TryMakePerspective(fov, 1.0, 0.1, static_cast<double>(__builtin_huge_val()));
    REQUIRE(infinite);
    CHECK(Near(ndc(infinite.Value(), Vec3d(0.1, 0.0, 0.0)).z, 1.0, 1.0e-15));
    CHECK(ndc(infinite.Value(), Vec3d(1.0e9, 0.0, 0.0)).z < 1.0e-9);
    // 실패 입력
    CHECK(!TryMakePerspective(0.0, 1.0, 0.1, 10.0));
    CHECK(!TryMakePerspective(math::Pi<double>, 1.0, 0.1, 10.0));
    CHECK(!TryMakePerspective(1.0, 0.0, 0.1, 10.0));
    CHECK(!TryMakePerspective(1.0, 1.0, 0.0, 10.0));
    CHECK(!TryMakePerspective(1.0, 1.0, 10.0, 10.0));
    CHECK(!TryMakePerspective(1.0, 1.0, 0.1, __builtin_nan("")));
    CHECK(!TryMakePerspectiveOffCenter(1.0, -1.0, -1.0, 1.0, 0.1, 10.0));
}

TEST(MatrixFactory_OffCenterPerspectiveMatchesBounds) {
    auto projection = TryMakePerspectiveOffCenter(-0.2, 0.6, -0.1, 0.3, 0.5, 50.0);
    REQUIRE(projection);
    // near 평면에서 화면 left/right = view y의 -1배.
    const Vec4d leftBottom = projection.Value() * Vec4d::Point(Vec3d(0.5, 0.2, -0.1));
    const Vec4d rightTop = projection.Value() * Vec4d::Point(Vec3d(0.5, -0.6, 0.3));
    CHECK(Near(leftBottom.x / leftBottom.w, -1.0, 1.0e-14));
    CHECK(Near(leftBottom.y / leftBottom.w, -1.0, 1.0e-14));
    CHECK(Near(rightTop.x / rightTop.w, 1.0, 1.0e-14));
    CHECK(Near(rightTop.y / rightTop.w, 1.0, 1.0e-14));
}

TEST(MatrixFactory_OrthographicMapsBoxToNdc) {
    auto reversed = TryMakeOrthographic(20.0, 10.0, 1.0, 101.0);
    auto standard =
        TryMakeOrthographicOffCenter(-10.0, 10.0, -5.0, 5.0, 1.0, 101.0, DepthRange::Standard);
    REQUIRE(reversed);
    REQUIRE(standard);
    const Vec4d corner = reversed.Value() * Vec4d::Point(Vec3d(1.0, -10.0, 5.0));
    CHECK(corner.w == 1.0);
    CHECK(Near(corner.x, 1.0, 1.0e-15) && Near(corner.y, 1.0, 1.0e-15));
    CHECK(Near(corner.z, 1.0, 1.0e-15)); // reversed near
    CHECK(Near((reversed.Value() * Vec4d::Point(Vec3d(101.0, 0.0, 0.0))).z, 0.0, 1.0e-15));
    CHECK(Near((standard.Value() * Vec4d::Point(Vec3d(1.0, 0.0, 0.0))).z, 0.0, 1.0e-15));
    CHECK(Near((standard.Value() * Vec4d::Point(Vec3d(101.0, 0.0, 0.0))).z, 1.0, 1.0e-15));
    CHECK(!TryMakeOrthographic(0.0, 1.0, 0.0, 1.0));
    CHECK(!TryMakeOrthographic(1.0, 1.0, 5.0, 5.0));
    CHECK(!TryMakeOrthographic(1.0, 1.0, 0.0, static_cast<double>(__builtin_huge_val())));
    CHECK(!TryMakeOrthographic(1.0, 1.0, -1.0, 5.0));
}

TEST(MatrixFactory_LookAtBuildsRocksViewSpace) {
    // eye (0,0,0)에서 +Y를 보면 view +X(전방)는 world +Y, view +Y(왼쪽)는 world -X다.
    auto view = TryMakeLookAt(Vec3d::Zero(), Vec3d(0.0, 5.0, 0.0), Vec3d::Up());
    REQUIRE(view);
    CHECK(view.Value().TransformPoint(Vec3d(0.0, 5.0, 0.0)).Equals(Vec3d(5.0, 0.0, 0.0), 1.0e-15));
    CHECK(view.Value().TransformPoint(Vec3d(-1.0, 0.0, 0.0)).Equals(Vec3d(0.0, 1.0, 0.0), 1.0e-15));
    CHECK(view.Value().TransformPoint(Vec3d(0.0, 0.0, 2.0)).Equals(Vec3d(0.0, 0.0, 2.0), 1.0e-15));
    auto moved = TryMakeLookTo(Vec3d(10.0, 0.0, 0.0), Vec3d::Forward(), Vec3d::Up());
    REQUIRE(moved);
    CHECK(
        moved.Value().TransformPoint(Vec3d(13.0, 1.0, 2.0)).Equals(Vec3d(3.0, 1.0, 2.0), 1.0e-15));
    CHECK(Near(moved.Value().Determinant(), 1.0, 1.0e-15));
    // 실패: eye == target, up이 전방과 평행, 비유한 입력.
    CHECK(!TryMakeLookAt(Vec3d::One(), Vec3d::One(), Vec3d::Up()));
    CHECK(!TryMakeLookTo(Vec3d::Zero(), Vec3d::Up(), Vec3d::Up()));
    // 외적이 정확히 0이 아닌 평행 입력(리뷰 재현: |up × forward| ≈ 4.4e-16)도 실패한다.
    CHECK(!TryMakeLookTo(Vec3d::Zero(), Vec3d(3.0, -7.0, 11.0), Vec3d(3.0, -7.0, 11.0)));
    CHECK(!TryMakeLookTo(Vec3d::Zero(), Vec3d(3.0, -7.0, 11.0), Vec3d(-6.0, 14.0, -22.0)));
    CHECK(!TryMakeLookTo(Vec3::Zero(), Vec3(3.0f, -7.0f, 11.0f), Vec3(0.3f, -0.7f, 1.1f)));
    CHECK(!TryMakeLookTo(Vec3d::Zero(), Vec3d::Forward(), Vec3d::Zero()));
    // 거의 평행하지만 기준보다 큰 각은 성공한다.
    CHECK(TryMakeLookTo(Vec3d::Zero(), Vec3d::Up(), Vec3d(1.0e-6, 0.0, 1.0)));
    CHECK(!TryMakeLookTo(Vec3d::Zero(), Vec3d::Forward(), Vec3d(0.0, 0.0, __builtin_nan(""))));
}

TEST(MatrixFactory_ObliqueClipPlaneBecomesNearPlane) {
    auto projection = TryMakePerspective(1.2, 1.0, 0.1, 100.0, DepthRange::Standard);
    REQUIRE(projection);
    // view x = 5 평면(법선 +X, 거리 5)보다 앞쪽만 보이게 한다.
    auto oblique = TryMakeObliqueClipProjection(projection.Value(), Vec3d(2.0, 0.0, 0.0), 10.0,
                                                DepthRange::Standard);
    REQUIRE(oblique);
    auto depthAt = [&](const Vec3d& view) {
        const Vec4d clip = oblique.Value() * Vec4d::Point(view);
        return clip.z / clip.w;
    };
    CHECK(Near(depthAt(Vec3d(5.0, 0.0, 0.0)), 0.0, 1.0e-12));
    CHECK(Near(depthAt(Vec3d(5.0, 1.0, -2.0)), 0.0, 1.0e-12));
    CHECK(depthAt(Vec3d(4.0, 0.0, 0.0)) < 0.0); // 잘리는 쪽
    const double inside = depthAt(Vec3d(20.0, 1.0, 1.0));
    CHECK(inside > 0.0 && inside <= 1.0);
    // 카메라가 보이는 쪽에 있으면 실패한다.
    CHECK(!TryMakeObliqueClipProjection(projection.Value(), Vec3d(1.0, 0.0, 0.0), -1.0,
                                        DepthRange::Standard));
    CHECK(!TryMakeObliqueClipProjection(projection.Value(), Vec3d::Zero(), 1.0,
                                        DepthRange::Standard));
}

TEST(MatrixFactory_ObliqueClipSupportsReversedDepth) {
    // 기본 Reversed 투영에 Standard 공식을 쓰면 보이는 쪽이 뒤집힌다(리뷰 재현 입력).
    auto projection = TryMakePerspective(1.2, 1.0, 0.1, 100.0);
    REQUIRE(projection);
    auto oblique = TryMakeObliqueClipProjection(projection.Value(), Vec3d(1.0, 0.0, 0.0), 5.0,
                                                DepthRange::Reversed);
    REQUIRE(oblique);
    auto depthAt = [&](const Vec3d& view) {
        const Vec4d clip = oblique.Value() * Vec4d::Point(view);
        return clip.z / clip.w;
    };
    // Reversed: 새 near(평면 위)는 z/w = 1, 잘리는 쪽은 1보다 크고 보이는 쪽은 [0, 1)이다.
    CHECK(Near(depthAt(Vec3d(5.0, 0.0, 0.0)), 1.0, 1.0e-12));
    CHECK(Near(depthAt(Vec3d(5.0, 0.3, -0.2)), 1.0, 1.0e-12));
    CHECK(depthAt(Vec3d(4.0, 0.0, 0.0)) > 1.0);
    const double inside = depthAt(Vec3d(20.0, 1.0, 1.0));
    CHECK(inside >= 0.0 && inside < 1.0);
    const double far = depthAt(Vec3d(100.0, 0.0, 0.0));
    CHECK(far >= -1.0e-12 && far < 1.0);
    // 무작위 frustum 점: 평면 앞쪽은 [0, 1], 뒤쪽은 1 초과다.
    MathRandom random(0x0b11c0e5ull ^ CurrentSeed());
    for (int index = 0; index < 2000; ++index) {
        const double x = random.Range(0.2, 99.0);
        const double extent = x * 0.6;
        const Vec3d point{x, random.Range(-extent, extent), random.Range(-extent, extent)};
        const double depth = depthAt(point);
        if (x > 5.0 + 1.0e-9) {
            CHECK(depth <= 1.0 + 1.0e-12);
        } else if (x < 5.0 - 1.0e-9) {
            CHECK(depth > 1.0);
        }
    }
}

TEST(MatrixFactory_ObliqueClipRejectsUnsupportedInputs) {
    auto standard = TryMakePerspective(1.2, 1.0, 0.1, 100.0, DepthRange::Standard);
    auto reversed = TryMakePerspective(1.2, 1.0, 0.1, 100.0, DepthRange::Reversed);
    REQUIRE(standard && reversed);
    // 행렬과 다른 depth 플래그는 보이는 쪽을 뒤집으므로 거부한다.
    CHECK(TryMakeObliqueClipProjection(standard.Value(), Vec3d::UnitX(), 5.0, DepthRange::Reversed)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(TryMakeObliqueClipProjection(reversed.Value(), Vec3d::UnitX(), 5.0, DepthRange::Standard)
              .Error() == ErrorCode::InvalidArgument);
    // far 평면 너머의 평면(재검토 재현: distance 200 > far 100)은 보이는 영역이 없다.
    CHECK(
        TryMakeObliqueClipProjection(standard.Value(), Vec3d::UnitX(), 200.0, DepthRange::Standard)
            .Error() == ErrorCode::InvalidArgument);
    CHECK(
        TryMakeObliqueClipProjection(reversed.Value(), Vec3d::UnitX(), 200.0, DepthRange::Reversed)
            .Error() == ErrorCode::InvalidArgument);
    // 무한 원거리 Reversed 투영은 지원한다.
    auto infinite = TryMakePerspective(1.2, 1.0, 0.1, static_cast<double>(__builtin_huge_val()));
    REQUIRE(infinite);
    CHECK(TryMakeObliqueClipProjection(infinite.Value(), Vec3d::UnitX(), 200.0,
                                       DepthRange::Reversed));
    // 직교 투영은 지원하지 않는다.
    auto ortho = TryMakeOrthographic(10.0, 10.0, 0.0, 100.0, DepthRange::Standard);
    REQUIRE(ortho);
    CHECK(TryMakeObliqueClipProjection(ortho.Value(), Vec3d::UnitX(), 5.0, DepthRange::Standard)
              .Error() == ErrorCode::Unsupported);
}

TEST(MatrixFactory_LookToStaysOrthonormalNearThreshold) {
    // 재검토 재현 입력(float): 기준 바로 위의 각에서 X·Y가 0.0043이었다.
    const Vec3 forward(-0x1.ce63dp-2f, -0x1.290d8p-1f, 0x1.5b1012p-1f);
    const Vec3 up(-0x1.ce640ep-2f, -0x1.290c7ap-1f, 0x1.5b10dep-1f);
    auto view = TryMakeLookTo(Vec3::Zero(), forward, up);
    REQUIRE(view);
    const Mat4& m = view.Value();
    const Vec3 x(m.At(0, 0), m.At(0, 1), m.At(0, 2));
    const Vec3 y(m.At(1, 0), m.At(1, 1), m.At(1, 2));
    const Vec3 z(m.At(2, 0), m.At(2, 1), m.At(2, 2));
    CHECK(math::Abs(Dot(x, y)) <= 1.0e-6f && math::Abs(Dot(x, z)) <= 1.0e-6f &&
          math::Abs(Dot(y, z)) <= 1.0e-6f);
    CHECK(x.IsNormalized(1.0e-6f) && y.IsNormalized(1.0e-6f) && z.IsNormalized(1.0e-6f));
    CHECK(Dot(z, up) > 0.0f); // 위쪽 방향은 유지한다.
}

TEST(MatrixFactory_PerspectiveAcceptsHugeFarPlane) {
    // n * f가 float 범위를 넘어도 n * (f / (f - n))은 유한하다.
    auto projection = TryMakePerspective(1.0f, 1.0f, 10.0f, 1.0e38f);
    REQUIRE(projection);
    auto standard = TryMakePerspective(1.0f, 1.0f, 10.0f, 1.0e38f, DepthRange::Standard);
    REQUIRE(standard);
}

TEST(MathBatch_TransformPointsMatchesPerElement) {
    const Mat4 matrix = MakeTranslationRotationScale(
        Vec3(1.0f, -2.0f, 3.0f), Quat::FromUnitAxisAngle(Vec3::Up(), 0.3f), Vec3(2.0f, 1.0f, 0.5f));
    MathRandom random(0xfeedfacecafebeefull ^ CurrentSeed());
    const usize counts[] = {0, 1, 2, 3, 4, 5, 17};
    for (usize count : counts) {
        Vec3 input[17];
        Vec3 output[17];
        for (usize index = 0; index < count; ++index) {
            input[index] = Vec3{static_cast<float>(random.Range(-9.0, 9.0)),
                                static_cast<float>(random.Range(-9.0, 9.0)),
                                static_cast<float>(random.Range(-9.0, 9.0))};
        }
        math::TransformPoints(matrix, Span<const Vec3>(input, count), Span<Vec3>(output, count));
        bool same = true;
        for (usize index = 0; index < count; ++index) {
            same = same && output[index] == matrix.TransformPoint(input[index]);
        }
        CHECK(same);
        // in-place 변환은 같은 결과다.
        math::TransformVectors(matrix, Span<const Vec3>(input, count), Span<Vec3>(output, count));
        math::TransformVectors(matrix, Span<const Vec3>(input, count), Span<Vec3>(input, count));
        for (usize index = 0; index < count; ++index) {
            same = same && output[index] == input[index];
        }
        CHECK(same);
    }
}

} // namespace rocks::test
