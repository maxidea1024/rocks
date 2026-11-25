#pragma once

#include <Input/InputState.h>

namespace rocks::input {

// 터치 제스처 종류다(UE EKeys::Gesture_Pinch·Gesture_Rotate·Gesture_Flick —
// Docs/Design/TouchInput.ko.md 5.1절). Tap·LongPress·Pan은 인식기가 만들지 않는다(매핑 trigger로
// 만든다 — TD8).
enum class TouchGesture : uint8 {
    Pinch,
    Rotate,
    Flick,
    Count
};

inline constexpr uint32 TouchGestureCount = static_cast<uint32>(TouchGesture::Count);

// 제스처 하나의 이번 프레임 상태다.
// - Pinch: 손가락 0·1이 닿아 있는 동안 active, 값은 기준 거리 제곱 대비 현재 거리 제곱
// 비율(시작 1.0).
// - Rotate: 같은 동안 active, 값은 시작 각도 대비 회전 각(도). 시작 프레임 값은 0이다.
// - Flick: 뗀 프레임에만 started·ended가 함께 참이고, 값은 이동 방향 각(도, 오른쪽 0·위 90 —
//   [0, 360)).
struct TouchGestureValue {
    bool started = false;
    bool active = false;
    bool ended = false;
    float value = 0.0f;
};

// 터치 상태에서 Pinch·Rotate·Flick을 인식한다. 판정은 UE FGestureRecognizer와 같다
// (Engine/Private/GestureRecognizer.cpp): 손가락 수는 닿아 있는 전체 수이고, 위치는 손가락 0·1을
// 쓴다. 거리 임계는 픽셀 단위이며 distanceScale(DPI 배율, 기본 1 — TD14)을 곱한다.
//
// 사용 예(PlayerController가 입력 스냅숏 다음에 부른다):
//   recognizer.Update(playerInput, deltaSeconds);
//   const TouchGestureValue& pinch = recognizer.Gesture(TouchGesture::Pinch);
//   if (pinch.active) { Zoom(pinch.value); }
//
// 스레드: 동기화하지 않는다. 게임 스레드에서 프레임마다 한 번 부른다(시간에 기대는 판정은 Flick의
// deltaSeconds 합뿐이며, 고정 delta면 결정적이다).
class ROCKS_INPUT_API TouchGestureRecognizer {
public:
    // Pinch 기준 거리를 다시 잡는 임계(거리 제곱, px²)다. 두 점이 직전 pinch 점에서 이만큼 넘게
    // 벗어나면 새 pinch로 본다(UE PinchThreshold 10000 — 100 px).
    static constexpr float PinchReanchorDistanceSquared = 10000.0f;
    // Flick: 이 시간(초) 안에 떼고, 이동 거리 제곱이 FlickMinDistanceSquared(px²)를 넘어야 한다(UE
    // 0.25 s·10000).
    static constexpr float FlickMaxSeconds = 0.25f;
    static constexpr float FlickMinDistanceSquared = 10000.0f;

    // 한 프레임을 처리한다. deltaSeconds는 이번 프레임 시간, distanceScale은 거리 임계 배율이다.
    void Update(const InputState& state, float deltaSeconds, float distanceScale = 1.0f) noexcept;

    const TouchGestureValue& Gesture(TouchGesture gesture) const noexcept;

    // 진행 중인 제스처와 기억을 모두 지운다(손가락 상태가 끊길 때 — 입력 원천 교체 등).
    void Reset() noexcept;

private:
    TouchGestureValue values_[TouchGestureCount];
    // 손가락 index별 기준점(그 index까지 손가락 수가 처음 늘었을 때 위치)이다.
    float anchorX_[MaxTouches] = {};
    float anchorY_[MaxTouches] = {};
    float anchorDistanceSquared_ = 0.0f;
    float lastPinchStartX_ = 0.0f;
    float lastPinchStartY_ = 0.0f;
    float lastPinchEndX_ = 0.0f;
    float lastPinchEndY_ = 0.0f;
    float startAngle_ = 0.0f;
    float flickX_ = 0.0f;
    float flickY_ = 0.0f;
    float flickSeconds_ = 0.0f;
    uint32 previousCount_ = 0;
};

} // namespace rocks::input
