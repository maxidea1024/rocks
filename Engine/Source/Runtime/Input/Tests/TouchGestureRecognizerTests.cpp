#include <Input/InputState.h>
#include <Input/TouchGestureRecognizer.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace touch_gesture_tests_file {

using namespace rocks::input;

namespace {

// 고정 프레임 시간이다(결정적 판정 — Flick의 시간 합만 이 값에 기댄다).
constexpr float FrameSeconds = 0.05f;

bool Near(float actual, float expected) noexcept {
    const float difference = actual - expected;
    return difference < 1.0e-3f && difference > -1.0e-3f;
}

struct Rig {
    InputState state;
    TouchGestureRecognizer recognizer;
    float distanceScale = 1.0f;

    template <usize Count>
    void Frame(const InputEvent (&events)[Count], float seconds = FrameSeconds) noexcept {
        state.BeginFrame(Span<const InputEvent>(events, Count));
        recognizer.Update(state, seconds, distanceScale);
    }

    void Frame(float seconds = FrameSeconds) noexcept {
        state.BeginFrame({});
        recognizer.Update(state, seconds, distanceScale);
    }

    const TouchGestureValue& Of(TouchGesture gesture) const noexcept {
        return recognizer.Gesture(gesture);
    }
};

} // namespace

// 두 손가락: pinch는 거리 제곱 비율, rotate는 시작 각도 대비 회전 각이다(UE GestureRecognizer.cpp와
// 같다).
TEST(TouchGesture_PinchAndRotate) {
    Rig rig;
    rig.Frame({InputEvent::TouchBegan(0, 0.0f, 0.0f), InputEvent::TouchBegan(1, 100.0f, 0.0f)});
    CHECK(rig.Of(TouchGesture::Pinch).started && rig.Of(TouchGesture::Pinch).active);
    CHECK(Near(rig.Of(TouchGesture::Pinch).value, 1.0f));
    CHECK(rig.Of(TouchGesture::Rotate).started && Near(rig.Of(TouchGesture::Rotate).value, 0.0f));
    rig.Frame({InputEvent::TouchMoved(1, 200.0f, 0.0f)});
    CHECK(!rig.Of(TouchGesture::Pinch).started && rig.Of(TouchGesture::Pinch).active);
    CHECK(Near(rig.Of(TouchGesture::Pinch).value, 4.0f));
    CHECK(Near(rig.Of(TouchGesture::Rotate).value, 0.0f));
    // 손가락 1이 아래로(화면 Y+) 가면 두 점 벡터(0−1)의 각이 180°에서 270°로 바뀐다.
    rig.Frame({InputEvent::TouchMoved(1, 0.0f, 100.0f)});
    CHECK(Near(rig.Of(TouchGesture::Rotate).value, 90.0f) &&
          Near(rig.Of(TouchGesture::Pinch).value, 1.0f));
    rig.Frame({InputEvent::TouchEnded(1, 0.0f, 100.0f)});
    CHECK(rig.Of(TouchGesture::Pinch).ended && !rig.Of(TouchGesture::Pinch).active);
    CHECK(rig.Of(TouchGesture::Rotate).ended && !rig.Of(TouchGesture::Rotate).active);
    rig.Frame();
    CHECK(!rig.Of(TouchGesture::Pinch).ended && !rig.Of(TouchGesture::Rotate).ended);
}

// 직전 pinch 점 근처에서 다시 시작하면 이전 기준 거리를 이어 쓰고, 멀리서 시작하면 새로 잡는다.
TEST(TouchGesture_PinchReanchor) {
    Rig rig;
    rig.Frame({InputEvent::TouchBegan(0, 0.0f, 0.0f), InputEvent::TouchBegan(1, 0.0f, 100.0f)});
    rig.Frame({InputEvent::TouchEnded(0, 0.0f, 0.0f), InputEvent::TouchEnded(1, 0.0f, 100.0f)});
    rig.Frame();
    // 끝점이 직전 점에서 100 px(거리 제곱 10000 — 임계와 같아 넘지 않음) 떨어졌다: 기준 10000을
    // 잇는다.
    rig.Frame({InputEvent::TouchBegan(0, 0.0f, 0.0f), InputEvent::TouchBegan(1, 0.0f, 200.0f)});
    CHECK(rig.Of(TouchGesture::Pinch).started && Near(rig.Of(TouchGesture::Pinch).value, 4.0f));
    rig.Frame({InputEvent::TouchEnded(0, 0.0f, 0.0f), InputEvent::TouchEnded(1, 0.0f, 200.0f)});
    rig.Frame();
    rig.Frame(
        {InputEvent::TouchBegan(0, 500.0f, 500.0f), InputEvent::TouchBegan(1, 600.0f, 500.0f)});
    CHECK(rig.Of(TouchGesture::Pinch).started && Near(rig.Of(TouchGesture::Pinch).value, 1.0f));
}

// Flick: 0.25초 안에 100 px 넘게 움직이고 떼면 뗀 프레임에 한 번(시작·끝 함께), 값은 방향 각이다.
TEST(TouchGesture_FlickDirectionsAndLimits) {
    struct Case {
        float x;
        float y;
        float angle;
    };
    const Case directions[] = {{150.0f, 0.0f, 0.0f},
                               {0.0f, -150.0f, 90.0f},
                               {-150.0f, 0.0f, 180.0f},
                               {0.0f, 150.0f, 270.0f}};
    for (const Case& direction : directions) {
        Rig rig;
        rig.Frame({InputEvent::TouchBegan(0, 0.0f, 0.0f)});
        rig.Frame({InputEvent::TouchMoved(0, direction.x, direction.y)});
        CHECK(!rig.Of(TouchGesture::Flick).started);
        rig.Frame({InputEvent::TouchEnded(0, direction.x, direction.y)});
        CHECK(rig.Of(TouchGesture::Flick).started && rig.Of(TouchGesture::Flick).ended);
        CHECK(!rig.Of(TouchGesture::Flick).active &&
              Near(rig.Of(TouchGesture::Flick).value, direction.angle));
        rig.Frame();
        CHECK(!rig.Of(TouchGesture::Flick).started);
    }

    // 느리다(머문 시간 합 0.25초 이상): 없음.
    Rig slow;
    slow.Frame({InputEvent::TouchBegan(0, 0.0f, 0.0f)});
    slow.Frame({InputEvent::TouchMoved(0, 150.0f, 0.0f)}, 0.3f);
    slow.Frame({InputEvent::TouchEnded(0, 150.0f, 0.0f)});
    CHECK(!slow.Of(TouchGesture::Flick).started);
    // 짧다(100 px 이하): 없음.
    Rig small;
    small.Frame({InputEvent::TouchBegan(0, 0.0f, 0.0f)});
    small.Frame({InputEvent::TouchMoved(0, 90.0f, 0.0f)});
    small.Frame({InputEvent::TouchEnded(0, 90.0f, 0.0f)});
    CHECK(!small.Of(TouchGesture::Flick).started);
    // DPI 배율 2면 임계가 200 px다(TD14): 150 px는 없음.
    Rig dense;
    dense.distanceScale = 2.0f;
    dense.Frame({InputEvent::TouchBegan(0, 0.0f, 0.0f)});
    dense.Frame({InputEvent::TouchMoved(0, 150.0f, 0.0f)});
    dense.Frame({InputEvent::TouchEnded(0, 150.0f, 0.0f)});
    CHECK(!dense.Of(TouchGesture::Flick).started);
}

// Reset은 진행 중인 제스처를 지운다.
TEST(TouchGesture_Reset) {
    Rig rig;
    rig.Frame({InputEvent::TouchBegan(0, 0.0f, 0.0f), InputEvent::TouchBegan(1, 100.0f, 0.0f)});
    rig.recognizer.Reset();
    CHECK(!rig.Of(TouchGesture::Pinch).active && !rig.Of(TouchGesture::Rotate).active);
}

} // namespace touch_gesture_tests_file
