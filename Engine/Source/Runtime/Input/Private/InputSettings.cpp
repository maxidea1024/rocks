#include <Config/CVar.h>
#include <Config/ConfigContext.h>
#include <Input/InputSettings.h>

namespace rocks::input {

namespace {

constinit CVar<bool> UseMouseForTouchVar =
    cvar::Var("input.use_mouse_for_touch", false)
        .Help("Treat the left mouse button as touch 0 (UE bUseMouseForTouch). The button no longer "
              "produces mouse presses while this is on.");

constinit CVar<bool> GestureRecognizerVar =
    cvar::Var("input.enable_gesture_recognizer", false)
        .Help("Recognize Pinch, Rotate and Flick touch gestures in player controllers by default "
              "(UE bEnableGestureRecognizer).");

constinit CVar<float> HapticScaleVar =
    cvar::Var("input.haptic_scale", 1.0f)
        .Help("Scale applied to every player's gamepad vibration before it is clamped to [0, 1] "
              "(UE ForceFeedbackScale).");

constinit CVar<bool> EnableHapticsVar =
    cvar::Var("input.enable_haptics", true)
        .Help("Allow gamepad vibration. Players keep playing effects while this is off, but the "
              "motors stay still (UE bForceFeedbackEnabled).");

} // namespace

Result<void> RegisterInputSettings(config::ConfigContext& context) noexcept {
    return context.Register(UseMouseForTouchVar, GestureRecognizerVar, HapticScaleVar,
                            EnableHapticsVar);
}

bool UseMouseForTouchSetting() noexcept {
    return UseMouseForTouchVar.Get();
}

bool GestureRecognizerSetting() noexcept {
    return GestureRecognizerVar.Get();
}

float HapticScaleSetting() noexcept {
    return HapticScaleVar.Get();
}

bool EnableHapticsSetting() noexcept {
    return EnableHapticsVar.Get();
}

} // namespace rocks::input
