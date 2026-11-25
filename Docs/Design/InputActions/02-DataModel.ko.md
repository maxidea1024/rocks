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
- 예외: DeadZone 식·FovScaling 배율·SmoothDelta 감속 비율은 UE와 같은 결과를 내도록 UE처럼 float로 계산한다.

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
- Rocks `Key` 이름과 UE 이름이 다른 키는 UE 이름을 쓴다. 예: `Key::Space` → `SpaceBar`, `Key::Digit0` → `Zero`, `Key::Grave` → `Tilde`, `Key::Minus` → `Hyphen`, `Key::LeftSuper` → `LeftCommand`, `Key::Numpad0` → `NumPadZero`, `Key::NumpadAdd` → `Add`, `Key::Back` → `Android_Back`. 전체 대응은 키 이름표(`InputKey.cpp`)와 그 기대표 시험이다. 비교는 대소문자를 무시한다.
- UE는 숫자패드 Enter를 따로 두지 않는다(`EKeys::Enter` 하나). 그래서 `Enter` 키는 Enter와 숫자패드 Enter를 함께 읽는다(둘 중 하나라도 눌려 있으면 눌림, 놓임은 둘 다 놓였을 때). `Key::NumpadEnter`는 Rocks에만 있는 키이고 이름은 `NumPadEnter`다(UE 이름 규칙을 따른 Rocks 이름 — 7.3절). 숫자패드 Enter 키는 Enter를 읽지 않는다.
- 터치 키의 원시값은 화면 좌표다. Boolean 액션은 원시값 대신 `down || pressed`로 1·0을 만든다(x = 0 열의 터치도 작동, 한 프레임 탭 포함 — 4절 평가기).
- Rocks 키 집합에 없는 UE 키는 7.4절에 적는다. 그 키 이름은 `ParseInputKey`가 `NotFound`를 내고, 설정 읽기는 건너뛰고 진단한다. 해당 키는 디지털 트리거 버튼(`Gamepad_LeftTrigger`), 스틱 방향 버튼, `AnyKey`다.

### 2.3 InputAction

리플렉션 클래스(Object)다. 필드는 `id`와 설정 struct `settings`(`InputActionSettings` — 평가기가 그대로 복사한다, 2.7절)다. 아래 표에서 `id` 밖의 필드는 모두 `settings` 안이다. 필드와 기본값은 UE `EI/Public/InputAction.h`와 같다.

| 필드 | 기본 | UE |
|---|---|---|
| `id`(Guid — 저장·리바인딩 키, 바인딩이 액션을 가리키는 값) | 생성 때 정함(0이면 잘못된 액션) | 없음 — UE는 에셋 경로. 상세 계획 21.1절 ActionId |
| `valueType` | Boolean | :116 |
| `triggerWhenPaused` | 거짓 | :92 |
| `consumeInput` | 참 | :96 — 하위 우선순위 컨텍스트의 같은 키 바인딩을 막는다 |
| `reserveAllMappings` | 거짓 | :108 — 표시만 한다(UE도 강제하지 않는다) |
| `accumulation` | TakeHighestAbsoluteValue | :127 |
| `triggers`·`modifiers` | 빈 배열 | :135·:145 |
| `mappingSettings`(`PlayerMappingSettings`, 선택) | 없음 | :153 |

### 2.4 InputContext·InputBinding

`InputContext`(리플렉션 클래스)의 필드는 다음과 같다.
- `id`(Guid, 0이면 잘못된 컨텍스트)
- `actions: ObjectRefArray<InputAction>`(Required — null 원소 없음): 바인딩이 가리키는 액션의 GC 간선이자 패키지 Hard 의존이다(함께 로드·쿠킹 — [에셋 시스템](../AssetSystem/06-AssetTypes.ko.md) 6장 참조 표). chord 트리거의 대상은 넣어도 되고 안 넣어도 된다(2.7절).
- `data: InputContextData`(리플렉션 struct — 평가기가 그대로 복사한다):
  - `bindings: Array<InputBinding>`
  - `profileOverrides: Array<InputProfileBindings{ profile(Name), bindings }>`: 그 프로필이 활성이면 기본 bindings를 통째로 대신한다(UE `MappingProfileOverrides` — `InputMappingContext.cpp:113-121`).
  - `registrationTracking { Untracked, CountRegistrations }`(UE :47-64)

`InputBinding`(리플렉션 struct):

| 필드 | UE `EnhancedActionKeyMapping.h` |
|---|---|
| `action`(액션 `id` Guid — 2.7절) | :94(UE는 객체 포인터) |
| `key`(String — UE `EKeys` 이름, 2.2절. 평가기가 다시 만들 때 `ParseInputKey`로 읽는다) | :98(UE `FKey`도 이름으로 저장한다) |
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
| 0 id 액션·컨텍스트, 범위 밖 enum, 비유한 매개변수, DeadZone lower ≥ upper(UE `IsDataValid`와 같다) | `InvalidArgument` |
| `ParseInputKey`가 모르는 이름 | `NotFound` |
| 할당 실패 | `OutOfMemory`(상태는 그대로 — 모디파이어 복사도 커브 셋을 임시본에 먼저 복사한다) |
| 사용자 설정 JSON 파손 | `CorruptData`. 모르는 이름은 진단만 하고 건너뛴다 |

스레드: 모든 타입은 게임 스레드 전용이다. 데이터(액션·컨텍스트)는 평가 중 바꾸지 않는다(바꾸면 다음 다시 만들기에서 반영 — 4.2절).

### 2.7 객체와 평가기 데이터 사본(IAD10)

Rocks 객체 규칙 때문에 UE처럼 평가기가 액션·컨텍스트 객체 포인터를 프레임 사이에 쥐지 않는다.
- `ObjectRef`는 Array 원소 안에 둘 수 없다(참조 struct 목록은 Registry가 관리하는 `StructRefArray` — `Object/ObjectRefs.h`). 바인딩을 `StructRefArray<InputBinding>`로 두는 것 자체는 된다(원소가 일반 `Array<값 struct>`를 담을 수 있다). 막히는 것은 다음이다.
  - chord 대상을 `ObjectRef`로 두면 `InputTrigger`가 참조 struct가 되어 `triggers`도 `StructRefArray`여야 하고, 그것을 `StructRefArray` 원소 안에 둘 수 없다(메타 컴파일러 RMC2304). `profileOverrides[i].bindings`도 같다.
  - `ObjectRef`·`StructRefArray`는 값 복사가 안 된다(`ValueCopy` Unsupported). 평가기가 컨텍스트 데이터를 사본으로 가질 수 없다.
  - World 객체는 에셋 객체를 `ObjectRef`로 걸 수 없다. 에셋은 `SoftObjectRef`(영속)·`AssetLease`(실행 중)로 쥐고 `AssetManager::With`로 읽는다([에셋 시스템](../AssetSystem/06-AssetTypes.ko.md) 6.3절).
- 객체 포인터는 `Borrow` 동안만 쓴다. 활성 Borrow는 전체 GC를 막으므로 평가기가 쥐고 있을 수 없다.

그래서 다음과 같이 나눈다.
- **바인딩은 액션을 `id`(Guid)로 가리킨다.** chord 트리거의 대상도 액션 id다(`InputTrigger::chordActionId`). 컨텍스트 객체는 가리키는 액션을 `actions`(ObjectRefArray)로 붙잡는다(GC 간선 — 액션이 컨텍스트보다 먼저 회수되지 않는다). NodeGraph 문서가 노드를 Guid로 잇는 것과 비슷하다. 다만 NodeGraph는 한 문서 안의 닫힌 참조이고, 이쪽은 객체 사이 참조라 무결성은 검증이 맡는다.
- **평가기는 데이터 사본만 갖는다.** `InputContextBundle`(리플렉션 타입이 아닌 C++ 값 — 이동만)은 컨텍스트 id, `InputContextData` 사본, `actions` 전체의 `InputActionData{ id, settings }` 사본 목록이다. 평가기는 객체도 Registry도 모른다(시험이 객체 없이 결정적이다).
- **모으기**: `CollectInputContext(registry, access, ObjectHandle context, bundle)`가 컨텍스트와 `actions`를 Borrow해 사본을 만든다. `registry`·`access`는 호출자가 그 객체를 읽을 수 있는 문맥이다(에셋이면 `AssetManager::With` 범위 — 5.1절). 실패: 객체 접근 실패는 그 오류, null·파괴된 `actions` 원소는 `InvalidArgument`, 할당 실패는 `OutOfMemory`(bundle은 그대로)다.
- **검증**: `ValidateInputContextBundle`(평가기 `AddContext`·`UpdateContext`도 부른다)이 다음을 거부한다(`InvalidArgument` + 진단에 바인딩·트리거 위치). 0 id, 같은 id 액션 둘, 바인딩 액션이 목록에 없음, 키 이름을 읽을 수 없음(`ParseInputKey` 실패), 범위 밖 enum, 비유한 매개변수, DeadZone lower ≥ upper. chord 대상이 목록에 없는 것은 허용한다(다른 컨텍스트의 액션일 수 있다 — 평가 때 표에 없으면 None, 3.2절).
- **데이터를 바꾸면** 다시 모아 `UpdateContext(bundle, forceImmediately)`로 사본만 바꾼다(UE `RequestRebuildControlMappingsUsingContext` — 4.1절). 우선순위·등록 횟수는 그대로다. `AddContext`는 이미 있는 id면 UE처럼 데이터를 건드리지 않는다. 실행 중 편집(키 추가·제거)을 그 컨텍스트를 쓰는 모든 플레이어에 퍼뜨리는 것은 Gameplay 도우미다(5.1절 — UE `MapKey`·`UnmapKey`는 그 컨텍스트를 가진 모든 플레이어를 다시 만든다).
- **같은 id 액션을 여러 컨텍스트가 넣으면** 평가기는 액션 표에 하나만 둔다. 다시 만들 때마다 남은 bundle들에서 정렬 순(4.2절 1 — 높은 우선순위, 같으면 먼저 추가한 것)으로 처음 나온 사본을 쓴다(넣은 순서·제거에 따라 결과가 흔들리지 않는다). 같은 id인데 설정이 다르면 Warning 진단 `InputActionSettingsConflict`를 낸다(다른 객체가 같은 id를 가진 경우 — 에셋 복제는 id를 새로 만든다, IA6).
- `ActionState`·"사라진 액션"(4.2절 7)은 액션 표에 있는지가 아니라 바인딩·주입이 있는지로 판정한다. 액션 수준 chord(`settings.triggers`의 ChordAction)는 대상 액션의 GC 간선이 없다(id만 — 대상이 어느 컨텍스트에도 없으면 None, 3.2절).
- 바인딩 없는 액션 주입은 `InjectValue`가 `InputActionData`를 함께 받는다(4.3절 3 — UE `InjectInputForAction`은 어느 컨텍스트에도 없는 액션도 받는다).
