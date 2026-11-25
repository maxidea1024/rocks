# 입력 액션 계층 — 5. 게임플레이 연결·리바인딩

[목차](README.ko.md)

## 5. 게임플레이 연결·리바인딩

### 5.1 PlayerController·바인딩

`PlayerController`는 `PlayerActionInput`과 `InputActionBindings`(UE `UEnhancedInputComponent`)를 가진다. 둘 다 NATIVE(실행 중 상태)다.

컨텍스트 객체 쥐기(2.7절 — UE `AppliedInputContextData`가 컨텍스트를 강하게 붙잡는 역할):
- IA3은 bundle 수준 API다: `PlayerController::ActionInput()`(`PlayerActionInput`)에 `AddContext(bundle, …)`로 넣는다. 객체·에셋에서 모으는 도우미(아래 둘)는 에셋 감싸기와 함께 IA6이다.
- 에셋 컨텍스트는 적용한 동안 `AssetLease`로 쥐고, 코드로 만든 객체는 handle로 쥔다(PlayerController의 기존 NATIVE handle + `World::With` 방식).
- `AddInputContext(asset, priority, options)`는 `AssetManager::With` 범위에서 `CollectInputContext`로 모아 평가기에 넣는다. 데이터를 바꾼 뒤에는 `RefreshInputContext(asset)`이 다시 모아 `UpdateContext`한다.
- 실행 중 편집을 퍼뜨리는 도우미(UE `MapKey`·`UnmapKey`·`UnmapAll`): 한 번 모은 bundle을 그 id의 컨텍스트를 가진 모든 PlayerController에 `UpdateContext`한다. `notifyUserSettings`로 등록된 컨텍스트면 사용자 설정 행도 갱신한다(5.2절).

평가 시점:
- `PlayerController::OnTick` 안에서 한다. 입력 스냅숏과 제스처 다음, `OnPlayerTick` 전이다(UE `TickActor`·`PlayerTick`이 `TickPlayerInput`을 먼저 한다 — `PlayerController.cpp:2326·5504-5510`).
- 순서: 입력 스냅숏 → 제스처 → `Evaluate` → `InputActionBindings::Dispatch` → `FinishDispatch` → `OnPlayerTick`(pause 중 생략) → `OnControllerTick`. 평가 실패(다시 만들기 할당 실패 — 이전 목록으로 평가된다 — 또는 이벤트 기록 실패)는 Warning `InputEvaluationFailed`로 남기고 그 프레임 결과로 전달한다. 경고는 실패가 이어지는 동안 한 번이다.
- 평가 문맥: `deltaSeconds` = `TickContext::unscaledDeltaSeconds`, `timeDilation` = World 시계 `TimeScale()`(UE `GetActorTimeDilation` 대응 — pause는 따로), `paused` = `TickContext::paused`, `fov` = CameraDirector 뷰의 fov(없으면 0 — 카메라 없음), `hasPlayer` 참, `gamepad` = LocalPlayer `ControllerId`(음수면 0 — `SetPlayer`가 받고 `ClearPlayer`가 0으로 되돌린다). fov는 앞 프레임 카메라 뷰다(카메라는 LateUpdate에 갱신 — UE FovScaling도 PlayerCameraManager의 직전 값을 읽는다).
- 일시정지 중에도 평가하고 전달한다. 막는 것은 액션의 `triggerWhenPaused`다. `OnPlayerTick`은 pause 중 건너뛰지만 전달은 하므로, pause 메뉴의 "재개" 액션이 동작한다. UE도 pause Tick에서 `TickPlayerInput(…, bGamePaused = true)`로 입력을 처리한다.

바인딩 API(UE `EI/Public/EnhancedInputComponent.h`):
- `BindAction(actionId, ActionEvent events, Function<void(const ActionEventData&)>, bool consumes = false) → ActionBindingHandle`
  - `ActionEventData`는 액션 id, 이벤트, 값, 경과·Triggered 시간이다(UE `(const FInputActionInstance&)` 형태).
  - 빈 함수·None 마스크는 `InvalidArgument`다(UE는 `ETriggerEvent::None` 바인딩을 이벤트 없는 틱마다 부른다 — 버린 대안: 쓰임이 불분명하고 매 평가 호출이 된다). 할당 실패면 `OutOfMemory`다.
  - 콜백 수명: `Function`은 캡처를 그대로 쥔다(UE delegate처럼 객체 파괴를 알지 못한다). 객체는 handle을 캡처해 `World::With`로 접근하거나, 없어지기 전에(OnUnPossess·EndPlay) `RemoveBinding`한다. 콜백을 만든 allocator는 바인딩보다 오래 살아야 한다.
- `BindActionValue(actionId) → ActionValueHandle`과 `ActionValue(handle) → InputValue`: 평가마다 갱신한다. 포인터를 내주지 않는다(재배치 안전).
- `RemoveBinding(handle)`·`RemoveValueBinding(handle)`·`ClearBindings()`. 전달 중(콜백 안)에 지운 바인딩은 그 평가의 실행 목록에 있으면 실행되고, 전달이 끝난 뒤 지워진다(UE는 실행 목록을 복제본으로 만든다).
- 바인딩 마스크와 이벤트: 한 바인딩은 마스크(여러 비트 가능)와 겹치는 이벤트가 있으면 한 번 실행한다. 콜백이 받는 `events`는 (이번 이벤트 & 마스크)다. 그래서 Started와 Triggered가 한 평가에 나면 Started 바인딩은 Started를, Triggered 바인딩은 Triggered를 받는다(UE가 그 평가 동안 `TriggerEvent`를 바인딩 이벤트로 바꿔 부르는 것과 같다 — :868-883).
- 값 바인딩은 전달마다 그 액션의 현재 값이다. 표에 없는 액션은 0이고 타입은 마지막 값의 것이다(UE는 액션 타입의 0 — :911-925).
- `InputActionBindings`는 InputActions 모듈의 값 타입이다(Gameplay에 묶이지 않는다 — 시험이 PlayerController 없이 결정적이다). 바인딩은 하나씩 따로 할당해 콜백 실행 중 추가가 실행 중인 콜백을 옮기지 않는다.

전달 규칙(UE :720-1006):
- 바인딩 목록을 등록 순서로 돌며, 액션의 이번 이벤트와 바인딩 이벤트 마스크가 겹치는 것을 실행 목록에 넣는다.
- Started 바인딩은 실행 목록 **맨 앞에 차례로 끼워 넣는다**(UE `EmplaceAt(0)` — :801-808). 그래서 Started끼리는 등록 역순이고, Started 전체가 다른 이벤트보다 먼저 실행된다.
- `consumes`: 실행 목록 순으로 그 바인딩 뒤의 같은 액션 바인딩 실행을 막는다. chord 억제로 건너뛴 바인딩은 소비하지 않는다(UE 리스너 소비 — `EnhancedInput.EnableListenerConsumption` 기본 1, :838-844·863·900-905). 실행 직전에 기록을 다시 찾고, 없으면 실행만 건너뛴다(소비는 건다 — UE :865).
- chord 억제(4.4절)를 적용한다. "Triggered로 전달됨"은 Triggered 비트가 겹친 바인딩이 실행 목록에 든 액션이다(실행 전에 모은다 — UE :808-812). chord 짝은 바인딩마다 다시 읽는다(콜백의 즉시 다시 만들기가 목록을 바꾼다).
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
