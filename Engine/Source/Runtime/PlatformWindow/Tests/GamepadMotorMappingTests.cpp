#include "../Private/Common/GamepadMotorMapping.h"

#include <PlatformWindow/Window.h>
#include <TestSupport/Test.h>
#include <math.h>

// 모터 값 → XInput 모터 변환과 backend 기본 SetMotors 시험이다(Docs/Design/InputActions 6.4절 —
// IA5).
using namespace rocks;
using namespace rocks::platform::gamepad;
namespace gamepad_motor_mapping_tests_file {

namespace {

input::MotorValues Motors(float leftLarge, float leftSmall, float rightLarge,
                          float rightSmall) noexcept {
    input::MotorValues values;
    values.leftLarge = leftLarge;
    values.leftSmall = leftSmall;
    values.rightLarge = rightLarge;
    values.rightSmall = rightSmall;
    return values;
}

// 진동을 덮어쓰지 않은 backend다(기본 구현을 본다).
class SilentGamepads final : public platform::GamepadBackend {
public:
    void Poll(input::InputQueue&) noexcept override {
    }
};

} // namespace

// UE XInputInterface.cpp:378-381: 왼쪽 = 큰 모터 둘의 최댓값, 오른쪽 = 작은 모터 둘의 최댓값을
// 65535 배 반올림한다. [0, 1] 밖은 잘린다.
TEST(GamepadMotorMapping_XInputSpeeds) {
    CHECK(XInputMotorSpeeds(Motors(0.0f, 0.0f, 0.0f, 0.0f)) == XInputMotorPair{});
    const XInputMotorPair full = XInputMotorSpeeds(Motors(1.0f, 0.0f, 0.0f, 1.0f));
    CHECK(full.left == MaxXInputMotorSpeed && full.right == MaxXInputMotorSpeed);
    // 0.25·65535 = 16383.75 → 16384, 0.5·65535 = 32767.5 → 32768(반올림은 0에서 먼 쪽).
    const XInputMotorPair mixed = XInputMotorSpeeds(Motors(0.25f, 0.5f, 0.125f, 0.0f));
    CHECK(mixed.left == 16384 && mixed.right == 32768);
    const XInputMotorPair right = XInputMotorSpeeds(Motors(0.0f, 0.0f, 0.75f, 0.25f));
    CHECK(right.left == 49151 && right.right == 16384);
    const XInputMotorPair clamped = XInputMotorSpeeds(Motors(2.0f, -1.0f, -3.0f, -0.5f));
    CHECK(clamped.left == MaxXInputMotorSpeed && clamped.right == 0);
    // NaN은 0이다(SetMotors는 그 전에 InvalidArgument로 막는다).
    const float notANumber = static_cast<float>(nan(""));
    CHECK(XInputMotorSpeeds(Motors(notANumber, notANumber, 0.0f, 0.0f)) == XInputMotorPair{});
}

TEST(GamepadMotorMapping_FiniteCheckAndDefaultBackend) {
    CHECK(MotorValuesAreFinite(Motors(0.0f, 1.0f, 2.0f, -1.0f)));
    const float infinity = static_cast<float>(HUGE_VAL);
    const float notANumber = static_cast<float>(nan(""));
    CHECK(!MotorValuesAreFinite(Motors(infinity, 0.0f, 0.0f, 0.0f)));
    CHECK(!MotorValuesAreFinite(Motors(0.0f, notANumber, 0.0f, 0.0f)));
    CHECK(!MotorValuesAreFinite(Motors(0.0f, 0.0f, -infinity, 0.0f)));
    CHECK(!MotorValuesAreFinite(Motors(0.0f, 0.0f, 0.0f, notANumber)));

    // 진동을 붙이지 않은 backend(Android·Mac 현재)는 Unsupported다.
    SilentGamepads gamepads;
    CHECK(gamepads.SetMotors(0, Motors(0.5f, 0.5f, 0.5f, 0.5f)).Error() == ErrorCode::Unsupported);
}

} // namespace gamepad_motor_mapping_tests_file
