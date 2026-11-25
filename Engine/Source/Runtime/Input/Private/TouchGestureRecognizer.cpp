#include <Core/Math/MathUtility.h>
#include <Input/TouchGestureRecognizer.h>

namespace rocks::input {

namespace {

constexpr float FullTurnDegrees = 360.0f;

// UE FRotator::ClampAxis와 같이 [0, 360)으로 접는다.
float ClampAxis(float degrees) noexcept {
    float angle = math::Fmod(degrees, FullTurnDegrees);
    if (angle < 0.0f) {
        angle += FullTurnDegrees;
    }

    return angle;
}

float DistanceSquared(float ax, float ay, float bx, float by) noexcept {
    const float dx = ax - bx;
    const float dy = ay - by;
    return dx * dx + dy * dy;
}

constexpr uint32 Index(TouchGesture gesture) noexcept {
    return static_cast<uint32>(gesture);
}

} // namespace

void TouchGestureRecognizer::Update(const InputState& state, float deltaSeconds,
                                    float distanceScale) noexcept {
    // 지난 프레임의 시작·끝 표지를 지운다(값은 진행 중인 제스처만 남긴다).
    for (TouchGestureValue& gesture : values_) {
        gesture.started = false;
        gesture.ended = false;
        if (!gesture.active) {
            gesture.value = 0.0f;
        }
    }

    const uint32 count = state.TouchCount();
    const float scale = distanceScale > 0.0f ? distanceScale * distanceScale : 1.0f;
    if (previousCount_ == 0 && count == 0) {
        return;
    }

    // 손가락 수가 index를 넘어 늘어나면 그 index의 기준점을 둔다(UE AnchorPoints).
    for (uint32 index = 0; index < MaxTouches; ++index) {
        if (previousCount_ < index + 1 && count >= index + 1) {
            anchorX_[index] = state.TouchX(index);
            anchorY_[index] = state.TouchY(index);
        }
    }

    if (count >= 2) {
        const float startX = state.TouchX(0);
        const float startY = state.TouchY(0);
        const float endX = state.TouchX(1);
        const float endY = state.TouchY(1);
        TouchGestureValue& pinch = values_[Index(TouchGesture::Pinch)];
        if (!pinch.active) {
            // 직전 pinch 점에 가까우면 이전 기준 거리를 이어 쓰고, 아니면 새로 잡는다.
            const float threshold = PinchReanchorDistanceSquared * scale;
            if (anchorDistanceSquared_ == 0.0f ||
                DistanceSquared(lastPinchStartX_, lastPinchStartY_, startX, startY) > threshold ||
                DistanceSquared(lastPinchEndX_, lastPinchEndY_, endX, endY) > threshold) {
                anchorDistanceSquared_ = DistanceSquared(startX, startY, endX, endY);
            }

            pinch.started = true;
            pinch.active = true;
        }

        const float distanceSquared = DistanceSquared(startX, startY, endX, endY);
        pinch.value =
            anchorDistanceSquared_ > 0.0f ? distanceSquared / anchorDistanceSquared_ : 1.0f;
        lastPinchStartX_ = startX;
        lastPinchStartY_ = startY;
        lastPinchEndX_ = endX;
        lastPinchEndY_ = endY;

        const float angle =
            ClampAxis(math::RadiansToDegrees(math::Atan2(startY - endY, startX - endX)));
        TouchGestureValue& rotate = values_[Index(TouchGesture::Rotate)];
        if (!rotate.active) {
            startAngle_ = angle;
            rotate.started = true;
            rotate.active = true;
            rotate.value = 0.0f;
        } else {
            rotate.value = angle - startAngle_;
        }
    }

    // 손가락이 둘 미만이 되면 pinch·rotate가 끝난다(값은 마지막 값을 이 프레임까지 유지한다).
    if (previousCount_ >= 2 && count < 2) {
        const TouchGesture twoFinger[] = {TouchGesture::Pinch, TouchGesture::Rotate};
        for (TouchGesture gesture : twoFinger) {
            values_[Index(gesture)].active = false;
            values_[Index(gesture)].ended = true;
        }
    }

    if (previousCount_ == 0 && count == 1) {
        flickSeconds_ = 0.0f;
    } else if (previousCount_ == 1 && count == 1) {
        flickX_ = state.TouchX(0);
        flickY_ = state.TouchY(0);
        flickSeconds_ += deltaSeconds;
    } else if (previousCount_ >= 1 && count == 0) {
        if (flickSeconds_ < FlickMaxSeconds &&
            DistanceSquared(flickX_, flickY_, anchorX_[0], anchorY_[0]) >
                FlickMinDistanceSquared * scale) {
            // 화면 +X 기준 각이다(오른쪽 0, 위 90, 왼쪽 180, 아래 270 — 화면 Y는 아래가 +).
            TouchGestureValue& flick = values_[Index(TouchGesture::Flick)];
            flick.started = true;
            flick.ended = true;
            flick.value = ClampAxis(math::RadiansToDegrees(
                math::Atan2(-(flickY_ - anchorY_[0]), flickX_ - anchorX_[0])));
        }
    }

    previousCount_ = count;
}

const TouchGestureValue& TouchGestureRecognizer::Gesture(TouchGesture gesture) const noexcept {
    const uint32 index = Index(gesture) < TouchGestureCount ? Index(gesture) : 0u;
    return values_[index];
}

void TouchGestureRecognizer::Reset() noexcept {
    *this = TouchGestureRecognizer();
}

} // namespace rocks::input
