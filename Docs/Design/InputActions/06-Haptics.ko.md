# 입력 액션 계층 — 6. 햅틱

[목차](README.ko.md)

## 6. 햅틱(게임패드 진동)

### 6.1 데이터(UE `RT/Engine/Classes/GameFramework/ForceFeedbackEffect.h:17-154`)

`HapticEffect`(리플렉션 클래스, `InputActions` 모듈):
- `channels: Array<HapticChannel>`. 기본은 채널 하나다(UE cpp:24-30).
- `Duration()`: 모든 채널 커브의 마지막 키 시간 중 최댓값이다(UE cpp:47-70). 키가 없는 커브는 0으로 친다. 채널이 없으면 0이다.

`HapticChannel`:
- `affectsLeftLarge`·`affectsLeftSmall`·`affectsRightLarge`·`affectsRightSmall`(모두 참)
- `curve`: Curves 모듈 `TangentCurve`. UE `FRuntimeFloatCurve` = FRichCurve cubic이다. 시간(초) → 세기 0–1.

UE `PerDeviceOverrides`·`DeviceProperties`는 7.4절이다.

`MotorValues { float leftLarge, leftSmall, rightLarge, rightSmall; }`
- **`Input` 모듈**에 둔다(UE `FForceFeedbackValues` — `RT/ApplicationCore/Public/GenericPlatform/IInputInterface.h:26-39`).
- PlatformWindow(Input에만 의존)와 InputActions가 같이 쓴다.

### 6.2 재생

`PlayerController::PlayHapticEffect(effect, HapticParams)`(UE `ClientPlayForceFeedback` — `PlayerController.cpp:4191-4214`):
- `HapticParams { tag, looping, ignoreTimeDilation, playWhilePaused }`(UE `ForceFeedbackParameters.h:8-29`)
- 같은 tag(비어 있지 않은)의 효과가 재생 중이면 교체한다.
- null effect는 `InvalidArgument`다.

`StopHapticEffect(effect, tag)`(UE :4226-4250):
- effect가 null이면 tag만 본다.
- tag가 비면 effect만 본다.
- 둘 다 비면 모두 멈춘다.

`ActiveHaptic::Update(dt)`(UE cpp:154-184):
- `playTime += dt`다. dt는 게임 dt이고, `ignoreTimeDilation`이면 unscaled dt다.
- playTime > duration이고 looping이 아니면 끝난다. duration이 0이어도 끝난다.
- 평가 시각은 playTime을 duration으로 나눈 나머지다.

동적 진동(UE `PlayDynamicForceFeedback` — `PlayerController.cpp:4256-4267·4416-4470`):
- `StartDynamicHaptic(intensity, duration, affectsLeftLarge, affectsLeftSmall, affectsRightLarge, affectsRightSmall) → handle`
- `UpdateDynamicHaptic(handle, …)`, `StopDynamicHaptic(handle)`
- duration < 0이면 멈출 때까지 계속된다.
- pause 중에는 처리하지 않는다(기여 0 — UE :4728-4740).

### 6.3 합성(UE `ProcessForceFeedbackAndHaptics` — `PlayerController.cpp:4704-4780`)

`PlayerController::OnTick`의 입력 단계 끝에서 한 번 한다.

1. 네 값을 0으로 둔다.
2. 활성 효과를 차례로 갱신하고 끝난 것을 지운다.
   - pause 중에는 `playWhilePaused`인 것만 갱신하고 그것만 기여한다. 나머지는 멈춘 채 기여 0이다.
3. 동적 진동을 갱신한다(pause 중 제외).
4. 모터마다 V = Clamp(값, V, 1)로 합친다. 즉 **최댓값**이다(합이 아니다 — UE ForceFeedbackEffect.cpp:92-118).
5. `input.haptic_scale`(기본 1)을 곱하고 [0, 1]로 자른다.
6. `input.enable_haptics`(기본 참 — UE `bForceFeedbackEnabled`)가 거짓이면 0이다.
7. 결과를 `PlayerController::MotorOutput()`으로 내놓는다.

출력:
- Gameplay는 플랫폼에 쓰지 않는다. 호스트(Sandbox·엔진 루프)가 로컬 플레이어의 패드 번호로 `GamepadBackend::SetMotors`(6.4절)에 넘긴다.
- PlayerController가 사라지거나 LocalPlayer 연결이 끊기면 호스트가 0을 쓴다.
- 엔진 종료·패드 해제 때도 0이다.

UE `UForceFeedbackComponent`는 7.4절이다.

### 6.4 플랫폼 출력

`GamepadBackend::SetMotors(uint32 index, const input::MotorValues&) → Result<void>`
- 게임 스레드에서 `Poll`과 같은 스레드로 부른다.
- Windows XInput(UE `XInputInterface.cpp:373-385`):
  - wLeftMotorSpeed = round(max(leftLarge, rightLarge) · 65535)
  - wRightMotorSpeed = round(max(leftSmall, rightSmall) · 65535)
- 변환 결과(16비트 두 값)가 직전에 쓴 값과 다를 때만 `XInputSetState`를 부른다. 패드가 새로 연결되면 직전 값을 0으로 되돌린다.
- 연결되지 않은 패드는 `NotFound`, 비유한 값은 `InvalidArgument`, 지원하지 않는 플랫폼은 `Unsupported`다(Android 패드 진동은 AndroidPlatform 후속).
- 변환은 순수 함수 `XInputMotorSpeeds(MotorValues)`로 따로 두고 시험한다.
