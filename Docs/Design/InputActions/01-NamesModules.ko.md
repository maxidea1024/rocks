# 입력 액션 계층 — 1. 이름과 모듈

[목차](README.ko.md)

## 1. 이름과 모듈

### 1.1 대응표

| Rocks | UE 5.8 | 비고 |
|---|---|---|
| `InputAction` | `UInputAction` | 리플렉션 Object(에셋으로 감쌀 수 있다 — 7.1절 IA6) |
| `InputValue` · `InputValueType` | `FInputActionValue` · `EInputActionValueType` | |
| `InputAccumulation` | `EInputActionAccumulationBehavior` | |
| `InputContext` | `UInputMappingContext` | |
| `InputBinding` | `FEnhancedActionKeyMapping` | 키 하나 → 액션 하나 |
| `InputKey` | `FKey` | 키보드·마우스·패드·터치를 한 값으로 |
| `InputModifier` | `UInputModifier` 파생(내장 11종) | 종류 enum + 값 struct(3.1절) |
| `InputTrigger` | `UInputTrigger` 파생(내장 11종) | 종류 enum + 값 struct(3.2절) |
| `TriggerState` · `TriggerKind` | `ETriggerState` · `ETriggerType` | |
| `ActionEvent` | `ETriggerEvent` | 비트 플래그 |
| `ContextOptions` | `FModifyContextOptions` | |
| `PlayerActionInput` | `UEnhancedPlayerInput` + 입력 subsystem의 컨텍스트 목록 | 로컬 플레이어마다 하나 |
| `ActionState` | `FInputActionInstance` | 액션의 실행 중 상태 |
| `InputActionBindings` | `UEnhancedInputComponent`의 바인딩 | PlayerController가 가진다(5.1절) |
| `PlayerMappingSettings` | `UPlayerMappableKeySettings` | |
| `InputUserSettings` · `PlayerKeyMapping` · `KeyProfile` | `UEnhancedInputUserSettings` · `FPlayerKeyMapping` · `UEnhancedPlayerMappableKeyProfile` | |
| `HapticEffect` · `HapticChannel` · `ActiveHaptic` | `UForceFeedbackEffect` · `FForceFeedbackChannelDetails` · `FActiveForceFeedbackEffect` | |
| `MotorValues` | `FForceFeedbackValues` | 큰·작은 모터 × 좌우 — `Input` 모듈(6.1절) |

UE 접두(U·F·E)는 쓰지 않는다. "Enhanced"·"Mappable" 같은 UE 고유 수식어도 쓰지 않는다.

### 1.2 모듈

- **`InputActions`**(새 모듈, `Engine/Source/Runtime/InputActions`): 1.1절 표의 타입, 평가기, 사용자 설정, 햅틱 효과 평가를 둔다.
  - 의존은 `Input`(입력 스냅숏·키 enum·`MotorValues`), `ObjectRuntime`·`CoreTypeBindings`(리플렉션), `Curves`(사용자 응답 커브·햅틱 커브 `TangentCurve`)다.
  - World·Gameplay에 의존하지 않는다. 평가는 입력 스냅숏과 시간만 받는다.
  - 모듈 유형은 Runtime이다. 전용 서버도 액션 데이터를 읽을 수 있어야 한다(로컬 입력 평가는 클라이언트에서만 돈다).
- **`Gameplay`**: `PlayerController`가 `PlayerActionInput`과 `InputActionBindings`를 갖고 평가·전달·햅틱 재생을 돌린다(5절, 6절).
- **`PlatformWindow`**: `GamepadBackend::SetMotors`(6.4절)로 진동을 출력한다(Windows XInput).
- 편집기·에셋 감싸기(`InputActionAsset`·`InputContextAsset`)는 IA6이다(7절). 그 전에는 코드나 리플렉션 데이터로 만든다.
