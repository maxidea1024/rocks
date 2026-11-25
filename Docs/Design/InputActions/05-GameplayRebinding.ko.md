# 입력 액션 계층 — 5. 게임플레이 연결·리바인딩

[목차](README.ko.md)

## 5. 게임플레이 연결·리바인딩

### 5.1 PlayerController·바인딩

`PlayerController`는 `PlayerActionInput`과 `InputActionBindings`(UE `UEnhancedInputComponent`)를 가진다. 둘 다 NATIVE(실행 중 상태)다.

평가 시점:
- `PlayerController::OnTick` 안에서 한다. 입력 스냅숏과 제스처 다음, `OnPlayerTick` 전이다(지금 `Controllers.cpp:322-336`).
- 일시정지 중에도 평가하고 전달한다. 막는 것은 액션의 `triggerWhenPaused`다. `OnPlayerTick`은 pause 중 건너뛰지만 전달은 하므로, pause 메뉴의 "재개" 액션이 동작한다. UE도 pause Tick에서 `TickPlayerInput(…, bGamePaused = true)`로 입력을 처리한다.

바인딩 API(UE `EI/Public/EnhancedInputComponent.h`):
- `BindAction(action, ActionEvent events, Function<void(const ActionEventData&)>, bool consumes = false) → BindingHandle`
  - `ActionEventData`는 액션, 이벤트, 값, 경과·Triggered 시간이다(UE `(const FInputActionInstance&)` 형태).
  - 할당 실패면 `OutOfMemory`다.
- `BindActionValue(action) → ValueBindingHandle`과 `ActionValue(handle) → InputValue`: 평가마다 갱신한다. 포인터를 내주지 않는다(재배치 안전).
- `RemoveBinding(handle)`·`ClearBindings()`

전달 규칙(UE :720-1006):
- 바인딩 목록을 등록 순서로 돌며, 액션의 이번 이벤트와 바인딩 이벤트 마스크가 겹치는 것을 실행 목록에 넣는다.
- Started 바인딩은 실행 목록 **맨 앞에 차례로 끼워 넣는다**(UE `EmplaceAt(0)` — :801-808). 그래서 Started끼리는 등록 역순이고, Started 전체가 다른 이벤트보다 먼저 실행된다.
- `consumes`: 등록 순으로 그 바인딩보다 뒤에 있는 같은 액션 바인딩의 실행을 막는다(UE 리스너 소비 — `EnhancedInput.EnableListenerConsumption` 기본 1).
- chord 억제(4.4절)를 적용한다.
- 실행은 평가 시점 목록의 사본으로 한다. 그래서 콜백이 바인딩을 더하거나 지우면 다음 평가부터 반영된다.
- 입력 컴포넌트 **스택**(Pawn 등이 층을 쌓고 `bBlockInput`이 아래를 막는 것 — UE `PlayerInput.cpp:1500-1527`)은 CR26이다. 이번에는 PlayerController 한 층이다(7.4절).

### 5.2 사용자 설정·리바인딩(UE `EI/Public/UserSettings/EnhancedInputUserSettings.h`)

`InputUserSettings`(로컬 플레이어마다):
- `profiles: Array<KeyProfile>`, `currentProfileId`
- `KeyProfile { id, displayName, rows: Array<KeyMappingRow> }`
- `KeyMappingRow { mappingName, Array<PlayerKeyMapping> }`
- `PlayerKeyMapping { slot, defaultKey, currentKey }`
  - `slot`은 First…Seventh다(UE `EPlayerMappableKeySlot`).
  - `IsCustomized()`는 currentKey ≠ defaultKey다.

UE는 slot을 하드웨어 장치 ID별로 둔다. 그런데 기본 구현이 Invalid를 돌려주므로(`UserSettings.cpp:1798-1801`) 모든 장치가 한 slot 열을 쓴다. Rocks도 장치 칸 없이 **이름마다 slot 한 열**이다(IAD9).

동작:
- **등록**: 컨텍스트를 `notifyUserSettings`로 추가하면 리바인딩 가능한 바인딩마다 행을 만든다. 같은 이름의 바인딩마다 slot을 차례로 배정한다(UE cpp:1676-).
- **`MapPlayerKey(mappingName, slot, key, profile = 현재, createSlotIfNeeded = 거짓)`**(UE cpp:1133-1250):
  - 그 (이름, slot)의 currentKey를 바꾼다.
  - slot이 없고 `createSlotIfNeeded`면 새로 만든다. 이때 defaultKey는 없음이다. 같은 이름 행에 매핑이 하나 이상 있어야 한다(UE :1192).
  - 바꾸면 다시 만들기를 예약한다.
  - 오류:
    - 이름 행이 없다: `NotFound`
    - slot이 없고 만들 수 없다: `NotFound`
    - 키가 무효다: `InvalidArgument`
    - 프로필이 없다: `NotFound`
- **`UnmapPlayerKey(mappingName, slot)`**: 기본 키로 되돌린다(UE cpp:1252-).
- **적용**: 다시 만들기(4.2절 2)에서 리바인딩 가능한 바인딩의 키를 바꾼다(UE `EnhancedInputSubsystemInterface.cpp:1004-1020`, UserSettings :479-500·535-546).
  - 활성 프로필의 같은 이름 행에서, 기본 키와 **같은 종류**(게임패드 키인지·터치인지)인 currentKey들로 바꾼다.
  - 키 하나에 바인딩 하나를 만든다.
- **충돌**: 다른 이름이 이미 쓰는 키로 바꾸는 요청은 막지 않는다(UE와 같다). `FindMappingsUsingKey(key)`로 UI가 알린다.

### 5.3 저장

UE는 `USaveGame` 슬롯 "EnhancedInputUserSettings"에 쓴다(cpp:842-895·948-1040). 형식은 버전 2이고, 프로필 헤더 다음에 바뀐 매핑만 {이름, 키 이름, slot}을 쓴다.

Rocks:
- 같은 내용을 리플렉션 struct `InputUserSettingsRecord`로 두고 JSON으로 읽고 쓴다.
  - `SaveUserSettings → Array<uint8>`
  - `LoadUserSettings(bytes) → Result<void>`
- 키는 이름(2.2절)이다.
- 파일 위치(사용자 Saved 폴더)는 호출자(Gameplay `LocalPlayer`)가 정한다. Rocks에는 아직 SaveGame 체계가 없다.
- 모르는 매핑 이름·키 이름은 건너뛰고 진단으로 알린다(설정을 버리지 않는다). 형식이 깨졌으면 `CorruptData`이고 기존 설정은 그대로다.
