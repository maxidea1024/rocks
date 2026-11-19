#include <TestSupport/Test.h>
#include <TestSupport/MathTestSupport.h>
#include <Core/Math/MatrixFactory.h>
#include <Core/Math/Transform.h>

// 행렬·Transform·view·투영 규약의 UE 5.8 golden 시험이다(좌표계 UE 전환 C2). 기대값은 UE 5.8.4
// 원본 식을 그대로 옮긴 독립 C++ probe(x64 Windows clang-cl /O2, -ffp-contract=off, double)의
// 출력을 17자리로 옮긴 것이다. 옮긴 UE 식:
// VectorMatrixMultiply·VectorTransformVector(UnrealMathFPU.h: 1538-1564·:1619-1631),
// FQuatRotationTranslationMatrix(QuatRotationTranslationMatrix.h:76-91), TQuat(const
// TMatrix&)(Quat.h:775-847), TTransform::Multiply·TransformPosition·ToMatrixWithScale
// (TransformVectorized.cpp:295-345, TransformVectorized.h:303-345·:1675-1690),
// FInverseRotationMatrix (InverseRotationMatrix.h:35-51)·view
// swizzle(LocalPlayer.cpp:1241-1245)·WorldToView(SceneView.cpp: 684),
// FReversedZPerspectiveMatrix·FPerspectiveMatrix(PerspectiveMatrix.h:101-153),
// FReversedZOrthoMatrix·FOrthoMatrix(OrthoMatrix.h:53-72), FLookFromMatrix(Matrix.inl:957-976),
// FMirrorMatrix(MirrorMatrix.h:36-46).
// 정확 일치(==)는 Rocks가 UE와 같은 식·같은 연산 순서로 계산하는 항목(정수 행렬 곱, quaternion →
// 행렬, 투영의 1/tan·깊이 scale, 직교, 회전 없는 view 축)에만 쓴다. 허용 오차는 Rocks가 다른 경로로
// 같은 값을 계산하는 항목(행렬 곱의 분해로 하는 Transform 합성, quaternion 경유 view 회전,
// Gram-Schmidt를 더한 LookFrom, 법선 정규화를 하는 Mirror, overflow를 피하는 Reversed 깊이
// offset)에 반올림 수준으로 둔다.
namespace rocks::test {
namespace math_matrix_transform_golden_tests_file {

namespace {

struct Row4 {
    double values[4];
};

Mat4d FromUnrealRows(const Row4 (&rows)[4]) {
    Mat4d result;
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            result.At(row, column) = rows[row].values[column];
        }
    }

    return result;
}

// 성분 차이를 q와 -q 중 가까운 쪽으로 잰다.
bool NearRotation(const Quatd& actual, const Quatd& expected, double tolerance) {
    const double sign = Dot(actual, expected) < 0.0 ? -1.0 : 1.0;
    return Near(actual.x, sign * expected.x, tolerance) &&
           Near(actual.y, sign * expected.y, tolerance) &&
           Near(actual.z, sign * expected.z, tolerance) &&
           Near(actual.w, sign * expected.w, tolerance);
}

} // namespace

TEST(MatrixGolden_CompositionOrderAndRowVectorMatchUnreal) {
    // UE (T * S).TransformPosition(1, 0, 0) = 22(T 먼저), (S * T) = 12. 정확 일치.
    const Mat4d translate = Mat4d::Translation(Vec3d(10.0, 0.0, 0.0));
    const Mat4d scale = Mat4d::Scale(Vec3d(2.0, 2.0, 2.0));
    CHECK((translate * scale).TransformPoint(Vec3d(1.0, 0.0, 0.0)) == Vec3d(22.0, 0.0, 0.0));
    CHECK((scale * translate).TransformPoint(Vec3d(1.0, 0.0, 0.0)) == Vec3d(12.0, 0.0, 0.0));
    // 정수 원소 행렬의 A * B와 행 벡터 곱(UE VectorMatrixMultiply·VectorTransformVector). float
    // SIMD 경로도 정수라 정확하다.
    const Row4 a[4] = {{{2, 3, 5, 7}}, {{11, 13, 17, 19}}, {{23, 29, 31, 37}}, {{41, 43, 47, 53}}};
    const Row4 b[4] = {
        {{59, 61, 67, 71}}, {{73, 79, 83, 89}}, {{97, 101, 103, 107}}, {{109, 113, 127, 131}}};
    const Row4 product[4] = {{{1585, 1655, 1787, 1861}},
                             {{5318, 5562, 5980, 6246}},
                             {{10514, 11006, 11840, 12378}},
                             {{15894, 16634, 17888, 18710}}};
    CHECK(FromUnrealRows(a) * FromUnrealRows(b) == FromUnrealRows(product));
    CHECK(Mat4(FromUnrealRows(a)) * Mat4(FromUnrealRows(b)) == Mat4(FromUnrealRows(product)));
    CHECK(Vec4d(1.0, 2.0, 3.0, 4.0) * FromUnrealRows(a) == Vec4d(257.0, 288.0, 320.0, 368.0));
    CHECK(Vec4(1.0f, 2.0f, 3.0f, 4.0f) * Mat4(FromUnrealRows(a)) ==
          Vec4(257.0f, 288.0f, 320.0f, 368.0f));
    // 메모리 배치도 UE와 같다: M[row][column] = m[row * 4 + column], 이동은 m[12..14].
    CHECK(FromUnrealRows(a).m[1] == 3.0 && FromUnrealRows(a).m[4] == 11.0);
}

TEST(MatrixGolden_QuaternionMatrixMatchesUnreal) {
    // q = UE FRotator(30, 45, 60).Quaternion(). UE FQuatRotationTranslationMatrix(q, (1, 2, 3))와
    // 원소가 정확히 같다(2를 곱하는 위치만 다르고 2배는 정확하다).
    const Quatd q(-0.36042340565035591, -0.39190383732911993, 0.20056212114657512,
                  0.82236317190599928);
    const Row4 expected[4] = {{{0.61237243569579447, 0.61237243569579469, 0.49999999999999989, 0}},
                              {{-0.047367172745376662, 0.65973960844117108, -0.75, 0}},
                              {{-0.78914913099243134, 0.43559574039915749, 0.43301270189221941, 0}},
                              {{1, 2, 3, 1}}};
    CHECK(MakeRotationTranslation(q, Vec3d(1.0, 2.0, 3.0)) == FromUnrealRows(expected));
    // 행 0은 회전된 X축(GetForwardVector)이다.
    CHECK(q.ToMatrix().GetScaledAxis(0).Equals(q.GetForwardVector(), 1.0e-15));
    // 행렬 → quaternion: UE TQuat(FMatrix) 결과와 성분 1e-15 안(대각합 분기·나눗셈 방식이 달라
    // 마지막 비트가 다를 수 있다).
    auto recovered = Quatd::TryFromRotationMatrix(FromUnrealRows(expected));
    REQUIRE(recovered);
    CHECK(NearRotation(
        recovered.Value(),
        Quatd(-0.36042340565035591, -0.39190383732911988, 0.20056212114657512, 0.82236317190599928),
        1.0e-15));
    // 대각합 음수 분기(UE FRotator(10, 170, -175)). UE는 w < 0을 그대로 두고 Rocks는 w >= 0으로
    // 맞춘다(같은 회전).
    const Quatd turned(0.090528665103007833, 0.99112798966120974, 0.050876942779673764,
                       -0.082954237976069364);
    const Row4 turnedMatrix[4] = {
        {{-0.96984631039295466, 0.17101007166283427, 0.17364817766693039, 0}},
        {{0.18789190373819398, 0.97843219497612255, 0.085831651177431328, 0}},
        {{-0.15522489080946647, 0.11587059689187451, -0.98106026219040721, 0}},
        {{0, 0, 0, 1}}};
    CHECK(turned.ToMatrix() == FromUnrealRows(turnedMatrix));
    auto turnedBack = Quatd::TryFromRotationMatrix(FromUnrealRows(turnedMatrix));
    REQUIRE(turnedBack);
    CHECK(turnedBack.Value().w >= 0.0);
    CHECK(NearRotation(turnedBack.Value(),
                       Quatd(0.090528665103007833, 0.99112798966120974, 0.050876942779673764,
                             -0.082954237976069378),
                       1.0e-15));
}

TEST(TransformGolden_ChildTimesParentMatchesUnreal) {
    // UE SceneComponent: NewCompToWorld = NewRelativeTransform * ParentToWorld(SceneComponent.cpp:
    // 732). 자식 (100, 0, 0) · 부모 yaw 90°·이동 (0, 0, 50)·scale 2 → 원점 (0, 200, 50).
    const Quatd yaw90(0.0, -0.0, 0.70710678118654757, 0.70710678118654757);
    const Transform parent(yaw90, Vec3d(0.0, 0.0, 50.0), Vec3d::Splat(2.0));
    const Transform child(Quatd::Identity(), Vec3d(100.0, 0.0, 0.0), Vec3d::One());
    auto world = Transform::TryMultiply(child, parent);
    REQUIRE(world);
    CHECK(world.Value()
              .TransformPoint(Vec3d::Zero())
              .Equals(Vec3d(-2.8421709430404007e-14, 200.00000000000003, 50.0), 1.0e-12));
    CHECK(NearRotation(world.Value().rotation, yaw90, 1.0e-15));
    CHECK(world.Value().scale.Equals(Vec3d::Splat(2.0), 1.0e-14));
    // 순서를 바꾸면(UE Parent * Child) 원점은 (100, 0, 50)이다.
    auto swapped = Transform::TryMultiply(parent, child);
    REQUIRE(swapped);
    CHECK(swapped.Value().TransformPoint(Vec3d::Zero()).Equals(Vec3d(100.0, 0.0, 50.0), 1.0e-12));
    // 행렬 경로도 같은 순서다(UE LocalToWorld 행렬 = Local * Parent).
    CHECK((child.ToMatrix() * parent.ToMatrix())
              .TransformPoint(Vec3d::Zero())
              .Equals(Vec3d(0.0, 200.0, 50.0), 1.0e-12));

    // 임의 회전·비균일 자식 scale·균일 부모 scale(shear 없음): UE Multiply 성분 식 결과와 같다.
    const Transform first(
        Quatd(-0.23929833774473031, -0.12767944069578063, 0.14487812541736916, 0.95154852464378858),
        Vec3d(1.0, 2.0, 3.0), Vec3d(0.5, 1.5, 2.0));
    const Transform second(
        Quatd(-0.57094351502289731, 0.20741790554001355, 0.29496907738688466, 0.73755986773971272),
        Vec3d(-7.0, 11.0, 5.0), Vec3d::Splat(3.0));
    auto composed = Transform::TryMultiply(first, second);
    REQUIRE(composed);
    CHECK(NearRotation(
        composed.Value().rotation,
        Quatd(-0.65206550555637122, 0.11532858687958683, 0.51006619010546717, 0.54894660507481496),
        1.0e-14));
    CHECK(composed.Value().translation.Equals(
        Vec3d(-9.0896448661971849, 21.320165676128649, 1.110582980647127), 1.0e-13));
    CHECK(composed.Value().scale.Equals(Vec3d(1.5, 4.5, 6.0), 1.0e-14));
    CHECK(composed.Value()
              .TransformPoint(Vec3d(0.3, -2.0, 7.0))
              .Equals(Vec3d(-25.112285984310553, 59.849954320528326, 11.305329599275385), 1.0e-12));
    // ToMatrix = UE ToMatrixWithScale(행 0·1·2에 scale x·y·z, 마지막 행 이동). Rocks는 회전을 다시
    // 정규화하므로 1e-15 안이다.
    const Row4 firstMatrix[4] = {
        {{0.46270828919916168, 0.16841204441673258, 0.086824088833465152, 0}},
        {{-0.32191526571413465, 1.2652399029844741, -0.73860581475915599, 0}},
        {{-0.62464911203745266, 0.83682408883346515, 1.7057370639048866, 0}},
        {{1, 2, 3, 1}}};
    CHECK(first.ToMatrix().Equals(FromUnrealRows(firstMatrix), 1.0e-15));
    const Row4 productMatrix[4] = {
        {{0.67959539627895316, 0.6143919303393055, -1.1877174130618755, 0}},
        {{-3.1968080705752304, -1.6682124764267312, -2.6921153937748024, 0}},
        {{-3.2314479825925027, 5.0012923017777728, 0.73811872214246166, 0}},
        {{-9.0896448661971867, 21.320165676128649, 1.1105829806471266, 1}}};
    CHECK((first.ToMatrix() * second.ToMatrix()).Equals(FromUnrealRows(productMatrix), 1.0e-13));
    // Transform3f도 같은 순서다(float 반올림 안).
    auto single = Transform3f::TryMultiply(child.RoundToTransform3f(), parent.RoundToTransform3f());
    REQUIRE(single);
    CHECK(single.Value().TransformPoint(Vec3::Zero()).Equals(Vec3(0.0f, 200.0f, 50.0f), 1.0e-4f));
}

TEST(ViewGolden_WorldToViewMatchesUnreal) {
    // UE WorldToView = T(-Origin) * FInverseRotationMatrix(Rot) * swizzle. 카메라 (100, 200, 300),
    // FRotator(10, 30, 20). Rocks는 quaternion 경유라 원소 1e-15(회전)·1e-13(이동) 안이다.
    const Row4 expected[4] = {
        {{-0.41841204441673258, -0.31232455601872633, 0.85286853195244328, 0}},
        {{0.84349326865631613, 0.21461017714275646, 0.49240387650610395, 0}},
        {{-0.33682408883346515, 0.92541657839832336, 0.17364817766693033, 0}},
        {{-25.810222639550418, -289.31455334617567, -235.86208179654423, 1}}};
    const Mat4d view =
        MakeWorldToView(Vec3d(100.0, 200.0, 300.0), Rotator<double>(10.0, 30.0, 20.0));
    const Mat4d unreal = FromUnrealRows(expected);
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 4; ++column) {
            CHECK(Near(view.At(row, column), unreal.At(row, column), 1.0e-15));
        }
    }

    CHECK(view.Row(3).Equals(unreal.Row(3), 1.0e-12));
    CHECK(
        view.TransformPoint(Vec3d(150.0, -20.0, 310.0))
            .Equals(Vec3d(-209.8573622135608, -53.576300988359492, -63.948944457051425), 1.0e-12));
    // UE FInverseRotationMatrix(10, 30, 20) = Rocks MakeInverseRotation(같은 회전의 quaternion).
    const Row4 inverseRotation[4] = {
        {{0.85286853195244328, -0.41841204441673258, -0.31232455601872633, 0}},
        {{0.49240387650610395, 0.84349326865631613, 0.21461017714275646, 0}},
        {{0.17364817766693033, -0.33682408883346515, 0.92541657839832336, 0}},
        {{0, 0, 0, 1}}};
    const Quatd camera(-0.14487812541736914, -0.12767944069578063, 0.23929833774473031,
                       0.95154852464378847);
    CHECK(MakeInverseRotation(camera).Equals(FromUnrealRows(inverseRotation), 1.0e-15));
}

TEST(ProjectionGolden_PerspectiveAndOrthoMatchUnreal) {
    // UE FReversedZPerspectiveMatrix(π/4, 16, 9, 10, 10000): 1/tan·W/tan/H·n/(n-f)는 UE와 같은
    // 식이라 정확히 같고, 깊이 offset(UE -f·n/(n-f), Rocks n·(f/(f-n)))만 반올림 수준에서 다를 수
    // 있다.
    const double quarterPi = math::Pi<double> / 4.0;
    auto reversed = TryMakePerspective(quarterPi, 16.0, 9.0, 10.0, 10000.0);
    REQUIRE(reversed);
    const Mat4d& m = reversed.Value();
    CHECK(m.At(0, 0) == 1.0000000000000002 && m.At(1, 1) == 1.7777777777777781);
    CHECK(m.At(2, 2) == -0.001001001001001001 && m.At(2, 3) == 1.0 && m.At(3, 3) == 0.0);
    CHECK(Near(m.At(3, 2), 10.01001001001001, 2.0e-15));
    CHECK(m.At(0, 1) == 0.0 && m.At(2, 0) == 0.0 && m.At(3, 0) == 0.0 && m.At(3, 1) == 0.0);
    // 무한 원거리(UE 4인자 생성자 — 카메라 기본): 모든 원소가 정확히 같다.
    const Row4 infiniteRows[4] = {{{1.0000000000000002, 0, 0, 0}},
                                  {{0, 1.7777777777777781, 0, 0}},
                                  {{0, 0, 0, 1}},
                                  {{0, 0, 10, 0}}};
    auto infinite = TryMakePerspective(quarterPi, 16.0, 9.0, 10.0);
    REQUIRE(infinite);
    CHECK(infinite.Value() == FromUnrealRows(infiniteRows));
    // UE FPerspectiveMatrix(π/4, 16, 9, 10, 10000): 같은 식이라 정확히 같다.
    const Row4 standardRows[4] = {{{1.0000000000000002, 0, 0, 0}},
                                  {{0, 1.7777777777777781, 0, 0}},
                                  {{0, 0, 1.0010010010010011, 1}},
                                  {{0, 0, -10.01001001001001, 0}}};
    auto standard = TryMakePerspective(quarterPi, 16.0, 9.0, 10.0, 10000.0, DepthRange::Standard);
    REQUIRE(standard);
    CHECK(standard.Value() == FromUnrealRows(standardRows));
    // 다른 반화각·화면 크기(UE FReversedZPerspectiveMatrix(0.6, 1920, 1080, 10, 5000)).
    auto wide = TryMakePerspective(0.6, 1920.0, 1080.0, 10.0, 5000.0);
    REQUIRE(wide);
    CHECK(wide.Value().At(0, 0) == 1.4616959470781021 &&
          wide.Value().At(1, 1) == 2.5985705725832928);
    CHECK(wide.Value().At(2, 2) == -0.002004008016032064);
    CHECK(Near(wide.Value().At(3, 2), 10.020040080160321, 2.0e-15));

    // UE FReversedZOrthoMatrix(800, 450, 1/(20000-10), -10)·FOrthoMatrix — 카메라와 같은 인자로
    // 같은 식을 쓰므로 정확히 같다.
    const Row4 reversedOrthoRows[4] = {{{0.00125, 0, 0, 0}},
                                       {{0, 0.0022222222222222222, 0, 0}},
                                       {{0, 0, -5.002501250625313e-05, 0}},
                                       {{0, 0, 1.0005002501250626, 1}}};
    const Row4 standardOrthoRows[4] = {{{0.00125, 0, 0, 0}},
                                       {{0, 0.0022222222222222222, 0, 0}},
                                       {{0, 0, 5.002501250625313e-05, 0}},
                                       {{0, 0, -0.00050025012506253134, 1}}};
    auto reversedOrtho = TryMakeOrthographic(800.0, 450.0, 10.0, 20000.0);
    auto standardOrtho = TryMakeOrthographic(800.0, 450.0, 10.0, 20000.0, DepthRange::Standard);
    REQUIRE(reversedOrtho && standardOrtho);
    CHECK(reversedOrtho.Value() == FromUnrealRows(reversedOrthoRows));
    CHECK(standardOrtho.Value() == FromUnrealRows(standardOrthoRows));
}

TEST(ViewGolden_LookFromAndMirrorMatchUnreal) {
    // UE FLookFromMatrix((100, -50, 30), (1, 2, 0.5), (0, 0, 1)). Rocks는 X축을 한 번 더
    // 직교화하므로 축 원소 1e-15, 이동 1e-13 안이다.
    const Row4 lookRows[4] = {
        {{-0.89442719099991586, -0.097590007294853315, 0.43643578047198478, 0}},
        {{0.44721359549995793, -0.19518001458970663, 0.87287156094396956, 0}},
        {{0, 0.9759000729485332, 0.21821789023599239, 0}},
        {{111.80339887498948, -29.277002188455995, -6.5465367070797713, 1}}};
    auto look = TryMakeLookFrom(Vec3d(100.0, -50.0, 30.0), Vec3d(1.0, 2.0, 0.5), Vec3d::Up());
    REQUIRE(look);
    const Mat4d unrealLook = FromUnrealRows(lookRows);
    for (int row = 0; row < 3; ++row) {
        CHECK(look.Value().Row(row).Equals(unrealLook.Row(row), 1.0e-15));
    }

    CHECK(look.Value().Row(3).Equals(unrealLook.Row(3), 1.0e-13));
    // UE FMirrorMatrix(FPlane(0, 0.6, 0.8, 5)): 3x3 대칭 I - 2nnᵀ, 이동 행 2dn. Rocks는 법선을
    // 정규화하므로 1e-15 안이다.
    const Row4 mirrorRows[4] = {{{1, -0.0, -0.0, 0}},
                                {{-0.0, 0.28000000000000003, -0.95999999999999996, 0}},
                                {{-0.0, -0.95999999999999996, -0.28000000000000025, 0}},
                                {{0, 6, 8, 1}}};
    auto mirror = TryMakeMirror(Vec3d(0.0, 0.6, 0.8), 5.0);
    REQUIRE(mirror);
    const Mat4d unrealMirror = FromUnrealRows(mirrorRows);
    for (int row = 0; row < 3; ++row) {
        CHECK(mirror.Value().Row(row).Equals(unrealMirror.Row(row), 1.0e-15));
    }

    // 이동 행 2dn(크기 8)은 정규화한 법선·거리의 반올림이 몇 ulp(8 근처 1 ulp = 1.8e-15) 남는다.
    CHECK(mirror.Value().Row(3).Equals(unrealMirror.Row(3), 1.0e-14));
}
} // namespace math_matrix_transform_golden_tests_file

} // namespace rocks::test
