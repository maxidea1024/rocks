#include "../Private/Common/GamepadMotorMapping.h"

#include <PlatformWindow/Window.h>
#include <TestSupport/Test.h>
#include <math.h>

// 모터 값 → XInput 모터·Android 진동자 변환과 backend 기본 SetMotors 시험이다(Docs/Design/
// InputActions 6.4절 — IA5·IA5b).
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

    // 진동을 덮어쓰지 않은 backend(Mac 현재)는 Unsupported다.
    SilentGamepads gamepads;
    CHECK(gamepads.SetMotors(0, Motors(0.5f, 0.5f, 0.5f, 0.5f)).Error() == ErrorCode::Unsupported);
}

// UE UpdateControllerVibeMotors·ConvertToByte: 왼쪽 = 왼쪽 둘의 최댓값, 오른쪽 = 오른쪽 둘의
// 최댓값을 255 배 버린다. [0, 1] 밖은 잘리고 NaN은 0이다.
TEST(GamepadMotorMapping_AndroidIntensities) {
    CHECK(AndroidVibratorIntensities(Motors(0.0f, 0.0f, 0.0f, 0.0f)) == AndroidVibratorPair{});
    const AndroidVibratorPair full = AndroidVibratorIntensities(Motors(1.0f, 0.0f, 0.0f, 1.0f));
    CHECK(full.left == MaxAndroidVibrationIntensity && full.right == MaxAndroidVibrationIntensity);
    // 0.5·255 = 127.5 → 127, 0.25·255 = 63.75 → 63(버림 — XInput 반올림과 다르다).
    const AndroidVibratorPair mixed = AndroidVibratorIntensities(Motors(0.25f, 0.5f, 0.25f, 0.0f));
    CHECK(mixed.left == 127 && mixed.right == 63);
    // 짝: 큰·작은이 아니라 왼쪽·오른쪽이다(leftSmall은 왼쪽, rightLarge는 오른쪽).
    const AndroidVibratorPair sides = AndroidVibratorIntensities(Motors(0.0f, 1.0f, 0.0f, 0.0f));
    CHECK(sides.left == MaxAndroidVibrationIntensity && sides.right == 0);
    const AndroidVibratorPair clamped =
        AndroidVibratorIntensities(Motors(2.0f, -1.0f, -3.0f, -0.5f));
    CHECK(clamped.left == MaxAndroidVibrationIntensity && clamped.right == 0);
    const float notANumber = static_cast<float>(nan(""));
    CHECK(AndroidVibratorIntensities(Motors(notANumber, 0.0f, 0.0f, notANumber)) ==
          AndroidVibratorPair{});
}

// 바뀐 세기만 쓰고, 0이 아닌 값은 one-shot 길이(1000 ms)가 지나면 같은 값도 다시 쓴다. 포커스가
// 없으면 0이다. 시각은 인자로 준다.
TEST(GamepadMotorMapping_AndroidRewriteSchedule) {
    AndroidVibrationState state;
    AndroidVibratorPair out;
    const input::MotorValues half = Motors(0.5f, 0.0f, 0.0f, 0.5f);
    const AndroidVibratorPair halfPair{127, 127};
    // 처음부터 0이면 쓸 것이 없다.
    CHECK(!NextAndroidVibration(state, Motors(0.0f, 0.0f, 0.0f, 0.0f), true, 0, out));
    // 켜기 → 쓴다. 같은 값은 1000 ms 전까지 다시 쓰지 않는다.
    REQUIRE(NextAndroidVibration(state, half, true, 100, out));
    CHECK(out == halfPair);
    CHECK(state.writtenAtMilliseconds == 100);
    CHECK(!NextAndroidVibration(state, half, true, 500, out));
    CHECK(!NextAndroidVibration(state, half, true, 1099, out));
    // 1000 ms(경계 포함) 지나면 one-shot이 끝났으므로 다시 쓴다.
    REQUIRE(NextAndroidVibration(state, half, true, 1100, out));
    CHECK(out == halfPair);
    CHECK(state.writtenAtMilliseconds == 1100);
    // 값이 바뀌면 곧바로 쓴다.
    const input::MotorValues strong = Motors(1.0f, 0.0f, 0.0f, 0.0f);
    REQUIRE(NextAndroidVibration(state, strong, true, 1200, out));
    CHECK(out == (AndroidVibratorPair{255, 0}));
    // 끄기는 쓴다(cancel). 꺼진 뒤에는 시간이 지나도 다시 쓰지 않는다.
    REQUIRE(NextAndroidVibration(state, Motors(0.0f, 0.0f, 0.0f, 0.0f), true, 1300, out));
    CHECK(out == AndroidVibratorPair{});
    CHECK(!NextAndroidVibration(state, Motors(0.0f, 0.0f, 0.0f, 0.0f), true, 5000, out));
    // 켠 채로 시간이 지난 뒤 0을 원하면 이미 멈췄으므로 쓰지 않는다(UE와 같다).
    REQUIRE(NextAndroidVibration(state, half, true, 6000, out));
    CHECK(!NextAndroidVibration(state, Motors(0.0f, 0.0f, 0.0f, 0.0f), true, 7000, out));
    CHECK(state.written == AndroidVibratorPair{});
    // 포커스가 없으면 0을 쓴다. 포커스가 없는 동안은 0이다.
    REQUIRE(NextAndroidVibration(state, half, true, 8000, out));
    REQUIRE(NextAndroidVibration(state, half, false, 8100, out));
    CHECK(out == AndroidVibratorPair{});
    CHECK(!NextAndroidVibration(state, half, false, 9500, out));
    // 포커스가 돌아오면 다시 쓴다.
    REQUIRE(NextAndroidVibration(state, half, true, 9600, out));
    CHECK(out == halfPair);
    // one-shot이 끝난 뒤 포커스를 잃으면 이미 멈췄으므로 쓰지 않는다(UE는 0을 한 번 더 쓴다
    // — 6.4절).
    CHECK(!NextAndroidVibration(state, half, false, 10600, out));
    CHECK(state.written == AndroidVibratorPair{});
    // 시각이 뒤로 가면(쓴 시각보다 이른 now) 지난 시간을 0으로 보고 다시 쓰지 않는다.
    REQUIRE(NextAndroidVibration(state, half, true, 20000, out));
    CHECK(!NextAndroidVibration(state, half, true, 100, out));
    CHECK(state.writtenAtMilliseconds == 20000);
}

} // namespace gamepad_motor_mapping_tests_file
