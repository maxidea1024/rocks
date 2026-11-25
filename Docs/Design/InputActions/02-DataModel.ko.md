# 입력 액션 계층 — 2. 데이터 모델

[목차](README.ko.md)

## 2. 데이터 모델

### 2.1 값

`InputValueType { Boolean, Axis1D, Axis2D, Axis3D }`. 이 순서가 승격 순서다(UE `EI/Public/InputActionValue.h:10-19`).

`InputValue { Vec3d value; InputValueType type; }`

| 연산 | 규칙(UE `InputActionValue.h`) |
|---|---|
| 타입 지정 생성 | 쓰지 않는 성분을 0으로 한다. **x 값은 그대로다**(Boolean이어도 0.3은 0.3 — :49-65·114-121) |
| `ConvertTo(type)` | 타입 지정 생성과 같다. 키 원시값을 액션 타입으로 바꿀 때 쓴다(UE `EnhancedPlayerInput.cpp:235`) |
| `+=` | 성분 합이고, 타입은 큰 쪽이다 (:73-79) |
| `IsNonZero(tolerance = 1e-4)` | SizeSquared ≥ tolerance² (:111 — `KINDA_SMALL_NUMBER`) |
| `AsBool` | `IsNonZero()` (:205-209) |
| `Magnitude`·`MagnitudeSquared` | Boolean·1D는 x(부호 유지)·x², 2D는 \|(x, y)\|, 3D는 \|v\| (:159-174) |

정밀도는 다음과 같다.
- 값은 `Vec3d`(double)다. UE `FVector`도 double이다.
- 모디파이어·트리거 매개변수는 UE처럼 float다.
- 계산은 double로 하고, 커브 평가(float) 결과는 double로 바꾼다.

### 2.2 키

`InputKey { InputKeyKind kind; uint16 code; }`. 종류는 다음과 같다.
- `Keyboard`(`input::Key`)
- `MouseButton`
- `MouseAxis`(`MouseX`·`MouseY`·`MouseWheel`·`MouseXY` 2D)
- `GamepadButton`
- `GamepadAxis`(스틱 축·트리거·2D 스틱 `LeftStick`·`RightStick`)
- `Touch`(손가락 번호 — UE `EKeys::Touch1`…)

`ReadKey(const input::InputState&, uint32 gamepad, InputKey) → KeySample { InputValue raw; bool down; bool pressed; bool released; }`.

| 종류 | 원시값 |
|---|---|
| 디지털 키·버튼 | 눌림 1, 아니면 0 |
| 패드 축 | **dead zone 전 원시 정규화 값**(`InputState::GamepadAxisRawValue` — IA1에서 더한다) |
| 마우스 축 | 이번 Tick의 delta |
| 터치 | 화면 좌표(x, y), 놓으면 0이다. Boolean 액션은 down만 본다(UE `EnhancedPlayerInput.cpp:551-566`) |

- 패드 축은 UE처럼 원시 값이다. UE는 게임패드에 암묵 dead zone을 걸지 않는다. 플랫폼 dead zone은 이벤트를 보낼지만 정한다(`XInputInterface.cpp:110-115·316-327`, `EnhancedInputSubsystemInterface.cpp:630-635`). dead zone은 DeadZone 모디파이어로 건다.
- InputState의 dead zone 적용 값(`GamepadAxisValue`)을 쓰면 이중 적용이 된다(IAD8).
- 키 이름(`InputKeyName`·`ParseInputKey`)은 저장·리바인딩용이다. UE `EKeys` 이름과 같다(`SpaceBar`, `Gamepad_FaceButton_Bottom`, `Mouse2D`). 업계 공용 식별자라 Rocks 고유 이름 원칙의 예외다(IAD4).
- Rocks 키 집합에 없는 UE 키는 7.4절에 적는다. 그 키 이름은 `ParseInputKey`가 `NotFound`를 내고, 설정 읽기는 건너뛰고 진단한다. 해당 키는 디지털 트리거 버튼(`Gamepad_LeftTrigger`), 스틱 방향 버튼, `AnyKey`다.

### 2.3 InputAction

리플렉션 클래스(Object)다. 필드와 기본값은 UE `EI/Public/InputAction.h`와 같다.

| 필드 | 기본 | UE |
|---|---|---|
| `id`(Guid — 저장·리바인딩 키) | 생성 때 정함 | 없음 — UE는 에셋 경로. 상세 계획 21.1절 ActionId |
| `valueType` | Boolean | :116 |
| `triggerWhenPaused` | 거짓 | :92 |
| `consumeInput` | 참 | :96 — 하위 우선순위 컨텍스트의 같은 키 바인딩을 막는다 |
| `reserveAllMappings` | 거짓 | :108 — 표시만 한다(UE도 강제하지 않는다) |
| `accumulation` | TakeHighestAbsoluteValue | :127 |
| `triggers`·`modifiers` | 빈 배열 | :135·:145 |
| `mappingSettings`(`PlayerMappingSettings`, 선택) | 없음 | :153 |

### 2.4 InputContext·InputBinding

`InputContext`(리플렉션 클래스)의 필드는 다음과 같다.
- `id`(Guid)
- `bindings: Array<InputBinding>`
- `profileOverrides: Array<{ profileId, bindings }>`: 그 프로필이 활성이면 기본 bindings를 통째로 대신한다(UE `MappingProfileOverrides` — `InputMappingContext.cpp:113-121`).
- `registrationTracking { Untracked, CountRegistrations }`(UE :47-64)

`InputBinding`(리플렉션 struct):

| 필드 | UE `EnhancedActionKeyMapping.h` |
|---|---|
| `action`(InputAction 참조) | :94 |
| `key` | :98 |
| `triggers`·`modifiers` | :80·:90 |
| `settingBehavior { InheritFromAction, Override, Ignore }`·`mappingSettings` | :20-28·:126·:132 |

UE의 GameplayTag 입력 모드 필터는 7.4절이다.

### 2.5 PlayerMappingSettings

리바인딩 표시 정보다(UE `EI/Public/PlayerMappableKeySettings.h`).
- `name`: 저장 키다. 프로젝트 안에서 유일하다.
- `displayName`·`displayCategory`(Text)
- `supportedProfiles`: 비면 모든 프로필이다.

### 2.6 오류 코드

| 경우 | 코드 |
|---|---|
| null 액션·컨텍스트, 범위 밖 enum, 비유한 매개변수 | `InvalidArgument` |
| `ParseInputKey`가 모르는 이름 | `NotFound` |
| 할당 실패 | `OutOfMemory`(상태는 그대로) |
| 사용자 설정 JSON 파손 | `CorruptData`. 모르는 이름은 진단만 하고 건너뛴다 |

스레드: 모든 타입은 게임 스레드 전용이다. 데이터(액션·컨텍스트)는 평가 중 바꾸지 않는다(바꾸면 다음 다시 만들기에서 반영 — 4.2절).
