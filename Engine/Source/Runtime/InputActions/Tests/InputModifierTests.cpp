#include <Base/InitializerList.h>
#include <Core/Math/Easing.h>
#include <Core/Math/InterpTo.h>
#include <Core/Memory/SystemAllocator.h>
#include <InputActions/InputModifier.h>
#include <TestSupport/Test.h>
#include <math.h>
#include <stdio.h>

// 입력 액션 모디파이어 시험이다(Docs/Design/InputActions 3.1절 — IA1): 종류마다 UE 식 대조, 타입
// 되돌림, Boolean 통과, 상태가 있는 모디파이어.
using namespace rocks;
using namespace rocks::input;
namespace input_modifier_tests_file {

namespace {

constexpr double ModifierTolerance = 1.0e-6;

bool NearModifier(double actual, double expected) noexcept {
    const bool same = fabs(actual - expected) <= ModifierTolerance;
    if (!same) {
        printf("  value %.9g, expected %.9g\n", actual, expected);
    }

    return same;
}

InputValue Apply(const InputModifier& modifier, const InputValue& value, double dt = 0.0,
                 float fov = 0.0f) noexcept {
    InputModifierState state;
    return ApplyInputModifier(modifier, state, value, ModifierContext{dt, fov});
}

} // namespace

// DeadZone: Axial·Radial·UnscaledRadial(UE InputModifiers.cpp:188-243).
TEST(InputModifier_DeadZone) {
    SystemAllocator allocator;
    InputModifier dead(allocator);
    dead.kind = InputModifierKind::DeadZone;
    dead.lowerThreshold = 0.2f;
    dead.upperThreshold = 0.8f;
    // 1D Radial: f(0.5) = (0.5 − 0.2) / 0.6 = 0.5, 아래는 0, 위는 1, 부호 유지.
    CHECK(NearModifier(Apply(dead, InputValue(InputValueType::Axis1D, Vec3d(0.5, 0, 0))).value.x,
                       0.5));
    CHECK(Apply(dead, InputValue(InputValueType::Axis1D, Vec3d(0.1, 0, 0))).value.x == 0.0);
    CHECK(NearModifier(Apply(dead, InputValue(InputValueType::Axis1D, Vec3d(-0.95, 0, 0))).value.x,
                       -1.0));
    // 2D Radial: 크기 0.5인 (0.3, 0.4) → 방향 유지, 크기 0.5.
    const InputValue radial = Apply(dead, InputValue(InputValueType::Axis2D, Vec3d(0.3, 0.4, 0)));
    CHECK(NearModifier(radial.value.x, 0.3) && NearModifier(radial.value.y, 0.4));
    // 2D Axial: 성분마다 — x 0.3 → 1/6, y 0.4 → 1/3.
    dead.deadZoneType = DeadZoneType::Axial;
    const InputValue axial = Apply(dead, InputValue(InputValueType::Axis2D, Vec3d(0.3, 0.4, 0)));
    CHECK(NearModifier(axial.value.x, 0.1 / 0.6) && NearModifier(axial.value.y, 0.2 / 0.6));
    // UnscaledRadial: L 아래 0, 위는 min(|a|, U) 그대로.
    dead.deadZoneType = DeadZoneType::UnscaledRadial;
    CHECK(NearModifier(Apply(dead, InputValue(InputValueType::Axis1D, Vec3d(0.5, 0, 0))).value.x,
                       0.5));
    CHECK(NearModifier(Apply(dead, InputValue(InputValueType::Axis1D, Vec3d(0.9, 0, 0))).value.x,
                       0.8));
    CHECK(Apply(dead, InputValue(InputValueType::Axis1D, Vec3d(0.19, 0, 0))).value.x == 0.0);
    // 3D Radial.
    dead.deadZoneType = DeadZoneType::Radial;
    const InputValue spatial =
        Apply(dead, InputValue(InputValueType::Axis3D, Vec3d(0.0, 0.0, 0.65)));
    CHECK(NearModifier(spatial.value.z, 0.75));
    // Boolean은 그대로다.
    CHECK(Apply(dead, InputValue(InputValueType::Boolean, Vec3d(0.1, 0, 0))).value.x ==
          static_cast<double>(0.1));
    // 0벡터는 안전 정규화로 0이다(NaN 없음).
    const InputValue zero = Apply(dead, InputValue(InputValueType::Axis2D, Vec3d()));
    CHECK(zero.value == Vec3d());
    // lower ≥ upper는 검증기가 거부한다. 적용에 오면 값을 그대로 둔다(0/0 NaN 방지 — 2.6절).
    dead.lowerThreshold = 0.5f;
    dead.upperThreshold = 0.5f;
    CHECK(Apply(dead, InputValue(InputValueType::Axis1D, Vec3d(0.3, 0, 0))).value.x == 0.3);
    dead.deadZoneType = DeadZoneType::Axial;
    CHECK(Apply(dead, InputValue(InputValueType::Axis2D, Vec3d(0.3, 0.1, 0))).value ==
          Vec3d(0.3, 0.1, 0));
}

// Scalar·ScaleByDeltaTime·Negate·Swizzle·ToWorldSpace와 타입 되돌림.
TEST(InputModifier_LinearKinds) {
    SystemAllocator allocator;
    InputModifier modifier(allocator);
    modifier.kind = InputModifierKind::Scalar;
    modifier.scalar = Vec3d(2.0, 3.0, 4.0);
    const InputValue scaled = Apply(modifier, InputValue(InputValueType::Axis2D, Vec3d(1, 1, 1)));
    CHECK(scaled.value.x == 2.0 && scaled.value.y == 3.0 && scaled.value.z == 0.0);
    CHECK(Apply(modifier, InputValue::FromBool(true)).value.x == 1.0);

    modifier.kind = InputModifierKind::ScaleByDeltaTime;
    CHECK(NearModifier(
        Apply(modifier, InputValue(InputValueType::Axis1D, Vec3d(4, 0, 0)), 0.25).value.x, 1.0));
    CHECK(Apply(modifier, InputValue::FromBool(true), 0.25).value.x == 1.0);

    modifier.kind = InputModifierKind::Negate;
    modifier.negateY = false;
    const InputValue negated = Apply(modifier, InputValue(InputValueType::Axis3D, Vec3d(1, 2, 3)));
    CHECK(negated.value.x == -1.0 && negated.value.y == 2.0 && negated.value.z == -3.0);
    // Negate는 Boolean도 뒤집는다(UE는 막지 않는다).
    CHECK(Apply(modifier, InputValue::FromBool(true)).value.x == -1.0);

    modifier.kind = InputModifierKind::Swizzle;
    const InputValue axis3(InputValueType::Axis3D, Vec3d(1, 2, 3));
    const struct {
        SwizzleOrder order;
        Vec3d expected;
    } orders[] = {{SwizzleOrder::YXZ, Vec3d(2, 1, 3)},
                  {SwizzleOrder::ZYX, Vec3d(3, 2, 1)},
                  {SwizzleOrder::XZY, Vec3d(1, 3, 2)},
                  {SwizzleOrder::YZX, Vec3d(2, 3, 1)},
                  {SwizzleOrder::ZXY, Vec3d(3, 1, 2)}};
    for (const auto& entry : orders) {
        modifier.swizzle = entry.order;
        CHECK(Apply(modifier, axis3).value == entry.expected);
    }

    // 1D 값에 YXZ: x가 y로 가고 타입 되돌림이 y를 지운다(UE와 같다 — 1D를 2D 축으로 보내려면 액션
    // 타입을 2D로).
    modifier.swizzle = SwizzleOrder::YXZ;
    CHECK(Apply(modifier, InputValue(InputValueType::Axis1D, Vec3d(1, 0, 0))).value == Vec3d());
    const InputValue planar = Apply(modifier, InputValue(InputValueType::Axis2D, Vec3d(1, 0, 0)));
    CHECK(planar.value == Vec3d(0, 1, 0));

    modifier.kind = InputModifierKind::ToWorldSpace;
    CHECK(Apply(modifier, InputValue(InputValueType::Axis2D, Vec3d(1, 2, 0))).value ==
          Vec3d(2, 1, 0));
    CHECK(Apply(modifier, axis3).value == Vec3d(3, 1, 2));
    CHECK(Apply(modifier, InputValue(InputValueType::Axis1D, Vec3d(5, 0, 0))).value ==
          Vec3d(5, 0, 0));
}

// 응답 커브(지수·사용자)와 FOV 배율.
TEST(InputModifier_ResponseCurvesAndFov) {
    SystemAllocator allocator;
    InputModifier modifier(allocator);
    modifier.kind = InputModifierKind::ResponseCurveExponential;
    modifier.curveExponent = Vec3d(2.0, 3.0, 2.0);
    const InputValue curved =
        Apply(modifier, InputValue(InputValueType::Axis2D, Vec3d(-0.5, 0.5, 0)));
    CHECK(NearModifier(curved.value.x, -0.25) && NearModifier(curved.value.y, 0.125));
    // Boolean은 그대로다.
    CHECK(Apply(modifier, InputValue(InputValueType::Boolean, Vec3d(0.5, 0, 0))).value.x == 0.5);

    // 사용자 커브: 키가 없으면 0(UE null 커브), 선형 키 (0, 0)–(1, 2)면 두 배.
    modifier.kind = InputModifierKind::ResponseCurveUser;
    CHECK(Apply(modifier, InputValue(InputValueType::Axis1D, Vec3d(0.5, 0, 0))).value.x == 0.0);
    REQUIRE(modifier.responseX.TryAddKey(0.0f, 0.0f));
    REQUIRE(modifier.responseX.TryAddKey(1.0f, 2.0f));
    REQUIRE(modifier.responseX.TrySetKeyInterp(0, CurveInterp::Linear));
    CHECK(NearModifier(
        Apply(modifier, InputValue(InputValueType::Axis1D, Vec3d(0.5, 0, 0))).value.x, 1.0));
    // 2D면 y 커브도 본다(없으면 0).
    const InputValue user =
        Apply(modifier, InputValue(InputValueType::Axis2D, Vec3d(0.25, 0.9, 0)));
    CHECK(NearModifier(user.value.x, 0.5) && user.value.y == 0.0);
    // 복사.
    InputModifier copy(allocator);
    REQUIRE(copy.TryCopyFrom(modifier));
    CHECK(copy.kind == InputModifierKind::ResponseCurveUser && copy.responseX.KeyCount() == 2);

    // FOV(UE cpp:391-419): 플레이어 없음이면 그대로, 카메라 없음(fov 0)이면 fovScale만, 기준 80°면
    // 배율 fovScale, 90°면 fovScale · tan45/tan40.
    modifier.kind = InputModifierKind::FovScaling;
    modifier.fovScale = 2.0f;
    const InputValue mouse(InputValueType::Axis2D, Vec3d(1.0, -1.0, 0));
    InputModifierState fovState;
    const auto withPlayer = [&](float fov) noexcept {
        return ApplyInputModifier(modifier, fovState, mouse, ModifierContext{0.0, fov, true});
    };

    CHECK(NearModifier(withPlayer(80.0f).value.x, 2.0));
    const double expected = 2.0 * tan(45.0 * 3.14159265358979323846 / 180.0) /
                            tan(40.0 * 3.14159265358979323846 / 180.0);
    CHECK(fabs(withPlayer(90.0f).value.x - expected) < 1.0e-5);
    CHECK(NearModifier(withPlayer(0.0f).value.y, -2.0));
    CHECK(Apply(modifier, mouse, 0.0, 90.0f).value == mouse.value);
}

// 상태가 있는 모디파이어: SmoothDelta(정규화한 변화 방향의 보간)·Smooth(UE와 같은 동작).
TEST(InputModifier_StatefulKinds) {
    SystemAllocator allocator;
    InputModifier modifier(allocator);
    modifier.kind = InputModifierKind::SmoothDelta;
    InputModifierState state;
    const ModifierContext context{0.1, 0.0f};
    // (0,0) → (3,4): 변화 방향 (0.6, 0.8)의 Lerp 0.5 → (0.3, 0.4).
    InputValue out = ApplyInputModifier(
        modifier, state, InputValue(InputValueType::Axis2D, Vec3d(3, 4, 0)), context);
    CHECK(NearModifier(out.value.x, 0.3) && NearModifier(out.value.y, 0.4));
    // 같은 값: 변화 0 → 목표 0, Lerp 0.5 → (0.15, 0.2).
    out = ApplyInputModifier(modifier, state, InputValue(InputValueType::Axis2D, Vec3d(3, 4, 0)),
                             context);
    CHECK(NearModifier(out.value.x, 0.15) && NearModifier(out.value.y, 0.2));
    // InterpConstantTo speed 2·dt 0.1 → 0.2만큼 다가간다.
    modifier.smoothMethod = SmoothDeltaMethod::InterpConstantTo;
    modifier.smoothSpeed = 2.0f;
    InputModifierState constant;
    out = ApplyInputModifier(modifier, constant, InputValue(InputValueType::Axis1D, Vec3d(5, 0, 0)),
                             context);
    CHECK(NearModifier(out.value.x, 0.2));
    // 감속식은 Core Easing과 같다(EaseIn 지수 2, alpha 0.5 → 0.25).
    modifier.smoothMethod = SmoothDeltaMethod::EaseIn;
    modifier.smoothSpeed = 0.5f;
    InputModifierState eased;
    out = ApplyInputModifier(modifier, eased, InputValue(InputValueType::Axis1D, Vec3d(5, 0, 0)),
                             context);
    CHECK(NearModifier(out.value.x, 0.25));
    // Boolean은 그대로다.
    CHECK(ApplyInputModifier(modifier, eased, InputValue::FromBool(true), context).value.x == 1.0);

    // Smooth: UE는 표본 수 0·평균 0에서 시작해 평균을 갱신하지 않으므로 값을 그대로 낸다(UE
    // UInputModifierSmooth와 같다). 긴 프레임(≥ 0.25 s)은 상태를 초기화한다.
    modifier.kind = InputModifierKind::Smooth;
    InputModifierState smooth;
    for (int frame = 0; frame < 3; ++frame) {
        out = ApplyInputModifier(modifier, smooth,
                                 InputValue(InputValueType::Axis2D, Vec3d(2, 0, 0)), context);
        CHECK(out.value.x == 2.0);
    }

    CHECK(smooth.samples == 0 && smooth.totalSampleTime == SmoothInitialSampleTime);
    smooth.samples = 4;
    out = ApplyInputModifier(modifier, smooth, InputValue(InputValueType::Axis2D, Vec3d(1, 0, 0)),
                             ModifierContext{0.3, 0.0f});
    CHECK(out.value.x == 1.0 && smooth.samples == 0);

    // 평균 분기(UE cpp:276-321): 평균이 있는 상태를 미리 채워 식을 대조한다.
    InputModifierState averaged;
    averaged.samples = 2;
    averaged.totalSampleTime = 0.2;
    averaged.averageValue = InputValue(InputValueType::Axis2D, Vec3d(1, 0, 0));
    const ModifierContext frame{0.05, 0.0f};
    // 표본 3·누적 0.25 → 간격 1/12. dt 0.05 < 2/12라 값 × 0.05 · 12 = 2 × 0.6.
    out = ApplyInputModifier(modifier, averaged, InputValue(InputValueType::Axis2D, Vec3d(2, 0, 0)),
                             frame);
    CHECK(NearModifier(out.value.x, 1.2) && averaged.samples == 3);
    CHECK(NearModifier(averaged.averageValue.value.x, 1.2));
    // 움직임 없음: 표본 4·누적 0.3 → 간격 0.075, 평균 × 0.05/0.075.
    const InputValue still(InputValueType::Axis2D, Vec3d());
    out = ApplyInputModifier(modifier, averaged, still, frame);
    CHECK(NearModifier(out.value.x, 1.2 * 0.05 / 0.075));
    // 표본 5·누적 0.35 → 간격 0.07, zeroTime 0.05 < 0.07이라 평균 × 0.05/0.07.
    out = ApplyInputModifier(modifier, averaged, still, frame);
    CHECK(NearModifier(out.value.x, 1.2 * 0.05 / 0.07));
    // 표본 6·누적 0.4 → 간격 1/15, zeroTime 0.1 ≥ 1/15이라 지운다.
    out = ApplyInputModifier(modifier, averaged, still, frame);
    CHECK(out.value == Vec3d() && averaged.samples == 0);
    CHECK(averaged.averageValue.value == Vec3d());
}

// SmoothDelta 15방식이 각자의 비율 함수에 연결된다: (0) → (5) 첫 평가의 결과는 Lerp(0, 1, alpha)다.
TEST(InputModifier_SmoothDeltaMethodTable) {
    SystemAllocator allocator;
    InputModifier modifier(allocator);
    modifier.kind = InputModifierKind::SmoothDelta;
    modifier.smoothSpeed = 0.5f;
    modifier.easingExponent = 3.0f;
    constexpr double dt = 0.1;
    const struct {
        SmoothDeltaMethod method;
        double expected;
    } methods[] = {
        {SmoothDeltaMethod::Lerp, 0.5},
        {SmoothDeltaMethod::InterpTo, math::VInterpTo(Vec3d(), Vec3d(1, 0, 0), dt, 0.5).x},
        {SmoothDeltaMethod::InterpConstantTo,
         math::VInterpConstantTo(Vec3d(), Vec3d(1, 0, 0), dt, 0.5).x},
        {SmoothDeltaMethod::CircularIn, 1.0 - sqrt(0.75)},
        {SmoothDeltaMethod::CircularOut, sqrt(0.75)},
        {SmoothDeltaMethod::CircularInOut, math::CircularInOutAlpha(0.5f)},
        {SmoothDeltaMethod::EaseIn, 0.125},
        {SmoothDeltaMethod::EaseOut, 0.875},
        {SmoothDeltaMethod::EaseInOut, math::EaseInOutAlpha(0.5f, 3.0f)},
        {SmoothDeltaMethod::ExpoIn, 0.03125},
        {SmoothDeltaMethod::ExpoOut, 0.96875},
        {SmoothDeltaMethod::ExpoInOut, math::ExpoInOutAlpha(0.5f)},
        {SmoothDeltaMethod::SinIn, 1.0 - cos(3.14159265358979323846 / 4.0)},
        {SmoothDeltaMethod::SinOut, sin(3.14159265358979323846 / 4.0)},
        {SmoothDeltaMethod::SinInOut, math::SinInOutAlpha(0.5f)},
    };
    uint32 checked = 0;
    for (const auto& entry : methods) {
        modifier.smoothMethod = entry.method;
        InputModifierState state;
        const InputValue out =
            ApplyInputModifier(modifier, state, InputValue(InputValueType::Axis1D, Vec3d(5, 0, 0)),
                               ModifierContext{dt, 0.0f});
        if (fabs(out.value.x - entry.expected) > 1.0e-5) {
            printf("  method %u: %.9g, expected %.9g\n", static_cast<unsigned>(entry.method),
                   out.value.x, entry.expected);
            CHECK(false);
        }

        ++checked;
    }

    CHECK(checked == SmoothDeltaMethodCount);
}

// Core 감속 비율 함수(UE FMath Interp* — UnrealMathUtility.h).
TEST(InputModifier_EasingAlpha) {
    CHECK(fabs(math::EaseInAlpha(0.5f, 2.0f) - 0.25f) < 1.0e-6f);
    CHECK(fabs(math::EaseOutAlpha(0.5f, 2.0f) - 0.75f) < 1.0e-6f);
    CHECK(fabs(math::EaseInOutAlpha(0.25f, 2.0f) - 0.125f) < 1.0e-6f);
    CHECK(fabs(math::EaseInOutAlpha(0.75f, 2.0f) - 0.875f) < 1.0e-6f);
    CHECK(fabs(math::SinInAlpha(1.0f) - 1.0f) < 1.0e-6f &&
          fabs(math::SinOutAlpha(1.0f) - 1.0f) < 1e-6f);
    CHECK(math::ExpoInAlpha(0.0f) == 0.0f && math::ExpoOutAlpha(1.0f) == 1.0f);
    CHECK(fabs(math::ExpoInAlpha(1.0f) - 1.0f) < 1.0e-6f);
    CHECK(fabs(math::CircularInAlpha(1.0f) - 1.0f) < 1.0e-6f &&
          math::CircularOutAlpha(1.0f) == 1.0f);
    CHECK(fabs(math::CircularInOutAlpha(0.5f) - 0.5f) < 1.0e-6f);
    CHECK(fabs(math::SinInOutAlpha(0.5f) - 0.5f) < 1.0e-6f);
    CHECK(fabs(math::ExpoInOutAlpha(0.5f) - 0.5f) < 1.0e-6f);
    // 안쪽 값(In·Out이 바뀌면 잡힌다 — UE FMath::Interp* 식).
    CHECK(fabs(math::SinInAlpha(0.5f) - 0.29289322f) < 1.0e-6f);
    CHECK(fabs(math::SinOutAlpha(0.5f) - 0.70710678f) < 1.0e-6f);
    CHECK(fabs(math::ExpoInAlpha(0.5f) - 0.03125f) < 1.0e-6f);
    CHECK(fabs(math::ExpoOutAlpha(0.5f) - 0.96875f) < 1.0e-6f);
    CHECK(fabs(math::CircularInAlpha(0.5f) - 0.13397460f) < 1.0e-6f);
    CHECK(fabs(math::CircularOutAlpha(0.5f) - 0.86602540f) < 1.0e-6f);
    CHECK(fabs(math::EaseInAlpha(0.5f, 3.0f) - 0.125f) < 1.0e-6f);
    CHECK(fabs(math::EaseOutAlpha(0.5f, 3.0f) - 0.875f) < 1.0e-6f);
    CHECK(fabs(math::SinInOutAlpha(0.25f) - 0.5f * (1.0f - cosf(0.25f * 3.14159265f))) < 1.0e-6f);
    CHECK(fabs(math::ExpoInOutAlpha(0.25f) - 0.5f * powf(2.0f, -5.0f)) < 1.0e-6f);
    CHECK(fabs(math::CircularInOutAlpha(0.25f) - 0.5f * (1.0f - sqrtf(0.75f))) < 1.0e-6f);
    // VInterpConstantTo: 남은 거리 이하면 도착, 속도 0이면 그대로.
    CHECK(math::VInterpConstantTo(Vec3d(0, 0, 0), Vec3d(0.1, 0, 0), 1.0, 1.0) == Vec3d(0.1, 0, 0));
    CHECK(math::VInterpConstantTo(Vec3d(0, 0, 0), Vec3d(5, 0, 0), 1.0, 0.0) == Vec3d());
}

} // namespace input_modifier_tests_file
