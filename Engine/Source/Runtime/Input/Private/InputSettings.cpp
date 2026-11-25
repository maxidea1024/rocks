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

} // namespace

Result<void> RegisterInputSettings(config::ConfigContext& context) noexcept {
    return context.Register(UseMouseForTouchVar, GestureRecognizerVar);
}

bool UseMouseForTouchSetting() noexcept {
    return UseMouseForTouchVar.Get();
}

bool GestureRecognizerSetting() noexcept {
    return GestureRecognizerVar.Get();
}

} // namespace rocks::input
