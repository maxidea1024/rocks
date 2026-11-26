# 입력 액션 계층 — 6. 햅틱

[목차](README.ko.md)

## 6. 햅틱(게임패드 진동)

### 6.1 데이터(UE `RT/Engine/Classes/GameFramework/ForceFeedbackEffect.h:17-154`)

`HapticEffect`(리플렉션 struct, `InputActions` 모듈 — 에셋 감싸기는 IA6, `InputActionData`와 같은 값 모양):
- `id: Guid` — 효과의 정체다(`StopHapticEffect`가 효과를 고른다 — UE는 UObject 포인터). 0이면 무효다.
- `channels: Array<HapticChannel>`. 기본은 비어 있다(UE 생성자는 채널 하나를 넣는다 — cpp:24-30. Rocks 생성자는 할당하지 않는다. 채널 없는 효과는 길이 0이라 재생하자마자 끝난다).
- `Duration()`: 모든 채널 커브의 마지막 키 시간 중 최댓값이다(UE cpp:47-70). 키가 없는 커브는 0으로 친다. 채널이 없으면 0이다.

`HapticChannel`:
- `affectsLeftLarge`·`affectsLeftSmall`·`affectsRightLarge`·`affectsRightSmall`(모두 참)
- `curve`: Curves 모듈 `TangentCurve`. UE `FRuntimeFloatCurve` = FRichCurve cubic이다. 시간(초) → 세기 0–1.

UE `PerDeviceOverrides`·`DeviceProperties`는 7.4절이다.

`MotorValues { float leftLarge, leftSmall, rightLarge, rightSmall; }`
- **`Input` 모듈**에 둔다(UE `FForceFeedbackValues` — `RT/ApplicationCore/Public/GenericPlatform/IInputInterface.h:26-39`).
- PlatformWindow(Input에만 의존)와 InputActions가 같이 쓴다.

### 6.2 재생

재생·합성은 InputActions의 값 타입 `HapticPlayer`(플레이어 하나 몫, `InputActions/Haptics/`)가 한다. `PlayerController`가 하나 갖고 `Haptics()`로 내놓는다(아래 함수는 그 멤버다 — 같은 함수를 PlayerController에 다시 두지 않는다. Gameplay 없이 시험한다).

`PlayHapticEffect(effect, HapticParams)`(UE `ClientPlayForceFeedback` — `PlayerController.cpp:4191-4214`):
- `HapticParams { tag(StringView — 재생이 사본을 둔다), looping, ignoreTimeDilation, playWhilePaused }`(UE `ForceFeedbackParameters.h:8-29`)
- effect는 **깊은 사본**으로 둔다(버린 대안: 포인터 — 효과가 재생보다 먼저 없어지는 수명 계약이 호출자에 남는다. 재생은 드물어 복사 비용이 작다).
- 같은 tag(비어 있지 않은)의 효과가 재생 중이면 교체한다(사본을 먼저 만들고 지운다 — 할당 실패면 그대로).
- 무효 effect(id 0)는 `InvalidArgument`다(UE null). 할당 실패는 `OutOfMemory`(그대로)다.

`StopHapticEffect(effectId, tag)`(UE :4226-4250):
- effectId가 0이면 tag만 본다.
- tag가 비면 effect만 본다.
- 둘 다 비면 모두 멈춘다.

`ActiveHaptic::Update(dt)`(UE cpp:154-184):
- `playTime += dt`다. dt는 게임 dt이고, `ignoreTimeDilation`이면 unscaled dt다.
- playTime > duration이고 looping이 아니면 끝난다. duration이 0이어도 끝난다.
- 평가 시각은 playTime − duration·floor(playTime / duration)이다. 반복이 아니면 playTime ≤ duration일 때만 값을 낸다(playTime이 정확히 duration이면 평가 시각은 0이다 — UE 식 그대로). duration이 0이면 평가 시각은 0이다(UE는 0/0 NaN으로 커브를 평가한다 — Rocks는 피한다).
- 커브 값은 그대로 쓴다(UE `Eval` — 음수는 합성에서 지금 값에 지고, 1 초과는 1로 잘린다). NaN도 지금 값에 진다(UE `Clamp(NaN, 지금, 1)`은 1이다 — 다른 점, 진동을 키우지 않는 쪽. TangentCurve가 비유한 키를 받지 않아 실제로는 닿기 어렵다).

동적 진동(UE `PlayDynamicForceFeedback` — `PlayerController.cpp:4256-4267·4416-4470`):
- `StartDynamicHaptic(DynamicHaptic) → Result<DynamicHapticHandle>`. `DynamicHaptic { intensity, duration, affectsLeftLarge, affectsLeftSmall, affectsRightLarge, affectsRightSmall }`(UE `FDynamicForceFeedbackDetails`).
- handle은 플레이어마다 1부터 는다(0은 무효 — UE는 프로세스 전역 static 카운터다. Rocks는 전역 상태를 두지 않는다).
- `UpdateDynamicHaptic(handle, DynamicHaptic)`: 세기·모터만 바꾼다(duration·경과 시간 그대로 — UE Update 동작). 없는 handle은 `NotFound`다.
- `StopDynamicHaptic(handle)`: 없으면 아무 일도 없다.
- UE의 "멈춘 handle로 다시 Start"는 두지 않는다(버린 대안 — 새 handle을 받으면 된다).
- 갱신: 경과 += dt(게임 dt), duration ≥ 0이고 경과 ≥ duration이면 끝난다. duration < 0이면 멈출 때까지 계속된다.
- pause 중에는 처리하지 않는다(기여 0, 경과도 멈춤 — UE :4728-4740).
- 비유한 intensity·duration은 `InvalidArgument`다.

### 6.3 합성(UE `ProcessForceFeedbackAndHaptics` — `PlayerController.cpp:4704-4780`)

`PlayerController::OnTick`의 입력 단계 끝에서 한 번 한다.

`HapticPlayer::Update(HapticUpdateContext{deltaSeconds, unscaledDeltaSeconds, paused, scale, enabled}) → MotorValues`이고, PlayerController는 TickContext의 dt·unscaled dt·paused와 아래 설정 값을 넘긴다.

1. 네 값을 0으로 둔다.
2. 활성 효과를 차례로 갱신하고 끝난 것을 지운다.
   - pause 중에는 `playWhilePaused`인 것만 갱신하고 그것만 기여한다. 나머지는 멈춘 채 기여 0이다.
3. 동적 진동을 갱신한다(pause 중 제외).
4. 모터마다 V = Clamp(값, V, 1)로 합친다. 즉 **최댓값**이다(합이 아니다 — UE ForceFeedbackEffect.cpp:92-118).
5. `input.haptic_scale`(기본 1)을 곱하고 [0, 1]로 자른다.
6. `input.enable_haptics`(기본 참 — UE `bForceFeedbackEnabled`)가 거짓이면 0이다.
7. 결과를 `PlayerController::MotorOutput()`으로 내놓는다. LocalPlayer가 없으면(UE `Player == nullptr`) 처리하지 않고 0이다.

설정 `input.haptic_scale`(float)·`input.enable_haptics`(bool)는 Input 모듈 `InputSettings`의 CVar다. `GameInstanceClient`가 엔진 프레임마다 각 PlayerController에 `ApplyHapticSettings(scale, enabled)`로 넣는다(`input.enable_gesture_recognizer`와 같은 자리 — 설정 문맥이 없으면 1·켬). UE는 PlayerController 속성(`ForceFeedbackScale`·`bForceFeedbackEnabled`)이지만 사용자 옵션이라 전역 설정으로 둔다. 비유한 scale은 0으로 친다.

출력:
- Gameplay는 플랫폼에 쓰지 않는다. 호스트(Sandbox·엔진 루프)가 프레임 끝에 로컬 플레이어의 패드 번호로 `GamepadBackend::SetMotors`(6.4절)에 넘긴다. 플레이어의 값은 `GameInstance`에서 LocalPlayer → PlayerController로 읽는다.
- 패드 번호는 `ControllerId`다. 음수 ControllerId 플레이어는 진동을 쓰지 않는다(입력은 패드 0을 읽지만 — 5.1절 — 패드 0의 진동 주인은 ControllerId 0이다. 두 플레이어가 한 패드에 번갈아 쓰지 않게 한다).
- 호스트는 지난 프레임에 0이 아닌 값을 쓴 패드를 기억하고, 이번 프레임에 아무도 쓰지 않은 패드(LocalPlayer 제거·인스턴스 정지)에 0을 쓴다(바뀐 값만 쓰는 backend에 마지막 값이 남지 않게).
- PlayerController가 사라지거나 LocalPlayer 연결이 끊기면 호스트가 0을 쓴다.
- 엔진 종료·패드 해제 때도 0이다.

UE `UForceFeedbackComponent`는 7.4절이다.

### 6.4 플랫폼 출력

`GamepadBackend::SetMotors(uint32 index, const input::MotorValues&) → Result<void>`
- 게임 스레드에서 `Poll`과 같은 스레드로 부른다.
- 가상 함수이고 기본 구현은 `Unsupported`다(Mac backend는 진동을 붙일 때 덮어쓴다).
- Windows XInput(UE `XInputInterface.cpp:373-385`). `XInputSetState`는 선택 함수다(찾지 못하면 SetMotors가 `Unsupported` — 입력은 그대로):
  - wLeftMotorSpeed = round(max(leftLarge, rightLarge) · 65535)
  - wRightMotorSpeed = round(max(leftSmall, rightSmall) · 65535)
- 변환 결과(16비트 두 값)가 직전에 쓴 값과 다를 때만 `XInputSetState`를 부른다. 패드가 새로 연결되면 직전 값을 0으로 되돌린다.
- 범위 밖 index는 `InvalidArgument`, 연결되지 않은 패드는 `NotFound`, 비유한 값은 `InvalidArgument`, 지원하지 않는 플랫폼은 `Unsupported`다. `XInputSetState`가 실패하면 `IoError`다(직전 값은 그대로 — 다음에 다시 쓴다).
- 백엔드가 없어지면(소멸자) 연결된 패드에 0을 쓴다.
- 변환은 순수 함수 `XInputMotorSpeeds(MotorValues)`로 따로 두고 시험한다. 입력은 [0, 1]로 잘라 쓴다.

Android 패드(UE `AndroidInputInterface.cpp:598-651` `UpdateControllerVibeMotors`, `GameActivity.java.template:7339-7370` `AndroidThunkJava_SetInputDeviceVibrators` — 2026-11-26):
- 세기: 왼쪽 = 바이트(max(leftLarge, leftSmall)), 오른쪽 = 바이트(max(rightLarge, rightSmall))다. 바이트(v) = clamp(⌊v·255⌋, 0, 255)(UE `ConvertToByte` — 버림). XInput과 짝짓는 방식이 다르다(XInput은 큰·작은 모터, Android는 왼쪽·오른쪽 진동자 — 둘 다 UE 그대로).
- 진동자: 패드 `InputDevice`의 `getVibratorManager()`(API 31)가 있으면 진동자가 하나면 그것에 max(왼쪽, 오른쪽), 둘 이상이면 첫째에 왼쪽·둘째에 오른쪽이다. 메서드가 없으면(API 31 미만) `getVibrator()` 하나에 max다(UE는 SDK 버전을 보고, Rocks는 메서드가 있는지 본다 — 같은 결과).
- 한 번 쓰기: 세기가 1 이상이면 `vibrate(VibrationEffect.createOneShot(1000 ms, 세기))`, 0이면 `cancel()`이다.
- 다시 쓰기: one-shot이 1초 뒤 멈추므로, 0이 아닌 값을 쓴 지 1초(UE `MaxVibeTime` 1000) 이상 지났으면 직전 값을 0으로 보고 같은 값도 다시 쓴다. 그 밖에는 바뀐 세기만 쓴다. 판단은 순수 함수 `NextAndroidVibration(상태, MotorValues, 포커스, 지금 ms, 쓸 값)`(Common)이고 시험은 시각을 인자로 준다(벽시계에 기대지 않는다).
- 호출 빈도: UE는 입력 인터페이스가 Tick마다 스스로 다시 건다. Rocks는 다시 걸기와 포커스 끄기가 `SetMotors` 안에 있으므로, 호스트는 진동하는 동안 같은 값이라도 프레임마다 부른다(Sandbox 호스트가 그렇게 한다 — 바뀔 때만 부르면 Android 진동이 1초 뒤 멈춘다).
- 스레드: 게임 스레드는 JavaVM에 붙어 있지 않다. `SetMotors`는 쓸 요청(장치 ID·두 세기)을 패드 저장소의 그 슬롯에 두고(아직 처리되지 않은 요청은 새 요청이 덮는다) 창 입력 스레드의 looper를 깨운다. JavaVM에 붙은 입력 스레드가 JNI로 쓴다(UE는 `runOnUiThread`로 UI 스레드에 넘긴다 — 다른 점. 바뀐 값만 쓰고 처리 전 요청을 덮으므로 binder 호출은 많아야 프레임마다 패드당 진동자 수다). 창이 없거나 입력 스레드가 JavaVM에 붙지 못했으면 `Unsupported`다.
- 포커스: 앱이 포커스를 잃으면 0으로 쓴다(UE `IsGameInFocus`). 포커스가 없는 동안 요청은 0이다. one-shot이 이미 끝난 뒤 포커스를 잃으면 쓰지 않는다(UE는 지난 세기로 판단해 0을 한 번 더 쓴다 — 결과는 같다).
- 패드 진동자 판정: UE는 `Android.UseControllerFeedback`과 장치 정보의 모터 수(`FeedbackMotorCount > 0`)를 본다. Rocks는 그 설정이 없고 모든 패드에 JNI로 물어본다(진동자 id가 비었거나 진동자가 없으면 아무것도 하지 않는다).
- 권한: `android.permission.VIBRATE`를 manifest에 늘 넣는다(UE `UEDeployAndroid.cs:3080` — UE는 Meta Quest 패키지와 manifest 요구 재정의가 있을 때만 뺀다).
- 장치가 사라졌거나(`getDevice`가 null) 진동자가 없거나 Java 예외가 나면 그 요청을 버린다(예외는 지운다). 비동기라 `SetMotors`의 결과로 돌려주지 못한다 — 요청을 넣으면 성공이다.
- 휴대폰 본체 진동(패드가 없을 때 UE `AndroidThunkCpp_Vibrate`)은 들이지 않는다(후속 — `SetMotors`는 패드 슬롯 단위다).
- 백엔드가 없어지면 0이 아닌 값을 쓴 슬롯에 0 요청을 넣는다. 입력 스레드는 끝나기 전에 남은 요청을 쓴다. 창(입력 스레드)이 먼저 없어졌으면 요청을 넣지 못하고, 진동은 one-shot 길이(1초) 안에 스스로 멈춘다.
