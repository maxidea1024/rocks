#include <TestSupport/Test.h>
#include <TestSupport/MathTestSupport.h>
#include <Core/Math/MathBatch.h>
#include <Core/Math/MatrixFactory.h>
#include <Platform/PlatformMath.h>

namespace rocks::test {
namespace math_matrix_tests_file {

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

TEST(Matrix4_StorageIsRowMajorWithRowVectors) {
    // UE FMatrix와 같은 M[row][column] 행 우선 배치, 이동은 마지막 행(m[12..14] = M[3][0..2]).
    const Mat4d translation = Mat4d::Translation(Vec3d(1.0, 2.0, 3.0));
    CHECK(translation.m[12] == 1.0 && translation.m[13] == 2.0 && translation.m[14] == 3.0);
    CHECK(translation.At(3, 0) == 1.0 && translation.At(3, 1) == 2.0 &&
          translation.At(0, 3) == 0.0);
    CHECK(translation.TransformPoint(Vec3d(10.0, 0.0, 0.0)) == Vec3d(11.0, 2.0, 3.0));
    CHECK(translation.TransformVector(Vec3d(10.0, 0.0, 0.0)) == Vec3d(10.0, 0.0, 0.0));
    const Mat4d rows = Asymmetric();
    CHECK(rows.At(0, 1) == 3.0 && rows.At(1, 0) == 11.0);
    CHECK(rows.m[1] == 3.0 && rows.m[4] == 11.0);
    CHECK(rows.Row(2) == Vec4d(23.0, 29.0, 31.0, 37.0));
    CHECK(rows.Column(1) == Vec4d(3.0, 13.0, 29.0, 43.0));
    CHECK(Mat4d::FromColumns(rows.Column(0), rows.Column(1), rows.Column(2), rows.Column(3)) ==
          rows);
    CHECK(rows.GetScaledAxis(1) == Vec3d(11.0, 13.0, 17.0));
    CHECK(rows.Transposed().Transposed() == rows);
    // 행 벡터 곱: (1, 2, 3, 4) · M = Σ v[k] · M[k][*].
    CHECK(Vec4d(1.0, 2.0, 3.0, 4.0) * rows ==
          Vec4d(2.0 + 22.0 + 69.0 + 164.0, 3.0 + 26.0 + 87.0 + 172.0, 5.0 + 34.0 + 93.0 + 188.0,
                7.0 + 38.0 + 111.0 + 212.0));
    CHECK(Mat4::Identity() == Mat4());
}

TEST(Matrix4_PrecisionConversionIsExplicit) {
    // float → double은 원소마다 정확하고, 되돌리면 원래 값이다.
    const Mat4 single =
        Mat4::FromRows(Vec4(0.1f, -2.5f, 3.0e30f, 7.0f), Vec4(1.0e-40f, 1.0f, -0.0f, 2.0f),
                       Vec4(0x1.fffffep127f, 0.3f, 4.0f, -1.0f), Vec4(0.0f, 0.0f, 0.0f, 1.0f));
    const Mat4d wide(single);
    for (int index = 0; index < 16; ++index) {
        CHECK(wide.m[index] == static_cast<double>(single.m[index]));
    }

    CHECK(Mat4(wide) == single);
    // double → float은 원소마다 가장 가까운 값(같은 거리면 짝수 쪽)이고 float 범위 밖은 ±Inf다.
    Mat4d exact = Mat4d::Identity();
    exact.m[0] = 0x1.000001p0;
    exact.m[1] = 0x1.000003p0;
    exact.m[2] = 1.0e39;
    exact.m[3] = -1.0e39;
    const Mat4 rounded(exact);
    CHECK(rounded.m[0] == 1.0f && rounded.m[1] == 0x1.000004p0f);
    CHECK(rounded.m[2] == platform::PlatformMath::Infinity<float>());
    CHECK(rounded.m[3] == -platform::PlatformMath::Infinity<float>());
    CHECK(!rounded.IsFinite());
}

TEST(Matrix4_CompositionAppliesLeftOperandFirst) {
    // UE FMatrix와 같이 A * B는 A를 먼저 적용한다(Matrix.h:28-32): local * parent.
    const Mat4d scale = Mat4d::Scale(Vec3d(2.0, 2.0, 2.0));
    const Mat4d move = Mat4d::Translation(Vec3d(1.0, 0.0, 0.0));
    CHECK((scale * move).TransformPoint(Vec3d(1.0, 0.0, 0.0)) == Vec3d(3.0, 0.0, 0.0));
    CHECK((move * scale).TransformPoint(Vec3d(1.0, 0.0, 0.0)) == Vec3d(4.0, 0.0, 0.0));
    const Vec3d p(0.5, -1.5, 2.5);
    CHECK((scale * move).TransformPoint(p) == move.TransformPoint(scale.TransformPoint(p)));
    // 비대칭 행렬 곱의 원소를 손으로 계산해 고정한다: (A * Aᵀ)[0][1] = A의 행 0 · 행 1.
    const Mat4d product = Asymmetric() * Asymmetric().Transposed();
    CHECK(product.At(0, 0) == 4.0 + 9.0 + 25.0 + 49.0);
    CHECK(product.At(0, 1) == 2.0 * 11.0 + 3.0 * 13.0 + 5.0 * 17.0 + 7.0 * 19.0);
    // (Aᵀ * A)[0][1] = A의 열 0 · 열 1(곱 순서가 바뀌면 다른 값이다).
    const Mat4d reversed = Asymmetric().Transposed() * Asymmetric();
    CHECK(reversed.At(0, 1) == 2.0 * 3.0 + 11.0 * 13.0 + 23.0 * 29.0 + 41.0 * 43.0);
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
        const Vec4 transformed = v * af;
        const Vec4d expectedVector = Vec4d(v) * reference;
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
    nanMatrix.m[5] = platform::PlatformMath::QuietNaN<double>();
    CHECK(!nanMatrix.TryInverse());
    // float 행렬: double 계산 뒤 float로 표현할 수 없는 역행렬은 실패한다.
    CHECK(!Mat4::Scale(Vec3(1.0e-39f, 1.0f, 1.0f)).TryInverse());    // 역수 1e39 > FLT_MAX
    CHECK(Mat4::Scale(Vec3(1.0e-30f, 1.0e-30f, 1.0f)).TryInverse()); // 1e30은 float 범위 안
    auto floatInverse = Mat4::Scale(Vec3(2.0f, 4.0f, 8.0f)).TryInverse();
    REQUIRE(floatInverse);
    CHECK(floatInverse.Value() == Mat4::Scale(Vec3(0.5f, 0.25f, 0.125f)));
}

TEST(MatrixFactory_QuaternionRotationMatrixMatchesUnreal) {
    // UE에는 축별 회전 행렬 생성 함수가 없다. 축 회전은 quaternion 축-각 → 회전 행렬이며 양의 각은
    // 왼손 회전이다(UE Quat.h:912-921 — Z +90도는 Forward를 Right로 보낸다, UE 식 probe).
    const double quarter = math::HalfPi<double>;
    const Mat4d yaw = MakeRotation(Quatd::FromUnitAxisAngle(Vec3d::Up(), quarter));
    const Mat4d roll = MakeRotation(Quatd::FromUnitAxisAngle(Vec3d::Forward(), quarter));
    const Mat4d pitch = MakeRotation(Quatd::FromUnitAxisAngle(Vec3d::Right(), quarter));
    CHECK(yaw.TransformVector(Vec3d::Forward()).Equals(Vec3d::Right(), 1.0e-15));
    CHECK(roll.TransformVector(Vec3d::Right()).Equals(Vec3d::Up(), 1.0e-15));
    CHECK(pitch.TransformVector(Vec3d::Up()).Equals(Vec3d::Forward(), 1.0e-15));
    // 행 0·1·2는 회전된 X·Y·Z 축이다(UE GetScaledAxis·FQuat::GetAxisX/Y/Z).
    CHECK(yaw.GetScaledAxis(0).Equals(Vec3d::Right(), 1.0e-15));
    CHECK(yaw.GetScaledAxis(1).Equals(Vec3d::Backward(), 1.0e-15));
    // 회전 행렬과 같은 quaternion은 같은 결과를 내고, 역회전은 전치다.
    const Quatd q = Quatd::FromUnitAxisAngle(Vec3d(0.3, -0.4, 0.5).TryNormalize().Value(), 0.7);
    const Vec3d v(1.0, -2.0, 3.0);
    CHECK(MakeRotation(q).TransformVector(v).Equals(q.RotateVector(v), 1.0e-14));
    CHECK((MakeRotation(q) * MakeInverseRotation(q)).Equals(Mat4d::Identity(), 1.0e-15));
}

TEST(MatrixFactory_ScaleRotationTranslationAndRotationAboutPoint) {
    const Quatd rotation = Quatd::FromUnitAxisAngle(Vec3d::Up(), math::HalfPi<double>);
    // UE FScaleRotationTranslationMatrix(Scale, Rot, Origin) 인자 순서: scale(x2) → +90도 Z 회전 →
    // 이동.
    const Mat4d srt =
        MakeScaleRotationTranslation(Vec3d(2.0, 1.0, 1.0), rotation, Vec3d(10.0, 0.0, 0.0));
    CHECK(srt.TransformPoint(Vec3d(1.0, 0.0, 0.0)).Equals(Vec3d(10.0, 2.0, 0.0), 1.0e-14));
    CHECK(srt.Equals(Mat4d::Scale(Vec3d(2.0, 1.0, 1.0)) * MakeRotation(rotation) *
                         Mat4d::Translation(Vec3d(10.0, 0.0, 0.0)),
                     1.0e-15));
    const Mat4d about = MakeRotationAboutPoint(rotation, Vec3d(1.0, 1.0, 0.0));
    CHECK(about.TransformPoint(Vec3d(1.0, 1.0, 0.0)).Equals(Vec3d(1.0, 1.0, 0.0), 1.0e-15));
    CHECK(about.TransformPoint(Vec3d(2.0, 1.0, 0.0)).Equals(Vec3d(1.0, 2.0, 0.0), 1.0e-15));
    const Mat4d rotationTranslation = MakeRotationTranslation(rotation, Vec3d(0.0, 0.0, 5.0));
    CHECK(rotationTranslation.GetTranslation() == Vec3d(0.0, 0.0, 5.0));
    CHECK(rotationTranslation.Equals(
        MakeRotation(rotation) * Mat4d::Translation(Vec3d(0.0, 0.0, 5.0)), 0.0));
}

TEST(MatrixFactory_MirrorReflectsAcrossPlane) {
    // z = 2 평면 반사: (1, 2, 5) → (1, 2, -1). 법선 길이는 결과에 영향을 주지 않는다.
    auto mirror = TryMakeMirror(Vec3d(0.0, 0.0, 3.0), 6.0);
    REQUIRE(mirror);
    CHECK(
        mirror.Value().TransformPoint(Vec3d(1.0, 2.0, 5.0)).Equals(Vec3d(1.0, 2.0, -1.0), 1.0e-14));
    CHECK(Near(mirror.Value().Determinant(), -1.0, 1.0e-15));
    CHECK(!TryMakeMirror(Vec3d::Zero(), 1.0));
    CHECK(!TryMakeMirror(Vec3d::Up(), platform::PlatformMath::QuietNaN<double>()));
}

namespace {

// view 공간 점(행 벡터)을 투영해 NDC로 나눈다.
Vec3d ToNdc(const Mat4d& projection, const Vec3d& view) {
    const Vec4d clip = Vec4d::Point(view) * projection;
    return Vec3d(clip.x / clip.w, clip.y / clip.w, clip.z / clip.w);
}

} // namespace

TEST(MatrixFactory_PerspectiveMapsUnrealViewToNdc) {
    // UE view 공간(+X 오른쪽, +Y 위, +Z 전방 깊이) → NDC. 수평 반화각 45도, 화면 2:1.
    const double halfFov = math::DegreesToRadians(45.0);
    auto reversed = TryMakePerspective(halfFov, 2.0, 1.0, 0.5, 100.0);
    auto standard = TryMakePerspective(halfFov, 2.0, 1.0, 0.5, 100.0, DepthRange::Standard);
    REQUIRE(reversed);
    REQUIRE(standard);
    // reversed: near → 1, far → 0. standard: near → 0, far → 1. w는 view z다.
    CHECK(Near(ToNdc(reversed.Value(), Vec3d(0.0, 0.0, 0.5)).z, 1.0, 1.0e-15));
    CHECK(Near(ToNdc(reversed.Value(), Vec3d(0.0, 0.0, 100.0)).z, 0.0, 1.0e-15));
    CHECK(Near(ToNdc(standard.Value(), Vec3d(0.0, 0.0, 0.5)).z, 0.0, 1.0e-15));
    CHECK(Near(ToNdc(standard.Value(), Vec3d(0.0, 0.0, 100.0)).z, 1.0, 1.0e-15));
    // 수평 반화각 45도: view (1, 0, 1)은 오른쪽 가장자리(NDC x = +1), 화면 2:1이라 (0, 0.5, 1)은 위
    // 가장자리(NDC y = +1)다. view +X·+Y가 NDC +x·+y로 간다.
    CHECK(Near(ToNdc(reversed.Value(), Vec3d(1.0, 0.0, 1.0)).x, 1.0, 1.0e-15));
    CHECK(Near(ToNdc(reversed.Value(), Vec3d(0.0, 0.5, 1.0)).y, 1.0, 1.0e-15));
    CHECK(Near(ToNdc(reversed.Value(), Vec3d(-1.0, -0.5, 1.0)).x, -1.0, 1.0e-15));
    // 무한 원거리(UE 4인자 생성자): M[2][2] = 0, M[3][2] = near이고 깊이가 커질수록 0에 수렴한다.
    auto infinite = TryMakePerspective(halfFov, 1.0, 1.0, 0.1);
    REQUIRE(infinite);
    CHECK(infinite.Value().At(2, 2) == 0.0 && infinite.Value().At(3, 2) == 0.1);
    CHECK(Near(ToNdc(infinite.Value(), Vec3d(0.0, 0.0, 0.1)).z, 1.0, 1.0e-15));
    CHECK(ToNdc(infinite.Value(), Vec3d(0.0, 0.0, 1.0e9)).z < 1.0e-9);
    // 실패 입력
    const double nan = platform::PlatformMath::QuietNaN<double>();
    CHECK(!TryMakePerspective(0.0, 1.0, 1.0, 0.1, 10.0));
    CHECK(!TryMakePerspective(math::HalfPi<double>, 1.0, 1.0, 0.1, 10.0));
    CHECK(!TryMakePerspective(1.0, 0.0, 1.0, 0.1, 10.0));
    CHECK(!TryMakePerspective(1.0, 1.0, -1.0, 0.1, 10.0));
    CHECK(!TryMakePerspective(1.0, 1.0, 1.0, 0.0, 10.0));
    CHECK(!TryMakePerspective(1.0, 1.0, 1.0, 10.0, 10.0));
    CHECK(!TryMakePerspective(1.0, 1.0, 1.0, 0.1, nan));
    CHECK(!TryMakePerspective(nan, 1.0, 1.0, 0.1));
    CHECK(!TryMakePerspectiveOffCenter(1.0, -1.0, -1.0, 1.0, 0.1, 10.0));
}

TEST(MatrixFactory_OffCenterPerspectiveMatchesBounds) {
    auto projection = TryMakePerspectiveOffCenter(-0.2, 0.6, -0.1, 0.3, 0.5, 50.0);
    REQUIRE(projection);
    // near 평면(view z = 0.5)에서 view x = left/right, view y = bottom/top이 NDC ±1이다.
    const Vec3d leftBottom = ToNdc(projection.Value(), Vec3d(-0.2, -0.1, 0.5));
    const Vec3d rightTop = ToNdc(projection.Value(), Vec3d(0.6, 0.3, 0.5));
    CHECK(Near(leftBottom.x, -1.0, 1.0e-14) && Near(leftBottom.y, -1.0, 1.0e-14));
    CHECK(Near(rightTop.x, 1.0, 1.0e-14) && Near(rightTop.y, 1.0, 1.0e-14));
    CHECK(Near(leftBottom.z, 1.0, 1.0e-14)); // reversed near
    // 대칭 경계면 대칭 투영과 같다(off-center 열 M[2][0]·M[2][1]이 0).
    auto symmetric = TryMakePerspectiveOffCenter(-0.5, 0.5, -0.25, 0.25, 0.5, 50.0);
    auto reference = TryMakePerspective(math::Pi<double> / 4.0, 2.0, 1.0, 0.5, 50.0);
    REQUIRE(symmetric && reference);
    CHECK(symmetric.Value().Equals(reference.Value(), 1.0e-15));
}

TEST(MatrixFactory_OrthographicMapsBoxToNdc) {
    // 반폭 10·반높이 5(UE FReversedZOrthoMatrix의 Width·Height 의미), near 1, far 101.
    auto reversed = TryMakeOrthographic(10.0, 5.0, 1.0, 101.0);
    auto standard =
        TryMakeOrthographicOffCenter(-10.0, 10.0, -5.0, 5.0, 1.0, 101.0, DepthRange::Standard);
    REQUIRE(reversed);
    REQUIRE(standard);
    const Vec4d corner = Vec4d::Point(Vec3d(10.0, 5.0, 1.0)) * reversed.Value();
    CHECK(corner.w == 1.0);
    CHECK(Near(corner.x, 1.0, 1.0e-15) && Near(corner.y, 1.0, 1.0e-15));
    CHECK(Near(corner.z, 1.0, 1.0e-15)); // reversed near
    CHECK(Near((Vec4d::Point(Vec3d(0.0, 0.0, 101.0)) * reversed.Value()).z, 0.0, 1.0e-15));
    CHECK(Near((Vec4d::Point(Vec3d(0.0, 0.0, 1.0)) * standard.Value()).z, 0.0, 1.0e-15));
    CHECK(Near((Vec4d::Point(Vec3d(0.0, 0.0, 101.0)) * standard.Value()).z, 1.0, 1.0e-15));
    // 대칭 off-center는 대칭 함수와 같은 행렬이다.
    auto reversedOffCenter = TryMakeOrthographicOffCenter(-10.0, 10.0, -5.0, 5.0, 1.0, 101.0);
    REQUIRE(reversedOffCenter);
    CHECK(reversedOffCenter.Value().Equals(reversed.Value(), 1.0e-15));
    CHECK(!TryMakeOrthographic(0.0, 1.0, 0.0, 1.0));
    CHECK(!TryMakeOrthographic(1.0, 1.0, 5.0, 5.0));
    CHECK(!TryMakeOrthographic(1.0, 1.0, 0.0,
                               static_cast<double>(platform::PlatformMath::Infinity<double>())));
    CHECK(!TryMakeOrthographic(1.0, 1.0, -1.0, 5.0));
}

TEST(MatrixFactory_LookFromBuildsUnrealViewSpace) {
    // eye (0,0,0)에서 world +Y(오른쪽)를 보면 view +Z(전방) = world +Y, view +X(오른쪽) = world -X,
    // view +Y(위) = world +Z다(UE LookFrom: X = Up × Z, Y = Z × X).
    auto view = TryMakeLookAt(Vec3d::Zero(), Vec3d(0.0, 5.0, 0.0), Vec3d::Up());
    REQUIRE(view);
    CHECK(view.Value().TransformPoint(Vec3d(0.0, 5.0, 0.0)) == Vec3d(0.0, 0.0, 5.0));
    CHECK(view.Value().TransformPoint(Vec3d(-1.0, 0.0, 0.0)) == Vec3d(1.0, 0.0, 0.0));
    CHECK(view.Value().TransformPoint(Vec3d(0.0, 0.0, 2.0)) == Vec3d(0.0, 2.0, 0.0));
    auto moved = TryMakeLookFrom(Vec3d(10.0, 0.0, 0.0), Vec3d::Forward(), Vec3d::Up());
    REQUIRE(moved);
    CHECK(moved.Value().TransformPoint(Vec3d(13.0, 1.0, 2.0)) == Vec3d(1.0, 2.0, 3.0));
    CHECK(Near(moved.Value().Determinant(), 1.0, 1.0e-15));
    // 카메라 회전의 전방·위 벡터로 만든 LookFrom은 MakeWorldToView와 같은 view다.
    const Quatd camera = Rotator<double>(10.0, 30.0, 20.0).ToQuaternion();
    const Vec3d eye(100.0, 200.0, 300.0);
    auto fromAxes = TryMakeLookFrom(eye, camera.GetForwardVector(), camera.GetUpVector());
    REQUIRE(fromAxes);
    CHECK(fromAxes.Value().Equals(MakeWorldToView(eye, camera), 1.0e-12));
    // 실패: eye == target, up이 전방과 평행, 비유한 입력.
    CHECK(!TryMakeLookAt(Vec3d::One(), Vec3d::One(), Vec3d::Up()));
    CHECK(!TryMakeLookFrom(Vec3d::Zero(), Vec3d::Up(), Vec3d::Up()));
    // 외적이 정확히 0이 아닌 평행 입력(리뷰 재현: |up × forward| ≈ 4.4e-16)도 실패한다.
    CHECK(!TryMakeLookFrom(Vec3d::Zero(), Vec3d(3.0, -7.0, 11.0), Vec3d(3.0, -7.0, 11.0)));
    CHECK(!TryMakeLookFrom(Vec3d::Zero(), Vec3d(3.0, -7.0, 11.0), Vec3d(-6.0, 14.0, -22.0)));
    CHECK(!TryMakeLookFrom(Vec3::Zero(), Vec3(3.0f, -7.0f, 11.0f), Vec3(0.3f, -0.7f, 1.1f)));
    CHECK(!TryMakeLookFrom(Vec3d::Zero(), Vec3d::Forward(), Vec3d::Zero()));
    // 거의 평행하지만 기준보다 큰 각은 성공한다.
    CHECK(TryMakeLookFrom(Vec3d::Zero(), Vec3d::Up(), Vec3d(1.0e-6, 0.0, 1.0)));
    CHECK(!TryMakeLookFrom(Vec3d::Zero(), Vec3d::Forward(),
                           Vec3d(0.0, 0.0, platform::PlatformMath::QuietNaN<double>())));
}

TEST(MatrixFactory_ViewRotationSwizzlesCameraAxes) {
    // UE FInverseRotationMatrix(Rot) * swizzle(LocalPlayer.cpp:1241-1245): 회전 없는 카메라에서
    // 월드 전방·오른쪽·위가 view +Z·+X·+Y다(UE 식 probe, 정확 일치).
    const Mat4d identityView = MakeWorldToView(Vec3d::Zero(), Quatd::Identity());
    CHECK(identityView.TransformPoint(Vec3d(100.0, 0.0, 0.0)) == Vec3d(0.0, 0.0, 100.0));
    CHECK(identityView.TransformPoint(Vec3d(0.0, 100.0, 0.0)) == Vec3d(100.0, 0.0, 0.0));
    CHECK(identityView.TransformPoint(Vec3d(0.0, 0.0, 100.0)) == Vec3d(0.0, 100.0, 0.0));
    // 카메라가 yaw 90도(오른쪽)를 보면 월드 +Y가 view 전방이다. 카메라 위치는 먼저 뺀다.
    const Mat4d facingRight =
        MakeWorldToView(Vec3d(0.0, 0.0, 170.0), Rotator<double>(0.0, 90.0, 0.0));
    CHECK(facingRight.TransformPoint(Vec3d(0.0, 500.0, 170.0))
              .Equals(Vec3d(0.0, 0.0, 500.0), 1.0e-12));
    CHECK(facingRight.TransformPoint(Vec3d(-30.0, 0.0, 170.0))
              .Equals(Vec3d(30.0, 0.0, 0.0), 1.0e-12));
    // view 회전 행렬은 직교이고 행렬식 +1이다.
    const Mat4d rotation = MakeViewRotation(Rotator<double>(10.0, 30.0, 20.0));
    CHECK(Near(rotation.Determinant(), 1.0, 1.0e-15));
    CHECK((rotation * rotation.Transposed()).Equals(Mat4d::Identity(), 1.0e-15));
}

TEST(MatrixFactory_ObliqueClipPlaneBecomesNearPlane) {
    auto projection = TryMakePerspective(0.6, 1.0, 1.0, 0.1, 100.0, DepthRange::Standard);
    REQUIRE(projection);
    // view z = 5 평면(법선 +Z 전방, 거리 5)보다 앞쪽만 보이게 한다.
    auto oblique = TryMakeObliqueClipProjection(projection.Value(), Vec3d(0.0, 0.0, 2.0), 10.0,
                                                DepthRange::Standard);
    REQUIRE(oblique);
    auto depthAt = [&](const Vec3d& view) { return ToNdc(oblique.Value(), view).z; };
    CHECK(Near(depthAt(Vec3d(0.0, 0.0, 5.0)), 0.0, 1.0e-12));
    CHECK(Near(depthAt(Vec3d(1.0, -2.0, 5.0)), 0.0, 1.0e-12));
    CHECK(depthAt(Vec3d(0.0, 0.0, 4.0)) < 0.0); // 잘리는 쪽
    const double inside = depthAt(Vec3d(1.0, 1.0, 20.0));
    CHECK(inside > 0.0 && inside <= 1.0);
    // x·y 열과 w 열은 그대로다(z 열만 바뀐다).
    CHECK(oblique.Value().Column(0) == projection.Value().Column(0));
    CHECK(oblique.Value().Column(3) == projection.Value().Column(3));
    // 카메라가 보이는 쪽에 있으면 실패한다.
    CHECK(!TryMakeObliqueClipProjection(projection.Value(), Vec3d(0.0, 0.0, 1.0), -1.0,
                                        DepthRange::Standard));
    CHECK(!TryMakeObliqueClipProjection(projection.Value(), Vec3d::Zero(), 1.0,
                                        DepthRange::Standard));
}

TEST(MatrixFactory_ObliqueClipSupportsReversedDepth) {
    // 기본 Reversed 투영에 Standard 공식을 쓰면 보이는 쪽이 뒤집힌다(리뷰 재현 입력).
    auto projection = TryMakePerspective(0.6, 1.0, 1.0, 0.1, 100.0);
    REQUIRE(projection);
    auto oblique = TryMakeObliqueClipProjection(projection.Value(), Vec3d(0.0, 0.0, 1.0), 5.0,
                                                DepthRange::Reversed);
    REQUIRE(oblique);
    auto depthAt = [&](const Vec3d& view) { return ToNdc(oblique.Value(), view).z; };
    // Reversed: 새 near(평면 위)는 z/w = 1, 잘리는 쪽은 1보다 크고 보이는 쪽은 [0, 1)이다.
    CHECK(Near(depthAt(Vec3d(0.0, 0.0, 5.0)), 1.0, 1.0e-12));
    CHECK(Near(depthAt(Vec3d(0.3, -0.2, 5.0)), 1.0, 1.0e-12));
    CHECK(depthAt(Vec3d(0.0, 0.0, 4.0)) > 1.0);
    const double inside = depthAt(Vec3d(1.0, 1.0, 20.0));
    CHECK(inside >= 0.0 && inside < 1.0);
    const double far = depthAt(Vec3d(0.0, 0.0, 100.0));
    CHECK(far >= -1.0e-12 && far < 1.0);
    // 무작위 frustum 점: 평면 앞쪽은 [0, 1], 뒤쪽은 1 초과다.
    MathRandom random(0x0b11c0e5ull ^ CurrentSeed());
    for (int index = 0; index < 2000; ++index) {
        const double z = random.Range(0.2, 99.0);
        const double extent = z * 0.6;
        const Vec3d point{random.Range(-extent, extent), random.Range(-extent, extent), z};
        const double depth = depthAt(point);
        if (z > 5.0 + 1.0e-9) {
            CHECK(depth <= 1.0 + 1.0e-12);
        } else if (z < 5.0 - 1.0e-9) {
            CHECK(depth > 1.0);
        }
    }
}

TEST(MatrixFactory_ObliqueClipRejectsUnsupportedInputs) {
    auto standard = TryMakePerspective(0.6, 1.0, 1.0, 0.1, 100.0, DepthRange::Standard);
    auto reversed = TryMakePerspective(0.6, 1.0, 1.0, 0.1, 100.0, DepthRange::Reversed);
    REQUIRE(standard && reversed);
    // 행렬과 다른 depth 플래그는 보이는 쪽을 뒤집으므로 거부한다.
    CHECK(TryMakeObliqueClipProjection(standard.Value(), Vec3d::UnitZ(), 5.0, DepthRange::Reversed)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(TryMakeObliqueClipProjection(reversed.Value(), Vec3d::UnitZ(), 5.0, DepthRange::Standard)
              .Error() == ErrorCode::InvalidArgument);
    // far 평면 너머의 평면(재검토 재현: distance 200 > far 100)은 보이는 영역이 없다.
    CHECK(
        TryMakeObliqueClipProjection(standard.Value(), Vec3d::UnitZ(), 200.0, DepthRange::Standard)
            .Error() == ErrorCode::InvalidArgument);
    CHECK(
        TryMakeObliqueClipProjection(reversed.Value(), Vec3d::UnitZ(), 200.0, DepthRange::Reversed)
            .Error() == ErrorCode::InvalidArgument);
    // 무한 원거리 Reversed 투영은 지원한다.
    auto infinite = TryMakePerspective(0.6, 1.0, 1.0, 0.1);
    REQUIRE(infinite);
    CHECK(TryMakeObliqueClipProjection(infinite.Value(), Vec3d::UnitZ(), 200.0,
                                       DepthRange::Reversed));
    // 직교 투영은 지원하지 않는다.
    auto ortho = TryMakeOrthographic(5.0, 5.0, 0.0, 100.0, DepthRange::Standard);
    REQUIRE(ortho);
    CHECK(TryMakeObliqueClipProjection(ortho.Value(), Vec3d::UnitZ(), 5.0, DepthRange::Standard)
              .Error() == ErrorCode::Unsupported);
}

TEST(MatrixFactory_LookFromStaysOrthonormalNearThreshold) {
    // 재검토 재현 입력(float): 기준 바로 위의 각에서 축 사이 내적이 0.0043이었다.
    const Vec3 forward(-0x1.ce63dp-2f, -0x1.290d8p-1f, 0x1.5b1012p-1f);
    const Vec3 up(-0x1.ce640ep-2f, -0x1.290c7ap-1f, 0x1.5b10dep-1f);
    auto view = TryMakeLookFrom(Vec3::Zero(), forward, up);
    REQUIRE(view);
    // view 축은 열 0·1·2다(행 벡터 규약).
    const Mat4& m = view.Value();
    const Vec3 x = m.Column(0).Xyz();
    const Vec3 y = m.Column(1).Xyz();
    const Vec3 z = m.Column(2).Xyz();
    CHECK(math::Abs(Dot(x, y)) <= 1.0e-6f && math::Abs(Dot(x, z)) <= 1.0e-6f &&
          math::Abs(Dot(y, z)) <= 1.0e-6f);
    CHECK(x.IsNormalized(1.0e-6f) && y.IsNormalized(1.0e-6f) && z.IsNormalized(1.0e-6f));
    CHECK(Dot(y, up) > 0.0f); // 위쪽 방향은 유지한다.
}

TEST(MatrixFactory_PerspectiveAcceptsHugeFarPlane) {
    // n * f가 float 범위를 넘어도 n * (f / (f - n))은 유한하다.
    auto projection = TryMakePerspective(0.5f, 1.0f, 1.0f, 10.0f, 1.0e38f);
    REQUIRE(projection);
    auto standard = TryMakePerspective(0.5f, 1.0f, 1.0f, 10.0f, 1.0e38f, DepthRange::Standard);
    REQUIRE(standard);
}

TEST(MathBatch_TransformPointsMatchesPerElement) {
    const Mat4 matrix = MakeScaleRotationTranslation(
        Vec3(2.0f, 1.0f, 0.5f), Quat::FromUnitAxisAngle(Vec3::Up(), 0.3f), Vec3(1.0f, -2.0f, 3.0f));
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
} // namespace math_matrix_tests_file

} // namespace rocks::test
